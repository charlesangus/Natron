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

#ifdef NATRON_HAVE_GPU

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QString>
#include <QStringList>
#include <QThread>

#include "Tests/BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/FrameRenderContext.h"
#include "Engine/GpuContext.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/LibraryBinary.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/OpenGLViewerI.h"
#include "Engine/Plugin.h"
#include "Engine/PluginActionShortcut.h"
#include "Engine/Project.h"
#include "Engine/RenderStats.h"
#include "Engine/TimeLine.h"
#include "Engine/UpdateViewerParams.h"
#include "Engine/ViewIdx.h"
#include "Engine/ViewerHandoff.h"
#include "Engine/ViewerInstance.h"

#include "Engine/Gpu/GpuBufferPool.h"
#include "Engine/Gpu/GpuRuntime.h"

NATRON_NAMESPACE_USING

namespace {

const int kSize = 64;

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
        // Registered after AppManager::load(), so the label-without-suffix the script name is made
        // from was never assigned.
        p->setLabelWithoutSuffix(Plugin::makeLabelWithoutSuffix(p->getPluginLabel()));
        std::list<PluginActionShortcut> shortcuts;
        node->getPluginShortcuts(&shortcuts);
        p->setShorcuts(shortcuts);
        p->setOpenGLRenderSupport(node->supportsOpenGLRender());
        p->setForInternalUseOnly(true);
    });
}

// The viewer widget as the render thread sees it: a single tile covering the whole frame at 100%.
class FakeViewerUi
    : public OpenGLViewerI {
public:
    FakeViewerUi()
        : timeline()
        , rung(ViewerHandoffRung::HostSync)
        , refusePrepare(false)
        , prepares(0)
        , preparedOnMainThread(false)
    {
    }

    ViewerHandoffRung handoffRung() const OVERRIDE
    {
        return rung;
    }

    ViewerHandoffResult prepareHandoff(const ProjectColorManagement::DisplayProcessorPtr& /*processor*/,
                                       ViewerHandoffFormat /*format*/) OVERRIDE
    {
        ++prepares;
        if (QCoreApplication::instance() && (QThread::currentThread() == QCoreApplication::instance()->thread())) {
            preparedOnMainThread = true;
        }
        ViewerHandoffResult result;
        result.ok = !refusePrepare;
        if (refusePrepare) {
            result.reason = "refused by the test";
        }

        return result;
    }

    bool isViewerUIVisible() const OVERRIDE { return false; }
    double getZoomFactor() const OVERRIDE { return 1.; }
    void fitImageToFormat() OVERRIDE { }
    bool isClippingImageToFormat() const OVERRIDE { return true; }

    RectI getImageRectangleDisplayed(const RectI& pixelRod,
                                     const double /*par*/,
                                     unsigned int /*mipmapLevel*/) OVERRIDE
    {
        return pixelRod;
    }

    RectI getImageRectangleDisplayedRoundedToTileSize(int /*texIndex*/,
                                                      const RectD& rod,
                                                      const double par,
                                                      unsigned int mipmapLevel,
                                                      std::vector<RectI>* tiles,
                                                      std::vector<RectI>* tilesRounded,
                                                      int* tileSize,
                                                      RectI* roiNotRounded) OVERRIDE
    {
        const RectI frame(0, 0, kSize >> mipmapLevel, kSize >> mipmapLevel);
        RectI roi = rod.toPixelEnclosing(mipmapLevel, par);
        roi.clip(frame);
        if (tileSize) {
            *tileSize = kSize;
        }
        if (roiNotRounded) {
            *roiNotRounded = roi;
        }
        if (!roi.isNull()) {
            if (tiles) {
                tiles->push_back(roi);
            }
            if (tilesRounded) {
                tilesRounded->push_back(frame);
            }
        }

        return roi.isNull() ? roi : frame;
    }

    RectI getExactImageRectangleDisplayed(int /*texIndex*/,
                                          const RectD& rod,
                                          const double par,
                                          unsigned int mipmapLevel) OVERRIDE
    {
        RectI roi = rod.toPixelEnclosing(mipmapLevel, par);
        roi.clip(RectI(0, 0, kSize >> mipmapLevel, kSize >> mipmapLevel));

        return roi;
    }

    ImageBitDepthEnum getBitDepth() const OVERRIDE { return eImageBitDepthFloat; }
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

    int getCurrentlyDisplayedTime() const OVERRIDE
    {
        return (int)getTimeline()->currentFrame();
    }

    void getViewerFrameRange(int* first, int* last) const OVERRIDE
    {
        *first = 1;
        *last = 1;
    }

    ViewerCompositingOperatorEnum getCompositingOperator() const OVERRIDE { return eViewerCompositingOperatorNone; }
    void setCompositingOperator(ViewerCompositingOperatorEnum /*op*/) OVERRIDE { }

    TimeLinePtr getTimeline() const OVERRIDE
    {
        return timeline;
    }

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
        width = kSize;
        height = kSize;
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

    RectD getViewportRect() const OVERRIDE { return RectD(0, 0, kSize, kSize); }

    void getCursorPosition(double& x, double& y) const OVERRIDE
    {
        x = 0.;
        y = 0.;
    }

    TimeLinePtr timeline;
    std::atomic<ViewerHandoffRung> rung;
    std::atomic<bool> refusePrepare;
    std::atomic<int> prepares;
    std::atomic<bool> preparedOnMainThread;
};

// What the hook saw of the frame the viewer submitted.
struct HookRecord {
    std::mutex lock;
    int calls = 0;
    bool wanted = false;
    GpuDeviceImagePtr delivered;
};

struct RenderOutcome {
    ViewerInstance::ViewerRenderRetCode ret = ViewerInstance::eViewerRenderRetCodeFail;
    UpdateViewerParamsPtr params;
};

bool
anyTileHoldsRam(const UpdateViewerParams& params)
{
    for (std::list<UpdateViewerParams::CachedTile>::const_iterator it = params.tiles.begin(); it != params.tiles.end(); ++it) {
        if (it->ramBuffer) {
            return true;
        }
    }

    return false;
}

} // namespace

class ViewerDeviceRoot
    : public BaseTest {
protected:
    ViewerDeviceRoot()
        : BaseTest()
        , _ui()
        , _gpu()
        , _viewer(0)
        , _viewerNode()
        , _savedMode(eRenderSchedulerModeTaskGraph)
        , _skipReason()
    {
    }

    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        _savedMode = appPTR->getRenderSchedulerMode();
        appPTR->setRenderSchedulerMode(eRenderSchedulerModeTaskGraph);
        ProjectPtr project = getApp()->getProject();
        project->reset(false, true);
        project->setOrAddProjectFormat(Format(0, 0, kSize, kSize, "viewerDeviceRoot64", 1.));
        appPTR->clearAllCaches();

        GpuContextOptions options;
        options.mode = GpuResolvedMode::Force;
        std::shared_ptr<std::string> unavailable = std::make_shared<std::string>();
        options.infoLog = [unavailable](const std::string& line) {
            *unavailable = line;
        };
        std::unique_ptr<GpuContext> gpu(new GpuContext(options));
        if (gpu->waitForInit(std::chrono::seconds(60)) && gpu->isAvailable()) {
            _gpu = std::move(gpu);
        } else {
            _skipReason = unavailable->empty() ? std::string("no Vulkan device") : *unavailable;
        }

        NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
        registerViewerPluginOnce();
        CreateNodeArgs viewerArgs(PLUGINID_NATRON_VIEWER, getApp()->getProject());
        viewerArgs.setProperty<bool>(kCreateNodeArgsPropAllowNonUserCreatablePlugins, true);
        _viewerNode = getApp()->createNode(viewerArgs);
        ASSERT_TRUE(constant && _viewerNode);
        _viewer = _viewerNode->isEffectViewer();
        ASSERT_TRUE(_viewer != NULL);
        _ui.timeline = getApp()->getTimeLine();
        _viewer->setUiContext(&_ui);
        connectNodes(constant, _viewerNode, 0, true);
    }

    virtual void TearDown() OVERRIDE
    {
        ViewerInstance::setSchedulerResultHookForTests(ViewerInstance::SchedulerResultHook());
        if (_viewer) {
            _viewer->invalidateUiContext();
        }
        _viewer = 0;
        _viewerNode.reset();
        appPTR->clearAllCaches();
        getApp()->getProject()->reset(false, true);
        _gpu.reset();
        appPTR->setRenderSchedulerMode(_savedMode);
        BaseTest::TearDown();
    }

    // The bundled gtest has no GTEST_SKIP, so each test returns early on false.
    bool gpuOrSkip()
    {
        if (_gpu) {
            return true;
        }
        const char* require = std::getenv("NATRON_TEST_REQUIRE_GPU");
        if (require && (std::strcmp(require, "1") == 0)) {
            ADD_FAILURE() << "NATRON_TEST_REQUIRE_GPU=1 but " << _skipReason;
        } else {
            std::printf("[  SKIPPED ] %s\n", _skipReason.c_str());
        }

        return false;
    }

    /**
     * @brief Makes the scheduler's result carry a device root as large as the RAM root, recording what the frame
     * asked for. When dropRamPlanes is set the RAM root is taken away, as from a frame whose root stayed on the
     * device.
     **/
    void deliverDeviceRoot(const std::shared_ptr<HookRecord>& record,
                           bool dropRamPlanes)
    {
        gpu::GpuRuntime* runtime = _gpu->runtime();
        ViewerInstance::setSchedulerResultHookForTests([record, runtime, dropRamPlanes](const FrameRenderContextPtr& context,
                                                                                        std::map<ImageLayerDesc, ImagePtr>* rootPlanes) {
            std::lock_guard<std::mutex> k(record->lock);
            ++record->calls;
            record->wanted = context && context->wantsRootOnDevice();
            if (!context || !rootPlanes || rootPlanes->empty() || !rootPlanes->begin()->second) {
                return;
            }
            const ImagePtr& ram = rootPlanes->begin()->second;
            GpuImageLayout layout;
            layout.layer = ram->getComponents();
            layout.rod = ram->getRoD();
            layout.bounds = ram->getBounds();
            layout.mipmapLevel = ram->getMipmapLevel();
            gpu::GpuBufferHandle handle;
            if (!runtime->acquire(layout.deviceBytes(), handle)) {
                return;
            }
            GpuDeviceImagePtr device = GpuDeviceImage::create(*runtime, handle, layout, RenderStatsPtr());
            std::map<ImageLayerDesc, GpuDeviceImagePtr> devicePlanes;
            devicePlanes[rootPlanes->begin()->first] = device;
            context->setRootDevicePlanes(devicePlanes);
            record->delivered = device;
            if (dropRamPlanes) {
                rootPlanes->clear();
            }
        });
    }

    /**
     * @brief Renders the current frame on a thread of its own, neither the main thread nor one of the pool, as the
     * viewer's render thread does.
     **/
    RenderOutcome renderFrame(const RenderStatsPtr& stats)
    {
        RenderOutcome outcome;
        const SequenceTime time = (SequenceTime)getApp()->getTimeLine()->currentFrame();
        const U64 viewerHash = _viewer->getHash();
        ViewerArgsPtr args[2];
        args[0] = std::make_shared<ViewerArgs>();
        const ViewerInstance::ViewerRenderRetCode prepared = _viewer->getRenderViewerArgsAndCheckCache_public(time, false, ViewIdx(0), 0, viewerHash, true, NodePtr(), stats, args[0].get());
        if ((prepared == ViewerInstance::eViewerRenderRetCodeFail) || (prepared == ViewerInstance::eViewerRenderRetCodeBlack)) {
            outcome.ret = prepared;

            return outcome;
        }
        ViewerInstance* viewer = _viewer;
        std::thread renderThread([&outcome, &args, viewer, viewerHash, stats]() {
            try {
                outcome.ret = viewer->renderViewer(ViewIdx(0), false, false, viewerHash, true, NodePtr(), true, args,
                                                   ViewerCurrentFrameRequestSchedulerStartArgsPtr(), stats);
            } catch (...) {
                outcome.ret = ViewerInstance::eViewerRenderRetCodeFail;
            }
        });
        renderThread.join();
        if (args[0]) {
            outcome.params = args[0]->params;
        }

        return outcome;
    }

    FakeViewerUi _ui;
    std::unique_ptr<GpuContext> _gpu;
    ViewerInstance* _viewer;
    NodePtr _viewerNode;
    RenderSchedulerModeEnum _savedMode;
    std::string _skipReason;
};

TEST_F(ViewerDeviceRoot, ADeviceRootReplacesTheRamTiles)
{
    if (!gpuOrSkip()) {
        return;
    }
    std::shared_ptr<HookRecord> record = std::make_shared<HookRecord>();
    deliverDeviceRoot(record, true);

    RenderOutcome outcome = renderFrame(std::make_shared<RenderStats>(false));

    ASSERT_EQ(1, record->calls);
    EXPECT_TRUE(record->wanted);
    ASSERT_EQ(ViewerInstance::eViewerRenderRetCodeRender, outcome.ret);
    ASSERT_TRUE(bool(outcome.params));
    EXPECT_TRUE(bool(outcome.params->deviceColor));
    EXPECT_EQ(record->delivered, outcome.params->deviceColor);
    EXPECT_FALSE(bool(outcome.params->colorImage));
    EXPECT_FALSE(anyTileHoldsRam(*outcome.params));
    EXPECT_EQ(std::size_t(0), outcome.params->sizeInRAM());
    EXPECT_EQ(1, _ui.prepares.load());
    EXPECT_FALSE(_ui.preparedOnMainThread.load());

    outcome = RenderOutcome();
    record->delivered.reset();
}

TEST_F(ViewerDeviceRoot, AViewerOnTheCpuRungKeepsTheRamPath)
{
    if (!gpuOrSkip()) {
        return;
    }
    _ui.rung = ViewerHandoffRung::Cpu;
    std::shared_ptr<HookRecord> record = std::make_shared<HookRecord>();
    deliverDeviceRoot(record, false);

    RenderOutcome outcome = renderFrame(std::make_shared<RenderStats>(false));

    ASSERT_EQ(1, record->calls);
    EXPECT_FALSE(record->wanted);
    ASSERT_EQ(ViewerInstance::eViewerRenderRetCodeRender, outcome.ret);
    ASSERT_TRUE(bool(outcome.params));
    EXPECT_FALSE(bool(outcome.params->deviceColor));
    EXPECT_TRUE(bool(outcome.params->colorImage));
    EXPECT_TRUE(anyTileHoldsRam(*outcome.params));
    EXPECT_EQ(0, _ui.prepares.load());

    outcome = RenderOutcome();
    record->delivered.reset();
}

TEST_F(ViewerDeviceRoot, AutoContrastKeepsTheRamPath)
{
    if (!gpuOrSkip()) {
        return;
    }
    _viewer->onAutoContrastChanged(true, false);
    std::shared_ptr<HookRecord> record = std::make_shared<HookRecord>();
    deliverDeviceRoot(record, false);

    RenderOutcome outcome = renderFrame(std::make_shared<RenderStats>(false));

    ASSERT_EQ(1, record->calls);
    EXPECT_FALSE(record->wanted);
    ASSERT_EQ(ViewerInstance::eViewerRenderRetCodeRender, outcome.ret);
    ASSERT_TRUE(bool(outcome.params));
    EXPECT_FALSE(bool(outcome.params->deviceColor));
    EXPECT_TRUE(anyTileHoldsRam(*outcome.params));
    EXPECT_EQ(0, _ui.prepares.load());

    outcome = RenderOutcome();
    record->delivered.reset();
    _viewer->onAutoContrastChanged(false, false);
}

TEST_F(ViewerDeviceRoot, ARefusedPrepareDownloadsTheDeviceRoot)
{
    if (!gpuOrSkip()) {
        return;
    }
    _ui.refusePrepare = true;
    std::shared_ptr<HookRecord> record = std::make_shared<HookRecord>();
    deliverDeviceRoot(record, true);
    RenderStatsPtr stats = std::make_shared<RenderStats>(false);

    RenderOutcome outcome = renderFrame(stats);

    ASSERT_EQ(1, record->calls);
    EXPECT_TRUE(record->wanted);
    ASSERT_EQ(ViewerInstance::eViewerRenderRetCodeRender, outcome.ret);
    ASSERT_TRUE(bool(outcome.params));
    EXPECT_FALSE(bool(outcome.params->deviceColor));
    EXPECT_TRUE(bool(outcome.params->colorImage));
    EXPECT_TRUE(anyTileHoldsRam(*outcome.params));
    EXPECT_EQ(1, _ui.prepares.load());
    const std::map<std::string, int> reasons = stats->getLegacyFallbackReasons();
    EXPECT_EQ(1u, reasons.count("viewer handoff: refused by the test"));

    outcome = RenderOutcome();
    record->delivered.reset();
}

#endif // NATRON_HAVE_GPU
