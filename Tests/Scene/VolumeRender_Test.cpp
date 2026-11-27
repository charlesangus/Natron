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

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include <pxr/base/plug/registry.h>
#include <pxr/base/tf/diagnosticMgr.h>

#include "CyclesTestUtils.h"
#include "Engine/Nodes/Scene/CyclesAovs.h"
#include "Engine/Nodes/Scene/DelegateSupport.h"
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
// The temperature grid, 1 at the sphere's centre and 0 at its surface, as an emission field. It
// takes the density field's time-sampled paths by reference, as they are anchored to the scene's
// layer and not to this one.
const char* const kEmission = "over \"World\"\n"
                              "{\n"
                              "    over \"Smoke\"\n"
                              "    {\n"
                              "        rel field:emission = </World/Smoke/emission>\n"
                              "\n"
                              "        def OpenVDBAsset \"emission\" (\n"
                              "            prepend references = </World/Smoke/density>\n"
                              "        )\n"
                              "        {\n"
                              "            token fieldName = \"temperature\"\n"
                              "        }\n"
                              "    }\n"
                              "}\n";
const char* const kSurfaceOnlyMaterial = "over \"World\"\n"
                                         "{\n"
                                         "    over \"Smoke\" (\n"
                                         "        prepend apiSchemas = [\"MaterialBindingAPI\"]\n"
                                         "    )\n"
                                         "    {\n"
                                         "        rel material:binding = </World/Looks/SurfaceOnly>\n"
                                         "    }\n"
                                         "\n"
                                         "    over \"Looks\"\n"
                                         "    {\n"
                                         "        def Material \"SurfaceOnly\"\n"
                                         "        {\n"
                                         "            token outputs:surface.connect = </World/Looks/SurfaceOnly/Surface.outputs:surface>\n"
                                         "\n"
                                         "            def Shader \"Surface\"\n"
                                         "            {\n"
                                         "                uniform token info:id = \"UsdPreviewSurface\"\n"
                                         "                color3f inputs:diffuseColor = (0.5, 0.5, 0.5)\n"
                                         "                token outputs:surface\n"
                                         "            }\n"
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
// The camera's shutter of kOpenShutter, and the frame 1 sphere of volume-sphere-dilated.0001.vdb,
// whose velocity reaches past its density.
const char* const kDilatedVelocity = "over \"World\"\n"
                                     "{\n"
                                     "    over \"Smoke\"\n"
                                     "    {\n"
                                     "        over \"density\"\n"
                                     "        {\n"
                                     "            asset filePath.timeSamples = { 1: @" NATRON_TESTS_FIXTURES_DIR "/volume-sphere-dilated.0001.vdb@ }\n"
                                     "        }\n"
                                     "        over \"velocity\"\n"
                                     "        {\n"
                                     "            asset filePath.timeSamples = { 1: @" NATRON_TESTS_FIXTURES_DIR "/volume-sphere-dilated.0001.vdb@ }\n"
                                     "        }\n"
                                     "    }\n"
                                     "}\n";
const char* const kDilatedVelocityOpenShutter = "over \"World\"\n"
                                                "{\n"
                                                "    over \"Cam\"\n"
                                                "    {\n"
                                                "        double shutter:close = 0.25\n"
                                                "        double shutter:open = -0.25\n"
                                                "    }\n"
                                                "    over \"Smoke\"\n"
                                                "    {\n"
                                                "        over \"density\"\n"
                                                "        {\n"
                                                "            asset filePath.timeSamples = { 1: @" NATRON_TESTS_FIXTURES_DIR "/volume-sphere-dilated.0001.vdb@ }\n"
                                                "        }\n"
                                                "        over \"velocity\"\n"
                                                "        {\n"
                                                "            asset filePath.timeSamples = { 1: @" NATRON_TESTS_FIXTURES_DIR "/volume-sphere-dilated.0001.vdb@ }\n"
                                                "        }\n"
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
    std::string errors;
    // Every TfError posted while it rendered, on any thread, function first.
    std::string codingErrors;
    HydraRender::RenderTrace trace;
};

// The delegate's Hydra sync runs on threads with no TfErrorMark, so its coding errors reach only
// the diagnostic manager's delegates.
class CodingErrors
    : public PXR_NS::TfDiagnosticMgr::Delegate {
public:
    CodingErrors()
    {
        PXR_NS::TfDiagnosticMgr::GetInstance().AddDelegate(this);
    }

    ~CodingErrors() override
    {
        PXR_NS::TfDiagnosticMgr::GetInstance().RemoveDelegate(this);
    }

    CodingErrors(const CodingErrors&) = delete;
    CodingErrors& operator=(const CodingErrors&) = delete;

    void IssueError(const PXR_NS::TfError& err) override
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        _text += err.GetSourceFunction() + ": " + err.GetCommentary() + "\n";
    }

    void IssueFatalError(const PXR_NS::TfCallContext&,
                         const std::string&) override
    {
        _UnhandledAbort();
    }

    void IssueStatus(const PXR_NS::TfStatus&) override { }

    void IssueWarning(const PXR_NS::TfWarning&) override { }

    std::string text() const
    {
        const std::lock_guard<std::mutex> lock(_mutex);

        return _text;
    }

private:
    mutable std::mutex _mutex;
    std::string _text;
};

// For a failure that may be a render the delegate cut short.
std::string
traceOf(const Render& render)
{
    const HydraRender::RenderTrace& t = render.trace;

    return std::string(" (") + (t.freshEngine ? "fresh" : "reused") + " engine, " + (t.watched ? "watched" : "trusted") + ", " + std::to_string(t.passes) + " passes, " + std::to_string(t.remakes) + " remakes, " + std::to_string(t.samples) + " samples, resets requested " + std::to_string(t.resetsRequested) + " written " + std::to_string(t.resetsWritten) + " converged " + std::to_string(t.resetsConverged) + ", " + (t.converged ? "converged" : "not converged") + ")";
}

// Renders of kScene with overrides layered on top by one owner, whose engine and stage last from
// one render to the next until the session ends: a change of time alone then reaches the delegate
// as an edit of the scene it already holds, not as a new scene.
class Session {
public:
    Session(const std::string& renderer,
            const std::string& overrides)
        : _renderer(renderer)
    {
        _stage = PXR_NS::UsdStage::Open(kScene);
        if (!_stage) {
            ADD_FAILURE() << "the volume scene did not open";

            return;
        }
        _scene = SceneOps::appendLayer(ScenePayload(), _stage->GetRootLayer(), 1);
        if (!overrides.empty()) {
            PXR_NS::SdfLayerRefPtr layer = PXR_NS::SdfLayer::CreateAnonymous(".usda");
            if (!layer->ImportFromString("#usda 1.0\n" + overrides)) {
                ADD_FAILURE() << "the overrides do not parse:\n"
                              << overrides;

                return;
            }
            _scene = SceneOps::appendLayer(_scene, layer, (U64)std::hash<std::string>()(overrides) | 2);
        }
        _valid = true;
    }

    ~Session()
    {
        HydraRender::release(this);
    }

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // False after a gtest failure.
    bool render(double time,
                Render* render)
    {
        if (!_valid) {
            return false;
        }
        HydraRender::Request request;
        request.owner = this;
        request.scene = _scene;
        request.time = PXR_NS::UsdTimeCode(time);
        request.width = kSize;
        request.height = kSize;
        request.renderer = _renderer;
        std::string error;
        if (!HydraRender::cameraFrustum(_stage, PXR_NS::SdfPath(kCamera), request.time, 1., HydraRender::eFitModeExpand, &request.frustum, &error)) {
            ADD_FAILURE() << error;

            return false;
        }
        request.camera = PXR_NS::SdfPath(kCamera);
        request.fit = HydraRender::eFitModeExpand;
        request.pixelAspect = 1.;
        HydraRender::Result result;
        bool rendered = false;
        {
            const CodingErrors codingErrors;
            rendered = HydraRender::render(request, &result, &error);
            render->codingErrors = codingErrors.text();
        }
        if (!rendered) {
            ADD_FAILURE() << _renderer << " at " << time << ": " << error;

            return false;
        }
        if (result.rgba.size() != (std::size_t)kSize * (std::size_t)kSize * 4) {
            ADD_FAILURE() << _renderer << " rendered " << result.rgba.size() << " values";

            return false;
        }
        render->rgba = result.rgba;
        render->warning = result.warning;
        render->errors = result.errors;
        render->trace = result.trace;

        return true;
    }

private:
    std::string _renderer;
    PXR_NS::UsdStageRefPtr _stage;
    ScenePayload _scene;
    bool _valid = false;
};

// kScene at time with overrides layered on top, rendered by renderer with an engine of its own.
// False after a gtest failure.
bool
renderScene(const std::string& renderer,
            double time,
            const std::string& overrides,
            Render* render)
{
    Session session(renderer, overrides);

    return session.render(time, render);
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
    std::istringstream lines(render.warning);
    std::string line;
    while (std::getline(lines, line)) {
        if (DelegateSupport::isVolumeWarningLine(line)) {
            return true;
        }
    }

    return false;
}

// The typed read of an empty VtValue, which a delegate's volume sync once hit for every volume.
void
expectNoFailedGet(const std::string& renderer,
                  const Render& render)
{
    EXPECT_EQ(std::string::npos, render.errors.find("_FailGet")) << renderer << ": " << render.errors;
    EXPECT_EQ(std::string::npos, render.codingErrors.find("_FailGet")) << renderer << ": " << render.codingErrors;
    EXPECT_EQ(std::string::npos, render.codingErrors.find("from empty VtValue")) << renderer << ": " << render.codingErrors;
}

/**
 * The volume darkens the card behind the sphere on its own side only: the left on frame 1, the
 * right on frame 2, against the card without the volume. The volume is rendered first: for Cycles
 * that is the process's first OpenVDB read, which the delegate must have initialised the library
 * for, whereas the warning check after it initialises it for every later one. Frame 2 is rendered
 * by frame 1's engine, so only the field's new path reaching the volume it binds can move it.
 **/
void
expectTheVolumeDarkensItsSide(const std::string& renderer)
{
    const RendererSettings settings(renderer);
    Render frame1;
    Render frame2;
    {
        Session sequence(renderer, std::string());
        ASSERT_TRUE(sequence.render(1., &frame1));
        ASSERT_TRUE(sequence.render(2., &frame2));
    }
    EXPECT_FALSE(hasVolumeWarning(frame1)) << frame1.warning;
    EXPECT_FALSE(hasVolumeWarning(frame2)) << frame2.warning;
    expectNoFailedGet(renderer, frame1);
    expectNoFailedGet(renderer, frame2);
    EXPECT_FALSE(frame2.trace.freshEngine) << renderer << ": frame 2" << traceOf(frame2);
    EXPECT_EQ(0, frame2.trace.remakes) << renderer << ": frame 2" << traceOf(frame2);
    Render card;
    ASSERT_TRUE(renderScene(renderer, 1., kMeshOnly, &card));
    ASSERT_GT(leftOf(card), 0.1f) << renderer << " does not draw the card";
    ASSERT_GT(rightOf(card), 0.1f) << renderer << " does not draw the card";

    EXPECT_LT(leftOf(frame1), 0.6f * leftOf(card)) << renderer << ": frame 1 left " << leftOf(frame1) << ", card " << leftOf(card) << traceOf(frame1);
    EXPECT_GT(rightOf(frame1), 0.9f * rightOf(card)) << renderer << ": frame 1 right " << rightOf(frame1) << ", card " << rightOf(card) << traceOf(frame1);
    EXPECT_GT(leftOf(frame2), 0.9f * leftOf(card)) << renderer << ": frame 2 left " << leftOf(frame2) << ", card " << leftOf(card) << traceOf(frame2);
    EXPECT_LT(rightOf(frame2), 0.6f * rightOf(card)) << renderer << ": frame 2 right " << rightOf(frame2) << ", card " << rightOf(card) << traceOf(frame2);
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

// A material with a surface but no volume output leaves the volume its default volume shader, as
// when nothing is bound.
void
expectASurfaceOnlyMaterialKeepsTheDefaultVolume(const std::string& renderer)
{
    const RendererSettings settings(renderer);
    Render bound;
    ASSERT_TRUE(renderScene(renderer, 1., kSurfaceOnlyMaterial, &bound));
    Render card;
    ASSERT_TRUE(renderScene(renderer, 1., kMeshOnly, &card));
    ASSERT_GT(leftOf(card), 0.1f) << renderer << " does not draw the card";
    EXPECT_LT(leftOf(bound), 0.6f * leftOf(card)) << renderer << ": left " << leftOf(bound) << ", card " << leftOf(card) << traceOf(bound);
    EXPECT_GT(rightOf(bound), 0.9f * rightOf(card)) << renderer << ": right " << rightOf(bound) << ", card " << rightOf(card) << traceOf(bound);
}

// An emission field bound to the volume changes the sphere's side against the same volume
// without one, and leaves the other side to the card.
void
expectAnEmissionFieldChangesTheVolume(const std::string& renderer)
{
    const RendererSettings settings(renderer);
    Render plain;
    ASSERT_TRUE(renderScene(renderer, 1., std::string(), &plain));
    Render emissive;
    ASSERT_TRUE(renderScene(renderer, 1., kEmission, &emissive));
    EXPECT_FALSE(hasVolumeWarning(emissive)) << emissive.warning;
    expectNoFailedGet(renderer, emissive);
    ASSERT_GT(rightOf(plain), 0.1f) << renderer << " does not draw the card";
    EXPECT_GT(std::fabs(leftOf(emissive) - leftOf(plain)), 0.02f * rightOf(plain)) << renderer << ": emissive left " << leftOf(emissive) << ", plain left " << leftOf(plain) << traceOf(emissive);
    EXPECT_GT(rightOf(emissive), 0.9f * rightOf(plain)) << renderer << ": emissive right " << rightOf(emissive) << ", plain right " << rightOf(plain);
}

// With the shutter open a quarter frame either side of frame 1, the velocity field blurs the
// sphere along its motion, into the card right of where it sits at the frame. still and open are
// the overrides of the render with a closed shutter and of the one with it open.
void
expectVelocityBlursTheVolume(const std::string& renderer,
                             const std::string& still = std::string(),
                             const std::string& open = kOpenShutter)
{
    const RendererSettings settings(renderer);
    Render stillRender;
    ASSERT_TRUE(renderScene(renderer, 1., still, &stillRender));
    Render blurred;
    ASSERT_TRUE(renderScene(renderer, 1., open, &blurred));
    const float stillTrail = meanOf(stillRender, kTrailX0, kTrailX1);
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

TEST(VolumeRenderCycles, ASurfaceOnlyMaterialKeepsTheDefaultVolume)
{
    if (!CyclesTestUtils::cyclesOrSkip()) {
        return;
    }
    expectASurfaceOnlyMaterialKeepsTheDefaultVolume(kRenderDelegateCyclesId);
}

// Cycles reads the velocity where it shades and looks the density up back along it, so it blurs
// the density only as far as the velocity grid reaches, as MoonRay does.
TEST(VolumeRenderCycles, VelocityBlursTheVolume)
{
    if (!CyclesTestUtils::cyclesOrSkip()) {
        return;
    }
    expectVelocityBlursTheVolume(kRenderDelegateCyclesId, kDilatedVelocity, kDilatedVelocityOpenShutter);
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

TEST(VolumeRenderMoonRay, AnEmissionFieldChangesTheVolume)
{
    if (!moonRayOrSkip()) {
        return;
    }
    expectAnEmissionFieldChangesTheVolume(kMoonRayRendererId);
}

// MoonRay blurs the density only as far as the velocity grid reaches, so this renders the sphere
// whose velocity reaches past its density as far as the shutter moves it.
TEST(VolumeRenderMoonRay, VelocityBlursTheVolume)
{
    if (!moonRayOrSkip()) {
        return;
    }
    expectVelocityBlursTheVolume(kMoonRayRendererId, kDilatedVelocity, kDilatedVelocityOpenShutter);
}
