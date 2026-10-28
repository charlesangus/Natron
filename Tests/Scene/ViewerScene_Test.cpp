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

#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QStringList>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/LibraryBinary.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/OpenGLViewerI.h"
#include "Engine/Plugin.h"
#include "Engine/PluginActionShortcut.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/ScenePayload.h"
#include "Engine/Settings.h"
#include "Engine/TimeLine.h"
#include "Engine/UpdateViewerParams.h"
#include "Engine/ViewIdx.h"
#include "Engine/ViewerInstance.h"

NATRON_NAMESPACE_USING

namespace {

// AppManager registers the viewer only for a GUI session, and the tests run in the background. It
// is registered for internal use only, so that the sweeps over every plug-in that run later in the
// same process do not try to build it.
void
registerViewerPluginOnce()
{
    static std::once_flag once;

    std::call_once(once, []() {
        try {
            if (appPTR->getPluginBinary(QString::fromUtf8(PLUGINID_NATRON_VIEWER), -1, -1, false)) {
                return;
            }
        } catch (const std::exception&) {
        }
        EffectInstancePtr node(ViewerInstance::BuildEffect(NodePtr()));
        std::map<std::string, void (*)()> functions;
        functions.insert(std::make_pair("BuildEffect", (void (*)())&ViewerInstance::BuildEffect));
        LibraryBinary* binary = new LibraryBinary(functions);

        std::list<std::string> grouping;
        node->getPluginGrouping(&grouping);
        QStringList qgrouping;
        for (std::list<std::string>::iterator it = grouping.begin(); it != grouping.end(); ++it) {
            qgrouping.push_back(QString::fromUtf8(it->c_str()));
        }

        Plugin* p = appPTR->registerPlugin(QString(), qgrouping, QString::fromUtf8(node->getPluginID().c_str()), QString::fromUtf8(node->getPluginLabel().c_str()),
                                           QString(), QStringList(), node->isReader(), node->isWriter(), binary, node->renderThreadSafety() == eRenderSafetyUnsafe,
                                           node->getMajorVersion(), node->getMinorVersion(), false);
        p->setLabelWithoutSuffix(Plugin::makeLabelWithoutSuffix(p->getPluginLabel()));
        std::list<PluginActionShortcut> shortcuts;
        node->getPluginShortcuts(&shortcuts);
        p->setShorcuts(shortcuts);
        p->setOpenGLRenderSupport(node->supportsOpenGLRender());
        p->setForInternalUseOnly(true);
    });
}

// Just enough of a viewer widget for the render path to run: a scene never reaches the texture
// calls, so they do nothing.
class SceneFakeViewerUi
    : public OpenGLViewerI {
public:
    bool isViewerUIVisible() const OVERRIDE { return false; }
    double getZoomFactor() const OVERRIDE { return 1.; }
    void fitImageToFormat() OVERRIDE { }
    bool isClippingImageToFormat() const OVERRIDE { return true; }
    RectI getImageRectangleDisplayed(const RectI& pixelRod, const double /*par*/, unsigned int /*mipmapLevel*/) OVERRIDE { return pixelRod; }

    RectI getImageRectangleDisplayedRoundedToTileSize(int /*texIndex*/,
                                                      const RectD& /*rod*/,
                                                      const double /*par*/,
                                                      unsigned int /*mipmapLevel*/,
                                                      std::vector<RectI>* /*tiles*/,
                                                      std::vector<RectI>* /*tilesRounded*/,
                                                      int* /*tileSize*/,
                                                      RectI* /*roiNotRounded*/) OVERRIDE
    {
        return RectI();
    }

    RectI getExactImageRectangleDisplayed(int /*texIndex*/, const RectD& /*rod*/, const double /*par*/, unsigned int /*mipmapLevel*/) OVERRIDE { return RectI(); }
    ImageBitDepthEnum getBitDepth() const OVERRIDE { return appPTR->getCurrentSettings()->getViewersBitDepth(); }
    bool isUserRegionOfInterestEnabled() const OVERRIDE { return false; }
    RectD getUserRegionOfInterest() const OVERRIDE { return RectD(); }
    void clearPartialUpdateTextures() OVERRIDE { }

    void transferBufferFromRAMtoGPU(const unsigned char* /*ramBuffer*/,
                                    size_t /*bytesCount*/,
                                    const RectI& /*roiRoundedToTileSize*/,
                                    const RectI& /*roi*/,
                                    const TextureRect& /*tileRect*/,
                                    int /*textureIndex*/,
                                    bool /*isPartialRect*/,
                                    bool /*isFirstTile*/,
                                    TexturePtr* /*texture*/) OVERRIDE
    {
    }

    void endTransferBufferFromRAMToGPU(int /*textureIndex*/,
                                       const TexturePtr& /*texture*/,
                                       const ImagePtr& /*image*/,
                                       int /*time*/,
                                       const RectD& /*rod*/,
                                       double /*par*/,
                                       ImageBitDepthEnum /*depth*/,
                                       unsigned int /*mipmapLevel*/,
                                       double /*gain*/,
                                       double /*gamma*/,
                                       double /*offset*/,
                                       bool /*recenterViewer*/,
                                       const Point& /*viewportCenter*/,
                                       bool /*isPartialRect*/) OVERRIDE
    {
    }

    void setLastRenderedDeepImage(int /*textureIndex*/, unsigned int /*mipmapLevel*/, const DeepImagePtr& /*deepImage*/) OVERRIDE { }
    void disconnectInputTexture(int /*textureIndex*/, bool /*clearRoD*/) OVERRIDE { }
    void updateColorPicker(int /*textureIndex*/, int /*x*/, int /*y*/) OVERRIDE { }

    void getTextureColorAt(int /*x*/, int /*y*/, double* r, double* g, double* b, double* a) OVERRIDE
    {
        *r = *g = *b = *a = 0.;
    }

    void makeOpenGLcontextCurrent() OVERRIDE { }
    void removeGUI() OVERRIDE { }
    ViewIdx getCurrentView() const OVERRIDE { return ViewIdx(0); }
    int getCurrentlyDisplayedTime() const OVERRIDE { return (int)getTimeline()->currentFrame(); }

    void getViewerFrameRange(int* first, int* last) const OVERRIDE
    {
        *first = 1;
        *last = 1;
    }

    ViewerCompositingOperatorEnum getCompositingOperator() const OVERRIDE { return eViewerCompositingOperatorNone; }
    void setCompositingOperator(ViewerCompositingOperatorEnum /*op*/) OVERRIDE { }
    TimeLinePtr getTimeline() const OVERRIDE { return timeline; }
    void saveOpenGLContext() OVERRIDE { }
    void restoreOpenGLContext() OVERRIDE { }
    void clearLastRenderedImage() OVERRIDE { }
    void redrawNow() OVERRIDE { }
    void toWidgetCoordinates(double* /*x*/, double* /*y*/) const OVERRIDE { }
    void toCanonicalCoordinates(double* /*x*/, double* /*y*/) const OVERRIDE { }
    int getWidgetFontHeight() const OVERRIDE { return 10; }
    int getStringWidthForCurrentFont(const std::string& string) const OVERRIDE { return (int)string.size() * 6; }
    void swapOpenGLBuffers() OVERRIDE { }
    void redraw() OVERRIDE { }

    void getViewportSize(double& width, double& height) const OVERRIDE
    {
        width = 64.;
        height = 64.;
    }

    void getPixelScale(double& xScale, double& yScale) const OVERRIDE
    {
        xScale = 1.;
        yScale = 1.;
    }

    double getScreenPixelRatio() const OVERRIDE { return 1.; }

    void getBackgroundColour(double& r, double& g, double& b) const OVERRIDE
    {
        r = g = b = 0.;
    }

    RectD getViewportRect() const OVERRIDE { return RectD(0, 0, 64, 64); }

    void getCursorPosition(double& x, double& y) const OVERRIDE
    {
        x = 0.;
        y = 0.;
    }

    TimeLinePtr timeline;
};

} // namespace

class ViewerSceneTest
    : public BaseTest {
protected:
    ViewerSceneTest()
        : BaseTest()
        , _ui()
        , _viewer(0)
        , _viewerNode()
    {
    }

    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        registerViewerPluginOnce();
        CreateNodeArgs viewerArgs(PLUGINID_NATRON_VIEWER, getApp()->getProject());
        viewerArgs.setProperty<bool>(kCreateNodeArgsPropAllowNonUserCreatablePlugins, true);
        _viewerNode = getApp()->createNode(viewerArgs);
        ASSERT_TRUE(bool(_viewerNode));
        _viewer = _viewerNode->isEffectViewer();
        ASSERT_TRUE(_viewer != NULL);
        _ui.timeline = getApp()->getTimeLine();
        _viewer->setUiContext(&_ui);
    }

    virtual void TearDown() OVERRIDE
    {
        if (_viewer) {
            _viewer->invalidateUiContext();
        }
        _viewer = 0;
        _viewerNode.reset();
        getApp()->getProject()->reset(false, true);
        SceneQueryPool::instance().releaseIdle();
        BaseTest::TearDown();
    }

    NodePtr createUsdRead(const std::string& path)
    {
        NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
        if (reader) {
            KnobFile* file = dynamic_cast<KnobFile*>(reader->getKnobByName(kUSDReadParamFile).get());
            EXPECT_TRUE(file != NULL);
            if (file) {
                file->setValue(path);
            }
        }

        return reader;
    }

    // The A input through the 2D path as the viewer's current-frame render runs it: arguments on
    // this thread, the render on a thread of its own.
    ViewerInstance::ViewerRenderRetCode renderFrame()
    {
        const SequenceTime time = (SequenceTime)getApp()->getTimeLine()->currentFrame();
        const U64 viewerHash = _viewer->getHash();
        ViewerArgsPtr args[2];
        args[0] = std::make_shared<ViewerArgs>();
        const ViewerInstance::ViewerRenderRetCode prepared = _viewer->getRenderViewerArgsAndCheckCache_public(time, false, ViewIdx(0), 0, viewerHash, true, NodePtr(), RenderStatsPtr(), args[0].get());
        if (prepared != ViewerInstance::eViewerRenderRetCodeRender) {
            return prepared;
        }
        EXPECT_TRUE(args[0]->sceneUpstream);
        ViewerInstance::ViewerRenderRetCode ret = ViewerInstance::eViewerRenderRetCodeFail;
        ViewerInstance* viewer = _viewer;
        std::thread renderThread([&ret, &args, viewer, viewerHash]() {
            try {
                ret = viewer->renderViewer(ViewIdx(0), false, false, viewerHash, true, NodePtr(), true, args,
                                           ViewerCurrentFrameRequestSchedulerStartArgsPtr(), RenderStatsPtr());
            } catch (...) {
                ret = ViewerInstance::eViewerRenderRetCodeFail;
            }
        });
        renderThread.join();
        if (args[0] && args[0]->params) {
            EXPECT_TRUE(args[0]->params->tiles.empty());
        }

        return ret;
    }

    SceneFakeViewerUi _ui;
    ViewerInstance* _viewer;
    NodePtr _viewerNode;
};

TEST_F(ViewerSceneTest, ShowsASceneThroughTheSceneActionAndFlipsBackForAnImage)
{
    // The fixture is a centimetre scene: a project in the same units reads it without a warning.
    KnobDouble* metersPerUnit = dynamic_cast<KnobDouble*>(getApp()->getProject()->getKnobByName("metersPerUnit").get());
    ASSERT_TRUE(metersPerUnit != NULL);
    metersPerUnit->setValue(0.01);
    NodePtr reader = createUsdRead(NATRON_TESTS_FIXTURES_DIR "/scene-reference.usda");
    NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
    ASSERT_TRUE(reader && constant);
    ASSERT_EQ(eDataKindScene, reader->getEffectiveOutputDataKind());

    int kindChanges = 0;
    QObject::connect(_viewer, &ViewerInstance::activeInputDataKindChanged, [&kindChanges]() {
        ++kindChanges;
    });

    EXPECT_EQ(eDataKindImage, _viewer->getActiveInputDataKind());
    EXPECT_EQ(Node::eCanConnectInput_ok, _viewerNode->canConnectInput(reader, 0));
    connectNodes(reader, _viewerNode, 0, true);
    EXPECT_FALSE(_viewerNode->isInputDataKindUnacceptable(reader, 0));
    EXPECT_FALSE(_viewerNode->hasPersistentMessage());
    EXPECT_EQ(eDataKindScene, _viewer->getActiveInputDataKind());
    EXPECT_TRUE(_viewer->isWipeDisabledByDataKind());
    EXPECT_GE(kindChanges, 1);

    // The 2D path hands the scene to the scene action and renders no texture, without failing.
    EXPECT_NE(ViewerInstance::eViewerRenderRetCodeFail, renderFrame());
    ScenePayload scene;
    std::string error;
    EXPECT_TRUE(_viewer->getLastEvaluatedScene(0, &scene, &error)) << error;
    EXPECT_FALSE(scene.isEmpty());
    EXPECT_FALSE(reader->hasPersistentMessage());
    EXPECT_FALSE(_viewerNode->hasPersistentMessage());

    const int changesBeforeImage = kindChanges;
    _viewerNode->disconnectInput(0);
    connectNodes(constant, _viewerNode, 0, true);
    EXPECT_EQ(eDataKindImage, _viewer->getActiveInputDataKind());
    EXPECT_FALSE(_viewer->isWipeDisabledByDataKind());
    EXPECT_GT(kindChanges, changesBeforeImage);
    EXPECT_FALSE(_viewer->getLastEvaluatedScene(0, NULL, NULL));

    // Mixed kinds: A decides what is shown, and wipe is off.
    connectNodes(reader, _viewerNode, 1, true);
    _viewer->setInputA(0);
    _viewer->setInputB(1);
    EXPECT_EQ(eDataKindImage, _viewer->getActiveInputDataKind());
    EXPECT_EQ(eDataKindScene, _viewer->getInputDataKindForTexture(1));
    EXPECT_TRUE(_viewer->isWipeDisabledByDataKind());
}

TEST_F(ViewerSceneTest, AFailingSceneIsReportedWithoutFailingTheViewer)
{
    NodePtr reader = createUsdRead(NATRON_TESTS_FIXTURES_DIR "/no-such-scene.usda");
    ASSERT_TRUE(bool(reader));
    connectNodes(reader, _viewerNode, 0, true);
    ASSERT_EQ(eDataKindScene, _viewer->getActiveInputDataKind());

    EXPECT_NE(ViewerInstance::eViewerRenderRetCodeFail, renderFrame());
    std::string error;
    EXPECT_FALSE(_viewer->getLastEvaluatedScene(0, NULL, &error));
    EXPECT_FALSE(error.empty());
}

TEST_F(ViewerSceneTest, APausedViewerRendersNoLiveStroke)
{
    NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
    NodePtr rotoPaint = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTOPAINT));
    ASSERT_TRUE(constant && rotoPaint);
    connectNodes(constant, rotoPaint, 0, true);
    connectNodes(rotoPaint, _viewerNode, 0, true);
    _viewer->setViewerPaused(true, true);

    // The live stroke render, as the viewer's render thread runs it while the pen is down.
    const SequenceTime time = (SequenceTime)getApp()->getTimeLine()->currentFrame();
    const U64 viewerHash = _viewer->getHash();
    ViewerArgsPtr argsA, argsB;
    ViewerInstance::ViewerRenderRetCode ret = ViewerInstance::eViewerRenderRetCodeFail;
    ViewerInstance* viewer = _viewer;
    std::thread renderThread([&]() {
        try {
            ret = viewer->getViewerArgsAndRenderViewer(time, true, ViewIdx(0), viewerHash, rotoPaint, RotoStrokeItemPtr(), RenderStatsPtr(), &argsA, &argsB);
        } catch (...) {
            ret = ViewerInstance::eViewerRenderRetCodeFail;
        }
    });
    renderThread.join();

    EXPECT_NE(ViewerInstance::eViewerRenderRetCodeFail, ret);
    EXPECT_FALSE(argsA && argsA->params);
    EXPECT_FALSE(argsB && argsB->params);
}
