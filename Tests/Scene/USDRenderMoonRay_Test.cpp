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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <list>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <unistd.h>

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
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/MoonRayAovs.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Nodes/Scene/USDRender.h"
#include "Engine/ViewIdx.h"
#include "RenderBothWays.h"
#include "ofxNatron.h"

NATRON_NAMESPACE_USING

/*
 * Renders of Tests/fixtures/scene-moonray.usda through a USDRender set to MoonRay, headless,
 * 32x32 at 2 samples a pixel, frame 1 (MoonRay seeds its samplers from the frame number).
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
// The fixture with a PortalLight, which hdMoonray has no Hydra prim type for.
const char* const kPortalFixture = NATRON_TESTS_FIXTURES_DIR "/scene-moonray-portal.usda";
const char* const kCamera = "/World/Cam";
const int kSize = 32;
const int kRow = 16;
const int kLeftColumn = 10;
const int kRightColumn = 25;
const int kMissColumn = 16;
const float kFaceDistance = 5.25f;

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

std::uint32_t
bitsOf(float value)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));

    return bits;
}

std::size_t
pixel(int row,
      int column)
{
    return (std::size_t)row * kSize + (std::size_t)column;
}

std::list<ImageLayerDesc>
allPlanes()
{
    static const char* const normalChannels[] = { "X", "Y", "Z" };
    std::list<ImageLayerDesc> layers;
    layers.push_back(ImageLayerDesc::getRGBAComponents());
    layers.push_back(ImageLayerDesc("depth", "depth", "", std::vector<std::string>(1, "Z")));
    layers.push_back(ImageLayerDesc("primId", "primId", "", std::vector<std::string>(1, "id")));
    layers.push_back(ImageLayerDesc("normal", "normal", "", std::vector<std::string>(normalChannels, normalChannels + 3)));

    return layers;
}

std::map<std::string, PXR_NS::VtValue>
pixelSamples(int samples)
{
    std::map<std::string, PXR_NS::VtValue> settings;
    settings["moonray:sceneVariable:pixel_samples"] = PXR_NS::VtValue(samples);

    return settings;
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

// Runs job while sampling the execComp children: the most threads any of them had.
int
peakRenderThreads(const std::function<bool()>& job)
{
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
    const bool ok = job();
    done = true;
    sampler.join();

    return ok ? peak.load() : -1;
}
} // anon namespace

class USDRenderMoonRayTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        HydraRender::setRendererSettings(kMoonRayRendererId, pixelSamples(2));
    }

    virtual void TearDown() OVERRIDE
    {
        HydraRender::setRendererSettings(kMoonRayRendererId, std::map<std::string, PXR_NS::VtValue>());
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
     * fixture read by a USDRead into a USDRender rendering it through MoonRay from its camera,
     * kSize x kSize. Returns the USDRender, null after a gtest failure; *reader gets the USDRead.
     **/
    NodePtr createGraph(const char* fixture = kFixture,
                        NodePtr* reader = nullptr)
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
        file->setValue(std::string(fixture));
        if (!selectRenderer(renderer, kMoonRayRendererId)) {
            ADD_FAILURE() << "the Renderer menu has no " << kMoonRayRendererId;

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
     * with MoonRay. False after a gtest failure.
     **/
    static bool moonRayRequest(const NodePtr& reader,
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
        request->renderer = kMoonRayRendererId;

        return true;
    }
};

TEST_F(USDRenderMoonRayTest, BeautyAndDepth)
{
    if (!moonRayOrSkip()) {
        return;
    }
    NodePtr node = createGraph();
    ASSERT_TRUE(bool(node));
    std::vector<RenderedPlane> planes;
    ASSERT_TRUE(renderAll(node, &planes));
    EXPECT_FALSE(node->hasPersistentMessage());
    const std::vector<float>& rgba = planes[0].pixels;
    const std::vector<float>& depth = planes[1].pixels;

    const std::size_t left = pixel(kRow, kLeftColumn);
    EXPECT_GT(rgba[left * 4] + rgba[left * 4 + 1] + rgba[left * 4 + 2], 0.f);
    EXPECT_NEAR(1.f, rgba[left * 4 + 3], 1e-3f);
    // Read from cameraDepth: the distance along the view axis, not hdMoonray's window depth.
    EXPECT_NEAR(kFaceDistance, depth[left], 0.01f);
    EXPECT_NEAR(kFaceDistance, depth[pixel(kRow, kRightColumn)], 0.01f);

    const std::size_t miss = pixel(kRow, kMissColumn);
    EXPECT_TRUE(std::isinf(depth[miss]) && (depth[miss] > 0.f)) << depth[miss];
    EXPECT_EQ(0.f, rgba[miss * 4 + 3]);
}

TEST_F(USDRenderMoonRayTest, PrimIdAndNormal)
{
    if (!moonRayOrSkip()) {
        return;
    }
    NodePtr node = createGraph();
    ASSERT_TRUE(bool(node));
    std::vector<RenderedPlane> planes;
    ASSERT_TRUE(renderAll(node, &planes));
    const std::vector<float>& primId = planes[2].pixels;
    const std::vector<float>& normal = planes[3].pixels;

    // hdMoonray writes Hydra's own prim and instance ids, which decode as Storm's do.
    const std::size_t left = pixel(kRow, kLeftColumn);
    const std::size_t right = pixel(kRow, kRightColumn);
    EXPECT_EQ(bitsOf(HydraRender::hashToFloat(HydraRender::primPathHash("/World/Left"))), bitsOf(primId[left]));
    EXPECT_EQ(bitsOf(HydraRender::hashToFloat(HydraRender::primPathHash("/World/Right"))), bitsOf(primId[right]));
    EXPECT_EQ(0u, bitsOf(primId[pixel(kRow, kMissColumn)]));
    std::set<std::uint32_t> ids;
    for (float value : primId) {
        if (bitsOf(value) != 0) {
            ids.insert(bitsOf(value));
        }
    }
    EXPECT_EQ(2u, ids.size());

    // The faces point down world +X, back at the camera: in its space, +Z. MoonRay writes the
    // normal in camera space already, so a second transform would turn it away.
    for (std::size_t at : { left, right }) {
        EXPECT_NEAR(0.f, normal[at * 3 + 0], 0.01f) << at;
        EXPECT_NEAR(0.f, normal[at * 3 + 1], 0.01f) << at;
        EXPECT_NEAR(1.f, normal[at * 3 + 2], 0.01f) << at;
    }
    const std::size_t miss = pixel(kRow, kMissColumn);
    EXPECT_EQ(0.f, normal[miss * 3]);
    EXPECT_EQ(0.f, normal[miss * 3 + 1]);
    EXPECT_EQ(0.f, normal[miss * 3 + 2]);
}

// A PortalLight has no hdMoonray prim type: Hydra would drop it silently, so the node warns of it
// by path and type, and renders the rest as before.
TEST_F(USDRenderMoonRayTest, WarnsOfAnUnsupportedLightAndStillRenders)
{
    if (!moonRayOrSkip()) {
        return;
    }
    ASSERT_FALSE(MoonRayAovs::supportedPrimTypes().empty());
    NodePtr node = createGraph(kPortalFixture);
    ASSERT_TRUE(bool(node));
    std::vector<RenderedPlane> planes;
    ASSERT_TRUE(renderAll(node, &planes));

    ASSERT_TRUE(node->hasPersistentMessage());
    QString message;
    int type = 0;
    node->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeWarning, type) << message.toStdString();
    EXPECT_TRUE(message.contains(QString::fromUtf8("/World/Portal (PortalLight)"))) << message.toStdString();

    const std::size_t left = pixel(kRow, kLeftColumn);
    EXPECT_GT(planes[0].pixels[left * 4] + planes[0].pixels[left * 4 + 1] + planes[0].pixels[left * 4 + 2], 0.f);

    // The warning goes with the light.
    NodePtr plain = createGraph();
    ASSERT_TRUE(bool(plain));
    ASSERT_TRUE(renderAll(plain, &planes));
    EXPECT_FALSE(plain->hasPersistentMessage());
}

// hdMoonray has no stop: an aborted render destroys the engine, whose Arras session ends its
// execComp as it goes. render() returns within a second of the abort, and no execComp is left.
TEST_F(USDRenderMoonRayTest, AbortEndsTheRenderProcessPromptly)
{
    if (!moonRayOrSkip()) {
        return;
    }
    // Enough samples that the frame is still rendering when it is aborted.
    HydraRender::setRendererSettings(kMoonRayRendererId, pixelSamples(4096));
    NodePtr reader;
    NodePtr node = createGraph(kFixture, &reader);
    ASSERT_TRUE(bool(node));
    static const int owner = 0;
    HydraRender::Request request;
    ASSERT_TRUE(moonRayRequest(reader, &owner, &request));

    typedef std::chrono::steady_clock Clock;
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
    EXPECT_NE(std::string::npos, error.find("cancelled")) << error;
    while (!execCompChildren().empty() && (Clock::now() - abortedAt < std::chrono::seconds(1))) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const double seconds = std::chrono::duration<double>(Clock::now() - abortedAt).count();
    EXPECT_LT(seconds, 1.);
    EXPECT_TRUE(execCompChildren().empty());
    HydraRender::release(&owner);
}

// With the per-effect thread count at 2, USDRender's MoonRay session renders on 2 threads. Arras
// starts the render on hardware_concurrency() minus the reserved cores, so its render process has
// fewer threads than an unbudgeted render's, by about the cores the budget leaves out; the
// process's own service threads are the same in both.
TEST_F(USDRenderMoonRayTest, RendersWithinTheThreadBudget)
{
    if (!moonRayOrSkip()) {
        return;
    }
    const int hardware = (int)std::thread::hardware_concurrency();
    if (hardware < 6) {
        std::printf("[  SKIPPED ] %d hardware threads leave no room to tell a budget of 2 apart\n", hardware);

        return;
    }
    NodePtr reader;
    NodePtr node = createGraph(kFixture, &reader);
    ASSERT_TRUE(bool(node));

    int nThreadsToRender = 0;
    int nThreadsPerEffect = 0;
    appPTR->getNThreadsSettings(&nThreadsToRender, &nThreadsPerEffect);
    appPTR->setNThreadsPerEffect(2);
    std::vector<RenderedPlane> planes;
    const int budgeted = peakRenderThreads([&]() { return renderAll(node, &planes); });
    appPTR->setNThreadsPerEffect(nThreadsPerEffect);
    ASSERT_GT(budgeted, 0);

    // The thread count is taken when the engine is made.
    HydraRender::releaseAllCpuEngines();
    static const int owner = 0;
    HydraRender::Request request;
    ASSERT_TRUE(moonRayRequest(reader, &owner, &request));
    request.threads = 0;
    std::string error;
    const int unbudgeted = peakRenderThreads([&]() {
        HydraRender::Result result;

        return HydraRender::render(request, &result, &error);
    });
    HydraRender::release(&owner);
    ASSERT_GT(unbudgeted, 0) << error;
    // The unbudgeted render reserves one core, the budgeted one hardware - 2.
    EXPECT_GE(unbudgeted - budgeted, (hardware - 3) / 2) << "budgeted " << budgeted << ", unbudgeted " << unbudgeted;
}

// The fixture rendered twice, by engines made apart, gives the same image: the same frame seeds
// the same samples.
TEST_F(USDRenderMoonRayTest, RerunIsIdentical)
{
    if (!moonRayOrSkip()) {
        return;
    }
    NodePtr node = createGraph();
    ASSERT_TRUE(bool(node));
    std::vector<RenderedPlane> first;
    ASSERT_TRUE(renderAll(node, &first));
    HydraRender::releaseAllCpuEngines();
    ASSERT_EQ(0u, HydraRender::getCpuEngineCount());
    std::vector<RenderedPlane> second;
    ASSERT_TRUE(renderAll(node, &second));
    for (std::size_t plane = 0; plane < first.size(); ++plane) {
        std::size_t differing = 0;
        for (std::size_t i = 0; i < first[plane].pixels.size(); ++i) {
            differing += (bitsOf(first[plane].pixels[i]) != bitsOf(second[plane].pixels[i])) ? 1 : 0;
        }
        EXPECT_EQ(0u, differing) << "plane " << plane << " differs between two renders on this host";
    }
}
