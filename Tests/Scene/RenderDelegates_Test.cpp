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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include <pxr/base/plug/registry.h>

#include "CyclesTestUtils.h"
#include "Engine/Nodes/Scene/CyclesAovs.h"
#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/MoonRayAovs.h"
#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/ScenePayload.h"

NATRON_NAMESPACE_USING

// The test mains register Tests/fixtures/renderDelegates, a plugInfo.json declaring this renderer
// with a library that does not exist.
namespace {
const char* const kStubId = "NatronTestStubRendererPlugin";

// The dev image's /usr/local glvnd lists only NVIDIA; the system directory holds Mesa. Set before
// the first EGL use, as the render thread's context is made once.
void
pointEGLAtMesa()
{
    if (!std::getenv("__EGL_VENDOR_LIBRARY_FILENAMES") && !std::getenv("__EGL_VENDOR_LIBRARY_DIRS") && (::access("/usr/share/glvnd/egl_vendor.d", F_OK) == 0) && (::access("/usr/local/share/glvnd/egl_vendor.d", F_OK) == 0)) {
        ::setenv("__EGL_VENDOR_LIBRARY_DIRS", "/usr/share/glvnd/egl_vendor.d", 1);
    }
}

// True when the test must stop: no 4.5 context could be made, which fails the test only under
// NATRON_TEST_REQUIRE_GL=1.
bool
skipWithoutGL()
{
    pointEGLAtMesa();
    std::string error;
    if (!HydraRender::contextDescription(&error).empty()) {
        return false;
    }
    const char* require = std::getenv("NATRON_TEST_REQUIRE_GL");
    if (require && (std::strcmp(require, "1") == 0)) {
        ADD_FAILURE() << error << " (NATRON_TEST_REQUIRE_GL=1)";

        return true;
    }
    std::printf("[  SKIPPED ] Storm: %s\n", error.c_str());

    return true;
}

// The bundled gtest has no GTEST_SKIP, so the test returns early on false;
// NATRON_TEST_REQUIRE_MOONRAY=1 turns that skip into a failure.
bool
moonRayOrSkip()
{
    if (PXR_NS::PlugRegistry::GetInstance().GetPluginWithName("hd_moonray")) {
        return true;
    }
    const char* reason = "OpenUSD has no hd_moonray plug-in registered; this build image predates the "
                         "MoonRay render delegate (tools/ci/local/build-moonray.sh), or the CPU lacks AVX2";
    const char* require = std::getenv("NATRON_TEST_REQUIRE_MOONRAY");
    if (require && (std::strcmp(require, "1") == 0)) {
        ADD_FAILURE() << "NATRON_TEST_REQUIRE_MOONRAY=1 but " << reason;
    } else {
        std::printf("[  SKIPPED ] %s\n", reason);
    }

    return false;
}

// A request for the reference scene through its camera, size x size, with renderer. False after a
// gtest failure.
bool
referenceRequest(const void* owner,
                 const std::string& renderer,
                 int size,
                 HydraRender::Request* request)
{
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(NATRON_TESTS_FIXTURES_DIR "/scene-reference.usda");
    if (!stage) {
        ADD_FAILURE() << "the reference scene did not open";

        return false;
    }
    request->owner = owner;
    request->scene = SceneOps::appendLayer(ScenePayload(), stage->GetRootLayer(), 1);
    request->time = PXR_NS::UsdTimeCode(1.);
    request->width = size;
    request->height = size;
    request->renderer = renderer;
    std::string error;
    if (!HydraRender::cameraFrustum(stage, PXR_NS::SdfPath("/World/Cam"), request->time, 1., HydraRender::eFitModeFill, &request->frustum, &error)) {
        ADD_FAILURE() << error;

        return false;
    }

    return true;
}

/**
 * Renders request offline and aborts it once abortAtPass of its delegate's Render() calls have
 * begun. The render fault hook holds each pass up to passMs before the delegate's Render(), so
 * that the abort, polled every 20 ms, lands inside a pass; the hold ends as soon as the abort is
 * seen. What the measured time holds besides that is the delegate's own Render() of the pass in
 * flight, which nothing can stop, and the way back to the caller. Expects the render to be
 * cancelled within a second of the abort and prints the time it took.
 **/
void
expectAbortReturnsPromptly(const char* label,
                           HydraRender::Request request,
                           int abortAtPass,
                           int passMs)
{
    typedef std::chrono::steady_clock Clock;
    std::atomic<int> passes(0);
    std::atomic<bool> abortSeen(false);
    RenderDelegates::setRenderFaultHook([&passes, &abortSeen, passMs]() {
        ++passes;
        const Clock::time_point until = Clock::now() + std::chrono::milliseconds(passMs);
        while (!abortSeen.load() && (Clock::now() < until)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    Clock::time_point abortedAt;
    int passesAtAbort = 0;
    request.aborted = [&]() {
        if (!abortSeen.load() && (passes.load() >= abortAtPass)) {
            abortedAt = Clock::now();
            passesAtAbort = passes.load();
            abortSeen = true;
        }

        return abortSeen.load();
    };
    HydraRender::Result result;
    std::string error;
    const bool rendered = HydraRender::render(request, &result, &error);
    const Clock::time_point returnedAt = Clock::now();
    RenderDelegates::setRenderFaultHook(nullptr);

    EXPECT_FALSE(rendered) << label << " finished a render that was aborted";
    ASSERT_TRUE(abortSeen.load()) << label << " ended before it was aborted: " << error;
    EXPECT_NE(std::string::npos, error.find("cancelled")) << error;
    const double seconds = std::chrono::duration<double>(returnedAt - abortedAt).count();
    std::printf("[ ABORT    ] %s: render() returned %.3f s after the abort\n", label, seconds);
    EXPECT_LT(seconds, 1.) << label;
    EXPECT_LE(passes.load(), passesAtAbort + 1) << label << " began passes after the abort";
}
} // anon namespace

TEST(RenderDelegates, StormIsListedFirstAndAvailable)
{
    const std::vector<RenderDelegates::Delegate> delegates = RenderDelegates::list();

    ASSERT_FALSE(delegates.empty());
    EXPECT_EQ(kRenderDelegateStormId, delegates.front().id);
    EXPECT_EQ("Storm", delegates.front().displayName);
    EXPECT_TRUE(delegates.front().available) << delegates.front().error;
    EXPECT_TRUE(RenderDelegates::isStorm(delegates.front().id));
    EXPECT_TRUE(RenderDelegates::unavailableMessage(kRenderDelegateStormId).empty());
}

TEST(RenderDelegates, ARendererWhoseLibraryIsMissingIsUnavailableWithTheLoadersError)
{
    RenderDelegates::Delegate stub;

    ASSERT_TRUE(RenderDelegates::find(kStubId, &stub));
    EXPECT_EQ("Natron Test Stub", stub.displayName);
    EXPECT_FALSE(stub.available);
    EXPECT_NE(std::string::npos, stub.error.find("libNatronTestStubRendererMissing.so")) << stub.error;
    const std::string message = RenderDelegates::unavailableMessage(kStubId);
    EXPECT_NE(std::string::npos, message.find("Natron Test Stub")) << message;
    EXPECT_NE(std::string::npos, message.find(kStubId)) << message;
}

TEST(RenderDelegates, AnUnknownRendererIsNotFoundAndItsMessageNamesIt)
{
    RenderDelegates::Delegate missing;

    EXPECT_FALSE(RenderDelegates::find("NoSuchRendererPlugin", &missing));
    const std::string message = RenderDelegates::unavailableMessage("NoSuchRendererPlugin");
    EXPECT_NE(std::string::npos, message.find("NoSuchRendererPlugin")) << message;
}

// An unavailable renderer fails before any engine or GL work: no fallback to Storm.
TEST(RenderDelegates, RenderingWithAnUnavailableRendererIsAnErrorAndMakesNoEngine)
{
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(NATRON_TESTS_FIXTURES_DIR "/scene-reference.usda");
    ASSERT_TRUE(stage);
    const int owner = 0;
    HydraRender::Request request;
    request.owner = &owner;
    request.scene = SceneOps::appendLayer(ScenePayload(), stage->GetRootLayer(), 1);
    request.time = PXR_NS::UsdTimeCode(1.);
    request.width = 16;
    request.height = 16;
    request.renderer = kStubId;
    std::string error;
    ASSERT_TRUE(HydraRender::cameraFrustum(stage, PXR_NS::SdfPath("/World/Cam"), request.time, 1., HydraRender::eFitModeFill, &request.frustum, &error)) << error;

    const std::size_t stormEngines = HydraRender::getEngineCount();
    const std::size_t cpuEngines = HydraRender::getCpuEngineCount();
    const std::uint64_t builds = HydraRender::getEngineBuildCount();
    HydraRender::Result result;
    EXPECT_FALSE(HydraRender::render(request, &result, &error));
    EXPECT_NE(std::string::npos, error.find(kStubId)) << error;
    EXPECT_EQ(stormEngines, HydraRender::getEngineCount());
    EXPECT_EQ(cpuEngines, HydraRender::getCpuEngineCount());
    EXPECT_EQ(builds, HydraRender::getEngineBuildCount());

    std::vector<float> pixels;
    HydraRender::PlaneWindow window;
    window.x2 = 16;
    window.y2 = 16;
    error.clear();
    EXPECT_FALSE(HydraRender::renderWindow(request, window, &pixels, nullptr, &error));
    EXPECT_NE(std::string::npos, error.find(kStubId)) << error;
    EXPECT_EQ(builds, HydraRender::getEngineBuildCount());
}

// Storm renders up to several passes on its GL thread; an abort inside one stops it before the
// next. Its engine runs nothing between passes, so it survives the abort.
TEST(RenderDelegates, AbortingAStormRenderMidPassReturnsWithinASecond)
{
    if (skipWithoutGL()) {
        return;
    }
    static const int owner = 0;
    HydraRender::Request request;
    ASSERT_TRUE(referenceRequest(&owner, kRenderDelegateStormId, 64, &request));
    // A new engine's first pass syncs the scene and has llvmpipe compile every shader, over a
    // second on a loaded runner, and no pass can be stopped once begun. A cancelled render is
    // usually one a later frame or edit replaces, on a warm engine, so the aborted pass is one.
    HydraRender::Result result;
    std::string error;
    ASSERT_TRUE(HydraRender::render(request, &result, &error)) << error;
    const std::uint64_t builds = HydraRender::getEngineBuildCount();
    request.time = PXR_NS::UsdTimeCode(2.);
    expectAbortReturnsPromptly("Storm", request, 1, 400);
    request.aborted = std::function<bool()>();
    EXPECT_TRUE(HydraRender::render(request, &result, &error)) << error;
    EXPECT_EQ(builds, HydraRender::getEngineBuildCount()) << "Storm's engine did not survive the abort";
    HydraRender::release(&owner);
}

// Cycles has no stop: the engine is destroyed on its render thread, which cancels the session and
// joins its threads, before render() returns.
TEST(RenderDelegates, AbortingACyclesRenderMidPassReturnsWithinASecond)
{
    if (!CyclesTestUtils::cyclesOrSkip()) {
        return;
    }
    HydraRender::setRendererSettings(kRenderDelegateCyclesId, CyclesTestUtils::fixedSampling(1000000));
    static const int owner = 0;
    HydraRender::Request request;
    if (referenceRequest(&owner, kRenderDelegateCyclesId, 64, &request)) {
        expectAbortReturnsPromptly("Cycles", request, 2, 0);
    }
    HydraRender::release(&owner);
    EXPECT_EQ(0u, HydraRender::getCpuEngineCount());
    HydraRender::setRendererSettings(kRenderDelegateCyclesId, std::map<std::string, PXR_NS::VtValue>());
}

// MoonRay has no stop either: destroying the engine ends its Arras session's render process.
TEST(RenderDelegates, AbortingAMoonRayRenderMidPassReturnsWithinASecond)
{
    if (!moonRayOrSkip()) {
        return;
    }
    std::map<std::string, PXR_NS::VtValue> settings;
    settings["moonray:sceneVariable:pixel_samples"] = PXR_NS::VtValue(4096);
    HydraRender::setRendererSettings(kMoonRayRendererId, settings);
    static const int owner = 0;
    HydraRender::Request request;
    if (referenceRequest(&owner, kMoonRayRendererId, 64, &request)) {
        expectAbortReturnsPromptly("MoonRay", request, 2, 0);
    }
    HydraRender::release(&owner);
    EXPECT_EQ(0u, HydraRender::getCpuEngineCount());
    HydraRender::setRendererSettings(kMoonRayRendererId, std::map<std::string, PXR_NS::VtValue>());
}
