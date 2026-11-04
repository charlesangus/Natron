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
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "BaseTest.h"
#include "Engine/Nodes/Scene/CyclesAovs.h"
#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneStage.h"
#include "Tests/Scene/CyclesTestUtils.h"

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
        HydraRender::setRendererSettings(kRenderDelegateCyclesId, fixedSampling(kSamples));
    }

    virtual void TearDown() OVERRIDE
    {
        _render.reset();
        _stage.reset();
        HydraRender::setRendererSettings(kRenderDelegateCyclesId, std::map<std::string, PXR_NS::VtValue>());
        BaseTest::TearDown();
    }

    // Syncs payload into the stage under the stage lock, and restarts the render if that edited
    // it, as the viewer's sync does.
    void sync(const ScenePayload& payload)
    {
        bool edited = false;
        {
            std::lock_guard<std::mutex> lock(_stageLock);
            edited = _stage->sync(payload);
        }
        if (edited && _render) {
            _render->invalidate();
        }
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
        SceneStage* stage = _stage.get();
        _render.reset(new ProgressiveRender(&_stageLock, [stage]() { return stage->getStage(); }, std::function<void()>(), 0.05f));
        _render->setSettings(settings);
        _render->setRenderer(kRenderDelegateCyclesId);
        _render->setThreads([]() { return 2; });
        _render->setView(_view);

        return true;
    }

    // Records each frame published, polling, until the render is done, it fails, or the time is up.
    // True if it finished.
    bool collectUntilDone(std::vector<std::shared_ptr<const ProgressiveRender::Frame>>* frames)
    {
        const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now() + kTimeout;
        while (std::chrono::steady_clock::now() < end) {
            record(frames);
            const ProgressiveRender::Status status = _render->getStatus();
            if (!status.error.empty()) {
                ADD_FAILURE() << status.error;

                return false;
            }
            if (status.converged && !frames->empty() && frames->back()->converged && (frames->back()->restart == status.restarts) && (status.samples == frames->back()->samples)) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        ADD_FAILURE() << "the render did not finish in " << kTimeout.count() << " s";

        return false;
    }

    void record(std::vector<std::shared_ptr<const ProgressiveRender::Frame>>* frames)
    {
        std::shared_ptr<const ProgressiveRender::Frame> frame = _render->getFrame();
        if (frame && (frames->empty() || (frames->back()->serial != frame->serial))) {
            frames->push_back(frame);
        }
    }

    std::mutex _stageLock;
    std::unique_ptr<SceneStage> _stage;
    std::unique_ptr<ProgressiveRender> _render;
    ProgressiveRender::View _view;
    ScenePayload _base;
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
    ASSERT_TRUE(collectUntilDone(&frames));
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
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_EQ(status.frames, _render->getStatus().frames) << "frames were published after the render was done";
    EXPECT_EQ(last.serial, _render->getFrame()->serial);

    EXPECT_EQ(liveBefore + 1, ProgressiveRender::getLiveCount());
    _render.reset();
    EXPECT_EQ(liveBefore, ProgressiveRender::getLiveCount());
}

// Ten edits in about 50 ms, while the render refines, restart it once, and no frame of the scene
// before them is published after them: every later frame shows the near cube where the last edit
// put it.
TEST_F(ProgressiveRenderTest, CoalescesEditsIntoOneRestart)
{
    if (!cyclesOrSkip()) {
        return;
    }
    ASSERT_TRUE(start());
    std::vector<std::shared_ptr<const ProgressiveRender::Frame>> frames;
    ASSERT_TRUE(collectUntilDone(&frames));
    const U64 restartsBefore = _render->getRestartCount();
    // A render under way, as when edits come while it refines.
    _render->setMaxSamples(kSamples * 64);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const U64 restartsRefining = _render->getRestartCount();
    EXPECT_EQ(restartsBefore + 1, restartsRefining);
    record(&frames);

    const int kEdits = 10;
    const double kLastBack = 5.;
    const std::chrono::steady_clock::time_point burstStart = std::chrono::steady_clock::now();
    for (int i = 1; i <= kEdits; ++i) {
        sync(movedNear(_base, kLastBack * i / kEdits, 0xed17000ULL + (U64)i));
        record(&frames);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const double burstMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - burstStart).count();
    std::cout << "[   NOTE   ] " << kEdits << " edits took " << burstMs << " ms" << std::endl;
    const U64 serialAfterBurst = frames.back()->serial;
    // Frames of the edited scene, for a second.
    const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now() + kTimeout;
    std::chrono::steady_clock::time_point firstNew = end;
    while (std::chrono::steady_clock::now() < std::min(end, firstNew + std::chrono::seconds(1))) {
        record(&frames);
        if ((firstNew == end) && (frames.back()->serial > serialAfterBurst)) {
            firstNew = std::chrono::steady_clock::now();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    _render->stop();
    ASSERT_GT(frames.back()->serial, serialAfterBurst) << "no frame of the edited scene in " << kTimeout.count() << " s";

    const U64 restartsAfter = _render->getRestartCount();
    // The edits are one restart while each comes within the restart's quiet time of the last and
    // the burst is shorter than the longest a restart waits.
    if (burstMs < 90.) {
        EXPECT_EQ(restartsRefining + 1, restartsAfter);
    } else {
        std::cout << "[   NOTE   ] the edits outlasted the longest a restart waits; checking a bound" << std::endl;
        EXPECT_LE(restartsAfter, restartsRefining + 1 + (U64)(burstMs / 100.));
    }
    const float moved = windowDepth(_view.frustum, kNearDistance + kLastBack);
    const float before = windowDepth(_view.frustum, kNearDistance);
    for (const std::shared_ptr<const ProgressiveRender::Frame>& frame : frames) {
        if ((frame->serial > serialAfterBurst) || (frame->restart > restartsRefining)) {
            EXPECT_GT(frame->restart, restartsRefining) << "frame " << frame->serial << " is of the render before the edits";
            if (frame->serial > serialAfterBurst) {
                EXPECT_EQ(restartsAfter, frame->restart) << "frame " << frame->serial;
                EXPECT_NEAR(moved, centreDepth(*frame), 1e-4f) << "frame " << frame->serial << " (restart " << frame->restart << ") is stale";
            }
        } else {
            EXPECT_NEAR(before, centreDepth(*frame), 1e-4f) << "frame " << frame->serial;
        }
    }
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
    const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now() + kTimeout;
    while ((_render->getStatus().samples < 2) && (std::chrono::steady_clock::now() < end)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_GE(_render->getStatus().samples, 2);
    _render->stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const ProgressiveRender::Status stopped = _render->getStatus();
    EXPECT_TRUE(stopped.stopped);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const ProgressiveRender::Status still = _render->getStatus();
    EXPECT_EQ(stopped.samples, still.samples);
    EXPECT_EQ(stopped.frames, still.frames);

    _render->resume();
    while ((_render->getStatus().samples <= still.samples) && (std::chrono::steady_clock::now() < end)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const ProgressiveRender::Status resumed = _render->getStatus();
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
    _render.reset();
    std::atomic<int> calls(0);
    SceneStage* stage = _stage.get();
    _render.reset(new ProgressiveRender(&_stageLock, [stage]() { return stage->getStage(); }, [&calls]() { ++calls; }, 0.05f));
    _render->setRenderer("NatronNoSuchRenderDelegate");
    _render->setView(_view);
    const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now() + kTimeout;
    while ((calls.load() == 0) && (std::chrono::steady_clock::now() < end)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_GT(calls.load(), 0) << "the listener was not called";
    const ProgressiveRender::Status status = _render->getStatus();
    EXPECT_FALSE(status.error.empty());
    EXPECT_FALSE(status.active);
    EXPECT_TRUE(_render->getFrame() == nullptr);
    // It does not retry: no further call comes.
    const int after = calls.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(after, calls.load());
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
    ASSERT_TRUE(collectUntilDone(&frames));
    EXPECT_EQ(2, HydraRender::getCyclesThreads());
    const U64 restartsBefore = _render->getRestartCount();
    const std::uint64_t buildsBefore = HydraRender::getEngineBuildCount();

    SceneStage* stage = _stage.get();
    std::unique_ptr<ProgressiveRender> second(new ProgressiveRender(&_stageLock, [stage]() { return stage->getStage(); }, std::function<void()>(), 0.05f));
    second->setRenderer(kRenderDelegateCyclesId);
    second->setThreads([]() { return 3; });
    second->setView(_view);
    const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now() + kTimeout;
    for (;;) {
        const ProgressiveRender::Status a = _render->getStatus();
        const ProgressiveRender::Status b = second->getStatus();
        ASSERT_TRUE(a.error.empty()) << a.error;
        ASSERT_TRUE(b.error.empty()) << b.error;
        const std::shared_ptr<const ProgressiveRender::Frame> frameA = _render->getFrame();
        const std::shared_ptr<const ProgressiveRender::Frame> frameB = second->getFrame();
        const bool doneA = a.converged && frameA && frameA->converged && (frameA->restart == a.restarts) && (a.restarts > restartsBefore);
        const bool doneB = b.converged && frameB && frameB->converged && (frameB->restart == b.restarts);
        if (doneA && doneB) {
            break;
        }
        ASSERT_TRUE(std::chrono::steady_clock::now() < end) << "the renders did not finish in " << kTimeout.count() << " s";
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
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
    ASSERT_TRUE(collectUntilDone(&frames));
    EXPECT_EQ(12, _render->getMaxSamples());
    EXPECT_EQ(12, frames.back()->samples);

    _render->setMaxSamples(5);
    frames.clear();
    ASSERT_TRUE(collectUntilDone(&frames));
    EXPECT_EQ(5, _render->getMaxSamples());
    EXPECT_EQ(5, frames.back()->samples);

    const U64 restarts = _render->getRestartCount();
    const U64 builds = HydraRender::getEngineBuildCount();
    _render->setSettings(prim);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(restarts, _render->getRestartCount());
    EXPECT_EQ(builds, HydraRender::getEngineBuildCount());

    _render->setMaxSamples(0);
    prim["cycles:samples"] = PXR_NS::VtValue(7);
    _render->setSettings(prim);
    frames.clear();
    ASSERT_TRUE(collectUntilDone(&frames));
    EXPECT_EQ(7, _render->getMaxSamples());
    EXPECT_EQ(7, frames.back()->samples);
    EXPECT_GT(HydraRender::getEngineBuildCount(), builds);
}
