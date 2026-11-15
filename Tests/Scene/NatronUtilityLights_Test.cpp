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
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/vt/dictionary.h>
#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/types.h>
#include <pxr/imaging/hdx/tokens.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>

#include "Global/Macros.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <QTemporaryDir>

#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/Utility/UtilityLight.h"
#include "Engine/Nodes/Scene/Utility/UtilityRenderDelegate.h"
#include "Engine/Nodes/Scene/Utility/WavefrontGpu.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/AppManager.h"
#include "Engine/Gpu/GpuBufferPool.h"
#include "Engine/Gpu/GpuRuntime.h"
#include "Engine/GpuContext.h"
#include "Gpu/GpuTestDevice.h"
#include "Gpu/GpuTestSelector.h"
#endif

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_USING

namespace {
const int kRenderSize = 16;
const double kCameraZ = 30.;

// Half the width of what the camera sees at z = 0.
const double kHalfFrame = kCameraZ * std::tan(22.5 * M_PI / 180.);

// Where the ray through the centre of pixel i, along either axis, meets the plane z = 0.
double
groundAt(int i)
{
    return ((i + 0.5) / kRenderSize * 2. - 1.) * kHalfFrame;
}

// Pure diffuse white: the specular workflow with a black specular colour leaves a Fresnel term of
// (1 - cos)^5 only, far below the tolerances here at the angles these scenes see.
const char* const kWhiteMaterial = R"(
def Material "White" {
    token outputs:surface.connect = </White/Surface.outputs:surface>
    def Shader "Surface" {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor = (1, 1, 1)
        int inputs:useSpecularWorkflow = 1
        color3f inputs:specularColor = (0, 0, 0)
        token outputs:surface
    }
}
)";

const char* const kBoundWhite = "    rel material:binding = </White>\n";

// A white quad at z = 0 facing the camera, wider than the frame.
std::string
ground()
{
    return std::string("def Mesh \"Ground\" (prepend apiSchemas = [\"MaterialBindingAPI\"]) {\n"
                       "    int[] faceVertexCounts = [4]\n"
                       "    int[] faceVertexIndices = [0, 1, 2, 3]\n"
                       "    point3f[] points = [(-20, -20, 0), (20, -20, 0), (20, 20, 0), (-20, 20, 0)]\n")
        + kBoundWhite + "}\n";
}

std::string
translate(double x,
          double y,
          double z)
{
    return "    double3 xformOp:translate = (" + std::to_string(x) + ", " + std::to_string(y) + ", " + std::to_string(z) + ")\n"
        + "    uniform token[] xformOpOrder = [\"xformOp:translate\"]\n";
}

std::string
whiteSphere(const char* name,
            double x,
            double y,
            double z)
{
    return std::string("def Sphere \"") + name + "\" (prepend apiSchemas = [\"MaterialBindingAPI\"]) {\n"
        + "    double radius = 1\n" + translate(x, y, z) + kBoundWhite + "}\n";
}

// A distant light of the given intensity whose rays leave it along -Z turned about Y by degrees, so that
// a surface facing +Z sees it at that angle.
std::string
distantLight(double intensity,
             double degrees,
             const std::string& extra = std::string())
{
    return "def DistantLight \"Sun\" {\n"
           "    float inputs:angle = 0\n"
           "    float inputs:intensity = "
        + std::to_string(intensity) + "\n"
        + "    float xformOp:rotateY = " + std::to_string(degrees) + "\n"
        + "    uniform token[] xformOpOrder = [\"xformOp:rotateY\"]\n"
        + extra + "}\n";
}

// Renders a stage with the delegate through the engine, from a camera at z = 30 looking down -Z.
class LitRender {
public:
    explicit LitRender(const std::string& usda)
    {
        RenderDelegates::list();
        _layer = SdfLayer::CreateAnonymous(".usda");
        EXPECT_TRUE(_layer->ImportFromString(std::string("#usda 1.0\n") + usda + kWhiteMaterial));
        _stage = UsdStage::Open(_layer);

        UsdImagingGLEngine::Parameters parameters;

        parameters.rendererPluginId = TfToken(kRenderDelegateUtilityId);
        parameters.gpuEnabled = false;
        _engine.reset(new UsdImagingGLEngine(parameters));
        EXPECT_EQ(TfToken(kRenderDelegateUtilityId), _engine->GetCurrentRendererId());
        _engine->SetEnablePresentation(false);
        EXPECT_TRUE(_engine->SetRendererAovs({ HdAovTokens->color, HdAovTokens->depth, HdAovTokens->primId, HdAovTokens->instanceId }));
    }

    void setDevice(const char* device)
    {
        _engine->SetRendererSetting(TfToken(kUtilitySettingDevice), VtValue(TfToken(device)));
    }

    void setMaxCutouts(int limit)
    {
        _engine->SetRendererSetting(TfToken(kUtilitySettingMaxCutouts), VtValue(limit));
    }

    // The first GPU frame in a process compiles the wavefront kernels, which on a loaded software
    // Vulkan device has taken longer than 30 s.
    void render()
    {
        const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now() + std::chrono::seconds(180);

        renderOnce();
        while (!_engine->IsConverged() && std::chrono::steady_clock::now() < end) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            renderOnce();
        }
        EXPECT_TRUE(_engine->IsConverged());
    }

    GfVec4f colorAt(int x,
                    int y) const
    {
        HdRenderBuffer* buffer = _engine->GetAovRenderBuffer(HdAovTokens->color);

        if (!buffer) {
            ADD_FAILURE() << "no colour buffer";

            return GfVec4f(0.f);
        }
        EXPECT_EQ(sizeof(GfVec4f), HdDataSizeOfFormat(buffer->GetFormat()));

        const GfVec4f* pixels = static_cast<const GfVec4f*>(buffer->Map());
        const GfVec4f value = pixels[y * kRenderSize + x];

        buffer->Unmap();

        return value;
    }

    std::vector<GfVec4f> colors() const
    {
        std::vector<GfVec4f> all;

        for (int y = 0; y < kRenderSize; ++y) {
            for (int x = 0; x < kRenderSize; ++x) {
                all.push_back(colorAt(x, y));
            }
        }

        return all;
    }

    std::string textStat(const char* key) const
    {
        return VtDictionaryGet<std::string>(_engine->GetRenderStats(), key, VtDefault = std::string());
    }

private:
    void renderOnce()
    {
        GfFrustum camera;

        camera.SetPerspective(45., 1., 0.1, 1000.);
        camera.SetPosition(GfVec3d(0., 0., kCameraZ));
        _engine->SetRenderBufferSize(GfVec2i(kRenderSize, kRenderSize));
        _engine->SetFraming(CameraUtilFraming(GfRect2i(GfVec2i(0, 0), kRenderSize, kRenderSize)));
        _engine->SetCameraState(camera.ComputeViewMatrix(), camera.ComputeProjectionMatrix());

        UsdImagingGLRenderParams params;

        params.drawMode = UsdImagingGLDrawMode::DRAW_SHADED_SMOOTH;
        params.clearColor = GfVec4f(0.f, 0.f, 0.f, 0.f);
        params.colorCorrectionMode = HdxColorCorrectionTokens->disabled;
        params.highlight = false;
        _engine->Render(_stage->GetPseudoRoot(), params);
    }

    SdfLayerRefPtr _layer;
    UsdStageRefPtr _stage;
    std::unique_ptr<UsdImagingGLEngine> _engine;
};

void
expectGrey(float expected,
           const GfVec4f& color,
           const std::string& where)
{
    const float tolerance = 1e-3f * std::max(1.f, expected);

    for (int c = 0; c < 3; ++c) {
        EXPECT_NEAR(expected, color[c], tolerance) << where << " channel " << c;
    }
    EXPECT_FLOAT_EQ(1.f, color[3]) << where;
}

std::string
pixelName(int x,
          int y)
{
    return "pixel " + std::to_string(x) + ", " + std::to_string(y);
}

// The irradiance a point light of intensity I at height h over the ground gives it at (x, y), where the
// light's rays meet it at angle theta from straight down.
double
pointIrradiance(double intensity,
                double height,
                double x,
                double y,
                double* theta = nullptr)
{
    const double d2 = x * x + y * y + height * height;
    const double cosine = height / std::sqrt(d2);

    if (theta) {
        *theta = std::acos(cosine);
    }

    return intensity * cosine / d2;
}

// A type C profile, symmetric about its axis, of 100 cd straight down, 60 cd at 45 degrees and 20 cd
// at 90, given with a multiplier of 2 on a table of half those values.
const char* const kIesProfile = "IESNA:LM-63-2002\n"
                                "[TEST] three angles\n"
                                "TILT=NONE\n"
                                "1 -1 2 3 1 1 2 0 0 0\n"
                                "1 1 0\n"
                                "0 45 90\n"
                                "0\n"
                                "50 30 10\n";

double
profileCandela(double thetaDegrees)
{
    if (thetaDegrees <= 45.) {
        return 100. - 40. * thetaDegrees / 45.;
    }

    return 60. - 40. * (thetaDegrees - 45.) / 45.;
}

#ifdef NATRON_HAVE_GPU
// Puts a GPU context of the test's own in the application's place while it lives, able to pick a
// CPU-type device. Must outlive every LitRender of the test.
class LightsGpuRuntime {
public:
    LightsGpuRuntime()
    {
        GpuContextOptions options;
        std::shared_ptr<std::string> logged = std::make_shared<std::string>();

        options.mode = GpuResolvedMode::Force;
        options.device = gputest::cpuFallbackSelector();
        options.infoLog = [logged](const std::string& line) {
            *logged = line;
        };

        std::unique_ptr<GpuContext> own(new GpuContext(options));

        if (!own->waitForInit(std::chrono::seconds(60)) || !own->isAvailable()) {
            unavailable = logged->empty() ? std::string("the GPU context did not come up") : *logged;

            return;
        }
        _saved = appPTR->swapGpuContextForTests(std::move(own));
        _swapped = true;
    }

    ~LightsGpuRuntime()
    {
        if (_swapped) {
            appPTR->swapGpuContextForTests(std::move(_saved));
        }
    }

    std::string missingSupport() const
    {
        return WavefrontGpu::missingSupport(*appPTR->getGpuContext()->runtime());
    }

    std::string unavailable;

private:
    std::unique_ptr<GpuContext> _saved;
    bool _swapped = false;
};
#endif // NATRON_HAVE_GPU

// A cube of side 2 two to four units over the ground, under a sun 45 degrees off overhead towards +X.
// Its shadow falls on x in [-5, -1], y in [-1, 1], and the camera sees past the cube onto it there.
std::string
cubeOnGround()
{
    return ground()
        + "def Mesh \"Cube\" (prepend apiSchemas = [\"MaterialBindingAPI\"]) {\n"
          "    int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]\n"
          "    int[] faceVertexIndices = [0, 1, 3, 2, 2, 3, 5, 4, 4, 5, 7, 6, 6, 7, 1, 0, 1, 7, 5, 3, 6, 0, 2, 4]\n"
          "    point3f[] points = [(-1, -1, 1), (1, -1, 1), (-1, 1, 1), (1, 1, 1), (-1, 1, -1), (1, 1, -1), (-1, -1, -1), (1, -1, -1)]\n"
        + translate(0., 0., 3.) + kBoundWhite + "}\n"
        + distantLight(M_PI, 45.);
}

void
expectCubeShadow(const LitRender& render)
{
    const float lit = static_cast<float>(std::cos(M_PI / 4.));

    for (int x = 5; x <= 6; ++x) {
        for (int y = 7; y <= 8; ++y) {
            expectGrey(0.f, render.colorAt(x, y), "shadowed " + pixelName(x, y));
        }
    }
    for (int x = 11; x < kRenderSize; ++x) {
        for (int y = 0; y < kRenderSize; ++y) {
            expectGrey(lit, render.colorAt(x, y), "lit " + pixelName(x, y));
        }
    }
}
// Blue where displayOpacity is 1 and nothing at all where it is 0, under an opacityThreshold of a half.
const char* const kCutoutMaterial = R"(
def Material "Cutout" {
    token outputs:surface.connect = </Cutout/Surface.outputs:surface>
    def Shader "Surface" {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor = (0, 0, 1)
        int inputs:useSpecularWorkflow = 1
        color3f inputs:specularColor = (0, 0, 0)
        float inputs:opacity.connect = </Cutout/Opacity.outputs:result>
        float inputs:opacityThreshold = 0.5
        token outputs:surface
    }
    def Shader "Opacity" {
        uniform token info:id = "UsdPrimvarReader_float"
        string inputs:varname = "displayOpacity"
        float outputs:result
    }
}
def Material "Red" {
    token outputs:surface.connect = </Red/Surface.outputs:surface>
    def Shader "Surface" {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor = (1, 0, 0)
        int inputs:useSpecularWorkflow = 1
        color3f inputs:specularColor = (0, 0, 0)
        token outputs:surface
    }
}
)";

// A card at height z facing +Z, wider than the frame, whose half at x < 0 is cut out.
std::string
cutoutCard(double z,
           const std::string& name = "Cutout")
{
    const std::string h = std::to_string(z);

    return "def Mesh \"" + name + "\" (prepend apiSchemas = [\"MaterialBindingAPI\"]) {\n"
                                  "    int[] faceVertexCounts = [4, 4]\n"
                                  "    int[] faceVertexIndices = [0, 1, 2, 3, 1, 4, 5, 2]\n"
                                  "    point3f[] points = [(-20, -20, "
        + h + "), (0, -20, " + h + "), (0, 20, " + h + "), (-20, 20, " + h + "), (20, -20, " + h + "), (20, 20, " + h + ")]\n"
        + "    float[] primvars:displayOpacity = [0, 1] (interpolation = \"uniform\")\n"
        + "    rel material:binding = </Cutout>\n}\n";
}

// The cutout card over a red card at z = 0, under a sun straight down that casts no shadows.
std::string
cutoutOverRed()
{
    std::string usda = ground() + cutoutCard(5.) + distantLight(M_PI, 0.) + kCutoutMaterial;
    const std::string sun = "def DistantLight \"Sun\" {\n";

    usda.replace(usda.find(sun), sun.size(), "def DistantLight \"Sun\" (prepend apiSchemas = [\"ShadowAPI\"]) {\n    bool inputs:shadow:enable = 0\n");
    usda.replace(usda.find(kBoundWhite), std::strlen(kBoundWhite), "    rel material:binding = </Red>\n");

    return usda;
}

// Three cutout cards, one over another, over the red card.
std::string
cutoutStackOverRed()
{
    std::string usda = ground() + cutoutCard(5., "CutoutA") + cutoutCard(6., "CutoutB") + cutoutCard(7., "CutoutC") + distantLight(M_PI, 0.) + kCutoutMaterial;
    const std::string sun = "def DistantLight \"Sun\" {\n";

    usda.replace(usda.find(sun), sun.size(), "def DistantLight \"Sun\" (prepend apiSchemas = [\"ShadowAPI\"]) {\n    bool inputs:shadow:enable = 0\n");
    usda.replace(usda.find(kBoundWhite), std::strlen(kBoundWhite), "    rel material:binding = </Red>\n");

    return usda;
}

void
expectCutoutShowsRed(const LitRender& render)
{
    for (int y = 0; y < kRenderSize; ++y) {
        for (int x = 0; x < kRenderSize; ++x) {
            if ((x == kRenderSize / 2 - 1) || (x == kRenderSize / 2)) {
                continue;
            }

            const GfVec4f expected = x < kRenderSize / 2 ? GfVec4f(1.f, 0.f, 0.f, 1.f) : GfVec4f(0.f, 0.f, 1.f, 1.f);
            const GfVec4f color = render.colorAt(x, y);

            for (int c = 0; c < 4; ++c) {
                EXPECT_NEAR(expected[c], color[c], 1e-3f) << pixelName(x, y) << " channel " << c;
            }
        }
    }
}

// The stack's holes at x < 0 over red when the limit reaches the red card, and a miss when it does not.
void
expectStackHolesOver(const LitRender& render,
                     bool reachesRed)
{
    for (int y = 0; y < kRenderSize; ++y) {
        for (int x = 0; x < kRenderSize / 2 - 1; ++x) {
            const GfVec4f expected = reachesRed ? GfVec4f(1.f, 0.f, 0.f, 1.f) : GfVec4f(0.f);
            const GfVec4f color = render.colorAt(x, y);

            for (int c = 0; c < 4; ++c) {
                EXPECT_NEAR(expected[c], color[c], 1e-3f) << pixelName(x, y) << " channel " << c;
            }
        }
    }
}

// The cutout card above the camera, out of its sight, shading the ground under a sun straight down.
std::string
cutoutOverhead()
{
    return ground() + cutoutCard(40.) + distantLight(M_PI, 0.) + kCutoutMaterial;
}

void
expectCutoutShadow(const LitRender& render)
{
    for (int y = 0; y < kRenderSize; ++y) {
        for (int x = 0; x < kRenderSize; ++x) {
            if ((x == kRenderSize / 2 - 1) || (x == kRenderSize / 2)) {
                continue;
            }
            expectGrey(x < kRenderSize / 2 ? 1.f : 0.f, render.colorAt(x, y), pixelName(x, y));
        }
    }
}
} // anon namespace

TEST(NatronUtilityLights, ADistantLightAtNormalIncidenceGivesIrradianceOverPi)
{
    LitRender render(ground() + distantLight(2., 0.));

    render.render();
    EXPECT_EQ(std::string(), render.textStat(kUtilityStatWarning));
    for (int y = 0; y < kRenderSize; ++y) {
        for (int x = 0; x < kRenderSize; ++x) {
            expectGrey(static_cast<float>(2. / M_PI), render.colorAt(x, y), pixelName(x, y));
        }
    }
}

TEST(NatronUtilityLights, AStageWithoutLightsIsBlackWhereSurfacesShow)
{
    LitRender render(ground());

    render.render();
    expectGrey(0.f, render.colorAt(8, 8), pixelName(8, 8));
}

TEST(NatronUtilityLights, ASpotConeCutsOffAtItsAngle)
{
    const double height = 10.;
    const double intensity = 1000.;
    LitRender render(ground()
                     + "def SphereLight \"Spot\" (prepend apiSchemas = [\"ShapingAPI\"]) {\n"
                       "    float inputs:radius = 0\n"
                       "    float inputs:intensity = "
                     + std::to_string(intensity) + "\n"
                     + "    float inputs:shaping:cone:angle = 20\n"
                     + translate(0., 0., height) + "}\n");

    render.render();
    for (int y = 0; y < kRenderSize; ++y) {
        for (int x = 0; x < kRenderSize; ++x) {
            double theta = 0.;
            const double irradiance = pointIrradiance(intensity, height, groundAt(x), groundAt(y), &theta);
            const double degrees = theta * 180. / M_PI;

            // Pixel centres stay at least a degree clear of the cone's edge.
            if (degrees < 19.) {
                expectGrey(static_cast<float>(irradiance / M_PI), render.colorAt(x, y), "inside " + pixelName(x, y));
            } else if (degrees > 21.) {
                expectGrey(0.f, render.colorAt(x, y), "outside " + pixelName(x, y));
            }
        }
    }
    EXPECT_GT(render.colorAt(9, 8)[0], 0.f);
    EXPECT_EQ(0.f, render.colorAt(10, 8)[0]);
}

TEST(NatronUtilityLights, AnIesProfileParsesToItsTableTimesTheMultiplier)
{
    std::string error;
    const UtilityIesProfileConstPtr profile = utilityParseIes(kIesProfile, &error);

    ASSERT_NE(nullptr, profile) << error;
    EXPECT_EQ(1, profile->phiCount);
    EXPECT_NEAR(100.f, profile->evaluate(0.f, 0.f), 1e-3f);
    EXPECT_NEAR(80.f, profile->evaluate(static_cast<float>(M_PI / 8.), 1.f), 1e-3f);
    EXPECT_NEAR(60.f, profile->evaluate(static_cast<float>(M_PI / 4.), 2.f), 1e-3f);
    EXPECT_NEAR(20.f, profile->evaluate(static_cast<float>(M_PI / 2.), 3.f), 1e-3f);
    EXPECT_GT(profile->mean, 0.f);
}

TEST(NatronUtilityLights, AnIesFileThatIsNotAProfileIsRefusedWithAReason)
{
    std::string error;

    EXPECT_EQ(nullptr, utilityParseIes("IESNA:LM-63-2002\n[TEST] no tilt\n", &error));
    EXPECT_NE(std::string(), error);
    error.clear();
    EXPECT_EQ(nullptr, utilityParseIes("TILT=NONE\n1 -1 1 3 1 3 2 0 0 0\n1 1 0\n0 45 90\n0\n1 1 1\n", &error));
    EXPECT_NE(std::string::npos, error.find("type")) << error;
}

TEST(NatronUtilityLights, AnIesProfileScalesIntensityByItsTable)
{
    QTemporaryDir dir;

    ASSERT_TRUE(dir.isValid());

    const std::string path = dir.filePath(QString::fromUtf8("three.ies")).toStdString();

    {
        std::ofstream file(path);

        file << kIesProfile;
        ASSERT_TRUE(file.good());
    }

    const double height = 10.;
    LitRender render(ground()
                     + "def SphereLight \"Fixture\" (prepend apiSchemas = [\"ShapingAPI\"]) {\n"
                       "    float inputs:radius = 0\n"
                       "    asset inputs:shaping:ies:file = @"
                     + path + "@\n" + translate(0., 0., height) + "}\n");

    render.render();
    EXPECT_EQ(std::string(), render.textStat(kUtilityStatSceneWarning));
    for (int x = 8; x < kRenderSize; ++x) {
        double theta = 0.;
        const double irradiance = pointIrradiance(1., height, groundAt(x), groundAt(8), &theta);

        expectGrey(static_cast<float>(profileCandela(theta * 180. / M_PI) * irradiance / M_PI), render.colorAt(x, 8), pixelName(x, 8));
    }
}

TEST(NatronUtilityLights, ACubeShadowsTheGroundAndTheLitGroundMatchesTheCosine)
{
    LitRender render(cubeOnGround());

    render.render();
    EXPECT_EQ(std::string(), render.textStat(kUtilityStatWarning));
    expectCubeShadow(render);
}

TEST(NatronUtilityLights, ALightThatCastsNoShadowsLightsTheShadowedGround)
{
    std::string usda = cubeOnGround();
    const std::string sun = "def DistantLight \"Sun\" {\n";

    usda.replace(usda.find(sun), sun.size(), "def DistantLight \"Sun\" (prepend apiSchemas = [\"ShadowAPI\"]) {\n    bool inputs:shadow:enable = 0\n");

    LitRender render(usda);

    render.render();
    expectGrey(static_cast<float>(std::cos(M_PI / 4.)), render.colorAt(5, 8), pixelName(5, 8));
}

TEST(NatronUtilityLights, ALightLinkedToOneOfTwoSpheresLightsOnlyThatSphere)
{
    LitRender render(whiteSphere("A", -4., 0., 0.) + whiteSphere("B", 4., 0., 0.)
                     + distantLight(M_PI, 0.,
                                    "    rel collection:lightLink:includes = </A>\n"
                                    "    uniform bool collection:lightLink:includeRoot = 0\n"));

    render.render();

    // Pixels 5 and 10 look at the middles of A and B.
    const GfVec4f a = render.colorAt(5, 8);
    const GfVec4f b = render.colorAt(10, 8);

    EXPECT_GT(a[0], 0.3f) << "A, which the light links";
    EXPECT_FLOAT_EQ(1.f, a[3]);
    expectGrey(0.f, b, "B, which the light does not link");
}

TEST(NatronUtilityLights, AnEmptyLightLinkCollectionLightsNothing)
{
    LitRender render(whiteSphere("A", -4., 0., 0.)
                     + distantLight(M_PI, 0., "    uniform bool collection:lightLink:includeRoot = 0\n"));

    render.render();
    expectGrey(0.f, render.colorAt(5, 8), "A");
    EXPECT_EQ(std::string(), render.textStat(kUtilityStatSceneWarning));
}

TEST(NatronUtilityLights, AShadowLinkExclusionRemovesOneSpheresShadowAndLeavesItLit)
{
    // Spheres of radius 1 three units over the ground at x = -6 and x = 6; the sun 45 degrees towards +X
    // throws their shadows three units towards -X, onto the ground around x = -9 and x = 3.
    LitRender render(ground() + whiteSphere("A", -6., 0., 3.) + whiteSphere("B", 6., 0., 3.)
                     + distantLight(M_PI, 45., "    rel collection:shadowLink:excludes = </B>\n"));

    render.render();

    const float lit = static_cast<float>(std::cos(M_PI / 4.));

    expectGrey(0.f, render.colorAt(2, 8), "A's shadow");
    expectGrey(lit, render.colorAt(9, 8), "where B's shadow would fall");
    EXPECT_GT(render.colorAt(12, 8)[0], 0.1f) << "B itself";
    expectGrey(lit, render.colorAt(14, 2), "open ground");
}

// The UsdPreviewSurface inputs a probe material sets.
struct Probe {
    double diffuse[3];
    bool specularWorkflow;
    double specular[3];
    double metallic;
    double roughness;
    double ior;
    double clearcoat;
    double clearcoatRoughness;
};

std::string
probeMaterial(const Probe& p)
{
    const auto color = [](const double* c) {
        return "(" + std::to_string(c[0]) + ", " + std::to_string(c[1]) + ", " + std::to_string(c[2]) + ")";
    };

    return "def Material \"Probe\" {\n"
           "    token outputs:surface.connect = </Probe/Surface.outputs:surface>\n"
           "    def Shader \"Surface\" {\n"
           "        uniform token info:id = \"UsdPreviewSurface\"\n"
           "        color3f inputs:diffuseColor = "
        + color(p.diffuse) + "\n"
        + "        int inputs:useSpecularWorkflow = " + (p.specularWorkflow ? "1" : "0") + "\n"
        + "        color3f inputs:specularColor = " + color(p.specular) + "\n"
        + "        float inputs:metallic = " + std::to_string(p.metallic) + "\n"
        + "        float inputs:roughness = " + std::to_string(p.roughness) + "\n"
        + "        float inputs:ior = " + std::to_string(p.ior) + "\n"
        + "        float inputs:clearcoat = " + std::to_string(p.clearcoat) + "\n"
        + "        float inputs:clearcoatRoughness = " + std::to_string(p.clearcoatRoughness) + "\n"
        + "        token outputs:surface\n"
          "    }\n"
          "}\n"
          "def Mesh \"ProbeGround\" (prepend apiSchemas = [\"MaterialBindingAPI\"]) {\n"
          "    int[] faceVertexCounts = [4]\n"
          "    int[] faceVertexIndices = [0, 1, 2, 3]\n"
          "    point3f[] points = [(-20, -20, 0), (20, -20, 0), (20, 20, 0), (-20, 20, 0)]\n"
          "    rel material:binding = </Probe>\n"
          "}\n";
}

// Storm's previewSurface.glslfx direct lighting, written out independently of the kernels: the
// radiance toward the eye along e of a surface of normal n lit by irradiance from direction l.
GfVec3d
stormRadiance(const Probe& p,
              const GfVec3d& n,
              const GfVec3d& l,
              const GfVec3d& e,
              double irradiance)
{
    const double epsilon = 0.001;
    const GfVec3d h = (l + e).GetNormalized();
    const double NdotL = std::max(0., GfDot(n, l));
    const double NdotE = std::max(0., GfDot(n, e));
    const double NdotH = std::max(0., GfDot(n, h));
    const double EdotH = std::max(0., GfDot(e, h));
    const double fresnel = std::pow(std::max(0., 1. - EdotH), 5.);
    const auto mix = [](double a, double b, double t) { return a * (1. - t) + b * t; };
    const auto specular = [&](double f0, double f90, double roughness) {
        const double alpha = roughness * roughness;
        const double alpha2 = alpha * alpha;
        const double denominator = NdotH * NdotH * (alpha2 - 1.) + 1.;
        const double D = (alpha2 + epsilon) / (denominator * denominator * M_PI);
        const double k = alpha * 0.5;
        const double G = NdotE / (NdotE * (1. - k) + k) * NdotL / (NdotL * (1. - k) + k);

        return mix(f0, f90, fresnel) * G * D / (4. * NdotL * NdotE + epsilon);
    };
    const double R = (1. - p.ior) / (1. + p.ior);
    GfVec3d radiance;

    for (int c = 0; c < 3; ++c) {
        double d = p.diffuse[c] / M_PI;
        double f0 = p.specular[c];
        double f90 = 1.;

        if (!p.specularWorkflow) {
            const double color = mix(1., p.diffuse[c], p.metallic);

            f0 = mix(R * R * color, color, p.metallic);
            f90 = color;
            d *= 1. - p.metallic;
        }

        const double s1 = specular(f0, f90, std::max(0.001, p.roughness));

        d *= 1. - mix(f0, f90, fresnel);

        const double s2 = p.clearcoat > 0. ? p.clearcoat * specular(R * R, 1., std::max(0.001, p.clearcoatRoughness)) : 0.;

        radiance[c] = NdotL * (d + s1 + s2) * irradiance;
    }

    return radiance;
}

void
expectStormLighting(const Probe& probe,
                    const char* what)
{
    const double degrees = 30.;
    const double intensity = M_PI;
    LitRender render(probeMaterial(probe) + distantLight(intensity, degrees));

    render.render();
    EXPECT_EQ(std::string(), render.textStat(kUtilityStatSceneWarning));

    const double theta = degrees * M_PI / 180.;
    const GfVec3d l(std::sin(theta), 0., std::cos(theta));

    for (int y = 0; y < kRenderSize; ++y) {
        for (int x = 0; x < kRenderSize; ++x) {
            const GfVec3d e = (GfVec3d(0., 0., kCameraZ) - GfVec3d(groundAt(x), groundAt(y), 0.)).GetNormalized();
            const GfVec3d expected = stormRadiance(probe, GfVec3d(0., 0., 1.), l, e, intensity);
            const GfVec4f color = render.colorAt(x, y);

            for (int c = 0; c < 3; ++c) {
                EXPECT_NEAR(expected[c], color[c], 1e-3 * std::max(1., expected[c])) << what << " " << pixelName(x, y) << " channel " << c;
            }
            EXPECT_FLOAT_EQ(1.f, color[3]) << what << " " << pixelName(x, y);
        }
    }
}

TEST(NatronUtilityLights, PartialMetallicLightsAsStormDoes)
{
    expectStormLighting(Probe { { 0.8, 0.4, 0.2 }, false, { 0., 0., 0. }, 0.5, 0.3, 1.5, 0., 0.01 }, "half metal");
}

TEST(NatronUtilityLights, AColouredMetalLightsAsStormDoes)
{
    expectStormLighting(Probe { { 0.9, 0.6, 0.2 }, false, { 0., 0., 0. }, 1., 0.25, 1.5, 0., 0.01 }, "gold");
}

TEST(NatronUtilityLights, ANonDefaultIorLightsAsStormDoes)
{
    expectStormLighting(Probe { { 0.3, 0.5, 0.7 }, false, { 0., 0., 0. }, 0., 0.2, 2., 0., 0.01 }, "ior 2");
}

TEST(NatronUtilityLights, AClearcoatLightsAsStormDoes)
{
    expectStormLighting(Probe { { 0.5, 0.2, 0.2 }, false, { 0., 0., 0. }, 0., 0.6, 1.5, 1., 0.1 }, "clearcoat");
}

TEST(NatronUtilityLights, TheSpecularWorkflowLightsAsStormDoes)
{
    expectStormLighting(Probe { { 0.4, 0.4, 0.6 }, true, { 0.2, 0.3, 0.1 }, 0., 0.35, 1.5, 0.5, 0.2 }, "specular workflow");
}

// A focused sphere light of the given orientation over the ground, uncut by its cone.
std::string
focusedLight(double intensity,
             double height,
             double focus,
             double rotateX)
{
    return "def SphereLight \"Focused\" (prepend apiSchemas = [\"ShapingAPI\"]) {\n"
           "    float inputs:radius = 0\n"
           "    float inputs:intensity = "
        + std::to_string(intensity) + "\n"
        + "    float inputs:shaping:cone:angle = 180\n"
        + "    float inputs:shaping:focus = " + std::to_string(focus) + "\n"
        + "    double3 xformOp:translate = (0, 0, " + std::to_string(height) + ")\n"
        + "    float xformOp:rotateX = " + std::to_string(rotateX) + "\n"
        + "    uniform token[] xformOpOrder = [\"xformOp:translate\", \"xformOp:rotateX\"]\n"
        + "}\n";
}

TEST(NatronUtilityLights, FocusFallsOffByTheAbsoluteCosineOnBothSidesOfTheLight)
{
    const double height = 10.;
    const double intensity = 1000.;
    const double focus = 2.;

    // Facing the ground and facing away, the ground sees the same falloff.
    for (double rotateX : { 0., 180. }) {
        LitRender render(ground() + focusedLight(intensity, height, focus, rotateX));

        render.render();
        for (int y = 0; y < kRenderSize; ++y) {
            for (int x = 0; x < kRenderSize; ++x) {
                double theta = 0.;
                const double irradiance = pointIrradiance(intensity, height, groundAt(x), groundAt(y), &theta);
                const double expected = irradiance * std::pow(std::cos(theta), focus) / M_PI;

                expectGrey(static_cast<float>(expected), render.colorAt(x, y), "rotated " + std::to_string(rotateX) + " " + pixelName(x, y));
            }
        }
    }
}

const char* const kBlueDome = "def DomeLight \"Sky\" {\n"
                              "    color3f inputs:color = (0.2, 0.4, 0.6)\n"
                              "    float inputs:intensity = 1\n"
                              "}\n";

TEST(NatronUtilityLights, ADomeAloneFillsTheFrameWithItsColour)
{
    LitRender render(kBlueDome);

    render.render();
    for (int y = 0; y < kRenderSize; ++y) {
        for (int x = 0; x < kRenderSize; ++x) {
            const GfVec4f color = render.colorAt(x, y);

            EXPECT_NEAR(0.2f, color[0], 1e-5f) << pixelName(x, y);
            EXPECT_NEAR(0.4f, color[1], 1e-5f) << pixelName(x, y);
            EXPECT_NEAR(0.6f, color[2], 1e-5f) << pixelName(x, y);
            EXPECT_FLOAT_EQ(1.f, color[3]) << pixelName(x, y);
        }
    }
}

TEST(NatronUtilityLights, RaysThatMissShowTheDomeBesideGeometry)
{
    LitRender render(whiteSphere("Ball", 0., 0., 0.) + kBlueDome);

    render.render();

    const GfVec4f corner = render.colorAt(0, 0);

    EXPECT_NEAR(0.2f, corner[0], 1e-5f);
    EXPECT_NEAR(0.4f, corner[1], 1e-5f);
    EXPECT_NEAR(0.6f, corner[2], 1e-5f);
    EXPECT_FLOAT_EQ(1.f, corner[3]);

    const GfVec4f ball = render.colorAt(8, 8);

    EXPECT_GT(ball[2], 0.f) << "the dome lights the ball";
    EXPECT_FLOAT_EQ(1.f, ball[3]);
}

TEST(NatronUtilityLights, ACutoutCardShowsTheCardBehindItThroughItsHole)
{
    LitRender render(cutoutOverRed());

    render.render();
    EXPECT_EQ(std::string(), render.textStat(kUtilityStatWarning));
    expectCutoutShowsRed(render);
}

TEST(NatronUtilityLights, MaxCutoutsBelowTheStackMakesTheRayAMissAndRaisingItRevealsTheCardBehind)
{
    LitRender render(cutoutStackOverRed());

    render.setMaxCutouts(2);
    render.render();
    EXPECT_EQ(std::string(), render.textStat(kUtilityStatWarning));
    expectStackHolesOver(render, false);

    render.setMaxCutouts(3);
    render.render();
    expectStackHolesOver(render, true);

    render.setMaxCutouts(1);
    render.render();
    expectStackHolesOver(render, false);
}

TEST(NatronUtilityLights, ACutoutCardsShadowIsLitUnderItsHole)
{
    LitRender render(cutoutOverhead());

    render.render();
    EXPECT_EQ(std::string(), render.textStat(kUtilityStatWarning));
    expectCutoutShadow(render);
}

TEST(NatronUtilityLights, ALightLinkedToOneNativeInstanceLightsOnlyThatInstance)
{
    LitRender render(std::string("class Xform \"Ball\" {\n"
                                 "    def Sphere \"Shape\" {\n"
                                 "        double radius = 1\n"
                                 "    }\n"
                                 "}\n"
                                 "def Xform \"A\" (instanceable = true\n"
                                 "    prepend references = </Ball>) {\n")
                     + translate(-4., 0., 0.) + "}\n"
                     + "def Xform \"B\" (instanceable = true\n"
                       "    prepend references = </Ball>) {\n"
                     + translate(4., 0., 0.) + "}\n"
                     + distantLight(M_PI, 0.,
                                    "    rel collection:lightLink:includes = </A>\n"
                                    "    uniform bool collection:lightLink:includeRoot = 0\n"));

    render.render();

    const GfVec4f a = render.colorAt(5, 8);
    const GfVec4f b = render.colorAt(10, 8);

    EXPECT_GT(a[0], 0.05f) << "A, which the light links";
    EXPECT_FLOAT_EQ(1.f, a[3]);
    expectGrey(0.f, b, "B, which the light does not link");
}

#ifdef NATRON_HAVE_GPU
TEST(NatronUtilityLights, GpuShadowsAndLightsAsTheCpuDoes)
{
    LightsGpuRuntime runtime;

    GPUTEST_DEVICE_OR_SKIP(runtime.unavailable.empty(), runtime.unavailable);

    const std::string missing = runtime.missingSupport();

    if (!missing.empty()) {
        GPUTEST_SKIP() << missing;
    }

    LitRender render(cubeOnGround());

    render.setDevice(kUtilityDeviceCpu);
    render.render();
    EXPECT_EQ("CPU", render.textStat(kUtilityStatDevice));

    const std::vector<GfVec4f> cpu = render.colors();

    render.setDevice(kUtilityDeviceGpu);
    render.render();
    EXPECT_EQ(0u, render.textStat(kUtilityStatDevice).rfind("GPU", 0)) << render.textStat(kUtilityStatDevice);
    EXPECT_EQ(std::string(), render.textStat(kUtilityStatWarning));
    expectCubeShadow(render);

    const std::vector<GfVec4f> gpu = render.colors();

    for (std::size_t i = 0; i < cpu.size(); ++i) {
        for (int c = 0; c < 4; ++c) {
            EXPECT_NEAR(cpu[i][c], gpu[i][c], 1e-4f) << pixelName(static_cast<int>(i) % kRenderSize, static_cast<int>(i) / kRenderSize);
        }
    }
}

TEST(NatronUtilityLights, GpuHonoursShadowLinksAsTheCpuDoes)
{
    LightsGpuRuntime runtime;

    GPUTEST_DEVICE_OR_SKIP(runtime.unavailable.empty(), runtime.unavailable);

    const std::string missing = runtime.missingSupport();

    if (!missing.empty()) {
        GPUTEST_SKIP() << missing;
    }

    LitRender render(ground() + whiteSphere("A", -6., 0., 3.) + whiteSphere("B", 6., 0., 3.)
                     + distantLight(M_PI, 45., "    rel collection:shadowLink:excludes = </B>\n"));

    render.setDevice(kUtilityDeviceCpu);
    render.render();

    const std::vector<GfVec4f> cpu = render.colors();

    render.setDevice(kUtilityDeviceGpu);
    render.render();
    EXPECT_EQ(0u, render.textStat(kUtilityStatDevice).rfind("GPU", 0)) << render.textStat(kUtilityStatDevice);

    const std::vector<GfVec4f> gpu = render.colors();

    for (std::size_t i = 0; i < cpu.size(); ++i) {
        for (int c = 0; c < 4; ++c) {
            EXPECT_NEAR(cpu[i][c], gpu[i][c], 1e-4f) << pixelName(static_cast<int>(i) % kRenderSize, static_cast<int>(i) / kRenderSize);
        }
    }
    expectGrey(0.f, render.colorAt(2, 8), "A's shadow");
}
TEST(NatronUtilityLights, GpuLightsMaterialsFocusAndTheDomeAsTheCpuDoes)
{
    LightsGpuRuntime runtime;

    GPUTEST_DEVICE_OR_SKIP(runtime.unavailable.empty(), runtime.unavailable);

    const std::string missing = runtime.missingSupport();

    if (!missing.empty()) {
        GPUTEST_SKIP() << missing;
    }

    LitRender render(probeMaterial(Probe { { 0.9, 0.6, 0.2 }, false, { 0., 0., 0. }, 0.5, 0.25, 2., 1., 0.1 })
                     + focusedLight(1000., 10., 2., 180.) + distantLight(M_PI, 30.));

    render.setDevice(kUtilityDeviceCpu);
    render.render();

    const std::vector<GfVec4f> cpu = render.colors();

    render.setDevice(kUtilityDeviceGpu);
    render.render();
    EXPECT_EQ(0u, render.textStat(kUtilityStatDevice).rfind("GPU", 0)) << render.textStat(kUtilityStatDevice);

    const std::vector<GfVec4f> gpu = render.colors();

    for (std::size_t i = 0; i < cpu.size(); ++i) {
        for (int c = 0; c < 4; ++c) {
            EXPECT_NEAR(cpu[i][c], gpu[i][c], 1e-4f * std::max(1.f, std::abs(cpu[i][c]))) << pixelName(static_cast<int>(i) % kRenderSize, static_cast<int>(i) / kRenderSize);
        }
    }

    LitRender dome(kBlueDome);

    dome.setDevice(kUtilityDeviceGpu);
    dome.render();
    EXPECT_EQ(0u, dome.textStat(kUtilityStatDevice).rfind("GPU", 0)) << dome.textStat(kUtilityStatDevice);

    const GfVec4f sky = dome.colorAt(3, 12);

    EXPECT_NEAR(0.2f, sky[0], 1e-5f);
    EXPECT_NEAR(0.4f, sky[1], 1e-5f);
    EXPECT_NEAR(0.6f, sky[2], 1e-5f);
    EXPECT_FLOAT_EQ(1.f, sky[3]);
}

TEST(NatronUtilityLights, GpuSeesAndLightsThroughCutoutsAsTheCpuDoes)
{
    LightsGpuRuntime runtime;

    GPUTEST_DEVICE_OR_SKIP(runtime.unavailable.empty(), runtime.unavailable);

    const std::string missing = runtime.missingSupport();

    if (!missing.empty()) {
        GPUTEST_SKIP() << missing;
    }

    for (int scene = 0; scene < 2; ++scene) {
        LitRender render(scene == 0 ? cutoutOverRed() : cutoutOverhead());

        render.setDevice(kUtilityDeviceCpu);
        render.render();

        const std::vector<GfVec4f> cpu = render.colors();

        render.setDevice(kUtilityDeviceGpu);
        render.render();
        EXPECT_EQ(0u, render.textStat(kUtilityStatDevice).rfind("GPU", 0)) << render.textStat(kUtilityStatDevice);
        EXPECT_EQ(std::string(), render.textStat(kUtilityStatWarning));
        if (scene == 0) {
            expectCutoutShowsRed(render);
        } else {
            expectCutoutShadow(render);
        }

        const std::vector<GfVec4f> gpu = render.colors();

        for (std::size_t i = 0; i < cpu.size(); ++i) {
            for (int c = 0; c < 4; ++c) {
                EXPECT_NEAR(cpu[i][c], gpu[i][c], 1e-4f) << "scene " << scene << " " << pixelName(static_cast<int>(i) % kRenderSize, static_cast<int>(i) / kRenderSize);
            }
        }
    }
}

TEST(NatronUtilityLights, GpuMaxCutoutsLimitsTheStackAsTheCpuDoes)
{
    LightsGpuRuntime runtime;

    GPUTEST_DEVICE_OR_SKIP(runtime.unavailable.empty(), runtime.unavailable);

    const std::string missing = runtime.missingSupport();

    if (!missing.empty()) {
        GPUTEST_SKIP() << missing;
    }

    LitRender render(cutoutStackOverRed());

    render.setDevice(kUtilityDeviceGpu);
    for (int limit : { 2, 3 }) {
        render.setMaxCutouts(limit);
        render.render();
        EXPECT_EQ(0u, render.textStat(kUtilityStatDevice).rfind("GPU", 0)) << render.textStat(kUtilityStatDevice);
        EXPECT_EQ(std::string(), render.textStat(kUtilityStatWarning));
        expectStackHolesOver(render, limit == 3);
    }
}
#endif // NATRON_HAVE_GPU
