/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 * (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Engine/Nodes/Scene/SceneRaster.h"

#include "Global/Macros.h"

#include <algorithm>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/Curve.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeColorSampler.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Nodes/Metadata/MetadataView.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/TexturePaint.h"
#include "Engine/Nodes/Scene/TexturePaintView.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoLayer.h"
#include "Engine/RotoPoint.h"
#include "Engine/RotoStrokeItem.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
template <typename KNOB>
std::shared_ptr<KNOB>
getKnob(const NodePtr& node,
        const std::string& name)
{
    return std::dynamic_pointer_cast<KNOB>(node->getKnobByName(name));
}

TexturePaint*
getTexturePaint(const NodePtr& node)
{
    return node ? dynamic_cast<TexturePaint*>(node->getEffectInstance().get()) : nullptr;
}

typedef std::vector<std::pair<double, double>> CurveKeys;

CurveKeys
curveKeys(const CurvePtr& curve)
{
    CurveKeys keys;
    const KeyFrameSet keyframes = curve->getKeyFrames_mt_safe();

    for (KeyFrameSet::const_iterator it = keyframes.begin(); it != keyframes.end(); ++it) {
        keys.push_back(std::make_pair(it->getTime(), it->getValue()));
    }

    return keys;
}

// Per stroke script name, the keys of its x then y curves.
std::map<std::string, std::vector<CurveKeys>>
strokeKeys(const RotoLayerPtr& layer)
{
    std::map<std::string, std::vector<CurveKeys>> strokes;

    for (const RotoItemPtr& item : layer->getItems()) {
        RotoStrokeItemPtr stroke = std::dynamic_pointer_cast<RotoStrokeItem>(item);
        if (!stroke) {
            continue;
        }
        std::vector<CurveKeys>& curves = strokes[stroke->getScriptName()];
        for (const CurvePtr& curve : stroke->getXControlPoints()) {
            curves.push_back(curveKeys(curve));
        }
        for (const CurvePtr& curve : stroke->getYControlPoints()) {
            curves.push_back(curveKeys(curve));
        }
    }

    return strokes;
}

// Near rather than equal: stroke samples may be stored as single-precision floats.
void
expectSameStrokes(const std::map<std::string, std::vector<CurveKeys>>& expected,
                  const std::map<std::string, std::vector<CurveKeys>>& actual)
{
    ASSERT_EQ(expected.size(), actual.size());
    for (const auto& stroke : expected) {
        const auto found = actual.find(stroke.first);
        ASSERT_TRUE(found != actual.end()) << stroke.first;
        ASSERT_EQ(stroke.second.size(), found->second.size()) << stroke.first;
        for (std::size_t c = 0; c < stroke.second.size(); ++c) {
            const CurveKeys& expectedKeys = stroke.second[c];
            const CurveKeys& actualKeys = found->second[c];
            ASSERT_EQ(expectedKeys.size(), actualKeys.size()) << stroke.first << " curve " << c;
            for (std::size_t k = 0; k < expectedKeys.size(); ++k) {
                EXPECT_NEAR(expectedKeys[k].first, actualKeys[k].first, 1e-6) << stroke.first << " curve " << c;
                EXPECT_NEAR(expectedKeys[k].second, actualKeys[k].second, 1e-6) << stroke.first << " curve " << c;
            }
        }
    }
}

SceneRaster::ExplicitCamera
paintedCamera()
{
    SceneRaster::ExplicitCamera camera;
    const double cameraToWorld[16] = {
        0.8,
        0.,
        -0.6,
        0.,
        0.,
        1.,
        0.,
        0.,
        0.6,
        0.,
        0.8,
        0.,
        3.5,
        1.25,
        4.75,
        1.,
    };

    for (int i = 0; i < 16; ++i) {
        camera.cameraToWorld[i] = cameraToWorld[i];
    }
    camera.viewHeight = 0.75;
    camera.aspect = 1.5;
    camera.orthographic = true;
    camera.nearClip = 0.25;
    camera.farClip = 500.;

    return camera;
}

// Two quads filling the height of viewCamera(): red on x in [-2, 0], blue on x in [0, 2].
const char* const kTwoQuads = R"usda(#usda 1.0
(
    upAxis = "Y"
)

def Xform "World"
{
    def Mesh "Red" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-2, -2, 0), (0, -2, 0), (0, 2, 0), (-2, 2, 0)]
        texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 1)] (
            interpolation = "vertex"
        )
        rel material:binding = </World/Looks/Red>
    }

    def Mesh "Blue" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(0, -2, 0), (2, -2, 0), (2, 2, 0), (0, 2, 0)]
        texCoord2f[] primvars:st = [(1, 0), (2, 0), (2, 1), (1, 1)] (
            interpolation = "vertex"
        )
        rel material:binding = </World/Looks/Blue>
    }

    def Scope "Looks"
    {
        def Material "Red"
        {
            token outputs:surface.connect = </World/Looks/Red/Surface.outputs:surface>

            def Shader "Surface"
            {
                uniform token info:id = "UsdPreviewSurface"
                color3f inputs:diffuseColor = (1, 0, 0)
                token outputs:surface
            }
        }

        def Material "Blue"
        {
            token outputs:surface.connect = </World/Looks/Blue/Surface.outputs:surface>

            def Shader "Surface"
            {
                uniform token info:id = "UsdPreviewSurface"
                color3f inputs:diffuseColor = (0, 0, 1)
                token outputs:surface
            }
        }
    }
}
)usda";

// Orthographic, at z = 5 looking down -Z, 4 units high and 6 wide: the quads fill the middle
// two thirds of the view, red at u = 1/3 and blue at u = 2/3 on the centre line.
SceneRaster::ExplicitCamera
viewCamera()
{
    SceneRaster::ExplicitCamera camera;

    camera.cameraToWorld[14] = 5.;
    camera.viewHeight = 4.;
    camera.aspect = 1.5;
    camera.orthographic = true;
    camera.nearClip = 0.1;
    camera.farClip = 100.;

    return camera;
}

const double kRedU = 1. / 3.;
const double kBlueU = 2. / 3.;

struct BufferPixel {
    float rgba[4];
    bool touched;
};

// The buffer pixel under normalized view coordinates (u, v).
BufferPixel
pixelAt(const TexturePaint::SessionBuffer& buffer,
        double u,
        double v)
{
    const int x = std::min(buffer.width - 1, std::max(0, (int)(u * buffer.width)));
    const int y = std::min(buffer.height - 1, std::max(0, (int)(v * buffer.height)));
    const std::size_t i = (std::size_t)y * (std::size_t)buffer.width + (std::size_t)x;
    BufferPixel pixel;

    for (int c = 0; c < 4; ++c) {
        pixel.rgba[c] = buffer.rgba[i * 4 + (std::size_t)c];
    }
    pixel.touched = buffer.touched[i] != 0;

    return pixel;
}

void
expectColor(const BufferPixel& pixel,
            float r,
            float g,
            float b,
            float a,
            const char* what)
{
    EXPECT_NEAR(r, pixel.rgba[0], 1e-3) << what;
    EXPECT_NEAR(g, pixel.rgba[1], 1e-3) << what;
    EXPECT_NEAR(b, pixel.rgba[2], 1e-3) << what;
    EXPECT_NEAR(a, pixel.rgba[3], 1e-3) << what;
}
} // anonymous namespace

class TexturePaintTest
    : public BaseTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        SceneQueryPool::instance().releaseIdle();
        BaseTest::TearDown();
    }

    /**
     * A TexturePaint over a USDRead of kTwoQuads, written into dir, with one session through
     * viewCamera(). Null after a gtest failure.
     **/
    NodePtr createPaintedQuads(const QTemporaryDir& dir,
                               RotoLayerPtr* session)
    {
        return createPaintedScene(dir, kTwoQuads, session);
    }

    // As createPaintedQuads(), over the scene usda.
    NodePtr createPaintedScene(const QTemporaryDir& dir,
                               const char* usda,
                               RotoLayerPtr* session)
    {
        const QString path = dir.path() + QString::fromUtf8("/quads.usda");
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || (file.write(usda) < 0)) {
            ADD_FAILURE() << "could not write " << path.toStdString();

            return NodePtr();
        }
        file.close();

        NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
        NodePtr paint = createNode(QString::fromUtf8(PLUGINID_NATRON_TEXTUREPAINT));
        KnobFilePtr fileKnob = reader ? getKnob<KnobFile>(reader, kUSDReadParamFile) : KnobFilePtr();
        if (!fileKnob || !getTexturePaint(paint)) {
            ADD_FAILURE() << "USDRead or TexturePaint is not registered";

            return NodePtr();
        }
        fileKnob->setValue(path.toStdString());
        connectNodes(reader, paint, 0, true);
        *session = getTexturePaint(paint)->addSession(viewCamera());
        if (!*session) {
            ADD_FAILURE() << "no session";

            return NodePtr();
        }

        return paint;
    }

    /**
     * A stroke of type in session, across normalized view point (u, v), large enough to cover
     * the pixel there, shown at every frame.
     **/
    RotoStrokeItemPtr addBrushStroke(const NodePtr& paint,
                                     const RotoLayerPtr& session,
                                     RotoStrokeType type,
                                     const std::string& name,
                                     double u,
                                     double v)
    {
        RotoContextPtr context = paint->getRotoContext();
        RotoStrokeItemPtr stroke(new RotoStrokeItem(type, context, name, RotoLayerPtr()));

        stroke->createNodes();
        context->addItem(session, 0, stroke, RotoItem::eSelectionReasonOther);
        KnobChoicePtr lifeTime = std::dynamic_pointer_cast<KnobChoice>(stroke->getKnobByName("lifeTime"));
        if (lifeTime) {
            lifeTime->setValue(0);
        }
        stroke->getBrushSizeKnob()->setValue(400.);
        stroke->getBrushHardnessKnob()->setValue(1.);

        double x = 0.;
        double y = 0.;
        TexturePaintView::viewToPaintSpace(viewCamera().aspect, u, v, &x, &y);
        stroke->appendPoint(true, RotoPoint(x - 20., y, 1., 1.));
        stroke->appendPoint(false, RotoPoint(x, y, 1., 2.));
        stroke->appendPoint(false, RotoPoint(x + 20., y, 1., 3.));
        stroke->setStrokeFinished();
        context->refreshRotoPaintTree();

        return stroke;
    }

    static void setColor(const RotoStrokeItemPtr& stroke,
                         double r,
                         double g,
                         double b)
    {
        KnobColorPtr color = stroke->getColorKnob();
        color->setValue(r, ViewSpec::all(), 0);
        color->setValue(g, ViewSpec::all(), 1);
        color->setValue(b, ViewSpec::all(), 2);
    }

    static bool render(const NodePtr& paint,
                       const RotoLayerPtr& session,
                       const ImageLayerDesc& plane,
                       TexturePaint::SessionBuffer* buffer)
    {
        std::string error;
        const bool ok = getTexturePaint(paint)->renderSessionBuffer(session, plane, 1., ViewIdx(0), buffer, &error);

        EXPECT_TRUE(ok) << error;
        EXPECT_EQ((std::size_t)buffer->width * (std::size_t)buffer->height * 4, buffer->rgba.size());
        EXPECT_EQ((std::size_t)buffer->width * (std::size_t)buffer->height, buffer->touched.size());

        return ok && !buffer->rgba.empty();
    }

    RotoStrokeItemPtr addStroke(const RotoContextPtr& context,
                                const RotoLayerPtr& layer,
                                const std::string& name,
                                double offset)
    {
        RotoStrokeItemPtr stroke(new RotoStrokeItem(eRotoStrokeTypeSolid, context, name, RotoLayerPtr()));

        stroke->createNodes();
        context->addItem(layer, 0, stroke, RotoItem::eSelectionReasonOther);
        stroke->appendPoint(true, RotoPoint(0.1 + offset, 0.1, 1., 1.));
        stroke->appendPoint(false, RotoPoint(0.4 + offset, 0.6, 0.5, 2.));
        stroke->appendPoint(false, RotoPoint(0.7 + offset, 0.15, 0.75, 3.));
        stroke->setStrokeFinished();

        return stroke;
    }
};

TEST_F(TexturePaintTest, SessionsStrokesAndKnobsSurviveSaveAndLoad)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_TEXTUREPAINT));
    ASSERT_TRUE(bool(node));
    TexturePaint* effect = getTexturePaint(node);
    ASSERT_TRUE(effect);
    RotoContextPtr context = node->getRotoContext();
    ASSERT_TRUE(bool(context));
    const std::string name = node->getScriptName();

    getKnob<KnobString>(node, kTexturePaintParamPrimPattern)->setValue(std::string("/World/Geo//*"));
    getKnob<KnobString>(node, kTexturePaintParamUvPrimvar)->setValue(std::string("uv"));
    getKnob<KnobChoice>(node, kTexturePaintParamTileResolution)->setValueFromID("8K", 0);
    getKnob<KnobChoice>(node, kTexturePaintParamBufferResolution)->setValueFromID("4K", 0);
    getKnob<KnobChoice>(node, kTexturePaintParamTextureMode)->setValueFromID("animated", 0);
    getKnob<KnobOutputFile>(node, kTexturePaintParamBakeInputsPath)->setValue(std::string("[Project]/in.<UDIM>.exr"));
    getKnob<KnobOutputFile>(node, kTexturePaintParamBakeOutputPath)->setValue(std::string("[Project]/out.<UDIM>.exr"));
    getKnob<KnobInt>(node, kTexturePaintParamOutputTile)->setValue(1012);

    const SceneRaster::ExplicitCamera camera = paintedCamera();
    RotoLayerPtr painted = effect->addSession(camera);
    ASSERT_TRUE(bool(painted));
    TexturePaint::setSessionFrozen(painted, true);
    addStroke(context, painted, "firstStroke", 0.);
    addStroke(context, painted, "secondStroke", 0.05);
    const std::string paintedName = painted->getScriptName();
    const std::map<std::string, std::vector<CurveKeys>> expectedStrokes = strokeKeys(painted);
    ASSERT_EQ(2u, expectedStrokes.size());

    RotoLayerPtr unfrozen = effect->addSession(SceneRaster::ExplicitCamera());
    ASSERT_TRUE(bool(unfrozen));
    const std::string unfrozenName = unfrozen->getScriptName();
    ASSERT_EQ(2u, effect->getSessions().size());

    ProjectPtr project = getApp()->getProject();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("texture-paint.ntpz");
    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));

    painted.reset();
    unfrozen.reset();
    context.reset();
    effect = nullptr;
    node.reset();
    project->reset(false, true);
    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    NodePtr loaded = project->getNodeByName(name);
    ASSERT_TRUE(bool(loaded));
    TexturePaint* loadedEffect = getTexturePaint(loaded);
    ASSERT_TRUE(loadedEffect);

    EXPECT_EQ(std::string("/World/Geo//*"), getKnob<KnobString>(loaded, kTexturePaintParamPrimPattern)->getValue());
    EXPECT_EQ(std::string("uv"), getKnob<KnobString>(loaded, kTexturePaintParamUvPrimvar)->getValue());
    EXPECT_EQ(8192, loadedEffect->getTileResolution());
    EXPECT_EQ(4096, loadedEffect->getBufferResolution());
    EXPECT_EQ(TexturePaint::eTextureModeAnimated, loadedEffect->getTextureMode());
    EXPECT_EQ(std::string("[Project]/in.<UDIM>.exr"), getKnob<KnobOutputFile>(loaded, kTexturePaintParamBakeInputsPath)->getValue());
    EXPECT_EQ(std::string("[Project]/out.<UDIM>.exr"), getKnob<KnobOutputFile>(loaded, kTexturePaintParamBakeOutputPath)->getValue());
    EXPECT_EQ(1012, getKnob<KnobInt>(loaded, kTexturePaintParamOutputTile)->getValue());

    const std::list<RotoLayerPtr> sessions = loadedEffect->getSessions();
    ASSERT_EQ(2u, sessions.size());
    RotoLayerPtr loadedPainted;
    RotoLayerPtr loadedUnfrozen;
    for (const RotoLayerPtr& session : sessions) {
        if (session->getScriptName() == paintedName) {
            loadedPainted = session;
        } else if (session->getScriptName() == unfrozenName) {
            loadedUnfrozen = session;
        }
    }
    ASSERT_TRUE(bool(loadedPainted));
    ASSERT_TRUE(bool(loadedUnfrozen));

    SceneRaster::ExplicitCamera loadedCamera;
    ASSERT_TRUE(TexturePaint::getSessionCamera(loadedPainted, &loadedCamera));
    for (int i = 0; i < 16; ++i) {
        EXPECT_DOUBLE_EQ(camera.cameraToWorld[i], loadedCamera.cameraToWorld[i]) << "element " << i;
    }
    EXPECT_DOUBLE_EQ(camera.viewHeight, loadedCamera.viewHeight);
    EXPECT_DOUBLE_EQ(camera.aspect, loadedCamera.aspect);
    EXPECT_EQ(camera.orthographic, loadedCamera.orthographic);
    EXPECT_DOUBLE_EQ(camera.nearClip, loadedCamera.nearClip);
    EXPECT_DOUBLE_EQ(camera.farClip, loadedCamera.farClip);
    EXPECT_TRUE(TexturePaint::isSessionFrozen(loadedPainted));
    expectSameStrokes(expectedStrokes, strokeKeys(loadedPainted));

    const SceneRaster::ExplicitCamera defaults;
    SceneRaster::ExplicitCamera loadedDefaults;
    ASSERT_TRUE(TexturePaint::getSessionCamera(loadedUnfrozen, &loadedDefaults));
    for (int i = 0; i < 16; ++i) {
        EXPECT_DOUBLE_EQ(defaults.cameraToWorld[i], loadedDefaults.cameraToWorld[i]) << "element " << i;
    }
    EXPECT_EQ(defaults.orthographic, loadedDefaults.orthographic);
    EXPECT_FALSE(TexturePaint::isSessionFrozen(loadedUnfrozen));
    EXPECT_TRUE(strokeKeys(loadedUnfrozen).empty());

    SceneRaster::ExplicitCamera unused;
    RotoLayerPtr baseLayer = loadedPainted->getParentLayer();
    ASSERT_TRUE(bool(baseLayer));
    EXPECT_FALSE(TexturePaint::getSessionCamera(baseLayer, &unused));
} // TEST_F(TexturePaintTest, SessionsStrokesAndKnobsSurviveSaveAndLoad)

TEST_F(TexturePaintTest, ViewShowsTheMeshesThroughTheSessionCamera)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RotoLayerPtr session;
    NodePtr paint = createPaintedQuads(dir, &session);
    ASSERT_TRUE(bool(paint));

    TexturePaint::SessionBuffer buffer;
    ASSERT_TRUE(render(paint, session, ImageLayerDesc::getRGBAComponents(), &buffer));
    expectColor(pixelAt(buffer, kRedU, 0.5), 1.f, 0.f, 0.f, 1.f, "red quad");
    expectColor(pixelAt(buffer, kBlueU, 0.5), 0.f, 0.f, 1.f, 1.f, "blue quad");
    expectColor(pixelAt(buffer, 0.05, 0.5), 0.f, 0.f, 0.f, 0.f, "no mesh");
    for (std::uint8_t touched : buffer.touched) {
        ASSERT_EQ(0, touched);
    }
}

TEST_F(TexturePaintTest, SolidStrokeFillsTheBufferAndEraserRemovesIt)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RotoLayerPtr session;
    NodePtr paint = createPaintedQuads(dir, &session);
    ASSERT_TRUE(bool(paint));

    RotoStrokeItemPtr solid = addBrushStroke(paint, session, eRotoStrokeTypeSolid, "solid", kRedU, 0.5);
    setColor(solid, 0., 1., 0.);
    TexturePaint::SessionBuffer painted;
    ASSERT_TRUE(render(paint, session, ImageLayerDesc::getRGBAComponents(), &painted));
    const BufferPixel stroked = pixelAt(painted, kRedU, 0.5);
    expectColor(stroked, 0.f, 1.f, 0.f, 1.f, "under the stroke");
    EXPECT_TRUE(stroked.touched);
    const BufferPixel away = pixelAt(painted, kBlueU, 0.5);
    expectColor(away, 0.f, 0.f, 1.f, 1.f, "away from the stroke");
    EXPECT_FALSE(away.touched);

    addBrushStroke(paint, session, eRotoStrokeTypeEraser, "eraser", kRedU, 0.5);
    TexturePaint::SessionBuffer erased;
    ASSERT_TRUE(render(paint, session, ImageLayerDesc::getRGBAComponents(), &erased));
    expectColor(pixelAt(erased, kRedU, 0.5), 1.f, 0.f, 0.f, 1.f, "erased back to the view");
    EXPECT_FALSE(pixelAt(erased, kBlueU, 0.5).touched);
}

TEST_F(TexturePaintTest, CloneCopiesTheViewByItsOffset)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RotoLayerPtr session;
    NodePtr paint = createPaintedQuads(dir, &session);
    ASSERT_TRUE(bool(paint));

    RotoStrokeItemPtr clone = addBrushStroke(paint, session, eRotoStrokeTypeClone, "clone", kRedU, 0.5);
    double redX = 0., blueX = 0., y = 0.;
    TexturePaintView::viewToPaintSpace(viewCamera().aspect, kRedU, 0.5, &redX, &y);
    TexturePaintView::viewToPaintSpace(viewCamera().aspect, kBlueU, 0.5, &blueX, &y);
    // A clone stroke at p shows its source at p - translate.
    KnobDoublePtr translate = clone->getBrushCloneTranslateKnob();
    translate->setValue(redX - blueX, ViewSpec::all(), 0);
    translate->setValue(0., ViewSpec::all(), 1);

    TexturePaint::SessionBuffer buffer;
    ASSERT_TRUE(render(paint, session, ImageLayerDesc::getRGBAComponents(), &buffer));
    const BufferPixel cloned = pixelAt(buffer, kRedU, 0.5);
    expectColor(cloned, 0.f, 0.f, 1.f, 1.f, "blue cloned onto red");
    EXPECT_TRUE(cloned.touched);
    expectColor(pixelAt(buffer, kBlueU, 0.5), 0.f, 0.f, 1.f, 1.f, "the source itself");
}

TEST_F(TexturePaintTest, StrokeOnColorLeavesAnotherPlaneUntouched)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RotoLayerPtr session;
    NodePtr paint = createPaintedQuads(dir, &session);
    ASSERT_TRUE(bool(paint));

    RotoStrokeItemPtr solid = addBrushStroke(paint, session, eRotoStrokeTypeSolid, "solid", kRedU, 0.5);
    setColor(solid, 0., 1., 0.);

    TexturePaint::SessionBuffer color;
    ASSERT_TRUE(render(paint, session, ImageLayerDesc::getRGBAComponents(), &color));
    EXPECT_TRUE(pixelAt(color, kRedU, 0.5).touched);

    const std::vector<std::string> channels = { "R", "G", "B" };
    const ImageLayerDesc diffuse("diffuse", "diffuse", "RGB", channels);
    TexturePaint::SessionBuffer other;
    ASSERT_TRUE(render(paint, session, diffuse, &other));
    EXPECT_EQ(color.width, other.width);
    EXPECT_EQ(color.height, other.height);
    for (std::uint8_t touched : other.touched) {
        ASSERT_EQ(0, touched);
    }
    expectColor(pixelAt(other, kRedU, 0.5), 0.f, 0.f, 0.f, 0.f, "nothing painted on diffuse");
}

TEST_F(TexturePaintTest, BufferSizeFollowsBufferResolutionNotTheViewer)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RotoLayerPtr session;
    NodePtr paint = createPaintedQuads(dir, &session);
    ASSERT_TRUE(bool(paint));
    TexturePaint* effect = getTexturePaint(paint);
    addBrushStroke(paint, session, eRotoStrokeTypeSolid, "solid", kRedU, 0.5);

    // 2K by default: 2048 on the longer side, 2048 / 1.5 rounded up on the other.
    TexturePaint::SessionBuffer defaultSize;
    ASSERT_TRUE(render(paint, session, ImageLayerDesc::getRGBAComponents(), &defaultSize));
    EXPECT_EQ(2048, defaultSize.width);
    EXPECT_EQ(1366, defaultSize.height);

    ProjectPtr project = getApp()->getProject();
    project->setOrAddProjectFormat(Format(0, 0, 640, 360, "small", 1.));
    TexturePaint::SessionBuffer small;
    ASSERT_TRUE(render(paint, session, ImageLayerDesc::getRGBAComponents(), &small));
    EXPECT_EQ(defaultSize.width, small.width);
    EXPECT_EQ(defaultSize.height, small.height);
    EXPECT_EQ(pixelAt(defaultSize, kRedU, 0.5).touched, pixelAt(small, kRedU, 0.5).touched);

    project->setOrAddProjectFormat(Format(0, 0, 3840, 2160, "large", 1.));
    TexturePaint::SessionBuffer large;
    ASSERT_TRUE(render(paint, session, ImageLayerDesc::getRGBAComponents(), &large));
    EXPECT_EQ(defaultSize.width, large.width);
    EXPECT_EQ(defaultSize.height, large.height);

    getKnob<KnobChoice>(paint, kTexturePaintParamBufferResolution)->setValueFromID("1K", 0);
    int width = 0;
    int height = 0;
    effect->getSessionBufferSize(session, &width, &height);
    EXPECT_EQ(1024, width);
    EXPECT_EQ(683, height);
    TexturePaint::SessionBuffer oneK;
    ASSERT_TRUE(render(paint, session, ImageLayerDesc::getRGBAComponents(), &oneK));
    EXPECT_EQ(1024, oneK.width);
    EXPECT_EQ(683, oneK.height);
    EXPECT_TRUE(pixelAt(oneK, kRedU, 0.5).touched);
}

namespace {
// A red quad on tile 1001 with the left half of it hidden behind an occluder, which has no UVs and
// so is not painted, and beside it a blue quad on tile 1002 facing away from viewCamera().
const char* const kOccludedAndBackFacing = R"usda(#usda 1.0
(
    upAxis = "Y"
)

def Xform "World"
{
    def Mesh "Red" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-2, -2, 0), (0, -2, 0), (0, 2, 0), (-2, 2, 0)]
        texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 1)] (
            interpolation = "vertex"
        )
        rel material:binding = </World/Looks/Red>
    }

    def Mesh "Occluder"
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-2.5, -2.5, 1), (-1, -2.5, 1), (-1, 2.5, 1), (-2.5, 2.5, 1)]
    }

    def Mesh "Back" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 3, 2, 1]
        point3f[] points = [(0, -2, 0), (2, -2, 0), (2, 2, 0), (0, 2, 0)]
        texCoord2f[] primvars:st = [(1, 0), (2, 0), (2, 1), (1, 1)] (
            interpolation = "vertex"
        )
        rel material:binding = </World/Looks/Blue>
    }

    def Scope "Looks"
    {
        def Material "Red"
        {
            token outputs:surface.connect = </World/Looks/Red/Surface.outputs:surface>

            def Shader "Surface"
            {
                uniform token info:id = "UsdPreviewSurface"
                color3f inputs:diffuseColor = (1, 0, 0)
                token outputs:surface
            }
        }

        def Material "Blue"
        {
            token outputs:surface.connect = </World/Looks/Blue/Surface.outputs:surface>

            def Shader "Surface"
            {
                uniform token info:id = "UsdPreviewSurface"
                color3f inputs:diffuseColor = (0, 0, 1)
                token outputs:surface
            }
        }
    }
}
)usda";

// Two quads side by side in space whose UV layouts both cover the whole of tile 1001.
const char* const kOverlappingQuads = R"usda(#usda 1.0
(
    upAxis = "Y"
)

def Xform "World"
{
    def Mesh "Left"
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-2, -2, 0), (0, -2, 0), (0, 2, 0), (-2, 2, 0)]
        texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 1)] (
            interpolation = "vertex"
        )
    }

    def Mesh "Right"
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(0, -2, 0), (2, -2, 0), (2, 2, 0), (0, 2, 0)]
        texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 1)] (
            interpolation = "vertex"
        )
    }
}
)usda";

const int kSmallTile = 1024;

// Keeps the tiles and buffers small so that baking stays quick.
void
useSmallTiles(const NodePtr& paint)
{
    getKnob<KnobChoice>(paint, kTexturePaintParamTileResolution)->setValueFromID("1K", 0);
    getKnob<KnobChoice>(paint, kTexturePaintParamBufferResolution)->setValueFromID("1K", 0);
}

ImagePtr
renderTile(const NodePtr& paint,
           const ImageLayerDesc& plane = ImageLayerDesc::getRGBAComponents())
{
    RectI rendered;

    return renderNodeImage(paint, 1., ViewIdx(0), 0, RectI(0, 0, kSmallTile, kSmallTile), plane, &rendered);
}

// The texel of tile under texture coordinates (s, t) within the tile.
BufferPixel
texelAt(const ImagePtr& tile,
        double s,
        double t)
{
    BufferPixel pixel = { { 0.f, 0.f, 0.f, 0.f }, false };
    const int x = std::min(kSmallTile - 1, std::max(0, (int)(s * kSmallTile)));
    const int y = std::min(kSmallTile - 1, std::max(0, (int)(t * kSmallTile)));

    if (!tile || !tile->getBounds().contains(x, y)) {
        ADD_FAILURE() << "no texel at " << x << ", " << y;

        return pixel;
    }
    Image::ReadAccess access(tile.get());
    const float* in = reinterpret_cast<const float*>(access.pixelAt(x, y));
    const int nComps = (int)tile->getComponentsCount();
    for (int c = 0; (c < nComps) && (c < 4) && in; ++c) {
        pixel.rgba[c] = in[c];
    }

    return pixel;
}

// A solid stroke along v from normalized view point u0 to u1, wide enough to cover a band of texels.
RotoStrokeItemPtr
addSpanStroke(const NodePtr& paint,
              const RotoLayerPtr& session,
              const std::string& name,
              double u0,
              double u1,
              double v)
{
    RotoContextPtr context = paint->getRotoContext();
    RotoStrokeItemPtr stroke(new RotoStrokeItem(eRotoStrokeTypeSolid, context, name, RotoLayerPtr()));

    stroke->createNodes();
    context->addItem(session, 0, stroke, RotoItem::eSelectionReasonOther);
    KnobChoicePtr lifeTime = std::dynamic_pointer_cast<KnobChoice>(stroke->getKnobByName("lifeTime"));
    if (lifeTime) {
        lifeTime->setValue(0);
    }
    stroke->getBrushSizeKnob()->setValue(400.);
    stroke->getBrushHardnessKnob()->setValue(1.);
    const int steps = 40;
    for (int i = 0; i <= steps; ++i) {
        double x = 0.;
        double y = 0.;
        TexturePaintView::viewToPaintSpace(viewCamera().aspect, u0 + (u1 - u0) * i / steps, v, &x, &y);
        stroke->appendPoint(i == 0, RotoPoint(x, y, 1., 1. + i));
    }
    stroke->setStrokeFinished();
    context->refreshRotoPaintTree();

    return stroke;
}
} // anonymous namespace

TEST_F(TexturePaintTest, TileTakesPaintOnlyWhereTheSessionCameraSeesTheMesh)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RotoLayerPtr session;
    NodePtr paint = createPaintedScene(dir, kOccludedAndBackFacing, &session);
    ASSERT_TRUE(bool(paint));
    useSmallTiles(paint);

    // Across both quads, through the middle of the view.
    RotoStrokeItemPtr stroke = addSpanStroke(paint, session, "across", 0.17, 0.83, 0.5);
    {
        KnobColorPtr color = stroke->getColorKnob();
        color->setValue(0., ViewSpec::all(), 0);
        color->setValue(1., ViewSpec::all(), 1);
        color->setValue(0., ViewSpec::all(), 2);
    }

    ImagePtr red = renderTile(paint);
    ASSERT_TRUE(bool(red));
    // The red quad's s runs with x, so the occluder over x in [-2, -1] hides s below 0.5.
    expectColor(texelAt(red, 0.75, 0.5), 0.f, 1.f, 0.f, 1.f, "visible half under the stroke");
    expectColor(texelAt(red, 0.25, 0.5), 1.f, 0.f, 0.f, 1.f, "occluded half under the stroke");
    expectColor(texelAt(red, 0.75, 0.9), 1.f, 0.f, 0.f, 1.f, "visible half away from the stroke");

    getKnob<KnobInt>(paint, kTexturePaintParamOutputTile)->setValue(1002);
    ImagePtr back = renderTile(paint);
    ASSERT_TRUE(bool(back));
    for (double s : { 0.25, 0.5, 0.75 }) {
        expectColor(texelAt(back, s, 0.5), 0.f, 0.f, 1.f, 1.f, "back face under the stroke");
    }
}

TEST_F(TexturePaintTest, PlanePurposesReachMetadataDownstream)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RotoLayerPtr session;
    NodePtr paint = createPaintedQuads(dir, &session);
    ASSERT_TRUE(bool(paint));
    useSmallTiles(paint);
    addBrushStroke(paint, session, eRotoStrokeTypeSolid, "solid", kRedU, 0.5);

    NodePtr viewer = createNode(QString::fromUtf8(PLUGINID_NATRON_METADATAVIEW));
    ASSERT_TRUE(bool(viewer));
    connectNodes(paint, viewer, 0, true);
    NativeEffectBase* effect = dynamic_cast<NativeEffectBase*>(viewer->getEffectInstance().get());
    ASSERT_TRUE(effect != nullptr);

    const ImageMetadata metadata = effect->getOutputMetadata(1., ViewIdx(0));
    const std::optional<std::string> purpose = metadata.getString(kTexturePaintMetadataPurposePrefix "rgba");
    ASSERT_TRUE(purpose.has_value());
    EXPECT_EQ(std::string(kTexturePaintPurposeDiffuseColor), *purpose);
}

TEST_F(TexturePaintTest, OverlappingUvLayoutsWarnNamingBothMeshes)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RotoLayerPtr session;
    NodePtr paint = createPaintedScene(dir, kOverlappingQuads, &session);
    ASSERT_TRUE(bool(paint));
    useSmallTiles(paint);

    ASSERT_TRUE(bool(renderTile(paint)));
    QString message;
    int type = -1;
    paint->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeWarning, type);
    EXPECT_TRUE(message.contains(QString::fromUtf8("/World/Left"))) << message.toStdString();
    EXPECT_TRUE(message.contains(QString::fromUtf8("/World/Right"))) << message.toStdString();
}

TEST_F(TexturePaintTest, LoadedProjectRendersItsTileWithoutANewSession)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RotoLayerPtr session;
    NodePtr paint = createPaintedQuads(dir, &session);
    ASSERT_TRUE(bool(paint));
    useSmallTiles(paint);
    RotoStrokeItemPtr solid = addBrushStroke(paint, session, eRotoStrokeTypeSolid, "solid", kRedU, 0.5);
    setColor(solid, 0., 1., 0.);
    const std::string name = paint->getScriptName();

    ProjectPtr project = getApp()->getProject();
    const QString dirPath = dir.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("texture-paint-tile.ntpz");
    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));

    solid.reset();
    session.reset();
    paint.reset();
    project->reset(false, true);
    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    NodePtr loaded = project->getNodeByName(name);
    ASSERT_TRUE(bool(loaded));
    // The stroke at view u = 1/3 lies at x = -1 on the red quad, which is s = 0.5 on tile 1001.
    ImagePtr tile = renderTile(loaded);
    ASSERT_TRUE(bool(tile));
    expectColor(texelAt(tile, 0.5, 0.5), 0.f, 1.f, 0.f, 1.f, "under the stroke");
    expectColor(texelAt(tile, 0.1, 0.5), 1.f, 0.f, 0.f, 1.f, "away from the stroke");
}

TEST_F(TexturePaintTest, EachNodeBakesToItsOwnFiles)
{
    NodePtr first = createNode(QString::fromUtf8(PLUGINID_NATRON_TEXTUREPAINT));
    NodePtr second = createNode(QString::fromUtf8(PLUGINID_NATRON_TEXTUREPAINT));
    ASSERT_TRUE(first && second);

    for (const char* knob : { kTexturePaintParamBakeInputsPath, kTexturePaintParamBakeOutputPath }) {
        const std::string firstPath = getKnob<KnobOutputFile>(first, knob)->getValue();
        const std::string secondPath = getKnob<KnobOutputFile>(second, knob)->getValue();
        EXPECT_NE(firstPath, secondPath) << knob;
        EXPECT_NE(std::string::npos, firstPath.find(first->getScriptName())) << firstPath;
        EXPECT_NE(std::string::npos, secondPath.find(second->getScriptName())) << secondPath;
        EXPECT_NE(std::string::npos, firstPath.find("<UDIM>")) << firstPath;
    }
    EXPECT_EQ(std::string("[Project]/textures/") + first->getScriptName() + "_output.<UDIM>.exr",
              getKnob<KnobOutputFile>(first, kTexturePaintParamBakeOutputPath)->getValue());
}
