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
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

#include <QCoreApplication>
#include <QEvent>
#include <QFile>
#include <QPointer>
#include <QString>
#include <QTemporaryDir>
#include <QThread>

#include "BaseTest.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/Curve.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/LogEntry.h"
#include "Engine/Node.h"
#include "Engine/NodeColorSampler.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Nodes/Metadata/MetadataView.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Scene/MaterialBake.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/TexturePaint.h"
#include "Engine/Nodes/Scene/TexturePaintBaker.h"
#include "Engine/Nodes/Scene/TexturePaintCache.h"
#include "Engine/Nodes/Scene/TexturePaintView.h"
#include "Engine/Nodes/Scene/USDCamera.h"
#include "Engine/Nodes/Scene/USDCard.h"
#include "Engine/Nodes/Scene/USDProject3D.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoCurveInteract.h"
#include "Engine/RotoLayer.h"
#include "Engine/RotoPoint.h"
#include "Engine/RotoStrokeItem.h"
#include "Engine/RotoUndoCommand.h"
#include "Engine/UndoCommand.h"
#include "Engine/ViewIdx.h"

#include "Serialization/NodeSerialization.h"

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

namespace {
// One quad on tile 1001 whose diffuseColor is the file tex.exr beside the scene, read through st.
const char* const kTexturedQuad = R"usda(#usda 1.0
(
    upAxis = "Y"
)

def Xform "World"
{
    def Mesh "Quad" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-2, -2, 0), (2, -2, 0), (2, 2, 0), (-2, 2, 0)]
        texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 1)] (
            interpolation = "vertex"
        )
        rel material:binding = </World/Looks/Textured>
    }

    def Scope "Looks"
    {
        def Material "Textured"
        {
            token outputs:surface.connect = </World/Looks/Textured/Surface.outputs:surface>

            def Shader "Surface"
            {
                uniform token info:id = "UsdPreviewSurface"
                color3f inputs:diffuseColor.connect = </World/Looks/Textured/Texture.outputs:rgb>
                token outputs:surface
            }

            def Shader "Texture"
            {
                uniform token info:id = "UsdUVTexture"
                asset inputs:file = @tex.exr@
                float2 inputs:st.connect = </World/Looks/Textured/Reader.outputs:result>
                float3 outputs:rgb
            }

            def Shader "Reader"
            {
                uniform token info:id = "UsdPrimvarReader_float2"
                string inputs:varname = "st"
                float2 outputs:result
            }
        }
    }
}
)usda";

// Sets where paint bakes its inputs to, in dir, and returns the file of tile 1001's diffuseColor.
std::string
setBakeInputsPath(const NodePtr& paint,
                  const QTemporaryDir& dir)
{
    getKnob<KnobOutputFile>(paint, kTexturePaintParamBakeInputsPath)->setValue((dir.path() + QString::fromUtf8("/in.<UDIM>.exr")).toStdString());

    return (dir.path() + QString::fromUtf8("/in.1001.exr")).toStdString();
}

bool
bake(const NodePtr& paint,
     std::vector<std::string>* files,
     std::string* warning)
{
    std::string error;
    const bool ok = getTexturePaint(paint)->bakeInputs(1., ViewIdx(0), files, warning, &error);

    EXPECT_TRUE(ok) << error;

    return ok;
}

// The texel of the baked file at path, size texels wide, under texture coordinates (s, t).
BufferPixel
bakedTexelAt(const std::string& path,
             int size,
             double s,
             double t)
{
    BufferPixel pixel = { { 0.f, 0.f, 0.f, 0.f }, false };
    std::vector<float> rgba;
    bool resampled = true;
    std::string error;

    if (!MaterialBake::readBaseTile(path, size, &rgba, &resampled, &error)) {
        ADD_FAILURE() << error;

        return pixel;
    }
    EXPECT_FALSE(resampled) << path;
    const int x = std::min(size - 1, std::max(0, (int)(s * size)));
    const int y = std::min(size - 1, std::max(0, (int)(t * size)));
    const std::size_t i = ((std::size_t)y * (std::size_t)size + (std::size_t)x) * 4;
    for (int c = 0; c < 4; ++c) {
        pixel.rgba[c] = rgba[i + (std::size_t)c];
    }

    return pixel;
}
} // anonymous namespace

TEST_F(TexturePaintTest, ConstantDiffuseBakesIntoItsTile)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RotoLayerPtr session;
    NodePtr paint = createPaintedQuads(dir, &session);
    ASSERT_TRUE(bool(paint));
    useSmallTiles(paint);
    const std::string red = setBakeInputsPath(paint, dir);

    std::vector<std::string> files;
    std::string warning;
    ASSERT_TRUE(bake(paint, &files, &warning));
    EXPECT_TRUE(warning.empty()) << warning;
    EXPECT_NE(files.end(), std::find(files.begin(), files.end(), red));
    const std::string blue = (dir.path() + QString::fromUtf8("/in.1002.exr")).toStdString();
    EXPECT_NE(files.end(), std::find(files.begin(), files.end(), blue));

    expectColor(bakedTexelAt(red, kSmallTile, 0.5, 0.5), 1.f, 0.f, 0.f, 1.f, "red quad's tile");
    expectColor(bakedTexelAt(blue, kSmallTile, 0.5, 0.5), 0.f, 0.f, 1.f, 1.f, "blue quad's tile");
    EXPECT_EQ(files, getTexturePaint(paint)->getBakedInputFiles());

    ImagePtr tile = renderTile(paint);
    ASSERT_TRUE(bool(tile));
    expectColor(texelAt(tile, 0.5, 0.5), 1.f, 0.f, 0.f, 1.f, "the baked base");
}

TEST_F(TexturePaintTest, BakedInputsOfAnotherSizeResampleAndWarn)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    {
        const std::string path = (dir.path() + QString::fromUtf8("/tex.exr")).toStdString();
        OIIO::ImageSpec spec(kSmallTile, kSmallTile, 3, OIIO::TypeDesc::HALF);
        std::vector<float> pixels;
        for (int i = 0; i < kSmallTile * kSmallTile; ++i) {
            pixels.insert(pixels.end(), { 0.25f, 0.5f, 0.75f });
        }
        OIIO::ImageOutput::unique_ptr out = OIIO::ImageOutput::create(path);
        ASSERT_TRUE(bool(out));
        ASSERT_TRUE(out->open(path, spec)) << out->geterror();
        ASSERT_TRUE(out->write_image(OIIO::TypeDesc::FLOAT, pixels.data())) << out->geterror();
        ASSERT_TRUE(out->close()) << out->geterror();
    }
    RotoLayerPtr session;
    NodePtr paint = createPaintedScene(dir, kTexturedQuad, &session);
    ASSERT_TRUE(bool(paint));
    useSmallTiles(paint);
    const std::string base = setBakeInputsPath(paint, dir);

    std::vector<std::string> files;
    std::string warning;
    ASSERT_TRUE(bake(paint, &files, &warning));
    EXPECT_TRUE(warning.empty()) << warning;
    expectColor(bakedTexelAt(base, kSmallTile, 0.5, 0.5), 0.25f, 0.5f, 0.75f, 1.f, "the texture");

    getKnob<KnobChoice>(paint, kTexturePaintParamTileResolution)->setValueFromID("2K", 0);
    ImagePtr tile = renderTile(paint);
    ASSERT_TRUE(bool(tile));
    expectColor(texelAt(tile, 0.25, 0.25), 0.25f, 0.5f, 0.75f, 1.f, "the resampled texture");
    QString message;
    int type = -1;
    paint->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeWarning, type);
    EXPECT_TRUE(message.contains(QString::fromUtf8("resampled"))) << message.toStdString();
    EXPECT_TRUE(message.contains(QString::fromStdString(base))) << message.toStdString();
}

TEST_F(TexturePaintTest, ProjectedCardBakesItsPlate)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ProjectPtr project = getApp()->getProject();
    project->setAutoSetProjectFormatEnabled(true);
    project->setOrAddProjectFormat(Format(0, 0, 2000, 1000, "wide2to1", 1.));

    // A camera on +Z whose frame is exactly the 2 by 1 card at the origin.
    NodePtr plate = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
    NodePtr camera = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCAMERA));
    NodePtr projector = createNode(QString::fromUtf8(PLUGINID_NATRON_USDPROJECT3D));
    NodePtr card = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCARD));
    NodePtr paint = createNode(QString::fromUtf8(PLUGINID_NATRON_TEXTUREPAINT));
    ASSERT_TRUE(plate && camera && projector && card && paint);
    KnobColorPtr color = getKnob<KnobColor>(plate, kConstantParamColor);
    ASSERT_TRUE(bool(color));
    const double plateColor[4] = { 0.25, 0.5, 0.75, 1. };
    for (int c = 0; c < 4; ++c) {
        color->setValue(plateColor[c], ViewSpec::all(), c);
    }
    getKnob<KnobDouble>(camera, kUSDCameraParamFocalLength)->setValue(50.);
    getKnob<KnobDouble>(camera, kUSDCameraParamHorizontalAperture)->setValue(36.);
    getKnob<KnobDouble>(camera, kUSDCameraParamVerticalAperture)->setValue(18.);
    getKnob<KnobDouble>(camera, kSceneParamTranslate)->setValue(50. / 18., ViewSpec::all(), 2);
    ASSERT_TRUE(projector->connectInput(plate, kUSDProject3DInputImage));
    ASSERT_TRUE(projector->connectInput(camera, kUSDProject3DInputCamera));
    ASSERT_TRUE(card->connectInput(projector, 0));
    connectNodes(card, paint, 0, true);
    useSmallTiles(paint);
    const std::string base = setBakeInputsPath(paint, dir);

    std::vector<std::string> files;
    std::string warning;
    ASSERT_TRUE(bake(paint, &files, &warning));
    EXPECT_TRUE(warning.empty()) << warning;
    ASSERT_NE(files.end(), std::find(files.begin(), files.end(), base));
    for (double s : { 0.25, 0.5, 0.75 }) {
        expectColor(bakedTexelAt(base, kSmallTile, s, 0.5), 0.25f, 0.5f, 0.75f, 1.f, "the plate");
    }
}

TEST_F(TexturePaintTest, DeletedBakedInputReloadsAsMissing)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RotoLayerPtr session;
    NodePtr paint = createPaintedQuads(dir, &session);
    ASSERT_TRUE(bool(paint));
    useSmallTiles(paint);
    const std::string base = setBakeInputsPath(paint, dir);
    std::vector<std::string> files;
    std::string warning;
    ASSERT_TRUE(bake(paint, &files, &warning));
    ASSERT_TRUE(bool(renderTile(paint)));
    const std::string name = paint->getScriptName();

    ProjectPtr project = getApp()->getProject();
    const QString dirPath = dir.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("texture-paint-baked.ntpz");
    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));
    ASSERT_TRUE(QFile::remove(QString::fromStdString(base)));

    session.reset();
    paint.reset();
    project->reset(false, true);
    appPTR->clearAllCaches();
    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    NodePtr loaded = project->getNodeByName(name);
    ASSERT_TRUE(bool(loaded));
    EXPECT_FALSE(bool(renderTile(loaded)));
    QString message;
    int type = -1;
    loaded->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeError, type);
    EXPECT_TRUE(message.contains(QString::fromStdString(base))) << message.toStdString();
}

namespace {
// Sets where paint bakes its output to, in dir, and returns the file of tile 1001's diffuseColor.
std::string
setBakeOutputPath(const NodePtr& paint,
                  const QTemporaryDir& dir)
{
    getKnob<KnobOutputFile>(paint, kTexturePaintParamBakeOutputPath)->setValue((dir.path() + QString::fromUtf8("/out.<UDIM>.exr")).toStdString());

    return (dir.path() + QString::fromUtf8("/out.1001.exr")).toStdString();
}

UndoCommandPtr
bakeOutput(const NodePtr& paint,
           std::vector<std::string>* files)
{
    std::string error;
    UndoCommandPtr command = getTexturePaint(paint)->bakeOutput(1., ViewIdx(0), files, &error);

    EXPECT_TRUE(bool(command)) << error;

    return command;
}

// The RGBA texels of a rendered tile, row-major and bottom-up.
std::vector<float>
tileTexels(const ImagePtr& tile)
{
    std::vector<float> texels;

    if (!tile) {
        ADD_FAILURE() << "no tile";

        return texels;
    }
    const int nComps = (int)tile->getComponentsCount();
    Image::ReadAccess access(tile.get());
    for (int y = 0; y < kSmallTile; ++y) {
        for (int x = 0; x < kSmallTile; ++x) {
            const float* in = reinterpret_cast<const float*>(access.pixelAt(x, y));
            for (int c = 0; c < 4; ++c) {
                texels.push_back((in && (c < nComps)) ? in[c] : 0.f);
            }
        }
    }

    return texels;
}

// How many texel channels of a and b differ by more than tolerance.
std::size_t
countDifferences(const std::vector<float>& a,
                 const std::vector<float>& b,
                 float tolerance)
{
    if (a.size() != b.size()) {
        ADD_FAILURE() << "sizes differ: " << a.size() << " and " << b.size();

        return std::max(a.size(), b.size());
    }
    std::size_t count = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::abs(a[i] - b[i]) > tolerance) {
            ++count;
        }
    }

    return count;
}

// Writes a kSmallTile square texture at path whose left half is left and right half right.
void
writeSplitTexture(const std::string& path,
                  const float left[3],
                  const float right[3])
{
    OIIO::ImageSpec spec(kSmallTile, kSmallTile, 3, OIIO::TypeDesc::HALF);
    std::vector<float> pixels;
    for (int y = 0; y < kSmallTile; ++y) {
        for (int x = 0; x < kSmallTile; ++x) {
            const float* color = (x < kSmallTile / 2) ? left : right;
            pixels.insert(pixels.end(), { color[0], color[1], color[2] });
        }
    }
    OIIO::ImageOutput::unique_ptr out = OIIO::ImageOutput::create(path);
    ASSERT_TRUE(bool(out));
    ASSERT_TRUE(out->open(path, spec)) << out->geterror();
    ASSERT_TRUE(out->write_image(OIIO::TypeDesc::FLOAT, pixels.data())) << out->geterror();
    ASSERT_TRUE(out->close()) << out->geterror();
}
} // anonymous namespace

TEST_F(TexturePaintTest, BakeOutputFilesMatchTheOutput)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RotoLayerPtr session;
    NodePtr paint = createPaintedQuads(dir, &session);
    ASSERT_TRUE(bool(paint));
    useSmallTiles(paint);
    const std::string red = setBakeOutputPath(paint, dir);
    setColor(addBrushStroke(paint, session, eRotoStrokeTypeSolid, "red", kRedU, 0.5), 0., 1., 0.);
    setColor(addBrushStroke(paint, session, eRotoStrokeTypeSolid, "blue", kBlueU, 0.5), 1., 1., 0.);

    const std::vector<float> painted = tileTexels(renderTile(paint));
    std::vector<std::string> files;
    UndoCommandPtr command = bakeOutput(paint, &files);
    ASSERT_TRUE(bool(command));
    const std::string blue = (dir.path() + QString::fromUtf8("/out.1002.exr")).toStdString();
    EXPECT_NE(files.end(), std::find(files.begin(), files.end(), red));
    EXPECT_NE(files.end(), std::find(files.begin(), files.end(), blue));
    EXPECT_EQ(files, getTexturePaint(paint)->getBakedOutputFiles());
    EXPECT_TRUE(TexturePaint::isSessionFrozen(session));

    std::vector<float> baked;
    bool resampled = true;
    std::string error;
    ASSERT_TRUE(MaterialBake::readBaseTile(red, kSmallTile, &baked, &resampled, &error)) << error;
    EXPECT_FALSE(resampled);
    EXPECT_EQ(0u, countDifferences(painted, baked, 1e-3f));
    const std::vector<float> output = tileTexels(renderTile(paint));
    EXPECT_EQ(0u, countDifferences(output, baked, 1e-3f));
    expectColor(bakedTexelAt(red, kSmallTile, 0.5, 0.5), 0.f, 1.f, 0.f, 1.f, "red tile under its stroke");
    expectColor(bakedTexelAt(blue, kSmallTile, 0.5, 0.5), 1.f, 1.f, 0.f, 1.f, "blue tile under its stroke");

    getKnob<KnobInt>(paint, kTexturePaintParamOutputTile)->setValue(1002);
    ASSERT_TRUE(MaterialBake::readBaseTile(blue, kSmallTile, &baked, &resampled, &error)) << error;
    EXPECT_EQ(0u, countDifferences(tileTexels(renderTile(paint)), baked, 1e-3f));
}

TEST_F(TexturePaintTest, NewStrokesLeaveFrozenSessionsUnrendered)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RotoLayerPtr first;
    NodePtr paint = createPaintedQuads(dir, &first);
    ASSERT_TRUE(bool(paint));
    useSmallTiles(paint);
    (void)setBakeOutputPath(paint, dir);
    TexturePaint* effect = getTexturePaint(paint);
    RotoStrokeItemPtr frozenStroke = addBrushStroke(paint, first, eRotoStrokeTypeSolid, "frozen", kRedU, 0.5);
    setColor(frozenStroke, 0., 1., 0.);

    int before = effect->getSessionRenderCount();
    ASSERT_TRUE(bool(renderTile(paint)));
    const int oneSession = effect->getSessionRenderCount() - before;
    ASSERT_GT(oneSession, 0);

    std::vector<std::string> files;
    ASSERT_TRUE(bool(bakeOutput(paint, &files)));
    ASSERT_TRUE(TexturePaint::isSessionFrozen(first));

    RotoLayerPtr second = effect->addSession(viewCamera());
    ASSERT_TRUE(bool(second));
    setColor(addBrushStroke(paint, second, eRotoStrokeTypeSolid, "live", kBlueU, 0.5), 1., 1., 0.);
    before = effect->getSessionRenderCount();
    ImagePtr tile = renderTile(paint);
    ASSERT_TRUE(bool(tile));
    // Only the live session renders: the frozen one is in the baked files.
    EXPECT_EQ(oneSession, effect->getSessionRenderCount() - before);
    expectColor(texelAt(tile, 0.5, 0.5), 0.f, 1.f, 0.f, 1.f, "the frozen stroke");

    // Deleting the frozen session's strokes cannot take back what is baked.
    paint->getRotoContext()->removeItem(frozenStroke);
    frozenStroke.reset();
    tile = renderTile(paint);
    ASSERT_TRUE(bool(tile));
    expectColor(texelAt(tile, 0.5, 0.5), 0.f, 1.f, 0.f, 1.f, "the frozen stroke once deleted");

    getKnob<KnobInt>(paint, kTexturePaintParamOutputTile)->setValue(1002);
    tile = renderTile(paint);
    ASSERT_TRUE(bool(tile));
    expectColor(texelAt(tile, 0.5, 0.5), 1.f, 1.f, 0.f, 1.f, "the live stroke");
}

namespace {
typedef std::chrono::steady_clock Clock;

// Runs the main thread's event loop, where bakes land, until done() or timeoutMs passes.
bool
pumpUntil(const std::function<bool()>& done,
          int timeoutMs)
{
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);

    while (!done()) {
        if (Clock::now() > deadline) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }

    return true;
}

long
millisSince(const Clock::time_point& start)
{
    return (long)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

// Undoes or redoes a Bake Output command, whose files move on the baker, and waits for it to land.
void
undoBakeOutput(const NodePtr& paint,
               const UndoCommandPtr& command,
               bool undo)
{
    if (undo) {
        command->undo();
    } else {
        command->redo();
    }
    EXPECT_TRUE(pumpUntil([&]() { return !getTexturePaint(paint)->isBaking(); }, 30000));
}
} // anon namespace

TEST_F(TexturePaintTest, UndoingABakeBringsItsSessionsBackLive)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RotoLayerPtr session;
    NodePtr paint = createPaintedQuads(dir, &session);
    ASSERT_TRUE(bool(paint));
    useSmallTiles(paint);
    (void)setBakeOutputPath(paint, dir);
    TexturePaint* effect = getTexturePaint(paint);
    setColor(addBrushStroke(paint, session, eRotoStrokeTypeSolid, "solid", kRedU, 0.5), 0., 1., 0.);
    const std::vector<float> preBake = tileTexels(renderTile(paint));

    std::vector<std::string> files;
    UndoCommandPtr command = bakeOutput(paint, &files);
    ASSERT_TRUE(bool(command));
    command->redo();
    ASSERT_TRUE(bool(renderTile(paint)));

    undoBakeOutput(paint, command, true);
    EXPECT_FALSE(TexturePaint::isSessionFrozen(session));
    EXPECT_TRUE(effect->getBakedOutputFiles().empty());
    appPTR->clearAllCaches();
    const int before = effect->getSessionRenderCount();
    const std::vector<float> undone = tileTexels(renderTile(paint));
    EXPECT_GT(effect->getSessionRenderCount(), before) << "the session renders from its strokes";
    EXPECT_EQ(0u, countDifferences(preBake, undone, 0.f));

    // Live again: a new stroke in the session lands on the tile.
    setColor(addBrushStroke(paint, session, eRotoStrokeTypeSolid, "after", kRedU, 0.5), 1., 0., 1.);
    ImagePtr tile = renderTile(paint);
    ASSERT_TRUE(bool(tile));
    expectColor(texelAt(tile, 0.5, 0.5), 1.f, 0.f, 1.f, 1.f, "a stroke after the undo");

    undoBakeOutput(paint, command, false);
    EXPECT_TRUE(TexturePaint::isSessionFrozen(session));
    EXPECT_FALSE(effect->getBakedOutputFiles().empty());
}

TEST_F(TexturePaintTest, UndoingAStrokeRestoresThePreviousTile)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RotoLayerPtr session;
    NodePtr paint = createPaintedQuads(dir, &session);
    ASSERT_TRUE(bool(paint));
    useSmallTiles(paint);
    setColor(addBrushStroke(paint, session, eRotoStrokeTypeSolid, "first", kRedU, 0.5), 0., 1., 0.);
    const std::vector<float> previous = tileTexels(renderTile(paint));

    RotoStrokeItemPtr stroke = addBrushStroke(paint, session, eRotoStrokeTypeSolid, "second", kRedU, 0.5);
    setColor(stroke, 1., 0., 1.);
    RotoCurveInteractPtr interact = RotoCurveInteract::create(getTexturePaint(paint));
    ASSERT_TRUE(bool(interact));
    AddStrokeUndoCommand command(interact, stroke);
    command.redo();
    ImagePtr tile = renderTile(paint);
    ASSERT_TRUE(bool(tile));
    expectColor(texelAt(tile, 0.5, 0.5), 1.f, 0.f, 1.f, 1.f, "the second stroke");

    command.undo();
    EXPECT_EQ(0u, countDifferences(previous, tileTexels(renderTile(paint)), 0.f));

    command.redo();
    tile = renderTile(paint);
    ASSERT_TRUE(bool(tile));
    expectColor(texelAt(tile, 0.5, 0.5), 1.f, 0.f, 1.f, 1.f, "the second stroke redone");
}

TEST_F(TexturePaintTest, ViewShowsTheBakedBase)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const float left[3] = { 0.25f, 0.5f, 0.75f };
    const float right[3] = { 0.75f, 0.5f, 0.25f };
    writeSplitTexture((dir.path() + QString::fromUtf8("/tex.exr")).toStdString(), left, right);
    RotoLayerPtr session;
    NodePtr paint = createPaintedScene(dir, kTexturedQuad, &session);
    ASSERT_TRUE(bool(paint));
    useSmallTiles(paint);
    (void)setBakeInputsPath(paint, dir);
    (void)setBakeOutputPath(paint, dir);
    std::vector<std::string> files;
    std::string warning;
    ASSERT_TRUE(bake(paint, &files, &warning));

    // The quad spans u in [1/6, 5/6], so u = 0.35 lies on its left half and u = 0.65 on its right.
    TexturePaint::SessionBuffer buffer;
    ASSERT_TRUE(render(paint, session, ImageLayerDesc::getRGBAComponents(), &buffer));
    expectColor(pixelAt(buffer, 0.35, 0.5), left[0], left[1], left[2], 1.f, "the baked texture's left half");
    expectColor(pixelAt(buffer, 0.65, 0.5), right[0], right[1], right[2], 1.f, "the baked texture's right half");

    setColor(addBrushStroke(paint, session, eRotoStrokeTypeSolid, "solid", 0.35, 0.5), 0., 1., 0.);
    ASSERT_TRUE(bool(bakeOutput(paint, &files)));
    RotoLayerPtr next = getTexturePaint(paint)->addSession(viewCamera());
    ASSERT_TRUE(bool(next));
    ASSERT_TRUE(render(paint, next, ImageLayerDesc::getRGBAComponents(), &buffer));
    expectColor(pixelAt(buffer, 0.35, 0.5), 0.f, 1.f, 0.f, 1.f, "the baked output under the stroke");
    expectColor(pixelAt(buffer, 0.65, 0.5), right[0], right[1], right[2], 1.f, "the baked output away from it");
}

TEST_F(TexturePaintTest, QueuedSessionBakesFinishInOrder)
{
    TexturePaintBaker baker;
    std::mutex mutex;
    std::vector<int> worked;
    std::vector<int> landed;

    for (int i = 0; i < 3; ++i) {
        TexturePaintBaker::Bake bake;
        bake.label = QString::number(i);
        bake.work = [&, i](TexturePaintBaker::Context& context) {
            // Later bakes are quicker, so that only the queue keeps them in order.
            QThread::msleep(60 - 20 * i);
            context.progress(1., QString::number(i));
            std::lock_guard<std::mutex> lock(mutex);
            worked.push_back(i);

            return true;
        };
        bake.completion = [&, i](bool ok) {
            EXPECT_TRUE(ok);
            std::lock_guard<std::mutex> lock(mutex);
            landed.push_back(i);
        };
        ASSERT_TRUE(baker.enqueue(bake));
    }
    EXPECT_TRUE(baker.isBaking());
    ASSERT_TRUE(pumpUntil([&]() { return !baker.isBaking(); }, 10000));
    EXPECT_EQ((std::vector<int> { 0, 1, 2 }), worked);
    EXPECT_EQ((std::vector<int> { 0, 1, 2 }), landed);
    EXPECT_EQ(3u, baker.getGeneration());
    baker.stop();
}

TEST_F(TexturePaintTest, EnqueueingABakeNeverBlocksTheMainThread)
{
    TexturePaintBaker baker;
    std::atomic<bool> release(false);
    long slowest = 0;

    for (int i = 0; i < 5; ++i) {
        TexturePaintBaker::Bake bake;
        bake.work = [&](TexturePaintBaker::Context& context) {
            while (!release.load() && !context.cancelled()) {
                QThread::msleep(2);
            }

            return true;
        };
        const Clock::time_point start = Clock::now();
        ASSERT_TRUE(baker.enqueue(bake));
        slowest = std::max(slowest, millisSince(start));
    }
    EXPECT_LT(slowest, 50);
    EXPECT_EQ(5, baker.getPendingCount());
    release = true;
    ASSERT_TRUE(pumpUntil([&]() { return !baker.isBaking(); }, 10000));
    baker.stop();
}

TEST_F(TexturePaintTest, StoppingTheBakerCancelsTheRunningBakeAndDropsTheRest)
{
    TexturePaintBaker baker;
    std::atomic<bool> started(false);
    std::atomic<bool> sawCancel(false);
    std::atomic<int> completions(0);

    for (int i = 0; i < 3; ++i) {
        TexturePaintBaker::Bake bake;
        bake.work = [&](TexturePaintBaker::Context& context) {
            started = true;
            while (!context.cancelled()) {
                QThread::msleep(2);
            }
            sawCancel = true;

            return false;
        };
        bake.completion = [&](bool) { ++completions; };
        ASSERT_TRUE(baker.enqueue(bake));
    }
    ASSERT_TRUE(pumpUntil([&]() { return started.load(); }, 10000));
    const Clock::time_point start = Clock::now();
    baker.stop();
    EXPECT_LT(millisSince(start), 2000);
    EXPECT_TRUE(sawCancel.load());
    EXPECT_FALSE(baker.isBaking());
    QCoreApplication::processEvents();
    EXPECT_EQ(0, completions.load());
}

TEST_F(TexturePaintTest, DeletingTheNodeMidBakeCancelsItCleanly)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_TEXTUREPAINT));
    ASSERT_TRUE(bool(node));
    TexturePaint* effect = getTexturePaint(node);
    ASSERT_TRUE(effect != nullptr);
    std::atomic<bool> started(false);
    std::atomic<bool> exited(false);
    std::atomic<int> completions(0);
    TexturePaintBaker::Bake bake;
    bake.work = [&](TexturePaintBaker::Context& context) {
        started = true;
        while (!context.cancelled()) {
            QThread::msleep(2);
        }
        exited = true;

        return false;
    };
    bake.completion = [&](bool) { ++completions; };
    ASSERT_TRUE(effect->getBaker()->enqueue(bake));
    ASSERT_TRUE(effect->getBaker()->enqueue(bake));
    ASSERT_TRUE(pumpUntil([&]() { return started.load(); }, 10000));

    // A non-blocking destroy, as the node graph's delete makes, quits the node's processing from a
    // watcher thread and finishes the teardown back on the main thread.
    const Clock::time_point start = Clock::now();
    node->destroyNode(false, false);
    effect = nullptr;
    node.reset();
    EXPECT_TRUE(pumpUntil([&]() { return exited.load(); }, 5000));
    EXPECT_LT(millisSince(start), 5000);
    for (int i = 0; i < 50; ++i) {
        QCoreApplication::processEvents();
    }
    EXPECT_EQ(0, completions.load());
}

TEST_F(TexturePaintTest, SessionBakeLandsAndBumpsTheGeneration)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RotoLayerPtr session;
    NodePtr paint = createPaintedQuads(dir, &session);
    ASSERT_TRUE(bool(paint));
    useSmallTiles(paint);
    TexturePaint* effect = getTexturePaint(paint);
    setColor(addBrushStroke(paint, session, eRotoStrokeTypeSolid, "bake", kRedU, 0.5), 0., 1., 0.);

    KnobIntPtr generation = getKnob<KnobInt>(paint, kTexturePaintParamBakeGeneration);
    ASSERT_TRUE(bool(generation));
    const int before = generation->getValue();
    EXPECT_EQ(0u, effect->getBakeGeneration());
    const Clock::time_point start = Clock::now();
    ASSERT_TRUE(effect->enqueueSessionBake(session, 1., ViewIdx(0)));
    EXPECT_LT(millisSince(start), 50);
    EXPECT_TRUE(effect->isSessionBakePending(session));
    ASSERT_TRUE(pumpUntil([&]() { return !effect->isBaking(); }, 60000));
    EXPECT_FALSE(effect->isSessionBakePending(session));
    EXPECT_EQ(1u, effect->getBakeGeneration());
    EXPECT_NE(before, generation->getValue());
    ASSERT_TRUE(bool(renderTile(paint)));
}

namespace {
// Makes stroke show at frames first to last of 1 to 9 only, through its Custom lifetime.
void
limitLifetime(const RotoStrokeItemPtr& stroke,
              int first,
              int last)
{
    KnobChoicePtr lifeTime = std::dynamic_pointer_cast<KnobChoice>(stroke->getKnobByName("lifeTime"));
    ASSERT_TRUE(bool(lifeTime));
    lifeTime->setValue(4);
    KnobBoolPtr activated = stroke->getActivatedKnob();
    for (int frame = 1; frame <= 9; ++frame) {
        activated->setValueAtTime(frame, (frame >= first) && (frame <= last), ViewSpec::all(), 0);
    }
}

void
setTextureMode(const NodePtr& paint,
               const char* id)
{
    getKnob<KnobChoice>(paint, kTexturePaintParamTextureMode)->setValueFromID(id, 0);
}

bool
touchesAnything(const NodePtr& paint,
                const RotoLayerPtr& session,
                double time)
{
    TexturePaint::SessionBuffer buffer;
    std::string error;

    EXPECT_TRUE(getTexturePaint(paint)->renderSessionBuffer(session, ImageLayerDesc::getRGBAComponents(), time, ViewIdx(0), &buffer, &error)) << error;

    return std::find(buffer.touched.begin(), buffer.touched.end(), 1) != buffer.touched.end();
}

void
setProjectFrameRange(const NodePtr& node,
                     int first,
                     int last)
{
    KnobIntPtr range = std::dynamic_pointer_cast<KnobInt>(node->getApp()->getProject()->getKnobByName("frameRange"));

    ASSERT_TRUE(bool(range));
    range->setValue(first, ViewSpec::all(), 0);
    range->setValue(last, ViewSpec::all(), 1);
}

std::string
framePath(const QTemporaryDir& dir,
          int tile,
          int frame)
{
    return (dir.path() + QString::fromUtf8("/anim.%1.%2.exr").arg(tile).arg(frame, 4, 10, QLatin1Char('0'))).toStdString();
}
} // anonymous namespace

TEST_F(TexturePaintTest, AnimatedStrokeShowsOnlyOnItsLifetimeFrames)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RotoLayerPtr session;
    NodePtr paint = createPaintedQuads(dir, &session);
    ASSERT_TRUE(bool(paint));
    useSmallTiles(paint);
    setTextureMode(paint, "animated");
    limitLifetime(addBrushStroke(paint, session, eRotoStrokeTypeSolid, "short", kRedU, 0.5), 5, 7);

    for (int frame = 1; frame <= 9; ++frame) {
        EXPECT_EQ((frame >= 5) && (frame <= 7), touchesAnything(paint, session, frame)) << "frame " << frame;
    }
}

TEST_F(TexturePaintTest, StaticModeIgnoresLifetimes)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RotoLayerPtr session;
    NodePtr paint = createPaintedQuads(dir, &session);
    ASSERT_TRUE(bool(paint));
    useSmallTiles(paint);
    RotoStrokeItemPtr stroke = addBrushStroke(paint, session, eRotoStrokeTypeSolid, "short", kRedU, 0.5);
    setColor(stroke, 0., 1., 0.);
    limitLifetime(stroke, 5, 7);

    for (int frame : { 1, 6, 9 }) {
        EXPECT_TRUE(touchesAnything(paint, session, frame)) << "frame " << frame;
    }
    RectI rendered;
    const ImagePtr atNine = renderNodeImage(paint, 9., ViewIdx(0), 0, RectI(0, 0, kSmallTile, kSmallTile), ImageLayerDesc::getRGBAComponents(), &rendered);
    expectColor(texelAt(renderTile(paint), 0.5, 0.5), 0.f, 1.f, 0.f, 1.f, "the stroke at the project's first frame");
    expectColor(texelAt(atNine, 0.5, 0.5), 0.f, 1.f, 0.f, 1.f, "the stroke at another frame");
}

TEST_F(TexturePaintTest, StaticTilesFollowTheProjectsFirstFrame)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RotoLayerPtr session;
    NodePtr paint = createPaintedQuads(dir, &session);
    ASSERT_TRUE(bool(paint));
    useSmallTiles(paint);
    TexturePaint* effect = getTexturePaint(paint);
    setColor(addBrushStroke(paint, session, eRotoStrokeTypeSolid, "solid", kRedU, 0.5), 0., 1., 0.);
    ASSERT_TRUE(bool(renderTile(paint)));

    // Clearing the image caches makes the render reach the node, which then finds its composed tiles.
    appPTR->clearAllCaches();
    int before = effect->getSessionRenderCount();
    ASSERT_TRUE(bool(renderTile(paint)));
    EXPECT_EQ(before, effect->getSessionRenderCount()) << "the composed tiles are reused";

    setProjectFrameRange(paint, 3, 9);
    appPTR->clearAllCaches();
    before = effect->getSessionRenderCount();
    ASSERT_TRUE(bool(renderTile(paint)));
    EXPECT_GT(effect->getSessionRenderCount(), before) << "a static texture is composed at the new first frame";
}

TEST_F(TexturePaintTest, QuittingBackgroundWorkNeverWaitsForTheBake)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_TEXTUREPAINT));
    ASSERT_TRUE(bool(node));
    TexturePaint* effect = getTexturePaint(node);
    ASSERT_TRUE(effect != nullptr);
    std::atomic<bool> started(false);
    std::atomic<bool> release(false);
    std::atomic<bool> exited(false);
    std::atomic<int> completions(0);
    TexturePaintBaker::Bake bake;
    // Ignores cancellation, as a bake inside one long render does until that render returns.
    bake.work = [&](TexturePaintBaker::Context&) {
        started = true;
        while (!release.load()) {
            QThread::msleep(2);
        }
        exited = true;

        return false;
    };
    bake.completion = [&](bool) { ++completions; };
    ASSERT_TRUE(effect->getBaker()->enqueue(bake));
    ASSERT_TRUE(pumpUntil([&]() { return started.load(); }, 10000));

    const Clock::time_point start = Clock::now();
    effect->quitBackgroundWork(true);
    EXPECT_LT(millisSince(start), 100);
    EXPECT_FALSE(exited.load());

    release = true;
    EXPECT_TRUE(pumpUntil([&]() { return exited.load(); }, 5000));
    for (int i = 0; i < 50; ++i) {
        QCoreApplication::processEvents();
    }
    EXPECT_EQ(0, completions.load());
}

TEST_F(TexturePaintTest, RetiringABakerReturnsAtOnceAndDeletesItLater)
{
    std::unique_ptr<TexturePaintBaker> baker(new TexturePaintBaker());
    std::atomic<bool> started(false);
    std::atomic<bool> release(false);
    std::atomic<bool> exited(false);
    std::atomic<int> completions(0);
    TexturePaintBaker::Bake bake;
    bake.work = [&](TexturePaintBaker::Context&) {
        started = true;
        while (!release.load()) {
            QThread::msleep(2);
        }
        exited = true;

        return false;
    };
    bake.completion = [&](bool) { ++completions; };
    ASSERT_TRUE(baker->enqueue(bake));
    ASSERT_TRUE(pumpUntil([&]() { return started.load(); }, 10000));

    QPointer<TexturePaintBaker> watched(baker.get());
    const Clock::time_point start = Clock::now();
    TexturePaintBaker::retire(std::move(baker));
    EXPECT_LT(millisSince(start), 100);
    EXPECT_FALSE(watched.isNull()) << "a baker whose thread still runs is not deleted yet";

    release = true;
    // processEvents() alone leaves deferred deletes posted at this loop level pending.
    EXPECT_TRUE(pumpUntil([&]() {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

        return watched.isNull();
    },
                          5000));
    EXPECT_TRUE(exited.load());
    EXPECT_EQ(0, completions.load());
}

TEST_F(TexturePaintTest, AnAbortedSessionRenderFailsWithoutAnError)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RotoLayerPtr session;
    NodePtr paint = createPaintedQuads(dir, &session);
    ASSERT_TRUE(bool(paint));
    useSmallTiles(paint);
    TexturePaint* effect = getTexturePaint(paint);
    setColor(addBrushStroke(paint, session, eRotoStrokeTypeSolid, "solid", kRedU, 0.5), 0., 1., 0.);
    NodePtr viewNode = effect->getSessionView(session);
    ASSERT_TRUE(bool(viewNode));

    AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(true, 0);
    abortInfo->setAborted();
    TexturePaint::SessionBuffer buffer;
    std::string error;
    EXPECT_FALSE(effect->renderSessionBufferFrom(viewNode, session, ImageLayerDesc::getRGBAComponents(), 1., ViewIdx(0), &buffer, &error, abortInfo));
    EXPECT_TRUE(error.empty()) << error;

    TexturePaint::SessionBuffer live;
    error.clear();
    EXPECT_TRUE(effect->renderSessionBufferFrom(viewNode, session, ImageLayerDesc::getRGBAComponents(), 1., ViewIdx(0), &live, &error, AbortableRenderInfo::create(true, 0))) << error;
    EXPECT_FALSE(live.rgba.empty());
}

TEST_F(TexturePaintTest, AnimatedBakeOutputWritesAFilePerFrame)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RotoLayerPtr session;
    NodePtr paint = createPaintedQuads(dir, &session);
    ASSERT_TRUE(bool(paint));
    useSmallTiles(paint);
    setProjectFrameRange(paint, 4, 8);
    setTextureMode(paint, "animated");
    RotoStrokeItemPtr stroke = addBrushStroke(paint, session, eRotoStrokeTypeSolid, "short", kRedU, 0.5);
    setColor(stroke, 0., 1., 0.);
    limitLifetime(stroke, 5, 7);

    std::string error;
    std::vector<std::string> files;
    KnobOutputFilePtr pathKnob = getKnob<KnobOutputFile>(paint, kTexturePaintParamBakeOutputPath);
    pathKnob->setValue((dir.path() + QString::fromUtf8("/anim.<UDIM>.exr")).toStdString());
    EXPECT_FALSE(bool(getTexturePaint(paint)->bakeOutput(1., ViewIdx(0), &files, &error)));
    EXPECT_NE(std::string::npos, error.find("frame")) << error;
    EXPECT_FALSE(TexturePaint::isSessionFrozen(session));

    pathKnob->setValue((dir.path() + QString::fromUtf8("/anim.<UDIM>.####.exr")).toStdString());
    UndoCommandPtr command = bakeOutput(paint, &files);
    ASSERT_TRUE(bool(command));
    EXPECT_EQ(10u, files.size());
    for (int frame = 4; frame <= 8; ++frame) {
        for (int tile : { 1001, 1002 }) {
            const std::string path = framePath(dir, tile, frame);
            EXPECT_NE(files.end(), std::find(files.begin(), files.end(), path)) << path;
            EXPECT_TRUE(std::filesystem::is_regular_file(path)) << path;
        }
    }
    EXPECT_EQ(files, getTexturePaint(paint)->getBakedOutputFiles());
    for (int frame = 4; frame <= 8; ++frame) {
        const BufferPixel texel = bakedTexelAt(framePath(dir, 1001, frame), kSmallTile, 0.5, 0.5);
        if ((frame >= 5) && (frame <= 7)) {
            expectColor(texel, 0.f, 1.f, 0.f, 1.f, "stroke on its lifetime frame");
        } else {
            EXPECT_LT(texel.rgba[1], 0.5f) << "no stroke at frame " << frame;
        }
    }

    // The tile at a frame starts from that frame's baked file.
    RectI rendered;
    const ImageLayerDesc rgba = ImageLayerDesc::getRGBAComponents();
    const ImagePtr atSix = renderNodeImage(paint, 6., ViewIdx(0), 0, RectI(0, 0, kSmallTile, kSmallTile), rgba, &rendered);
    const ImagePtr atFour = renderNodeImage(paint, 4., ViewIdx(0), 0, RectI(0, 0, kSmallTile, kSmallTile), rgba, &rendered);
    expectColor(texelAt(atSix, 0.5, 0.5), 0.f, 1.f, 0.f, 1.f, "frame 6 of the output");
    EXPECT_LT(texelAt(atFour, 0.5, 0.5).rgba[1], 0.5f) << "frame 4 of the output";
}

TEST_F(TexturePaintTest, RedoingABakeOutputRestoresItsFilesWithoutBaking)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RotoLayerPtr session;
    NodePtr paint = createPaintedQuads(dir, &session);
    ASSERT_TRUE(bool(paint));
    useSmallTiles(paint);
    const std::string red = setBakeOutputPath(paint, dir);
    TexturePaint* effect = getTexturePaint(paint);
    setColor(addBrushStroke(paint, session, eRotoStrokeTypeSolid, "solid", kRedU, 0.5), 0., 1., 0.);

    std::vector<std::string> files;
    UndoCommandPtr command = bakeOutput(paint, &files);
    ASSERT_TRUE(bool(command));
    command->redo();
    std::vector<float> baked;
    bool resampled = true;
    std::string error;
    ASSERT_TRUE(MaterialBake::readBaseTile(red, kSmallTile, &baked, &resampled, &error)) << error;

    undoBakeOutput(paint, command, true);
    EXPECT_FALSE(TexturePaint::isSessionFrozen(session));
    EXPECT_FALSE(std::filesystem::exists(red)) << "the bake made the file, so its undo takes it away";
    // Something else takes the place of the bake's file while it is undone.
    const float grey[3] = { 0.5f, 0.5f, 0.5f };
    writeSplitTexture(red, grey, grey);

    const int renders = effect->getSessionRenderCount();
    undoBakeOutput(paint, command, false);
    EXPECT_EQ(renders, effect->getSessionRenderCount()) << "redo does not render a session";
    EXPECT_TRUE(TexturePaint::isSessionFrozen(session));
    EXPECT_EQ(files, effect->getBakedOutputFiles());
    std::vector<float> restored;
    ASSERT_TRUE(MaterialBake::readBaseTile(red, kSmallTile, &restored, &resampled, &error)) << error;
    EXPECT_EQ(0u, countDifferences(baked, restored, 0.f));
    expectColor(texelAt(renderTile(paint), 0.5, 0.5), 0.f, 1.f, 0.f, 1.f, "the redone bake");
    EXPECT_EQ(renders, effect->getSessionRenderCount()) << "the redone bake's tiles come from its files";

    undoBakeOutput(paint, command, true);
    expectColor(bakedTexelAt(red, kSmallTile, 0.5, 0.5), 0.5f, 0.5f, 0.5f, 1.f, "the file the redo replaced");
}

TEST_F(TexturePaintTest, UndoingAFirstBakeOutputBringsBackTheFilesItReplaced)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RotoLayerPtr session;
    NodePtr paint = createPaintedQuads(dir, &session);
    ASSERT_TRUE(bool(paint));
    useSmallTiles(paint);
    const std::string red = setBakeOutputPath(paint, dir);
    const std::string blue = (dir.path() + QString::fromUtf8("/out.1002.exr")).toStdString();
    const float left[3] = { 0.25f, 0.5f, 0.75f };
    const float right[3] = { 0.75f, 0.5f, 0.25f };
    writeSplitTexture(red, left, right);
    ASSERT_FALSE(std::filesystem::exists(blue));
    TexturePaint* effect = getTexturePaint(paint);
    setColor(addBrushStroke(paint, session, eRotoStrokeTypeSolid, "solid", kRedU, 0.5), 0., 1., 0.);

    std::vector<std::string> files;
    UndoCommandPtr command = bakeOutput(paint, &files);
    ASSERT_TRUE(bool(command));
    command->redo();
    ASSERT_TRUE(std::filesystem::exists(blue));
    expectColor(bakedTexelAt(red, kSmallTile, 0.5, 0.5), 0.f, 1.f, 0.f, 1.f, "the bake over the old file");

    undoBakeOutput(paint, command, true);
    EXPECT_FALSE(TexturePaint::isSessionFrozen(session));
    EXPECT_TRUE(effect->getBakedOutputFiles().empty());
    EXPECT_FALSE(std::filesystem::exists(blue)) << "the file the bake made";
    expectColor(bakedTexelAt(red, kSmallTile, 0.1, 0.5), left[0], left[1], left[2], 1.f, "the old file's left half");
    expectColor(bakedTexelAt(red, kSmallTile, 0.9, 0.5), right[0], right[1], right[2], 1.f, "the old file's right half");

    undoBakeOutput(paint, command, false);
    EXPECT_TRUE(TexturePaint::isSessionFrozen(session));
    EXPECT_TRUE(std::filesystem::exists(blue));
    expectColor(bakedTexelAt(red, kSmallTile, 0.5, 0.5), 0.f, 1.f, 0.f, 1.f, "the redone bake");
}

TEST_F(TexturePaintTest, ACachedTileAppearsWholeOrNotAtAll)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const int size = 8;
    const std::vector<float> rgba((std::size_t)size * size * 4, 0.5f);
    const std::string path = (dir.path() + QString::fromUtf8("/tile.1001.exr")).toStdString();
    std::string error;
    ASSERT_TRUE(TexturePaintCache::writeTile(path, size, rgba, "note", &error)) << error;
    std::string note;
    EXPECT_TRUE(TexturePaintCache::readTileNote(path, size, &note));
    EXPECT_EQ("note", note);

    // A directory in the way makes the write fail at its last step, once the pixels are written.
    const std::string blocked = (dir.path() + QString::fromUtf8("/blocked.1001.exr")).toStdString();
    ASSERT_TRUE(std::filesystem::create_directory(blocked));
    error.clear();
    EXPECT_FALSE(TexturePaintCache::writeTile(blocked, size, rgba, "note", &error));
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(std::filesystem::is_directory(blocked));

    std::vector<std::string> names;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(dir.path().toStdString())) {
        names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    EXPECT_EQ((std::vector<std::string> { "blocked.1001.exr", "tile.1001.exr" }), names) << "no partial file is left";
}

TEST_F(TexturePaintTest, PastedNodeBakesToItsOwnFiles)
{
    NodePtr source = createNode(QString::fromUtf8(PLUGINID_NATRON_TEXTUREPAINT));
    ASSERT_TRUE(bool(getTexturePaint(source)));
    const std::string sourceName = source->getScriptName();
    KnobOutputFilePtr sourceOutput = getKnob<KnobOutputFile>(source, kTexturePaintParamBakeOutputPath);
    sourceOutput->setValue("/shots/" + sourceName + "/" + sourceName + "_out.<UDIM>.exr");
    const std::string sourceInputs = getKnob<KnobOutputFile>(source, kTexturePaintParamBakeInputsPath)->getValue();
    ASSERT_NE(std::string::npos, sourceInputs.find(sourceName));

    Serialization::NodeSerializationPtr serialization = std::make_shared<Serialization::NodeSerialization>();
    source->toSerialization(serialization.get());
    CreateNodeArgs args(serialization->pluginID, source->getGroup());
    args.setProperty<Serialization::NodeSerializationPtr>(kCreateNodeArgsPropNodeSerialization, serialization);
    args.setProperty<int>(kCreateNodeArgsPropPluginVersion, serialization->pluginMajorVersion, 0);
    args.setProperty<int>(kCreateNodeArgsPropPluginVersion, serialization->pluginMinorVersion, 1);
    args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
    args.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, false);
    args.setProperty<bool>(kCreateNodeArgsPropSettingsOpened, false);
    args.setProperty<bool>(kCreateNodeArgsPropAllowNonUserCreatablePlugins, true);
    NodePtr copy = getApp()->createNode(args);
    ASSERT_TRUE(bool(getTexturePaint(copy)));
    const std::string copyName = copy->getScriptName();
    ASSERT_NE(sourceName, copyName);

    EXPECT_EQ("/shots/" + copyName + "/" + copyName + "_out.<UDIM>.exr", getKnob<KnobOutputFile>(copy, kTexturePaintParamBakeOutputPath)->getValue());
    const std::string copyInputs = getKnob<KnobOutputFile>(copy, kTexturePaintParamBakeInputsPath)->getValue();
    EXPECT_NE(sourceInputs, copyInputs);
    EXPECT_NE(std::string::npos, copyInputs.find(copyName)) << copyInputs;
    EXPECT_EQ("/shots/" + sourceName + "/" + sourceName + "_out.<UDIM>.exr", sourceOutput->getValue()) << "the source keeps its paths";
}

TEST_F(TexturePaintTest, NodesAndProcessesCacheTilesApart)
{
    NodePtr first = createNode(QString::fromUtf8(PLUGINID_NATRON_TEXTUREPAINT));
    NodePtr second = createNode(QString::fromUtf8(PLUGINID_NATRON_TEXTUREPAINT));
    ASSERT_TRUE(first && second);
    EXPECT_NE(first->getCacheID(), second->getCacheID());
    const std::string root = "/cache";
    const std::string id = first->getCacheID();
    EXPECT_NE(TexturePaintCache::tileDirectory(root, id, 0), TexturePaintCache::tileDirectory(root, second->getCacheID(), 0));
    EXPECT_NE(TexturePaintCache::tileDirectory(root, id, 100), TexturePaintCache::tileDirectory(root, id, 101));
    EXPECT_NE(TexturePaintCache::tileDirectory(root, id, 0), TexturePaintCache::tileDirectory(root, id, 100));

    // Above the kernel's largest pid, so that no process can have it.
    const long deadPid = 4194305;
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string cache = dir.path().toStdString();
    const std::string dead = TexturePaintCache::tileDirectory(cache, id, deadPid);
    const std::string mine = TexturePaintCache::tileDirectory(cache, id, (long)::getpid());
    const std::string saved = TexturePaintCache::tileDirectory(cache, id, 0);
    for (const std::string& path : { dead, mine, saved }) {
        ASSERT_TRUE(std::filesystem::create_directories(path)) << path;
    }
    TexturePaintCache::pruneDeadProcessDirectories(cache);
    EXPECT_FALSE(std::filesystem::exists(dead));
    EXPECT_TRUE(std::filesystem::exists(mine));
    EXPECT_TRUE(std::filesystem::exists(saved));
}

namespace {
// The one directory a Bake Output in dir keeps its backups and stashes in.
std::string
undoDirectory(const QTemporaryDir& dir)
{
    std::vector<std::string> found;
    std::error_code ec;

    for (std::filesystem::directory_iterator it(dir.path().toStdString() + "/.texturepaint-undo", ec), end; !ec && (it != end); it.increment(ec)) {
        found.push_back(it->path().string());
    }
    EXPECT_EQ(1u, found.size());

    return found.empty() ? std::string() : found.front();
}

// The temporary files Bake Outputs left in dir, which a bake that ended removes.
std::vector<std::string>
bakeTemps(const QTemporaryDir& dir)
{
    std::vector<std::string> found;
    std::error_code ec;

    for (std::filesystem::directory_iterator it(dir.path().toStdString(), ec), end; !ec && (it != end); it.increment(ec)) {
        if (it->path().filename().string().find(".baking") != std::string::npos) {
            found.push_back(it->path().string());
        }
    }

    return found;
}
} // anon namespace

TEST_F(TexturePaintTest, ARedoAfterAFailedUndoKeepsTheOriginal)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RotoLayerPtr session;
    NodePtr paint = createPaintedQuads(dir, &session);
    ASSERT_TRUE(bool(paint));
    useSmallTiles(paint);
    const std::string red = setBakeOutputPath(paint, dir);
    const float left[3] = { 0.25f, 0.5f, 0.75f };
    const float right[3] = { 0.75f, 0.5f, 0.25f };
    writeSplitTexture(red, left, right);
    setColor(addBrushStroke(paint, session, eRotoStrokeTypeSolid, "solid", kRedU, 0.5), 0., 1., 0.);

    std::vector<std::string> files;
    UndoCommandPtr command = bakeOutput(paint, &files);
    ASSERT_TRUE(bool(command));
    command->redo();

    // Something in the way of the stash makes the undo fail before it moves a file.
    const std::string undo = undoDirectory(dir);
    ASSERT_FALSE(undo.empty());
    const std::filesystem::path blocker = std::filesystem::path(undo) / "s0_out.1001.exr";
    ASSERT_TRUE(std::filesystem::create_directories(blocker / "inside"));
    undoBakeOutput(paint, command, true);
    expectColor(bakedTexelAt(red, kSmallTile, 0.5, 0.5), 0.f, 1.f, 0.f, 1.f, "the bake the failed undo left");

    undoBakeOutput(paint, command, false);
    expectColor(bakedTexelAt(red, kSmallTile, 0.5, 0.5), 0.f, 1.f, 0.f, 1.f, "the bake the redo kept");

    std::error_code ec;
    std::filesystem::remove_all(blocker, ec);
    ASSERT_FALSE(ec) << ec.message();
    undoBakeOutput(paint, command, true);
    expectColor(bakedTexelAt(red, kSmallTile, 0.1, 0.5), left[0], left[1], left[2], 1.f, "the original's left half");
    expectColor(bakedTexelAt(red, kSmallTile, 0.9, 0.5), right[0], right[1], right[2], 1.f, "the original's right half");
}

TEST_F(TexturePaintTest, UndoingABakeWhoseFilesALaterBakeReplacedIsRefused)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RotoLayerPtr session;
    NodePtr paint = createPaintedQuads(dir, &session);
    ASSERT_TRUE(bool(paint));
    useSmallTiles(paint);
    const std::string red = setBakeOutputPath(paint, dir);
    const float grey[3] = { 0.5f, 0.5f, 0.5f };
    writeSplitTexture(red, grey, grey);
    TexturePaint* effect = getTexturePaint(paint);
    setColor(addBrushStroke(paint, session, eRotoStrokeTypeSolid, "solid", kRedU, 0.5), 0., 1., 0.);

    std::vector<std::string> files;
    UndoCommandPtr first = bakeOutput(paint, &files);
    ASSERT_TRUE(bool(first));
    first->redo();

    RotoLayerPtr later = effect->addSession(viewCamera());
    ASSERT_TRUE(bool(later));
    setColor(addBrushStroke(paint, later, eRotoStrokeTypeSolid, "later", kRedU, 0.5), 0., 0., 1.);
    UndoCommandPtr second = bakeOutput(paint, &files);
    ASSERT_TRUE(bool(second));
    second->redo();
    expectColor(bakedTexelAt(red, kSmallTile, 0.5, 0.5), 0.f, 0.f, 1.f, 1.f, "the later bake");

    undoBakeOutput(paint, first, true);
    expectColor(bakedTexelAt(red, kSmallTile, 0.5, 0.5), 0.f, 0.f, 1.f, 1.f, "the later bake, which the refused undo left");

    undoBakeOutput(paint, second, true);
    expectColor(bakedTexelAt(red, kSmallTile, 0.5, 0.5), 0.f, 1.f, 0.f, 1.f, "the first bake");
    undoBakeOutput(paint, first, true);
    expectColor(bakedTexelAt(red, kSmallTile, 0.5, 0.5), 0.5f, 0.5f, 0.5f, 1.f, "the file before both bakes");
}

TEST_F(TexturePaintTest, StrokesThatRenderNothingTouchNoTexel)
{
    const bool all[4] = { true, true, true, true };
    const std::vector<float> view = { 0.2f, 0.4f, 0.6f, 1.f, 0.1f, 0.3f, 0.5f, 1.f };

    std::vector<float> base = view;
    TexturePaint::SessionBuffer buffer;
    std::string error;
    ASSERT_TRUE(TexturePaint::layStrokesOverBase(base, NULL, all, &buffer, &error)) << error;
    EXPECT_EQ(view, buffer.rgba);
    EXPECT_EQ((std::vector<std::uint8_t> { 0, 0 }), buffer.touched);

    // Paint on the second texel only, in a channel the layer does not select as well.
    const bool rgbOnly[4] = { true, true, true, false };
    std::vector<float> strokes = view;
    strokes[4] = 0.9f;
    strokes[7] = 0.f;
    base = view;
    ASSERT_TRUE(TexturePaint::layStrokesOverBase(base, &strokes, rgbOnly, &buffer, &error)) << error;
    EXPECT_EQ((std::vector<std::uint8_t> { 0, 1 }), buffer.touched);
    EXPECT_EQ((std::vector<float> { 0.2f, 0.4f, 0.6f, 1.f, 0.9f, 0.3f, 0.5f, 1.f }), buffer.rgba);

    // Strokes of another size than the view cannot be laid over it.
    const std::vector<float> shorter(view.begin(), view.begin() + 4);
    const std::vector<float> before = buffer.rgba;
    base = view;
    error.clear();
    EXPECT_FALSE(TexturePaint::layStrokesOverBase(base, &shorter, all, &buffer, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(before, buffer.rgba) << "the buffer is left as it was";
}

TEST_F(TexturePaintTest, AFailedBakeOutputComposesItsTilesAgain)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RotoLayerPtr session;
    NodePtr paint = createPaintedQuads(dir, &session);
    ASSERT_TRUE(bool(paint));
    useSmallTiles(paint);
    const std::string red = setBakeOutputPath(paint, dir);
    TexturePaint* effect = getTexturePaint(paint);
    setColor(addBrushStroke(paint, session, eRotoStrokeTypeSolid, "solid", kRedU, 0.5), 0., 1., 0.);
    ASSERT_TRUE(bool(renderTile(paint)));

    // Tile 1001 is written, then a directory in the way of tile 1002 fails the bake.
    const std::string blue = (dir.path() + QString::fromUtf8("/out.1002.exr")).toStdString();
    ASSERT_TRUE(std::filesystem::create_directories(std::filesystem::path(blue) / "inside"));
    std::vector<std::string> files;
    std::string error;
    EXPECT_FALSE(bool(effect->bakeOutput(1., ViewIdx(0), &files, &error)));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(std::filesystem::exists(red)) << "the bake's own file is rolled back";
    EXPECT_FALSE(TexturePaint::isSessionFrozen(session));

    appPTR->clearAllCaches();
    const int before = effect->getSessionRenderCount();
    ASSERT_TRUE(bool(renderTile(paint)));
    EXPECT_GT(effect->getSessionRenderCount(), before) << "tiles composed while the bake ran are not reused";
}

namespace {
void startBakeOutputWithoutLanding(TexturePaint* effect);
} // anon namespace

TEST_F(TexturePaintTest, ABakeOutputThatNeverLandsPutsItsFilesBack)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RotoLayerPtr session;
    NodePtr paint = createPaintedQuads(dir, &session);
    ASSERT_TRUE(bool(paint));
    useSmallTiles(paint);
    const std::string red = setBakeOutputPath(paint, dir);
    const float grey[3] = { 0.5f, 0.5f, 0.5f };
    writeSplitTexture(red, grey, grey);
    TexturePaint* effect = getTexturePaint(paint);
    setColor(addBrushStroke(paint, session, eRotoStrokeTypeSolid, "solid", kRedU, 0.5), 0., 1., 0.);

    startBakeOutputWithoutLanding(effect);
    // The baker runs one bake at a time: once the next starts, the first has written its files.
    std::atomic<bool> next(false);
    TexturePaintBaker::Bake marker;
    marker.work = [&](TexturePaintBaker::Context&) {
        next = true;

        return true;
    };
    ASSERT_TRUE(effect->getBaker()->enqueue(marker));
    const Clock::time_point deadline = Clock::now() + std::chrono::seconds(60);
    while (!next.load() && (Clock::now() < deadline)) {
        QThread::msleep(5);
    }
    ASSERT_TRUE(next.load());
    ASSERT_TRUE(std::filesystem::exists(dir.path().toStdString() + "/out.1002.exr"));

    effect->quitBackgroundWork(true);
    const std::string undoRoot = dir.path().toStdString() + "/.texturepaint-undo";
    // The files go back on a thread of their own.
    EXPECT_TRUE(pumpUntil([&]() { return !std::filesystem::exists(undoRoot); }, 30000));
    EXPECT_FALSE(TexturePaint::isSessionFrozen(session));
    EXPECT_TRUE(effect->getBakedOutputFiles().empty());
    expectColor(bakedTexelAt(red, kSmallTile, 0.5, 0.5), 0.5f, 0.5f, 0.5f, 1.f, "the file before the bake");
    EXPECT_FALSE(std::filesystem::exists(dir.path().toStdString() + "/out.1002.exr"));
    EXPECT_FALSE(std::filesystem::exists(dir.path().toStdString() + "/.texturepaint-undo"));
    EXPECT_TRUE(bakeTemps(dir).empty());
}

TEST_F(TexturePaintTest, WhatABakeKeepsAliveIsReleasedOnTheMainThread)
{
    std::unique_ptr<TexturePaintBaker> baker(new TexturePaintBaker());
    std::atomic<bool> released(false);
    std::atomic<bool> landed(false);
    std::thread::id releasedOn;
    std::mutex releasedMutex;
    TexturePaintBaker::Bake bake;
    bake.work = [&](TexturePaintBaker::Context& context) {
        context.keepAlive(std::shared_ptr<int>(new int(0), [&](int* value) {
            delete value;
            std::lock_guard<std::mutex> lock(releasedMutex);
            releasedOn = std::this_thread::get_id();
            released = true;
        }));

        return true;
    };
    bake.completion = [&](bool) { landed = true; };
    ASSERT_TRUE(baker->enqueue(bake));
    ASSERT_TRUE(pumpUntil([&]() { return landed.load() && released.load(); }, 10000));
    std::lock_guard<std::mutex> lock(releasedMutex);
    EXPECT_EQ(QThread::currentThread(), QCoreApplication::instance()->thread());
    EXPECT_EQ(std::this_thread::get_id(), releasedOn);
}

TEST_F(TexturePaintTest, ATileDirectoryHasOneHolderAtATime)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string root = dir.path().toStdString();
    const std::string id = "paint";
    const std::string shared = TexturePaintCache::tileDirectory(root, id, 0);

    std::shared_ptr<TexturePaintCache::TileDirectoryClaim> first = TexturePaintCache::claimTileDirectory(root, id, true);
    ASSERT_TRUE(bool(first));
    EXPECT_TRUE(first->isShared());
    EXPECT_EQ(shared, first->getPath());

    // Another process, or a node of this one, that holds the same saved project's id.
    std::shared_ptr<TexturePaintCache::TileDirectoryClaim> second = TexturePaintCache::claimTileDirectory(root, id, true);
    ASSERT_TRUE(bool(second));
    EXPECT_FALSE(second->isShared());
    EXPECT_EQ(TexturePaintCache::tileDirectory(root, id, (long)::getpid()), second->getPath());
    std::shared_ptr<TexturePaintCache::TileDirectoryClaim> third = TexturePaintCache::claimTileDirectory(root, id, false);
    ASSERT_TRUE(bool(third));
    EXPECT_NE(second->getPath(), third->getPath());
    EXPECT_NE(shared, third->getPath());

    // Only the tile files are listed, never the lock.
    const std::string tile = TexturePaintCache::tilePath(shared, 0xabcULL, std::string(), "rgba", 1001);
    std::ofstream(tile.c_str()) << "x";
    const std::map<U64, std::vector<std::string>> tiles = TexturePaintCache::listTiles(shared);
    ASSERT_EQ(1u, tiles.size());
    EXPECT_EQ(0xabcULL, tiles.begin()->first);
    EXPECT_EQ((std::vector<std::string> { tile }), tiles.begin()->second);

    const std::string secondPath = second->getPath();
    TexturePaintCache::removeTileDirectory(std::move(second));
    EXPECT_FALSE(std::filesystem::exists(secondPath)) << "moved aside at once";

    first.reset();
    std::shared_ptr<TexturePaintCache::TileDirectoryClaim> again = TexturePaintCache::claimTileDirectory(root, id, true);
    ASSERT_TRUE(bool(again));
    EXPECT_TRUE(again->isShared());
    EXPECT_TRUE(std::filesystem::exists(tile)) << "releasing a claim deletes nothing";

    // Old shared directories are pruned unless held.
    const std::string idle = TexturePaintCache::tileDirectory(root, "idle", 0);
    ASSERT_TRUE(std::filesystem::create_directories(idle));
    const std::filesystem::file_time_type old = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 40);
    std::filesystem::last_write_time(idle, old);
    std::filesystem::last_write_time(shared, old);
    TexturePaintCache::pruneDeadProcessDirectories(root, 30);
    EXPECT_FALSE(std::filesystem::exists(idle));
    EXPECT_TRUE(std::filesystem::exists(tile)) << "a held directory is in use, however old";
}

TEST_F(TexturePaintTest, PasteOfACustomPathBakesElsewhereAndStartsUnbaked)
{
    NodePtr source = createNode(QString::fromUtf8(PLUGINID_NATRON_TEXTUREPAINT));
    TexturePaint* sourceEffect = getTexturePaint(source);
    ASSERT_TRUE(sourceEffect != nullptr);
    KnobOutputFilePtr sourceOutput = getKnob<KnobOutputFile>(source, kTexturePaintParamBakeOutputPath);
    sourceOutput->setValue("/shots/textures/painted.<UDIM>.exr");
    getKnob<KnobString>(source, kTexturePaintParamBakedOutput)->setValue("1;diffuseColor;1001;1,1");
    RotoLayerPtr session = sourceEffect->addSession(viewCamera());
    ASSERT_TRUE(bool(session));
    TexturePaint::setSessionFrozen(session, true);

    Serialization::NodeSerializationPtr serialization = std::make_shared<Serialization::NodeSerialization>();
    source->toSerialization(serialization.get());
    CreateNodeArgs args(serialization->pluginID, source->getGroup());
    args.setProperty<Serialization::NodeSerializationPtr>(kCreateNodeArgsPropNodeSerialization, serialization);
    args.setProperty<int>(kCreateNodeArgsPropPluginVersion, serialization->pluginMajorVersion, 0);
    args.setProperty<int>(kCreateNodeArgsPropPluginVersion, serialization->pluginMinorVersion, 1);
    args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
    args.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, false);
    args.setProperty<bool>(kCreateNodeArgsPropSettingsOpened, false);
    args.setProperty<bool>(kCreateNodeArgsPropAllowNonUserCreatablePlugins, true);
    NodePtr copy = getApp()->createNode(args);
    TexturePaint* copyEffect = getTexturePaint(copy);
    ASSERT_TRUE(copyEffect != nullptr);

    std::string qualified = copy->getFullyQualifiedName();
    std::replace(qualified.begin(), qualified.end(), '.', '_');
    EXPECT_EQ("/shots/textures/painted_" + qualified + ".<UDIM>.exr", getKnob<KnobOutputFile>(copy, kTexturePaintParamBakeOutputPath)->getValue());
    EXPECT_EQ("/shots/textures/painted.<UDIM>.exr", sourceOutput->getValue());
    EXPECT_TRUE(getKnob<KnobString>(copy, kTexturePaintParamBakedOutput)->getValue().empty());
    EXPECT_TRUE(copyEffect->getBakedOutputFiles().empty());
    const std::list<RotoLayerPtr> sessions = copyEffect->getSessions();
    ASSERT_EQ(1u, sessions.size());
    EXPECT_FALSE(TexturePaint::isSessionFrozen(sessions.front()));
    EXPECT_TRUE(TexturePaint::isSessionFrozen(session)) << "the source keeps its record";
}

namespace {
// What path holds, empty when it cannot be read.
std::string
readText(const std::string& path)
{
    std::ifstream in(path.c_str(), std::ios::binary);

    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Long enough for a file system's coarse clock to give a later write a later modification time.
void
waitForTheClock()
{
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
}

// A paste of source into group, its own when null: a node made from its serialization, outside a
// project load.
NodePtr
pasteNode(const AppInstancePtr& app,
          const NodePtr& source,
          const NodeCollectionPtr& group = NodeCollectionPtr())
{
    Serialization::NodeSerializationPtr serialization = std::make_shared<Serialization::NodeSerialization>();
    source->toSerialization(serialization.get());
    CreateNodeArgs args(serialization->pluginID, group ? group : source->getGroup());
    args.setProperty<Serialization::NodeSerializationPtr>(kCreateNodeArgsPropNodeSerialization, serialization);
    args.setProperty<int>(kCreateNodeArgsPropPluginVersion, serialization->pluginMajorVersion, 0);
    args.setProperty<int>(kCreateNodeArgsPropPluginVersion, serialization->pluginMinorVersion, 1);
    args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
    args.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, false);
    args.setProperty<bool>(kCreateNodeArgsPropSettingsOpened, false);
    args.setProperty<bool>(kCreateNodeArgsPropAllowNonUserCreatablePlugins, true);

    return app->createNode(args);
}

std::string
qualifiedFileName(const NodePtr& node)
{
    std::string qualified = node->getFullyQualifiedName();
    std::replace(qualified.begin(), qualified.end(), '.', '_');

    return qualified;
}
} // anon namespace

TEST_F(TexturePaintTest, UndoingABakeWhoseFileWasRewrittenInPlaceIsRefused)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RotoLayerPtr session;
    NodePtr paint = createPaintedQuads(dir, &session);
    ASSERT_TRUE(bool(paint));
    useSmallTiles(paint);
    const std::string red = setBakeOutputPath(paint, dir);
    const float grey[3] = { 0.5f, 0.5f, 0.5f };
    writeSplitTexture(red, grey, grey);
    setColor(addBrushStroke(paint, session, eRotoStrokeTypeSolid, "solid", kRedU, 0.5), 0., 1., 0.);

    std::vector<std::string> files;
    UndoCommandPtr command = bakeOutput(paint, &files);
    ASSERT_TRUE(bool(command));
    command->redo();
    const TexturePaint::BakeOutputFiles::FileIdentity baked = TexturePaint::BakeOutputFiles::FileIdentity::of(red);
    ASSERT_TRUE(baked.known);

    waitForTheClock();
    const float user[3] = { 0.25f, 0.5f, 0.75f };
    writeSplitTexture(red, user, user);
    const TexturePaint::BakeOutputFiles::FileIdentity rewritten = TexturePaint::BakeOutputFiles::FileIdentity::of(red);
    ASSERT_EQ(baked.device, rewritten.device);
    ASSERT_EQ(baked.inode, rewritten.inode) << "the file is rewritten in place";
    ASSERT_TRUE(baked != rewritten);

    undoBakeOutput(paint, command, true);
    expectColor(bakedTexelAt(red, kSmallTile, 0.5, 0.5), 0.25f, 0.5f, 0.75f, 1.f, "the user's file, which the refused undo left");
    EXPECT_TRUE(TexturePaint::isSessionFrozen(session)) << "the refused undo changes nothing";

    // Dropping the step deletes its backups, never the target.
    command.reset();
    const std::string undoRoot = dir.path().toStdString() + "/.texturepaint-undo";
    EXPECT_TRUE(pumpUntil([&]() { return !std::filesystem::exists(undoRoot); }, 30000));
    expectColor(bakedTexelAt(red, kSmallTile, 0.5, 0.5), 0.25f, 0.5f, 0.75f, 1.f, "the user's file, once the step is gone");
}

namespace {
// Writes content to target as a bake does: a whole file renamed over it.
void
writeByRename(const std::string& target,
              const std::string& content)
{
    std::ofstream((target + ".tmp").c_str()) << content;
    std::filesystem::rename(target + ".tmp", target);
}

/**
 * Holds the file queue's thread before each step until released, and records the thread the
 * step ran on. Shared with the hook, which may still be returning when the gate goes.
 **/
class RollbackGate {
public:
    RollbackGate()
        : _state(std::make_shared<State>())
    {
        const std::shared_ptr<State> state = _state;
        TexturePaintFileQueue::setBeforeWorkHookForTesting([state]() {
            std::unique_lock<std::mutex> lock(state->mutex);
            ++state->held;
            state->thread = std::this_thread::get_id();
            state->changed.notify_all();
            state->changed.wait(lock, [&state]() { return state->released; });
        });
    }

    ~RollbackGate()
    {
        release();
        TexturePaintFileQueue::setBeforeWorkHookForTesting(std::function<void()>());
    }

    bool waitUntilHeld(int timeoutMs)
    {
        std::unique_lock<std::mutex> lock(_state->mutex);

        return _state->changed.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this]() { return _state->held > 0; });
    }

    std::thread::id thread() const
    {
        std::lock_guard<std::mutex> lock(_state->mutex);

        return _state->thread;
    }

    void release()
    {
        std::lock_guard<std::mutex> lock(_state->mutex);
        _state->released = true;
        _state->changed.notify_all();
    }

private:
    struct State {
        std::mutex mutex;
        std::condition_variable changed;
        int held = 0;
        bool released = false;
        std::thread::id thread;
    };
    std::shared_ptr<State> _state;
};
} // anon namespace

TEST_F(TexturePaintTest, ADroppedBakeRollsBackOnTheFileQueueAndKeepsWhatARewrittenTargetHeld)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    NodePtr paint = createNode(QString::fromUtf8(PLUGINID_NATRON_TEXTUREPAINT));
    const std::shared_ptr<TexturePaint> effect = std::dynamic_pointer_cast<TexturePaint>(paint->getEffectInstance());
    ASSERT_TRUE(bool(effect));
    const std::string kept = dir.path().toStdString() + "/kept.1001.exr";
    const std::string rewritten = dir.path().toStdString() + "/rewritten.1001.exr";
    std::ofstream(kept.c_str()) << "kept before";
    std::ofstream(rewritten.c_str()) << "rewritten before";

    std::shared_ptr<TexturePaint::BakeOutputFiles> files = std::make_shared<TexturePaint::BakeOutputFiles>();
    std::string error;
    ASSERT_TRUE(TexturePaint::backUpBakeTarget(files.get(), kept, &error)) << error;
    ASSERT_TRUE(TexturePaint::backUpBakeTarget(files.get(), rewritten, &error)) << error;
    for (const std::string& target : { kept, rewritten }) {
        writeByRename(target, "the bake's");
        TexturePaint::markBakeTargetWritten(files.get(), target);
    }
    const TexturePaint::BakeOutputFiles::FileIdentity baked = TexturePaint::BakeOutputFiles::FileIdentity::of(rewritten);

    // Same size and inode: only the modification time tells the user's file from the bake's.
    waitForTheClock();
    {
        std::ofstream out(rewritten.c_str(), std::ios::in | std::ios::out | std::ios::binary);
        out << "the user's";
    }
    const TexturePaint::BakeOutputFiles::FileIdentity user = TexturePaint::BakeOutputFiles::FileIdentity::of(rewritten);
    ASSERT_EQ(baked.inode, user.inode);
    ASSERT_EQ(baked.size, user.size);
    ASSERT_TRUE(baked != user);

    std::shared_ptr<std::atomic<std::uint64_t>> epoch = std::make_shared<std::atomic<std::uint64_t>>(0);
    {
        RollbackGate gate;
        (void)TexturePaint::enqueueBakeOutputRollback(files, epoch, effect);
        ASSERT_TRUE(gate.waitUntilHeld(10000));
        EXPECT_NE(std::this_thread::get_id(), gate.thread()) << "the rollback runs on the file queue's thread";
        EXPECT_EQ("the bake's", readText(kept)) << "the caller returned before the rollback ran";
        gate.release();
        ASSERT_TRUE(pumpUntil([&]() { return (epoch->load() == 2) && TexturePaintFileQueue::isIdleForTesting(); }, 10000));
    }
    files.reset();
    EXPECT_EQ("kept before", readText(kept));
    EXPECT_EQ("the user's", readText(rewritten)) << "a target written since the bake is left as it is";
    EXPECT_EQ("rewritten before", readText(rewritten + ".pre-bake")) << "what it held before the bake is kept beside it";
    EXPECT_FALSE(std::filesystem::exists(dir.path().toStdString() + "/.texturepaint-undo"));

    ASSERT_TRUE(pumpUntil([&]() { return paint->hasPersistentMessage(); }, 10000));
    QString message;
    int type = 0;
    paint->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeWarning, type);
    EXPECT_TRUE(message.contains(QString::fromStdString(rewritten + ".pre-bake"))) << message.toStdString();
    std::list<LogEntry> log;
    appPTR->getErrorLog_mt_safe(&log);
    EXPECT_TRUE(std::any_of(log.begin(), log.end(), [&](const LogEntry& entry) { return entry.message.contains(QString::fromStdString(rewritten + ".pre-bake")); }));
}

TEST_F(TexturePaintTest, AFailedBakePutsBackOnlyWhatItWroteAndKeepsWhatWasEditedMidBake)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string first = dir.path().toStdString() + "/first.1001.exr";
    const std::string second = dir.path().toStdString() + "/second.1001.exr";
    const std::string third = dir.path().toStdString() + "/third.1001.exr";
    std::ofstream(first.c_str()) << "first before";
    std::ofstream(second.c_str()) << "second before";
    std::ofstream(third.c_str()) << "third before";

    std::shared_ptr<TexturePaint::BakeOutputFiles> files = std::make_shared<TexturePaint::BakeOutputFiles>();
    std::string error;
    ASSERT_TRUE(TexturePaint::backUpBakeTarget(files.get(), first, &error)) << error;
    writeByRename(first, "first baked");
    TexturePaint::markBakeTargetWritten(files.get(), first);

    // Same size and inode: only the modification time tells the user's edit from the bake's file.
    waitForTheClock();
    {
        std::ofstream out(first.c_str(), std::ios::in | std::ios::out | std::ios::binary);
        out << "first edits";
    }

    ASSERT_TRUE(TexturePaint::backUpBakeTarget(files.get(), second, &error)) << error;
    writeByRename(second, "second baked");
    TexturePaint::markBakeTargetWritten(files.get(), second);

    // Backed up, then the bake failed before its file reached the target.
    ASSERT_TRUE(TexturePaint::backUpBakeTarget(files.get(), third, &error)) << error;
    writeByRename(third, "third edits");
    const TexturePaint::BakeOutputFiles::FileIdentity thirdBefore = TexturePaint::BakeOutputFiles::FileIdentity::of(third);

    std::shared_ptr<std::atomic<std::uint64_t>> epoch = std::make_shared<std::atomic<std::uint64_t>>(0);
    (void)TexturePaint::enqueueBakeOutputRollback(files, epoch);
    ASSERT_TRUE(pumpUntil([&]() { return (epoch->load() == 2) && TexturePaintFileQueue::isIdleForTesting(); }, 10000));
    files.reset();

    EXPECT_EQ("first edits", readText(first)) << "an edit made mid-bake is left as it is";
    EXPECT_EQ("first before", readText(first + ".pre-bake")) << "what it held before the bake is kept beside it";
    EXPECT_EQ("second before", readText(second));
    EXPECT_FALSE(std::filesystem::exists(second + ".pre-bake"));
    EXPECT_EQ("third edits", readText(third)) << "a target the bake never wrote is not put back";
    EXPECT_TRUE(TexturePaint::BakeOutputFiles::FileIdentity::of(third) == thirdBefore);
    EXPECT_FALSE(std::filesystem::exists(third + ".pre-bake"));
    EXPECT_FALSE(std::filesystem::exists(dir.path().toStdString() + "/.texturepaint-undo"));
}

namespace {
// A node's identity on the file queue, named as long as it lives.
struct NamedOwner {
    explicit NamedOwner(const std::string& name)
        : id(TexturePaintFileQueue::newId())
    {
        TexturePaintFileQueue::setOwnerName(id, name);
    }

    ~NamedOwner()
    {
        TexturePaintFileQueue::forgetOwner(id);
    }

    NamedOwner(const NamedOwner&) = delete;
    NamedOwner& operator=(const NamedOwner&) = delete;

    const std::uint64_t id;
};

// A Bake Output's files as a node's bake starts them.
std::shared_ptr<TexturePaint::BakeOutputFiles>
bakeOutputFilesOf(const NamedOwner& owner)
{
    std::shared_ptr<TexturePaint::BakeOutputFiles> files = std::make_shared<TexturePaint::BakeOutputFiles>();
    files->owner = owner.id;

    return files;
}

// Backs up target for files and writes content over it, as a bake does on the file queue.
bool
bakeOverOnTheQueue(const std::shared_ptr<TexturePaint::BakeOutputFiles>& files,
                   const std::string& target,
                   const std::string& content,
                   std::string* error)
{
    std::shared_ptr<bool> ok = std::make_shared<bool>(false);
    std::shared_ptr<std::string> failure = std::make_shared<std::string>();
    if (!TexturePaintFileQueue::wait(TexturePaintFileQueue::submit([=](bool) {
            *ok = TexturePaint::backUpBakeTarget(files.get(), target, failure.get());
            if (*ok) {
                writeByRename(target, content);
                TexturePaint::markBakeTargetWritten(files.get(), target);
            }
        }))) {
        return false;
    }
    *error = *failure;

    return *ok;
}
} // anon namespace

TEST_F(TexturePaintTest, AnUnsettledBakeHoldsItsFilesAgainstEveryOtherBake)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string target = dir.path().toStdString() + "/target.1001.exr";
    std::ofstream(target.c_str()) << "the original";
    const NamedOwner paintA("Group1.PaintA");
    const NamedOwner paintB("PaintB");

    std::string error;
    std::shared_ptr<TexturePaint::BakeOutputFiles> first = bakeOutputFilesOf(paintA);
    ASSERT_TRUE(bakeOverOnTheQueue(first, target, "the first bake", &error)) << error;

    std::shared_ptr<TexturePaint::BakeOutputFiles> otherNode = bakeOutputFilesOf(paintB);
    bool heldBySameOwner = true;
    EXPECT_FALSE(TexturePaint::backUpBakeTarget(otherNode.get(), target, &error, &heldBySameOwner));
    EXPECT_FALSE(heldBySameOwner);
    EXPECT_NE(std::string::npos, error.find("Group1.PaintA")) << "the error names the node holding the file: " << error;
    EXPECT_TRUE(otherNode->entries.empty());

    std::shared_ptr<TexturePaint::BakeOutputFiles> sameNode = bakeOutputFilesOf(paintA);
    EXPECT_FALSE(TexturePaint::backUpBakeTarget(sameNode.get(), target, &error, &heldBySameOwner));
    EXPECT_TRUE(heldBySameOwner) << "a later bake of the same node waits for the earlier one instead of failing";
    EXPECT_EQ("the first bake", readText(target));

    std::uint64_t holder = 0;
    EXPECT_TRUE(TexturePaintFileQueue::isHeldByAnotherBake(otherNode->bake, TexturePaintFileQueue::claimKey(target), &holder));
    EXPECT_EQ(paintA.id, holder);
    EXPECT_EQ("Group1.PaintA", TexturePaintFileQueue::getOwnerName(holder));

    // Two dropped bakes of one file: the newer one could only claim it once the older one went
    // back, so the file ends as it was before both.
    const TexturePaintFileTicket rollBack = TexturePaint::enqueueBakeOutputRollback(first, std::shared_ptr<std::atomic<std::uint64_t>>());
    ASSERT_TRUE(TexturePaintFileQueue::wait(rollBack));
    EXPECT_EQ("the original", readText(target));
    ASSERT_TRUE(bakeOverOnTheQueue(otherNode, target, "the other node's bake", &error)) << error;
    ASSERT_TRUE(TexturePaintFileQueue::wait(TexturePaint::enqueueBakeOutputRollback(otherNode, std::shared_ptr<std::atomic<std::uint64_t>>())));
    EXPECT_EQ("the original", readText(target));
    EXPECT_FALSE(std::filesystem::exists(target + ".pre-bake"));
    EXPECT_FALSE(std::filesystem::exists(dir.path().toStdString() + "/.texturepaint-undo"));
}

TEST_F(TexturePaintTest, ABakeAfterADroppedOneBacksUpTheOriginal)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string target = dir.path().toStdString() + "/target.1001.exr";
    std::ofstream(target.c_str()) << "the original";
    const NamedOwner paint("Paint1");

    std::string error;
    std::shared_ptr<TexturePaint::BakeOutputFiles> dropped = bakeOutputFilesOf(paint);
    ASSERT_TRUE(bakeOverOnTheQueue(dropped, target, "the dropped bake", &error)) << error;

    std::shared_ptr<std::atomic<std::uint64_t>> epoch = std::make_shared<std::atomic<std::uint64_t>>(0);
    std::shared_ptr<TexturePaint::BakeOutputFiles> next = bakeOutputFilesOf(paint);
    std::shared_ptr<bool> ok = std::make_shared<bool>(false);
    std::shared_ptr<std::string> nextError = std::make_shared<std::string>();
    TexturePaintFileTicket backUp;
    {
        RollbackGate gate;
        (void)TexturePaint::enqueueBakeOutputRollback(dropped, epoch);
        ASSERT_TRUE(gate.waitUntilHeld(10000));
        // Submitted while the rollback has not run: the queue still runs it after the rollback.
        backUp = TexturePaintFileQueue::submit([next, target, ok, nextError](bool) {
            *ok = TexturePaint::backUpBakeTarget(next.get(), target, nextError.get());
        });
        EXPECT_EQ("the dropped bake", readText(target));
        gate.release();
        ASSERT_TRUE(TexturePaintFileQueue::wait(backUp));
    }
    ASSERT_TRUE(*ok) << *nextError;
    EXPECT_EQ(2u, epoch->load());
    ASSERT_EQ(1u, next->entries.size());
    ASSERT_TRUE(next->entries[0].hasBackup);
    EXPECT_EQ("the original", readText(next->entries[0].backup));

    ASSERT_TRUE(TexturePaintFileQueue::wait(TexturePaint::enqueueBakeOutputRollback(next, epoch)));
    EXPECT_EQ("the original", readText(target));
}

TEST_F(TexturePaintTest, AnUndoSubmittedAfterARollbackOfTheSameFileRunsAfterIt)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string target = dir.path().toStdString() + "/target.1001.exr";
    std::ofstream(target.c_str()) << "the original";
    const NamedOwner paint("Paint1");

    // A landed bake, whose undo step owns its files.
    std::string error;
    std::shared_ptr<TexturePaint::BakeOutputFiles> landed = bakeOutputFilesOf(paint);
    ASSERT_TRUE(bakeOverOnTheQueue(landed, target, "the landed bake", &error)) << error;
    TexturePaint::settleBakeOutputFiles(*landed);
    // A later bake of the same file that was then dropped.
    std::shared_ptr<TexturePaint::BakeOutputFiles> dropped = bakeOutputFilesOf(paint);
    ASSERT_TRUE(bakeOverOnTheQueue(dropped, target, "the dropped bake", &error)) << error;

    std::string swapError;
    EXPECT_FALSE(TexturePaint::swapBakeOutputFiles(landed.get(), true, &swapError)) << "an undo never moves a file another bake holds";
    EXPECT_NE(std::string::npos, swapError.find("Paint1")) << swapError;
    EXPECT_EQ("the dropped bake", readText(target));

    std::shared_ptr<bool> undone = std::make_shared<bool>(false);
    std::shared_ptr<std::string> undoError = std::make_shared<std::string>();
    {
        RollbackGate gate;
        (void)TexturePaint::enqueueBakeOutputRollback(dropped, std::shared_ptr<std::atomic<std::uint64_t>>());
        ASSERT_TRUE(gate.waitUntilHeld(10000));
        const TexturePaintFileTicket undo = TexturePaintFileQueue::submit([landed, undone, undoError](bool) {
            *undone = TexturePaint::swapBakeOutputFiles(landed.get(), true, undoError.get());
        });
        gate.release();
        ASSERT_TRUE(TexturePaintFileQueue::wait(undo));
    }
    ASSERT_TRUE(*undone) << *undoError;
    EXPECT_EQ("the original", readText(target)) << "the rollback put the landed bake's file back, then the undo the original";
    EXPECT_FALSE(std::filesystem::exists(target + ".pre-bake"));

    TexturePaint::discardBakeOutputFiles(*landed);
    EXPECT_FALSE(std::filesystem::exists(dir.path().toStdString() + "/.texturepaint-undo"));
}

TEST_F(TexturePaintTest, ABakeRemovesOnlyTheTemporaryFilesOfEndedProcessesOfThisPidNamespace)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string target = dir.path().toStdString() + "/texture.1001.exr";
    const std::string& space = TexturePaint::bakeProcessSpace();
    ASSERT_EQ(12u, space.size()) << "a short token, whatever the host name";
    EXPECT_EQ(std::string::npos, space.find_first_not_of("0123456789abcdef")) << space;
    const long self = (long)::getpid();
    unsigned long long started = 0;
    ASSERT_TRUE(TexturePaint::processStartTime(self, &started));

    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        ::_exit(0);
    }
    int status = 0;
    ASSERT_EQ(child, ::waitpid(child, &status, 0));
    const long ended = (long)child;
    unsigned long long unused = 0;
    ASSERT_FALSE(TexturePaint::processStartTime(ended, &unused));

    const std::string otherSpace = (space == "000000000000") ? std::string("ffffffffffff") : std::string("000000000000");
    const std::string dead = TexturePaint::bakeTempPath(target, space, ended, started, 1);
    const std::string earlierRun = TexturePaint::bakeTempPath(target, space, self, started + 1, 2);
    const std::string live = TexturePaint::bakeTempPath(target, space, self, started, 3);
    const std::string elsewhere = TexturePaint::bakeTempPath(target, otherSpace, ended, started, 4);
    for (const std::string& path : { dead, earlierRun, live, elsewhere }) {
        std::ofstream(path.c_str()) << "partial";
        ASSERT_TRUE(std::filesystem::exists(path)) << path;
    }

    TexturePaint::removeStaleBakeTemps(dir.path().toStdString());
    EXPECT_FALSE(std::filesystem::exists(dead)) << "the process that wrote it has ended";
    EXPECT_FALSE(std::filesystem::exists(earlierRun)) << "this pid then named an earlier process, started at another time";
    EXPECT_TRUE(std::filesystem::exists(live)) << "this process may still rename it";
    EXPECT_TRUE(std::filesystem::exists(elsewhere)) << "its pid names a process of another pid namespace";
}

TEST_F(TexturePaintTest, ShuttingDownStopsEveryBakerAndPutsBackTheFilesOfABakeThatNeverLanded)
{
    // Destroyed last, whatever fails first, so that the tests after this one bake again.
    struct RestartAfterShutdown {
        ~RestartAfterShutdown()
        {
            TexturePaintBaker::restartForTesting();
        }
    } restart;
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RotoLayerPtr session;
    NodePtr paint = createPaintedQuads(dir, &session);
    ASSERT_TRUE(bool(paint));
    useSmallTiles(paint);
    const std::string red = setBakeOutputPath(paint, dir);
    const float grey[3] = { 0.5f, 0.5f, 0.5f };
    writeSplitTexture(red, grey, grey);
    TexturePaint* effect = getTexturePaint(paint);
    setColor(addBrushStroke(paint, session, eRotoStrokeTypeSolid, "solid", kRedU, 0.5), 0., 1., 0.);

    startBakeOutputWithoutLanding(effect);
    // Running once the bake has written its files, and only cancellation ends it; nothing pumps
    // the main thread meanwhile, so the bake's completion is still queued when shutting down.
    std::atomic<bool> started(false);
    TexturePaintBaker::Bake marker;
    marker.work = [&](TexturePaintBaker::Context& context) {
        started = true;
        const Clock::time_point deadline = Clock::now() + std::chrono::seconds(120);
        while (!context.cancelled() && (Clock::now() < deadline)) {
            QThread::msleep(2);
        }

        return false;
    };
    ASSERT_TRUE(effect->getBaker()->enqueue(marker));
    const Clock::time_point deadline = Clock::now() + std::chrono::seconds(60);
    while (!started.load() && (Clock::now() < deadline)) {
        QThread::msleep(5);
    }
    ASSERT_TRUE(started.load());
    ASSERT_TRUE(std::filesystem::exists(dir.path().toStdString() + "/out.1002.exr"));

    TexturePaintBaker::shutdownAll();
    EXPECT_FALSE(effect->getBaker()->isRunning()) << "the baker is joined";
    EXPECT_FALSE(TexturePaintFileQueue::isThreadRunningForTesting()) << "the file queue is drained and joined";
    expectColor(bakedTexelAt(red, kSmallTile, 0.5, 0.5), 0.5f, 0.5f, 0.5f, 1.f, "the file before the bake");
    EXPECT_FALSE(std::filesystem::exists(dir.path().toStdString() + "/out.1002.exr"));
    EXPECT_FALSE(std::filesystem::exists(dir.path().toStdString() + "/.texturepaint-undo"));
    EXPECT_TRUE(bakeTemps(dir).empty());
}

TEST_F(TexturePaintTest, PasteOfAPasteReplacesTheSuffixAndKeepsEmptyPathsEmpty)
{
    NodePtr source = createNode(QString::fromUtf8(PLUGINID_NATRON_TEXTUREPAINT));
    ASSERT_TRUE(bool(getTexturePaint(source)));
    getKnob<KnobOutputFile>(source, kTexturePaintParamBakeOutputPath)->setValue("/shots/textures/painted.<UDIM>.exr");
    getKnob<KnobOutputFile>(source, kTexturePaintParamBakeInputsPath)->setValue(std::string());

    NodePtr copy = pasteNode(getApp(), source);
    ASSERT_TRUE(bool(getTexturePaint(copy)));
    NodePtr copyOfCopy = pasteNode(getApp(), copy);
    ASSERT_TRUE(bool(getTexturePaint(copyOfCopy)));

    EXPECT_EQ("/shots/textures/painted_" + qualifiedFileName(copy) + ".<UDIM>.exr", getKnob<KnobOutputFile>(copy, kTexturePaintParamBakeOutputPath)->getValue());
    EXPECT_EQ("/shots/textures/painted_" + qualifiedFileName(copyOfCopy) + ".<UDIM>.exr", getKnob<KnobOutputFile>(copyOfCopy, kTexturePaintParamBakeOutputPath)->getValue());
    EXPECT_TRUE(getKnob<KnobOutputFile>(copy, kTexturePaintParamBakeInputsPath)->getValue().empty());
    EXPECT_TRUE(getKnob<KnobOutputFile>(copyOfCopy, kTexturePaintParamBakeInputsPath)->getValue().empty());
}

TEST_F(TexturePaintTest, PasteOfAPasteIntoAnotherGroupReplacesTheGroupQualifiedSuffix)
{
    AppInstancePtr app = getApp();
    const NodeCollectionPtr project = app->getProject();
    // Group names holding "_" themselves, and one that ends another: "grp" ends "fx_grp".
    const std::vector<std::pair<std::string, std::string>> pairs = { { "Outer", "Other" }, { "grp", "lt_grp" }, { "fx_grp", "Other2" } };
    for (const std::pair<std::string, std::string>& pair : pairs) {
        SCOPED_TRACE(pair.first + " then " + pair.second);
        std::vector<NodePtr> groups;
        for (const std::string& name : { pair.first, pair.second }) {
            CreateNodeArgs args(PLUGINID_NATRON_GROUP, project);
            args.setProperty<bool>(kCreateNodeArgsPropNodeGroupDisableCreateInitialNodes, true);
            args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
            args.setProperty<std::string>(kCreateNodeArgsPropNodeInitialName, name);
            NodePtr group = app->createNode(args);
            ASSERT_TRUE(bool(group));
            ASSERT_EQ(name, group->getScriptName());
            groups.push_back(group);
        }
        NodePtr source = createNode(QString::fromUtf8(PLUGINID_NATRON_TEXTUREPAINT));
        ASSERT_TRUE(bool(getTexturePaint(source)));
        getKnob<KnobOutputFile>(source, kTexturePaintParamBakeOutputPath)->setValue("/shots/textures/painted.<UDIM>.exr");

        NodePtr copy = pasteNode(app, source, std::dynamic_pointer_cast<NodeCollection>(groups[0]->getEffectInstance()));
        ASSERT_TRUE(bool(getTexturePaint(copy)));
        ASSERT_NE(std::string::npos, qualifiedFileName(copy).find(pair.first + "_")) << "the copy's name is qualified by its group";
        EXPECT_EQ("/shots/textures/painted_" + qualifiedFileName(copy) + ".<UDIM>.exr", getKnob<KnobOutputFile>(copy, kTexturePaintParamBakeOutputPath)->getValue());

        NodePtr copyOfCopy = pasteNode(app, copy, std::dynamic_pointer_cast<NodeCollection>(groups[1]->getEffectInstance()));
        ASSERT_TRUE(bool(getTexturePaint(copyOfCopy)));
        EXPECT_EQ("/shots/textures/painted_" + qualifiedFileName(copyOfCopy) + ".<UDIM>.exr", getKnob<KnobOutputFile>(copyOfCopy, kTexturePaintParamBakeOutputPath)->getValue()) << "nothing of the first group's qualified name is left";
    }
}

TEST_F(TexturePaintTest, AClaimedTileDirectoryIsNeverPrunedAndExcludesOtherClaimsOfTheProcess)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string root = dir.path().toStdString();
    const std::string id = "held";
    const std::string shared = TexturePaintCache::tileDirectory(root, id, 0);
    const std::filesystem::file_time_type::clock::time_point old = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 40);
    ASSERT_TRUE(std::filesystem::create_directories(shared));
    // Made beforehand, so that only the claim itself can change the directory's time.
    std::ofstream((shared + "/.lock").c_str()) << "";
    std::filesystem::last_write_time(shared, old);

    std::shared_ptr<TexturePaintCache::TileDirectoryClaim> first = TexturePaintCache::claimTileDirectory(root, id, true);
    ASSERT_TRUE(bool(first));
    ASSERT_TRUE(first->isShared());
    EXPECT_GT(std::filesystem::last_write_time(shared), old + std::chrono::hours(24 * 39)) << "a claim touches the directory";

    std::filesystem::last_write_time(shared, old);
    TexturePaintCache::pruneDeadProcessDirectories(root, 30);
    ASSERT_TRUE(std::filesystem::exists(shared)) << "a held directory is in use, however old";
    EXPECT_TRUE(first->isShared());
    EXPECT_EQ(shared, first->getPath());

    std::shared_ptr<TexturePaintCache::TileDirectoryClaim> second = TexturePaintCache::claimTileDirectory(root, id, true);
    ASSERT_TRUE(bool(second));
    EXPECT_FALSE(second->isShared()) << "two claims of one process exclude each other";
    second.reset();
    std::shared_ptr<TexturePaintCache::TileDirectoryClaim> third = TexturePaintCache::claimTileDirectory(root, id, true);
    ASSERT_TRUE(bool(third));
    EXPECT_FALSE(third->isShared()) << "releasing one claim leaves the other held";
    third.reset();

    first.reset();
    std::shared_ptr<TexturePaintCache::TileDirectoryClaim> again = TexturePaintCache::claimTileDirectory(root, id, true);
    ASSERT_TRUE(bool(again));
    EXPECT_TRUE(again->isShared());
}

namespace {
// Whether a child process gets id's shared tile directory under root: 1 when it does, 0 when not,
// -1 when the child cannot be run or does not finish. Forked rather than a re-run of this binary,
// which would load every plug-in again for one claim. A forked copy of a threaded process may only
// use what no other thread held at the fork: the child only claims and exits, glibc makes malloc
// safe across fork, and no other thread of the test claims tile directories meanwhile. Should one
// ever do so, the deadline turns the hung child into a failure, never a hung test.
int
childGetsSharedTileDirectory(const std::string& root,
                             const std::string& id)
{
    const pid_t child = ::fork();
    if (child < 0) {
        return -1;
    }
    if (child == 0) {
        TexturePaintCache::forgetProcessClaimsForTesting();
        int code = 2;
        {
            std::shared_ptr<TexturePaintCache::TileDirectoryClaim> claim = TexturePaintCache::claimTileDirectory(root, id, true);
            if (claim) {
                code = claim->isShared() ? 1 : 0;
            }
        }
        ::_exit(code);
    }
    const Clock::time_point deadline = Clock::now() + std::chrono::seconds(20);
    int status = 0;
    for (;;) {
        const pid_t done = ::waitpid(child, &status, WNOHANG);
        if (done == child) {
            break;
        }
        if ((done < 0) || (Clock::now() > deadline)) {
            ::kill(child, SIGKILL);
            ::waitpid(child, &status, 0);

            return -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!WIFEXITED(status) || (WEXITSTATUS(status) > 1)) {
        return -1;
    }

    return WEXITSTATUS(status);
}
} // anon namespace

TEST_F(TexturePaintTest, ATileDirectoryClaimExcludesClaimsOfThisProcessAndOthersWithEitherLockKind)
{
    struct LockKindReset {
        ~LockKindReset() { TexturePaintCache::setRecordLocksForTesting(false); }
    } reset;
    for (bool recordLocks : { false, true }) {
        SCOPED_TRACE(recordLocks ? "record locks" : "flock");
        QTemporaryDir dir;
        ASSERT_TRUE(dir.isValid());
        const std::string root = dir.path().toStdString();
        const std::string id = "held";
        TexturePaintCache::setRecordLocksForTesting(recordLocks);

        std::shared_ptr<TexturePaintCache::TileDirectoryClaim> first = TexturePaintCache::claimTileDirectory(root, id, true);
        ASSERT_TRUE(bool(first));
        ASSERT_TRUE(first->isShared());
        EXPECT_EQ(0, childGetsSharedTileDirectory(root, id)) << "another process cannot get a held directory";

        std::shared_ptr<TexturePaintCache::TileDirectoryClaim> second = TexturePaintCache::claimTileDirectory(root, id, true);
        ASSERT_TRUE(bool(second));
        EXPECT_FALSE(second->isShared()) << "two claims of one process exclude each other";
        second.reset();
        // A record lock goes with any descriptor of its file that its process closes.
        EXPECT_EQ(0, childGetsSharedTileDirectory(root, id)) << "releasing one claim leaves the other's lock held";
        std::shared_ptr<TexturePaintCache::TileDirectoryClaim> third = TexturePaintCache::claimTileDirectory(root, id, true);
        ASSERT_TRUE(bool(third));
        EXPECT_FALSE(third->isShared()) << "releasing one claim leaves the other held in the process";
        third.reset();

        first.reset();
        EXPECT_EQ(1, childGetsSharedTileDirectory(root, id)) << "a released directory goes to the next process";
        std::shared_ptr<TexturePaintCache::TileDirectoryClaim> again = TexturePaintCache::claimTileDirectory(root, id, true);
        ASSERT_TRUE(bool(again));
        EXPECT_TRUE(again->isShared());
        again.reset();
    }
}

TEST_F(TexturePaintTest, ASessionWhoseStrokesRenderNothingTouchesNoTexelOfTheView)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RotoLayerPtr session;
    NodePtr paint = createPaintedQuads(dir, &session);
    ASSERT_TRUE(bool(paint));
    useSmallTiles(paint);
    TexturePaint* effect = getTexturePaint(paint);
    NodePtr viewNode = effect->getSessionView(session);
    ASSERT_TRUE(bool(viewNode));

    // A stroke far outside the view. Its merge is wired over the view, which has an image wherever
    // the view does, so no wired session renders nothing: cutting the merge from the view leaves
    // the stroke alone, which is the only way to reach the path for strokes that render nothing.
    RotoStrokeItemPtr stroke = addBrushStroke(paint, session, eRotoStrokeTypeSolid, "away", 20., 20.);
    NodePtr merge = stroke->getMergeNode();
    ASSERT_TRUE(bool(merge));
    merge->disconnectInput(0);
    ASSERT_FALSE(bool(merge->getInput(0)));
    const unsigned int level = TexturePaintView::bufferMipmapLevel(effect->getBufferResolution());
    const RectI pixels = TexturePaintView::paintSpaceBounds(viewCamera().aspect).toPixelEnclosing(level, 1.);
    ImagePtr strokes;
    RectI rendered;
    ASSERT_TRUE(renderNodeImageIfAny(merge, 1., ViewIdx(0), level, pixels, ImageLayerDesc::getRGBAComponents(), &strokes, &rendered));
    ASSERT_FALSE(bool(strokes)) << "the strokes render nothing";

    TexturePaint::SessionBuffer buffer;
    std::string error;
    ASSERT_TRUE(effect->renderSessionBufferFrom(viewNode, session, ImageLayerDesc::getRGBAComponents(), 1., ViewIdx(0), &buffer, &error)) << error;
    ASSERT_EQ((std::size_t)buffer.width * (std::size_t)buffer.height, buffer.touched.size());
    for (std::uint8_t touched : buffer.touched) {
        ASSERT_EQ(0, touched);
    }
    expectColor(pixelAt(buffer, kRedU, 0.5), 1.f, 0.f, 0.f, 1.f, "the view's red quad");
    expectColor(pixelAt(buffer, kBlueU, 0.5), 0.f, 0.f, 1.f, 1.f, "the view's blue quad");
}

TEST_F(TexturePaintTest, UndoingABakeWhileALaterBakeIsQueuedIsRefused)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RotoLayerPtr session;
    NodePtr paint = createPaintedQuads(dir, &session);
    ASSERT_TRUE(bool(paint));
    useSmallTiles(paint);
    const std::string red = setBakeOutputPath(paint, dir);
    const float grey[3] = { 0.5f, 0.5f, 0.5f };
    writeSplitTexture(red, grey, grey);
    TexturePaint* effect = getTexturePaint(paint);
    setColor(addBrushStroke(paint, session, eRotoStrokeTypeSolid, "solid", kRedU, 0.5), 0., 1., 0.);

    std::vector<std::string> files;
    UndoCommandPtr first = bakeOutput(paint, &files);
    ASSERT_TRUE(bool(first));
    first->redo();

    RotoLayerPtr later = effect->addSession(viewCamera());
    ASSERT_TRUE(bool(later));
    setColor(addBrushStroke(paint, later, eRotoStrokeTypeSolid, "later", kRedU, 0.5), 0., 0., 1.);

    // The baker runs one bake at a time, in order: held by the marker, the later bake is still
    // queued when the undo is asked for, and the undo's swap runs after it.
    std::atomic<bool> release(false);
    TexturePaintBaker::Bake marker;
    marker.work = [&](TexturePaintBaker::Context&) {
        const Clock::time_point deadline = Clock::now() + std::chrono::seconds(60);
        while (!release.load() && (Clock::now() < deadline)) {
            QThread::msleep(2);
        }

        return true;
    };
    ASSERT_TRUE(effect->getBaker()->enqueue(marker));
    ASSERT_TRUE(effect->enqueueBakeOutput(1., ViewIdx(0)));
    first->undo();
    EXPECT_FALSE(paint->hasPersistentMessage()) << "the undo waits for the bake instead of being refused at once";
    release = true;
    ASSERT_TRUE(pumpUntil([&]() { return !effect->isBaking(); }, 60000));

    expectColor(bakedTexelAt(red, kSmallTile, 0.5, 0.5), 0.f, 0.f, 1.f, 1.f, "the later bake, which the refused undo left");
    EXPECT_TRUE(TexturePaint::isSessionFrozen(session)) << "the refused undo changes nothing";
    EXPECT_TRUE(TexturePaint::isSessionFrozen(later));
    EXPECT_TRUE(paint->hasPersistentMessage());
    const std::string record = getKnob<KnobString>(paint, kTexturePaintParamBakedOutput)->getValue();
    EXPECT_FALSE(record.empty());

    // The undo never happened, so redoing the bake has nothing to do.
    undoBakeOutput(paint, first, false);
    expectColor(bakedTexelAt(red, kSmallTile, 0.5, 0.5), 0.f, 0.f, 1.f, 1.f, "the later bake, which the redo left");
    EXPECT_EQ(record, getKnob<KnobString>(paint, kTexturePaintParamBakedOutput)->getValue());
}

namespace {
// Holds paint's baker on a bake of its own until release is set, so that what is enqueued after
// it is still queued.
void
holdBaker(TexturePaint* effect,
          std::atomic<bool>* release)
{
    TexturePaintBaker::Bake marker;
    marker.work = [release](TexturePaintBaker::Context&) {
        const Clock::time_point deadline = Clock::now() + std::chrono::seconds(60);
        while (!release->load() && (Clock::now() < deadline)) {
            QThread::msleep(2);
        }

        return true;
    };
    ASSERT_TRUE(effect->getBaker()->enqueue(marker));
}

// Waits until every bake paint's baker ran before now has finished and its result, landed or
// dropped, has reached the main thread.
bool
drainBaker(TexturePaint* effect)
{
    const std::shared_ptr<std::atomic<bool>> done = std::make_shared<std::atomic<bool>>(false);
    TexturePaintBaker::Bake marker;
    marker.work = [](TexturePaintBaker::Context&) {
        return true;
    };
    marker.completion = [done](bool) {
        *done = true;
    };
    if (!effect->getBaker()->enqueue(marker)) {
        return false;
    }

    return pumpUntil([&]() { return done->load(); }, 60000);
}

/**
 * Lets the file queue's steps through one at a time, as allowed, holding the rest before they
 * run. Shared with the hook, which may still be returning when the gate goes.
 **/
class StepGate {
public:
    StepGate()
        : _state(std::make_shared<State>())
    {
        const std::shared_ptr<State> state = _state;
        TexturePaintFileQueue::setBeforeWorkHookForTesting([state]() {
            std::unique_lock<std::mutex> lock(state->mutex);
            ++state->arrived;
            state->changed.notify_all();
            state->changed.wait(lock, [&state]() { return state->open || (state->passed < state->allowed); });
            ++state->passed;
        });
    }

    ~StepGate()
    {
        open();
        TexturePaintFileQueue::setBeforeWorkHookForTesting(std::function<void()>());
    }

    // Whether count steps have reached the gate, through it or held at it, within timeoutMs.
    bool waitUntilArrived(int count,
                          int timeoutMs)
    {
        std::unique_lock<std::mutex> lock(_state->mutex);

        return _state->changed.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this, count]() { return _state->arrived >= count; });
    }

    void allowOne()
    {
        std::lock_guard<std::mutex> lock(_state->mutex);
        ++_state->allowed;
        _state->changed.notify_all();
    }

    void open()
    {
        std::lock_guard<std::mutex> lock(_state->mutex);
        _state->open = true;
        _state->changed.notify_all();
    }

private:
    struct State {
        std::mutex mutex;
        std::condition_variable changed;
        int arrived = 0;
        int allowed = 0;
        int passed = 0;
        bool open = false;
    };
    std::shared_ptr<State> _state;
};

std::string
persistentMessageOf(const NodePtr& node)
{
    QString message;
    int type = 0;
    node->getPersistentMessage(&message, &type, false);

    return message.toStdString();
}

/**
 * Enqueues a Bake Output of effect and pumps the main thread only until the bake has taken its
 * snapshot there and reached its first file step, so that its completion stays queued for as long
 * as the caller does not pump.
 **/
void
startBakeOutputWithoutLanding(TexturePaint* effect)
{
    StepGate gate;
    ASSERT_TRUE(effect->enqueueBakeOutput(1., ViewIdx(0)));
    ASSERT_TRUE(pumpUntil([&]() { return gate.waitUntilArrived(1, 0); }, 60000)) << "the bake writes its files";
}
} // anon namespace

TEST_F(TexturePaintTest, AnUndoDroppedBeforeItsFileStepRunsLeavesTheFilesAndTheRecord)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RotoLayerPtr session;
    NodePtr paint = createPaintedQuads(dir, &session);
    ASSERT_TRUE(bool(paint));
    useSmallTiles(paint);
    const std::string red = setBakeOutputPath(paint, dir);
    const float grey[3] = { 0.5f, 0.5f, 0.5f };
    writeSplitTexture(red, grey, grey);
    TexturePaint* effect = getTexturePaint(paint);
    setColor(addBrushStroke(paint, session, eRotoStrokeTypeSolid, "solid", kRedU, 0.5), 0., 1., 0.);

    std::vector<std::string> files;
    UndoCommandPtr command = bakeOutput(paint, &files);
    ASSERT_TRUE(bool(command));
    command->redo();
    const std::string record = getKnob<KnobString>(paint, kTexturePaintParamBakedOutput)->getValue();

    {
        RollbackGate gate;
        command->undo();
        ASSERT_TRUE(pumpUntil([&]() { return gate.waitUntilHeld(0); }, 30000)) << "the undo's file step is next";
        effect->quitBackgroundWork(true);
        ASSERT_TRUE(drainBaker(effect));
        gate.release();
        ASSERT_TRUE(pumpUntil([]() { return TexturePaintFileQueue::isIdleForTesting(); }, 30000));
    }
    expectColor(bakedTexelAt(red, kSmallTile, 0.5, 0.5), 0.f, 1.f, 0.f, 1.f, "the bake, which the dropped undo left");
    EXPECT_EQ(record, getKnob<KnobString>(paint, kTexturePaintParamBakedOutput)->getValue());
    EXPECT_TRUE(TexturePaint::isSessionFrozen(session));

    // Still applied: a redo has nothing to do, and an undo puts back what the bake replaced.
    undoBakeOutput(paint, command, false);
    expectColor(bakedTexelAt(red, kSmallTile, 0.5, 0.5), 0.f, 1.f, 0.f, 1.f, "the bake, which the redo left");
    undoBakeOutput(paint, command, true);
    expectColor(bakedTexelAt(red, kSmallTile, 0.5, 0.5), 0.5f, 0.5f, 0.5f, 1.f, "the file before the bake");
    EXPECT_FALSE(TexturePaint::isSessionFrozen(session));
}

namespace {
/**
 * Drops paint's undo of command after its file step has run and before its result reaches the
 * main thread, then lets the step that puts the files back run. target must hold mid-grey before
 * the bake.
 **/
void
dropUndoAfterItsFileStep(const NodePtr& paint,
                         const UndoCommandPtr& command,
                         const std::string& target)
{
    TexturePaint* effect = getTexturePaint(paint);
    StepGate gate;
    command->undo();
    ASSERT_TRUE(pumpUntil([&]() { return gate.waitUntilArrived(1, 0); }, 30000)) << "the undo's file step is next";
    // Behind the undo's step: once it reaches the gate, that step has run.
    (void)TexturePaintFileQueue::submit([](bool) { });
    gate.allowOne();
    ASSERT_TRUE(gate.waitUntilArrived(2, 30000));
    const BufferPixel undone = bakedTexelAt(target, kSmallTile, 0.5, 0.5);
    const float grey[4] = { 0.5f, 0.5f, 0.5f, 1.f };
    for (int c = 0; c < 4; ++c) {
        EXPECT_NEAR(grey[c], undone.rgba[c], 1e-3) << "the undo's step put back the file before the bake, channel " << c;
    }
    effect->quitBackgroundWork(true);
    ASSERT_TRUE(drainBaker(effect));
    gate.open();
    ASSERT_TRUE(pumpUntil([]() { return TexturePaintFileQueue::isIdleForTesting(); }, 30000));
}
} // anon namespace

TEST_F(TexturePaintTest, AnUndoDroppedAfterItsFileStepRanPutsTheFilesBack)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RotoLayerPtr session;
    NodePtr paint = createPaintedQuads(dir, &session);
    ASSERT_TRUE(bool(paint));
    useSmallTiles(paint);
    const std::string red = setBakeOutputPath(paint, dir);
    const float grey[3] = { 0.5f, 0.5f, 0.5f };
    writeSplitTexture(red, grey, grey);
    setColor(addBrushStroke(paint, session, eRotoStrokeTypeSolid, "solid", kRedU, 0.5), 0., 1., 0.);

    std::vector<std::string> files;
    UndoCommandPtr command = bakeOutput(paint, &files);
    ASSERT_TRUE(bool(command));
    command->redo();
    const std::string record = getKnob<KnobString>(paint, kTexturePaintParamBakedOutput)->getValue();

    dropUndoAfterItsFileStep(paint, command, red);
    expectColor(bakedTexelAt(red, kSmallTile, 0.5, 0.5), 0.f, 1.f, 0.f, 1.f, "the bake, which the dropped undo put back");
    EXPECT_EQ(record, getKnob<KnobString>(paint, kTexturePaintParamBakedOutput)->getValue());
    EXPECT_TRUE(TexturePaint::isSessionFrozen(session));

    undoBakeOutput(paint, command, true);
    expectColor(bakedTexelAt(red, kSmallTile, 0.5, 0.5), 0.5f, 0.5f, 0.5f, 1.f, "the file before the bake, which a later undo put back");
    EXPECT_FALSE(TexturePaint::isSessionFrozen(session));
}

TEST_F(TexturePaintTest, AnUndoDroppedAfterItsFileStepRanThatCannotPutTheFilesBackLeavesTheBakeUndone)
{
    struct FailReverses {
        FailReverses()
        {
            TexturePaint::setFailReverseSwapsForTesting(true);
        }

        ~FailReverses()
        {
            TexturePaint::setFailReverseSwapsForTesting(false);
        }
    };
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RotoLayerPtr session;
    NodePtr paint = createPaintedQuads(dir, &session);
    ASSERT_TRUE(bool(paint));
    useSmallTiles(paint);
    const std::string red = setBakeOutputPath(paint, dir);
    const float grey[3] = { 0.5f, 0.5f, 0.5f };
    writeSplitTexture(red, grey, grey);
    setColor(addBrushStroke(paint, session, eRotoStrokeTypeSolid, "solid", kRedU, 0.5), 0., 1., 0.);
    const std::string before = getKnob<KnobString>(paint, kTexturePaintParamBakedOutput)->getValue();

    std::vector<std::string> files;
    UndoCommandPtr command = bakeOutput(paint, &files);
    ASSERT_TRUE(bool(command));
    command->redo();
    ASSERT_TRUE(TexturePaint::isSessionFrozen(session));

    {
        FailReverses failing;
        dropUndoAfterItsFileStep(paint, command, red);
        EXPECT_TRUE(pumpUntil([&]() { return getKnob<KnobString>(paint, kTexturePaintParamBakedOutput)->getValue() == before; }, 30000)) << "the record follows the files";
    }
    expectColor(bakedTexelAt(red, kSmallTile, 0.5, 0.5), 0.5f, 0.5f, 0.5f, 1.f, "the file before the bake, which the undo left");
    EXPECT_FALSE(TexturePaint::isSessionFrozen(session));
    const std::string message = persistentMessageOf(paint);
    EXPECT_NE(std::string::npos, message.find("could not be moved back")) << message;
    EXPECT_NE(std::string::npos, message.find("stays undone")) << message;

    // The node knows the bake is undone: an undo has nothing to do, and a redo brings it back.
    undoBakeOutput(paint, command, true);
    expectColor(bakedTexelAt(red, kSmallTile, 0.5, 0.5), 0.5f, 0.5f, 0.5f, 1.f, "the file before the bake, which the undo left");
    undoBakeOutput(paint, command, false);
    expectColor(bakedTexelAt(red, kSmallTile, 0.5, 0.5), 0.f, 1.f, 0.f, 1.f, "the bake, which the redo brought back");
    EXPECT_TRUE(TexturePaint::isSessionFrozen(session));
}

TEST_F(TexturePaintTest, AnUndoQueuedBehindAFailedBakeOutputLandsOnceItEnds)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RotoLayerPtr session;
    NodePtr paint = createPaintedQuads(dir, &session);
    ASSERT_TRUE(bool(paint));
    useSmallTiles(paint);
    const std::string red = setBakeOutputPath(paint, dir);
    const float grey[3] = { 0.5f, 0.5f, 0.5f };
    writeSplitTexture(red, grey, grey);
    TexturePaint* effect = getTexturePaint(paint);
    setColor(addBrushStroke(paint, session, eRotoStrokeTypeSolid, "solid", kRedU, 0.5), 0., 1., 0.);

    const std::string before = getKnob<KnobString>(paint, kTexturePaintParamBakedOutput)->getValue();
    std::vector<std::string> files;
    UndoCommandPtr first = bakeOutput(paint, &files);
    ASSERT_TRUE(bool(first));
    first->redo();

    // Under a regular file, where no tile can be written.
    const std::string blocker = dir.path().toStdString() + "/blocker";
    std::ofstream(blocker.c_str()) << "not a directory";
    getKnob<KnobOutputFile>(paint, kTexturePaintParamBakeOutputPath)->setValue(blocker + "/out.<UDIM>.exr");

    std::atomic<bool> release(false);
    holdBaker(effect, &release);
    ASSERT_TRUE(effect->enqueueBakeOutput(1., ViewIdx(0)));
    first->undo();
    release = true;
    ASSERT_TRUE(pumpUntil([&]() { return !effect->isBaking(); }, 60000));

    expectColor(bakedTexelAt(red, kSmallTile, 0.5, 0.5), 0.5f, 0.5f, 0.5f, 1.f, "the file before the bake, which the undo put back");
    EXPECT_EQ(before, getKnob<KnobString>(paint, kTexturePaintParamBakedOutput)->getValue()) << "the record before the first bake";
    EXPECT_FALSE(TexturePaint::isSessionFrozen(session));
    EXPECT_TRUE(bakeTemps(dir).empty());
}

TEST_F(TexturePaintTest, EveryPathToAClaimedFileFindsItsClaim)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string base = dir.path().toStdString();
    std::filesystem::create_directories(base + "/a");
    std::filesystem::create_directory_symlink(base + "/a", base + "/link");
    const std::uint64_t first = TexturePaintFileQueue::newId();
    const std::uint64_t second = TexturePaintFileQueue::newId();
    TexturePaintFileQueue::setOwnerName(first, "Group1.PaintA");
    TexturePaintFileQueue::setOwnerName(second, "Group1.PaintB");
    const std::uint64_t firstBake = TexturePaintFileQueue::newId();
    const std::uint64_t secondBake = TexturePaintFileQueue::newId();

    std::uint64_t holder = 0;
    ASSERT_EQ(TexturePaintFileQueue::eClaimResultTaken, TexturePaintFileQueue::claim(firstBake, first, TexturePaintFileQueue::claimKey(base + "/a/x.exr"), &holder));
    for (const std::string& alias : { base + "/a/../a/x.exr", base + "/link/x.exr", base + "//a/./x.exr", base + "/missing/../a/x.exr" }) {
        EXPECT_EQ(TexturePaintFileQueue::claimKey(base + "/a/x.exr"), TexturePaintFileQueue::claimKey(alias)) << alias;
        holder = 0;
        EXPECT_EQ(TexturePaintFileQueue::eClaimResultHeldByAnotherOwner, TexturePaintFileQueue::claim(secondBake, second, TexturePaintFileQueue::claimKey(alias), &holder)) << alias;
        EXPECT_EQ(first, holder) << alias;
        EXPECT_EQ("Group1.PaintA", TexturePaintFileQueue::getOwnerName(holder)) << alias;
    }
    EXPECT_NE(TexturePaintFileQueue::claimKey(base + "/a/y.exr"), TexturePaintFileQueue::claimKey(base + "/a/x.exr"));

    TexturePaintFileQueue::forgetOwner(first);
    EXPECT_EQ("Group1.PaintA", TexturePaintFileQueue::getOwnerName(first)) << "a refusal still names a node whose bake holds the file";
    TexturePaintFileQueue::settle(firstBake);
    EXPECT_TRUE(TexturePaintFileQueue::getOwnerName(first).empty());
    TexturePaintFileQueue::settle(secondBake);
    TexturePaintFileQueue::forgetOwner(second);
    EXPECT_TRUE(TexturePaintFileQueue::getOwnerName(second).empty());
    EXPECT_EQ(TexturePaintFileQueue::eClaimResultTaken, TexturePaintFileQueue::claim(secondBake, second, TexturePaintFileQueue::claimKey(base + "/link/x.exr"), &holder));
    TexturePaintFileQueue::settle(secondBake);
}

TEST_F(TexturePaintTest, TwoQueuedBakeOutputsOfANodeToTheSameFilesLandInOrder)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RotoLayerPtr session;
    NodePtr paint = createPaintedQuads(dir, &session);
    ASSERT_TRUE(bool(paint));
    useSmallTiles(paint);
    const std::string red = setBakeOutputPath(paint, dir);
    const float grey[3] = { 0.5f, 0.5f, 0.5f };
    writeSplitTexture(red, grey, grey);
    TexturePaint* effect = getTexturePaint(paint);
    setColor(addBrushStroke(paint, session, eRotoStrokeTypeSolid, "solid", kRedU, 0.5), 0., 1., 0.);

    std::vector<UndoCommandPtr> commands;
    effect->setUndoCommandSinkForTesting([&commands](const UndoCommandPtr& command) {
        commands.push_back(command);
    });
    RotoLayerPtr later;
    {
        // A bake renders the strokes there are when it runs: held at its first file write, the
        // first bake has rendered before the later session is painted and the second bake queued.
        StepGate gate;
        ASSERT_TRUE(effect->enqueueBakeOutput(1., ViewIdx(0)));
        ASSERT_TRUE(pumpUntil([&]() { return gate.waitUntilArrived(1, 0); }, 60000)) << "the first bake writes its files";
        later = effect->addSession(viewCamera());
        ASSERT_TRUE(bool(later));
        setColor(addBrushStroke(paint, later, eRotoStrokeTypeSolid, "later", kRedU, 0.5), 0., 0., 1.);
        ASSERT_TRUE(effect->enqueueBakeOutput(1., ViewIdx(0)));
        EXPECT_TRUE(effect->isBaking()) << "the second bake is queued behind the first";
        gate.open();
        ASSERT_TRUE(pumpUntil([&]() { return !effect->isBaking(); }, 60000));
    }
    effect->setUndoCommandSinkForTesting(std::function<void(const UndoCommandPtr&)>());

    EXPECT_FALSE(paint->hasPersistentMessage()) << persistentMessageOf(paint);
    ASSERT_EQ(2u, commands.size());
    expectColor(bakedTexelAt(red, kSmallTile, 0.5, 0.5), 0.f, 0.f, 1.f, 1.f, "the second bake");
    EXPECT_TRUE(bakeTemps(dir).empty());

    undoBakeOutput(paint, commands[1], true);
    expectColor(bakedTexelAt(red, kSmallTile, 0.5, 0.5), 0.f, 1.f, 0.f, 1.f, "the first bake, which the second backed up");
    EXPECT_FALSE(TexturePaint::isSessionFrozen(later));
    undoBakeOutput(paint, commands[0], true);
    expectColor(bakedTexelAt(red, kSmallTile, 0.5, 0.5), 0.5f, 0.5f, 0.5f, 1.f, "the file before both bakes");
    EXPECT_FALSE(TexturePaint::isSessionFrozen(session));
}

TEST_F(TexturePaintTest, ASessionPaintedAfterABakeOutputStartedIsNotBakedAndStaysLive)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RotoLayerPtr session;
    NodePtr paint = createPaintedQuads(dir, &session);
    ASSERT_TRUE(bool(paint));
    useSmallTiles(paint);
    const std::string red = setBakeOutputPath(paint, dir);
    const std::string blue = (dir.path() + QString::fromUtf8("/out.1002.exr")).toStdString();
    TexturePaint* effect = getTexturePaint(paint);
    setColor(addBrushStroke(paint, session, eRotoStrokeTypeSolid, "solid", kRedU, 0.5), 0., 1., 0.);

    std::vector<UndoCommandPtr> commands;
    effect->setUndoCommandSinkForTesting([&commands](const UndoCommandPtr& command) {
        commands.push_back(command);
    });
    RotoLayerPtr later;
    {
        // Held at the write of tile 1001, the bake has not rendered tile 1002 yet, which the
        // later session paints.
        StepGate gate;
        ASSERT_TRUE(effect->enqueueBakeOutput(1., ViewIdx(0)));
        ASSERT_TRUE(pumpUntil([&]() { return gate.waitUntilArrived(1, 0); }, 60000)) << "the bake writes its files";
        later = effect->addSession(viewCamera());
        ASSERT_TRUE(bool(later));
        setColor(addBrushStroke(paint, later, eRotoStrokeTypeSolid, "later", kBlueU, 0.5), 1., 1., 0.);
        gate.open();
        ASSERT_TRUE(pumpUntil([&]() { return !effect->isBaking(); }, 60000));
    }

    EXPECT_FALSE(paint->hasPersistentMessage()) << persistentMessageOf(paint);
    ASSERT_EQ(1u, commands.size());
    expectColor(bakedTexelAt(red, kSmallTile, 0.5, 0.5), 0.f, 1.f, 0.f, 1.f, "the session the bake began with");
    EXPECT_LT(bakedTexelAt(blue, kSmallTile, 0.5, 0.5).rgba[0], 0.9f) << "the later session is not in the files";
    EXPECT_TRUE(TexturePaint::isSessionFrozen(session));
    EXPECT_FALSE(TexturePaint::isSessionFrozen(later)) << "the later session stays live";

    // The next bake takes it.
    ASSERT_TRUE(effect->enqueueBakeOutput(1., ViewIdx(0)));
    ASSERT_TRUE(pumpUntil([&]() { return !effect->isBaking(); }, 60000));
    effect->setUndoCommandSinkForTesting(std::function<void(const UndoCommandPtr&)>());
    EXPECT_FALSE(paint->hasPersistentMessage()) << persistentMessageOf(paint);
    expectColor(bakedTexelAt(blue, kSmallTile, 0.5, 0.5), 1.f, 1.f, 0.f, 1.f, "the later session, baked next");
    EXPECT_TRUE(TexturePaint::isSessionFrozen(later));
}

TEST_F(TexturePaintTest, ASessionPaintedWhileABakeOutputIsQueuedIsBakedAndFrozen)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RotoLayerPtr session;
    NodePtr paint = createPaintedQuads(dir, &session);
    ASSERT_TRUE(bool(paint));
    useSmallTiles(paint);
    const std::string red = setBakeOutputPath(paint, dir);
    const std::string blue = (dir.path() + QString::fromUtf8("/out.1002.exr")).toStdString();
    TexturePaint* effect = getTexturePaint(paint);
    setColor(addBrushStroke(paint, session, eRotoStrokeTypeSolid, "solid", kRedU, 0.5), 0., 1., 0.);

    std::atomic<bool> release(false);
    holdBaker(effect, &release);
    ASSERT_TRUE(effect->enqueueBakeOutput(1., ViewIdx(0)));
    RotoLayerPtr later = effect->addSession(viewCamera());
    ASSERT_TRUE(bool(later));
    setColor(addBrushStroke(paint, later, eRotoStrokeTypeSolid, "later", kBlueU, 0.5), 1., 1., 0.);
    release = true;
    ASSERT_TRUE(pumpUntil([&]() { return !effect->isBaking(); }, 60000));

    EXPECT_FALSE(paint->hasPersistentMessage()) << persistentMessageOf(paint);
    expectColor(bakedTexelAt(red, kSmallTile, 0.5, 0.5), 0.f, 1.f, 0.f, 1.f, "the session painted before the bake was queued");
    expectColor(bakedTexelAt(blue, kSmallTile, 0.5, 0.5), 1.f, 1.f, 0.f, 1.f, "the session painted while the bake was queued");
    EXPECT_TRUE(TexturePaint::isSessionFrozen(session));
    EXPECT_TRUE(TexturePaint::isSessionFrozen(later)) << "a bake freezes every session it rendered";
}

TEST_F(TexturePaintTest, ABakeOutputEnqueuedAsTheNodeStopsItsBakerRunsOnceItHasStopped)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RotoLayerPtr session;
    NodePtr paint = createPaintedQuads(dir, &session);
    ASSERT_TRUE(bool(paint));
    useSmallTiles(paint);
    const std::string red = setBakeOutputPath(paint, dir);
    TexturePaint* effect = getTexturePaint(paint);
    setColor(addBrushStroke(paint, session, eRotoStrokeTypeSolid, "solid", kRedU, 0.5), 0., 1., 0.);

    // Still running when asked to stop, the held bake keeps the baker's thread from exiting at once.
    std::atomic<bool> release(false);
    holdBaker(effect, &release);
    effect->quitBackgroundWork(true);
    ASSERT_TRUE(effect->enqueueBakeOutput(1., ViewIdx(0))) << "queued behind the restart rather than refused";
    EXPECT_FALSE(paint->hasPersistentMessage()) << persistentMessageOf(paint);
    release = true;
    ASSERT_TRUE(pumpUntil([&]() { return !effect->isBaking(); }, 60000));

    EXPECT_FALSE(paint->hasPersistentMessage()) << persistentMessageOf(paint);
    expectColor(bakedTexelAt(red, kSmallTile, 0.5, 0.5), 0.f, 1.f, 0.f, 1.f, "the bake enqueued after the stop");
    EXPECT_TRUE(TexturePaint::isSessionFrozen(session));
}

TEST_F(TexturePaintTest, ABakeOutputOfFilesAnotherNodeIsBakingFailsNamingIt)
{
    QTemporaryDir dir;
    QTemporaryDir otherDir;
    ASSERT_TRUE(dir.isValid() && otherDir.isValid());
    RotoLayerPtr session;
    RotoLayerPtr otherSession;
    NodePtr paint = createPaintedQuads(dir, &session);
    NodePtr other = createPaintedQuads(otherDir, &otherSession);
    ASSERT_TRUE(paint && other);
    useSmallTiles(paint);
    useSmallTiles(other);
    const std::string red = setBakeOutputPath(paint, dir);
    (void)setBakeOutputPath(other, dir);
    setColor(addBrushStroke(paint, session, eRotoStrokeTypeSolid, "solid", kRedU, 0.5), 0., 1., 0.);
    setColor(addBrushStroke(other, otherSession, eRotoStrokeTypeSolid, "other", kRedU, 0.5), 0., 0., 1.);
    TexturePaint* effect = getTexturePaint(paint);
    TexturePaint* otherEffect = getTexturePaint(other);

    {
        RollbackGate gate;
        ASSERT_TRUE(effect->enqueueBakeOutput(1., ViewIdx(0)));
        ASSERT_TRUE(pumpUntil([&]() { return gate.waitUntilHeld(0); }, 60000)) << "the first node's tile is about to be written";
        ASSERT_TRUE(otherEffect->enqueueBakeOutput(1., ViewIdx(0)));
        ASSERT_TRUE(pumpUntil([]() { return TexturePaintFileQueue::getQueuedCountForTesting() >= 1; }, 60000));
        gate.release();
        ASSERT_TRUE(pumpUntil([&]() { return !effect->isBaking() && !otherEffect->isBaking(); }, 60000));
    }

    EXPECT_FALSE(paint->hasPersistentMessage()) << persistentMessageOf(paint);
    ASSERT_TRUE(other->hasPersistentMessage());
    EXPECT_NE(std::string::npos, persistentMessageOf(other).find(paint->getFullyQualifiedName())) << persistentMessageOf(other);
    expectColor(bakedTexelAt(red, kSmallTile, 0.5, 0.5), 0.f, 1.f, 0.f, 1.f, "the first node's bake");
    EXPECT_TRUE(bakeTemps(dir).empty());
}

TEST_F(TexturePaintTest, UndoingABakeWhoseFilesAnotherNodeIsBakingIsRefusedNamingIt)
{
    QTemporaryDir dir;
    QTemporaryDir otherDir;
    ASSERT_TRUE(dir.isValid() && otherDir.isValid());
    RotoLayerPtr session;
    RotoLayerPtr otherSession;
    NodePtr paint = createPaintedQuads(dir, &session);
    NodePtr other = createPaintedQuads(otherDir, &otherSession);
    ASSERT_TRUE(paint && other);
    useSmallTiles(paint);
    useSmallTiles(other);
    const std::string red = setBakeOutputPath(paint, dir);
    (void)setBakeOutputPath(other, dir);
    setColor(addBrushStroke(paint, session, eRotoStrokeTypeSolid, "solid", kRedU, 0.5), 0., 1., 0.);
    setColor(addBrushStroke(other, otherSession, eRotoStrokeTypeSolid, "other", kRedU, 0.5), 0., 0., 1.);
    TexturePaint* effect = getTexturePaint(paint);
    TexturePaint* otherEffect = getTexturePaint(other);

    std::vector<std::string> files;
    UndoCommandPtr command = bakeOutput(paint, &files);
    ASSERT_TRUE(bool(command));
    command->redo();
    const std::string record = getKnob<KnobString>(paint, kTexturePaintParamBakedOutput)->getValue();

    {
        RollbackGate gate;
        ASSERT_TRUE(otherEffect->enqueueBakeOutput(1., ViewIdx(0)));
        ASSERT_TRUE(pumpUntil([&]() { return gate.waitUntilHeld(0); }, 60000)) << "the other node's tile is about to be written";
        command->undo();
        // The undo's file step queues behind the held one, while the other node holds the file.
        ASSERT_TRUE(pumpUntil([]() { return TexturePaintFileQueue::getQueuedCountForTesting() >= 1; }, 60000));
        gate.release();
        ASSERT_TRUE(pumpUntil([&]() { return !effect->isBaking() && !otherEffect->isBaking(); }, 60000));
    }

    ASSERT_TRUE(paint->hasPersistentMessage());
    EXPECT_NE(std::string::npos, persistentMessageOf(paint).find(other->getFullyQualifiedName())) << persistentMessageOf(paint);
    EXPECT_EQ(record, getKnob<KnobString>(paint, kTexturePaintParamBakedOutput)->getValue()) << "the refused undo changes nothing";
    EXPECT_TRUE(TexturePaint::isSessionFrozen(session));
    expectColor(bakedTexelAt(red, kSmallTile, 0.5, 0.5), 0.f, 0.f, 1.f, 1.f, "the other node's bake");
    EXPECT_TRUE(bakeTemps(dir).empty());
}
