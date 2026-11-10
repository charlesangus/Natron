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

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QThread>

#include "BaseTest.h"
#include "CountingTestEffect.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/Node.h"
#include "Engine/OpenGLViewerI.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/OutputSchedulerThread.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/Settings.h"
#include "Engine/TimeLine.h"
#include "Engine/UpdateViewerParams.h"
#include "Engine/ViewIdx.h"
#include "Engine/ViewerInstance.h"

NATRON_NAMESPACE_USING

namespace {

const int kSize = 64;
const int kPlaybackFrames = 1000;
const qint64 kAbortBudgetMs = 500;
// A render that never sees the abort ends on its own after this, so a missed abort shows as a latency over the budget.
const qint64 kSlowRenderMs = 15000;
const qint64 kStartGuardMs = 20000;
const int kWatchdogMs = 60000;

// Aborts the whole test binary when a wait does not return in time: the thread stuck in a blocking wait cannot fail an
// EXPECT itself.
class Watchdog {
public:
    explicit Watchdog(const char* what)
        : _mutex()
        , _cond()
        , _done(false)
        , _thread([this, what]() {
            std::unique_lock<std::mutex> lock(_mutex);
            if (!_cond.wait_for(lock, std::chrono::milliseconds(kWatchdogMs), [this]() { return _done; })) {
                std::fprintf(stderr, "SchedulerAbortLatency: %s did not return within %d ms\n", what, kWatchdogMs);
                std::fflush(stderr);
                std::abort();
            }
        })
    {
    }

    ~Watchdog()
    {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _done = true;
        }
        _cond.notify_all();
        _thread.join();
    }

    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;

private:
    std::mutex _mutex;
    std::condition_variable _cond;
    bool _done;
    std::thread _thread;
};

// Aborts the engine and waits for the abort on a thread of its own, timing the two together.
class AbortProbe {
public:
    AbortProbe()
        : _latencyMs(-1)
        , _done(false)
    {
    }

    ~AbortProbe()
    {
        join();
    }

    void start(const RenderEnginePtr& engine)
    {
        _thread = std::thread([this, engine]() {
            QElapsedTimer clock;
            clock.start();
            engine->abortRenderingNoRestart();
            engine->waitForAbortToComplete_not_main_thread();
            _latencyMs = clock.elapsed();
            _done = true;
        });
    }

    bool done() const { return _done.load(); }
    qint64 latencyMs() const { return _latencyMs.load(); }

    void join()
    {
        if (_thread.joinable()) {
            _thread.join();
        }
    }

    AbortProbe(const AbortProbe&) = delete;
    AbortProbe& operator=(const AbortProbe&) = delete;

private:
    std::atomic<qint64> _latencyMs;
    std::atomic<bool> _done;
    std::thread _thread;
};

// The viewer widget as the render thread sees it: the whole frame at 100%, in tiles of the size the viewer cache was set
// up for. It holds no textures.
class AbortFakeViewerUi
    : public OpenGLViewerI {
public:
    bool isViewerUIVisible() const OVERRIDE { return true; }
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
        *last = kPlaybackFrames;
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
};

} // namespace

class SchedulerAbortLatency
    : public BaseTest {
protected:
    SchedulerAbortLatency()
        : BaseTest()
        , _rendering(false)
        , _probeStarted(false)
        , _probeInHook(false)
        , _savedMode(eRenderSchedulerModeLegacy)
        , _viewer(0)
    {
    }

    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        _savedMode = appPTR->getRenderSchedulerMode();
        appPTR->setRenderSchedulerMode(eRenderSchedulerModeTaskGraph);
        CountingTestRegistry::reset();
        ProjectPtr project = getApp()->getProject();
        project->reset(false, true);
        project->setOrAddProjectFormat(Format(0, 0, kSize, kSize, "schedulerAbortLatency", 1.));
        _slow = createNode(QString::fromUtf8(kTestPluginIDCounting));
        ASSERT_TRUE(bool(_slow));
    }

    virtual void TearDown() OVERRIDE
    {
        CountingTestRegistry::reset();
        if (_viewer) {
            _viewer->invalidateUiContext();
        }
        _viewer = 0;
        _viewerNode.reset();
        _slow.reset();
        getApp()->getProject()->reset(false, true);
        appPTR->setRenderSchedulerMode(_savedMode);
        BaseTest::TearDown();
    }

    // Every render of the slow effect holds its thread until the render is aborted. With probeEngine set, the first one
    // also starts the abort from a thread of its own, so the abort lands while the render is in progress.
    void installSlowRender(const RenderEnginePtr& probeEngine = RenderEnginePtr())
    {
        _probeEngine = probeEngine;
        _probeInHook = bool(probeEngine);
        CountingTestRegistry::setRenderHook([this](const Node* node) {
            _rendering = true;
            if (_probeInHook && !_probeStarted.exchange(true)) {
                _probe.start(_probeEngine);
            }
            QElapsedTimer clock;
            clock.start();
            while (!node->aborted() && (clock.elapsed() < kSlowRenderMs)) {
                QThread::msleep(2);
            }
        });
    }

    bool createViewer()
    {
        CreateNodeArgs viewerArgs(PLUGINID_NATRON_VIEWER, getApp()->getProject());
        viewerArgs.setProperty<bool>(kCreateNodeArgsPropAllowNonUserCreatablePlugins, true);
        _viewerNode = getApp()->createNode(viewerArgs);
        if (!_viewerNode) {
            return false;
        }
        _viewer = _viewerNode->isEffectViewer();
        if (!_viewer) {
            return false;
        }
        _ui.timeline = getApp()->getTimeLine();
        _viewer->setUiContext(&_ui);
        connectNodes(_slow, _viewerNode, 0, true);

        return true;
    }

    // Keeps the main thread's event loop running, as the schedulers' main-thread requests need.
    static bool pumpUntil(const std::function<bool()>& done,
                          qint64 timeoutMs)
    {
        QElapsedTimer clock;
        clock.start();
        while (!done()) {
            if (clock.elapsed() > timeoutMs) {
                return false;
            }
            QCoreApplication::processEvents();
            QThread::msleep(2);
        }

        return true;
    }

    // The render is in progress: aborts it and waits for the abort while the main thread keeps its event loop going.
    void expectAbortWithinBudget(const RenderEnginePtr& engine)
    {
        ASSERT_TRUE(pumpUntil([this]() { return _rendering.load(); }, kStartGuardMs)) << "the slow render never started";
        {
            Watchdog watchdog("the abort wait");
            _probe.start(engine);
            pumpUntil([this]() { return _probe.done(); }, 2 * kWatchdogMs);
            _probe.join();
        }
        EXPECT_LT(_probe.latencyMs(), kAbortBudgetMs);
        pumpUntil([]() { return false; }, 50);
    }

    NodePtr _slow;
    NodePtr _viewerNode;
    std::atomic<bool> _rendering;
    std::atomic<bool> _probeStarted;
    bool _probeInHook;
    RenderEnginePtr _probeEngine;
    AbortProbe _probe;
    RenderSchedulerModeEnum _savedMode;
    ViewerInstance* _viewer;
    AbortFakeViewerUi _ui;
};

// A blocking render of the writer's DefaultScheduler, aborted from inside the render of its first frame.
TEST_F(SchedulerAbortLatency, WriterAbortReturnsWithinBudget)
{
    NodePtr writer = createNativeWriter();
    ASSERT_TRUE(bool(writer));
    connectNodes(_slow, writer, 0, true);
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    writer->setOutputFilesForWriter((tmp.path() + QString::fromUtf8("/abort.####.exr")).toStdString());
    OutputEffectInstance* writerEffect = dynamic_cast<OutputEffectInstance*>(writer->getEffectInstance().get());
    ASSERT_TRUE(writerEffect != NULL);

    installSlowRender(writerEffect->getRenderEngine());

    std::list<AppInstance::RenderWork> works;
    works.push_back(AppInstance::RenderWork(writerEffect, 1, 50, 1, false));
    {
        Watchdog watchdog("the blocking writer render");
        getApp()->startWritersRendering(eRenderModeBlocking, works);
    }
    _probe.join();

    ASSERT_TRUE(_probe.done());
    EXPECT_LT(_probe.latencyMs(), kAbortBudgetMs);
}

// Playback of the viewer's display scheduler, aborted while its frame renders are in progress.
TEST_F(SchedulerAbortLatency, ViewerPlaybackAbortReturnsWithinBudget)
{
    ASSERT_TRUE(createViewer());
    installSlowRender();
    RenderEnginePtr engine = _viewer->getRenderEngine();
    ASSERT_TRUE(bool(engine));

    engine->renderFromCurrentFrame(false, std::vector<ViewIdx>(1, ViewIdx(0)), eRenderDirectionForward);
    expectAbortWithinBudget(engine);
}

// The current-frame scheduler, whose render runs on its backup thread in Task graph mode, aborted mid-render.
TEST_F(SchedulerAbortLatency, ViewerCurrentFrameAbortReturnsWithinBudget)
{
    ASSERT_TRUE(createViewer());
    installSlowRender();
    RenderEnginePtr engine = _viewer->getRenderEngine();
    ASSERT_TRUE(bool(engine));

    _viewer->renderCurrentFrame(true);
    expectAbortWithinBudget(engine);
}
