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

#include "Global/Macros.h"

#include <cmath>
#include <exception>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/ViewerInstance.h"

NATRON_NAMESPACE_USING

namespace {

class ViewerKnobsTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
    }

    NodePtr createViewer()
    {
        return createNode(QString::fromUtf8(PLUGINID_NATRON_VIEWER));
    }
};

ViewerInstance*
viewerOf(const NodePtr& node)
{
    return node ? dynamic_cast<ViewerInstance*>(node->getEffectInstance().get()) : 0;
}

template <class K>
std::shared_ptr<K>
knobOf(const NodePtr& node,
       const char* name)
{
    return std::dynamic_pointer_cast<K>(node->getKnobByName(name));
}

} // namespace

TEST_F(ViewerKnobsTest, GettersReadTheKnobs)
{
    NodePtr node = createViewer();
    ASSERT_TRUE(bool(node));
    ViewerInstance* viewer = viewerOf(node);
    ASSERT_TRUE(viewer != 0);

    KnobBoolPtr enableGain = knobOf<KnobBool>(node, kViewerParamEnableGain);
    KnobDoublePtr gain = knobOf<KnobDouble>(node, kViewerParamGain);
    KnobBoolPtr enableGamma = knobOf<KnobBool>(node, kViewerParamEnableGamma);
    KnobDoublePtr gamma = knobOf<KnobDouble>(node, kViewerParamGamma);
    KnobChoicePtr channels = knobOf<KnobChoice>(node, kViewerParamDisplayChannels);
    KnobChoicePtr channelsB = knobOf<KnobChoice>(node, kViewerParamDisplayChannelsB);
    KnobBoolPtr enableDownscale = knobOf<KnobBool>(node, kViewerParamEnableDownscale);
    KnobChoicePtr downscaleLevel = knobOf<KnobChoice>(node, kViewerParamDownscaleLevel);
    KnobBoolPtr fullFrame = knobOf<KnobBool>(node, kViewerParamFullFrame);
    KnobBoolPtr autoContrast = knobOf<KnobBool>(node, kViewerParamAutoContrast);
    ASSERT_TRUE(bool(enableGain) && bool(gain) && bool(enableGamma) && bool(gamma));
    ASSERT_TRUE(bool(channels) && bool(channelsB) && bool(enableDownscale) && bool(downscaleLevel));
    ASSERT_TRUE(bool(fullFrame) && bool(autoContrast));

    EXPECT_DOUBLE_EQ(1., viewer->getGain());
    EXPECT_DOUBLE_EQ(1., viewer->getGamma());
    EXPECT_EQ(eDisplayChannelsRGB, viewer->getChannels(0));
    EXPECT_EQ(0u, viewer->getMipmapLevel());

    // Off ignores the stored value.
    gain->setValue(1.);
    EXPECT_DOUBLE_EQ(1., viewer->getGain());
    enableGain->setValue(true);
    EXPECT_DOUBLE_EQ(2., viewer->getGain());

    gamma->setValue(2.2);
    enableGamma->setValue(true);
    EXPECT_DOUBLE_EQ(2.2, viewer->getGamma());

    channels->setValue(5);
    EXPECT_EQ(eDisplayChannelsA, viewer->getChannels(0));
    EXPECT_EQ(eDisplayChannelsRGB, viewer->getChannels(1));
    channelsB->setValue(0);
    EXPECT_EQ(eDisplayChannelsY, viewer->getChannels(1));

    downscaleLevel->setValue(2);
    EXPECT_EQ(0u, viewer->getMipmapLevel());
    enableDownscale->setValue(true);
    EXPECT_EQ(3u, viewer->getMipmapLevel());

    fullFrame->setValue(true);
    EXPECT_TRUE(viewer->isFullFrameProcessingEnabled());
    autoContrast->setValue(true);
    EXPECT_TRUE(viewer->isAutoContrastEnabled());
}

TEST_F(ViewerKnobsTest, SettersWriteTheKnobs)
{
    NodePtr node = createViewer();
    ASSERT_TRUE(bool(node));
    ViewerInstance* viewer = viewerOf(node);
    ASSERT_TRUE(viewer != 0);

    KnobBoolPtr enableGain = knobOf<KnobBool>(node, kViewerParamEnableGain);
    KnobDoublePtr gain = knobOf<KnobDouble>(node, kViewerParamGain);
    KnobChoicePtr channels = knobOf<KnobChoice>(node, kViewerParamDisplayChannels);
    KnobChoicePtr channelsB = knobOf<KnobChoice>(node, kViewerParamDisplayChannelsB);
    KnobBoolPtr enableDownscale = knobOf<KnobBool>(node, kViewerParamEnableDownscale);
    KnobChoicePtr downscaleLevel = knobOf<KnobChoice>(node, kViewerParamDownscaleLevel);
    KnobStringPtr display = knobOf<KnobString>(node, kViewerParamDisplay);
    ASSERT_TRUE(bool(enableGain) && bool(gain) && bool(channels) && bool(channelsB));
    ASSERT_TRUE(bool(enableDownscale) && bool(downscaleLevel) && bool(display));

    // The toolbar hands a linear multiplier; the knob keeps f-stops.
    viewer->onGainChanged(4.);
    EXPECT_TRUE(enableGain->getValue());
    EXPECT_DOUBLE_EQ(2., gain->getValue());
    viewer->onGainChanged(1.);
    EXPECT_FALSE(enableGain->getValue());
    EXPECT_DOUBLE_EQ(2., gain->getValue());
    EXPECT_DOUBLE_EQ(1., viewer->getGain());

    viewer->setDisplayChannels(eDisplayChannelsY, true);
    EXPECT_EQ(0, channels->getValue());
    EXPECT_EQ(0, channelsB->getValue());

    viewer->onMipmapLevelChanged(2);
    EXPECT_TRUE(enableDownscale->getValue());
    EXPECT_EQ(1, downscaleLevel->getValue());
    viewer->onMipmapLevelChanged(0);
    EXPECT_FALSE(enableDownscale->getValue());
    EXPECT_EQ(1, downscaleLevel->getValue());

    viewer->setDisplayTransform("Some Display", "Some View", "");
    EXPECT_EQ(std::string("Some Display"), display->getValue());
}

// The viewer's hash is the tree version of every frame it caches, and its knobs only change how
// a frame is shown: no edit may move it, or the hash of anything upstream.
TEST_F(ViewerKnobsTest, KnobEditsLeaveHashesUnchanged)
{
    NodePtr node = createViewer();
    NodePtr dot = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    ASSERT_TRUE(bool(node) && bool(dot));
    connectNodes(dot, node, 0, true);

    const U64 viewerHash = node->getHashValue();
    const U64 dotHash = dot->getHashValue();

    knobOf<KnobBool>(node, kViewerParamEnableGain)->setValue(true);
    knobOf<KnobDouble>(node, kViewerParamGain)->setValue(1.5);
    knobOf<KnobBool>(node, kViewerParamEnableGamma)->setValue(true);
    knobOf<KnobDouble>(node, kViewerParamGamma)->setValue(0.5);
    knobOf<KnobBool>(node, kViewerParamAutoContrast)->setValue(true);
    knobOf<KnobBool>(node, kViewerParamCheckerboard)->setValue(true);
    knobOf<KnobString>(node, kViewerParamDisplay)->setValue("Some Display");
    knobOf<KnobString>(node, kViewerParamView)->setValue("Some View");
    knobOf<KnobString>(node, kViewerParamLook)->setValue("Some Look");
    knobOf<KnobChoice>(node, kViewerParamDisplayChannels)->setValue(2);
    knobOf<KnobChoice>(node, kViewerParamOperation)->setValue(1);
    knobOf<KnobDouble>(node, kViewerParamWipeAmount)->setValue(0.25);
    knobOf<KnobDouble>(node, kViewerParamWipeAngle)->setValue(0.5);
    knobOf<KnobBool>(node, kViewerParamFullFrame)->setValue(true);
    knobOf<KnobBool>(node, kViewerParamEnableDownscale)->setValue(true);
    knobOf<KnobBool>(node, kViewerParamEnableUserRoI)->setValue(true);
    knobOf<KnobBool>(node, kViewerParamClipToFormat)->setValue(false);
    knobOf<KnobDouble>(node, kViewerParamFps)->setValue(12.);
    knobOf<KnobInt>(node, kViewerParamMaxSamples)->setValue(64);
    knobOf<KnobBool>(node, kViewerParamRefinementPaused)->setValue(true);

    EXPECT_EQ(viewerHash, node->getHashValue());
    EXPECT_EQ(dotHash, dot->getHashValue());
}

TEST_F(ViewerKnobsTest, RenderRequestedOnlyByProcessingKnobs)
{
    NodePtr node = createViewer();
    ASSERT_TRUE(bool(node));
    ViewerInstance* viewer = viewerOf(node);
    ASSERT_TRUE(viewer != 0);

    const unsigned int start = viewer->getKnobRenderRequestCount();

    knobOf<KnobBool>(node, kViewerParamEnableFps)->setValue(true);
    knobOf<KnobDouble>(node, kViewerParamFps)->setValue(12.);
    knobOf<KnobInt>(node, kViewerParamInPoint)->setValue(5);
    knobOf<KnobInt>(node, kViewerParamOutPoint)->setValue(50);
    knobOf<KnobBool>(node, kViewerParamCheckerboard)->setValue(true);
    knobOf<KnobChoice>(node, kViewerParamOperation)->setValue(2);
    knobOf<KnobDouble>(node, kViewerParamWipeCenter)->setValue(10.);
    knobOf<KnobDouble>(node, kViewerParamWipeAmount)->setValue(0.5);
    knobOf<KnobDouble>(node, kViewerParamWipeAngle)->setValue(1.);
    knobOf<KnobInt>(node, kViewerParamActiveView)->setValue(1);
    // The scene view's progressive render follows these itself.
    knobOf<KnobInt>(node, kViewerParamMaxSamples)->setValue(64);
    knobOf<KnobBool>(node, kViewerParamRefinementPaused)->setValue(true);
    EXPECT_EQ(start, viewer->getKnobRenderRequestCount());

    knobOf<KnobBool>(node, kViewerParamFullFrame)->setValue(true);
    EXPECT_EQ(start + 1, viewer->getKnobRenderRequestCount());
    knobOf<KnobBool>(node, kViewerParamEnableDownscale)->setValue(true);
    EXPECT_EQ(start + 2, viewer->getKnobRenderRequestCount());
    knobOf<KnobChoice>(node, kViewerParamDownscaleLevel)->setValue(3);
    EXPECT_EQ(start + 3, viewer->getKnobRenderRequestCount());
    knobOf<KnobBool>(node, kViewerParamEnableUserRoI)->setValue(true);
    EXPECT_EQ(start + 4, viewer->getKnobRenderRequestCount());
    knobOf<KnobDouble>(node, kViewerParamUserRoIBottomLeft)->setValue(10.);
    EXPECT_EQ(start + 5, viewer->getKnobRenderRequestCount());
    knobOf<KnobDouble>(node, kViewerParamUserRoISize)->setValue(100.);
    EXPECT_EQ(start + 6, viewer->getKnobRenderRequestCount());
    knobOf<KnobBool>(node, kViewerParamClipToFormat)->setValue(false);
    EXPECT_EQ(start + 7, viewer->getKnobRenderRequestCount());
}

// The viewer has a settings panel like any node, but outputs no image, so of the knobs every node
// gets it only takes the label.
TEST_F(ViewerKnobsTest, HasASettingsPanelWithALabelAndNoImageKnobs)
{
    NodePtr node = createViewer();
    ASSERT_TRUE(bool(node));
    EXPECT_TRUE(node->getEffectInstance()->getMakeSettingsPanel());
    EXPECT_TRUE(bool(knobOf<KnobString>(node, kUserLabelKnobName)));
    EXPECT_FALSE(bool(node->getKnobByName(kDisableNodeKnobName)));
    EXPECT_FALSE(bool(node->getKnobByName(kEnableLifeTimeNodeKnobName)));
    EXPECT_TRUE(bool(knobOf<KnobBool>(node, kViewerParamEnableFps)));
    EXPECT_TRUE(bool(knobOf<KnobDouble>(node, kViewerParamFps)));
}

// The scene view's progressive render: 0 samples is the render delegate's own count, and a new
// viewer refines.
TEST_F(ViewerKnobsTest, RefinementKnobsDefaultToTheDelegatesCountUnpaused)
{
    NodePtr node = createViewer();
    ASSERT_TRUE(bool(node));
    KnobIntPtr maxSamples = knobOf<KnobInt>(node, kViewerParamMaxSamples);
    KnobBoolPtr paused = knobOf<KnobBool>(node, kViewerParamRefinementPaused);
    ASSERT_TRUE(bool(maxSamples) && bool(paused));

    EXPECT_EQ(std::string("Max Samples"), maxSamples->getLabel());
    EXPECT_EQ(std::string("Refinement Paused"), paused->getLabel());
    EXPECT_EQ(0, maxSamples->getValue());
    EXPECT_EQ(0, maxSamples->getMinimum());
    EXPECT_FALSE(paused->getValue());
    EXPECT_FALSE(maxSamples->isAnimationEnabled());
    EXPECT_FALSE(paused->isAnimationEnabled());
}

TEST_F(ViewerKnobsTest, ConnectingAnInputSetsTheInputKnobs)
{
    NodePtr node = createViewer();
    NodePtr dot = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    ASSERT_TRUE(bool(node) && bool(dot));
    KnobIntPtr aInput = knobOf<KnobInt>(node, kViewerParamAInput);
    ASSERT_TRUE(bool(aInput));
    EXPECT_EQ(-1, aInput->getValue());

    connectNodes(dot, node, 0, true);
    EXPECT_EQ(0, aInput->getValue());
}

// Every knob comes back from project.yaml, and a layer, alpha channel or display the project
// lacks keeps its saved ID.
TEST_F(ViewerKnobsTest, ValuesSurviveSaveAndLoad)
{
    ProjectPtr project = getApp()->getProject();
    NodePtr node = createViewer();
    ASSERT_TRUE(bool(node));
    const std::string name = node->getScriptName();

    KnobLayerSelectPtr layer = knobOf<KnobLayerSelect>(node, kViewerParamLayer);
    KnobChannelSelectPtr alpha = knobOf<KnobChannelSelect>(node, kViewerParamAlphaChannel);
    ASSERT_TRUE(bool(layer) && bool(alpha));

    knobOf<KnobBool>(node, kViewerParamEnableGain)->setValue(true);
    knobOf<KnobDouble>(node, kViewerParamGain)->setValue(1.5);
    knobOf<KnobBool>(node, kViewerParamEnableGamma)->setValue(true);
    knobOf<KnobDouble>(node, kViewerParamGamma)->setValue(0.5);
    knobOf<KnobBool>(node, kViewerParamAutoContrast)->setValue(true);
    knobOf<KnobBool>(node, kViewerParamCheckerboard)->setValue(true);
    knobOf<KnobString>(node, kViewerParamDisplay)->setValue("NoSuchDisplay");
    knobOf<KnobString>(node, kViewerParamView)->setValue("NoSuchView");
    knobOf<KnobString>(node, kViewerParamLook)->setValue("NoSuchLook");
    knobOf<KnobChoice>(node, kViewerParamDisplayChannels)->setValue(3);
    knobOf<KnobChoice>(node, kViewerParamDisplayChannelsB)->setValue(4);
    layer->setLayer("NoSuchLayer");
    alpha->set("NoSuchLayer.A");
    knobOf<KnobInt>(node, kViewerParamBInput)->setValue(2);
    knobOf<KnobChoice>(node, kViewerParamOperation)->setValue(6);
    knobOf<KnobDouble>(node, kViewerParamWipeCenter)->setValue(10., ViewSpec::all(), 0);
    knobOf<KnobDouble>(node, kViewerParamWipeCenter)->setValue(20., ViewSpec::all(), 1);
    knobOf<KnobDouble>(node, kViewerParamWipeAmount)->setValue(0.25);
    knobOf<KnobDouble>(node, kViewerParamWipeAngle)->setValue(0.5);
    knobOf<KnobInt>(node, kViewerParamActiveView)->setValue(1);
    knobOf<KnobBool>(node, kViewerParamEnableFps)->setValue(true);
    knobOf<KnobDouble>(node, kViewerParamFps)->setValue(12.);
    knobOf<KnobInt>(node, kViewerParamInPoint)->setValue(5);
    knobOf<KnobInt>(node, kViewerParamOutPoint)->setValue(50);
    knobOf<KnobBool>(node, kViewerParamEnableUserRoI)->setValue(true);
    knobOf<KnobDouble>(node, kViewerParamUserRoIBottomLeft)->setValue(30., ViewSpec::all(), 1);
    knobOf<KnobDouble>(node, kViewerParamUserRoISize)->setValue(40., ViewSpec::all(), 0);
    knobOf<KnobBool>(node, kViewerParamClipToFormat)->setValue(false);
    knobOf<KnobBool>(node, kViewerParamFullFrame)->setValue(true);
    knobOf<KnobBool>(node, kViewerParamEnableDownscale)->setValue(true);
    knobOf<KnobChoice>(node, kViewerParamDownscaleLevel)->setValue(4);
    knobOf<KnobInt>(node, kViewerParamMaxSamples)->setValue(64);
    knobOf<KnobBool>(node, kViewerParamRefinementPaused)->setValue(true);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("viewer-knobs.ntp");
    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));
    ASSERT_TRUE(QFile::exists(savedFilePath));

    project->reset(false, true);
    node.reset();
    layer.reset();
    alpha.reset();

    ASSERT_TRUE(project->loadProject(dirPath, fileName));
    NodePtr loaded = project->getNodeByName(name);
    ASSERT_TRUE(bool(loaded));
    ViewerInstance* viewer = viewerOf(loaded);
    ASSERT_TRUE(viewer != 0);

    EXPECT_TRUE(knobOf<KnobBool>(loaded, kViewerParamEnableGain)->getValue());
    EXPECT_DOUBLE_EQ(1.5, knobOf<KnobDouble>(loaded, kViewerParamGain)->getValue());
    EXPECT_DOUBLE_EQ(std::pow(2., 1.5), viewer->getGain());
    EXPECT_TRUE(knobOf<KnobBool>(loaded, kViewerParamEnableGamma)->getValue());
    EXPECT_DOUBLE_EQ(0.5, viewer->getGamma());
    EXPECT_TRUE(viewer->isAutoContrastEnabled());
    EXPECT_TRUE(knobOf<KnobBool>(loaded, kViewerParamCheckerboard)->getValue());
    EXPECT_EQ(std::string("NoSuchDisplay"), knobOf<KnobString>(loaded, kViewerParamDisplay)->getValue());
    EXPECT_EQ(std::string("NoSuchView"), knobOf<KnobString>(loaded, kViewerParamView)->getValue());
    EXPECT_EQ(std::string("NoSuchLook"), knobOf<KnobString>(loaded, kViewerParamLook)->getValue());
    EXPECT_EQ(eDisplayChannelsG, viewer->getChannels(0));
    EXPECT_EQ(eDisplayChannelsB, viewer->getChannels(1));

    KnobLayerSelectPtr loadedLayer = knobOf<KnobLayerSelect>(loaded, kViewerParamLayer);
    KnobChannelSelectPtr loadedAlpha = knobOf<KnobChannelSelect>(loaded, kViewerParamAlphaChannel);
    ASSERT_TRUE(bool(loadedLayer) && bool(loadedAlpha));
    EXPECT_EQ(std::string("NoSuchLayer"), loadedLayer->getLayer());
    EXPECT_EQ(std::string("NoSuchLayer.A"), loadedAlpha->get());
    // Rendering falls back to colour while the saved selection stays.
    EXPECT_TRUE(viewer->getActiveLayer().isColorLayer());

    EXPECT_EQ(2, knobOf<KnobInt>(loaded, kViewerParamBInput)->getValue());
    EXPECT_EQ(6, knobOf<KnobChoice>(loaded, kViewerParamOperation)->getValue());
    KnobDoublePtr wipeCenter = knobOf<KnobDouble>(loaded, kViewerParamWipeCenter);
    EXPECT_DOUBLE_EQ(10., wipeCenter->getValue(0));
    EXPECT_DOUBLE_EQ(20., wipeCenter->getValue(1));
    EXPECT_DOUBLE_EQ(0.25, knobOf<KnobDouble>(loaded, kViewerParamWipeAmount)->getValue());
    EXPECT_DOUBLE_EQ(0.5, knobOf<KnobDouble>(loaded, kViewerParamWipeAngle)->getValue());
    EXPECT_EQ(1, knobOf<KnobInt>(loaded, kViewerParamActiveView)->getValue());
    EXPECT_TRUE(knobOf<KnobBool>(loaded, kViewerParamEnableFps)->getValue());
    EXPECT_DOUBLE_EQ(12., knobOf<KnobDouble>(loaded, kViewerParamFps)->getValue());
    EXPECT_EQ(5, knobOf<KnobInt>(loaded, kViewerParamInPoint)->getValue());
    EXPECT_EQ(50, knobOf<KnobInt>(loaded, kViewerParamOutPoint)->getValue());
    EXPECT_TRUE(knobOf<KnobBool>(loaded, kViewerParamEnableUserRoI)->getValue());
    EXPECT_DOUBLE_EQ(30., knobOf<KnobDouble>(loaded, kViewerParamUserRoIBottomLeft)->getValue(1));
    EXPECT_DOUBLE_EQ(40., knobOf<KnobDouble>(loaded, kViewerParamUserRoISize)->getValue(0));
    EXPECT_FALSE(knobOf<KnobBool>(loaded, kViewerParamClipToFormat)->getValue());
    EXPECT_TRUE(viewer->isFullFrameProcessingEnabled());
    EXPECT_EQ(5u, viewer->getMipmapLevel());
    EXPECT_EQ(64, knobOf<KnobInt>(loaded, kViewerParamMaxSamples)->getValue());
    EXPECT_TRUE(knobOf<KnobBool>(loaded, kViewerParamRefinementPaused)->getValue());
}

// A script run without the GUI that opens a project and saves it again keeps every viewer, its
// knobs and its input.
TEST_F(ViewerKnobsTest, ViewersSurviveALoadAndResaveWithoutTheGui)
{
    ASSERT_TRUE(appPTR->isBackground());
    ProjectPtr project = getApp()->getProject();
    NodePtr dot = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    NodePtr first = createViewer();
    NodePtr second = createViewer();
    ASSERT_TRUE(bool(dot) && bool(first) && bool(second));
    const std::string dotName = dot->getScriptName();
    const std::string firstName = first->getScriptName();
    const std::string secondName = second->getScriptName();
    ASSERT_NE(firstName, secondName);
    connectNodes(dot, first, 0, true);

    knobOf<KnobBool>(first, kViewerParamEnableFps)->setValue(true);
    knobOf<KnobDouble>(first, kViewerParamFps)->setValue(12.);
    knobOf<KnobBool>(first, kViewerParamEnableGain)->setValue(true);
    knobOf<KnobDouble>(first, kViewerParamGain)->setValue(1.5);
    knobOf<KnobDouble>(second, kViewerParamFps)->setValue(48.);
    knobOf<KnobBool>(second, kViewerParamCheckerboard)->setValue(true);
    knobOf<KnobInt>(second, kViewerParamInPoint)->setValue(7);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("viewers-original.ntp");
    const QString resavedName = QString::fromUtf8("viewers-resaved.ntp");
    ASSERT_TRUE(project->saveProject(dirPath, fileName, 0));

    project->reset(false, true);
    dot.reset();
    first.reset();
    second.reset();

    ASSERT_TRUE(project->loadProject(dirPath, fileName));
    ASSERT_TRUE(bool(project->getNodeByName(firstName)));
    ASSERT_TRUE(bool(project->getNodeByName(secondName)));
    ASSERT_TRUE(project->saveProject(dirPath, resavedName, 0));

    project->reset(false, true);
    ASSERT_TRUE(project->loadProject(dirPath, resavedName));

    NodePtr loadedDot = project->getNodeByName(dotName);
    NodePtr loadedFirst = project->getNodeByName(firstName);
    NodePtr loadedSecond = project->getNodeByName(secondName);
    ASSERT_TRUE(bool(loadedDot));
    ASSERT_TRUE(bool(loadedFirst));
    ASSERT_TRUE(bool(loadedSecond));
    ASSERT_TRUE(viewerOf(loadedFirst) != 0);
    ASSERT_TRUE(viewerOf(loadedSecond) != 0);

    EXPECT_EQ(loadedDot, loadedFirst->getInput(0));
    EXPECT_EQ(0, knobOf<KnobInt>(loadedFirst, kViewerParamAInput)->getValue());
    EXPECT_FALSE(bool(loadedSecond->getInput(0)));

    EXPECT_TRUE(knobOf<KnobBool>(loadedFirst, kViewerParamEnableFps)->getValue());
    EXPECT_DOUBLE_EQ(12., knobOf<KnobDouble>(loadedFirst, kViewerParamFps)->getValue());
    EXPECT_TRUE(knobOf<KnobBool>(loadedFirst, kViewerParamEnableGain)->getValue());
    EXPECT_DOUBLE_EQ(1.5, knobOf<KnobDouble>(loadedFirst, kViewerParamGain)->getValue());
    EXPECT_FALSE(knobOf<KnobBool>(loadedFirst, kViewerParamCheckerboard)->getValue());

    EXPECT_DOUBLE_EQ(48., knobOf<KnobDouble>(loadedSecond, kViewerParamFps)->getValue());
    EXPECT_TRUE(knobOf<KnobBool>(loadedSecond, kViewerParamCheckerboard)->getValue());
    EXPECT_EQ(7, knobOf<KnobInt>(loadedSecond, kViewerParamInPoint)->getValue());
    EXPECT_FALSE(knobOf<KnobBool>(loadedSecond, kViewerParamEnableGain)->getValue());
}

TEST_F(ViewerKnobsTest, KnobEditsBatchedIntoOneRenderRequest)
{
    NodePtr node = createViewer();
    ASSERT_TRUE(bool(node));
    ViewerInstance* viewer = viewerOf(node);
    ASSERT_TRUE(viewer != 0);
    KnobDoublePtr bottomLeft = knobOf<KnobDouble>(node, kViewerParamUserRoIBottomLeft);
    KnobDoublePtr size = knobOf<KnobDouble>(node, kViewerParamUserRoISize);
    ASSERT_TRUE(bool(bottomLeft) && bool(size));

    const unsigned int start = viewer->getKnobRenderRequestCount();
    viewer->beginKnobEdits();
    bottomLeft->setValue(10., ViewSpec::all(), 0);
    bottomLeft->setValue(20., ViewSpec::all(), 1);
    viewer->beginKnobEdits();
    size->setValue(100., ViewSpec::all(), 0);
    size->setValue(200., ViewSpec::all(), 1);
    viewer->endKnobEdits();
    EXPECT_EQ(start, viewer->getKnobRenderRequestCount());
    viewer->endKnobEdits();
    EXPECT_EQ(start + 1, viewer->getKnobRenderRequestCount());

    viewer->beginKnobEdits();
    knobOf<KnobBool>(node, kViewerParamCheckerboard)->setValue(true);
    viewer->endKnobEdits();
    EXPECT_EQ(start + 1, viewer->getKnobRenderRequestCount());
}

TEST_F(ViewerKnobsTest, DesiredFpsStaysAboveZero)
{
    NodePtr node = createViewer();
    ASSERT_TRUE(bool(node));
    KnobDoublePtr fps = knobOf<KnobDouble>(node, kViewerParamFps);
    ASSERT_TRUE(bool(fps));
    EXPECT_GT(fps->getMinimum(), 0.);
    EXPECT_GE(fps->getValue(), fps->getMinimum());
}

// The inputs reconnect in index order during a load, and each connection moves the inspector's
// active inputs, so a saved input that reconnects late or not at all must still win.
TEST_F(ViewerKnobsTest, SavedInputsWinWhicheverOrderTheInputsReconnect)
{
    ProjectPtr project = getApp()->getProject();
    NodePtr node = createViewer();
    NodePtr dot0 = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    NodePtr dot1 = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    ASSERT_TRUE(bool(node) && bool(dot0) && bool(dot1));
    const std::string name = node->getScriptName();
    connectNodes(dot0, node, 0, true);
    connectNodes(dot1, node, 1, true);
    knobOf<KnobInt>(node, kViewerParamAInput)->setValue(1);
    knobOf<KnobInt>(node, kViewerParamBInput)->setValue(3);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("viewer-inputs.ntp");
    ASSERT_TRUE(project->saveProject(dirPath, fileName, 0));

    project->reset(false, true);
    node.reset();
    dot0.reset();
    dot1.reset();

    ASSERT_TRUE(project->loadProject(dirPath, fileName));
    NodePtr loaded = project->getNodeByName(name);
    ASSERT_TRUE(bool(loaded));
    ViewerInstance* viewer = viewerOf(loaded);
    ASSERT_TRUE(viewer != 0);
    ASSERT_TRUE(bool(loaded->getInput(0)) && bool(loaded->getInput(1)));

    int active[2] = { -1, -1 };
    viewer->getActiveInputs(active[0], active[1]);
    EXPECT_EQ(1, knobOf<KnobInt>(loaded, kViewerParamAInput)->getValue());
    EXPECT_EQ(3, knobOf<KnobInt>(loaded, kViewerParamBInput)->getValue());
    EXPECT_EQ(1, active[0]);
    EXPECT_EQ(3, active[1]);
}
