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

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

#include <pxr/base/plug/registry.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include "CyclesTestUtils.h"
#include "Engine/Nodes/Scene/CyclesAovs.h"
#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/MoonRayAovs.h"
#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/ScenePayload.h"

NATRON_NAMESPACE_USING

namespace {
const char* const kMatchLightLevels = "natron:matchLightLevels";
const int kSize = 32;
const double kAlbedo = 0.8;
// The levels of the three renderers may differ by this fraction of the expected level, and the
// sampling noise of the path tracers by much less.
const double kTolerance = 0.05;

// The dev image's /usr/local glvnd lists only NVIDIA; the system directory holds Mesa. Set before
// the first EGL use, as the render thread's context is made once.
void
pointEGLAtMesa()
{
    if (!std::getenv("__EGL_VENDOR_LIBRARY_FILENAMES") && !std::getenv("__EGL_VENDOR_LIBRARY_DIRS") && (::access("/usr/share/glvnd/egl_vendor.d", F_OK) == 0) && (::access("/usr/local/share/glvnd/egl_vendor.d", F_OK) == 0)) {
        ::setenv("__EGL_VENDOR_LIBRARY_DIRS", "/usr/share/glvnd/egl_vendor.d", 1);
    }
}

// False when Storm cannot render here: no 4.5 context could be made, which fails the test only
// under NATRON_TEST_REQUIRE_GL=1.
bool
stormOrSkip()
{
    pointEGLAtMesa();
    std::string error;
    if (!HydraRender::contextDescription(&error).empty()) {
        return true;
    }
    const char* require = std::getenv("NATRON_TEST_REQUIRE_GL");
    if (require && (std::strcmp(require, "1") == 0)) {
        ADD_FAILURE() << error << " (NATRON_TEST_REQUIRE_GL=1)";
    } else {
        std::printf("[  SKIPPED ] Storm: %s\n", error.c_str());
    }

    return false;
}

// The bundled gtest has no GTEST_SKIP, so the caller leaves MoonRay out on false;
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

// A constant white latlong for the dome, as Storm lights nothing with a dome that has no texture.
class WhiteEnvironment {
public:
    WhiteEnvironment()
    {
        static int counter = 0;
        _dir = std::filesystem::temp_directory_path() / ("cycles_light_levels_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
        std::filesystem::create_directories(_dir);
        _path = (_dir / "white.exr").string();

        const int width = 64;
        const int height = 32;
        const std::vector<float> pixels(width * height * 3, 1.f);
        std::unique_ptr<OIIO::ImageOutput> out = OIIO::ImageOutput::create(_path);
        if (!out) {
            ADD_FAILURE() << OIIO::geterror();

            return;
        }
        const OIIO::ImageSpec spec(width, height, 3, OIIO::TypeDesc::FLOAT);
        EXPECT_TRUE(out->open(_path, spec)) << out->geterror();
        EXPECT_TRUE(out->write_image(OIIO::TypeDesc::FLOAT, pixels.data())) << out->geterror();
        EXPECT_TRUE(out->close()) << out->geterror();
    }

    ~WhiteEnvironment()
    {
        std::error_code ignored;
        std::filesystem::remove_all(_dir, ignored);
    }

    const std::string& path() const
    {
        return _path;
    }

private:
    std::filesystem::path _dir;
    std::string _path;
};

// A grey diffuse card at z = 0 facing +Z, far wider than the frame of a camera at z = 10 looking
// down -Z, lit by light alone. The surface has no specular (an IOR of 1 for Cycles, which takes
// no specular colour), so each pixel is albedo / pi times the irradiance there.
std::string
litCard(const std::string& light)
{
    return std::string("#usda 1.0\n"
                       "(\n"
                       "    metersPerUnit = 1\n"
                       "    upAxis = \"Y\"\n"
                       ")\n"
                       "def Xform \"World\" {\n"
                       "    def Camera \"Cam\" {\n"
                       "        float focalLength = 50\n"
                       "        float horizontalAperture = 20.955\n"
                       "        float verticalAperture = 20.955\n"
                       "        float2 clippingRange = (0.1, 1000)\n"
                       "        double3 xformOp:translate = (0, 0, 10)\n"
                       "        uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
                       "    }\n"
                       "    def Mesh \"Card\" (prepend apiSchemas = [\"MaterialBindingAPI\"]) {\n"
                       "        uniform token subdivisionScheme = \"none\"\n"
                       "        int[] faceVertexCounts = [4]\n"
                       "        int[] faceVertexIndices = [0, 1, 2, 3]\n"
                       "        point3f[] points = [(-50, -50, 0), (50, -50, 0), (50, 50, 0), (-50, 50, 0)]\n"
                       "        normal3f[] normals = [(0, 0, 1), (0, 0, 1), (0, 0, 1), (0, 0, 1)] (interpolation = \"vertex\")\n"
                       "        rel material:binding = </World/Grey>\n"
                       "    }\n"
                       "    def Material \"Grey\" {\n"
                       "        token outputs:surface.connect = </World/Grey/Surface.outputs:surface>\n"
                       "        def Shader \"Surface\" {\n"
                       "            uniform token info:id = \"UsdPreviewSurface\"\n"
                       "            color3f inputs:diffuseColor = (0.8, 0.8, 0.8)\n"
                       "            int inputs:useSpecularWorkflow = 1\n"
                       "            color3f inputs:specularColor = (0, 0, 0)\n"
                       "            float inputs:roughness = 1\n"
                       "            float inputs:ior = 1\n"
                       "            token outputs:surface\n"
                       "        }\n"
                       "    }\n")
        + light + "}\n";
}

// The mean of the red, green and blue of every pixel of the card lit by light, rendered by
// renderer with settings. False after a gtest failure.
bool
meanLevel(const std::string& renderer,
          const std::string& light,
          const std::map<std::string, PXR_NS::VtValue>& settings,
          double* mean)
{
    PXR_NS::SdfLayerRefPtr layer = PXR_NS::SdfLayer::CreateAnonymous(".usda");
    if (!layer->ImportFromString(litCard(light))) {
        ADD_FAILURE() << "the lit card did not parse";

        return false;
    }
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(layer);
    if (!stage) {
        ADD_FAILURE() << "the lit card did not open";

        return false;
    }

    static const int owner = 0;
    HydraRender::Request request;
    request.owner = &owner;
    request.scene = SceneOps::appendLayer(ScenePayload(), layer, 1);
    request.time = PXR_NS::UsdTimeCode(1.);
    request.width = kSize;
    request.height = kSize;
    request.renderer = renderer;
    request.settings = settings;
    std::string error;
    if (!HydraRender::cameraFrustum(stage, PXR_NS::SdfPath("/World/Cam"), request.time, 1., HydraRender::eFitModeFill, &request.frustum, &error)) {
        ADD_FAILURE() << error;

        return false;
    }
    HydraRender::Result result;
    const bool rendered = HydraRender::render(request, &result, &error);
    HydraRender::release(&owner);
    if (!rendered) {
        ADD_FAILURE() << renderer << ": " << error;

        return false;
    }
    if (result.rgba.size() != std::size_t(kSize * kSize * 4)) {
        ADD_FAILURE() << renderer << " gave " << result.rgba.size() << " values for " << kSize << " x " << kSize << " pixels";

        return false;
    }
    double sum = 0.;
    for (std::size_t i = 0; i < result.rgba.size(); i += 4) {
        sum += double(result.rgba[i]) + double(result.rgba[i + 1]) + double(result.rgba[i + 2]);
    }
    *mean = sum / (3. * kSize * kSize);

    return true;
}

std::map<std::string, PXR_NS::VtValue>
cyclesSettings(bool matchLightLevels)
{
    std::map<std::string, PXR_NS::VtValue> settings = CyclesTestUtils::fixedSampling(64);
    settings[kMatchLightLevels] = PXR_NS::VtValue(matchLightLevels);

    return settings;
}

std::map<std::string, PXR_NS::VtValue>
moonRaySettings()
{
    std::map<std::string, PXR_NS::VtValue> settings;
    settings["moonray:sceneVariable:pixel_samples"] = PXR_NS::VtValue(8);

    return settings;
}

struct LightCase {
    const char* name;
    std::string light;
    // albedo / pi times the irradiance UsdLux gives the card at the centre of the frame.
    double expected;
    // How much brighter Cycles renders the light with Match Light Levels off.
    double unmatchedRatio;
    // MoonRay's normalized distant light is pi brighter than UsdLux, so it is left out of that one.
    bool moonRay;
};

// Each light gives the card an irradiance of 1 at the centre of the frame (pi for the dome, whose
// radiance is 1 from the whole hemisphere), so the expected level is the albedo over pi (the
// albedo for the dome).
std::vector<LightCase>
lightCases(const std::string& environment)
{
    const std::string dome = "    def DomeLight \"Dome\" {\n"
                             "        asset inputs:texture:file = @"
        + environment + "@\n"
        + "        token inputs:texture:format = \"latlong\"\n"
          "        float inputs:intensity = 1\n"
          "    }\n";

    // Far enough that the irradiance across the frame stays within 1% of the centre's: radiance
    // I over a sphere of radius r at distance d gives I pi r^2 / d^2.
    const double sphereIntensity = 40. * 40. / (M_PI * 2. * 2.);
    const std::string sphere = "    def SphereLight \"Bulb\" {\n"
                               "        float inputs:radius = 2\n"
                               "        float inputs:intensity = "
        + std::to_string(sphereIntensity) + "\n"
        + "        bool inputs:normalize = 0\n"
          "        double3 xformOp:translate = (0, 0, 40)\n"
          "        uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
          "    }\n";

    // Normalized, the intensity of a distant light is the irradiance it gives.
    const std::string normalizedDistant = "    def DistantLight \"Sun\" {\n"
                                          "        float inputs:angle = 0.53\n"
                                          "        float inputs:intensity = 1\n"
                                          "        bool inputs:normalize = 1\n"
                                          "    }\n";

    // Not normalized, it is radiance over the cone of the angle: 2 pi (1 - cos(angle / 2)).
    const double distantIntensity = 1. / (2. * M_PI * (1. - std::cos(5. * M_PI / 180.)));
    const std::string distant = "    def DistantLight \"Sun\" {\n"
                                "        float inputs:angle = 10\n"
                                "        float inputs:intensity = "
        + std::to_string(distantIntensity) + "\n"
        + "        bool inputs:normalize = 0\n"
          "    }\n";

    std::vector<LightCase> cases;
    cases.push_back({ "dome", dome, kAlbedo, M_PI, true });
    cases.push_back({ "sphere", sphere, kAlbedo / M_PI, 1., true });
    cases.push_back({ "normalized distant", normalizedDistant, kAlbedo / M_PI, 4., false });
    cases.push_back({ "distant", distant, kAlbedo / M_PI, 4., true });

    return cases;
}
} // anon namespace

// With Match Light Levels on, Cycles lights the card as UsdLux says each light does, and as Storm
// and MoonRay do.
TEST(CyclesLightLevels, MatchedLevelsAgreeWithStormAndMoonRay)
{
    if (!CyclesTestUtils::cyclesOrSkip()) {
        return;
    }
    const bool storm = stormOrSkip();
    const bool moonRay = moonRayOrSkip();
    const WhiteEnvironment environment;
    for (const LightCase& c : lightCases(environment.path())) {
        double cycles = 0.;
        if (!meanLevel(kRenderDelegateCyclesId, c.light, cyclesSettings(true), &cycles)) {
            continue;
        }
        std::printf("[          ] %s: Cycles %.4f, expected %.4f\n", c.name, cycles, c.expected);
        EXPECT_NEAR(c.expected, cycles, kTolerance * c.expected) << c.name;
        double level = 0.;
        if (storm && meanLevel(kRenderDelegateStormId, c.light, std::map<std::string, PXR_NS::VtValue>(), &level)) {
            std::printf("[          ] %s: Storm %.4f\n", c.name, level);
            EXPECT_NEAR(level, cycles, kTolerance * c.expected) << c.name << ": Cycles against Storm";
        }
        if (moonRay && c.moonRay && meanLevel(kMoonRayRendererId, c.light, moonRaySettings(), &level)) {
            std::printf("[          ] %s: MoonRay %.4f\n", c.name, level);
            EXPECT_NEAR(level, cycles, kTolerance * c.expected) << c.name << ": Cycles against MoonRay";
        }
    }
    HydraRender::releaseAllCpuEngines();
}

// With it off, the dome is pi brighter and the distant light 4 times brighter, while the sphere
// light, which already matched, is unchanged.
TEST(CyclesLightLevels, UnmatchedLevelsAreCyclesOwn)
{
    if (!CyclesTestUtils::cyclesOrSkip()) {
        return;
    }
    const WhiteEnvironment environment;
    for (const LightCase& c : lightCases(environment.path())) {
        double matched = 0.;
        double unmatched = 0.;
        if (!meanLevel(kRenderDelegateCyclesId, c.light, cyclesSettings(true), &matched) || !meanLevel(kRenderDelegateCyclesId, c.light, cyclesSettings(false), &unmatched)) {
            continue;
        }
        ASSERT_GT(matched, 0.) << c.name;
        std::printf("[          ] %s: Cycles %.4f matched, %.4f unmatched\n", c.name, matched, unmatched);
        EXPECT_NEAR(c.unmatchedRatio, unmatched / matched, kTolerance * c.unmatchedRatio) << c.name;
    }
    HydraRender::releaseAllCpuEngines();
}
