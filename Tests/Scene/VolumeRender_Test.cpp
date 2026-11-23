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

#include "Global/GlobalDefines.h"
#include "Global/Macros.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
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

// scene-volume.usda: an unbound volume in front of a white emissive card, seen through /World/Cam
// at kSize x kSize. Its density and velocity fields come from volume-sphere.0001.vdb on frame 1,
// a sphere left of centre, and volume-sphere.0002.vdb on frame 2, the same sphere right of it
// (make-volume-fixtures.py). The fixtures are committed rather than written here: writing them
// would initialise OpenVDB in this process, which the Cycles delegate has to do itself.
namespace {
const char* const kScene = NATRON_TESTS_FIXTURES_DIR "/scene-volume.usda";
const char* const kCamera = "/World/Cam";
const int kSize = 64;
// The camera sees 15.27 pixels per unit at the volume: the sphere's centre, x = -0.625 on frame 1
// and +0.625 on frame 2, falls on column 22 or 41 of the middle rows.
const int kLeftX = 22;
const int kRightX = 41;
const int kMiddleY = 32;
const int kHalfWindow = 2;
// Just right of the frame 1 sphere's right edge (x = -0.125, column 30), where a sphere moving
// right blurs into.
const int kTrailX0 = 31;
const int kTrailX1 = 34;

const char* const kMeshOnly = "over \"World\"\n"
                              "{\n"
                              "    over \"Smoke\" (\n"
                              "        active = false\n"
                              "    )\n"
                              "    {\n"
                              "    }\n"
                              "}\n";
const char* const kWrongGridName = "over \"World\"\n"
                                   "{\n"
                                   "    over \"Smoke\"\n"
                                   "    {\n"
                                   "        over \"density\"\n"
                                   "        {\n"
                                   "            token fieldName = \"nothing\"\n"
                                   "        }\n"
                                   "    }\n"
                                   "}\n";
const char* const kOpenShutter = "over \"World\"\n"
                                 "{\n"
                                 "    over \"Cam\"\n"
                                 "    {\n"
                                 "        double shutter:close = 0.25\n"
                                 "        double shutter:open = -0.25\n"
                                 "    }\n"
                                 "}\n";

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

// Fixed sampling for the path tracers while a test runs.
class RendererSettings {
public:
    explicit RendererSettings(const std::string& renderer)
        : _renderer(renderer)
    {
        if (renderer == kRenderDelegateCyclesId) {
            HydraRender::setRendererSettings(renderer, CyclesTestUtils::fixedSampling(16));
        } else if (renderer == kMoonRayRendererId) {
            std::map<std::string, PXR_NS::VtValue> settings;
            settings["moonray:sceneVariable:pixel_samples"] = PXR_NS::VtValue(4);
            HydraRender::setRendererSettings(renderer, settings);
        }
    }

    ~RendererSettings()
    {
        HydraRender::setRendererSettings(_renderer, std::map<std::string, PXR_NS::VtValue>());
        HydraRender::releaseAllCpuEngines();
    }

private:
    std::string _renderer;
};

struct Render {
    std::vector<float> rgba;
    std::string warning;
};

// kScene at time with overrides layered on top, rendered by renderer. False after a gtest failure.
bool
renderScene(const std::string& renderer,
            double time,
            const std::string& overrides,
            Render* render)
{
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(kScene);
    if (!stage) {
        ADD_FAILURE() << "the volume scene did not open";

        return false;
    }
    static const int owner = 0;
    HydraRender::Request request;
    request.owner = &owner;
    request.scene = SceneOps::appendLayer(ScenePayload(), stage->GetRootLayer(), 1);
    if (!overrides.empty()) {
        PXR_NS::SdfLayerRefPtr layer = PXR_NS::SdfLayer::CreateAnonymous(".usda");
        if (!layer->ImportFromString("#usda 1.0\n" + overrides)) {
            ADD_FAILURE() << "the overrides do not parse:\n"
                          << overrides;

            return false;
        }
        request.scene = SceneOps::appendLayer(request.scene, layer, (U64)std::hash<std::string>()(overrides) | 2);
    }
    request.time = PXR_NS::UsdTimeCode(time);
    request.width = kSize;
    request.height = kSize;
    request.renderer = renderer;
    std::string error;
    if (!HydraRender::cameraFrustum(stage, PXR_NS::SdfPath(kCamera), request.time, 1., HydraRender::eFitModeExpand, &request.frustum, &error)) {
        ADD_FAILURE() << error;

        return false;
    }
    request.camera = PXR_NS::SdfPath(kCamera);
    request.fit = HydraRender::eFitModeExpand;
    request.pixelAspect = 1.;
    HydraRender::Result result;
    const bool rendered = HydraRender::render(request, &result, &error);
    HydraRender::release(&owner);
    if (!rendered) {
        ADD_FAILURE() << renderer << " at " << time << ": " << error;

        return false;
    }
    if (result.rgba.size() != (std::size_t)kSize * (std::size_t)kSize * 4) {
        ADD_FAILURE() << renderer << " rendered " << result.rgba.size() << " values";

        return false;
    }
    render->rgba = result.rgba;
    render->warning = result.warning;

    return true;
}

// The mean of the red, green and blue of columns x0 to x1 of the rows around the middle.
float
meanOf(const Render& render,
       int x0,
       int x1)
{
    double sum = 0.;
    int count = 0;
    for (int y = kMiddleY - kHalfWindow; y <= kMiddleY + kHalfWindow; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const std::size_t i = ((std::size_t)y * (std::size_t)kSize + (std::size_t)x) * 4;
            sum += render.rgba[i] + render.rgba[i + 1] + render.rgba[i + 2];
            count += 3;
        }
    }

    return (float)(sum / count);
}

float
leftOf(const Render& render)
{
    return meanOf(render, kLeftX - kHalfWindow, kLeftX + kHalfWindow);
}

float
rightOf(const Render& render)
{
    return meanOf(render, kRightX - kHalfWindow, kRightX + kHalfWindow);
}

bool
hasVolumeWarning(const Render& render)
{
    return render.warning.find("volume fields") != std::string::npos;
}

/**
 * The volume darkens the card behind the sphere on its own side only: the left on frame 1, the
 * right on frame 2, against the card without the volume. The volume is rendered first: for Cycles
 * that is the process's first OpenVDB read, which the delegate must have initialised the library
 * for, whereas the warning check after it initialises it for every later one.
 **/
void
expectTheVolumeDarkensItsSide(const std::string& renderer)
{
    const RendererSettings settings(renderer);
    Render frame1;
    ASSERT_TRUE(renderScene(renderer, 1., std::string(), &frame1));
    EXPECT_FALSE(hasVolumeWarning(frame1)) << frame1.warning;
    Render frame2;
    ASSERT_TRUE(renderScene(renderer, 2., std::string(), &frame2));
    EXPECT_FALSE(hasVolumeWarning(frame2)) << frame2.warning;
    Render card;
    ASSERT_TRUE(renderScene(renderer, 1., kMeshOnly, &card));
    ASSERT_GT(leftOf(card), 0.1f) << renderer << " does not draw the card";
    ASSERT_GT(rightOf(card), 0.1f) << renderer << " does not draw the card";

    EXPECT_LT(leftOf(frame1), 0.6f * leftOf(card)) << renderer << ": frame 1 left " << leftOf(frame1) << ", card " << leftOf(card);
    EXPECT_GT(rightOf(frame1), 0.9f * rightOf(card)) << renderer << ": frame 1 right " << rightOf(frame1) << ", card " << rightOf(card);
    EXPECT_GT(leftOf(frame2), 0.9f * leftOf(card)) << renderer << ": frame 2 left " << leftOf(frame2) << ", card " << leftOf(card);
    EXPECT_LT(rightOf(frame2), 0.6f * rightOf(card)) << renderer << ": frame 2 right " << rightOf(frame2) << ", card " << rightOf(card);
}

// A density field naming a grid its file lacks draws nothing, and the render warns of it.
void
expectAWrongGridNameDrawsNothing(const std::string& renderer)
{
    const RendererSettings settings(renderer);
    Render wrong;
    ASSERT_TRUE(renderScene(renderer, 1., kWrongGridName, &wrong));
    Render card;
    ASSERT_TRUE(renderScene(renderer, 1., kMeshOnly, &card));
    EXPECT_GT(leftOf(wrong), 0.9f * leftOf(card)) << renderer << ": left " << leftOf(wrong) << ", card " << leftOf(card);
    EXPECT_GT(rightOf(wrong), 0.9f * rightOf(card)) << renderer << ": right " << rightOf(wrong) << ", card " << rightOf(card);
    EXPECT_NE(std::string::npos, wrong.warning.find("finds no grid of the given name for volume fields: /World/Smoke (density): nothing")) << wrong.warning;
}

// With the shutter open a quarter frame either side of frame 1, the velocity field blurs the
// sphere along its motion, into the card right of where it sits at the frame.
void
expectVelocityBlursTheVolume(const std::string& renderer)
{
    const RendererSettings settings(renderer);
    Render still;
    ASSERT_TRUE(renderScene(renderer, 1., std::string(), &still));
    Render blurred;
    ASSERT_TRUE(renderScene(renderer, 1., kOpenShutter, &blurred));
    const float stillTrail = meanOf(still, kTrailX0, kTrailX1);
    const float blurredTrail = meanOf(blurred, kTrailX0, kTrailX1);
    EXPECT_LT(blurredTrail, 0.95f * stillTrail) << renderer << ": blurred " << blurredTrail << ", still " << stillTrail;
}
} // anon namespace

// Each renderer has a suite of its own, so each runs in its own process: Storm's OpenVDB reader
// initialises the library, which would hide a Cycles delegate that does not.
TEST(VolumeRenderStorm, TheVolumeDarkensItsSide)
{
    if (skipWithoutGL()) {
        return;
    }
    expectTheVolumeDarkensItsSide(kRenderDelegateStormId);
}

TEST(VolumeRenderStorm, AWrongGridNameDrawsNothing)
{
    if (skipWithoutGL()) {
        return;
    }
    expectAWrongGridNameDrawsNothing(kRenderDelegateStormId);
}

TEST(VolumeRenderCycles, TheVolumeDarkensItsSide)
{
    if (!CyclesTestUtils::cyclesOrSkip()) {
        return;
    }
    expectTheVolumeDarkensItsSide(kRenderDelegateCyclesId);
}

TEST(VolumeRenderCycles, AWrongGridNameDrawsNothing)
{
    if (!CyclesTestUtils::cyclesOrSkip()) {
        return;
    }
    expectAWrongGridNameDrawsNothing(kRenderDelegateCyclesId);
}

// Disabled: the Cycles delegate never turns on integrator motion blur, and leaves the volume's
// velocity scale at 1, which reads the per-second velocity field as per shutter interval.
TEST(VolumeRenderCycles, DISABLED_VelocityBlursTheVolume)
{
    if (!CyclesTestUtils::cyclesOrSkip()) {
        return;
    }
    expectVelocityBlursTheVolume(kRenderDelegateCyclesId);
}

TEST(VolumeRenderMoonRay, TheVolumeDarkensItsSide)
{
    if (!moonRayOrSkip()) {
        return;
    }
    expectTheVolumeDarkensItsSide(kMoonRayRendererId);
}

TEST(VolumeRenderMoonRay, AWrongGridNameDrawsNothing)
{
    if (!moonRayOrSkip()) {
        return;
    }
    expectAWrongGridNameDrawsNothing(kMoonRayRendererId);
}

TEST(VolumeRenderMoonRay, VelocityBlursTheVolume)
{
    if (!moonRayOrSkip()) {
        return;
    }
    expectVelocityBlursTheVolume(kMoonRayRendererId);
}
