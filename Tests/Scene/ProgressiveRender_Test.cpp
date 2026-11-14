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

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include "Global/Macros.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "BaseTest.h"
#include "Engine/Nodes/Scene/CyclesAovs.h"
#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneStage.h"
#include "Tests/Scene/CyclesTestUtils.h"
#include "Tests/TestWatchdog.h"

NATRON_NAMESPACE_USING

using CyclesTestUtils::cyclesOrSkip;
using CyclesTestUtils::fixedSampling;

// Drives HydraRender::ProgressiveRender, the viewer's background refinement of a delegate other
// than Storm, on Cycles over Tests/fixtures/scene-cycles.usda seen through /World/Cam: /World/Near,
// a 2-unit cube at the origin, fills the centre of the view nine units from the camera.

namespace {
const char* const kFixture = NATRON_TESTS_FIXTURES_DIR "/scene-cycles.usda";
const char* const kCamera = "/World/Cam";
const int kSize = 32;
const int kSamples = 256;
const float kNearDistance = 9.f;
const std::chrono::seconds kTimeout(120);

typedef HydraRender::ProgressiveRender ProgressiveRender;

// The fixture as a USDRead node hands it on: a node layer sublayering the file and carrying its
// stage metadata.
ScenePayload
fixtureScene()
{
    PXR_NS::SdfLayerRefPtr file = PXR_NS::SdfLayer::FindOrOpen(kFixture);
    if (!file) {
        ADD_FAILURE() << "cannot open " << kFixture;

        return ScenePayload();
    }
    PXR_NS::SdfLayerRefPtr layer = SceneOps::createNodeLayer("ProgressiveRenderTest");
    const PXR_NS::SdfPath& root = PXR_NS::SdfPath::AbsoluteRootPath();
    PXR_NS::TfToken upAxis;
    if (file->HasField(root, PXR_NS::UsdGeomTokens->upAxis, &upAxis)) {
        layer->SetField(root, PXR_NS::UsdGeomTokens->upAxis, upAxis);
    }
    double metersPerUnit = 0.;
    if (file->HasField(root, PXR_NS::UsdGeomTokens->metersPerUnit, &metersPerUnit)) {
        layer->SetField(root, PXR_NS::UsdGeomTokens->metersPerUnit, metersPerUnit);
    }
    layer->SetTimeCodesPerSecond(file->GetTimeCodesPerSecond());
    layer->InsertSubLayerPath(file->GetIdentifier());

    return SceneOps::appendLayer(ScenePayload(), layer, 0x9e0a11ULL);
}

// base with /World/Near moved back along the view axis by back units, as a node downstream would.
ScenePayload
movedNear(const ScenePayload& base,
          double back,
          U64 hash)
{
    PXR_NS::SdfLayerRefPtr layer = SceneOps::createNodeLayer("ProgressiveRenderTestEdit");
    PXR_NS::SdfPrimSpecHandle prim = PXR_NS::SdfCreatePrimInLayer(layer, PXR_NS::SdfPath("/World/Near"));
    PXR_NS::SdfAttributeSpecHandle translate = PXR_NS::SdfAttributeSpec::New(prim, "xformOp:translate", PXR_NS::SdfValueTypeNames->Double3);
    translate->SetDefaultValue(PXR_NS::VtValue(PXR_NS::GfVec3d(0., 0., -back)));
    PXR_NS::SdfAttributeSpecHandle order = PXR_NS::SdfAttributeSpec::New(prim, "xformOpOrder", PXR_NS::SdfValueTypeNames->TokenArray, PXR_NS::SdfVariabilityUniform);
    order->SetDefaultValue(PXR_NS::VtValue(PXR_NS::VtTokenArray(1, PXR_NS::TfToken("xformOp:translate"))));

    return SceneOps::appendLayer(base, layer, hash);
}

// The window depth, in frustum's projection, of a point distance units along the view axis.
float
windowDepth(const PXR_NS::GfFrustum& frustum,
            double distance)
{
    const PXR_NS::GfMatrix4d projection = frustum.ComputeProjectionMatrix();
    const double z = -distance;
    const double clipZ = z * projection[2][2] + projection[3][2];
    const double clipW = z * projection[2][3] + projection[3][3];

    return (float)((clipZ / clipW + 1.) * 0.5);
}

float
centreDepth(const ProgressiveRender::Frame& frame)
{
    return frame.depth[(std::size_t)(frame.height / 2) * (std::size_t)frame.width + (std::size_t)(frame.width / 2)];
}
} // anon namespace

class ProgressiveRenderTest
    : public BaseTest {
protected:
    ProgressiveRenderTest()
        : _stage(new SceneStage)
    {
    }

    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        _watchdog.start();
        HydraRender::setRendererSettings(kRenderDelegateCyclesId, fixedSampling(kSamples));
    }

    virtual void TearDown() OVERRIDE
    {
        _watchdog.setPhase("teardown");
        hold(false);
        _render.reset();
        _stage.reset();
        HydraRender::setRendererSettings(kRenderDelegateCyclesId, std::map<std::string, PXR_NS::VtValue>());
        BaseTest::TearDown();
        _watchdog.stop();
    }

    // Syncs payload into the stage under the stage lock, and restarts the render if that edited
    // it, as the viewer's sync does.
    void sync(const ScenePayload& payload)
    {
        _watchdog.setPhase("syncScene");
        bool edited = false;
        {
            std::lock_guard<std::mutex> lock(_stageLock);
            edited = _stage->sync(payload);
        }
        if (edited && _render) {
            _render->invalidate();
        }
    }

    // A render of the stage, with nothing set yet, whose listener is heard().
    void newRender()
    {
        hold(false);
        _listened = nullptr;
        _render.reset();
        {
            std::lock_guard<std::mutex> lock(_signalMutex);
            _published.clear();
            _calls = 0;
        }
        SceneStage* stage = _stage.get();
        _render.reset(new ProgressiveRender(&_stageLock, [stage]() { return stage->getStage(); }, [this]() { heard(); }, 0.05f));
        _listened = _render.get();
    }

    // Starts a Cycles render of the fixture through its camera, kSize pixels square, with a render
    // settings prim's settings.
    bool start(const std::map<std::string, PXR_NS::VtValue>& settings = std::map<std::string, PXR_NS::VtValue>())
    {
        if (!_stage->isValid()) {
            ADD_FAILURE() << "OpenUSD could not open a stage";

            return false;
        }
        _base = fixtureScene();
        sync(_base);
        PXR_NS::UsdGeomCamera camera(_stage->getStage()->GetPrimAtPath(PXR_NS::SdfPath(kCamera)));
        if (!camera) {
            ADD_FAILURE() << "the fixture has no " << kCamera;

            return false;
        }
        _view.frustum = camera.GetCamera(PXR_NS::UsdTimeCode::Default()).GetFrustum();
        _view.width = kSize;
        _view.height = kSize;
        _view.time = PXR_NS::UsdTimeCode::Default();
        newRender();
        _render->setSettings(settings);
        _render->setRenderer(kRenderDelegateCyclesId);
        _render->setThreads([]() { return 2; });
        _render->setView(_view);

        return true;
    }

    // The listener: records the frame just published, and waits while the render is held.
    void heard()
    {
        ProgressiveRender* render = _listened.load();
        const std::shared_ptr<const ProgressiveRender::Frame> frame = render ? render->getFrame() : std::shared_ptr<const ProgressiveRender::Frame>();
        std::unique_lock<std::mutex> lock(_signalMutex);

        if (frame && (_published.empty() || (_published.back() != frame))) {
            _published.push_back(frame);
        }
        ++_calls;
        if (_hold) {
            _held = true;
            _signal.notify_all();
            _signal.wait(lock, [this]() { return !_hold; });
            _held = false;
        }
        _signal.notify_all();
    }

    // While on, the worker stops in the listener after its next frame, without a lock of the
    // render's or the stage's, until it is off again.
    void hold(bool on)
    {
        std::lock_guard<std::mutex> lock(_signalMutex);

        _hold = on;
        _signal.notify_all();
    }

    std::size_t publishedCount()
    {
        std::lock_guard<std::mutex> lock(_signalMutex);

        return _published.size();
    }

    int calls()
    {
        std::lock_guard<std::mutex> lock(_signalMutex);

        return _calls;
    }

    // Waits, under _signalMutex, until reached() holds, reading it again at each call of the
    // listener and at least every 10 ms for what the listener is not told of. False, with a
    // failure, if it did not hold within kTimeout.
    bool waitUntil(const std::function<bool()>& reached,
                   const char* what)
    {
        _watchdog.setPhase("render");
        const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now() + kTimeout;
        std::unique_lock<std::mutex> lock(_signalMutex);

        while (!reached()) {
            if (std::chrono::steady_clock::now() >= end) {
                ADD_FAILURE() << what << " did not happen in " << kTimeout.count() << " s";

                return false;
            }
            _signal.wait_for(lock, std::chrono::milliseconds(10));
        }

        return true;
    }

    // Waits until the render is done or fails, and gives the frames published from the from-th
    // on. True if it finished.
    bool collectUntilDone(std::size_t from,
                          std::vector<std::shared_ptr<const ProgressiveRender::Frame>>* frames)
    {
        std::string error;
        const bool finished = waitUntil([this, &error]() {
            const ProgressiveRender::Status status = _render->getStatus();
            if (!status.error.empty()) {
                error = status.error;

                return true;
            }
            const std::shared_ptr<const ProgressiveRender::Frame> frame = _render->getFrame();

            return status.converged && frame && frame->converged && (frame->restart == status.restarts) && !_published.empty() && (_published.back() == frame);
        },
                                        "the end of the render");
        {
            std::lock_guard<std::mutex> lock(_signalMutex);
            frames->assign(_published.begin() + (std::ptrdiff_t)std::min(from, _published.size()), _published.end());
        }
        if (!error.empty()) {
            ADD_FAILURE() << error;

            return false;
        }

        return finished;
    }

    std::mutex _stageLock;
    std::unique_ptr<SceneStage> _stage;
    TestWatchdog _watchdog;
    std::unique_ptr<ProgressiveRender> _render;
    std::atomic<ProgressiveRender*> _listened { nullptr };
    ProgressiveRender::View _view;
    ScenePayload _base;

    // Guards what the listener records, and the hold.
    std::mutex _signalMutex;
    std::condition_variable _signal;
    std::vector<std::shared_ptr<const ProgressiveRender::Frame>> _published;
    int _calls = 0;
    bool _hold = false;
    bool _held = false;
};

// The render refines in the background: each frame published has at least the samples of the one
// before, the last has the cap and is converged, and nothing is published once it is done.
TEST_F(ProgressiveRenderTest, RefinesToTheCapAndStops)
{
    if (!cyclesOrSkip()) {
        return;
    }
    const int liveBefore = ProgressiveRender::getLiveCount();
    ASSERT_TRUE(start());
    std::vector<std::shared_ptr<const ProgressiveRender::Frame>> frames;
    ASSERT_TRUE(collectUntilDone(0, &frames));
    ASSERT_FALSE(frames.empty());

    EXPECT_EQ(kSamples, _render->getMaxSamples());
    std::cout << "[   NOTE   ] samples per frame:";
    for (std::size_t i = 0; i < frames.size(); ++i) {
        std::cout << " " << frames[i]->samples;
        EXPECT_GT(frames[i]->samples, 0) << "frame " << i;
        EXPECT_EQ(kSize, frames[i]->width);
        EXPECT_EQ(kSize, frames[i]->height);
        EXPECT_EQ((std::size_t)kSize * kSize * 4, frames[i]->rgba.size());
        EXPECT_EQ((std::size_t)kSize * kSize, frames[i]->depth.size());
        EXPECT_EQ(U64(1), frames[i]->restart);
        if (i > 0) {
            EXPECT_GE(frames[i]->samples, frames[i - 1]->samples) << "frame " << i;
            EXPECT_GT(frames[i]->serial, frames[i - 1]->serial);
        }
        if (i + 1 < frames.size()) {
            EXPECT_FALSE(frames[i]->converged) << "frame " << i;
        }
    }
    std::cout << std::endl;
    const ProgressiveRender::Frame& last = *frames.back();
    EXPECT_EQ(kSamples, last.samples);
    EXPECT_EQ(kSamples, last.maxSamples);
    EXPECT_TRUE(last.converged);
    // The centre shows the near cube, in front of the far plane, and is lit.
    EXPECT_NEAR(windowDepth(_view.frustum, kNearDistance), centreDepth(last), 1e-4f);
    const std::size_t centre = ((std::size_t)(kSize / 2) * kSize + kSize / 2) * 4;
    EXPECT_GT(last.rgba[centre] + last.rgba[centre + 1] + last.rgba[centre + 2], 3 * 20);

    const ProgressiveRender::Status status = _render->getStatus();
    EXPECT_TRUE(status.active);
    EXPECT_TRUE(status.converged);
    EXPECT_EQ(kSamples, status.samples);
    EXPECT_EQ(U64(1), status.restarts);
    // Nothing comes later: a frame would, were the render still under way.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_EQ(status.frames, _render->getStatus().frames) << "frames were published after the render was done";
    EXPECT_EQ(last.serial, _render->getFrame()->serial);

    EXPECT_EQ(liveBefore + 1, ProgressiveRender::getLiveCount());
    _render.reset();
    EXPECT_EQ(liveBefore, ProgressiveRender::getLiveCount());
}

// Ten edits while the render refines restart it once, and no frame of the scene before them is
// published after them: every later frame shows the near cube where the last edit put it. The
// render is held in its listener while the edits come, so that it cannot restart between them.
TEST_F(ProgressiveRenderTest, CoalescesEditsIntoOneRestart)
{
    if (!cyclesOrSkip()) {
        return;
    }
    ASSERT_TRUE(start());
    std::vector<std::shared_ptr<const ProgressiveRender::Frame>> frames;
    ASSERT_TRUE(collectUntilDone(0, &frames));
    const U64 restartsBefore = _render->getRestartCount();
    // A render under way, as when edits come while it refines: held at its first frame.
    hold(true);
    const std::size_t mark = publishedCount();
    _render->setMaxSamples(kSamples * 2);
    ASSERT_TRUE(waitUntil([this]() { return _held; }, "a frame of the raised cap"));
    const std::shared_ptr<const ProgressiveRender::Frame> held = _render->getFrame();
    ASSERT_TRUE(held != nullptr);
    const ProgressiveRender::Status refining = _render->getStatus();
    const U64 restartsRefining = refining.restarts;
    EXPECT_GT(restartsRefining, restartsBefore);
    EXPECT_EQ(restartsRefining, held->restart);

    const int kEdits = 10;
    const double kLastBack = 5.;
    for (int i = 1; i <= kEdits; ++i) {
        sync(movedNear(_base, kLastBack * i / kEdits, 0xed17000ULL + (U64)i));
    }
    hold(false);
    std::vector<std::shared_ptr<const ProgressiveRender::Frame>> after;
    ASSERT_TRUE(collectUntilDone(mark, &after));
    ASSERT_FALSE(after.empty());
    EXPECT_EQ(held, after.front());

    const ProgressiveRender::Status end = _render->getStatus();
    const U64 restartsAfter = end.restarts;
    // The worker makes its engine again when Cycles is slow to show the edits' restart, a
    // restart of its own that no edit caused.
    const U64 remade = end.engines - refining.engines;
    EXPECT_EQ(restartsRefining + 1 + remade, restartsAfter) << remade << " engine(s) made after the edits";
    const float moved = windowDepth(_view.frustum, kNearDistance + kLastBack);
    const float before = windowDepth(_view.frustum, kNearDistance);
    for (const std::shared_ptr<const ProgressiveRender::Frame>& frame : frames) {
        EXPECT_NEAR(before, centreDepth(*frame), 1e-4f) << "frame " << frame->serial;
    }
    EXPECT_NEAR(before, centreDepth(*held), 1e-4f) << "frame " << held->serial;
    U64 lastRestart = restartsRefining + 1;
    for (const std::shared_ptr<const ProgressiveRender::Frame>& frame : after) {
        if (frame->serial <= held->serial) {
            continue;
        }
        EXPECT_LE(lastRestart, frame->restart) << "frame " << frame->serial;
        EXPECT_GE(restartsAfter, frame->restart) << "frame " << frame->serial;
        lastRestart = frame->restart;
        EXPECT_NEAR(moved, centreDepth(*frame), 1e-4f) << "frame " << frame->serial << " (restart " << frame->restart << ") is stale";
    }
    EXPECT_EQ(kSamples * 2, after.back()->samples);
}

// Stopping pauses the delegate where it is; resuming continues from its samples, without a
// restart.
TEST_F(ProgressiveRenderTest, StopKeepsTheSamplesAndResumeContinues)
{
    if (!cyclesOrSkip()) {
        return;
    }
    HydraRender::setRendererSettings(kRenderDelegateCyclesId, fixedSampling(1000000));
    ASSERT_TRUE(start());
    ASSERT_TRUE(waitUntil([this]() {
        const ProgressiveRender::Status status = _render->getStatus();

        return !status.error.empty() || (status.samples >= 2);
    },
                          "a second sample"));
    ASSERT_TRUE(_render->getStatus().error.empty()) << _render->getStatus().error;
    _render->stop();
    ASSERT_TRUE(waitUntil([this]() { return _render->getStatus().paused; }, "the pause"));
    const ProgressiveRender::Status stopped = _render->getStatus();
    EXPECT_TRUE(stopped.stopped);
    // Nothing moves while it is paused.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const ProgressiveRender::Status still = _render->getStatus();
    EXPECT_TRUE(still.paused);
    EXPECT_EQ(stopped.samples, still.samples);
    EXPECT_EQ(stopped.frames, still.frames);

    _render->resume();
    ASSERT_TRUE(waitUntil([this, &still]() {
        const ProgressiveRender::Status status = _render->getStatus();

        return !status.error.empty() || (!status.paused && (status.samples > still.samples));
    },
                          "a sample after the resume"));
    const ProgressiveRender::Status resumed = _render->getStatus();
    EXPECT_TRUE(resumed.error.empty()) << resumed.error;
    EXPECT_FALSE(resumed.stopped);
    EXPECT_GT(resumed.samples, still.samples);
    EXPECT_EQ(still.restarts, resumed.restarts);
}

// A delegate that cannot start fails the render: the status says why, and the listener is called,
// so that a viewer waiting for a frame repaints to show the error.
TEST_F(ProgressiveRenderTest, AFailureReachesTheListener)
{
    if (!cyclesOrSkip()) {
        return;
    }
    ASSERT_TRUE(start());
    newRender();
    _render->setRenderer("NatronNoSuchRenderDelegate");
    _render->setView(_view);
    EXPECT_TRUE(waitUntil([this]() { return _calls > 0; }, "a call of the listener"));
    const ProgressiveRender::Status status = _render->getStatus();
    EXPECT_FALSE(status.error.empty());
    EXPECT_FALSE(status.active);
    EXPECT_TRUE(_render->getFrame() == nullptr);
    // It does not retry: no further call comes.
    const int after = calls();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(after, calls());
    _render.reset();
}

// A delegate can throw from any call on the worker, not only Render(): the render fails, the process
// lives, nothing retries in a loop, and an edit starts it again. threads is the one thing a test can
// make throw there, both before there is an engine and with one rendering.
TEST_F(ProgressiveRenderTest, AThrowOutsideTheRenderFailsItAndAnEditRecovers)
{
    if (!cyclesOrSkip()) {
        return;
    }
    ASSERT_TRUE(start());
    std::atomic<bool> throwing(true);
    newRender();
    _render->setThreads([&throwing]() -> int {
        if (throwing.load()) {
            throw std::runtime_error("delegate broke");
        }

        return 2;
    });
    _render->setRenderer(kRenderDelegateCyclesId);
    _render->setView(_view);
    ASSERT_TRUE(waitUntil([this]() { return _calls > 0; }, "a call of the listener"));
    EXPECT_NE(std::string::npos, _render->getStatus().error.find("delegate broke")) << _render->getStatus().error;
    EXPECT_FALSE(_render->getStatus().active);
    const int after = calls();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(after, calls());

    throwing = false;
    const std::size_t mark = publishedCount();
    _render->invalidate();
    EXPECT_TRUE(waitUntil([this]() { return _render->getStatus().error.empty(); }, "the recovery"));
    std::vector<std::shared_ptr<const ProgressiveRender::Frame>> frames;
    EXPECT_TRUE(collectUntilDone(mark, &frames));

    throwing = true;
    _render->invalidate();
    EXPECT_TRUE(waitUntil([this]() { return !_render->getStatus().error.empty(); }, "the second failure"));
    EXPECT_NE(std::string::npos, _render->getStatus().error.find("delegate broke")) << _render->getStatus().error;
    EXPECT_FALSE(_render->getStatus().active);
    throwing = false;
    _render.reset();
}

// The pass that fails is edited while it runs: its failure still counts, though what it rendered
// would be stale. The edit is a move of the view, after which the failure stays.
TEST_F(ProgressiveRenderTest, AnEditDuringAFailingPassDoesNotHideTheFailure)
{
    if (!cyclesOrSkip()) {
        return;
    }
    ASSERT_TRUE(start());
    std::atomic<bool> armed(true);
    newRender();
    ProgressiveRender::View moved = _view;
    moved.width += 2;
    RenderDelegates::setRenderFaultHook([this, &armed, moved]() {
        if (armed.exchange(false)) {
            _render->setView(moved);
            throw std::runtime_error("texture commit failed");
        }
    });
    _render->setRenderer(kRenderDelegateCyclesId);
    _render->setThreads([]() { return 2; });
    _render->setView(_view);
    EXPECT_TRUE(waitUntil([this]() { return _calls > 0; }, "a call of the listener"));
    EXPECT_FALSE(armed.load());
    EXPECT_NE(std::string::npos, _render->getStatus().error.find("texture commit failed")) << _render->getStatus().error;
    _render.reset();
    RenderDelegates::setRenderFaultHook(nullptr);
}

// One throw does not stop the rendering for good: the next scene or settings edit builds an engine
// that renders, while moving the view, which cannot be what the delegate failed on, does not.
TEST_F(ProgressiveRenderTest, AFailureRetriesAtTheNextSceneEditAndNotAtAMove)
{
    if (!cyclesOrSkip()) {
        return;
    }
    ASSERT_TRUE(start());
    std::atomic<bool> armed(true);
    RenderDelegates::setRenderFaultHook([&armed]() {
        if (armed.exchange(false)) {
            throw std::runtime_error("texture commit failed");
        }
    });
    newRender();
    _render->setRenderer(kRenderDelegateCyclesId);
    _render->setThreads([]() { return 2; });
    _render->setView(_view);
    ASSERT_TRUE(waitUntil([this]() { return _calls > 0; }, "a call of the listener"));
    EXPECT_FALSE(_render->getStatus().error.empty());
    const std::uint64_t builds = HydraRender::getEngineBuildCount();

    ProgressiveRender::View moved = _view;
    moved.width += 2;
    _render->setView(moved);
    // No engine is built for the move.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(builds, HydraRender::getEngineBuildCount());
    EXPECT_FALSE(_render->getStatus().error.empty());

    _render->setView(_view);
    const std::size_t mark = publishedCount();
    _render->invalidate();
    EXPECT_TRUE(waitUntil([this]() { return _render->getStatus().error.empty(); }, "the retry"));
    std::vector<std::shared_ptr<const ProgressiveRender::Frame>> frames;
    EXPECT_TRUE(collectUntilDone(mark, &frames));
    EXPECT_FALSE(frames.empty());
    EXPECT_GT(HydraRender::getEngineBuildCount(), builds);
    _render.reset();
    RenderDelegates::setRenderFaultHook(nullptr);
}

// An exception out of Hydra's Render() on the worker fails the render as one that cannot start,
// and choosing the delegate again builds a fresh engine that renders.
TEST_F(ProgressiveRenderTest, AnExceptionFromHydraFailsTheRenderAndALaterRenderRecovers)
{
    if (!cyclesOrSkip()) {
        return;
    }
    ASSERT_TRUE(start());
    RenderDelegates::setRenderFaultHook([]() { throw std::runtime_error("texture commit failed"); });
    newRender();
    _render->setRenderer(kRenderDelegateCyclesId);
    _render->setThreads([]() { return 2; });
    _render->setView(_view);
    EXPECT_TRUE(waitUntil([this]() { return _calls > 0; }, "a call of the listener"));
    RenderDelegates::setRenderFaultHook(nullptr);
    const ProgressiveRender::Status failed = _render->getStatus();
    EXPECT_NE(std::string::npos, failed.error.find("texture commit failed")) << failed.error;
    EXPECT_FALSE(failed.active);

    const std::size_t mark = publishedCount();
    _render->setRenderer(std::string());
    _render->setRenderer(kRenderDelegateCyclesId);
    std::vector<std::shared_ptr<const ProgressiveRender::Frame>> frames;
    EXPECT_TRUE(collectUntilDone(mark, &frames));
    EXPECT_FALSE(frames.empty());
    _render.reset();
}

// Every Cycles session of the process shares one thread count, the one asked for last: a second
// viewer asking for another has the first one's engine made again under it, and both refine.
TEST_F(ProgressiveRenderTest, CyclesSessionsShareTheLastCountAskedFor)
{
    if (!cyclesOrSkip()) {
        return;
    }
    ASSERT_TRUE(start());
    std::vector<std::shared_ptr<const ProgressiveRender::Frame>> frames;
    ASSERT_TRUE(collectUntilDone(0, &frames));
    EXPECT_EQ(2, HydraRender::getCyclesThreads());
    const U64 restartsBefore = _render->getRestartCount();
    const std::uint64_t buildsBefore = HydraRender::getEngineBuildCount();

    SceneStage* stage = _stage.get();
    std::unique_ptr<ProgressiveRender> second(new ProgressiveRender(&_stageLock, [stage]() { return stage->getStage(); }, std::function<void()>(), 0.05f));
    second->setRenderer(kRenderDelegateCyclesId);
    second->setThreads([]() { return 3; });
    second->setView(_view);
    std::string error;
    ASSERT_TRUE(waitUntil([this, &second, &error, restartsBefore]() {
        const ProgressiveRender::Status a = _render->getStatus();
        const ProgressiveRender::Status b = second->getStatus();
        if (!a.error.empty() || !b.error.empty()) {
            error = a.error + b.error;

            return true;
        }
        const std::shared_ptr<const ProgressiveRender::Frame> frameA = _render->getFrame();
        const std::shared_ptr<const ProgressiveRender::Frame> frameB = second->getFrame();
        const bool doneA = a.converged && frameA && frameA->converged && (frameA->restart == a.restarts) && (a.restarts > restartsBefore);
        const bool doneB = b.converged && frameB && frameB->converged && (frameB->restart == b.restarts);

        return doneA && doneB;
    },
                          "the end of both renders"));
    ASSERT_TRUE(error.empty()) << error;
    EXPECT_EQ(3, HydraRender::getCyclesThreads());
    EXPECT_GE(HydraRender::getEngineBuildCount(), buildsBefore + 2) << "the first viewer's engine was not made again";
    EXPECT_EQ(kSamples, _render->getFrame()->samples);
    EXPECT_EQ(kSamples, second->getFrame()->samples);
    second.reset();
}

// A render settings prim's settings are set on the engine: its cycles:samples is the cap the
// render refines to, a sample cap set applies over it, the same settings again restart nothing,
// and others make the engine again under them.
TEST_F(ProgressiveRenderTest, ThePrimsSettingsAreTheEnginesAndTheCapAppliesOverThem)
{
    if (!cyclesOrSkip()) {
        return;
    }
    // The fixed sampling without its samples, which would override the prim's.
    std::map<std::string, PXR_NS::VtValue> fixed = fixedSampling(kSamples);
    fixed.erase("cycles:samples");
    HydraRender::setRendererSettings(kRenderDelegateCyclesId, fixed);
    std::map<std::string, PXR_NS::VtValue> prim;
    prim["cycles:samples"] = PXR_NS::VtValue(12);
    ASSERT_TRUE(start(prim));
    std::vector<std::shared_ptr<const ProgressiveRender::Frame>> frames;
    ASSERT_TRUE(collectUntilDone(0, &frames));
    EXPECT_EQ(12, _render->getMaxSamples());
    EXPECT_EQ(12, frames.back()->samples);

    std::size_t mark = publishedCount();
    _render->setMaxSamples(5);
    ASSERT_TRUE(collectUntilDone(mark, &frames));
    EXPECT_EQ(5, _render->getMaxSamples());
    EXPECT_EQ(5, frames.back()->samples);

    const U64 restarts = _render->getRestartCount();
    const U64 builds = HydraRender::getEngineBuildCount();
    _render->setSettings(prim);
    // The same settings: nothing restarts, now or a little later.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(restarts, _render->getRestartCount());
    EXPECT_EQ(builds, HydraRender::getEngineBuildCount());

    mark = publishedCount();
    _render->setMaxSamples(0);
    prim["cycles:samples"] = PXR_NS::VtValue(7);
    _render->setSettings(prim);
    ASSERT_TRUE(collectUntilDone(mark, &frames));
    EXPECT_EQ(7, _render->getMaxSamples());
    EXPECT_EQ(7, frames.back()->samples);
    EXPECT_GT(HydraRender::getEngineBuildCount(), builds);
}
