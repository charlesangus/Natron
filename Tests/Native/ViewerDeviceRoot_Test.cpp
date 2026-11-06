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
#include <QThread>

#include "Tests/BaseTest.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/FrameRenderContext.h"
#include "Engine/GpuContext.h"
#include "Engine/GpuPlacement.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/OpenGLViewerI.h"
#include "Engine/Project.h"
#include "Engine/RenderStats.h"
#include "Engine/Settings.h"
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

// The viewer widget as the render thread sees it: the whole frame at 100%, in tiles of the size the viewer cache
// was set up for.
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
        // The viewer cache is tiled at the settings' tile size and bit depth, so every texture tile must be too.
        const int tile = 1 << appPTR->getCurrentSettings()->getViewerTilesPowerOf2();
        if (tileSize) {
            *tileSize = tile;
        }
        if (roiNotRounded) {
            *roiNotRounded = roi;
        }
        if (roi.isNull()) {
            return roi;
        }
        const RectI rounded((roi.x1 / tile) * tile, (roi.y1 / tile) * tile,
                            ((roi.x2 + tile - 1) / tile) * tile, ((roi.y2 + tile - 1) / tile) * tile);
        for (int y = rounded.y1; y < rounded.y2; y += tile) {
            for (int x = rounded.x1; x < rounded.x2; x += tile) {
                const RectI t(x, y, x + tile, y + tile);
                if (tilesRounded) {
                    tilesRounded->push_back(t);
                }
                if (tiles) {
                    tiles->push_back(t.intersect(roi));
                }
            }
        }

        return rounded;
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

// Leaves the process as the test found it, whatever it asserted on the way.
struct RenderAges {
    U64 render[2] = { 0, 0 };
    U64 display[2] = { 0, 0 };

    explicit RenderAges(const ViewerInstance* viewer = 0)
    {
        if (viewer) {
            read(viewer);
        }
    }

    void read(const ViewerInstance* viewer)
    {
        for (int texture = 0; texture < 2; ++texture) {
            viewer->getRenderAgesForTests(texture, &render[texture], &display[texture]);
        }
    }
};

void
expectSameRenderAges(const RenderAges& before,
                     const RenderAges& after)
{
    for (int texture = 0; texture < 2; ++texture) {
        EXPECT_EQ(before.render[texture], after.render[texture]);
        EXPECT_EQ(before.display[texture], after.display[texture]);
    }
}

struct PlacementOverlayScope {
    GpuPlacementAvailableFunc savedAvailable;

    PlacementOverlayScope(bool overlayEnabled,
                          GpuPlacementAvailableFunc available)
        : savedAvailable(setGpuPlacementAvailableFunc(available))
    {
        ViewerInstance::setGpuPlacementOverlayEnabled(overlayEnabled);
    }

    ~PlacementOverlayScope()
    {
        ViewerInstance::setGpuPlacementFrozenForTests(false);
        ViewerInstance::setGpuPlacementOverlayEnabled(false);
        setGpuPlacementAvailableFunc(savedAvailable);
    }
};

bool
placementAlwaysAvailable()
{
    return true;
}

// Dry runs happen on the viewer's own thread, some time after the request.
bool
waitForDryRunCount(int count)
{
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);

    while (ViewerInstance::getGpuPlacementDryRunCountForTests() < count) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        QThread::msleep(5);
    }

    return true;
}

// Counts on the emitting thread, so a test sees an emission as soon as it happened, with no event loop to run.
class SnapshotChangeSpy {
public:
    explicit SnapshotChangeSpy(ViewerInstance* viewer)
        : _count(std::make_shared<std::atomic<int>>(0))
    {
        std::shared_ptr<std::atomic<int>> count = _count;
        _connection = QObject::connect(viewer, &ViewerInstance::gpuPlacementSnapshotChanged, [count]() {
            count->fetch_add(1);
        });
    }

    ~SnapshotChangeSpy()
    {
        QObject::disconnect(_connection);
    }

    int count() const
    {
        return _count->load();
    }

private:
    std::shared_ptr<std::atomic<int>> _count;
    QMetaObject::Connection _connection;
};

// Longer than the planner's debounce and its poll while held, so that a plan it was going to make has been made.
void
letThePlannerSettle()
{
    QThread::msleep(500);
}

const char* const kTexturesBitDepthKnob = "texturesBitDepth";

KnobChoice*
texturesBitDepthKnob()
{
    return dynamic_cast<KnobChoice*>(appPTR->getCurrentSettings()->getKnobByName(kTexturesBitDepthKnob).get());
}

} // namespace

// The parameter is the "Viewer textures bit depth" choice the viewer cache and the viewer's textures follow.
class ViewerDeviceRoot
    : public BaseTest,
      public ::testing::WithParamInterface<std::string> {
protected:
    ViewerDeviceRoot()
        : BaseTest()
        , _ui()
        , _gpu()
        , _viewer(0)
        , _viewerNode()
        , _savedMode(eRenderSchedulerModeTaskGraph)
        , _savedTexturesDepth(-1)
        , _skipReason()
        , _depthSkipReason()
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
        selectTexturesDepth(GetParam());
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
        restoreTexturesDepth();
        appPTR->setRenderSchedulerMode(_savedMode);
        BaseTest::TearDown();
    }

    // The bundled gtest has no GTEST_SKIP, so each test returns early on false.
    bool gpuOrSkip()
    {
        if (!_depthSkipReason.empty()) {
            std::printf("[  SKIPPED ] %s\n", _depthSkipReason.c_str());

            return false;
        }
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
    RenderOutcome renderFrame(const RenderStatsPtr& stats,
                              bool sequential = false)
    {
        RenderOutcome outcome;
        const SequenceTime time = (SequenceTime)getApp()->getTimeLine()->currentFrame();
        const U64 viewerHash = _viewer->getHash();
        ViewerArgsPtr args[2];
        args[0] = std::make_shared<ViewerArgs>();
        const ViewerInstance::ViewerRenderRetCode prepared = _viewer->getRenderViewerArgsAndCheckCache_public(time, sequential, ViewIdx(0), 0, viewerHash, true, NodePtr(), stats, args[0].get());
        if ((prepared == ViewerInstance::eViewerRenderRetCodeFail) || (prepared == ViewerInstance::eViewerRenderRetCodeBlack)) {
            outcome.ret = prepared;

            return outcome;
        }
        ViewerInstance* viewer = _viewer;
        std::thread renderThread([&outcome, &args, viewer, viewerHash, stats, sequential]() {
            try {
                outcome.ret = viewer->renderViewer(ViewIdx(0), false, sequential, viewerHash, true, NodePtr(), true, args,
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

    /**
     * @brief Picks the viewer textures bit depth by its choice id and retiles the viewer cache for it. The retile is
     * explicit so the cache's tile size is right even when the choice already held that value.
     **/
    void selectTexturesDepth(const std::string& id)
    {
        KnobChoice* choice = texturesBitDepthKnob();
        ASSERT_TRUE(choice != NULL);
        _savedTexturesDepth = choice->getValue();
        const bool wantsFloat = (id == "32f");
        if (wantsFloat && !appPTR->isTextureFloatSupported()) {
            _depthSkipReason = "no OpenGL float texture support, so the viewer cannot use 32-bit textures";

            return;
        }
        const std::vector<ChoiceOption> entries = choice->getEntries_mt_safe();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].id == id) {
                choice->setValue((int)i, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);
                appPTR->onViewerTileCacheSizeChanged();
                ASSERT_EQ(wantsFloat ? eImageBitDepthFloat : eImageBitDepthByte, appPTR->getCurrentSettings()->getViewersBitDepth());

                return;
            }
        }
        FAIL() << "no viewer textures bit depth " << id;
    }

    void restoreTexturesDepth()
    {
        KnobChoice* choice = texturesBitDepthKnob();
        if (!choice || (_savedTexturesDepth < 0)) {
            return;
        }
        if (choice->getValue() != _savedTexturesDepth) {
            choice->setValue(_savedTexturesDepth, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);
        }
        appPTR->onViewerTileCacheSizeChanged();
    }

    ImageBitDepthEnum expectedDepth() const
    {
        return (GetParam() == "32f") ? eImageBitDepthFloat : eImageBitDepthByte;
    }

    FakeViewerUi _ui;
    std::unique_ptr<GpuContext> _gpu;
    ViewerInstance* _viewer;
    NodePtr _viewerNode;
    RenderSchedulerModeEnum _savedMode;
    int _savedTexturesDepth;
    std::string _skipReason;
    std::string _depthSkipReason;
};

TEST_P(ViewerDeviceRoot, ADeviceRootReplacesTheRamTiles)
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
    EXPECT_EQ(expectedDepth(), outcome.params->depth);
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

TEST_P(ViewerDeviceRoot, AViewerOnTheCpuRungKeepsTheRamPath)
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
    EXPECT_EQ(expectedDepth(), outcome.params->depth);
    EXPECT_FALSE(bool(outcome.params->deviceColor));
    EXPECT_TRUE(bool(outcome.params->colorImage));
    EXPECT_TRUE(anyTileHoldsRam(*outcome.params));
    EXPECT_EQ(0, _ui.prepares.load());

    outcome = RenderOutcome();
    record->delivered.reset();
}

TEST_P(ViewerDeviceRoot, AutoContrastKeepsTheRamPath)
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
    EXPECT_EQ(expectedDepth(), outcome.params->depth);
    EXPECT_FALSE(bool(outcome.params->deviceColor));
    EXPECT_TRUE(anyTileHoldsRam(*outcome.params));
    EXPECT_EQ(0, _ui.prepares.load());

    outcome = RenderOutcome();
    record->delivered.reset();
    _viewer->onAutoContrastChanged(false, false);
}

TEST_P(ViewerDeviceRoot, ARefusedPrepareDownloadsTheDeviceRoot)
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
    EXPECT_EQ(expectedDepth(), outcome.params->depth);
    EXPECT_FALSE(bool(outcome.params->deviceColor));
    EXPECT_TRUE(bool(outcome.params->colorImage));
    EXPECT_TRUE(anyTileHoldsRam(*outcome.params));
    EXPECT_EQ(1, _ui.prepares.load());
    const std::map<std::string, int> reasons = stats->getLegacyFallbackReasons();
    EXPECT_EQ(1u, reasons.count("viewer handoff: refused by the test"));

    outcome = RenderOutcome();
    record->delivered.reset();
}

TEST_P(ViewerDeviceRoot, WithTheOverlayOffARenderPlacesNoShadowAndPublishesNothing)
{
    if (!gpuOrSkip()) {
        return;
    }
    const PlacementOverlayScope scope(false, &placementAlwaysAvailable);
    const int shadowsBefore = ViewerInstance::getGpuShadowPlacementCountForTests();

    const RenderOutcome outcome = renderFrame(std::make_shared<RenderStats>(false));

    ASSERT_EQ(ViewerInstance::eViewerRenderRetCodeRender, outcome.ret);
    EXPECT_EQ(shadowsBefore, ViewerInstance::getGpuShadowPlacementCountForTests());
    EXPECT_FALSE(bool(_viewer->getGpuPlacementSnapshot()));

    ViewerInstance::setGpuPlacementOverlayEnabled(true);
    EXPECT_FALSE(bool(_viewer->getGpuPlacementSnapshot()));
}

TEST_P(ViewerDeviceRoot, WithTheOverlayOnARenderPublishesASnapshot)
{
    if (!gpuOrSkip()) {
        return;
    }
    const PlacementOverlayScope scope(true, &placementAlwaysAvailable);
    const int shadowsBefore = ViewerInstance::getGpuShadowPlacementCountForTests();

    const RenderOutcome outcome = renderFrame(std::make_shared<RenderStats>(false));

    ASSERT_EQ(ViewerInstance::eViewerRenderRetCodeRender, outcome.ret);
    EXPECT_EQ(shadowsBefore + 1, ViewerInstance::getGpuShadowPlacementCountForTests());
    EXPECT_TRUE(bool(_viewer->getGpuPlacementSnapshot()));

    ViewerInstance::setGpuPlacementOverlayEnabled(false);
    EXPECT_FALSE(bool(_viewer->getGpuPlacementSnapshot()));
}

TEST_P(ViewerDeviceRoot, WithNoPlacementAvailableTheOverlayHasNothingToDraw)
{
    if (!gpuOrSkip()) {
        return;
    }
    const PlacementOverlayScope scope(true, nullptr);
    const int shadowsBefore = ViewerInstance::getGpuShadowPlacementCountForTests();

    const RenderOutcome outcome = renderFrame(std::make_shared<RenderStats>(false));

    ASSERT_EQ(ViewerInstance::eViewerRenderRetCodeRender, outcome.ret);
    EXPECT_EQ(shadowsBefore, ViewerInstance::getGpuShadowPlacementCountForTests());
    const GpuPlacementSnapshotPtr snapshot = _viewer->getGpuPlacementSnapshot();
    ASSERT_TRUE(bool(snapshot));
    EXPECT_TRUE(snapshot->regions.empty());
}

TEST_P(ViewerDeviceRoot, TurningTheOverlayOnPlansWithoutARender)
{
    if (!gpuOrSkip()) {
        return;
    }
    const int dryRunsBefore = ViewerInstance::getGpuPlacementDryRunCountForTests();
    const int shadowsBefore = ViewerInstance::getGpuShadowPlacementCountForTests();
    const RenderAges agesBefore(_viewer);
    const int preparesBefore = _ui.prepares.load();

    const PlacementOverlayScope scope(true, &placementAlwaysAvailable);

    ASSERT_TRUE(waitForDryRunCount(dryRunsBefore + 1));
    const GpuPlacementSnapshotPtr snapshot = _viewer->getGpuPlacementSnapshot();
    ASSERT_TRUE(bool(snapshot));
    ASSERT_FALSE(snapshot->regions.empty());
    for (std::size_t i = 0; i < snapshot->regions.size(); ++i) {
        EXPECT_FALSE(snapshot->regions[i].fellBack);
    }

    EXPECT_EQ(shadowsBefore, ViewerInstance::getGpuShadowPlacementCountForTests());
    EXPECT_EQ(preparesBefore, _ui.prepares.load());
    expectSameRenderAges(agesBefore, RenderAges(_viewer));
}

TEST_P(ViewerDeviceRoot, ADryRunDuringARenderLeavesItsAgesAloneAndDoesNotAbortIt)
{
    if (!gpuOrSkip()) {
        return;
    }
    const int firstDryRun = ViewerInstance::getGpuPlacementDryRunCountForTests() + 1;
    const PlacementOverlayScope scope(true, &placementAlwaysAvailable);
    ASSERT_TRUE(waitForDryRunCount(firstDryRun));

    ViewerInstance* viewer = _viewer;
    RenderAges agesBefore, agesAfter;
    bool planned = false;
    ViewerInstance::setSchedulerResultHookForTests([&](const FrameRenderContextPtr&,
                                                       std::map<ImageLayerDesc, ImagePtr>*) {
        agesBefore.read(viewer);
        const int dryRuns = ViewerInstance::getGpuPlacementDryRunCountForTests();
        viewer->requestGpuPlacementDryRun();
        planned = waitForDryRunCount(dryRuns + 1);
        agesAfter.read(viewer);
    });

    const RenderOutcome outcome = renderFrame(std::make_shared<RenderStats>(false));
    ViewerInstance::setSchedulerResultHookForTests(ViewerInstance::SchedulerResultHook());

    ASSERT_TRUE(planned);
    expectSameRenderAges(agesBefore, agesAfter);
    ASSERT_EQ(ViewerInstance::eViewerRenderRetCodeRender, outcome.ret);
    ASSERT_TRUE(bool(outcome.params));
    EXPECT_FALSE(outcome.params->abortInfo->isAborted());
}

TEST_P(ViewerDeviceRoot, ABurstOfRequestsGivesOneDryRun)
{
    const int firstDryRun = ViewerInstance::getGpuPlacementDryRunCountForTests() + 1;
    const PlacementOverlayScope scope(true, nullptr);
    ASSERT_TRUE(waitForDryRunCount(firstDryRun));

    for (int i = 0; i < 20; ++i) {
        _viewer->requestGpuPlacementDryRun();
        QThread::msleep(2);
    }

    ASSERT_TRUE(waitForDryRunCount(firstDryRun + 1));
    letThePlannerSettle();
    EXPECT_EQ(firstDryRun + 1, ViewerInstance::getGpuPlacementDryRunCountForTests());
}

TEST_P(ViewerDeviceRoot, TurningTheOverlayOnDoesNotShowAnEarlierSnapshot)
{
    if (!gpuOrSkip()) {
        return;
    }
    const PlacementOverlayScope scope(true, &placementAlwaysAvailable);
    ASSERT_EQ(ViewerInstance::eViewerRenderRetCodeRender, renderFrame(std::make_shared<RenderStats>(false)).ret);
    const GpuPlacementSnapshotPtr earlier = _viewer->getGpuPlacementSnapshot();
    ASSERT_TRUE(bool(earlier));

    ViewerInstance::setGpuPlacementOverlayEnabled(false);
    const int dryRuns = ViewerInstance::getGpuPlacementDryRunCountForTests();
    ViewerInstance::setGpuPlacementOverlayEnabled(true);
    EXPECT_FALSE(bool(_viewer->getGpuPlacementSnapshot()));

    ASSERT_TRUE(waitForDryRunCount(dryRuns + 1));
    const GpuPlacementSnapshotPtr replaced = _viewer->getGpuPlacementSnapshot();
    ASSERT_TRUE(bool(replaced));
    EXPECT_NE(earlier.get(), replaced.get());
}

TEST_P(ViewerDeviceRoot, ARealRenderReplacesADryRun)
{
    if (!gpuOrSkip()) {
        return;
    }
    const int firstDryRun = ViewerInstance::getGpuPlacementDryRunCountForTests() + 1;
    const PlacementOverlayScope scope(true, &placementAlwaysAvailable);
    ASSERT_TRUE(waitForDryRunCount(firstDryRun));
    const GpuPlacementSnapshotPtr planned = _viewer->getGpuPlacementSnapshot();
    ASSERT_TRUE(bool(planned));

    ASSERT_EQ(ViewerInstance::eViewerRenderRetCodeRender, renderFrame(std::make_shared<RenderStats>(false)).ret);

    const GpuPlacementSnapshotPtr rendered = _viewer->getGpuPlacementSnapshot();
    ASSERT_TRUE(bool(rendered));
    EXPECT_NE(planned.get(), rendered.get());
}

TEST_P(ViewerDeviceRoot, APendingDryRunNeverReplacesARealRenderOfTheSameState)
{
    if (!gpuOrSkip()) {
        return;
    }
    const int firstDryRun = ViewerInstance::getGpuPlacementDryRunCountForTests() + 1;
    const PlacementOverlayScope scope(true, &placementAlwaysAvailable);
    ASSERT_TRUE(waitForDryRunCount(firstDryRun));

    // The render starts inside the plan's debounce and takes the stamp of the request, so both describe one state.
    _viewer->requestGpuPlacementDryRun();
    ASSERT_EQ(ViewerInstance::eViewerRenderRetCodeRender, renderFrame(std::make_shared<RenderStats>(false)).ret);
    const GpuPlacementSnapshotPtr rendered = _viewer->getGpuPlacementSnapshot();
    ASSERT_TRUE(bool(rendered));

    letThePlannerSettle();

    EXPECT_EQ(rendered.get(), _viewer->getGpuPlacementSnapshot().get());
}

TEST_P(ViewerDeviceRoot, InLegacyModeADryRunPublishesAnEmptySnapshot)
{
    appPTR->setRenderSchedulerMode(eRenderSchedulerModeLegacy);
    const int firstDryRun = ViewerInstance::getGpuPlacementDryRunCountForTests() + 1;
    const PlacementOverlayScope scope(true, &placementAlwaysAvailable);

    ASSERT_TRUE(waitForDryRunCount(firstDryRun));

    const GpuPlacementSnapshotPtr snapshot = _viewer->getGpuPlacementSnapshot();
    ASSERT_TRUE(bool(snapshot));
    EXPECT_TRUE(snapshot->regions.empty());
}

TEST_P(ViewerDeviceRoot, SwitchingTheSchedulerModeReplansEveryViewer)
{
    if (!gpuOrSkip()) {
        return;
    }
    if (std::getenv("NATRON_RENDER_SCHEDULER")) {
        std::printf("[  SKIPPED ] NATRON_RENDER_SCHEDULER pins the scheduler mode\n");

        return;
    }
    const SnapshotChangeSpy spy(_viewer);
    int dryRuns = ViewerInstance::getGpuPlacementDryRunCountForTests() + 1;
    const PlacementOverlayScope scope(true, &placementAlwaysAvailable);
    ASSERT_TRUE(waitForDryRunCount(dryRuns));
    ASSERT_EQ(1, spy.count());
    ASSERT_FALSE(_viewer->getGpuPlacementSnapshot()->regions.empty());

    appPTR->onRenderSchedulerModeSettingChanged(eRenderSchedulerModeLegacy);
    ASSERT_TRUE(waitForDryRunCount(++dryRuns));
    EXPECT_EQ(2, spy.count());
    EXPECT_TRUE(_viewer->getGpuPlacementSnapshot()->regions.empty());

    appPTR->onRenderSchedulerModeSettingChanged(eRenderSchedulerModeTaskGraph);
    ASSERT_TRUE(waitForDryRunCount(++dryRuns));
    EXPECT_EQ(3, spy.count());
    EXPECT_FALSE(_viewer->getGpuPlacementSnapshot()->regions.empty());
}

TEST_P(ViewerDeviceRoot, WithNoGpuADryRunPublishesAnEmptySnapshot)
{
    const int firstDryRun = ViewerInstance::getGpuPlacementDryRunCountForTests() + 1;
    const PlacementOverlayScope scope(true, nullptr);

    ASSERT_TRUE(waitForDryRunCount(firstDryRun));

    const GpuPlacementSnapshotPtr snapshot = _viewer->getGpuPlacementSnapshot();
    ASSERT_TRUE(bool(snapshot));
    EXPECT_TRUE(snapshot->regions.empty());
}

TEST_P(ViewerDeviceRoot, PlaybackHoldsADryRunUntilItStops)
{
    const int firstDryRun = ViewerInstance::getGpuPlacementDryRunCountForTests() + 1;
    const PlacementOverlayScope scope(true, nullptr);
    ASSERT_TRUE(waitForDryRunCount(firstDryRun));

    ViewerInstance::setGpuPlacementFrozenForTests(true);
    _viewer->requestGpuPlacementDryRun();
    letThePlannerSettle();
    EXPECT_EQ(firstDryRun, ViewerInstance::getGpuPlacementDryRunCountForTests());

    ViewerInstance::setGpuPlacementFrozenForTests(false);
    EXPECT_TRUE(waitForDryRunCount(firstDryRun + 1));
}

TEST_P(ViewerDeviceRoot, TheSnapshotSignalFiresOncePerChange)
{
    if (!gpuOrSkip()) {
        return;
    }
    const SnapshotChangeSpy spy(_viewer);
    int dryRuns = ViewerInstance::getGpuPlacementDryRunCountForTests() + 1;
    const PlacementOverlayScope scope(true, &placementAlwaysAvailable);

    ASSERT_TRUE(waitForDryRunCount(dryRuns));
    EXPECT_EQ(1, spy.count());

    _viewer->requestGpuPlacementDryRun();
    ASSERT_TRUE(waitForDryRunCount(++dryRuns));
    EXPECT_EQ(1, spy.count());
    EXPECT_FALSE(_viewer->getGpuPlacementSnapshot()->regions.empty());

    setGpuPlacementAvailableFunc(nullptr);
    _viewer->requestGpuPlacementDryRun();
    ASSERT_TRUE(waitForDryRunCount(++dryRuns));
    EXPECT_EQ(2, spy.count());
    EXPECT_TRUE(_viewer->getGpuPlacementSnapshot()->regions.empty());
}

TEST_P(ViewerDeviceRoot, TheSnapshotSignalReachesAMainThreadReceiverQueued)
{
    QObject receiver;
    int deliveries = 0;
    bool deliveredOnReceiverThread = false;
    const QThread* const receiverThread = QThread::currentThread();
    QObject::connect(_viewer, &ViewerInstance::gpuPlacementSnapshotChanged, &receiver, [&]() {
        ++deliveries;
        deliveredOnReceiverThread = QThread::currentThread() == receiverThread;
    });

    const int firstDryRun = ViewerInstance::getGpuPlacementDryRunCountForTests() + 1;
    const PlacementOverlayScope scope(true, nullptr);
    ASSERT_TRUE(waitForDryRunCount(firstDryRun));
    EXPECT_EQ(0, deliveries);

    QCoreApplication::processEvents();
    EXPECT_EQ(1, deliveries);
    EXPECT_TRUE(deliveredOnReceiverThread);
}

TEST_P(ViewerDeviceRoot, PlaybackFreezesTheSnapshotAndItsSignalThenUpdatesOnce)
{
    if (!gpuOrSkip()) {
        return;
    }
    const SnapshotChangeSpy spy(_viewer);
    const int firstDryRun = ViewerInstance::getGpuPlacementDryRunCountForTests() + 1;
    const PlacementOverlayScope scope(true, &placementAlwaysAvailable);
    ASSERT_TRUE(waitForDryRunCount(firstDryRun));
    ASSERT_EQ(1, spy.count());
    const GpuPlacementSnapshotPtr beforePlayback = _viewer->getGpuPlacementSnapshot();
    ASSERT_FALSE(beforePlayback->regions.empty());

    ViewerInstance::setGpuPlacementFrozenForTests(true);
    setGpuPlacementAvailableFunc(nullptr);
    _viewer->requestGpuPlacementDryRun();
    ASSERT_EQ(ViewerInstance::eViewerRenderRetCodeRender, renderFrame(std::make_shared<RenderStats>(false)).ret);
    letThePlannerSettle();

    EXPECT_EQ(1, spy.count());
    EXPECT_EQ(beforePlayback.get(), _viewer->getGpuPlacementSnapshot().get());

    ViewerInstance::setGpuPlacementFrozenForTests(false);
    ASSERT_TRUE(waitForDryRunCount(firstDryRun + 1));
    letThePlannerSettle();

    EXPECT_EQ(2, spy.count());
    EXPECT_TRUE(_viewer->getGpuPlacementSnapshot()->regions.empty());
}

TEST_P(ViewerDeviceRoot, ASequentialRenderPlacesNoShadow)
{
    if (!gpuOrSkip()) {
        return;
    }
    const int firstDryRun = ViewerInstance::getGpuPlacementDryRunCountForTests() + 1;
    const PlacementOverlayScope scope(true, &placementAlwaysAvailable);
    ASSERT_TRUE(waitForDryRunCount(firstDryRun));
    const int shadowsBefore = ViewerInstance::getGpuShadowPlacementCountForTests();
    const GpuPlacementSnapshotPtr beforePlayback = _viewer->getGpuPlacementSnapshot();
    ViewerInstance::setGpuPlacementFrozenForTests(true);

    ASSERT_EQ(ViewerInstance::eViewerRenderRetCodeRender, renderFrame(std::make_shared<RenderStats>(false), true).ret);

    EXPECT_EQ(shadowsBefore, ViewerInstance::getGpuShadowPlacementCountForTests());
    EXPECT_EQ(beforePlayback.get(), _viewer->getGpuPlacementSnapshot().get());
}

INSTANTIATE_TEST_CASE_P(TexturesBitDepth, ViewerDeviceRoot, ::testing::Values(std::string("8u"), std::string("32f")));

#endif // NATRON_HAVE_GPU
