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
#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Nodes/Scene/USDRender.h"
#include "Engine/ViewIdx.h"
#include "RenderBothWays.h"
#include "Tests/Native/NativeGolden.h"
#include "ofxNatron.h"

NATRON_NAMESPACE_USING

namespace {
const char* const kFixture = NATRON_TESTS_FIXTURES_DIR "/scene-cycles.usda";
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
     * The fixture read by a USDRead into a USDRender rendering it through Cycles from its camera,
     * kSize x kSize. Returns the USDRender, null after a gtest failure; *reader gets the USDRead.
     **/
    NodePtr createGraph(NodePtr* reader = nullptr)
    {
        NodePtr read = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
        NodePtr render = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDER));
        if (!read || !render) {
            ADD_FAILURE() << "a 3D node is not registered";

            return NodePtr();
        }
        KnobFile* file = knob<KnobFile>(read, kUSDReadParamFile);
        KnobChoice* renderer = knob<KnobChoice>(render, kUSDRenderParamRenderer);
        KnobChoice* camera = knob<KnobChoice>(render, kUSDRenderParamCamera);
        KnobInt* size = knob<KnobInt>(render, kNatronParamFormatSize);
        KnobDouble* par = knob<KnobDouble>(render, kNatronParamFormatPar);
        if (!file || !renderer || !camera || !size || !par) {
            return NodePtr();
        }
        file->setValue(std::string(kFixture));
        if (!selectRenderer(renderer, kRenderDelegateCyclesId)) {
            ADD_FAILURE() << "the Renderer menu has no " << kRenderDelegateCyclesId;

            return NodePtr();
        }
        connectNodes(read, render, 0, true);
        camera->setActiveEntry(ChoiceOption(kCamera, kCamera, ""));
        size->setValue(kSize, ViewSpec::all(), 0);
        size->setValue(kSize, ViewSpec::all(), 1);
        par->setValue(1.);
        if (reader) {
            *reader = read;
        }

        return render;
    }

    // Renders node's rgba, depth, primId and normal planes, in that order.
    static bool renderAll(const NodePtr& node,
                          std::vector<RenderedPlane>* planes)
    {
        std::string error;
        const bool ok = renderNodePlanesDirect(node, 1., ViewIdx(0), 0, RectI(0, 0, kSize, kSize), allPlanes(), planes, &error);
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
        if (reader->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), &request->scene, &error) != eRenderRoIRetCodeOk) {
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

// A render that would take minutes is aborted: render() returns within a second, and once the
// engine is gone the process has no more threads than after an earlier render was aborted the
// same way (the TBB workers Cycles renders on outlive it, as they serve every TBB user).
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
    int baseline = -1;
    for (int run = 0; run < 2; ++run) {
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
        const int threads = threadCount();
        if (run == 0) {
            baseline = threads;
        } else {
            EXPECT_LE(threads, baseline);
        }
    }
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

    const ImageLayerDesc layer = ImageLayerDesc::getRGBAComponents();
    const std::string recordDir = goldenRecordDir();
    if (!recordDir.empty()) {
        ParityF32Image image;
        image.width = rgba.window.width();
        image.height = rgba.window.height();
        image.nComps = (int)rgba.channels.size();
        image.pixels = rgba.pixels;
        const std::string path = parityReferencePath(recordDir, kGoldenGroup, kGoldenCase, 0, layer);
        std::string error;
        EXPECT_TRUE(writeParityF32(path, image, &error)) << "cannot record: " << error;
        std::cout << "[ golden ] recorded " << path << std::endl;

        return;
    }
    const std::string refDir = goldenReferenceDir();
    const std::string path = parityReferencePath(refDir, kGoldenGroup, kGoldenCase, 0, layer);
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
