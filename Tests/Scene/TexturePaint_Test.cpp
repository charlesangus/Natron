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

#include <list>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/Curve.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/TexturePaint.h"
#include "Engine/Project.h"
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
} // anonymous namespace

class TexturePaintTest
    : public BaseTest {
protected:
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
