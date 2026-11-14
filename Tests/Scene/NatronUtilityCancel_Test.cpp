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

#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/rect2i.h>
#include <pxr/base/gf/vec2i.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/vt/array.h>
#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hdx/tokens.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/timeCode.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>

#include "Global/Macros.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "BaseTest.h"
#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/RenderDelegates.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_USING

namespace {
typedef std::chrono::steady_clock Clock;
typedef HydraRender::ProgressiveRender ProgressiveRender;

const std::chrono::milliseconds kCancelBound(500);
const std::chrono::seconds kTimeout(300);
const int kHugeGrid = 1500;
const int kManyGridsAcross = 16;
const int kManyGridSize = 100;
const int kLatencyWindow = 4096;
const int kViewWindow = 1024;
const double kCameraZ = 30.;

// An n by n grid of quads, width units wide and facing the camera, n * n * 2 triangles.
void
defineGrid(const UsdStageRefPtr& stage,
           const std::string& path,
           int n,
           const GfVec3f& color,
           const GfVec3f& centre = GfVec3f(0.f),
           float width = 20.f)
{
    VtVec3fArray points;
    VtIntArray counts(n * n, 4);
    VtIntArray indices;
    const float left = centre[0] - width / 2.f;
    const float bottom = centre[1] - width / 2.f;

    points.reserve((n + 1) * (n + 1));
    indices.reserve(n * n * 4);
    for (int y = 0; y <= n; ++y) {
        for (int x = 0; x <= n; ++x) {
            points.push_back(GfVec3f(left + width * x / n, bottom + width * y / n, centre[2]));
        }
    }
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const int corner = y * (n + 1) + x;

            indices.push_back(corner);
            indices.push_back(corner + 1);
            indices.push_back(corner + n + 2);
            indices.push_back(corner + n + 1);
        }
    }

    UsdGeomMesh mesh = UsdGeomMesh::Define(stage, SdfPath(path));

    mesh.CreatePointsAttr(VtValue(points));
    mesh.CreateFaceVertexCountsAttr(VtValue(counts));
    mesh.CreateFaceVertexIndicesAttr(VtValue(indices));
    mesh.CreateSubdivisionSchemeAttr(VtValue(UsdGeomTokens->none));
    mesh.CreateDisplayColorAttr(VtValue(VtVec3fArray(1, color)));
}

UsdStageRefPtr
hugeMeshStage(const GfVec3f& color)
{
    UsdStageRefPtr stage = UsdStage::CreateInMemory();

    defineGrid(stage, "/Huge", kHugeGrid, color);

    return stage;
}

// The grids tile the view side by side rather than stack: stacked, every ray walks every grid's
// hierarchy, and the frame's tracing outgrows the test's time limit on slower runners. A grid is
// centred on the axis so the centre pixel never falls on a seam.
UsdStageRefPtr
manyMeshesStage(const GfVec3f& color)
{
    UsdStageRefPtr stage = UsdStage::CreateInMemory();
    const float width = 20.f / kManyGridsAcross;

    for (int row = 0; row < kManyGridsAcross; ++row) {
        for (int column = 0; column < kManyGridsAcross; ++column) {
            const GfVec3f centre(width * (column - kManyGridsAcross / 2 + 1), width * (row - kManyGridsAcross / 2 + 1), 0.f);

            defineGrid(stage, "/Grid" + std::to_string(row * kManyGridsAcross + column), kManyGridSize, color, centre, width);
        }
    }

    return stage;
}

GfFrustum
viewFrom(double x)
{
    GfFrustum camera;

    camera.SetPerspective(45., 1., 0.1, 1000.);
    camera.SetPosition(GfVec3d(x, 0., kCameraZ));

    return camera;
}

// The delegate through the same engine the node drives it with, at a size whose frame takes a while.
class HeavyRender {
public:
    explicit HeavyRender(const UsdStageRefPtr& stage)
        : _stage(stage)
    {
        RenderDelegates::list();

        UsdImagingGLEngine::Parameters parameters;

        parameters.rendererPluginId = TfToken(kRenderDelegateUtilityId);
        parameters.gpuEnabled = false;
        _engine.reset(new UsdImagingGLEngine(parameters));
        _engine->SetEnablePresentation(false);
        EXPECT_TRUE(_engine->SetRendererAovs({ HdAovTokens->color, HdAovTokens->depth, HdAovTokens->primId, HdAovTokens->instanceId }));
    }

    void render(double cameraX = 0.)
    {
        const GfFrustum camera = viewFrom(cameraX);

        _engine->SetRenderBufferSize(GfVec2i(kLatencyWindow, kLatencyWindow));
        _engine->SetFraming(CameraUtilFraming(GfRect2i(GfVec2i(0, 0), kLatencyWindow, kLatencyWindow)));
        _engine->SetCameraState(camera.ComputeViewMatrix(), camera.ComputeProjectionMatrix());

        UsdImagingGLRenderParams params;

        params.drawMode = UsdImagingGLDrawMode::DRAW_SHADED_SMOOTH;
        params.clearColor = GfVec4f(0.f, 0.f, 0.f, 0.f);
        params.colorCorrectionMode = HdxColorCorrectionTokens->disabled;
        params.highlight = false;
        _engine->Render(_stage->GetPseudoRoot(), params);
    }

    bool renderToTheEnd(double cameraX = 0.)
    {
        const Clock::time_point end = Clock::now() + kTimeout;

        for (;;) {
            render(cameraX);
            if (_engine->IsConverged()) {
                return true;
            }
            if (Clock::now() >= end) {
                ADD_FAILURE() << "the frame did not finish in " << kTimeout.count() << " s";

                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    // How long a blocking stop takes.
    std::chrono::milliseconds timedStop()
    {
        const Clock::time_point start = Clock::now();

        EXPECT_TRUE(_engine->StopRenderer());

        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
    }

    bool isConverged() const
    {
        return _engine->IsConverged();
    }

    void restart()
    {
        EXPECT_TRUE(_engine->RestartRenderer());
    }

    GfVec4f centreColor() const
    {
        HdRenderBuffer* buffer = _engine->GetAovRenderBuffer(HdAovTokens->color);

        if (!buffer) {
            ADD_FAILURE() << "no colour buffer";

            return GfVec4f(0.f);
        }

        const GfVec4f* pixels = static_cast<const GfVec4f*>(buffer->Map());
        const GfVec4f value = pixels[(kLatencyWindow / 2) * kLatencyWindow + kLatencyWindow / 2];

        buffer->Unmap();

        return value;
    }

private:
    UsdStageRefPtr _stage;
    std::unique_ptr<UsdImagingGLEngine> _engine;
};

// Stops a frame at several points of its preparation and drawing: a stop takes less than the bound and
// leaves it unfinished, and the render after it finishes correctly.
void
checkCancelLatency(const UsdStageRefPtr& stage,
                   const char* name)
{
    HeavyRender render(stage);
    int stopsOfARunningFrame = 0;

    for (const int delayMs : { 0, 20, 100, 300 }) {
        render.render();
        if (render.isConverged()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));

        const std::chrono::milliseconds took = render.timedStop();

        std::cout << "[   NOTE   ] " << name << ": a stop " << delayMs << " ms into the frame took " << took.count() << " ms" << std::endl;
        EXPECT_LT(took.count(), kCancelBound.count()) << delayMs << " ms into the frame";
        render.render();
        EXPECT_FALSE(render.isConverged()) << "a stopped frame finished, " << delayMs << " ms into the frame";
        render.restart();
        ++stopsOfARunningFrame;
    }
    EXPECT_GT(stopsOfARunningFrame, 0) << "the scene is too light to test this";

    ASSERT_TRUE(render.renderToTheEnd());
    EXPECT_FLOAT_EQ(1.f, render.centreColor()[0]);

    // Now that the scene is prepared, a new view spends its time on the tiles alone.
    render.render(0.5);
    if (!render.isConverged()) {
        const std::chrono::milliseconds took = render.timedStop();

        std::cout << "[   NOTE   ] " << name << ": a stop while drawing took " << took.count() << " ms" << std::endl;
        EXPECT_LT(took.count(), kCancelBound.count());
        render.restart();
    }
    ASSERT_TRUE(render.renderToTheEnd(0.5));
    EXPECT_FLOAT_EQ(1.f, render.centreColor()[0]);
}

ProgressiveRender::View
viewOf(bool showRender)
{
    ProgressiveRender::View view;

    view.frustum = viewFrom(0.);
    view.width = kViewWindow;
    view.height = kViewWindow;
    view.time = UsdTimeCode::Default();
    view.showRender = showRender;

    return view;
}

class NatronUtilityCancelTest
    : public BaseTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        _render.reset();
        BaseTest::TearDown();
    }

    void start(const UsdStageRefPtr& stage,
               bool showRender = false)
    {
        RenderDelegates::list();
        _render.reset(new ProgressiveRender(&_stageLock, [stage]() { return stage; }, std::function<void()>(), 0.05f));
        _render->setRenderer(kRenderDelegateUtilityId);
        _render->setThreads([]() { return 0; });
        _render->setView(viewOf(showRender));
    }

    // Keeps each frame published, until a finished frame of a render at least as late as minRestart is
    // the latest. True if that came.
    bool collectUntilFinished(std::uint64_t minRestart,
                              std::vector<std::shared_ptr<const ProgressiveRender::Frame>>* frames)
    {
        const Clock::time_point end = Clock::now() + kTimeout;

        while (Clock::now() < end) {
            const ProgressiveRender::Status status = _render->getStatus();
            const std::shared_ptr<const ProgressiveRender::Frame> frame = _render->getFrame();

            if (!status.error.empty()) {
                ADD_FAILURE() << status.error;

                return false;
            }
            if (frame && (frames->empty() || (frames->back()->serial != frame->serial))) {
                frames->push_back(frame);
            }
            if (status.converged && frame && frame->converged && (frame->restart == status.restarts) && (status.restarts >= minRestart)) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        ADD_FAILURE() << "the render did not finish in " << kTimeout.count() << " s";

        return false;
    }

    // The worker holds the stage lock across its Render(), which starts the frame. Once it has started
    // that pass, taking the lock waits for the frame to be under way.
    void waitForFrameUnderWay()
    {
        const Clock::time_point end = Clock::now() + kTimeout;

        while (!_render->getStatus().active && Clock::now() < end) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        ASSERT_TRUE(_render->getStatus().active);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::lock_guard<std::mutex> l(_stageLock);
    }

    static GfVec4f centreOf(const ProgressiveRender::Frame& frame)
    {
        const std::size_t at = (static_cast<std::size_t>(frame.height / 2) * frame.width + frame.width / 2) * 4;

        return GfVec4f(frame.rgba[at], frame.rgba[at + 1], frame.rgba[at + 2], frame.rgba[at + 3]);
    }

    std::mutex _stageLock;
    std::unique_ptr<ProgressiveRender> _render;
};
} // anon namespace

TEST(NatronUtilityCancel, AStopCutsAHugeMeshsFrameShortWithinTheBound)
{
    checkCancelLatency(hugeMeshStage(GfVec3f(1.f, 0.f, 0.f)), "one huge mesh");
}

TEST(NatronUtilityCancel, AStopCutsTheFrameOfManyMeshesShortWithinTheBound)
{
    checkCancelLatency(manyMeshesStage(GfVec3f(1.f, 0.f, 0.f)), "many meshes");
}

TEST_F(NatronUtilityCancelTest, AnEditDuringAFrameRestartsItAndTheOldFrameNeverLands)
{
    const UsdStageRefPtr stage = hugeMeshStage(GfVec3f(1.f, 0.f, 0.f));

    start(stage);
    waitForFrameUnderWay();
    ASSERT_FALSE(_render->getFrame()) << "the scene is too light to test this";

    std::uint64_t editedRestart = 0;
    {
        std::lock_guard<std::mutex> l(_stageLock);

        UsdGeomMesh(stage->GetPrimAtPath(SdfPath("/Huge"))).GetDisplayColorAttr().Set(VtVec3fArray(1, GfVec3f(0.f, 1.f, 0.f)));
        editedRestart = _render->getRestartCount() + 1;
    }
    _render->invalidate();

    std::vector<std::shared_ptr<const ProgressiveRender::Frame>> frames;

    ASSERT_TRUE(collectUntilFinished(editedRestart, &frames));
    ASSERT_FALSE(frames.empty());
    for (const std::shared_ptr<const ProgressiveRender::Frame>& frame : frames) {
        const GfVec4f centre = centreOf(*frame);

        EXPECT_GT(centre[1], centre[0]) << "frame " << frame->serial << " of render " << frame->restart << " shows the colour from before the edit";
        EXPECT_TRUE(frame->converged);
    }
}

TEST_F(NatronUtilityCancelTest, StopDuringAFramePublishesNothingAndResumeFinishesIt)
{
    start(hugeMeshStage(GfVec3f(1.f, 0.f, 0.f)));
    waitForFrameUnderWay();
    ASSERT_FALSE(_render->getFrame()) << "the scene is too light to test this";

    _render->stop();

    const Clock::time_point end = Clock::now() + kTimeout;

    while (!_render->getStatus().stopped && Clock::now() < end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_TRUE(_render->getStatus().stopped);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_FALSE(_render->getFrame());
    EXPECT_FALSE(_render->getStatus().converged);

    _render->resume();

    std::vector<std::shared_ptr<const ProgressiveRender::Frame>> frames;

    ASSERT_TRUE(collectUntilFinished(0, &frames));
    ASSERT_EQ(1u, frames.size());
    EXPECT_TRUE(frames[0]->converged);
}

TEST_F(NatronUtilityCancelTest, TheViewsPurposesDecideWhatIsDrawn)
{
    const UsdStageRefPtr stage = UsdStage::CreateInMemory();

    defineGrid(stage, "/Default", 1, GfVec3f(1.f, 0.f, 0.f));
    defineGrid(stage, "/RenderOnly", 1, GfVec3f(0.f, 1.f, 0.f), GfVec3f(0.f, 0.f, 1.f));
    UsdGeomMesh(stage->GetPrimAtPath(SdfPath("/RenderOnly"))).CreatePurposeAttr(VtValue(UsdGeomTokens->render));

    start(stage);

    std::vector<std::shared_ptr<const ProgressiveRender::Frame>> frames;

    ASSERT_TRUE(collectUntilFinished(0, &frames));
    GfVec4f centre = centreOf(*frames.back());
    EXPECT_GT(centre[0], centre[1]) << "the render-purpose prim shows with the purpose off";

    std::uint64_t restart = _render->getRestartCount() + 1;

    _render->setView(viewOf(true));
    ASSERT_TRUE(collectUntilFinished(restart, &frames));
    centre = centreOf(*frames.back());
    EXPECT_GT(centre[1], centre[0]) << "the render-purpose prim is missing with the purpose on";

    restart = _render->getRestartCount() + 1;
    _render->setView(viewOf(false));
    ASSERT_TRUE(collectUntilFinished(restart, &frames));
    centre = centreOf(*frames.back());
    EXPECT_GT(centre[0], centre[1]) << "the render-purpose prim stays with the purpose off again";
}
