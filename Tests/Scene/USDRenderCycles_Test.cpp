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
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <list>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <sys/resource.h>

#include <gtest/gtest.h>

#include <QString>

#include <pxr/base/plug/registry.h>

#include "BaseTest.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Scene/CyclesAovs.h"
#include "Engine/Nodes/Scene/CyclesSettings.h"
#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/RenderSettingsKnobs.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDEditTransform.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Nodes/Scene/USDRender.h"
#include "Engine/Nodes/Scene/USDRenderSettings.h"
#include "Engine/ViewIdx.h"
#include "RenderBothWays.h"
#include "RenderSettingsTestUtils.h"
#include "Tests/Native/NativeGolden.h"
#include "ofxNatron.h"

NATRON_NAMESPACE_USING

namespace {
const char* const kFixture = NATRON_TESTS_FIXTURES_DIR "/scene-cycles.usda";
// A mesh card filling the centre, ten units from the camera, beside two cylinder lights, which
// Cycles 5.2's delegate does not make.
const char* const kUnsupportedFixture = NATRON_TESTS_FIXTURES_DIR "/scene-cycles-unsupported.usda";
const char* const kMdlOnlyFixture = NATRON_TESTS_FIXTURES_DIR "/scene-cycles-mdl-only.usda";
// The cubes of kFixture under a blue dome light without a texture.
const char* const kDomeFixture = NATRON_TESTS_FIXTURES_DIR "/scene-cycles-dome.usda";
const float kCardDistance = 10.f;
const char* const kCamera = "/World/Cam";
const int kSize = 64;
// Pixels on each cube: the near one's front face fills the centre, at z = 1 nine units from the
// camera; the far one's, at z = -0.5, holds (57, 32).
const int kNearX = 32;
const int kNearY = 32;
const int kFarX = 57;
const int kFarY = 32;
const float kNearDistance = 9.f;
const float kFarDistance = 10.5f;

// A card lit by a distant light, bound to a UsdPreviewSurface whose normal is a bump texture
// authored with scale (2, 2, 2, 1) and bias (-1, -1, -1, 0).
const char* const kNormalMapFixture = NATRON_TESTS_FIXTURES_DIR "/scene-cycles-normalmap.usda";
const char* const kNormalMapGoldenCase = "CyclesNormalMapReference";
const float kNormalMapContrast = 0.02f;

// A black card whose only light is its emissiveColor (2, 0.5, 0), in a scene without lights.
const char* const kEmissionFixture = NATRON_TESTS_FIXTURES_DIR "/scene-cycles-emission.usda";
const char* const kEmissionGoldenCase = "CyclesEmissionReference";
// Two cards of opacity 0.3 (left, centred on pixel 16) and 0.7 (right, on 48) under an
// opacityThreshold of 0.5.
const char* const kCutoutFixture = NATRON_TESTS_FIXTURES_DIR "/scene-cycles-cutout.usda";
const char* const kCutoutGoldenCase = "CyclesCutoutReference";
const int kCutoutLeftX = 16;
const int kCutoutRightX = 48;
// Two black spheres, at pixels 16 and 48 on the middle row, the right one with a clearcoat.
const char* const kClearcoatFixture = NATRON_TESTS_FIXTURES_DIR "/scene-cycles-clearcoat.usda";
const char* const kClearcoatGoldenCase = "CyclesClearcoatReference";
const int kClearcoatPlainX = 16;
const int kClearcoatCoatedX = 48;
const int kClearcoatHalfWidth = 8;
const float kClearcoatGain = 0.005f;

const char* const kGoldenGroup = "USDRender";
const char* const kGoldenCase = "CyclesReference";

/**
 * Cycles is not bit-exact across machines even with a fixed seed and sample count: its CPU kernels
 * are built for several instruction sets and the one the host supports is picked at run time, and
 * they are built with fast math, so the same path's arithmetic differs in its last bits, and at
 * 16 samples a bounce that goes the other way moves a pixel by about a sixteenth of what it
 * carries. A pixel differs when a channel is off by more than kChannelTolerance; up to
 * kDifferingFraction of them may, and the mean difference stays under kMeanTolerance. A change
 * in what is rendered (a lost face, a light, a colour transform) is far past these. On one host,
 * a render repeated is identical.
 **/
const float kChannelTolerance = 0.08f;
const double kDifferingFraction = 0.01;
const double kMeanTolerance = 0.005;

// The Cycles delegate comes from the build image (tools/ci/local/build-cycles.sh), not from this
// tree, so a build in an older image has none. The bundled gtest has no GTEST_SKIP, so the test
// returns early on false; NATRON_TEST_REQUIRE_CYCLES=1 turns that skip into a failure.
bool
cyclesOrSkip()
{
    if (PXR_NS::PlugRegistry::GetInstance().GetPluginWithName("hdCycles")) {
        return true;
    }
    const char* reason = "OpenUSD has no hdCycles plug-in registered; this build image predates the "
                         "Cycles render delegate (tools/ci/local/build-cycles.sh)";
    const char* require = std::getenv("NATRON_TEST_REQUIRE_CYCLES");
    if (require && (std::strcmp(require, "1") == 0)) {
        ADD_FAILURE() << "NATRON_TEST_REQUIRE_CYCLES=1 but " << reason;
    } else {
        std::printf("[  SKIPPED ] %s\n", reason);
    }

    return false;
}

std::uint32_t
bitsOf(float value)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));

    return bits;
}

std::size_t
pixelIndex(int x,
           int y)
{
    return (std::size_t)y * (std::size_t)kSize + (std::size_t)x;
}

// The threads of this process.
int
threadCount()
{
    DIR* dir = ::opendir("/proc/self/task");
    if (!dir) {
        return -1;
    }
    int count = 0;
    while (const struct dirent* entry = ::readdir(dir)) {
        if (entry->d_name[0] != '.') {
            ++count;
        }
    }
    ::closedir(dir);

    return count;
}

// The CPU time every thread of this process has used, in seconds.
double
processCpuSeconds()
{
    struct rusage usage;
    if (::getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0.;
    }

    return (double)usage.ru_utime.tv_sec + usage.ru_utime.tv_usec * 1e-6 + (double)usage.ru_stime.tv_sec + usage.ru_stime.tv_usec * 1e-6;
}

ImageLayerDesc
normalLayer()
{
    static const char* const channels[] = { "X", "Y", "Z" };

    return ImageLayerDesc("normal", "normal", "", std::vector<std::string>(channels, channels + 3));
}

std::list<ImageLayerDesc>
allPlanes()
{
    std::list<ImageLayerDesc> layers;
    layers.push_back(ImageLayerDesc::getRGBAComponents());
    layers.push_back(ImageLayerDesc("depth", "depth", "", std::vector<std::string>(1, "Z")));
    layers.push_back(ImageLayerDesc("primId", "primId", "", std::vector<std::string>(1, "id")));
    layers.push_back(normalLayer());

    return layers;
}

// A sample count and seed fixed for every Cycles engine the tests make, and the adaptive sampler,
// which stops pixels at a noise level rather than a count, off.
std::map<std::string, PXR_NS::VtValue>
fixedSampling(int samples)
{
    std::map<std::string, PXR_NS::VtValue> settings;
    settings["cycles:samples"] = PXR_NS::VtValue(samples);
    settings["cycles:integrator:seed"] = PXR_NS::VtValue(7);
    settings["cycles:integrator:use_adaptive_sampling"] = PXR_NS::VtValue(false);

    return settings;
}
} // anon namespace

class USDRenderCyclesTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        HydraRender::setRendererSettings(kRenderDelegateCyclesId, fixedSampling(16));
    }

    virtual void TearDown() OVERRIDE
    {
        HydraRender::setRendererSettings(kRenderDelegateCyclesId, std::map<std::string, PXR_NS::VtValue>());
        HydraRender::releaseAllCpuEngines();
        SceneQueryPool::instance().releaseIdle();
        BaseTest::TearDown();
    }

    template <typename KNOB>
    static KNOB* knob(const NodePtr& node,
                      const char* name)
    {
        KNOB* k = dynamic_cast<KNOB*>(node->getKnobByName(name).get());
        EXPECT_TRUE(k != nullptr) << node->getPluginID() << " has no knob " << name;

        return k;
    }

    static bool selectRenderer(KnobChoice* knob,
                               const std::string& id)
    {
        const std::vector<ChoiceOption> entries = knob->getEntries_mt_safe();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].id == id) {
                knob->setValue((int)i);

                return true;
            }
        }

        return false;
    }

    /**
     * The fixture read by a USDRead into a USDRenderSettings whose prim frames the fixture's
     * camera at kSize x kSize, into a USDRender whose Renderer knob picks Cycles. Returns the
     * USDRender, null after a gtest failure; *reader gets the USDRead.
     **/
    NodePtr createGraph(NodePtr* reader = nullptr,
                        const char* fixture = kFixture)
    {
        NodePtr read = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
        NodePtr settings = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDERSETTINGS));
        NodePtr render = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDER));
        if (!read || !settings || !render) {
            ADD_FAILURE() << "a 3D node is not registered";

            return NodePtr();
        }
        KnobFile* file = knob<KnobFile>(read, kUSDReadParamFile);
        KnobChoice* renderer = knob<KnobChoice>(render, kUSDRenderParamRenderer);
        if (!file || !renderer) {
            return NodePtr();
        }
        file->setValue(std::string(fixture));
        if (!selectRenderer(renderer, kRenderDelegateCyclesId)) {
            ADD_FAILURE() << "the Renderer menu has no " << kRenderDelegateCyclesId;

            return NodePtr();
        }
        connectNodes(read, settings, 0, true);
        connectNodes(settings, render, 0, true);
        if (!RenderSettingsTestUtils::configure(settings, kCamera, kSize, kSize)) {
            return NodePtr();
        }
        if (reader) {
            *reader = read;
        }

        return render;
    }

    // Renders node's rgba, depth, primId and normal planes, in that order. Bypassing the cache
    // empties the caches after the render too, which releases the render engines, so with
    // keepEngine they are emptied before only, and the engine is there to ask for its settings.
    static bool renderAll(const NodePtr& node,
                          std::vector<RenderedPlane>* planes,
                          bool keepEngine = false)
    {
        std::string error;
        if (keepEngine) {
            appPTR->clearAllCaches();
        }
        const bool ok = renderNodePlanesDirect(node, 1., ViewIdx(0), 0, RectI(0, 0, kSize, kSize), allPlanes(), planes, &error, !keepEngine);
        EXPECT_TRUE(ok) << error;
        EXPECT_FALSE(node->hasPersistentMessage());
        if (!ok) {
            return false;
        }
        const std::size_t pixels = (std::size_t)kSize * (std::size_t)kSize;
        EXPECT_EQ(4u, planes->size());
        if ((planes->size() != 4) || ((*planes)[0].pixels.size() != pixels * 4) || ((*planes)[1].pixels.size() != pixels) || ((*planes)[2].pixels.size() != pixels) || ((*planes)[3].pixels.size() != pixels * 3)) {
            ADD_FAILURE() << "the planes are not the size of the format";

            return false;
        }

        return true;
    }

    /**
     * A Request for the fixture's scene as reader gives it, through its camera, kSize x kSize,
     * with Cycles. False after a gtest failure.
     **/
    static bool cyclesRequest(const NodePtr& reader,
                              const void* owner,
                              HydraRender::Request* request)
    {
        std::string error;
        if (reader->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), &request->scene, &error) != EffectInstance::eRenderRoIRetCodeOk) {
            ADD_FAILURE() << error;

            return false;
        }
        SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(request->scene, &error);
        if (!lease.getStage()) {
            ADD_FAILURE() << error;

            return false;
        }
        request->time = PXR_NS::UsdTimeCode(1.);
        if (!HydraRender::cameraFrustum(lease.getStage(), PXR_NS::SdfPath(kCamera), request->time, 1., HydraRender::eFitModeFill, &request->frustum, &error)) {
            ADD_FAILURE() << error;

            return false;
        }
        request->owner = owner;
        request->width = kSize;
        request->height = kSize;
        request->renderer = kRenderDelegateCyclesId;

        return true;
    }
};

// Cycles lights a stage with no light by a grey world of its own, and Natron adds no headlight.
TEST_F(USDRenderCyclesTest, BeautyAndDepth)
{
    if (!cyclesOrSkip()) {
        return;
    }
    NodePtr node = createGraph();
    ASSERT_TRUE(bool(node));
    std::vector<RenderedPlane> planes;
    ASSERT_TRUE(renderAll(node, &planes));
    const std::vector<float>& rgba = planes[0].pixels;
    const std::vector<float>& depth = planes[1].pixels;

    const std::size_t centre = pixelIndex(kNearX, kNearY);
    EXPECT_GT(rgba[centre * 4] + rgba[centre * 4 + 1] + rgba[centre * 4 + 2], 0.05f);
    EXPECT_NEAR(1.f, rgba[centre * 4 + 3], 1e-3f);
    // The stage's metersPerUnit of 0.01 scales nothing: depth is in scene units, as Storm's.
    EXPECT_NEAR(kNearDistance, depth[centre], 1e-3f);
    EXPECT_NEAR(kFarDistance, depth[pixelIndex(kFarX, kFarY)], 1e-3f);

    const std::size_t corner = pixelIndex(0, 0);
    EXPECT_TRUE(std::isinf(depth[corner]) && (depth[corner] > 0.f)) << depth[corner];
    EXPECT_EQ(0.f, rgba[corner * 4 + 3]);
}

TEST_F(USDRenderCyclesTest, PrimIdAndNormal)
{
    if (!cyclesOrSkip()) {
        return;
    }
    NodePtr node = createGraph();
    ASSERT_TRUE(bool(node));
    std::vector<RenderedPlane> planes;
    ASSERT_TRUE(renderAll(node, &planes));
    const std::vector<float>& primId = planes[2].pixels;
    const std::vector<float>& normal = planes[3].pixels;

    const std::uint32_t nearId = bitsOf(HydraRender::hashToFloat(HydraRender::primPathHash("/World/Near")));
    const std::uint32_t farId = bitsOf(HydraRender::hashToFloat(HydraRender::primPathHash("/World/Far")));
    EXPECT_EQ(nearId, bitsOf(primId[pixelIndex(kNearX, kNearY)]));
    EXPECT_EQ(farId, bitsOf(primId[pixelIndex(kFarX, kFarY)]));
    EXPECT_EQ(0u, bitsOf(primId[pixelIndex(0, 0)]));
    std::set<std::uint32_t> ids;
    for (float value : primId) {
        if (bitsOf(value) != 0) {
            ids.insert(bitsOf(value));
        }
    }
    EXPECT_EQ(2u, ids.size());

    NativeEffectBase* effect = dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
    ASSERT_TRUE(bool(effect));
    const ImageMetadata metadata = effect->getOutputMetadata(1., ViewIdx(0));
    for (std::uint32_t id : ids) {
        char key[32];
        std::snprintf(key, sizeof(key), kUSDRenderMetadataPrimIdPrefix "%08x", id);
        const std::optional<std::string> path = metadata.getString(key);
        ASSERT_TRUE(path.has_value()) << key;
        EXPECT_EQ((id == nearId) ? std::string("/World/Near") : std::string("/World/Far"), *path) << key;
    }

    // Both front faces face the camera, which looks down -Z unrotated: +Z in camera space.
    const std::size_t pixels[] = { pixelIndex(kNearX, kNearY), pixelIndex(kFarX, kFarY) };
    for (std::size_t i : pixels) {
        EXPECT_NEAR(0.f, normal[i * 3], 0.05f) << i;
        EXPECT_NEAR(0.f, normal[i * 3 + 1], 0.05f) << i;
        EXPECT_NEAR(1.f, normal[i * 3 + 2], 0.05f) << i;
    }
    const std::size_t corner = pixelIndex(0, 0);
    EXPECT_EQ(0.f, normal[corner * 3]);
    EXPECT_EQ(0.f, normal[corner * 3 + 1]);
    EXPECT_EQ(0.f, normal[corner * 3 + 2]);
}

// Two prims of a type Cycles does not make give one warning line naming the type, the rest still
// renders, and a render of a stage without them clears it.
TEST_F(USDRenderCyclesTest, WarnsOncePerUnsupportedPrimType)
{
    if (!cyclesOrSkip()) {
        return;
    }
    NodePtr reader;
    NodePtr node = createGraph(&reader, kUnsupportedFixture);
    ASSERT_TRUE(bool(node));
    std::vector<RenderedPlane> planes;
    std::string error;
    ASSERT_TRUE(renderNodePlanesDirect(node, 1., ViewIdx(0), 0, RectI(0, 0, kSize, kSize), allPlanes(), &planes, &error)) << error;
    const std::size_t pixels = (std::size_t)kSize * (std::size_t)kSize;
    ASSERT_EQ(4u, planes.size());
    ASSERT_EQ(pixels * 4, planes[0].pixels.size());
    ASSERT_EQ(pixels, planes[1].pixels.size());
    ASSERT_EQ(pixels, planes[2].pixels.size());

    const std::size_t centre = pixelIndex(kNearX, kNearY);
    EXPECT_NEAR(1.f, planes[0].pixels[centre * 4 + 3], 1e-3f);
    EXPECT_NEAR(kCardDistance, planes[1].pixels[centre], 1e-3f);
    EXPECT_EQ(bitsOf(HydraRender::hashToFloat(HydraRender::primPathHash("/World/Card"))), bitsOf(planes[2].pixels[centre]));

    ASSERT_TRUE(node->hasPersistentMessage());
    QString message;
    int type = 0;
    node->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeWarning, type);
    const std::string text = message.toStdString();
    EXPECT_EQ(std::string::npos, text.find('\n')) << text;
    std::size_t mentions = 0;
    for (std::size_t at = text.find("cylinderLight"); at != std::string::npos; at = text.find("cylinderLight", at + 1)) {
        ++mentions;
    }
    EXPECT_EQ(1u, mentions) << text;
    EXPECT_NE(std::string::npos, text.find("/World/LampA")) << text;
    EXPECT_NE(std::string::npos, text.find("/World/LampB")) << text;
    EXPECT_EQ(std::string::npos, text.find("/World/Card")) << text;

    KnobFile* file = knob<KnobFile>(reader, kUSDReadParamFile);
    ASSERT_TRUE(file != nullptr);
    file->setValue(std::string(kFixture));
    ASSERT_TRUE(renderAll(node, &planes));
}

// A material authored only for another render context gets Cycles' fallback shader, and the
// node's warning names it.
TEST_F(USDRenderCyclesTest, WarnsOfAMaterialWithNoSurfaceForCycles)
{
    if (!cyclesOrSkip()) {
        return;
    }
    NodePtr node = createGraph(nullptr, kMdlOnlyFixture);
    ASSERT_TRUE(bool(node));
    std::vector<RenderedPlane> planes;
    std::string error;
    ASSERT_TRUE(renderNodePlanesDirect(node, 1., ViewIdx(0), 0, RectI(0, 0, kSize, kSize), allPlanes(), &planes, &error)) << error;

    ASSERT_TRUE(node->hasPersistentMessage());
    QString message;
    int type = 0;
    node->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeWarning, type);
    EXPECT_NE(std::string::npos, message.toStdString().find("/World/MdlOnly")) << message.toStdString();
}

// A render that would take minutes is aborted: render() returns within a second, and once the
// engine is gone the process keeps no thread per abort. Cycles and TBB threads carry no name that
// tells them from other pools' (they all show the process name), so the whole process is counted:
// the first two runs warm the lazily grown pools up and set the reference, and the test fails if
// the count after the last run is more than 2 above it (a thread leaked per abort adds 3 over the
// three measured runs) or if it rose at every measured run. A single unrelated thread starting
// meanwhile does neither.
TEST_F(USDRenderCyclesTest, AbortStopsTheRenderPromptly)
{
    if (!cyclesOrSkip()) {
        return;
    }
    HydraRender::setRendererSettings(kRenderDelegateCyclesId, fixedSampling(1000000));
    NodePtr reader;
    NodePtr node = createGraph(&reader);
    ASSERT_TRUE(bool(node));
    static const int owner = 0;
    HydraRender::Request request;
    ASSERT_TRUE(cyclesRequest(reader, &owner, &request));

    typedef std::chrono::steady_clock Clock;
    const int kRuns = 5;
    const int kWarmUpRuns = 2;
    const int kAllowance = 2;
    int reference = -1;
    int previous = -1;
    bool roseEveryRun = true;
    for (int run = 0; run < kRuns; ++run) {
        const Clock::time_point start = Clock::now();
        Clock::time_point abortedAt;
        bool abortSeen = false;
        request.aborted = [&]() {
            if (!abortSeen && (Clock::now() - start > std::chrono::milliseconds(500))) {
                abortSeen = true;
                abortedAt = Clock::now();
            }

            return abortSeen;
        };
        HydraRender::Result result;
        std::string error;
        EXPECT_FALSE(HydraRender::render(request, &result, &error));
        ASSERT_TRUE(abortSeen) << "the render ended before it was aborted: " << error;
        const double seconds = std::chrono::duration<double>(Clock::now() - abortedAt).count();
        EXPECT_LT(seconds, 1.) << "run " << run << ": " << error;
        EXPECT_NE(std::string::npos, error.find("cancelled")) << error;

        HydraRender::release(&owner);
        EXPECT_EQ(0u, HydraRender::getCpuEngineCount());
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        int threads = threadCount();
        if (run < kWarmUpRuns - 1) {
            continue;
        }
        if (run == kWarmUpRuns - 1) {
            reference = threads;
            previous = threads;
            continue;
        }
        if (run == kRuns - 1) {
            const Clock::time_point deadline = Clock::now() + std::chrono::seconds(2);
            while (threads > reference + kAllowance && Clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                threads = threadCount();
            }
            EXPECT_LE(threads, reference + kAllowance) << "reference " << reference;
        }
        roseEveryRun = roseEveryRun && threads > previous;
        previous = threads;
    }
    EXPECT_FALSE(roseEveryRun) << "the thread count rose at every measured run from " << reference;
}

// With the per-effect thread count at 2, Cycles renders on 2 threads: the process uses no more
// than about 2 cores' time over the render, which all cores would far exceed.
TEST_F(USDRenderCyclesTest, RendersWithinTheThreadBudget)
{
    if (!cyclesOrSkip()) {
        return;
    }
    const int budget = 2;
    const unsigned cores = std::thread::hardware_concurrency();
    if (cores <= (unsigned)budget + 1) {
        std::printf("[  SKIPPED ] %u cores cannot tell a budget of %d from all of them\n", cores, budget);

        return;
    }
    // Cycles sizes its device from the first session alive, so none may be.
    HydraRender::releaseAllCpuEngines();
    ASSERT_EQ(0u, HydraRender::getCpuEngineCount());
    int nThreadsToRender = 0;
    int nThreadsPerEffect = 0;
    appPTR->getNThreadsSettings(&nThreadsToRender, &nThreadsPerEffect);
    appPTR->setNThreadsPerEffect(budget);
    HydraRender::setRendererSettings(kRenderDelegateCyclesId, fixedSampling(2000));

    NodePtr node = createGraph();
    std::vector<RenderedPlane> planes;
    const double cpuBefore = processCpuSeconds();
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    const bool ok = node && renderAll(node, &planes);
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const double cpu = processCpuSeconds() - cpuBefore;
    appPTR->setNThreadsPerEffect(nThreadsPerEffect);
    ASSERT_TRUE(ok);
    ASSERT_GT(wall, 0.);
    EXPECT_LE(cpu / wall, budget + 0.75) << cpu << " s of CPU over " << wall << " s on " << cores << " cores";
}

// A render under a budget of 2 renders on 2 threads although another owner's Cycles engine, made
// under all the cores, is alive: Cycles would size the new session's device from that one's.
TEST_F(USDRenderCyclesTest, LaterEngineFollowsASmallerThreadBudget)
{
    if (!cyclesOrSkip()) {
        return;
    }
    const int budget = 2;
    const unsigned cores = std::thread::hardware_concurrency();
    if (cores <= (unsigned)budget + 1) {
        std::printf("[  SKIPPED ] %u cores cannot tell a budget of %d from all of them\n", cores, budget);

        return;
    }
    HydraRender::releaseAllCpuEngines();
    NodePtr reader;
    NodePtr node = createGraph(&reader);
    ASSERT_TRUE(bool(node));
    static const int wide = 0;
    static const int narrow = 1;
    HydraRender::Request request;
    ASSERT_TRUE(cyclesRequest(reader, &wide, &request));
    request.threads = (int)cores;
    HydraRender::Result result;
    std::string error;
    ASSERT_TRUE(HydraRender::render(request, &result, &error)) << error;

    HydraRender::setRendererSettings(kRenderDelegateCyclesId, fixedSampling(2000));
    request.owner = &narrow;
    request.threads = budget;
    const double cpuBefore = processCpuSeconds();
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ASSERT_TRUE(HydraRender::render(request, &result, &error)) << error;
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const double cpu = processCpuSeconds() - cpuBefore;
    ASSERT_GT(wall, 0.);
    EXPECT_LE(cpu / wall, budget + 0.75) << cpu << " s of CPU over " << wall << " s on " << cores << " cores";
}

// The pixels of a and b, kSize x kSize rgba, that differ by more than kChannelTolerance in a channel.
static std::size_t
differingPixels(const std::vector<float>& a,
                const std::vector<float>& b)
{
    std::size_t differing = 0;
    const std::size_t pixels = std::min(a.size(), b.size()) / 4;
    for (std::size_t p = 0; p < pixels; ++p) {
        bool differs = false;
        for (int k = 0; k < 4; ++k) {
            differs = differs || !(std::fabs(a[p * 4 + k] - b[p * 4 + k]) <= kChannelTolerance);
        }
        differing += differs ? 1 : 0;
    }

    return differing;
}

// An engine that rendered the stage renders it again after an edit as a new engine does: Cycles
// marks its buffers converged when the edit's reset cuts its render short, before it has rendered
// a sample of the edited stage.
TEST_F(USDRenderCyclesTest, EditedStageRendersAsWithAFreshEngine)
{
    if (!cyclesOrSkip()) {
        return;
    }
    HydraRender::releaseAllCpuEngines();
    NodePtr reader;
    NodePtr node = createGraph(&reader);
    ASSERT_TRUE(bool(node));
    NodePtr transform = createNode(QString::fromUtf8(PLUGINID_NATRON_USDEDITTRANSFORM));
    ASSERT_TRUE(bool(transform));
    connectNodes(reader, transform, 0, true);
    KnobString* pattern = knob<KnobString>(transform, kUSDEditTransformParamPrimPattern);
    KnobDouble* translate = knob<KnobDouble>(transform, kSceneParamTranslate);
    ASSERT_TRUE(pattern && translate);
    pattern->setValue(std::string("/World/Near"));

    static const int reused = 0;
    static const int fresh = 1;
    HydraRender::Request request;
    ASSERT_TRUE(cyclesRequest(transform, &reused, &request));
    HydraRender::Result before;
    std::string error;
    const std::chrono::steady_clock::time_point firstStart = std::chrono::steady_clock::now();
    ASSERT_TRUE(HydraRender::render(request, &before, &error)) << error;
    const double firstWall = std::chrono::duration<double>(std::chrono::steady_clock::now() - firstStart).count();

    // The near cube's front face, two units wide and nine from the camera, moves from the centre
    // to centre on x = -1: (16, 32) was off it and is now on it, (40, 32) the other way round.
    translate->setValue(-1., ViewSpec::all(), 0);
    ASSERT_TRUE(cyclesRequest(transform, &reused, &request));
    HydraRender::Result edited;
    const std::uint64_t builds = HydraRender::getEngineBuildCount();
    const std::chrono::steady_clock::time_point editedStart = std::chrono::steady_clock::now();
    ASSERT_TRUE(HydraRender::render(request, &edited, &error)) << error;
    const double editedWall = std::chrono::duration<double>(std::chrono::steady_clock::now() - editedStart).count();
    // A restart that went unseen would have waited two seconds and made the engine again.
    EXPECT_EQ(builds, HydraRender::getEngineBuildCount());
    EXPECT_LT(editedWall, firstWall + 1.) << "the edited render took " << editedWall << " s, the first " << firstWall << " s";
    request.owner = &fresh;
    HydraRender::Result reference;
    ASSERT_TRUE(HydraRender::render(request, &reference, &error)) << error;

    const std::size_t pixels = (std::size_t)kSize * (std::size_t)kSize;
    ASSERT_EQ(pixels * 4, before.rgba.size());
    ASSERT_EQ(pixels * 4, edited.rgba.size());
    ASSERT_EQ(pixels * 4, reference.rgba.size());
    const std::size_t movedOn = pixelIndex(16, kNearY);
    const std::size_t movedOff = pixelIndex(40, kNearY);
    EXPECT_EQ(0.f, before.rgba[movedOn * 4 + 3]);
    EXPECT_NEAR(1.f, before.rgba[movedOff * 4 + 3], 1e-3f);

    EXPECT_GT(edited.rgba[movedOn * 4] + edited.rgba[movedOn * 4 + 1] + edited.rgba[movedOn * 4 + 2], 0.05f);
    EXPECT_NEAR(1.f, edited.rgba[movedOn * 4 + 3], 1e-3f);
    EXPECT_EQ(0.f, edited.rgba[movedOff * 4 + 3]);
    const std::size_t differing = differingPixels(edited.rgba, reference.rgba);
    EXPECT_LE((double)differing, kDifferingFraction * (double)pixels) << differing << " of " << pixels << " pixels differ from a fresh engine's render";
}

// A stage with nothing authored in time, rendered again at another time and then as it was, shows
// the delegate nothing new: the engine renders on, with no wait for a restart that never comes.
TEST_F(USDRenderCyclesTest, UnchangedStageRendersAgainWithoutARestartWait)
{
    if (!cyclesOrSkip()) {
        return;
    }
    HydraRender::releaseAllCpuEngines();
    NodePtr reader;
    NodePtr node = createGraph(&reader);
    ASSERT_TRUE(bool(node));
    static const int owner = 0;
    HydraRender::Request request;
    ASSERT_TRUE(cyclesRequest(reader, &owner, &request));
    HydraRender::Result first;
    std::string error;
    ASSERT_TRUE(HydraRender::render(request, &first, &error)) << error;
    const std::uint64_t builds = HydraRender::getEngineBuildCount();
    const std::size_t pixels = (std::size_t)kSize * (std::size_t)kSize;
    ASSERT_EQ(pixels * 4, first.rgba.size());

    request.time = PXR_NS::UsdTimeCode(2.);
    HydraRender::Result later;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ASSERT_TRUE(HydraRender::render(request, &later, &error)) << error;
    const double laterWall = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    EXPECT_EQ(builds, HydraRender::getEngineBuildCount());
    EXPECT_LT(laterWall, 1.) << "the render at another time took " << laterWall << " s";
    ASSERT_EQ(pixels * 4, later.rgba.size());
    EXPECT_EQ(0u, differingPixels(first.rgba, later.rgba));

    HydraRender::Result again;
    start = std::chrono::steady_clock::now();
    ASSERT_TRUE(HydraRender::render(request, &again, &error)) << error;
    const double againWall = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    EXPECT_EQ(builds, HydraRender::getEngineBuildCount());
    EXPECT_LT(againWall, 1.) << "the unchanged render took " << againWall << " s";
    ASSERT_EQ(pixels * 4, again.rgba.size());
    EXPECT_EQ(0u, differingPixels(later.rgba, again.rgba));
}

// Records rgba as the golden case when NATRON_GOLDEN_RECORD_DIR is set, otherwise compares it to the
// recorded one within the tolerances above.
static void
matchGolden(const RenderedPlane& rgba,
            const char* goldenCase)
{
    const ImageLayerDesc layer = ImageLayerDesc::getRGBAComponents();
    const std::string recordDir = goldenRecordDir();
    if (!recordDir.empty()) {
        ParityF32Image image;
        image.width = rgba.window.width();
        image.height = rgba.window.height();
        image.nComps = (int)rgba.channels.size();
        image.pixels = rgba.pixels;
        const std::string path = parityReferencePath(recordDir, kGoldenGroup, goldenCase, 0, layer);
        std::string error;
        EXPECT_TRUE(writeParityF32(path, image, &error)) << "cannot record: " << error;
        std::cout << "[ golden ] recorded " << path << std::endl;

        return;
    }
    const std::string refDir = goldenReferenceDir();
    const std::string path = parityReferencePath(refDir, kGoldenGroup, goldenCase, 0, layer);
    ParityF32Image reference;
    std::string error;
    if (!readParityF32(path, &reference, &error)) {
        ADD_FAILURE() << "no golden reference " << path << " (" << error << "). Record it in the published build image "
                      << "by running this test with NATRON_GOLDEN_RECORD_DIR=" << refDir << " and commit the file.";

        return;
    }
    ASSERT_EQ(rgba.window.width(), reference.width) << path;
    ASSERT_EQ(rgba.window.height(), reference.height) << path;
    ASSERT_EQ((int)rgba.channels.size(), reference.nComps) << path;
    ASSERT_EQ(rgba.pixels.size(), reference.pixels.size()) << path;

    const int nComps = reference.nComps;
    const std::size_t pixels = reference.pixels.size() / (std::size_t)nComps;
    std::size_t differing = 0;
    double sum = 0.;
    float worst = 0.f;
    for (std::size_t p = 0; p < pixels; ++p) {
        bool differs = false;
        for (int k = 0; k < nComps; ++k) {
            const float d = std::fabs(rgba.pixels[p * nComps + k] - reference.pixels[p * nComps + k]);
            sum += d;
            differs = differs || !(d <= kChannelTolerance);
            worst = std::max(worst, d);
        }
        differing += differs ? 1 : 0;
    }
    const double mean = sum / (double)reference.pixels.size();
    EXPECT_LE((double)differing, kDifferingFraction * (double)pixels) << differing << " of " << pixels << " pixels differ from " << path << "; worst " << worst;
    EXPECT_LE(mean, kMeanTolerance) << "mean difference from " << path << "; worst " << worst;
}

// The fixture's rgba against the reference, within the tolerances above, and a second render on
// this host bit for bit equal to the first.
TEST_F(USDRenderCyclesTest, MatchesReferenceRender)
{
    if (!cyclesOrSkip()) {
        return;
    }
    NodePtr node = createGraph();
    ASSERT_TRUE(bool(node));
    std::vector<RenderedPlane> planes;
    ASSERT_TRUE(renderAll(node, &planes));
    const RenderedPlane& rgba = planes[0];

    // An engine made again renders the same samples.
    HydraRender::releaseAllCpuEngines();
    ASSERT_EQ(0u, HydraRender::getCpuEngineCount());
    std::vector<RenderedPlane> again;
    ASSERT_TRUE(renderAll(node, &again));
    std::size_t repeatDiffers = 0;
    for (std::size_t i = 0; i < rgba.pixels.size(); ++i) {
        repeatDiffers += (bitsOf(rgba.pixels[i]) != bitsOf(again[0].pixels[i])) ? 1 : 0;
    }
    EXPECT_EQ(0u, repeatDiffers) << "values differ between two renders on this host";

    matchGolden(rgba, kGoldenCase);
}

// A normal map on UsdPreviewSurface.normal, stored 0..1 and decoded by the texture's scale and bias,
// shades the card the way the recorded render does, and the card is not flat.
TEST_F(USDRenderCyclesTest, NormalMapMatchesReferenceRender)
{
    if (!cyclesOrSkip()) {
        return;
    }
    NodePtr node = createGraph(nullptr, kNormalMapFixture);
    ASSERT_TRUE(bool(node));
    std::vector<RenderedPlane> planes;
    ASSERT_TRUE(renderAll(node, &planes));
    const RenderedPlane& rgba = planes[0];

    // The bump leans its normals both ways across the card; an undecoded map (0..1 read as a
    // vector) or an ignored one gives a card as even as a flat one.
    float lowest = 1e30f;
    float highest = -1e30f;
    for (int y = kSize / 4; y < kSize - kSize / 4; ++y) {
        for (int x = kSize / 4; x < kSize - kSize / 4; ++x) {
            const float v = rgba.pixels[pixelIndex(x, y) * 4];
            lowest = std::min(lowest, v);
            highest = std::max(highest, v);
        }
    }
    EXPECT_GT(highest - lowest, kNormalMapContrast);

    matchGolden(rgba, kNormalMapGoldenCase);
}

// The emissiveColor of a black card is what it renders: red well above green, both near the
// authored (2, 0.5), which only the emission can supply.
TEST_F(USDRenderCyclesTest, EmissionMatchesReferenceRender)
{
    if (!cyclesOrSkip()) {
        return;
    }
    NodePtr node = createGraph(nullptr, kEmissionFixture);
    ASSERT_TRUE(bool(node));
    std::vector<RenderedPlane> planes;
    ASSERT_TRUE(renderAll(node, &planes));
    const RenderedPlane& rgba = planes[0];

    const std::size_t centre = pixelIndex(kNearX, kNearY) * 4;
    EXPECT_NEAR(2.f, rgba.pixels[centre], 0.2f);
    EXPECT_NEAR(0.5f, rgba.pixels[centre + 1], 0.1f);

    matchGolden(rgba, kEmissionGoldenCase);
}

// An opacity under the opacityThreshold leaves nothing of the card, one over it is solid.
TEST_F(USDRenderCyclesTest, OpacityThresholdCutsOut)
{
    if (!cyclesOrSkip()) {
        return;
    }
    NodePtr node = createGraph(nullptr, kCutoutFixture);
    ASSERT_TRUE(bool(node));
    std::vector<RenderedPlane> planes;
    ASSERT_TRUE(renderAll(node, &planes));
    const RenderedPlane& rgba = planes[0];

    EXPECT_NEAR(0.f, rgba.pixels[pixelIndex(kCutoutLeftX, kNearY) * 4 + 3], 1e-3f);
    EXPECT_NEAR(1.f, rgba.pixels[pixelIndex(kCutoutRightX, kNearY) * 4 + 3], 1e-3f);

    matchGolden(rgba, kCutoutGoldenCase);
}

// The coated sphere reflects the environment in a second layer, so it is brighter than the same
// sphere without a clearcoat.
TEST_F(USDRenderCyclesTest, ClearcoatMatchesReferenceRender)
{
    if (!cyclesOrSkip()) {
        return;
    }
    NodePtr node = createGraph(nullptr, kClearcoatFixture);
    ASSERT_TRUE(bool(node));
    std::vector<RenderedPlane> planes;
    ASSERT_TRUE(renderAll(node, &planes));
    const RenderedPlane& rgba = planes[0];

    const auto mean = [&rgba](int centreX) {
        double sum = 0.;
        int count = 0;
        for (int y = kNearY - kClearcoatHalfWidth; y <= kNearY + kClearcoatHalfWidth; ++y) {
            for (int x = centreX - kClearcoatHalfWidth; x <= centreX + kClearcoatHalfWidth; ++x) {
                sum += rgba.pixels[pixelIndex(x, y) * 4];
                ++count;
            }
        }

        return (float)(sum / count);
    };
    EXPECT_GT(mean(kClearcoatCoatedX), mean(kClearcoatPlainX) + kClearcoatGain);

    matchGolden(rgba, kClearcoatGoldenCase);
}

namespace {
// The Cycles knobs of the USDRenderSettings node feeding the USDRender node.
const RenderSettingsKnobs*
cyclesSettingsOf(const NodePtr& node)
{
    USDRenderSettings* settings = RenderSettingsTestUtils::settingsOf(node);

    return settings ? settings->renderSettings() : nullptr;
}

// What Cycles' engine for node reports for key; empty if node has no engine.
PXR_NS::VtValue
cyclesReported(const NodePtr& node,
               const std::string& key)
{
    PXR_NS::VtValue value;
    const void* owner = dynamic_cast<USDRender*>(node->getEffectInstance().get());

    if (!HydraRender::engineSetting(owner, key, &value)) {
        return PXR_NS::VtValue();
    }

    return value;
}

// Moves an enabled int, float, bool, menu or string knob off its default.
bool
nudgeCyclesKnob(KnobI* k)
{
    if (KnobBool* b = dynamic_cast<KnobBool*>(k)) {
        b->setValue(!b->getDefaultValue(0));
    } else if (KnobChoice* c = dynamic_cast<KnobChoice*>(k)) {
        c->setValue((c->getDefaultValue(0) + 1) % c->getNumEntries());
    } else if (KnobInt* i = dynamic_cast<KnobInt*>(k)) {
        i->setValue(i->getDefaultValue(0) + 1);
    } else if (KnobDouble* d = dynamic_cast<KnobDouble*>(k)) {
        d->setValue(d->getDefaultValue(0) + 0.5);
    } else if (KnobString* s = dynamic_cast<KnobString*>(k)) {
        s->setValue(s->getDefaultValue(0) + "nudged");
    } else {
        return false;
    }

    return true;
}

// The script name of each declared key's knob. Saved projects and scripts name these knobs, so a
// layout must not rename one.
const std::map<std::string, std::string>&
cyclesScriptNames()
{
    static const std::map<std::string, std::string> names = {
        { "cycles:integrator:aa_samples", "cyclesAaSamples" },
        { "cycles:integrator:adaptive_min_samples", "cyclesAdaptiveMinSamples" },
        { "cycles:integrator:adaptive_threshold", "cyclesAdaptiveThreshold" },
        { "cycles:integrator:ao_additive_factor", "cyclesAoAdditiveFactor" },
        { "cycles:integrator:ao_bounces", "cyclesAoBounces" },
        { "cycles:integrator:ao_distance", "cyclesAoDistance" },
        { "cycles:integrator:ao_factor", "cyclesAoFactor" },
        { "cycles:integrator:caustics_reflective", "cyclesCausticsReflective" },
        { "cycles:integrator:caustics_refractive", "cyclesCausticsRefractive" },
        { "cycles:integrator:custom_pixel_jitter_sample", "cyclesCustomPixelJitterSample" },
        { "cycles:integrator:denoise_start_sample", "cyclesDenoiseStartSample" },
        { "cycles:integrator:denoise_use_gpu", "cyclesDenoiseUseGpu" },
        { "cycles:integrator:denoiser_passes", "cyclesDenoiserPasses" },
        { "cycles:integrator:denoiser_prefilter", "cyclesDenoiserPrefilter" },
        { "cycles:integrator:denoiser_quality", "cyclesDenoiserQuality" },
        { "cycles:integrator:denoiser_type", "cyclesDenoiserType" },
        { "cycles:integrator:denoiser_upscale_factor", "cyclesDenoiserUpscaleFactor" },
        { "cycles:integrator:deterministic_guiding", "cyclesDeterministicGuiding" },
        { "cycles:integrator:filter_glossy", "cyclesFilterGlossy" },
        { "cycles:integrator:guiding_directional_sampling_type", "cyclesGuidingDirectionalSamplingType" },
        { "cycles:integrator:guiding_distribution_type", "cyclesGuidingDistributionType" },
        { "cycles:integrator:guiding_roughness_threshold", "cyclesGuidingRoughnessThreshold" },
        { "cycles:integrator:guiding_training_samples", "cyclesGuidingTrainingSamples" },
        { "cycles:integrator:light_sampling_threshold", "cyclesLightSamplingThreshold" },
        { "cycles:integrator:max_bounce", "cyclesMaxBounce" },
        { "cycles:integrator:max_diffuse_bounce", "cyclesMaxDiffuseBounce" },
        { "cycles:integrator:max_glossy_bounce", "cyclesMaxGlossyBounce" },
        { "cycles:integrator:max_transmission_bounce", "cyclesMaxTransmissionBounce" },
        { "cycles:integrator:max_volume_bounce", "cyclesMaxVolumeBounce" },
        { "cycles:integrator:min_bounce", "cyclesMinBounce" },
        { "cycles:integrator:motion_blur", "cyclesMotionBlur" },
        { "cycles:integrator:sample_clamp_direct", "cyclesSampleClampDirect" },
        { "cycles:integrator:sample_clamp_indirect", "cyclesSampleClampIndirect" },
        { "cycles:integrator:sample_subset_length", "cyclesSampleSubsetLength" },
        { "cycles:integrator:sample_subset_offset", "cyclesSampleSubsetOffset" },
        { "cycles:integrator:sampling_pattern", "cyclesSamplingPattern" },
        { "cycles:integrator:scrambling_distance", "cyclesScramblingDistance" },
        { "cycles:integrator:seed", "cyclesSeed" },
        { "cycles:integrator:surface_guiding_probability", "cyclesSurfaceGuidingProbability" },
        { "cycles:integrator:transparent_max_bounce", "cyclesTransparentMaxBounce" },
        { "cycles:integrator:transparent_min_bounce", "cyclesTransparentMinBounce" },
        { "cycles:integrator:use_adaptive_sampling", "cyclesUseAdaptiveSampling" },
        { "cycles:integrator:use_custom_pixel_jitter_sample", "cyclesUseCustomPixelJitterSample" },
        { "cycles:integrator:use_denoise", "cyclesUseDenoise" },
        { "cycles:integrator:use_diffuse", "cyclesUseDiffuse" },
        { "cycles:integrator:use_direct_light", "cyclesUseDirectLight" },
        { "cycles:integrator:use_emission", "cyclesUseEmission" },
        { "cycles:integrator:use_glossy", "cyclesUseGlossy" },
        { "cycles:integrator:use_guiding", "cyclesUseGuiding" },
        { "cycles:integrator:use_guiding_direct_light", "cyclesUseGuidingDirectLight" },
        { "cycles:integrator:use_guiding_mis_weights", "cyclesUseGuidingMisWeights" },
        { "cycles:integrator:use_indirect_light", "cyclesUseIndirectLight" },
        { "cycles:integrator:use_light_tree", "cyclesUseLightTree" },
        { "cycles:integrator:use_pixel_jitter", "cyclesUsePixelJitter" },
        { "cycles:integrator:use_sample_subset", "cyclesUseSampleSubset" },
        { "cycles:integrator:use_surface_guiding", "cyclesUseSurfaceGuiding" },
        { "cycles:integrator:use_transmission", "cyclesUseTransmission" },
        { "cycles:integrator:use_volume_guiding", "cyclesUseVolumeGuiding" },
        { "cycles:integrator:volume_guiding_probability", "cyclesVolumeGuidingProbability" },
        { "cycles:integrator:volume_max_steps", "cyclesVolumeMaxSteps" },
        { "cycles:integrator:volume_ray_marching", "cyclesVolumeRayMarching" },
        { "cycles:integrator:volume_step_rate", "cyclesVolumeStepRate" },
        { "cycles:background:lightgroup", "cyclesLightgroup" },
        { "cycles:background:transparent", "cyclesTransparent" },
        { "cycles:background:transparent_glass", "cyclesTransparentGlass" },
        { "cycles:background:transparent_roughness_threshold", "cyclesTransparentRoughnessThreshold" },
        { "cycles:background:use_shader", "cyclesUseShader" },
        { "cycles:background:visibility", "cyclesVisibility" },
        { "cycles:film:cryptomatte_depth", "cyclesCryptomatteDepth" },
        { "cycles:film:cryptomatte_passes", "cyclesCryptomattePasses" },
        { "cycles:film:denoising_pass_follow_reflections", "cyclesDenoisingPassFollowReflections" },
        { "cycles:film:denoising_pass_use_albedo_roughness_weighting", "cyclesDenoisingPassUseAlbedoRoughnessWeighting" },
        { "cycles:film:exposure", "cyclesExposure" },
        { "cycles:film:filter_type", "cyclesFilterType" },
        { "cycles:film:filter_width", "cyclesFilterWidth" },
        { "cycles:film:mist_depth", "cyclesMistDepth" },
        { "cycles:film:mist_falloff", "cyclesMistFalloff" },
        { "cycles:film:mist_start", "cyclesMistStart" },
        { "cycles:film:pass_alpha_threshold", "cyclesPassAlphaThreshold" },
        { "cycles:film:use_approximate_shadow_catcher", "cyclesUseApproximateShadowCatcher" },
        { "cycles:sample_offset", "cyclesSampleOffset" },
        { "cycles:samples", "cyclesSamples" },
        { "cycles:time_limit", "cyclesTimeLimit" },
    };

    return names;
}

struct CyclesGroupLayout {
    const char* group;
    const char* parent;
    bool open;
    std::vector<std::string> children;
};

const std::vector<CyclesGroupLayout>&
cyclesLayout()
{
    static const std::vector<CyclesGroupLayout> layout = {
        { "cyclesSampling", "cycles", true, { "cyclesSamples", "cyclesUseAdaptiveSampling", "cyclesAdaptiveThreshold", "cyclesAdaptiveMinSamples", "cyclesTimeLimit", "cyclesSeed" } },
        { "cyclesDenoise", "cycles", true, { "cyclesUseDenoise", "cyclesDenoiserType", "cyclesDenoiserQuality", "cyclesDenoiserPrefilter", "cyclesDenoiserPasses", "cyclesDenoiseStartSample", "cyclesDenoiserUpscaleFactor", "cyclesDenoiseUseGpu", "cyclesDenoisingPassFollowReflections", "cyclesDenoisingPassUseAlbedoRoughnessWeighting" } },
        { "cyclesLightPaths", "cycles", true, { "cyclesBounces", "cyclesClamping", "cyclesCaustics", "cyclesFastGi" } },
        { "cyclesBounces", "cyclesLightPaths", false, { "cyclesMaxBounce", "cyclesMaxDiffuseBounce", "cyclesMaxGlossyBounce", "cyclesMaxTransmissionBounce", "cyclesMaxVolumeBounce", "cyclesTransparentMaxBounce" } },
        { "cyclesClamping", "cyclesLightPaths", false, { "cyclesSampleClampDirect", "cyclesSampleClampIndirect" } },
        { "cyclesCaustics", "cyclesLightPaths", false, { "cyclesCausticsReflective", "cyclesCausticsRefractive", "cyclesFilterGlossy" } },
        { "cyclesFastGi", "cyclesLightPaths", false, { "cyclesAoBounces", "cyclesAoFactor", "cyclesAoDistance", "cyclesAoAdditiveFactor" } },
        { "cyclesLights", "cycles", false, { "cyclesUseLightTree", "cyclesLightSamplingThreshold" } },
        { "cyclesVolumes", "cycles", false, { "cyclesVolumeRayMarching", "cyclesVolumeMaxSteps", "cyclesVolumeStepRate" } },
        { "cyclesPathGuiding", "cycles", false, { "cyclesUseGuiding", "cyclesDeterministicGuiding", "cyclesUseSurfaceGuiding", "cyclesSurfaceGuidingProbability", "cyclesUseVolumeGuiding", "cyclesVolumeGuidingProbability", "cyclesGuidingTrainingSamples", "cyclesUseGuidingDirectLight", "cyclesUseGuidingMisWeights", "cyclesGuidingDistributionType", "cyclesGuidingDirectionalSamplingType", "cyclesGuidingRoughnessThreshold" } },
        { "cyclesLightContributions", "cycles", false, { "cyclesUseDirectLight", "cyclesUseIndirectLight", "cyclesUseDiffuse", "cyclesUseGlossy", "cyclesUseTransmission", "cyclesUseEmission" } },
        { "cyclesFilm", "cycles", false, { "cyclesExposure", "cyclesFilterType", "cyclesFilterWidth", "cyclesTransparent", "cyclesTransparentGlass", "cyclesTransparentRoughnessThreshold", "cyclesUseApproximateShadowCatcher" } },
        { "cyclesPasses", "cycles", false, { "cyclesMistStart", "cyclesMistDepth", "cyclesMistFalloff", "cyclesCryptomattePasses", "cyclesCryptomatteDepth", "cyclesPassAlphaThreshold" } },
        { "cyclesWorld", "cycles", false, { "cyclesUseShader", "cyclesVisibility", "cyclesLightgroup" } },
        { "cyclesAdvanced", "cycles", false, { "cyclesSamplingPattern", "cyclesScramblingDistance", "cyclesAaSamples", "cyclesMinBounce", "cyclesTransparentMinBounce", "cyclesMotionBlur", "cyclesUsePixelJitter", "cyclesUseCustomPixelJitterSample", "cyclesCustomPixelJitterSample", "cyclesSampleSubset" } },
        { "cyclesSampleSubset", "cyclesAdvanced", false, { "cyclesSampleOffset", "cyclesUseSampleSubset", "cyclesSampleSubsetOffset", "cyclesSampleSubsetLength" } },
    };

    return layout;
}

std::vector<std::string>
knobNames(const std::vector<KnobIPtr>& knobs)
{
    std::vector<std::string> names;

    for (const KnobIPtr& k : knobs) {
        names.push_back(k->getName());
    }

    return names;
}

void
collectGroups(const std::vector<KnobIPtr>& knobs,
              std::map<std::string, KnobIPtr>* groups)
{
    for (const KnobIPtr& k : knobs) {
        if (KnobGroup* group = dynamic_cast<KnobGroup*>(k.get())) {
            (*groups)[k->getName()] = k;
            collectGroups(group->getChildren(), groups);
        }
    }
}
} // anon namespace

// The script names survive the table: no key is lost, gained or renamed, and the groups are valid.
TEST_F(USDRenderCyclesTest, TableKeepsEveryScriptName)
{
    const RenderSettingsKnobs::Curated& curated = CyclesSettings::curated();
    std::string error;

    EXPECT_TRUE(RenderSettingsKnobs::validateGroups(curated, &error)) << error;
    std::map<std::string, std::string> names;
    for (const RenderSettingsKnobs::CuratedSetting& c : curated.settings) {
        names[c.key] = "cycles" + std::string(1, (char)std::toupper((unsigned char)c.name[0])) + c.name.substr(1);
        EXPECT_FALSE(c.group.empty()) << c.key;
    }
    EXPECT_EQ(curated.settings.size(), names.size());
    EXPECT_EQ(cyclesScriptNames(), names);
}

// Every knob sits in its group, in order, and each group has its parent, open state and label.
TEST_F(USDRenderCyclesTest, KnobsAreLaidOutInGroups)
{
    if (!cyclesOrSkip()) {
        return;
    }
    NodePtr node = createGraph();
    ASSERT_TRUE(bool(node));
    const RenderSettingsKnobs* settings = cyclesSettingsOf(node);
    ASSERT_TRUE(settings != nullptr);
    KnobPagePtr page = settings->page(kRenderDelegateCyclesId);
    ASSERT_TRUE(bool(page));

    std::vector<std::string> topLevel;
    for (const CyclesGroupLayout& expected : cyclesLayout()) {
        if (std::string(expected.parent) == page->getName()) {
            topLevel.push_back(expected.group);
        }
    }
    EXPECT_EQ(topLevel, knobNames(page->getChildren()));

    std::map<std::string, KnobIPtr> groups;
    collectGroups(page->getChildren(), &groups);
    EXPECT_EQ(cyclesLayout().size(), groups.size());
    for (const CyclesGroupLayout& expected : cyclesLayout()) {
        ASSERT_EQ(1u, groups.count(expected.group)) << expected.group;
        KnobIPtr k = groups[expected.group];
        KnobGroup* group = dynamic_cast<KnobGroup*>(k.get());
        ASSERT_TRUE(group != nullptr) << expected.group;
        ASSERT_TRUE(bool(k->getParentKnob())) << expected.group;
        EXPECT_EQ(std::string(expected.parent), k->getParentKnob()->getName()) << expected.group;
        EXPECT_EQ(expected.open, group->getValue()) << expected.group;
        EXPECT_EQ(expected.children, knobNames(group->getChildren())) << expected.group;
    }
    EXPECT_EQ(std::string("Fast GI"), groups["cyclesFastGi"]->getLabel());
}

// The table's types and defaults are the delegate's, and each menu sends Cycles' own enum values
// and starts on the entry of the declared default.
TEST_F(USDRenderCyclesTest, MenusStartOnTheDeclaredDefault)
{
    if (!cyclesOrSkip()) {
        return;
    }
    NodePtr node = createGraph();
    ASSERT_TRUE(bool(node));
    const RenderSettingsKnobs* settings = cyclesSettingsOf(node);
    ASSERT_TRUE(settings != nullptr);
    std::map<std::string, PXR_NS::VtValue> declared;
    for (const RenderSettingsKnobs::Descriptor& d : RenderSettingsKnobs::descriptors(kRenderDelegateCyclesId)) {
        declared[d.key] = d.defaultValue;
    }
    const std::map<std::string, std::vector<int>> menus = {
        { "cycles:integrator:denoiser_quality", { 1, 2, 3 } },
        { "cycles:integrator:denoiser_prefilter", { 1, 2, 3 } },
        { "cycles:integrator:sampling_pattern", { 0, 1, 2, 4, 3 } },
        { "cycles:integrator:guiding_distribution_type", { 0, 1, 2 } },
        { "cycles:integrator:guiding_directional_sampling_type", { 0, 1, 2 } },
        { "cycles:film:filter_type", { 0, 1, 2 } },
    };

    for (const RenderSettingsKnobs::CuratedSetting& c : CyclesSettings::curated().settings) {
        ASSERT_EQ(1u, declared.count(c.key)) << c.key;
        const PXR_NS::VtValue& declaredDefault = declared[c.key];
        EXPECT_EQ(declaredDefault.GetTypeName(), c.defaultValue.GetTypeName()) << c.key;
        KnobIPtr k = settings->knob(kRenderDelegateCyclesId, c.key);
        ASSERT_TRUE(bool(k)) << c.key;
        if (c.choices.empty()) {
            if (!c.overrideDefault) {
                EXPECT_EQ(declaredDefault, c.defaultValue) << c.key;
            }
            EXPECT_EQ(0u, menus.count(c.key)) << c.key;
            continue;
        }
        KnobChoice* choice = dynamic_cast<KnobChoice*>(k.get());
        ASSERT_TRUE(choice != nullptr) << c.key;
        ASSERT_EQ((int)c.choices.size(), choice->getNumEntries()) << c.key;
        const int index = choice->getDefaultValue(0);
        ASSERT_TRUE((index >= 0) && (index < (int)c.choices.size())) << c.key;
        EXPECT_EQ(index, choice->getValue()) << c.key;
        const PXR_NS::VtValue& entry = c.choices[index].value;
        EXPECT_EQ(declaredDefault, entry.IsEmpty() ? declaredDefault : entry) << c.key;
        const std::map<std::string, std::vector<int>>::const_iterator values = menus.find(c.key);
        if (values == menus.end()) {
            EXPECT_FALSE(c.disabledReason.empty()) << c.key << " is a menu the test does not list";
            continue;
        }
        std::vector<int> sent;
        for (const RenderSettingsKnobs::Choice& e : c.choices) {
            EXPECT_FALSE(e.label.empty()) << c.key;
            sent.push_back(e.value.Get<int>());
        }
        EXPECT_EQ(values->second, sent) << c.key;
    }
}

// Cycles' samples knob changes the node's hash, and the next render is made with the new count,
// as Cycles' engine reports it.
TEST_F(USDRenderCyclesTest, SamplesKnobChangesTheHashAndTheRender)
{
    if (!cyclesOrSkip()) {
        return;
    }
    HydraRender::setRendererSettings(kRenderDelegateCyclesId, std::map<std::string, PXR_NS::VtValue>());
    NodePtr node = createGraph();
    ASSERT_TRUE(bool(node));
    const RenderSettingsKnobs* settings = cyclesSettingsOf(node);
    ASSERT_TRUE(settings != nullptr);
    KnobInt* samples = dynamic_cast<KnobInt*>(settings->knob(kRenderDelegateCyclesId, CyclesSettings::samplesKey()).get());
    ASSERT_TRUE(samples != nullptr);
    EXPECT_EQ(std::string("cyclesSamples"), samples->getName());
    EXPECT_FALSE(samples->getIsSecret());
    samples->setValue(3);
    const U64 hash = node->getHashValue();
    std::vector<RenderedPlane> planes;
    ASSERT_TRUE(renderAll(node, &planes, true));
    EXPECT_EQ(PXR_NS::VtValue(3), cyclesReported(node, CyclesSettings::samplesKey()));

    samples->setValue(5);
    EXPECT_NE(hash, node->getHashValue());
    planes.clear();
    ASSERT_TRUE(renderAll(node, &planes, true));
    EXPECT_EQ(PXR_NS::VtValue(5), cyclesReported(node, CyclesSettings::samplesKey()));
}

// Each knob of the Cycles page sends its setting, which Cycles' engine reports back.
TEST_F(USDRenderCyclesTest, CuratedKnobsReachTheDelegate)
{
    if (!cyclesOrSkip()) {
        return;
    }
    HydraRender::setRendererSettings(kRenderDelegateCyclesId, std::map<std::string, PXR_NS::VtValue>());
    NodePtr node = createGraph();
    ASSERT_TRUE(bool(node));
    const RenderSettingsKnobs* settings = cyclesSettingsOf(node);
    ASSERT_TRUE(settings != nullptr);
    const std::string denoise = "cycles:integrator:use_denoise";
    const std::set<std::string> notNudged = {
        // Denoising is checked on its own: it needs OpenImageDenoise in Cycles' build.
        denoise,
        CyclesSettings::samplesKey(),
        // Session::update_scene() overwrites these from the session's own parameters, so the
        // engine never reports what was sent.
        "cycles:integrator:aa_samples",
        "cycles:integrator:use_sample_subset",
        "cycles:integrator:sample_subset_offset",
        "cycles:integrator:sample_subset_length",
        // A text knob holding numbers, which the nudge would fill with words. The empty sample with
        // both jitter switches nudged on is the case Cycles must survive.
        "cycles:integrator:custom_pixel_jitter_sample",
        // Its default is FLT_MAX, which adding 0.5 does not change, so it would never be sent.
        "cycles:integrator:ao_distance",
        // The denoising render below needs passes and a resolution it can run with.
        "cycles:integrator:denoiser_passes",
        "cycles:integrator:denoiser_upscale_factor",
    };
    std::vector<std::string> nudged;
    for (const RenderSettingsKnobs::CuratedSetting& c : CyclesSettings::curated().settings) {
        KnobIPtr k = settings->knob(kRenderDelegateCyclesId, c.key);
        ASSERT_TRUE(bool(k)) << c.key;
        if (!c.disabledReason.empty()) {
            EXPECT_FALSE(k->isEnabled(0)) << c.key;
            continue;
        }
        if (notNudged.count(c.key)) {
            continue;
        }
        ASSERT_TRUE(nudgeCyclesKnob(k.get())) << c.key;
        nudged.push_back(c.key);
    }
    KnobInt* samples = dynamic_cast<KnobInt*>(settings->knob(kRenderDelegateCyclesId, CyclesSettings::samplesKey()).get());
    ASSERT_TRUE(samples != nullptr);
    samples->setValue(2);
    nudged.push_back(CyclesSettings::samplesKey());
    std::vector<RenderedPlane> planes;
    ASSERT_TRUE(renderAll(node, &planes, true));
    RenderSettingsKnobs::Values sent = settings->values(kRenderDelegateCyclesId);
    for (const std::string& key : nudged) {
        ASSERT_EQ(1u, sent.count(key)) << key << " is not sent";
        EXPECT_EQ(sent[key], cyclesReported(node, key)) << key << " sent " << RenderSettingsKnobs::valueText(sent[key]);
    }

    KnobBool* useDenoise = dynamic_cast<KnobBool*>(settings->knob(kRenderDelegateCyclesId, denoise).get());
    ASSERT_TRUE(useDenoise != nullptr);
    useDenoise->setValue(true);
    planes.clear();
    std::string error;
    // Without OpenImageDenoise the render may fail; the setting still reaches the delegate.
    appPTR->clearAllCaches();
    (void)renderNodePlanesDirect(node, 1., ViewIdx(0), 0, RectI(0, 0, kSize, kSize), allPlanes(), &planes, &error, false);
    EXPECT_EQ(PXR_NS::VtValue(true), cyclesReported(node, denoise)) << error;
}

// Every setting Cycles declares is reached by a knob of its script name, and none by two.
TEST_F(USDRenderCyclesTest, EveryDeclaredSettingHasAKnob)
{
    if (!cyclesOrSkip()) {
        return;
    }
    NodePtr node = createGraph();
    ASSERT_TRUE(bool(node));
    const RenderSettingsKnobs* settings = cyclesSettingsOf(node);
    ASSERT_TRUE(settings != nullptr);
    const std::vector<std::pair<std::string, std::string>> audit = settings->audit(kRenderDelegateCyclesId);
    EXPECT_FALSE(audit.empty());
    std::set<std::string> names;
    std::set<std::string> setByNatron;
    std::map<std::string, std::string> reached;
    for (const std::pair<std::string, std::string>& reach : audit) {
        std::printf("[  AUDIT   ] Cycles %s -> %s\n", reach.first.c_str(), reach.second.empty() ? "UNREACHABLE" : reach.second.c_str());
        EXPECT_FALSE(reach.second.empty()) << reach.first;
        if (reach.second == "(set by Natron)") {
            setByNatron.insert(reach.first);
            continue;
        }
        EXPECT_TRUE(names.insert(reach.second).second) << reach.second << " reaches two settings";
        reached[reach.first] = reach.second;
    }
    EXPECT_EQ(std::set<std::string>({ "cycles:film:display_pass", "cycles:film:show_active_pixels", "cycles:film:use_sample_count" }), setByNatron);
    EXPECT_EQ(cyclesScriptNames(), reached);
}

// Film and background settings reach the delegate like the integrator ones, and it reports them back
// from the socket they name.
TEST_F(USDRenderCyclesTest, FilmAndBackgroundSettingsReachTheDelegate)
{
    if (!cyclesOrSkip()) {
        return;
    }
    NodePtr node = createGraph();
    ASSERT_TRUE(bool(node));
    const RenderSettingsKnobs* settings = cyclesSettingsOf(node);
    ASSERT_TRUE(settings != nullptr);
    const std::vector<std::string> keys = {
        "cycles:film:exposure",
        "cycles:film:filter_width",
        "cycles:background:transparent_roughness_threshold",
    };
    for (const std::string& key : keys) {
        KnobIPtr k = settings->knob(kRenderDelegateCyclesId, key);
        ASSERT_TRUE(bool(k)) << key;
        ASSERT_TRUE(nudgeCyclesKnob(k.get())) << key;
    }
    std::vector<RenderedPlane> planes;
    ASSERT_TRUE(renderAll(node, &planes, true));
    RenderSettingsKnobs::Values sent = settings->values(kRenderDelegateCyclesId);
    for (const std::string& key : keys) {
        ASSERT_EQ(1u, sent.count(key)) << key << " is not sent";
        EXPECT_EQ(sent[key], cyclesReported(node, key)) << key << " sent " << RenderSettingsKnobs::valueText(sent[key]);
    }
}

// Under a dome light Cycles renders the dome behind the scene; Transparent Background makes those
// pixels transparent while the dome still lights the cubes as before.
TEST_F(USDRenderCyclesTest, TransparentBackgroundHidesTheDomeOnly)
{
    if (!cyclesOrSkip()) {
        return;
    }
    NodePtr node = createGraph(nullptr, kDomeFixture);
    ASSERT_TRUE(bool(node));
    USDRenderSettings* settingsNode = RenderSettingsTestUtils::settingsOf(node);
    ASSERT_TRUE(settingsNode != nullptr);
    KnobBool* transparent = dynamic_cast<KnobBool*>(settingsNode->getKnobByName(kUSDRenderSettingsParamTransparentBackground).get());
    ASSERT_TRUE(transparent != nullptr);
    EXPECT_FALSE(transparent->getValue());

    std::vector<RenderedPlane> opaque;
    ASSERT_TRUE(renderAll(node, &opaque, true));
    const std::size_t corner = pixelIndex(0, 0);
    EXPECT_NEAR(1.f, opaque[0].pixels[corner * 4 + 3], 1e-3f);
    EXPECT_GT(opaque[0].pixels[corner * 4 + 2], 0.05f) << "the dome is not behind the scene";

    transparent->setValue(true);
    std::vector<RenderedPlane> hidden;
    ASSERT_TRUE(renderAll(node, &hidden, true));
    EXPECT_EQ(PXR_NS::VtValue(true), cyclesReported(node, "cycles:background:transparent"));
    EXPECT_EQ(0.f, hidden[0].pixels[corner * 4 + 3]);
    EXPECT_EQ(0.f, hidden[0].pixels[pixelIndex(kSize - 1, kSize - 1) * 4 + 3]);

    // The near cube's front face, two units wide, covers the middle eight pixels each way.
    for (int y = kNearY - 4; y < kNearY + 4; ++y) {
        for (int x = kNearX - 4; x < kNearX + 4; ++x) {
            const std::size_t p = pixelIndex(x, y);
            EXPECT_NEAR(1.f, hidden[0].pixels[p * 4 + 3], 1e-3f) << x << ", " << y;
            for (int c = 0; c < 3; ++c) {
                EXPECT_NEAR(opaque[0].pixels[p * 4 + c], hidden[0].pixels[p * 4 + c], kChannelTolerance) << x << ", " << y << " channel " << c;
            }
        }
    }
}
