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

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include "Engine/GpuContext.h"
#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/RectI.h"
#include "Tests/Native/NativeGpuParity.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GlInterop.h"
#include "Engine/Gpu/GpuBuffer.h"
#include "Engine/Gpu/GpuDevice.h"
#include "Engine/Gpu/GpuRecord.h"
#include "Engine/Gpu/GpuRuntime.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Scene/HydraHandoff.h"
#endif

NATRON_NAMESPACE_USING

namespace {
const char* const kFixture = NATRON_TESTS_FIXTURES_DIR "/scene-reference.usda";
const int kWidth = 200;
const int kHeight = 100;

class ScopedEnv {
public:
    // A null value unsets the variable.
    ScopedEnv(const char* name,
              const char* value)
        : _name(name)
    {
        const char* old = std::getenv(name);
        _hadOld = old != nullptr;
        if (old) {
            _old = old;
        }
        if (value) {
            ::setenv(name, value, 1);
        } else {
            ::unsetenv(name);
        }
    }

    ~ScopedEnv()
    {
        if (_hadOld) {
            ::setenv(_name.c_str(), _old.c_str(), 1);
        } else {
            ::unsetenv(_name.c_str());
        }
    }

private:
    std::string _name;
    std::string _old;
    bool _hadOld = false;
};

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
    std::printf("[  SKIPPED ] USDRender hand-off: %s\n", error.c_str());

    return true;
}

#ifdef NATRON_HAVE_GPU

const char*
pathName(gpu::GlHandoffPath path)
{
    return gpu::overrideName(path);
}

// Restores the ladder to NATRON_USDRENDER_HANDOFF's choice whatever the test does.
class ScopedForcedPath {
public:
    explicit ScopedForcedPath(std::optional<gpu::GlHandoffPath> path)
    {
        HydraHandoff::setForcedPathForTests(path);
    }

    ~ScopedForcedPath()
    {
        HydraHandoff::setForcedPathForTests(std::nullopt);
    }
};

// Runs everything the render thread has been given, the hand-offs' clean-up included.
void
drainRenderThread()
{
    std::string error;
    EXPECT_TRUE(HydraRender::withContext([](const std::function<void*(const char*)>&) { }, &error)) << error;
}

// A GPU context of its own, whose messages the test reads, and the reference scene's request.
class Harness {
public:
    std::unique_ptr<GpuContext> context;
    PXR_NS::UsdStageRefPtr stage;
    HydraRender::Request request;

    ~Harness()
    {
        if (stage) {
            HydraRender::release(this);
            drainRenderThread();
        }
    }

    // False after reporting the skip, or the failure when the environment requires GL.
    bool init()
    {
        if (skipWithoutGL()) {
            return false;
        }
        GpuContextOptions options;
        options.mode = GpuResolvedMode::Force;
        options.infoLog = [this](const std::string& line) {
            std::lock_guard<std::mutex> l(_logMutex);
            _log.push_back(line);
        };
        context.reset(new GpuContext(options));
        if (!context->waitForInit(std::chrono::seconds(60)) || !context->isAvailable()) {
            reportNoGpu("the GPU context did not come up");

            return false;
        }
        std::printf("vulkan device: %s\n", context->deviceName().c_str());

        stage = PXR_NS::UsdStage::Open(kFixture);
        EXPECT_TRUE(bool(stage)) << kFixture;
        if (!stage) {
            return false;
        }
        std::string error;
        request.owner = this;
        // The fixture file is the scene's one layer; every rung and the reference reuse one
        // render of the unchanged scene.
        request.scene = SceneOps::appendLayer(ScenePayload(), stage->GetRootLayer(), 0x5eed);
        request.time = PXR_NS::UsdTimeCode(1);
        request.width = kWidth;
        request.height = kHeight;
        EXPECT_TRUE(HydraRender::cameraFrustum(stage, PXR_NS::SdfPath("/World/Cam"), request.time, double(kWidth) / kHeight, HydraRender::eFitModeFill, &request.frustum, &error)) << error;

        return !::testing::Test::HasFailure();
    }

    std::vector<std::string> log() const
    {
        std::lock_guard<std::mutex> l(_logMutex);

        return _log;
    }

private:
    mutable std::mutex _logMutex;
    std::vector<std::string> _log;
};

// A window of the frame, and where in a larger output image the hand-off writes it.
struct Placement {
    HydraRender::PlaneWindow window;
    RectI outputBounds;
    RectI outputRect;
};

std::vector<Placement>
placements()
{
    std::vector<Placement> all;
    const HydraRender::PlaneEnum planes[] = { HydraRender::ePlaneColor, HydraRender::ePlaneDepth, HydraRender::ePlanePrimId };
    for (HydraRender::PlaneEnum plane : planes) {
        // The whole frame into an image of its size: one copy region.
        Placement whole;
        whole.window.plane = plane;
        whole.window.nComps = (plane == HydraRender::ePlaneColor) ? 4 : 1;
        whole.window.x1 = 0;
        whole.window.y1 = 0;
        whole.window.x2 = kWidth;
        whole.window.y2 = kHeight;
        whole.outputBounds = RectI(0, 0, kWidth, kHeight);
        whole.outputRect = whole.outputBounds;
        all.push_back(whole);

        // Part of the frame into the middle of a wider image: a copy per row.
        Placement part = whole;
        part.window.x1 = 20;
        part.window.y1 = 10;
        part.window.x2 = 150;
        part.window.y2 = 80;
        part.outputBounds = RectI(-3, 5, 160, 90);
        part.outputRect = RectI(7, 8, 137, 78);
        all.push_back(part);
    }
    // Colour as alpha alone, the way a one-channel output asks for it.
    Placement alpha = all.front();
    alpha.window.nComps = 1;
    all.push_back(alpha);

    return all;
}

std::string
describe(const Placement& p)
{
    const char* const names[] = { "rgba", "depth", "primId" };

    return std::string(names[p.window.plane]) + "x" + std::to_string(p.window.nComps) + " window (" + std::to_string(p.window.x1) + "," + std::to_string(p.window.y1) + ")-(" + std::to_string(p.window.x2) + "," + std::to_string(p.window.y2) + ")";
}

// The bytes outside the window keep this pattern, a NaN no render writes.
const std::uint8_t kUntouched = 0xA5;

/**
 * Hands p's window over on handoff's current rung and compares the bytes with the readback path's.
 * False when the hand-off refused, *error then saying why.
 **/
bool
handOverAndCompare(Harness& h,
                   HydraHandoff& handoff,
                   const Placement& p,
                   const std::string& what,
                   std::string* error)
{
    std::vector<float> expected;
    std::map<std::uint32_t, std::string> primPaths;
    std::string renderError;
    EXPECT_TRUE(HydraRender::renderWindow(h.request, p.window, &expected, &primPaths, &renderError)) << renderError;
    if (expected.size() != p.window.floatCount()) {
        ADD_FAILURE() << what << ": the readback gave " << expected.size() << " floats";

        return true;
    }

    gpu::GpuRuntime* runtime = h.context->runtime();
    const int nComps = p.window.nComps;
    const int rowStrideFloats = p.outputBounds.width() * nComps;
    const VkDeviceSize outputBytes = (VkDeviceSize)rowStrideFloats * (VkDeviceSize)p.outputBounds.height() * sizeof(float);
    gpu::GpuBuffer output;
    if (gpu::GpuStatus s = gpu::GpuBuffer::createStaging(runtime->device(), outputBytes, gpu::kGpuBufferDefaultUsage, output); !s) {
        *error = "staging buffer: " + s.message;

        return false;
    }
    std::memset(output.mapped(), kUntouched, (std::size_t)outputBytes);
    GpuImageView view;
    view.buffer = &output;
    view.byteOffset = 0;
    view.bounds = p.outputBounds;
    view.rowStrideFloats = rowStrideFloats;
    view.nComps = nComps;

    {
        std::unique_ptr<gpu::GpuRecordContext> context;
        if (gpu::GpuStatus s = runtime->newRecordContext(context); !s) {
            *error = "record context: " + s.message;

            return false;
        }
        if (!handoff.record(h.request, p.window, *context, view, p.outputRect, error)) {
            return false;
        }
        if (gpu::GpuStatus s = context->submitAndWait(); !s) {
            *error = s.message;

            return false;
        }
    }

    const unsigned char* bytes = static_cast<const unsigned char*>(output.mapped());
    const std::size_t rowBytes = (std::size_t)(p.window.x2 - p.window.x1) * (std::size_t)nComps * sizeof(float);
    int badRows = 0;
    int touchedOutside = 0;
    for (int y = p.outputBounds.y1; y < p.outputBounds.y2; ++y) {
        const unsigned char* row = bytes + (std::size_t)(y - p.outputBounds.y1) * (std::size_t)rowStrideFloats * sizeof(float);
        const bool inRows = (y >= p.outputRect.y1) && (y < p.outputRect.y2);
        const std::size_t before = inRows ? (std::size_t)(p.outputRect.x1 - p.outputBounds.x1) * (std::size_t)nComps * sizeof(float) : (std::size_t)rowStrideFloats * sizeof(float);
        for (std::size_t i = 0; i < (std::size_t)rowStrideFloats * sizeof(float); ++i) {
            const bool inside = inRows && (i >= before) && (i < before + rowBytes);
            if (!inside && (row[i] != kUntouched)) {
                ++touchedOutside;
                break;
            }
        }
        if (inRows) {
            const float* want = expected.data() + (std::size_t)(y - p.outputRect.y1) * (std::size_t)(p.window.x2 - p.window.x1) * (std::size_t)nComps;
            if (std::memcmp(row + before, want, rowBytes) != 0) {
                if (badRows == 0) {
                    ADD_FAILURE() << what << ": output row " << y << " differs from the readback's bytes";
                }
                ++badRows;
            }
        }
    }
    EXPECT_EQ(0, badRows) << what;
    EXPECT_EQ(0, touchedOutside) << what << ": rows written outside the window";

    return true;
} // handOverAndCompare

// Every placement on rung `forced`, or a skip naming why the ladder could not reach it.
void
checkForcedRung(gpu::GlHandoffPath forced)
{
    Harness h;
    if (!h.init()) {
        return;
    }
    ScopedEnv env("NATRON_USDRENDER_HANDOFF", nullptr);
    ScopedForcedPath hook(forced);
    HydraHandoff handoff(h.context.get());
    const gpu::GlHandoffPath path = handoff.path();
    std::printf("[usdrender-handoff] forced %s, took %s: %s\n", pathName(forced), pathName(path), handoff.reason().c_str());
    EXPECT_NE(std::string::npos, handoff.reason().find(std::string("NATRON_USDRENDER_HANDOFF=") + pathName(forced))) << handoff.reason();
    if (path != forced) {
        // The ladder only ever steps down from where it is told to start.
        EXPECT_GT((int)path, (int)forced);
        if (path == gpu::GlHandoffPath::Readback) {
            std::string error;
            EXPECT_FALSE(handOverAndCompare(h, handoff, placements().front(), "readback", &error));
        }
        std::printf("[  SKIPPED ] the %s rung is out of reach here: %s\n", pathName(forced), handoff.reason().c_str());

        return;
    }
    for (const Placement& p : placements()) {
        std::string error;
        const std::string what = std::string(pathName(forced)) + " " + describe(p);
        EXPECT_TRUE(handOverAndCompare(h, handoff, p, what, &error)) << what << ": " << error;
        EXPECT_EQ(forced, handoff.path()) << what << ": " << handoff.reason();
    }
}

#endif // NATRON_HAVE_GPU
} // anonymous namespace

TEST(USDRenderHandoff, ZeroCopyMatchesTheReadbackBitForBit)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    checkForcedRung(gpu::GlHandoffPath::ZeroCopy);
#endif
}

TEST(USDRenderHandoff, HostSyncMatchesTheReadbackBitForBit)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    checkForcedRung(gpu::GlHandoffPath::ZeroCopyHostSync);
#endif
}

TEST(USDRenderHandoff, ForcedReadbackRefusesEveryHandOff)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    Harness h;
    if (!h.init()) {
        return;
    }
    // Through the environment this time, as a user forces it.
    ScopedEnv env("NATRON_USDRENDER_HANDOFF", "Readback");
    HydraHandoff handoff(h.context.get());
    EXPECT_EQ(gpu::GlHandoffPath::Readback, handoff.path());
    EXPECT_NE(std::string::npos, handoff.reason().find("NATRON_USDRENDER_HANDOFF=readback")) << handoff.reason();
    std::string error;
    EXPECT_FALSE(handOverAndCompare(h, handoff, placements().front(), "readback", &error));
    EXPECT_FALSE(error.empty());
#endif
}

TEST(USDRenderHandoff, GpuOffMeansReadbackWithoutAWord)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    std::vector<std::string> log;
    GpuContextOptions options;
    options.mode = GpuContext::resolveMode(eGpuComputeModeAuto, "off");
    options.infoLog = [&log](const std::string& line) { log.push_back(line); };
    GpuContext context(options);
    ScopedEnv env("NATRON_USDRENDER_HANDOFF", nullptr);
    HydraHandoff handoff(&context);
    EXPECT_EQ(gpu::GlHandoffPath::Readback, handoff.path());
    EXPECT_NE(std::string::npos, handoff.reason().find("unavailable")) << handoff.reason();
    // Asking again changes nothing and says nothing.
    EXPECT_EQ(gpu::GlHandoffPath::Readback, handoff.path());
    for (const std::string& line : log) {
        EXPECT_EQ(std::string::npos, line.find("USDRender")) << line;
    }
#endif
}

TEST(USDRenderHandoff, ADeviceLossFallsBackQuietlyAndRecovers)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    Harness h;
    if (!h.init()) {
        return;
    }
    ScopedEnv env("NATRON_USDRENDER_HANDOFF", nullptr);
    HydraHandoff handoff(h.context.get());
    const gpu::GlHandoffPath rung = handoff.path();
    if (rung == gpu::GlHandoffPath::Readback) {
        std::printf("[  SKIPPED ] no GL-to-Vulkan rung here: %s\n", handoff.reason().c_str());

        return;
    }
    const Placement p = placements().front();
    std::string error;
    ASSERT_TRUE(handOverAndCompare(h, handoff, p, "before the loss", &error)) << error;
    const std::size_t logBefore = h.log().size();

    gpu::GpuRuntime* runtime = h.context->runtime();
    // The fault plan NATRON_GPU_FAULT=lost@N installs, armed on the next submission.
    runtime->device().injectFault(gpu::GpuFaultPlan::Kind::DeviceLost, 1);
    error.clear();
    EXPECT_FALSE(handOverAndCompare(h, handoff, p, "the lost hand-off", &error));
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(runtime->isLost());

    // Readback until the runtime recovers, with nothing to show for it but the reason.
    EXPECT_EQ(gpu::GlHandoffPath::Readback, handoff.path());
    EXPECT_NE(std::string::npos, handoff.reason().find("lost")) << handoff.reason();
    error.clear();
    EXPECT_FALSE(handOverAndCompare(h, handoff, p, "while lost", &error));
    EXPECT_FALSE(error.empty());

    // The lost hand-offs' slots go back through the render thread; then nothing holds the runtime.
    drainRenderThread();
    ASSERT_TRUE(h.context->recoverIfLost()) << "the hand-off still holds the runtime";
    EXPECT_EQ(0u, runtime->leakedAllocationsAtLastTeardown());
    ASSERT_EQ(runtime, h.context->runtime());

    EXPECT_EQ(rung, handoff.path()) << handoff.reason();
    error.clear();
    EXPECT_TRUE(handOverAndCompare(h, handoff, p, "after the recovery", &error)) << error;

    // The context's own recovery line is all the loss printed.
    const std::vector<std::string> log = h.log();
    for (std::size_t i = logBefore; i < log.size(); ++i) {
        EXPECT_NE(std::string::npos, log[i].find("recovered from a device loss")) << log[i];
    }
#endif
} // TEST
