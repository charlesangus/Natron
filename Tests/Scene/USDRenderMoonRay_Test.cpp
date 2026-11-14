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
#include <array>
#include <atomic>
#include <cctype>
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
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>

#include <pxr/base/plug/registry.h>
#include <pxr/base/tf/diagnosticMgr.h>
#include <pxr/usd/usdGeom/basisCurves.h>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/CheckerBoard.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Generator/Ramp.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/LiveTextureSharedMemory.h"
#include "Engine/Nodes/Scene/LiveTextureStore.h"
#include "Engine/Nodes/Scene/MoonRayAovs.h"
#include "Engine/Nodes/Scene/MoonRaySettings.h"
#include "Engine/Nodes/Scene/RenderSettingsKnobs.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDAssignMaterial.h"
#include "Engine/Nodes/Scene/USDCamera.h"
#include "Engine/Nodes/Scene/USDCard.h"
#include "Engine/Nodes/Scene/USDEditTransform.h"
#include "Engine/Nodes/Scene/USDEmissiveMaterial.h"
#include "Engine/Nodes/Scene/USDLight.h"
#include "Engine/Nodes/Scene/USDMaterial.h"
#include "Engine/Nodes/Scene/USDMerge.h"
#include "Engine/Nodes/Scene/USDProject3D.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Nodes/Scene/USDRender.h"
#include "Engine/Nodes/Scene/USDRenderSettings.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"
#include "ProjectionTestUtils.h"
#include "RenderBothWays.h"
#include "RenderSettingsTestUtils.h"
#include "Tests/Native/NativeGolden.h"
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
 * 0.004) and the face normal to 0.01 per component.
 *
 * MoonRay draws the same samples on every run, its samplers seeded from pixel and frame, but it
 * cannot add them up in the same order. Arras renders progressively (its render computation
 * offers no batch mode): each pixel takes its samples over several passes, nothing holds a
 * tile's next pass back until its last one is done, and every pass, and in vectorized execution
 * every ray bundle, atomically adds into the pixel. Float addition does not associate, so the
 * sums differ in their last bits from run to run, and the half-float fine passes round some of
 * them to the neighbouring half. A second render is held to one half step; prim ids, which are
 * not summed, are compared exactly.
 */

namespace {
const char* const kFixture = NATRON_TESTS_FIXTURES_DIR "/scene-moonray.usda";
// The fixture with a PortalLight, which hdMoonray has no Hydra prim type for.
const char* const kPortalFixture = NATRON_TESTS_FIXTURES_DIR "/scene-moonray-portal.usda";
// A quad with the same image as a plain JPEG, as a plain PNG, and through maketx.
const char* const kTexturedJpg = NATRON_TESTS_FIXTURES_DIR "/scene-moonray-textured.usda";
const char* const kTexturedPng = NATRON_TESTS_FIXTURES_DIR "/scene-moonray-textured-png.usda";
const char* const kTexturedMaketx = NATRON_TESTS_FIXTURES_DIR "/scene-moonray-textured-tx.usda";
// The fixture's cubes with materials: Left's has a green DwaBaseMaterial on outputs:moonray:surface
// and a red UsdPreviewSurface on outputs:surface, Right's only a blue UsdPreviewSurface.
const char* const kRenderContextFixture = NATRON_TESTS_FIXTURES_DIR "/scene-moonray-render-context.usda";
const char* const kCamera = "/World/Cam";

// Keeps the text of every coding error posted while it is attached.
class CodingErrorCollector : public PXR_NS::TfDiagnosticMgr::Delegate {
public:
    CodingErrorCollector() { PXR_NS::TfDiagnosticMgr::GetInstance().AddDelegate(this); }
    ~CodingErrorCollector() override { PXR_NS::TfDiagnosticMgr::GetInstance().RemoveDelegate(this); }

    void IssueError(const PXR_NS::TfError& error) override
    {
        if (error.GetErrorCode() == PXR_NS::TF_DIAGNOSTIC_CODING_ERROR_TYPE) {
            std::lock_guard<std::mutex> lock(mutex);
            messages.push_back(error.GetCommentary());
        }
    }
    void IssueFatalError(const PXR_NS::TfCallContext&, const std::string&) override { }
    void IssueStatus(const PXR_NS::TfStatus&) override { }
    void IssueWarning(const PXR_NS::TfWarning&) override { }

    std::mutex mutex;
    std::vector<std::string> messages;
};
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

bool
withinHalfStep(float a,
               float b)
{
    if (bitsOf(a) == bitsOf(b)) {
        return true;
    }
    if (!std::isfinite(a) || !std::isfinite(b)) {
        return false;
    }
    int exponent = 0;
    (void)std::frexp(std::max(std::fabs(a), std::fabs(b)), &exponent);
    // A half has 10 mantissa bits; its smallest step is 2^-24.
    const float step = std::ldexp(1.f, std::max(exponent - 11, -24));

    return std::fabs(a - b) <= step;
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

// Runs job while sampling the execComp children it starts: the most threads any of them had. A
// child alive before, such as one a replaced engine's session is still ending, is not counted.
int
peakRenderThreads(const std::function<bool()>& job)
{
    const std::vector<int> before = execCompChildren();
    std::atomic<bool> done(false);
    std::atomic<int> peak(0);
    std::thread sampler([&]() {
        while (!done.load()) {
            for (int pid : execCompChildren()) {
                if (std::find(before.begin(), before.end(), pid) == before.end()) {
                    peak = std::max(peak.load(), threadCount(pid));
                }
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
     * fixture read by a USDRead into a USDRenderSettings whose prim frames its camera at kSize x
     * kSize, into a USDRender whose Renderer knob picks MoonRay. Returns the USDRender, null after a gtest failure; *reader gets the USDRead.
     **/
    NodePtr createGraph(const char* fixture = kFixture,
                        NodePtr* reader = nullptr)
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
        if (!selectRenderer(renderer, kMoonRayRendererId)) {
            ADD_FAILURE() << "the Renderer menu has no " << kMoonRayRendererId;

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

// One owner's engine follows a changed thread budget: the engine is made again, and its session
// renders on the new budget's threads, both when the budget grows and when it shrinks.
TEST_F(USDRenderMoonRayTest, FollowsAChangedThreadBudget)
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
    static const int owner = 0;
    HydraRender::Request request;
    ASSERT_TRUE(moonRayRequest(reader, &owner, &request));
    std::string error;
    const auto renderPeak = [&](int threads) {
        request.threads = threads;

        return peakRenderThreads([&]() {
            HydraRender::Result result;

            return HydraRender::render(request, &result, &error);
        });
    };

    const std::uint64_t builds = HydraRender::getEngineBuildCount();
    const int budgeted = renderPeak(2);
    ASSERT_GT(budgeted, 0) << error;
    EXPECT_EQ(builds + 1, HydraRender::getEngineBuildCount());
    // The same budget again reuses the engine.
    request.threads = 2;
    {
        HydraRender::Result result;
        ASSERT_TRUE(HydraRender::render(request, &result, &error)) << error;
    }
    EXPECT_EQ(builds + 1, HydraRender::getEngineBuildCount());

    const int unbudgeted = renderPeak(0);
    ASSERT_GT(unbudgeted, 0) << error;
    EXPECT_EQ(builds + 2, HydraRender::getEngineBuildCount());
    EXPECT_GE(unbudgeted - budgeted, (hardware - 3) / 2) << "budgeted " << budgeted << ", unbudgeted " << unbudgeted;

    const int rebudgeted = renderPeak(2);
    ASSERT_GT(rebudgeted, 0) << error;
    EXPECT_EQ(builds + 3, HydraRender::getEngineBuildCount());
    EXPECT_GE(unbudgeted - rebudgeted, (hardware - 3) / 2) << "rebudgeted " << rebudgeted << ", unbudgeted " << unbudgeted;
    HydraRender::release(&owner);
}

// The fixture rendered twice, by engines made apart, gives the same image to within the order
// MoonRay sums its samples in.
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
    const std::size_t primIdPlane = 2;
    for (std::size_t plane = 0; plane < first.size(); ++plane) {
        std::size_t differing = 0;
        for (std::size_t i = 0; i < first[plane].pixels.size(); ++i) {
            const float a = first[plane].pixels[i];
            const float b = second[plane].pixels[i];
            const bool same = (plane == primIdPlane) ? (bitsOf(a) == bitsOf(b)) : withinHalfStep(a, b);
            if (!same && (differing == 0)) {
                ADD_FAILURE() << "plane " << plane << " value " << i << ": " << a << " then " << b;
            }
            differing += same ? 0 : 1;
        }
        EXPECT_EQ(0u, differing) << "plane " << plane << " differs between two renders on this host";
    }
}

// The quad shows about 9 texels a pixel, so these lookups come from MIP levels. OpenImageIO's
// on-demand levels and maketx's are the same box filter and can differ only in the rounding of an
// 8-bit texel, 1/255 before MoonRay's 2.2 gamma; 0.02 holds that under the scene's lighting, while
// a render reading only the top level aliases the fixture's two-texel checker far beyond it.
TEST_F(USDRenderMoonRayTest, PlainTexturesMatchTheirMaketxConversion)
{
    if (!moonRayOrSkip()) {
        return;
    }
    const float tolerance = 0.02f;
    NodePtr maketxNode = createGraph(kTexturedMaketx);
    ASSERT_TRUE(bool(maketxNode));
    std::vector<RenderedPlane> maketx;
    ASSERT_TRUE(renderAll(maketxNode, &maketx));
    double sum = 0.;
    for (float value : maketx[0].pixels) {
        sum += value;
    }
    EXPECT_GT(sum / (double)maketx[0].pixels.size(), 0.05);

    for (const char* fixture : { kTexturedJpg, kTexturedPng }) {
        NodePtr node = createGraph(fixture);
        ASSERT_TRUE(bool(node));
        std::vector<RenderedPlane> plain;
        ASSERT_TRUE(renderAll(node, &plain));
        EXPECT_FALSE(node->hasPersistentMessage()) << fixture;
        std::size_t differing = 0;
        std::size_t notFinite = 0;
        for (std::size_t i = 0; i < plain[0].pixels.size(); ++i) {
            const float value = plain[0].pixels[i];
            notFinite += std::isfinite(value) ? 0 : 1;
            differing += (std::fabs(value - maketx[0].pixels[i]) <= tolerance) ? 0 : 1;
        }
        EXPECT_EQ(0u, notFinite) << fixture;
        EXPECT_EQ(0u, differing) << fixture;
    }
}

// hdMoonray reads the lightLink and shadowLink tokens of every light off a scene delegate that
// returns nothing for a light without links, and dirties the geometry through a change tracker that
// refuses it as emulation-only; both posted coding errors on every light.
TEST_F(USDRenderMoonRayTest, RendersWithoutCodingErrors)
{
    if (!moonRayOrSkip()) {
        return;
    }
    CodingErrorCollector collector;
    NodePtr node = createGraph(kTexturedPng);
    ASSERT_TRUE(bool(node));
    std::vector<RenderedPlane> planes;
    ASSERT_TRUE(renderAll(node, &planes));
    std::lock_guard<std::mutex> lock(collector.mutex);
    EXPECT_TRUE(collector.messages.empty()) << collector.messages.front();
}

// A material's outputs:moonray:surface wins over its universal outputs:surface, and a material with
// only universal outputs still renders through them.
TEST_F(USDRenderMoonRayTest, ReadsTheMoonRayRenderContext)
{
    if (!moonRayOrSkip()) {
        return;
    }
    NodePtr node = createGraph(kRenderContextFixture);
    ASSERT_TRUE(bool(node));
    std::vector<RenderedPlane> planes;
    ASSERT_TRUE(renderAll(node, &planes));
    EXPECT_FALSE(node->hasPersistentMessage());
    const std::vector<float>& rgba = planes[0].pixels;

    const std::size_t left = pixel(kRow, kLeftColumn);
    const float leftR = rgba[left * 4];
    const float leftG = rgba[left * 4 + 1];
    const float leftB = rgba[left * 4 + 2];
    EXPECT_GT(leftG, 2.f * leftR) << "Left renders (" << leftR << ", " << leftG << ", " << leftB << "), not its moonray green";
    EXPECT_GT(leftG, 2.f * leftB) << "Left renders (" << leftR << ", " << leftG << ", " << leftB << "), not its moonray green";

    const std::size_t right = pixel(kRow, kRightColumn);
    const float rightR = rgba[right * 4];
    const float rightG = rgba[right * 4 + 1];
    const float rightB = rgba[right * 4 + 2];
    EXPECT_GT(rightB, 2.f * rightR) << "Right renders (" << rightR << ", " << rightG << ", " << rightB << "), not its preview blue";
    EXPECT_GT(rightB, 2.f * rightG) << "Right renders (" << rightR << ", " << rightG << ", " << rightB << "), not its preview blue";
}

namespace {
// The MoonRay knobs of the USDRenderSettings node feeding the USDRender node.
const RenderSettingsKnobs*
moonRaySettingsOf(const NodePtr& node)
{
    USDRenderSettings* settings = RenderSettingsTestUtils::settingsOf(node);

    return settings ? settings->renderSettings() : nullptr;
}

// What MoonRay's engine for node reports for key; empty if node has no engine.
PXR_NS::VtValue
moonRayReported(const NodePtr& node,
                const std::string& key)
{
    PXR_NS::VtValue value;
    const void* owner = dynamic_cast<USDRender*>(node->getEffectInstance().get());

    if (!HydraRender::engineSetting(owner, key, &value)) {
        return PXR_NS::VtValue();
    }

    return value;
}

// Moves an enabled menu, int, float or bool knob off its default.
bool
nudgeMoonRayKnob(KnobI* k)
{
    if (KnobChoice* c = dynamic_cast<KnobChoice*>(k)) {
        if (c->getNumEntries() < 2) {
            return false;
        }
        c->setValue((c->getDefaultValue(0) + 1) % c->getNumEntries());
    } else if (KnobBool* b = dynamic_cast<KnobBool*>(k)) {
        b->setValue(!b->getDefaultValue(0));
    } else if (KnobInt* i = dynamic_cast<KnobInt*>(k)) {
        i->setValue(i->getDefaultValue(0) + 1);
    } else if (KnobDouble* d = dynamic_cast<KnobDouble*>(k)) {
        d->setValue(d->getDefaultValue(0) + 0.5);
    } else {
        return false;
    }

    return true;
}
// The script name of each declared key's knob. Saved projects and scripts name these knobs, so a
// layout must not rename one.
const std::map<std::string, std::string>&
moonRayScriptNames()
{
    static const std::map<std::string, std::string> names = {
        { "moonray:sceneVariable:pixel_samples", "moonrayPixelSamples" },
        { "moonray:sceneVariable:light_samples", "moonrayLightSamples" },
        { "moonray:sceneVariable:bsdf_samples", "moonrayBsdfSamples" },
        { "moonray:sceneVariable:bssrdf_samples", "moonrayBssrdfSamples" },
        { "moonray:sceneVariable:max_depth", "moonrayMaxDepth" },
        { "moonray:sceneVariable:max_diffuse_depth", "moonrayMaxDiffuseDepth" },
        { "moonray:sceneVariable:max_glossy_depth", "moonrayMaxGlossyDepth" },
        { "moonray:sceneVariable:max_mirror_depth", "moonrayMaxMirrorDepth" },
        { "moonray:sceneVariable:max_volume_depth", "moonrayMaxVolumeDepth" },
        { "moonray:sceneVariable:pixel_filter", "moonrayPixelFilter" },
        { "moonray:sceneVariable:pixel_filter_width", "moonrayPixelFilterWidth" },
        { "enableDenoise", "moonrayEnableDenoise" },
        { "enableOIDN", "moonrayEnableOIDN" },
        { "denoiseAlbedoGuiding", "moonrayDenoiseAlbedoGuiding" },
        { "denoiseNormalGuiding", "moonrayDenoiseNormalGuiding" },
        { "enableMotionBlur", "moonrayEnableMotionBlur" },
        { "disableLighting", "moonrayDisableLighting" },
        { "doubleSided", "moonrayDoubleSided" },
        { "decodeNormals", "moonrayDecodeNormals" },
        { "forcePolygon", "moonrayForcePolygon" },
        { "pruneWillow", "moonrayPruneWillow" },
        { "pruneFurDeform", "moonrayPruneFurDeform" },
        { "pruneVolume", "moonrayPruneVolume" },
        { "pruneWrapDeform", "moonrayPruneWrapDeform" },
        { "pruneCurveDeform", "moonrayPruneCurveDeform" },
        { "executionMode", "moonrayExecutionMode" },
        { "maxFps", "moonrayMaxFps" },
        { "useRemoteHosts", "moonrayUseRemoteHosts" },
        { "remoteHosts", "moonrayRemoteHosts" },
        { "logLevel", "moonrayLogLevel" },
        { "debug", "moonrayDebug" },
        { "info", "moonrayInfo" },
        { "rdlOutput", "moonrayRdlOutput" },
    };

    return names;
}

struct MoonRayGroupLayout {
    const char* group;
    const char* parent;
    bool open;
    std::vector<std::string> children;
};

const std::vector<MoonRayGroupLayout>&
moonRayLayout()
{
    static const std::vector<MoonRayGroupLayout> layout = {
        { "moonraySampling", "moonray", true, { "moonrayPixelSamples", "moonrayLightSamples", "moonrayBsdfSamples", "moonrayBssrdfSamples" } },
        { "moonrayDepth", "moonray", true, { "moonrayMaxDepth", "moonrayMaxDiffuseDepth", "moonrayMaxGlossyDepth", "moonrayMaxMirrorDepth", "moonrayMaxVolumeDepth" } },
        { "moonrayFiltering", "moonray", true, { "moonrayPixelFilter", "moonrayPixelFilterWidth" } },
        { "moonrayDenoise", "moonray", true, { "moonrayEnableDenoise", "moonrayEnableOIDN", "moonrayDenoiseAlbedoGuiding", "moonrayDenoiseNormalGuiding" } },
        { "moonraySceneToggles", "moonray", false, { "moonrayEnableMotionBlur", "moonrayDisableLighting", "moonrayDoubleSided", "moonrayDecodeNormals", "moonrayForcePolygon", "moonrayPrune" } },
        { "moonrayPrune", "moonraySceneToggles", false, { "moonrayPruneWillow", "moonrayPruneFurDeform", "moonrayPruneVolume", "moonrayPruneWrapDeform", "moonrayPruneCurveDeform" } },
        { "moonrayPerformance", "moonray", false, { "moonrayExecutionMode", "moonrayMaxFps" } },
        { "moonrayRemote", "moonray", false, { "moonrayUseRemoteHosts", "moonrayRemoteHosts" } },
        { "moonrayLogging", "moonray", false, { "moonrayLogLevel", "moonrayDebug", "moonrayInfo", "moonrayRdlOutput" } },
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
TEST_F(USDRenderMoonRayTest, TableKeepsEveryScriptName)
{
    const RenderSettingsKnobs::Curated& curated = MoonRaySettings::curated();
    std::string error;

    EXPECT_TRUE(RenderSettingsKnobs::validateGroups(curated, &error)) << error;
    std::map<std::string, std::string> names;
    for (const RenderSettingsKnobs::CuratedSetting& c : curated.settings) {
        names[c.key] = "moonray" + std::string(1, (char)std::toupper((unsigned char)c.name[0])) + c.name.substr(1);
        EXPECT_FALSE(c.group.empty()) << c.key;
    }
    EXPECT_EQ(curated.settings.size(), names.size());
    EXPECT_EQ(moonRayScriptNames(), names);
    EXPECT_TRUE(curated.disabledKeys.empty());
    EXPECT_EQ(std::string("MoonRay"), curated.pageLabel);
}

// Every knob sits in its group, in order, and each group has its parent, open state and label.
TEST_F(USDRenderMoonRayTest, KnobsAreLaidOutInGroups)
{
    if (!moonRayOrSkip()) {
        return;
    }
    NodePtr node = createGraph();
    ASSERT_TRUE(bool(node));
    const RenderSettingsKnobs* settings = moonRaySettingsOf(node);
    ASSERT_TRUE(settings != nullptr);
    KnobPagePtr page = settings->page(kMoonRayRendererId);
    ASSERT_TRUE(bool(page));
    EXPECT_EQ(std::string("MoonRay"), page->getLabel());

    std::vector<std::string> topLevel;
    for (const MoonRayGroupLayout& expected : moonRayLayout()) {
        if (std::string(expected.parent) == page->getName()) {
            topLevel.push_back(expected.group);
        }
    }
    EXPECT_EQ(topLevel, knobNames(page->getChildren()));

    std::map<std::string, KnobIPtr> groups;
    collectGroups(page->getChildren(), &groups);
    EXPECT_EQ(moonRayLayout().size(), groups.size());
    for (const MoonRayGroupLayout& expected : moonRayLayout()) {
        ASSERT_EQ(1u, groups.count(expected.group)) << expected.group;
        KnobIPtr k = groups[expected.group];
        KnobGroup* group = dynamic_cast<KnobGroup*>(k.get());
        ASSERT_TRUE(group != nullptr) << expected.group;
        ASSERT_TRUE(bool(k->getParentKnob())) << expected.group;
        EXPECT_EQ(std::string(expected.parent), k->getParentKnob()->getName()) << expected.group;
        EXPECT_EQ(expected.open, group->getValue()) << expected.group;
        EXPECT_EQ(expected.children, knobNames(group->getChildren())) << expected.group;
    }
}

// The table's types and defaults are the delegate's, Execution Mode is a menu that sends
// MoonRay's own strings, and the remote controls stay off with their reason.
TEST_F(USDRenderMoonRayTest, TableMatchesTheDelegate)
{
    if (!moonRayOrSkip()) {
        return;
    }
    NodePtr node = createGraph();
    ASSERT_TRUE(bool(node));
    const RenderSettingsKnobs* settings = moonRaySettingsOf(node);
    ASSERT_TRUE(settings != nullptr);
    std::map<std::string, PXR_NS::VtValue> declared;
    for (const RenderSettingsKnobs::Descriptor& d : RenderSettingsKnobs::descriptors(kMoonRayRendererId)) {
        declared[d.key] = d.defaultValue;
    }

    for (const RenderSettingsKnobs::CuratedSetting& c : MoonRaySettings::curated().settings) {
        KnobIPtr k = settings->knob(kMoonRayRendererId, c.key);
        ASSERT_TRUE(bool(k)) << c.key;
        if (declared.count(c.key) == 0) {
            // A scene variable, which hdMoonray takes without declaring.
            EXPECT_NE(c.key.find("moonray:sceneVariable:"), std::string::npos) << c.key;
            continue;
        }
        const PXR_NS::VtValue& declaredDefault = declared[c.key];
        EXPECT_EQ(declaredDefault.GetTypeName(), c.defaultValue.GetTypeName()) << c.key;
        if (c.choices.empty()) {
            EXPECT_EQ(declaredDefault, c.defaultValue) << c.key;
            continue;
        }
        KnobChoice* choice = dynamic_cast<KnobChoice*>(k.get());
        ASSERT_TRUE(choice != nullptr) << c.key;
        ASSERT_EQ((int)c.choices.size(), choice->getNumEntries()) << c.key;
        EXPECT_EQ(std::string("executionMode"), c.key);
        const int index = choice->getDefaultValue(0);
        ASSERT_TRUE((index >= 0) && (index < (int)c.choices.size())) << c.key;
        EXPECT_EQ(index, choice->getValue()) << c.key;
        EXPECT_EQ(declaredDefault, c.choices[index].value) << c.key;
        std::vector<std::string> sent;
        for (const RenderSettingsKnobs::Choice& e : c.choices) {
            EXPECT_FALSE(e.label.empty()) << c.key;
            sent.push_back(e.value.Get<std::string>());
        }
        EXPECT_EQ((std::vector<std::string> { "auto", "vectorized", "xpu", "scalar" }), sent);
    }
    for (const char* key : { "useRemoteHosts", "remoteHosts" }) {
        KnobIPtr k = settings->knob(kMoonRayRendererId, key);
        ASSERT_TRUE(bool(k)) << key;
        EXPECT_FALSE(k->isEnabled(0)) << key;
    }
}

// MoonRay's pixel samples knob changes the node's hash, and the next render is made with the new
// count, as MoonRay's engine reports it.
TEST_F(USDRenderMoonRayTest, PixelSamplesKnobChangesTheHashAndTheRender)
{
    if (!moonRayOrSkip()) {
        return;
    }
    HydraRender::setRendererSettings(kMoonRayRendererId, std::map<std::string, PXR_NS::VtValue>());
    NodePtr node = createGraph();
    ASSERT_TRUE(bool(node));
    const RenderSettingsKnobs* settings = moonRaySettingsOf(node);
    ASSERT_TRUE(settings != nullptr);
    KnobInt* samples = dynamic_cast<KnobInt*>(settings->knob(kMoonRayRendererId, MoonRaySettings::samplesKey()).get());
    ASSERT_TRUE(samples != nullptr);
    EXPECT_EQ(std::string("moonrayPixelSamples"), samples->getName());
    EXPECT_FALSE(samples->getIsSecret());
    samples->setValue(2);
    const U64 hash = node->getHashValue();
    std::vector<RenderedPlane> planes;
    ASSERT_TRUE(renderAll(node, &planes, true));
    EXPECT_EQ(PXR_NS::VtValue(2), moonRayReported(node, MoonRaySettings::samplesKey()));

    samples->setValue(3);
    EXPECT_NE(hash, node->getHashValue());
    planes.clear();
    ASSERT_TRUE(renderAll(node, &planes, true));
    EXPECT_EQ(PXR_NS::VtValue(3), moonRayReported(node, MoonRaySettings::samplesKey()));
}

// A prim in a file cannot reach what Natron keeps for itself: the reserved cores and retries an
// engine reports are Natron's, whatever the prim authors.
TEST_F(USDRenderMoonRayTest, APrimDoesNotOverrideReservedSettings)
{
    if (!moonRayOrSkip()) {
        return;
    }
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString filePath = tmp.path() + QString::fromUtf8("/reserved.usda");
    const std::string size = std::to_string(kSize);
    std::string text = "#usda 1.0\n(\n    renderSettingsPrimPath = \"/Render/shot\"\n    upAxis = \"Y\"\n";
    text += std::string("    subLayers = [@") + kFixture + "@]\n)\n";
    text += "def Scope \"Render\"\n{\n    def RenderSettings \"shot\"\n    {\n";
    text += std::string("        rel camera = <") + kCamera + ">\n";
    text += "        uniform int2 resolution = (" + size + ", " + size + ")\n";
    text += "        int moonray:localReservedCores = 9999\n";
    text += "        int moonray:maxConnectRetries = 7\n";
    text += "    }\n}\n";
    {
        QFile file(filePath);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        ASSERT_GT(file.write(text.c_str()), 0);
    }
    NodePtr read = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDER));
    ASSERT_TRUE(read && node);
    KnobFile* file = knob<KnobFile>(read, kUSDReadParamFile);
    KnobChoice* renderer = knob<KnobChoice>(node, kUSDRenderParamRenderer);
    ASSERT_TRUE(file && renderer);
    file->setValue(filePath.toStdString());
    ASSERT_TRUE(selectRenderer(renderer, kMoonRayRendererId));
    connectNodes(read, node, 0, true);

    std::vector<RenderedPlane> planes;
    ASSERT_TRUE(renderAll(node, &planes, true));
    const PXR_NS::VtValue reserved = moonRayReported(node, "localReservedCores");
    ASSERT_TRUE(reserved.IsHolding<int>());
    EXPECT_NE(9999, reserved.UncheckedGet<int>());
    EXPECT_EQ(PXR_NS::VtValue(0), moonRayReported(node, "maxConnectRetries"));
}

// Each knob of the MoonRay page sends its setting, which MoonRay's engine reports back.
TEST_F(USDRenderMoonRayTest, CuratedKnobsReachTheDelegate)
{
    if (!moonRayOrSkip()) {
        return;
    }
    HydraRender::setRendererSettings(kMoonRayRendererId, std::map<std::string, PXR_NS::VtValue>());
    NodePtr node = createGraph();
    ASSERT_TRUE(bool(node));
    const RenderSettingsKnobs* settings = moonRaySettingsOf(node);
    ASSERT_TRUE(settings != nullptr);
    const std::string denoise = "enableDenoise";
    std::vector<std::string> nudged;
    for (const RenderSettingsKnobs::CuratedSetting& c : MoonRaySettings::curated().settings) {
        KnobIPtr k = settings->knob(kMoonRayRendererId, c.key);
        ASSERT_TRUE(bool(k)) << c.key;
        if (!c.disabledReason.empty()) {
            EXPECT_FALSE(k->isEnabled(0)) << c.key;
            continue;
        }
        // Denoising is checked on its own: it needs OpenImageDenoise in MoonRay's build.
        // A text knob is not nudged, and a path set on rdlOutput would make MoonRay write a file.
        if ((c.key == denoise) || (c.key == MoonRaySettings::samplesKey()) || (c.key == "rdlOutput")) {
            continue;
        }
        ASSERT_TRUE(nudgeMoonRayKnob(k.get())) << c.key;
        nudged.push_back(c.key);
    }
    KnobInt* samples = dynamic_cast<KnobInt*>(settings->knob(kMoonRayRendererId, MoonRaySettings::samplesKey()).get());
    ASSERT_TRUE(samples != nullptr);
    samples->setValue(2);
    nudged.push_back(MoonRaySettings::samplesKey());
    std::vector<RenderedPlane> planes;
    ASSERT_TRUE(renderAll(node, &planes, true));
    RenderSettingsKnobs::Values sent = settings->values(kMoonRayRendererId);
    for (const std::string& key : nudged) {
        ASSERT_EQ(1u, sent.count(key)) << key << " is not sent";
        EXPECT_EQ(sent[key], moonRayReported(node, key)) << key << " sent " << RenderSettingsKnobs::valueText(sent[key]);
    }

    KnobBool* enableDenoise = dynamic_cast<KnobBool*>(settings->knob(kMoonRayRendererId, denoise).get());
    ASSERT_TRUE(enableDenoise != nullptr);
    enableDenoise->setValue(true);
    planes.clear();
    std::string error;
    // Without OpenImageDenoise the render may fail; the settings still reach the delegate.
    appPTR->clearAllCaches();
    (void)renderNodePlanesDirect(node, 1., ViewIdx(0), 0, RectI(0, 0, kSize, kSize), allPlanes(), &planes, &error, false);
    EXPECT_EQ(PXR_NS::VtValue(true), moonRayReported(node, denoise)) << error;
    EXPECT_EQ(PXR_NS::VtValue(true), moonRayReported(node, "enableOIDN")) << error;
}

// Every setting MoonRay declares, but those Natron sets itself, is reached by a knob of the
// MoonRay group or of Other Settings, and none by two.
TEST_F(USDRenderMoonRayTest, EveryDeclaredSettingHasAKnob)
{
    if (!moonRayOrSkip()) {
        return;
    }
    NodePtr node = createGraph();
    ASSERT_TRUE(bool(node));
    const RenderSettingsKnobs* settings = moonRaySettingsOf(node);
    ASSERT_TRUE(settings != nullptr);
    const std::vector<std::pair<std::string, std::string>> audit = settings->audit(kMoonRayRendererId);
    EXPECT_FALSE(audit.empty());
    std::set<std::string> names;
    for (const std::pair<std::string, std::string>& reach : audit) {
        std::printf("[  AUDIT   ] MoonRay %s -> %s\n", reach.first.c_str(), reach.second.empty() ? "UNREACHABLE" : reach.second.c_str());
        EXPECT_FALSE(reach.second.empty()) << reach.first;
        if (reach.second != "(set by Natron)") {
            EXPECT_TRUE(names.insert(reach.second).second) << reach.second << " reaches two settings";
        }
    }
}

namespace {
const char* const kLiveTextureGoldenGroup = "USDRender";
const char* const kLiveTextureGoldenCase = "MoonRayLiveTexture";
// One half step of the fine passes at these levels, as the fixture's header explains, on a render of
// only 2 samples a pixel.
const float kLiveTextureChannelTolerance = 0.02f;

void
matchLiveTextureGolden(const RenderedPlane& rgba)
{
    const ImageLayerDesc layer = ImageLayerDesc::getRGBAComponents();
    const std::string recordDir = goldenRecordDir();
    std::string error;

    if (!recordDir.empty()) {
        ParityF32Image image;
        image.width = rgba.window.width();
        image.height = rgba.window.height();
        image.nComps = (int)rgba.channels.size();
        image.pixels = rgba.pixels;
        const std::string path = parityReferencePath(recordDir, kLiveTextureGoldenGroup, kLiveTextureGoldenCase, 0, layer);
        EXPECT_TRUE(writeParityF32(path, image, &error)) << "cannot record: " << error;

        return;
    }
    const std::string refDir = goldenReferenceDir();
    const std::string path = parityReferencePath(refDir, kLiveTextureGoldenGroup, kLiveTextureGoldenCase, 0, layer);
    ParityF32Image reference;
    if (!readParityF32(path, &reference, &error)) {
        ADD_FAILURE() << "no golden reference " << path << " (" << error << "). Record it in the published build image "
                      << "by running this test with NATRON_GOLDEN_RECORD_DIR=" << refDir << " and commit the file.";

        return;
    }
    ASSERT_EQ(rgba.pixels.size(), reference.pixels.size()) << path;
    std::size_t differing = 0;
    for (std::size_t i = 0; i < reference.pixels.size(); ++i) {
        differing += (std::fabs(rgba.pixels[i] - reference.pixels[i]) <= kLiveTextureChannelTolerance) ? 0 : 1;
    }
    EXPECT_LE(differing, reference.pixels.size() / 100) << path;
}

// The texture files of this process's session.
std::vector<std::string>
sessionFiles()
{
    std::vector<std::string> names;
    const std::string session = LiveTextureSharedMemory::sessionName();
    DIR* directory = session.empty() ? nullptr : ::opendir(LiveTextureSharedMemory::directory().c_str());

    if (!directory) {
        return names;
    }
    const std::string prefix = "natron-" + session + "-";
    while (const struct dirent* entry = ::readdir(directory)) {
        const std::string name = entry->d_name;
        if (name.compare(0, prefix.size(), prefix) == 0) {
            names.push_back(name);
        }
    }
    ::closedir(directory);

    return names;
}
} // anonymous namespace

/*
 * A Natron image on a card's material, through MoonRay. Rendered off the main thread, where renders
 * run outside the tests: LiveTextureStore::prepare refuses the main thread, which is what the case
 * for a missing texture relies on. The project format keeps each image far below Docker's 64 MB
 * /dev/shm.
 */
class USDRenderMoonRayLiveTextureTest
    : public USDRenderMoonRayTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        USDRenderMoonRayTest::SetUp();
        ProjectPtr project = getApp()->getProject();
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kSize, kSize, "liveTexture", 1.));
    }

    virtual void TearDown() OVERRIDE
    {
        LiveTextureStore::instance().clear();
        USDRenderMoonRayTest::TearDown();
    }

    struct Graph {
        NodePtr material;
        NodePtr card;
        NodePtr merge;
        NodePtr render;
    };

    static void setPrimPath(const NodePtr& node,
                            const char* path)
    {
        KnobString* k = knob<KnobString>(node, kSceneParamPrimPath);
        if (k) {
            k->setValue(std::string(path));
        }
    }

    Graph createLiveTextureGraph(const NodePtr& source,
                                 bool emissive)
    {
        Graph graph;
        NodePtr material = createNode(QString::fromUtf8(emissive ? PLUGINID_NATRON_USDEMISSIVEMATERIAL : PLUGINID_NATRON_USDMATERIAL));
        NodePtr card = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCARD));
        NodePtr camera = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCAMERA));
        NodePtr light = createNode(QString::fromUtf8(PLUGINID_NATRON_USDLIGHT));
        NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMERGE));
        NodePtr settings = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDERSETTINGS));
        NodePtr render = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDER));
        if (!source || !material || !card || !camera || !light || !merge || !settings || !render) {
            ADD_FAILURE() << "a node is not registered";

            return graph;
        }
        setPrimPath(material, "/Live");
        setPrimPath(card, "/Card");
        setPrimPath(camera, "/Cam");
        setPrimPath(light, "/Key");
        KnobDouble* translate = knob<KnobDouble>(camera, kSceneParamTranslate);
        KnobChoice* renderer = knob<KnobChoice>(render, kUSDRenderParamRenderer);
        if (!translate || !renderer) {
            return graph;
        }
        translate->setValue(10., ViewSpec::all(), 2);
        if (!selectRenderer(renderer, kMoonRayRendererId)) {
            ADD_FAILURE() << "the Renderer menu has no " << kMoonRayRendererId;

            return graph;
        }
        connectNodes(source, material, emissive ? kUSDEmissiveMaterialInputEmissive : kUSDMaterialInputDiffuseColor, true);
        connectNodes(material, card, 0, true);
        connectNodes(card, merge, 0, true);
        connectNodes(camera, merge, 1, true);
        connectNodes(light, merge, 2, true);
        connectNodes(merge, settings, 0, true);
        connectNodes(settings, render, 0, true);
        if (!RenderSettingsTestUtils::configure(settings, "/Cam", kSize, kSize)) {
            return graph;
        }
        graph.material = material;
        graph.card = card;
        graph.merge = merge;
        graph.render = render;

        return graph;
    }

    NodePtr createConstant(double r,
                           double g,
                           double b,
                           double a = 1.)
    {
        NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
        KnobColor* color = constant ? knob<KnobColor>(constant, kConstantParamColor) : nullptr;
        if (color) {
            color->setValue(r, ViewSpec::all(), 0);
            color->setValue(g, ViewSpec::all(), 1);
            color->setValue(b, ViewSpec::all(), 2);
            color->setValue(a, ViewSpec::all(), 3);
        }

        return constant;
    }

    // Red at frame 1 and green at frame 10 over the whole image, with both of the ramp's colours keyed.
    NodePtr createAnimatedRamp()
    {
        NodePtr ramp = createNode(QString::fromUtf8(PLUGINID_NATRON_RAMP));
        if (!ramp) {
            ADD_FAILURE() << "Ramp is not registered";

            return ramp;
        }
        for (const char* name : { kRampParamColor0, kRampParamColor1 }) {
            KnobColor* color = knob<KnobColor>(ramp, name);
            if (!color) {
                return NodePtr();
            }
            color->setValueAtTime(1., 1., ViewSpec::all(), 0);
            color->setValueAtTime(10., 0., ViewSpec::all(), 0);
            color->setValueAtTime(1., 0., ViewSpec::all(), 1);
            color->setValueAtTime(10., 1., ViewSpec::all(), 1);
            color->setValue(0., ViewSpec::all(), 2);
            color->setValue(1., ViewSpec::all(), 3);
        }

        return ramp;
    }

    static std::list<ImageLayerDesc> colourPlane()
    {
        return std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents());
    }

    static bool renderOffMainThread(const NodePtr& render,
                                    double frame,
                                    std::vector<RenderedPlane>* planes)
    {
        bool rendered = false;
        std::string error;

        appPTR->clearAllCaches();
        std::thread worker([&]() {
            rendered = renderNodePlanesDirect(render, frame, ViewIdx(0), 0, RectI(0, 0, kSize, kSize), colourPlane(), planes, &error, false);
        });
        worker.join();
        appPTR->clearAllCaches();
        EXPECT_TRUE(rendered) << error;

        return rendered && (planes->size() == 1u);
    }

    // The mean of the 5 by 5 pixels at the centre, which the card covers.
    static std::array<double, 4> centreOf(const RenderedPlane& plane)
    {
        std::array<double, 4> mean = { 0., 0., 0., 0. };

        for (int y = kSize / 2 - 2; y <= kSize / 2 + 2; ++y) {
            for (int x = kSize / 2 - 2; x <= kSize / 2 + 2; ++x) {
                const std::size_t i = std::size_t(y) * kSize + x;
                for (int k = 0; k < 4; ++k) {
                    mean[k] += plane.pixels[i * 4 + k] / 25.;
                }
            }
        }

        return mean;
    }

    static std::array<double, 4> centreAtFrame(const NodePtr& render,
                                               double frame)
    {
        std::vector<RenderedPlane> planes;

        if (!renderOffMainThread(render, frame, &planes)) {
            return std::array<double, 4>();
        }

        return centreOf(planes[0]);
    }

    static std::string messageOf(const NodePtr& node)
    {
        QString message;
        int type = 0;
        node->getPersistentMessage(&message, &type, false);

        return message.toStdString();
    }
};

TEST_F(USDRenderMoonRayLiveTextureTest, AConstantOnACardRendersItsColour)
{
    if (!moonRayOrSkip()) {
        return;
    }
    const Graph graph = createLiveTextureGraph(createConstant(0., 1., 0.), false);
    ASSERT_TRUE(bool(graph.render));

    const std::array<double, 4> centre = centreAtFrame(graph.render, 1.);
    EXPECT_GT(centre[1], 0.02);
    EXPECT_GT(centre[1], 5. * centre[0]) << centre[0] << ", " << centre[1];
    EXPECT_GT(centre[1], 5. * centre[2]) << centre[1] << ", " << centre[2];
    EXPECT_FALSE(graph.render->hasPersistentMessage()) << messageOf(graph.render);
    EXPECT_FALSE(graph.material->hasPersistentMessage()) << messageOf(graph.material);
}

TEST_F(USDRenderMoonRayLiveTextureTest, AnAnimatedRampFollowsTheFrame)
{
    if (!moonRayOrSkip()) {
        return;
    }
    const Graph graph = createLiveTextureGraph(createAnimatedRamp(), false);
    ASSERT_TRUE(bool(graph.render));

    const std::array<double, 4> first = centreAtFrame(graph.render, 1.);
    const std::array<double, 4> tenth = centreAtFrame(graph.render, 10.);
    EXPECT_GT(first[0], 5. * first[1]) << first[0] << ", " << first[1];
    EXPECT_GT(tenth[1], 5. * tenth[0]) << tenth[0] << ", " << tenth[1];
}

// A source's texture is missing here because the main thread does not prepare it.
TEST_F(USDRenderMoonRayLiveTextureTest, AMissingTextureShowsMagentaAndWarnsOfItsSource)
{
    if (!moonRayOrSkip()) {
        return;
    }
    const NodePtr constant = createConstant(0., 1., 0.);
    const Graph graph = createLiveTextureGraph(constant, false);
    ASSERT_TRUE(bool(graph.render));

    std::vector<RenderedPlane> planes;
    std::string error;
    ASSERT_TRUE(renderNodePlanesDirect(graph.render, 1., ViewIdx(0), 0, RectI(0, 0, kSize, kSize), colourPlane(), &planes, &error, false)) << error;
    ASSERT_EQ(1u, planes.size());
    const std::array<double, 4> centre = centreOf(planes[0]);
    // Lit, so the hue is checked, not the value. hdMoonray's own handling would give 0.18 grey for
    // an empty path and white for a file it cannot open.
    EXPECT_GT(centre[0], 0.05) << centre[0] << ", " << centre[1] << ", " << centre[2];
    EXPECT_GT(centre[0], 5. * centre[1]);
    EXPECT_GT(centre[2], 5. * centre[1]);
    EXPECT_NEAR(centre[0], centre[2], 0.25 * std::max(centre[0], centre[2]));
    ASSERT_TRUE(graph.material->hasPersistentMessage());
    EXPECT_NE(std::string::npos, messageOf(graph.material).find(constant->getFullyQualifiedName())) << messageOf(graph.material);
}

TEST_F(USDRenderMoonRayLiveTextureTest, DeletingTheSourceShowsMagentaUntilUndone)
{
    if (!moonRayOrSkip()) {
        return;
    }
    const NodePtr constant = createConstant(0., 1., 0.);
    const Graph graph = createLiveTextureGraph(constant, false);
    ASSERT_TRUE(bool(graph.render));
    const std::string name = constant->getFullyQualifiedName();

    // What the delete commands' redo and undo do, with the same arguments.
    const NodesList outputs(1, graph.material);
    constant->deactivate(outputs, false, false, true, false);
    constant->notifyOutputsOfDeletion();
    const std::array<double, 4> missing = centreAtFrame(graph.render, 1.);
    // Lit, so the hue is checked, not the value.
    EXPECT_GT(missing[0], 0.05) << missing[0] << ", " << missing[1] << ", " << missing[2];
    EXPECT_GT(missing[0], 5. * missing[1]);
    EXPECT_GT(missing[2], 5. * missing[1]);
    EXPECT_NEAR(missing[0], missing[2], 0.25 * std::max(missing[0], missing[2]));
    ASSERT_TRUE(graph.material->hasPersistentMessage());
    EXPECT_NE(std::string::npos, messageOf(graph.material).find(name)) << messageOf(graph.material);

    constant->activate(outputs, false, false);
    const std::array<double, 4> restored = centreAtFrame(graph.render, 1.);
    EXPECT_GT(restored[1], 0.05) << restored[0] << ", " << restored[1] << ", " << restored[2];
    EXPECT_GT(restored[1], 5. * restored[0]);
    EXPECT_GT(restored[1], 5. * restored[2]);
    EXPECT_FALSE(graph.material->hasPersistentMessage()) << messageOf(graph.material);
}

/*
 * The store's entries and the filter's holds are all that keep a file. With the store emptied and the
 * engine alive, the filter holds the two latest of a texture node's files, lets go of a removed
 * material's at the next removal, and with the engine gone nothing is left in a filesystem made of RAM.
 */
TEST_F(USDRenderMoonRayLiveTextureTest, TheFilterHoldsTwoFilesPerTextureAndLetsGoOfRemovedOnes)
{
    if (!moonRayOrSkip()) {
        return;
    }
    const Graph graph = createLiveTextureGraph(createAnimatedRamp(), false);
    ASSERT_TRUE(bool(graph.render));
    LiveTextureStore& store = LiveTextureStore::instance();
    const auto renderFrame = [&](double frame) {
        std::vector<RenderedPlane> planes;

        return renderOffMainThread(graph.render, frame, &planes);
    };

    for (double frame : { 1., 2. }) {
        ASSERT_TRUE(renderFrame(frame));
    }
    store.clear();
    const std::vector<std::string> firstRange = sessionFiles();
    EXPECT_LE(firstRange.size(), 2u);

    for (double frame : { 3., 4. }) {
        ASSERT_TRUE(renderFrame(frame));
    }
    store.clear();
    const std::vector<std::string> secondRange = sessionFiles();
    EXPECT_LE(secondRange.size(), 2u);
    for (const std::string& name : firstRange) {
        EXPECT_EQ(secondRange.end(), std::find(secondRange.begin(), secondRange.end(), name)) << name << " outlived its frame range";
    }

    disconnectNodes(graph.material, graph.card, true);
    ASSERT_TRUE(renderFrame(4.));
    const std::vector<std::string> removed = sessionFiles();
    EXPECT_LE(removed.size(), 2u);

    disconnectNodes(graph.card, graph.merge, true);
    ASSERT_TRUE(renderFrame(4.));
    const std::vector<std::string> afterRemoval = sessionFiles();
    EXPECT_TRUE(afterRemoval.empty()) << afterRemoval.size() << " left with the engine alive, the first " << (afterRemoval.empty() ? std::string() : afterRemoval[0]);

    HydraRender::releaseAllCpuEngines();
    SceneQueryPool::instance().releaseIdle();
    const std::vector<std::string> files = sessionFiles();
    EXPECT_TRUE(files.empty()) << files.size() << " left, the first " << (files.empty() ? std::string() : files[0]);
}

TEST_F(USDRenderMoonRayLiveTextureTest, MatchesReferenceRender)
{
    if (!moonRayOrSkip()) {
        return;
    }
    NodePtr ramp = createNode(QString::fromUtf8(PLUGINID_NATRON_RAMP));
    ASSERT_TRUE(bool(ramp));
    KnobDouble* point0 = knob<KnobDouble>(ramp, kRampParamPoint0);
    KnobDouble* point1 = knob<KnobDouble>(ramp, kRampParamPoint1);
    KnobColor* color0 = knob<KnobColor>(ramp, kRampParamColor0);
    KnobColor* color1 = knob<KnobColor>(ramp, kRampParamColor1);
    ASSERT_TRUE(point0 && point1 && color0 && color1);
    point0->setValue(0., ViewSpec::all(), 0);
    point0->setValue(0., ViewSpec::all(), 1);
    point1->setValue(double(kSize), ViewSpec::all(), 0);
    point1->setValue(double(kSize), ViewSpec::all(), 1);
    color0->setValue(1., ViewSpec::all(), 0);
    color0->setValue(0., ViewSpec::all(), 1);
    color0->setValue(0., ViewSpec::all(), 2);
    color0->setValue(1., ViewSpec::all(), 3);
    color1->setValue(0., ViewSpec::all(), 0);
    color1->setValue(1., ViewSpec::all(), 1);
    color1->setValue(0., ViewSpec::all(), 2);
    color1->setValue(1., ViewSpec::all(), 3);
    const Graph graph = createLiveTextureGraph(ramp, false);
    ASSERT_TRUE(bool(graph.render));

    std::vector<RenderedPlane> planes;
    ASSERT_TRUE(renderOffMainThread(graph.render, 1., &planes));
    matchLiveTextureGolden(planes[0]);
}

namespace {
const char* const kGoldenCaseMoonRayProject3D = "MoonRayProject3DReference";
} // anon namespace

using namespace ProjectionTestUtils;

class USDRenderMoonRayProjectionTest
    : public ProjectionFixture<USDRenderMoonRayTest> {
protected:
    virtual const char* rendererId() const OVERRIDE
    {
        return kMoonRayRendererId;
    }
};

// Projected from the render camera, the plate reappears on the card pixel for pixel.
TEST_F(USDRenderMoonRayProjectionTest, APlateFromTheRenderCameraReproducesThePlate)
{
    if (!moonRayOrSkip()) {
        return;
    }
    const Graph graph = createProjectionGraph();
    ASSERT_TRUE(bool(graph.render));

    RenderedPlane rgba;
    ASSERT_TRUE(renderProjection(graph.render, 1., &rgba));
    EXPECT_PROJECTS(checkProjection(rgba, kProjectionPlaneWidth, 0.));
    EXPECT_FALSE(graph.projector->hasPersistentMessage()) << messageOf(graph.projector);
    EXPECT_FALSE(graph.render->hasPersistentMessage()) << messageOf(graph.render);
    matchGoldenCrop(rgba, RectI(16, 8, 112, 56), kGoldenCaseMoonRayProject3D);
}

// A plate larger than the format, projected from half the distance: Crop leaves the card around
// the projector's frame transparent, and without it the plate's overscan covers that band.
TEST_F(USDRenderMoonRayProjectionTest, CropHidesThePlateBeyondTheFrame)
{
    if (!moonRayOrSkip()) {
        return;
    }
    const Graph graph = createProjectionGraph();
    ASSERT_TRUE(bool(graph.render));
    KnobChoice* extent = knob<KnobChoice>(graph.plate, kNativeGeneratorParamExtent);
    KnobDouble* bottomLeft = knob<KnobDouble>(graph.plate, kNativeGeneratorParamBottomLeft);
    KnobDouble* size = knob<KnobDouble>(graph.plate, kNativeGeneratorParamSize);
    KnobBool* crop = knob<KnobBool>(graph.projector, kUSDProject3DParamCrop);
    ASSERT_TRUE(extent && bottomLeft && size && crop);
    extent->setValue((int)ExtentKnobs::eExtentSize);
    bottomLeft->setValue(-kProjectionWidth / 4., ViewSpec::all(), 0);
    bottomLeft->setValue(-kProjectionHeight / 4., ViewSpec::all(), 1);
    size->setValue(kProjectionWidth * 1.5, ViewSpec::all(), 0);
    size->setValue(kProjectionHeight * 1.5, ViewSpec::all(), 1);
    ASSERT_TRUE(setTriple(graph.projectorCamera, kSceneParamTranslate, 0., 0., kProjectionCameraZ / 2.));

    // The frame covers render pixels 32..96 by 16..48 and the overscan 16..112 by 8..56.
    const RectI frame(36, 20, 92, 44);
    const RectI band(19, 20, 29, 44);
    RenderedPlane cropped;
    crop->setValue(true);
    ASSERT_TRUE(renderProjection(graph.render, 1., &cropped));
    EXPECT_PROJECTS(checkProjection(cropped, kProjectionPlaneWidth / 2., 0.));
    EXPECT_EQ(band.area(), countIn(cropped, band, eProjectedTransparent));

    RenderedPlane uncropped;
    crop->setValue(false);
    ASSERT_TRUE(renderProjection(graph.render, 1., &uncropped));
    EXPECT_EQ(0, countIn(uncropped, band, eProjectedTransparent));
    EXPECT_GT(countIn(uncropped, band, eProjectedRed) + countIn(uncropped, band, eProjectedBlue), band.area() / 2);
    EXPECT_GT(countIn(uncropped, frame, eProjectedRed) + countIn(uncropped, frame, eProjectedBlue), frame.area() / 2);
}

// The card faces the projector: Front keeps the plate and Back shows the outside mode instead.
TEST_F(USDRenderMoonRayProjectionTest, ProjectOnPicksTheFacesThatShowThePlate)
{
    if (!moonRayOrSkip()) {
        return;
    }
    const Graph graph = createProjectionGraph();
    ASSERT_TRUE(bool(graph.render));
    KnobChoice* projectOn = knob<KnobChoice>(graph.projector, kUSDProject3DParamProjectOn);
    KnobChoice* outside = knob<KnobChoice>(graph.projector, kUSDProject3DParamOutside);
    ASSERT_TRUE(projectOn && outside);

    RenderedPlane rgba;
    projectOn->setValue((int)USDProject3D::eProjectOnFront);
    ASSERT_TRUE(renderProjection(graph.render, 1., &rgba));
    EXPECT_PROJECTS(checkProjection(rgba, kProjectionPlaneWidth, 0.));

    projectOn->setValue((int)USDProject3D::eProjectOnBack);
    outside->setValue((int)USDProject3D::eOutsideBlack);
    ASSERT_TRUE(renderProjection(graph.render, 1., &rgba));
    EXPECT_EQ(kProjectionCardInterior.area(), countIn(rgba, kProjectionCardInterior, eProjectedBlack));

    outside->setValue((int)USDProject3D::eOutsideTransparent);
    ASSERT_TRUE(renderProjection(graph.render, 1., &rgba));
    EXPECT_EQ(kProjectionCardInterior.area(), countIn(rgba, kProjectionCardInterior, eProjectedTransparent));
}

// The projector moves 2 units along x from frame 1 to frame 10, and the plate with it.
TEST_F(USDRenderMoonRayProjectionTest, AnAnimatedProjectorFollowsTheFrame)
{
    if (!moonRayOrSkip()) {
        return;
    }
    const Graph graph = createProjectionGraph();
    ASSERT_TRUE(bool(graph.render));
    KnobDouble* translate = knob<KnobDouble>(graph.projectorCamera, kSceneParamTranslate);
    ASSERT_TRUE(translate);
    translate->setValueAtTime(1., 0., ViewSpec::all(), 0);
    translate->setValueAtTime(10., 2., ViewSpec::all(), 0);

    RenderedPlane first, tenth;
    ASSERT_TRUE(renderProjection(graph.render, 1., &first));
    ASSERT_TRUE(renderProjection(graph.render, 10., &tenth));
    EXPECT_PROJECTS(checkProjection(first, kProjectionPlaneWidth, 0.));
    EXPECT_PROJECTS(checkProjection(tenth, kProjectionPlaneWidth, 2.));
    int differing = 0;
    for (int y = kProjectionCardInterior.y1; y < kProjectionCardInterior.y2; ++y) {
        for (int x = kProjectionCardInterior.x1; x < kProjectionCardInterior.x2; ++x) {
            differing += (classify(first, x, y) != classify(tenth, x, y)) ? 1 : 0;
        }
    }
    EXPECT_GT(differing, kProjectionCardInterior.area() / 4);
}

// An orthographic projector's aperture sets the frame whatever its distance. The knob is in
// millimetres and the stage in metres, so 20000 x 10000 spans the render camera's 20 x 10 units of
// the card's plane from 10 units and from 40.
TEST_F(USDRenderMoonRayProjectionTest, AnOrthographicProjectorIgnoresItsDistance)
{
    if (!moonRayOrSkip()) {
        return;
    }
    const Graph graph = createProjectionGraph();
    ASSERT_TRUE(bool(graph.render));
    KnobChoice* projection = knob<KnobChoice>(graph.projectorCamera, kUSDCameraParamProjection);
    ASSERT_TRUE(projection);
    ASSERT_TRUE(selectRenderer(projection, "orthographic"));
    ASSERT_TRUE(setLens(graph.projectorCamera, 20000., 10000.));

    RenderedPlane rgba;
    for (double z : { kProjectionCameraZ, 4. * kProjectionCameraZ }) {
        SCOPED_TRACE(z);
        ASSERT_TRUE(setTriple(graph.projectorCamera, kSceneParamTranslate, 0., 0., z));
        ASSERT_TRUE(renderProjection(graph.render, 1., &rgba));
        EXPECT_PROJECTS(checkProjection(rgba, kProjectionPlaneWidth, 0.));
    }
}

// The card is 10 units from the projector: a far clip of 5 or a near clip of 12 puts it outside.
TEST_F(USDRenderMoonRayProjectionTest, ClipNearFarPutsTheCardOutside)
{
    if (!moonRayOrSkip()) {
        return;
    }
    const Graph graph = createProjectionGraph();
    ASSERT_TRUE(bool(graph.render));
    KnobBool* clip = knob<KnobBool>(graph.projector, kUSDProject3DParamClipNearFar);
    KnobChoice* outside = knob<KnobChoice>(graph.projector, kUSDProject3DParamOutside);
    KnobDouble* nearClip = knob<KnobDouble>(graph.projectorCamera, kUSDCameraParamNear);
    KnobDouble* farClip = knob<KnobDouble>(graph.projectorCamera, kUSDCameraParamFar);
    ASSERT_TRUE(clip && outside && nearClip && farClip);
    outside->setValue((int)USDProject3D::eOutsideBlack);
    nearClip->setValue(1.);
    farClip->setValue(5.);

    RenderedPlane rgba;
    clip->setValue(false);
    ASSERT_TRUE(renderProjection(graph.render, 1., &rgba));
    EXPECT_PROJECTS(checkProjection(rgba, kProjectionPlaneWidth, 0.));

    clip->setValue(true);
    ASSERT_TRUE(renderProjection(graph.render, 1., &rgba));
    EXPECT_EQ(kProjectionCardInterior.area(), countIn(rgba, kProjectionCardInterior, eProjectedBlack));

    farClip->setValue(100.);
    ASSERT_TRUE(renderProjection(graph.render, 1., &rgba));
    EXPECT_PROJECTS(checkProjection(rgba, kProjectionPlaneWidth, 0.));

    nearClip->setValue(12.);
    ASSERT_TRUE(renderProjection(graph.render, 1., &rgba));
    EXPECT_EQ(kProjectionCardInterior.area(), countIn(rgba, kProjectionCardInterior, eProjectedBlack));
}

// In Geometry space held at frame 1 the plate sticks to the card as it moves 2 units by frame 11;
// in World space it stays where the projector puts it.
TEST_F(USDRenderMoonRayProjectionTest, GeometrySpaceHeldSticksToTheMovingCard)
{
    if (!moonRayOrSkip()) {
        return;
    }
    double first = 0., last = 0.;
    getApp()->getFrameRange(&first, &last);
    ASSERT_LE(first, 1.);
    ASSERT_GE(last, 11.);
    const Graph graph = createProjectionGraph();
    ASSERT_TRUE(bool(graph.render));
    KnobChoice* space = knob<KnobChoice>(graph.projector, kUSDProject3DParamProjectionSpace);
    KnobBool* useHold = knob<KnobBool>(graph.projector, kUSDProject3DParamUseFrameHold);
    KnobDouble* hold = knob<KnobDouble>(graph.projector, kUSDProject3DParamFrameHold);
    KnobDouble* translate = knob<KnobDouble>(graph.card, kSceneParamTranslate);
    ASSERT_TRUE(space && useHold && hold && translate);
    const double shift = 2.;
    translate->setValueAtTime(1., 0., ViewSpec::all(), 0);
    translate->setValueAtTime(11., shift, ViewSpec::all(), 0);

    RenderedPlane rgba;
    ASSERT_TRUE(renderProjection(graph.render, 11., &rgba));
    EXPECT_PROJECTS(checkProjection(rgba, kProjectionPlaneWidth, 0., shift, 0.));

    space->setValue((int)USDProject3D::eProjectionSpaceGeometry);
    useHold->setValue(true);
    hold->setValue(1.);
    ASSERT_TRUE(renderProjection(graph.render, 1., &rgba));
    EXPECT_PROJECTS(checkProjection(rgba, kProjectionPlaneWidth, 0.));
    ASSERT_TRUE(renderProjection(graph.render, 11., &rgba));
    EXPECT_PROJECTS(checkProjection(rgba, kProjectionPlaneWidth, 0., shift, shift));
}

// Linear curves, not a mesh: held in Geometry space, the plate rides on the projection's frame
// alone as the curves' transform carries them.
TEST_F(USDRenderMoonRayProjectionTest, GeometrySpaceHeldSticksToMovingCurvesWithoutReferencePoints)
{
    if (!moonRayOrSkip()) {
        return;
    }
    expectHeldCurvesCarryThePlate();
}

// A transform after the card carries the plate along in Geometry space, and a mirror there
// mirrors it.
TEST_F(USDRenderMoonRayProjectionTest, GeometrySpaceFollowsATransformAfterTheCard)
{
    if (!moonRayOrSkip()) {
        return;
    }
    expectATransformAfterTheCardCarriesThePlate();
}

// Where the plate's colour is flat the card shows what Storm shows, in World space, held in
// Geometry space and from a plate with overscan.
TEST_F(USDRenderMoonRayProjectionTest, Project3DMatchesStormsPlate)
{
    if (!moonRayOrSkip()) {
        return;
    }
    {
        SCOPED_TRACE("World");
        expectStormsWorldPlate(kStormMaxToleranceMoonRay);
    }
    {
        SCOPED_TRACE("GeometryHeld");
        expectStormsHeldPlate(kStormMaxToleranceMoonRay);
    }
    {
        SCOPED_TRACE("Overscan");
        expectStormsOverscanPlate(kStormMaxToleranceMoonRay);
    }
}

// The plate's alpha is the card's opacity over what is behind it, its colour straight.
TEST_F(USDRenderMoonRayProjectionTest, AHalfAlphaPlateBlendsOverTheBackground)
{
    if (!moonRayOrSkip()) {
        return;
    }
    expectAHalfAlphaPlateOverTheBackground();
}
