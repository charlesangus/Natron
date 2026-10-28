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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <pxr/base/gf/half.h>
#include <pxr/base/gf/rect2i.h>
#include <pxr/base/gf/vec2i.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/types.h>
#include <pxr/imaging/hdx/tokens.h>
#include <pxr/usd/usdLux/portalLight.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>

#include "Engine/Nodes/Scene/MoonRayAovs.h"

NATRON_NAMESPACE_USING

/*
 * Renders of Tests/fixtures/scene-moonray.usda through hdMoonray, headless, 32x32 at 4 samples a
 * pixel, frame 1 (MoonRay seeds its samplers from the frame number).
 *
 * The scene is checked against what it is built to give rather than a reference image: from the
 * camera at (6, 0, 0) looking down -X, both cubes' faces facing it lie at x = 0.75, 5.25 away
 * along the view, the Left cube (z = +1.2) in columns 6-13 and the Right one (z = -1.2) in columns
 * 19-26 of rows 12-19, and nothing at the centre column. MoonRay sends its fine passes at the
 * render outputs' default half precision, so depth is held to 0.01 (a half's step at 5.25 is
 * 0.004) and the face normal to 0.01 per component. MoonRay renders the same samples whatever its
 * thread count, so a second render on the same host is compared exactly.
 */

namespace {
const char* const kFixture = NATRON_TESTS_FIXTURES_DIR "/scene-moonray.usda";
const int kSize = 32;
const int kRow = 16;
const int kLeftColumn = 10;
const int kRightColumn = 25;
const int kMissColumn = 16;
const float kFaceDistance = 5.25f;
// A frame takes a few seconds; anything beyond this is a session that will never finish.
const std::chrono::seconds kRenderTimeout(120);

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

struct Frame {
    std::vector<float> rgba;
    std::vector<float> depth;
    std::vector<float> normal;
    std::vector<int> primId;
    std::vector<int> instanceId;
    std::vector<MoonRayAovs::Notice> notices;
};

double
componentValue(PXR_NS::HdFormat format,
               const unsigned char* at)
{
    switch (format) {
    case PXR_NS::HdFormatFloat32: {
        float value;
        std::memcpy(&value, at, sizeof(value));

        return value;
    }
    case PXR_NS::HdFormatFloat16: {
        PXR_NS::GfHalf value;
        std::memcpy(&value, at, sizeof(value));

        return float(value);
    }
    case PXR_NS::HdFormatInt32: {
        std::int32_t value;
        std::memcpy(&value, at, sizeof(value));

        return value;
    }
    case PXR_NS::HdFormatUNorm8:
        return *at / 255.;
    default:
        return 0.;
    }
}

// The first components of every pixel of the AOV name, resolved, as doubles.
bool
readAov(PXR_NS::UsdImagingGLEngine* engine,
        const PXR_NS::TfToken& name,
        std::size_t components,
        std::vector<double>* out,
        std::string* error)
{
    PXR_NS::HdRenderBuffer* buffer = engine->GetAovRenderBuffer(name);
    if (!buffer) {
        *error = "no " + name.GetString() + " AOV";

        return false;
    }
    buffer->Resolve();
    if (((int)buffer->GetWidth() != kSize) || ((int)buffer->GetHeight() != kSize)) {
        *error = name.GetString() + " is not " + std::to_string(kSize) + " square";

        return false;
    }
    const PXR_NS::HdFormat format = buffer->GetFormat();
    const PXR_NS::HdFormat componentFormat = PXR_NS::HdGetComponentFormat(format);
    const std::size_t count = PXR_NS::HdGetComponentCount(format);
    const std::size_t componentSize = PXR_NS::HdDataSizeOfFormat(componentFormat);
    const std::size_t stride = PXR_NS::HdDataSizeOfFormat(format);
    if (count < components) {
        *error = name.GetString() + " has " + std::to_string(count) + " components";

        return false;
    }
    const unsigned char* data = static_cast<const unsigned char*>(buffer->Map());
    if (!data) {
        *error = name.GetString() + " could not be mapped";

        return false;
    }
    out->assign((std::size_t)kSize * kSize * components, 0.);
    for (std::size_t i = 0; i < (std::size_t)kSize * kSize; ++i) {
        for (std::size_t c = 0; c < components; ++c) {
            (*out)[i * components + c] = componentValue(componentFormat, data + i * stride + c * componentSize);
        }
    }
    buffer->Unmap();

    return true;
}

std::size_t
pixel(int row,
      int column)
{
    return (std::size_t)row * kSize + (std::size_t)column;
}

std::unique_ptr<PXR_NS::UsdImagingGLEngine>
makeEngine(int threadBudget,
           std::string* error)
{
    PXR_NS::UsdImagingGLEngine::Parameters parameters;
    parameters.rendererPluginId = PXR_NS::TfToken(kMoonRayRendererId);
    parameters.gpuEnabled = false;
    std::unique_ptr<PXR_NS::UsdImagingGLEngine> engine(new PXR_NS::UsdImagingGLEngine(parameters));
    if (engine->GetCurrentRendererId() != PXR_NS::TfToken(kMoonRayRendererId)) {
        *error = "the engine did not start hdMoonray";

        return nullptr;
    }
    engine->SetEnablePresentation(false);
    MoonRayAovs::configureEngine(engine.get(), threadBudget);
    engine->SetRendererSetting(PXR_NS::TfToken("moonray:sceneVariable:pixel_samples"), PXR_NS::VtValue(2));
    if (!engine->SetRendererAovs(MoonRayAovs::aovs())) {
        *error = "hdMoonray refused the AOVs";

        return nullptr;
    }
    engine->SetCameraPath(PXR_NS::SdfPath("/World/Cam"));
    engine->SetRenderBufferSize(PXR_NS::GfVec2i(kSize, kSize));
    engine->SetFraming(PXR_NS::CameraUtilFraming(PXR_NS::GfRect2i(PXR_NS::GfVec2i(0, 0), kSize, kSize)));

    return engine;
}

PXR_NS::UsdImagingGLRenderParams
renderParams()
{
    PXR_NS::UsdImagingGLRenderParams params;
    params.frame = PXR_NS::UsdTimeCode(1.);
    params.enableLighting = true;
    params.enableSceneLights = true;
    params.enableSceneMaterials = true;
    params.clearColor = PXR_NS::GfVec4f(0.f, 0.f, 0.f, 0.f);
    params.colorCorrectionMode = PXR_NS::HdxColorCorrectionTokens->disabled;
    params.highlight = false;

    return params;
}

// Renders until converged, as USDRender's CPU path does. False with *error set on a session that
// failed or a frame that never finished; cancel, when set, ends the loop with true.
bool
converge(PXR_NS::UsdImagingGLEngine* engine,
         const PXR_NS::UsdStageRefPtr& stage,
         const MoonRayAovs::LogCapture& capture,
         const std::atomic<bool>* cancel,
         std::string* error)
{
    const PXR_NS::UsdImagingGLRenderParams params = renderParams();
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + kRenderTimeout;
    for (;;) {
        engine->Render(stage->GetPseudoRoot(), params);
        if (engine->IsConverged()) {
            return true;
        }
        if (cancel && cancel->load()) {
            return true;
        }
        const std::string failure = MoonRayAovs::sessionFailure(engine->GetRenderStats(), capture.notices());
        if (!failure.empty()) {
            *error = failure;

            return false;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            *error = "the frame did not finish in " + std::to_string(kRenderTimeout.count()) + " s";

            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

bool
render(const PXR_NS::UsdStageRefPtr& stage,
       int threadBudget,
       Frame* frame,
       std::string* error)
{
    std::unique_ptr<PXR_NS::UsdImagingGLEngine> engine = makeEngine(threadBudget, error);
    if (!engine) {
        return false;
    }
    // After the engine, so that MoonRay's libraries and their console log are loaded.
    MoonRayAovs::LogCapture capture;
    if (!converge(engine.get(), stage, capture, nullptr, error)) {
        return false;
    }
    std::vector<double> color, depth, normal, primId, instanceId;
    if (!readAov(engine.get(), PXR_NS::HdAovTokens->color, 4, &color, error) || !readAov(engine.get(), MoonRayAovs::depthAov(), 1, &depth, error) || !readAov(engine.get(), MoonRayAovs::normalAov(), 3, &normal, error) || !readAov(engine.get(), PXR_NS::HdAovTokens->primId, 1, &primId, error) || !readAov(engine.get(), PXR_NS::HdAovTokens->instanceId, 1, &instanceId, error)) {
        return false;
    }
    frame->rgba.assign(color.begin(), color.end());
    frame->depth.clear();
    for (double value : depth) {
        frame->depth.push_back(MoonRayAovs::cameraDepth((float)value));
    }
    frame->normal.assign(normal.begin(), normal.end());
    frame->primId.assign(primId.begin(), primId.end());
    frame->instanceId.assign(instanceId.begin(), instanceId.end());
    frame->notices = capture.notices();

    return true;
}

PXR_NS::UsdStageRefPtr
fixture()
{
    return PXR_NS::UsdStage::Open(kFixture);
}

// The ids of this process's children named execComp: the processes Arras renders in.
std::vector<int>
execCompChildren()
{
    std::vector<int> children;
    DIR* proc = ::opendir("/proc");
    if (!proc) {
        return children;
    }
    const int self = (int)::getpid();
    while (struct dirent* entry = ::readdir(proc)) {
        const int pid = std::atoi(entry->d_name);
        if (pid <= 0) {
            continue;
        }
        std::ifstream stat(std::string("/proc/") + entry->d_name + "/stat");
        std::string line;
        if (!std::getline(stat, line)) {
            continue;
        }
        // pid (comm) state ppid ...
        const std::string::size_type open = line.find('(');
        const std::string::size_type close = line.rfind(')');
        if ((open == std::string::npos) || (close == std::string::npos)) {
            continue;
        }
        const std::string comm = line.substr(open + 1, close - open - 1);
        std::istringstream rest(line.substr(close + 1));
        std::string state;
        int ppid = 0;
        rest >> state >> ppid;
        if ((ppid == self) && (comm == "execComp") && (state != "Z")) {
            children.push_back(pid);
        }
    }
    ::closedir(proc);

    return children;
}

int
threadCount(int pid)
{
    std::ifstream status("/proc/" + std::to_string(pid) + "/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.compare(0, 8, "Threads:") == 0) {
            return std::atoi(line.c_str() + 8);
        }
    }

    return 0;
}

// The most threads any execComp child had while a render with threadBudget ran.
int
peakRenderThreads(int threadBudget,
                  std::string* error)
{
    const PXR_NS::UsdStageRefPtr stage = fixture();
    std::unique_ptr<PXR_NS::UsdImagingGLEngine> engine = makeEngine(threadBudget, error);
    if (!engine) {
        return -1;
    }
    MoonRayAovs::LogCapture capture;
    std::atomic<bool> done(false);
    std::atomic<int> peak(0);
    std::thread sampler([&]() {
        while (!done.load()) {
            for (int pid : execCompChildren()) {
                peak = std::max(peak.load(), threadCount(pid));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    const bool rendered = converge(engine.get(), stage, capture, nullptr, error);
    for (int pid : execCompChildren()) {
        peak = std::max(peak.load(), threadCount(pid));
    }
    done = true;
    sampler.join();

    return rendered ? peak.load() : -1;
}
} // anon namespace

TEST(USDRenderMoonRay, BeautyAndDepth)
{
    if (!moonRayOrSkip()) {
        return;
    }
    const PXR_NS::UsdStageRefPtr stage = fixture();
    ASSERT_TRUE(bool(stage));
    Frame frame;
    std::string error;
    ASSERT_TRUE(render(stage, 0, &frame, &error)) << error;

    const std::size_t left = pixel(kRow, kLeftColumn);
    const float* rgba = &frame.rgba[left * 4];
    EXPECT_GT(rgba[0] + rgba[1] + rgba[2], 0.f);
    EXPECT_NEAR(1.f, rgba[3], 1e-3f);
    EXPECT_NEAR(kFaceDistance, frame.depth[left], 0.01f);
    EXPECT_NEAR(kFaceDistance, frame.depth[pixel(kRow, kRightColumn)], 0.01f);

    const std::size_t miss = pixel(kRow, kMissColumn);
    EXPECT_TRUE(std::isinf(frame.depth[miss])) << frame.depth[miss];
    EXPECT_EQ(0.f, frame.rgba[miss * 4 + 3]);
}

TEST(USDRenderMoonRay, PrimIdAndNormal)
{
    if (!moonRayOrSkip()) {
        return;
    }
    const PXR_NS::UsdStageRefPtr stage = fixture();
    ASSERT_TRUE(bool(stage));
    std::string error;
    std::unique_ptr<PXR_NS::UsdImagingGLEngine> engine = makeEngine(0, &error);
    ASSERT_TRUE(engine != nullptr) << error;
    MoonRayAovs::LogCapture capture;
    ASSERT_TRUE(converge(engine.get(), stage, capture, nullptr, &error)) << error;
    std::vector<double> primId, instanceId, normal;
    ASSERT_TRUE(readAov(engine.get(), PXR_NS::HdAovTokens->primId, 1, &primId, &error)) << error;
    ASSERT_TRUE(readAov(engine.get(), PXR_NS::HdAovTokens->instanceId, 1, &instanceId, &error)) << error;
    ASSERT_TRUE(readAov(engine.get(), MoonRayAovs::normalAov(), 3, &normal, &error)) << error;

    // hdMoonray writes Hydra's own prim ids, so the engine decodes them as it does Storm's.
    const std::size_t left = pixel(kRow, kLeftColumn);
    const std::size_t right = pixel(kRow, kRightColumn);
    ASSERT_NE(primId[left], primId[right]);
    PXR_NS::SdfPath leftPath, rightPath;
    ASSERT_TRUE(engine->DecodeIntersection((int)primId[left], (int)instanceId[left], &leftPath));
    ASSERT_TRUE(engine->DecodeIntersection((int)primId[right], (int)instanceId[right], &rightPath));
    EXPECT_EQ(PXR_NS::SdfPath("/World/Left"), leftPath);
    EXPECT_EQ(PXR_NS::SdfPath("/World/Right"), rightPath);
    EXPECT_EQ(-1., primId[pixel(kRow, kMissColumn)]);

    // The faces point down world +X, back at the camera: in its space, +Z.
    for (std::size_t at : { left, right }) {
        EXPECT_NEAR(0., normal[at * 3 + 0], 0.01);
        EXPECT_NEAR(0., normal[at * 3 + 1], 0.01);
        EXPECT_NEAR(1., normal[at * 3 + 2], 0.01);
    }
}

TEST(USDRenderMoonRay, WarnsOfAnUnsupportedLightAndStillRenders)
{
    if (!moonRayOrSkip()) {
        return;
    }
    const PXR_NS::UsdStageRefPtr stage = fixture();
    ASSERT_TRUE(bool(stage));
    (void)PXR_NS::UsdLuxPortalLight::Define(stage, PXR_NS::SdfPath("/World/Portal"));

    const PXR_NS::TfTokenVector& supported = MoonRayAovs::supportedPrimTypes();
    ASSERT_FALSE(supported.empty());
    const std::vector<MoonRayAovs::Light> lights = MoonRayAovs::unsupportedLights(stage, supported);
    ASSERT_EQ(1u, lights.size());
    EXPECT_EQ(PXR_NS::SdfPath("/World/Portal"), lights[0].path);

    Frame frame;
    std::string error;
    ASSERT_TRUE(render(stage, 0, &frame, &error)) << error;
    const std::string warning = MoonRayAovs::warningText(stage, lights, frame.notices);
    EXPECT_NE(std::string::npos, warning.find("/World/Portal (PortalLight)")) << warning;
    // The cubes and their other lights render as before.
    const float* rgba = &frame.rgba[pixel(kRow, kLeftColumn) * 4];
    EXPECT_GT(rgba[0] + rgba[1] + rgba[2], 0.f);
    EXPECT_TRUE(MoonRayAovs::errorText(stage, frame.notices).empty()) << MoonRayAovs::errorText(stage, frame.notices);
}

// Destroying the engine is how a render is abandoned: hdMoonray has no stop, and the engine's
// Arras session ends its execComp as it goes.
TEST(USDRenderMoonRay, AbortEndsTheRenderProcessPromptly)
{
    if (!moonRayOrSkip()) {
        return;
    }
    const PXR_NS::UsdStageRefPtr stage = fixture();
    ASSERT_TRUE(bool(stage));
    std::string error;
    std::unique_ptr<PXR_NS::UsdImagingGLEngine> engine = makeEngine(0, &error);
    ASSERT_TRUE(engine != nullptr) << error;
    // Enough samples that the frame is still rendering when it is abandoned.
    engine->SetRendererSetting(PXR_NS::TfToken("moonray:sceneVariable:pixel_samples"), PXR_NS::VtValue(64));
    MoonRayAovs::LogCapture capture;
    std::atomic<bool> cancel(false);
    std::thread canceller([&cancel]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        cancel = true;
    });
    const bool ended = converge(engine.get(), stage, capture, &cancel, &error);
    canceller.join();
    ASSERT_TRUE(ended) << error;

    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    engine.reset();
    while (!execCompChildren().empty() && (std::chrono::steady_clock::now() - start < std::chrono::seconds(1))) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    EXPECT_LT(seconds, 1.);
    EXPECT_TRUE(execCompChildren().empty());
}

// Arras starts the render on hardware_concurrency() minus the reserved cores, so a budget of two
// threads gives a render process with fewer threads than an unbudgeted one, by about the cores
// the budget leaves out; the process's own service threads are the same in both.
TEST(USDRenderMoonRay, RendersWithinTheThreadBudget)
{
    if (!moonRayOrSkip()) {
        return;
    }
    const int hardware = (int)std::thread::hardware_concurrency();
    if (hardware < 6) {
        std::printf("[  SKIPPED ] %d hardware threads leave no room to tell a budget of 2 apart\n", hardware);

        return;
    }
    std::string error;
    const int budgeted = peakRenderThreads(2, &error);
    ASSERT_GT(budgeted, 0) << error;
    const int unbudgeted = peakRenderThreads(0, &error);
    ASSERT_GT(unbudgeted, 0) << error;
    // The unbudgeted render reserves one core, the budgeted one hardware - 2.
    EXPECT_GE(unbudgeted - budgeted, (hardware - 3) / 2) << "budgeted " << budgeted << ", unbudgeted " << unbudgeted;
}

// The fixture rendered twice gives the same image: the same frame seeds the same samples.
TEST(USDRenderMoonRay, RerunIsIdentical)
{
    if (!moonRayOrSkip()) {
        return;
    }
    const PXR_NS::UsdStageRefPtr stage = fixture();
    ASSERT_TRUE(bool(stage));
    Frame first, second;
    std::string error;
    ASSERT_TRUE(render(stage, 0, &first, &error)) << error;
    ASSERT_TRUE(render(stage, 0, &second, &error)) << error;
    ASSERT_EQ(first.rgba.size(), second.rgba.size());
    std::size_t differing = 0;
    for (std::size_t i = 0; i < first.rgba.size(); ++i) {
        differing += (first.rgba[i] != second.rgba[i]) ? 1 : 0;
    }
    EXPECT_EQ(0u, differing);
    EXPECT_EQ(first.primId, second.primId);
}
