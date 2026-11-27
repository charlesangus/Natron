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
#include "Engine/Nodes/Scene/DelegateSupport.h"
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

// An aborted MoonRay render destroys its engine while frames still stream in over Arras. The
// client's message delivery thread must be stopped before the endpoints it reads through are
// freed, which only shows up as a crash some of the time, so the teardown is repeated.
TEST(RenderDelegates, AbortingMoonRayRendersMidPassRepeatedlyTearsDownCleanly)
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
        static const int kRounds = 8;
        for (int round = 0; round < kRounds; ++round) {
            const std::uint64_t builds = HydraRender::getEngineBuildCount();
            expectAbortReturnsPromptly("MoonRay", request, 2, 0);
            EXPECT_LT(builds, HydraRender::getEngineBuildCount()) << "round " << round << " did not make a new MoonRay session";
        }
    }
    HydraRender::release(&owner);
    EXPECT_EQ(0u, HydraRender::getCpuEngineCount());
    HydraRender::setRendererSettings(kMoonRayRendererId, std::map<std::string, PXR_NS::VtValue>());
}

namespace {
PXR_NS::UsdStageRefPtr
stageOf(const std::string& body)
{
    PXR_NS::SdfLayerRefPtr layer = PXR_NS::SdfLayer::CreateAnonymous(".usda");
    if (!layer->ImportFromString("#usda 1.0\n" + body)) {
        ADD_FAILURE() << "the layer does not parse:\n"
                      << body;

        return PXR_NS::UsdStageRefPtr();
    }

    return PXR_NS::UsdStage::Open(layer);
}

// A delegate that renders meshes, volumes and materials, reading the cycles render context.
DelegateSupport::Support
cyclesLikeSupport()
{
    DelegateSupport::Support support;
    support.primTypes = { PXR_NS::TfToken("mesh"), PXR_NS::TfToken("volume"), PXR_NS::TfToken("material") };
    support.materialRenderContexts = { PXR_NS::TfToken("cycles") };

    return support;
}

// A card bound to /Looks/Mat, which has outputs.
std::string
boundCard(const std::string& outputs)
{
    return "def Mesh \"Card\" (\n"
           "    prepend apiSchemas = [\"MaterialBindingAPI\"]\n"
           ")\n"
           "{\n"
           "    int[] faceVertexCounts = [4]\n"
           "    int[] faceVertexIndices = [0, 1, 2, 3]\n"
           "    point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]\n"
           "    rel material:binding = </Looks/Mat>\n"
           "}\n"
           "def Scope \"Looks\"\n"
           "{\n"
           "    def Material \"Mat\"\n"
           "    {\n"
        + outputs + "        def Shader \"Volume\"\n"
                    "        {\n"
                    "            uniform token info:id = \"principled_volume\"\n"
                    "            token outputs:volume\n"
                    "        }\n"
                    "        def Shader \"Surface\"\n"
                    "        {\n"
                    "            uniform token info:id = \"PxrSurface\"\n"
                    "            token outputs:out\n"
                    "        }\n"
                    "    }\n"
                    "}\n";
}

const char* const kCyclesVolumeOutput = "        token outputs:cycles:volume.connect = </Looks/Mat/Volume.outputs:volume>\n";
const char* const kRiSurfaceOutput = "        token outputs:ri:surface.connect = </Looks/Mat/Surface.outputs:out>\n";
const char* const kRiVolumeOutput = "        token outputs:ri:volume.connect = </Looks/Mat/Volume.outputs:volume>\n";

std::string
supportWarning(const std::string& body)
{
    const PXR_NS::UsdStageRefPtr stage = stageOf(body);

    return stage ? DelegateSupport::warningText(stage, cyclesLikeSupport(), "Test") : std::string("no stage");
}

struct Field {
    std::string name;
    std::string file;
    std::string grid;
    std::string type = "OpenVDBAsset";
};

std::string
volumeScene(const std::vector<Field>& fields)
{
    std::string body = "def Volume \"Vol\"\n{\n";
    for (const Field& field : fields) {
        body += "    rel field:" + field.name + " = </Vol/" + field.name + ">\n";
    }
    for (const Field& field : fields) {
        body += "    def " + field.type + " \"" + field.name + "\"\n    {\n";
        body += "        asset filePath = @" + field.file + "@\n";
        body += "        token fieldName = \"" + field.grid + "\"\n    }\n";
    }

    return body + "}\n";
}

std::string
volumeWarning(const std::string& body,
              const DelegateSupport::VolumeLimits& limits,
              double time = 1.)
{
    const PXR_NS::UsdStageRefPtr stage = stageOf(body);
    if (!stage) {
        return "no stage";
    }
    const std::vector<PXR_NS::SdfPath> volumes = DelegateSupport::volumePrims(stage);
    EXPECT_EQ(1u, volumes.size());

    return DelegateSupport::volumeWarningText(stage, volumes, PXR_NS::UsdTimeCode(time), limits, "Test");
}

const std::string kFrame1Vdb = NATRON_TESTS_FIXTURES_DIR "/volume-sphere.0001.vdb";
const std::string kFrame2Vdb = NATRON_TESTS_FIXTURES_DIR "/volume-sphere.0002.vdb";

bool
has(const std::string& text,
    const std::string& part)
{
    return text.find(part) != std::string::npos;
}
} // anon namespace

TEST(DelegateSupportWarnings, AVolumeOnlyMaterialHasNoSurfaceWarning)
{
    EXPECT_EQ(std::string(), supportWarning(boundCard(kCyclesVolumeOutput)));
}

// The surface is for another renderer, but the volume is for this one.
TEST(DelegateSupportWarnings, AVolumeForTheRenderersContextsCountsAsItsShading)
{
    EXPECT_EQ(std::string(), supportWarning(boundCard(std::string(kRiSurfaceOutput) + kCyclesVolumeOutput)));
}

TEST(DelegateSupportWarnings, AMaterialWithNeitherForTheRenderersContextsStillWarns)
{
    const std::string warning = supportWarning(boundCard(kRiSurfaceOutput));
    EXPECT_TRUE(has(warning, "finds no surface")) << warning;
    EXPECT_TRUE(has(warning, "/Looks/Mat")) << warning;
}

TEST(DelegateSupportWarnings, AMaterialWithOnlyAnotherRenderersVolumeWarns)
{
    const std::string warning = supportWarning(boundCard(kRiVolumeOutput));
    EXPECT_TRUE(has(warning, "finds no surface or volume")) << warning;
    EXPECT_TRUE(has(warning, "/Looks/Mat")) << warning;
}

TEST(DelegateSupportWarnings, AValidVolumeRaisesNoWarning)
{
    const std::string body = volumeScene({ { "density", kFrame1Vdb, "density" }, { "velocity", kFrame1Vdb, "velocity" } });
    EXPECT_EQ(std::string(), volumeWarning(body, DelegateSupport::VolumeLimits()));
    EXPECT_EQ(std::string(), volumeWarning(body, DelegateSupport::VolumeLimits::moonRay()));
}

TEST(DelegateSupportWarnings, AMissingFileIsNamedByVolumeAndField)
{
    const std::string warning = volumeWarning(volumeScene({ { "density", NATRON_TESTS_FIXTURES_DIR "/no-such-volume.vdb", "density" } }), DelegateSupport::VolumeLimits());
    EXPECT_TRUE(has(warning, "Test finds no file for volume fields: /Vol (density)")) << warning;
    EXPECT_TRUE(has(warning, "no-such-volume.vdb")) << warning;
}

TEST(DelegateSupportWarnings, AFileThatIsNotOpenVdbIsUnreadable)
{
    const std::string warning = volumeWarning(volumeScene({ { "density", NATRON_TESTS_FIXTURES_DIR "/scene-volume.usda", "density" } }), DelegateSupport::VolumeLimits());
    EXPECT_TRUE(has(warning, "Test cannot read the OpenVDB files of volume fields: /Vol (density)")) << warning;
}

TEST(DelegateSupportWarnings, AGridNotInItsFileIsNamed)
{
    const std::string warning = volumeWarning(volumeScene({ { "density", kFrame1Vdb, "smoke" } }), DelegateSupport::VolumeLimits());
    EXPECT_TRUE(has(warning, "Test finds no grid of the given name for volume fields: /Vol (density): smoke in ")) << warning;
    EXPECT_TRUE(has(warning, "volume-sphere.0001.vdb")) << warning;
}

TEST(DelegateSupportWarnings, AFieldThatIsNotOpenVdbIsNamed)
{
    Field field = { "density", kFrame1Vdb, "density" };
    field.type = "Field3DAsset";
    const std::string warning = volumeWarning(volumeScene({ field }), DelegateSupport::VolumeLimits());
    EXPECT_TRUE(has(warning, "Test reads only OpenVDB volume fields, not: /Vol (density)")) << warning;
}

// Any field name is fine without limits; MoonRay renders only three.
TEST(DelegateSupportWarnings, FieldNamesOutsideTheLimitsAreNamed)
{
    const std::string body = volumeScene({ { "density", kFrame1Vdb, "density" }, { "temperature", kFrame1Vdb, "temperature" } });
    EXPECT_EQ(std::string(), volumeWarning(body, DelegateSupport::VolumeLimits()));
    const std::string warning = volumeWarning(body, DelegateSupport::VolumeLimits::moonRay());
    EXPECT_TRUE(has(warning, "Test renders only density, emission and velocity volume fields and skips: /Vol (temperature)")) << warning;
    EXPECT_FALSE(has(warning, "/Vol (density)")) << warning;
}

TEST(DelegateSupportWarnings, FieldsFromSeveralFilesWarnOnlyUnderASingleFileLimit)
{
    const std::string body = volumeScene({ { "density", kFrame1Vdb, "density" }, { "velocity", kFrame2Vdb, "velocity" } });
    EXPECT_EQ(std::string(), volumeWarning(body, DelegateSupport::VolumeLimits()));
    const std::string warning = volumeWarning(body, DelegateSupport::VolumeLimits::moonRay());
    EXPECT_TRUE(has(warning, "Test reads each volume's fields from one file and cannot render volumes whose fields span several: /Vol")) << warning;
}

// A field's file can change in time: the check is of the file at the time asked for.
TEST(DelegateSupportWarnings, ATimeSampledFileIsCheckedAtTheTimeAskedFor)
{
    const std::string body = "def Volume \"Vol\"\n"
                             "{\n"
                             "    rel field:density = </Vol/density>\n"
                             "    def OpenVDBAsset \"density\"\n"
                             "    {\n"
                             "        asset filePath.timeSamples = {\n"
                             "            1: @"
        + kFrame1Vdb + "@,\n"
                       "            2: @" NATRON_TESTS_FIXTURES_DIR "/no-such-volume.vdb@,\n"
                       "        }\n"
                       "        token fieldName = \"density\"\n"
                       "    }\n"
                       "}\n";
    EXPECT_EQ(std::string(), volumeWarning(body, DelegateSupport::VolumeLimits(), 1.));
    const std::string warning = volumeWarning(body, DelegateSupport::VolumeLimits(), 2.);
    EXPECT_TRUE(has(warning, "finds no file for volume fields: /Vol (density)")) << warning;
}

TEST(DelegateSupportWarnings, AFieldWithNoPrimIsNamed)
{
    const std::string body = "def Volume \"Vol\"\n"
                             "{\n"
                             "    rel field:density = </Vol/missing>\n"
                             "}\n";
    const std::string warning = volumeWarning(body, DelegateSupport::VolumeLimits());
    EXPECT_TRUE(has(warning, "Test finds no field prim for volume fields: /Vol (density): /Vol/missing")) << warning;
}

// The prototype is walked once, and its volume is named by an instance, not by the prototype.
TEST(DelegateSupportWarnings, AnInstancedVolumeIsFoundOnceByAnInstance)
{
    const std::string body = "class Xform \"Proto\"\n"
                             "{\n"
                             "    def Volume \"Vol\"\n"
                             "    {\n"
                             "        rel field:density = </Proto/Vol/density>\n"
                             "        def OpenVDBAsset \"density\"\n"
                             "        {\n"
                             "            asset filePath = @" NATRON_TESTS_FIXTURES_DIR "/no-such-volume.vdb@\n"
                             "            token fieldName = \"density\"\n"
                             "        }\n"
                             "    }\n"
                             "}\n"
                             "def Xform \"A\" (\n"
                             "    instanceable = true\n"
                             "    prepend references = </Proto>\n"
                             ")\n"
                             "{\n"
                             "}\n"
                             "def Xform \"B\" (\n"
                             "    instanceable = true\n"
                             "    prepend references = </Proto>\n"
                             ")\n"
                             "{\n"
                             "}\n";
    const PXR_NS::UsdStageRefPtr stage = stageOf(body);
    ASSERT_TRUE(bool(stage));
    ASSERT_EQ(1u, stage->GetPrototypes().size());
    const std::vector<PXR_NS::SdfPath> volumes = DelegateSupport::volumePrims(stage);
    ASSERT_EQ(1u, volumes.size());
    EXPECT_TRUE((volumes[0] == PXR_NS::SdfPath("/A/Vol")) || (volumes[0] == PXR_NS::SdfPath("/B/Vol"))) << volumes[0].GetString();
    const std::string warning = DelegateSupport::volumeWarningText(stage, volumes, PXR_NS::UsdTimeCode(1.), DelegateSupport::VolumeLimits(), "Test");
    EXPECT_TRUE(has(warning, "Test finds no file for volume fields: " + volumes[0].GetString() + " (density)")) << warning;
}

TEST(DelegateSupportWarnings, AVolumeInAVariantIsFound)
{
    const std::string body = "def Xform \"Root\" (\n"
                             "    variants = {\n"
                             "        string look = \"smoke\"\n"
                             "    }\n"
                             "    prepend variantSets = \"look\"\n"
                             ")\n"
                             "{\n"
                             "    variantSet \"look\" = {\n"
                             "        \"smoke\" {\n"
                             "            def Volume \"Vol\"\n"
                             "            {\n"
                             "            }\n"
                             "        }\n"
                             "    }\n"
                             "}\n";
    const PXR_NS::UsdStageRefPtr stage = stageOf(body);
    ASSERT_TRUE(bool(stage));
    const std::vector<PXR_NS::SdfPath> volumes = DelegateSupport::volumePrims(stage);
    ASSERT_EQ(1u, volumes.size());
    EXPECT_EQ(PXR_NS::SdfPath("/Root/Vol"), volumes[0]);
}

TEST(DelegateSupportWarnings, ACancelledSearchIsDropped)
{
    const PXR_NS::UsdStageRefPtr stage = stageOf(volumeScene({ { "density", NATRON_TESTS_FIXTURES_DIR "/no-such-volume.vdb", "density" } }));
    ASSERT_TRUE(bool(stage));
    const std::vector<PXR_NS::SdfPath> volumes = DelegateSupport::volumePrims(stage);
    ASSERT_EQ(1u, volumes.size());
    const std::atomic<bool> cancel(true);
    EXPECT_TRUE(DelegateSupport::volumePrims(stage, &cancel).empty());
    EXPECT_EQ(std::string(), DelegateSupport::volumeWarningText(stage, volumes, PXR_NS::UsdTimeCode(1.), DelegateSupport::VolumeLimits(), "Test", &cancel));
}

// Every line a volume warning has is told apart from the other warnings a render carries.
TEST(DelegateSupportWarnings, EveryVolumeWarningLineIsRecognised)
{
    Field notOpenVdb = { "emission", kFrame1Vdb, "density" };
    notOpenVdb.type = "Field3DAsset";
    std::string body = volumeScene({ { "density", NATRON_TESTS_FIXTURES_DIR "/no-such-volume.vdb", "density" },
                                     { "velocity", NATRON_TESTS_FIXTURES_DIR "/scene-volume.usda", "velocity" },
                                     { "temperature", kFrame1Vdb, "smoke" },
                                     { "heat", kFrame2Vdb, "density" },
                                     notOpenVdb });
    body.insert(body.find("{\n") + 2, "    rel field:flame = </Vol/missing>\n");
    const std::string warning = volumeWarning(body, DelegateSupport::VolumeLimits::moonRay());
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < warning.size()) {
        const size_t end = std::min(warning.find('\n', start), warning.size());
        lines.push_back(warning.substr(start, end - start));
        start = end + 1;
    }
    EXPECT_EQ(7u, lines.size()) << warning;
    for (const std::string& line : lines) {
        EXPECT_TRUE(DelegateSupport::isVolumeWarningLine(line)) << line;
    }
    EXPECT_FALSE(DelegateSupport::isVolumeWarningLine("Test cannot render prims of type volume: /Vol"));
    EXPECT_FALSE(DelegateSupport::isVolumeWarningLine("Test finds no surface or volume for its render contexts in materials: /Looks/Mat"));
}
