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

#include "Global/Macros.h"

#ifdef NATRON_HAVE_GPU

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

#include <QByteArray>
#include <QElapsedTimer>
#include <QFile>
#include <QString>
#include <QTemporaryDir>
#include <QThread>

#include <ofxImageEffect.h>

#include "BaseTest.h"
#include "NativeParity.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/FrameRenderContext.h"
#include "Engine/GpuContext.h"
#include "Engine/GpuCostModel.h"
#include "Engine/GpuPlacement.h"
#include "Engine/GpuRegionStrips.h"
#include "Engine/GpuRegionTask.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobFile.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/ColorCorrect.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/IO/NativeRead.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/RenderScheduler.h"
#include "Engine/RenderStats.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

#include "Engine/Gpu/GpuRuntime.h"

NATRON_NAMESPACE_USING

namespace {

const int kWidth = 256;
const int kHeight = 768;
const double kTime = 1.;
const qint64 kFrameTimeoutMs = 60000;
const char* const kStripsReason = "viewer handoff: strips";

struct FrameRun {
    AbortableRenderInfoPtr abortInfo;
    RenderStatsPtr stats;
    FrameRenderContextPtr context;
    FrameGraph graph;
    FrameFuturePtr future;
};

int
taskOf(const FrameGraph& graph,
       const NodePtr& node)
{
    for (std::size_t i = 0; i < graph.tasks.size(); ++i) {
        if (graph.tasks[i].key.node == node) {
            return (int)i;
        }
    }

    return -1;
}

std::vector<float>
readRgba(const ImagePtr& image,
         const RectI& window)
{
    std::vector<float> out((std::size_t)window.area() * 4, 0.f);

    if (!image || (image->getBitDepth() != eImageBitDepthFloat)) {
        return out;
    }
    const int nComps = (int)image->getComponentsCount();
    Image::ReadAccess access = image->getReadRights();
    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            const float* pixel = (const float*)access.pixelAt(x, y);
            if (!pixel) {
                continue;
            }
            float* dst = &out[((std::size_t)(y - window.y1) * window.width() + (x - window.x1)) * 4];
            for (int c = 0; c < std::min(nComps, 4); ++c) {
                dst[c] = pixel[c];
            }
        }
    }

    return out;
}

float
worstDiff(const std::vector<float>& a,
          const std::vector<float>& b)
{
    float worst = 0.f;
    for (std::size_t i = 0; (i < a.size()) && (i < b.size()); ++i) {
        worst = std::max(worst, std::fabs(a[i] - b[i]));
    }

    return worst;
}

bool
anyNonZero(const std::vector<float>& pixels)
{
    return std::any_of(pixels.begin(), pixels.end(), [](float v) { return v != 0.f; });
}

std::string
writeSource(const QTemporaryDir& dir)
{
    const std::string path = (dir.path() + QString::fromUtf8("/source.exr")).toStdString();
    OIIO::ImageSpec spec(kWidth, kHeight, 4, OIIO::TypeDesc::FLOAT);
    std::vector<float> pixels((std::size_t)kWidth * kHeight * 4);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            float* p = &pixels[((std::size_t)y * kWidth + x) * 4];
            p[0] = (float)x / kWidth;
            p[1] = (float)y / kHeight;
            p[2] = (float)((x * 7 + y * 13) % 97) / 96.f;
            p[3] = 0.25f + 0.75f * (float)((x + y) % 5) / 4.f;
        }
    }
    OIIO::ImageOutput::unique_ptr out = OIIO::ImageOutput::create(path);
    if (!out || !out->open(path, spec) || !out->write_image(OIIO::TypeDesc::FLOAT, pixels.data()) || !out->close()) {
        return std::string();
    }

    return path;
}

std::vector<std::string>
readProfileRecords(const std::string& path)
{
    std::vector<std::string> records;
    QFile file(QString::fromStdString(path));
    if (!file.open(QIODevice::ReadOnly)) {
        return records;
    }
    for (const QByteArray& line : file.readAll().split('\n')) {
        if (!line.isEmpty()) {
            records.push_back(line.toStdString());
        }
    }

    return records;
}

double
numberOf(const std::string& record,
         const std::string& key)
{
    const std::size_t at = record.find("\"" + key + "\":");

    return (at == std::string::npos) ? -1. : std::atof(record.c_str() + at + key.size() + 3);
}

int
legacyFallbacksOf(const FrameRun& run,
                  const std::string& reason)
{
    const std::map<std::string, int> reasons = run.stats->getLegacyFallbackReasons();
    const std::map<std::string, int>::const_iterator found = reasons.find(reason);

    return (found == reasons.end()) ? 0 : found->second;
}

int
cpuFallbacksOf(const FrameRun& run)
{
    int total = 0;
    for (const std::pair<const std::string, int>& reason : run.stats->getCpuFallbackReasons()) {
        total += reason.second;
    }

    return total;
}

} // namespace

/**
 * @brief A CPU Read feeding a Grade, a Blur and a ColorCorrect, which the frames place on the GPU as one region whose
 * output is the frame's root.
 **/
class ViewerRootOnDevice
    : public BaseTest {
protected:
    ViewerRootOnDevice()
        : BaseTest()
        , _scheduler(0)
        , _savedBody()
        , _swappedContext()
        , _swapped(false)
        , _gpuReady(false)
        , _skipReason()
        , _last(std::make_shared<RenderScheduler::RegionReport>())
    {
    }

    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        ProjectPtr project = getApp()->getProject();
        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(false);
        project->setOrAddProjectFormat(Format(0, 0, kWidth, kHeight, "viewerRootOnDevice", 1.));
        appPTR->clearAllCaches();

        _scheduler = appPTR->getRenderScheduler();
        _savedBody = _scheduler->getRegionBody();
        std::shared_ptr<RenderScheduler::RegionReport> last = _last;
        _scheduler->setRegionBody([last](const FrameRenderContextPtr& context,
                                         const FrameGraph& graph,
                                         int region,
                                         const RenderScheduler::CpuFallbackLease& lease,
                                         std::map<ImageLayerDesc, ImagePtr>* rootPlanes,
                                         RenderScheduler::RegionReport* report) {
            const EffectInstance::RenderRoIRetCode code = runGpuRegion(context, graph, region, lease, rootPlanes, report);
            if (report) {
                *last = *report;
            }

            return code;
        });

        GpuContext* app = appPTR->getGpuContext();
        if (app) {
            app->waitForInit(std::chrono::seconds(60));
        }
        if (app && app->isAvailable()) {
            _gpuReady = true;

            return;
        }
        GpuContextOptions options;
        options.mode = GpuResolvedMode::Force;
        std::shared_ptr<std::string> unavailable = std::make_shared<std::string>();
        options.infoLog = [unavailable](const std::string& line) {
            *unavailable = line;
        };
        std::unique_ptr<GpuContext> forced(new GpuContext(options));
        if (!forced->waitForInit(std::chrono::seconds(60)) || !forced->isAvailable()) {
            _skipReason = unavailable->empty() ? std::string("no Vulkan device") : *unavailable;

            return;
        }
        _swappedContext = appPTR->swapGpuContextForTests(std::move(forced));
        _swapped = true;
        _gpuReady = true;
    }

    virtual void TearDown() OVERRIDE
    {
        setGpuStripOverridesForTests(0, 0);
        setGpuStripPipeliningForTests(GpuStripPipelining::Auto);
        _scheduler->setRegionBody(_savedBody);
        appPTR->clearAllCaches();
        getApp()->getProject()->reset(false, true);
        if (_swapped) {
            std::unique_ptr<GpuContext> forced = appPTR->swapGpuContextForTests(std::move(_swappedContext));
            forced.reset();
            _swapped = false;
        }
        BaseTest::TearDown();
    }

    // The bundled gtest has no GTEST_SKIP, so each test returns early on false.
    bool gpuOrSkip()
    {
        if (_gpuReady) {
            return true;
        }
        const char* require = std::getenv("NATRON_TEST_REQUIRE_GPU");
        if (require && (std::strcmp(require, "1") == 0)) {
            ADD_FAILURE() << "NATRON_TEST_REQUIRE_GPU=1 but " << _skipReason;
        } else {
            std::printf("[  SKIPPED ] %s\n", _skipReason.c_str());
        }

        return false;
    }

    NodePtr makeNode(const char* pluginID,
                     int major)
    {
        NodePtr node = createNode(QString::fromUtf8(pluginID), major);
        EXPECT_TRUE(bool(node)) << pluginID;

        return node;
    }

    bool makeChain()
    {
        if (!_sourceDir.isValid()) {
            return false;
        }
        const std::string path = writeSource(_sourceDir);
        if (path.empty()) {
            return false;
        }
        _read = makeNode(PLUGINID_NATRON_READ, PLUGIN_MAJOR_NATRON_READ);
        _grade = makeNode(PLUGINID_NATRON_GRADE, PLUGIN_MAJOR_NATRON_GRADE);
        _blur = makeNode(PLUGINID_NATRON_BLUR, PLUGIN_MAJOR_NATRON_BLUR);
        _colorCorrect = makeNode(PLUGINID_NATRON_COLORCORRECT, PLUGIN_MAJOR_NATRON_COLORCORRECT);
        if (!_read || !_grade || !_blur || !_colorCorrect) {
            return false;
        }
        KnobFile* file = dynamic_cast<KnobFile*>(_read->getKnobByName(kOfxImageEffectFileParamName).get());
        if (!file) {
            return false;
        }
        file->setValue(path);
        _read->getEffectInstance()->refreshMetadata_public(false);
        const std::string master = kColorCorrectGroupMaster;
        if (!setKnobValues(_grade, "multiply", { 1.25, 1.25, 1.25, 1.25 }) || !setKnobValues(_grade, "offset", { 0.05, 0.05, 0.05, 0.05 }) || !setKnobValues(_blur, kBlurParamSize, { 9., 9. }) || !setKnobValues(_colorCorrect, master + kColorCorrectParamSaturation, { 0.8, 0.8, 0.8, 0.8 }) || !setKnobValues(_colorCorrect, master + kColorCorrectParamGain, { 1.1, 1.1, 1.1, 1.1 })) {
            return false;
        }
        connectNodes(_read, _grade, 0, true);
        connectNodes(_grade, _blur, 0, true);
        connectNodes(_blur, _colorCorrect, 0, true);

        return true;
    }

    bool prepareFrame(const NodePtr& root,
                      const RectI& renderWindow,
                      FrameRun* run,
                      std::string* error)
    {
        run->abortInfo = AbortableRenderInfo::create(true, 0);
        run->stats = std::make_shared<RenderStats>(false);

        ParallelRenderArgsSetter frameArgs(kTime,
                                           ViewIdx(0),
                                           false /*isRenderUserInteraction*/,
                                           false /*isSequential*/,
                                           run->abortInfo,
                                           root,
                                           0 /*textureIndex*/,
                                           getApp()->getTimeLine().get(),
                                           NodePtr(),
                                           false /*isAnalysis*/,
                                           false /*draftMode*/,
                                           run->stats);
        std::shared_ptr<FrameRequestMap> request = std::make_shared<FrameRequestMap>();
        const RectD window(renderWindow.x1, renderWindow.y1, renderWindow.x2, renderWindow.y2);
        if (EffectInstance::computeRequestPass(kTime, ViewIdx(0), 0 /*mipmapLevel*/, window, root, *request) == eStatusFailed) {
            *error = "request pass failed";

            return false;
        }
        frameArgs.updateNodesRequest(*request);

        run->context = FrameRenderContext::create(kTime,
                                                  ViewIdx(0),
                                                  false /*isRenderUserInteraction*/,
                                                  false /*isSequential*/,
                                                  run->abortInfo,
                                                  root,
                                                  0 /*textureIndex*/,
                                                  getApp()->getTimeLine().get(),
                                                  NodePtr(),
                                                  false /*isAnalysis*/,
                                                  false /*draftMode*/,
                                                  run->stats);
        run->context->setRequest(request);
        run->graph = RenderScheduler::buildGraph(run->context, root, kTime, ViewIdx(0), 0 /*mipmapLevel*/);
        if (run->graph.tasks.empty()) {
            *error = "empty graph";

            return false;
        }

        return true;
    }

    // Every task of gpuNodes whose effect can render it on the GPU, placed as placeGraph() does without a cost model.
    static PlacementInputs gpuOnly(const FrameRenderContextPtr& context,
                                   const std::vector<NodePtr>& gpuNodes)
    {
        std::set<const Node*> gpu;
        for (const NodePtr& node : gpuNodes) {
            gpu.insert(node.get());
        }
        PlacementInputs inputs;
        inputs.canRenderOnGpu = [gpu, context](const FrameGraph::Task& task) {
            if (!gpu.count(task.key.node.get())) {
                return false;
            }
            GpuQuery query;
            if (!makeGpuQueryForTask(task, *context, &query)) {
                return false;
            }
            const NativeEffectBase* native = dynamic_cast<const NativeEffectBase*>(task.key.node->getEffectInstance().get());

            return native && native->canRenderOnGpu(query);
        };

        return inputs;
    }

    // The chain's frame, its three GPU nodes placed as one region, asking for its root on the device or not.
    bool prepareChainFrame(bool wantsRootOnDevice,
                           FrameRun* run,
                           std::string* error)
    {
        if (!prepareFrame(_colorCorrect, RectI(0, 0, kWidth, kHeight), run, error)) {
            return false;
        }
        placeGraph(run->graph, gpuOnly(run->context, { _grade, _blur, _colorCorrect }));
        if ((run->graph.regions.size() != 1) || (run->graph.regions[0].members.size() != 3)) {
            *error = "the chain is not one region of three members";

            return false;
        }
        run->context->setWantsRootOnDevice(wantsRootOnDevice);

        return true;
    }

    bool render(FrameRun* run)
    {
        *_last = RenderScheduler::RegionReport();
        run->future = _scheduler->submit(run->context, std::move(run->graph), RenderScheduler::Priority::Interactive);
        QElapsedTimer timer;
        timer.start();
        while (!run->future->isFinished()) {
            if (timer.elapsed() > kFrameTimeoutMs) {
                _scheduler->abort(run->context);
                run->future->wait();

                return false;
            }
            QThread::msleep(1);
        }

        return run->future->wait() == EffectInstance::eRenderRoIRetCodeOk;
    }

    // The chain's RAM root, rendered on the GPU without asking for the root on the device.
    bool renderRamRoot(ImagePtr* root,
                       std::string* error)
    {
        FrameRun run;
        if (!prepareChainFrame(false, &run, error)) {
            return false;
        }
        if (!render(&run)) {
            *error = "the frame without the flag failed";

            return false;
        }
        const std::map<ImageLayerDesc, ImagePtr> planes = run.future->getRootPlanes();
        if (planes.empty() || !planes.begin()->second || !run.context->takeRootDevicePlanes().empty()) {
            *error = "the frame without the flag has no RAM root, or a device one";

            return false;
        }
        if ((run.stats->getGpuRegionsRun() != 1) || (run.stats->getDownloadCount() != 1)) {
            *error = "the frame without the flag did not run one region downloading its root";

            return false;
        }
        *root = planes.begin()->second;
        appPTR->clearAllCaches();

        return true;
    }

    RenderScheduler* _scheduler;
    RenderScheduler::RegionBody _savedBody;
    std::unique_ptr<GpuContext> _swappedContext;
    bool _swapped;
    bool _gpuReady;
    std::string _skipReason;
    std::shared_ptr<RenderScheduler::RegionReport> _last;
    QTemporaryDir _sourceDir;
    NodePtr _read;
    NodePtr _grade;
    NodePtr _blur;
    NodePtr _colorCorrect;
};

TEST_F(ViewerRootOnDevice, TheRootStaysOnTheDeviceAndMatchesTheRamRoot)
{
    if (!gpuOrSkip()) {
        return;
    }
    ASSERT_TRUE(makeChain());
    const RectI window(0, 0, kWidth, kHeight);
    ImagePtr ramRoot;
    std::string error;
    ASSERT_TRUE(renderRamRoot(&ramRoot, &error)) << error;
    const std::vector<float> want = readRgba(ramRoot, window);
    ASSERT_TRUE(anyNonZero(want)) << "the RAM root is black, which proves nothing";

    FrameRun run;
    ASSERT_TRUE(prepareChainFrame(true, &run, &error)) << error;
    QTemporaryDir profileDir;
    ASSERT_TRUE(profileDir.isValid());
    RenderScheduler::setProfilePath((profileDir.path() + QString::fromUtf8("/profile")).toStdString());
    const bool rendered = render(&run);
    RenderScheduler::flushProfile();
    const std::string profilePath = RenderScheduler::getProfileFilePath();
    RenderScheduler::setProfilePath(std::string());
    ASSERT_TRUE(rendered);

    const RenderScheduler::RegionReport report = *_last;
    EXPECT_EQ(0, cpuFallbacksOf(run));
    EXPECT_EQ(1, run.stats->getGpuRegionsRun());
    EXPECT_EQ(0, report.strips);
    EXPECT_GT(report.uploadBytes, 0u);
    EXPECT_EQ(0u, report.downloadBytes);
    EXPECT_EQ(0, run.stats->getDownloadCount());
    EXPECT_EQ(0, run.context->getStore().downloadCount());
    EXPECT_EQ(0, legacyFallbacksOf(run, kStripsReason));
    EXPECT_TRUE(run.future->getRootPlanes().empty());

    int regionRecords = 0;
    for (const std::string& record : readProfileRecords(profilePath)) {
        if (record.compare(0, 17, "{\"kind\":\"region\",") != 0) {
            continue;
        }
        ++regionRecords;
        EXPECT_EQ(0., numberOf(record, "downloadBytes")) << record;
        EXPECT_GT(numberOf(record, "uploadBytes"), 0.) << record;
    }
    EXPECT_EQ(1, regionRecords);

    std::map<ImageLayerDesc, GpuDeviceImagePtr> devicePlanes = run.context->takeRootDevicePlanes();
    ASSERT_EQ(1u, devicePlanes.size());
    const GpuDeviceImagePtr device = devicePlanes.begin()->second;
    ASSERT_TRUE(bool(device));
    EXPECT_EQ(ramRoot->getBounds(), device->bounds());
    const ImagePtr downloaded = device->downloadToRam();
    ASSERT_TRUE(bool(downloaded));
    EXPECT_EQ(0.f, worstDiff(want, readRgba(downloaded, window)));
}

TEST_F(ViewerRootOnDevice, AnEvictedRootKeepsItsPixels)
{
    if (!gpuOrSkip()) {
        return;
    }
    ASSERT_TRUE(makeChain());
    const RectI window(0, 0, kWidth, kHeight);
    ImagePtr ramRoot;
    std::string error;
    ASSERT_TRUE(renderRamRoot(&ramRoot, &error)) << error;

    FrameRun run;
    ASSERT_TRUE(prepareChainFrame(true, &run, &error)) << error;
    ASSERT_TRUE(render(&run));
    std::map<ImageLayerDesc, GpuDeviceImagePtr> devicePlanes = run.context->takeRootDevicePlanes();
    ASSERT_EQ(1u, devicePlanes.size());
    const GpuDeviceImagePtr device = devicePlanes.begin()->second;
    ASSERT_TRUE(bool(device));
    const std::size_t bytes = device->bytes();
    ASSERT_GT(bytes, 0u);

    gpu::GpuRuntime* runtime = appPTR->getGpuContext()->runtime();
    EXPECT_GE(runtime->pool().evict(std::numeric_limits<std::uint64_t>::max()), (std::uint64_t)bytes);
    EXPECT_EQ(0u, device->bytes()) << "the pool could not reclaim the root";
    EXPECT_EQ(nullptr, device->pin());
    EXPECT_EQ(1, run.stats->getEvictions());
    const ImagePtr copy = device->downloadToRam();
    ASSERT_TRUE(bool(copy));
    EXPECT_EQ(0.f, worstDiff(readRgba(ramRoot, window), readRgba(copy, window)));
}

TEST_F(ViewerRootOnDevice, ForcedStripsDownloadTheRootAndCountIt)
{
    if (!gpuOrSkip()) {
        return;
    }
    ASSERT_TRUE(makeChain());
    const RectI window(0, 0, kWidth, kHeight);
    ImagePtr ramRoot;
    std::string error;
    ASSERT_TRUE(renderRamRoot(&ramRoot, &error)) << error;

    // A 1 MB storage buffer range holds 256 rows of the 256-pixel RGBA float image, so the region strips.
    setGpuStripOverridesForTests(1u << 20, 64);
    FrameRun run;
    ASSERT_TRUE(prepareChainFrame(true, &run, &error)) << error;
    ASSERT_TRUE(render(&run));

    const RenderScheduler::RegionReport report = *_last;
    EXPECT_EQ(0, cpuFallbacksOf(run));
    EXPECT_EQ(1, run.stats->getGpuRegionsRun());
    EXPECT_GT(report.strips, 1);
    const std::size_t rootBytes = (std::size_t)ramRoot->getBounds().area() * 4 * sizeof(float);
    EXPECT_EQ(rootBytes, report.downloadBytes);
    EXPECT_GT(run.stats->getDownloadCount(), 0);
    EXPECT_EQ((unsigned long long)rootBytes, run.stats->getDownloadBytes());
    EXPECT_EQ(1, legacyFallbacksOf(run, kStripsReason));
    EXPECT_TRUE(run.context->takeRootDevicePlanes().empty());

    const std::map<ImageLayerDesc, ImagePtr> planes = run.future->getRootPlanes();
    ASSERT_FALSE(planes.empty());
    EXPECT_EQ(0.f, worstDiff(readRgba(ramRoot, window), readRgba(planes.begin()->second, window)));
}

// With the root's download charged, the transfer outweighs what the GPU saves and placement leaves the chain on the
// CPU; kept on the device, the same chain at the same size goes to the GPU whole.
TEST_F(ViewerRootOnDevice, PlacementAcceptsTheRegionOnceTheRootStaysOnTheDevice)
{
    if (!gpuOrSkip()) {
        return;
    }
    ASSERT_TRUE(makeChain());
    FrameRun run;
    std::string error;
    ASSERT_TRUE(prepareFrame(_colorCorrect, RectI(0, 0, kWidth, kHeight), &run, &error)) << error;
    const FrameGraph base = run.graph;
    const int gradeTask = taskOf(base, _grade);
    const int blurTask = taskOf(base, _blur);
    const int colorCorrectTask = taskOf(base, _colorCorrect);
    ASSERT_TRUE(gradeTask >= 0 && blurTask >= 0 && colorCorrectTask >= 0);

    GpuCostModel model;
    model.setThreadCount(1);
    for (const NodePtr& node : { _grade, _blur, _colorCorrect }) {
        model.setCpuSeed(node->getPluginID(), 1000., 1.);
        model.setGpuSeed(node->getPluginID(), 1.);
    }
    GpuTransferRates rates;
    rates.uploadBytesPerSecond = 1e15;
    rates.downloadBytesPerSecond = 1e9;
    model.setTransferRates(rates);

    const PlacementInputs gpu = gpuOnly(run.context, { _grade, _blur, _colorCorrect });
    FrameGraph whole = base;
    placeGraph(whole, gpu);
    ASSERT_EQ(1u, whole.regions.size());
    const double kept = model.regionGainMs(whole, whole.regions[0], true);
    const double downloaded = model.regionGainMs(whole, whole.regions[0], false);
    ASSERT_GT(kept, 0.);
    ASSERT_LT(downloaded, kept);
    // The root's download now costs twice what the region gains without it.
    rates.downloadBytesPerSecond *= (kept - downloaded) / (2. * kept);
    model.setTransferRates(rates);
    EXPECT_NEAR(kept, model.regionGainMs(whole, whole.regions[0], true), kept * 1e-9);
    EXPECT_NEAR(-kept, model.regionGainMs(whole, whole.regions[0], false), kept * 1e-6);

    for (int rootOnDevice = 0; rootOnDevice < 2; ++rootOnDevice) {
        SCOPED_TRACE(rootOnDevice ? "root kept on the device" : "root downloaded");
        PlacementInputs inputs = gpu;
        inputs.regionGain = [&model, rootOnDevice](const FrameGraph& g, const FrameGraph::Region& region) {
            return model.regionGainMs(g, region, rootOnDevice != 0);
        };
        FrameGraph placed = base;
        placeGraph(placed, inputs);
        const FrameGraph::Device expected = rootOnDevice ? FrameGraph::Device::Gpu : FrameGraph::Device::Cpu;
        EXPECT_EQ(expected, placed.tasks[gradeTask].device);
        EXPECT_EQ(expected, placed.tasks[blurTask].device);
        EXPECT_EQ(expected, placed.tasks[colorCorrectTask].device);
        EXPECT_EQ(rootOnDevice ? 1u : 0u, placed.regions.size());
    }
}

// The placement a render uses reads the frame's flag: the gain with the root kept on the device is the gain with it
// downloaded plus the root's download time.
TEST_F(ViewerRootOnDevice, RenderPlacementChargesTheRootOnlyWhenItIsDownloaded)
{
    if (!gpuOrSkip()) {
        return;
    }
    ASSERT_TRUE(makeChain());
    FrameRun run;
    std::string error;
    ASSERT_TRUE(prepareFrame(_colorCorrect, RectI(0, 0, kWidth, kHeight), &run, &error)) << error;
    FrameGraph whole = run.graph;
    placeGraph(whole, gpuOnly(run.context, { _grade, _blur, _colorCorrect }));
    ASSERT_EQ(1u, whole.regions.size());
    const int colorCorrectTask = taskOf(whole, _colorCorrect);
    ASSERT_TRUE(colorCorrectTask >= 0);

    GpuCostModel& session = GpuCostModel::session();
    const GpuTransferRates saved = session.transferRates();
    const PlacementInputs inputs = makeRenderPlacementInputs(run.context);
    ASSERT_TRUE(bool(inputs.regionGain));
    struct RatesBack {
        GpuCostModel& model;
        GpuTransferRates rates;
        ~RatesBack()
        {
            model.setTransferRates(rates);
        }
    } ratesBack { session, saved };
    GpuTransferRates rates;
    rates.uploadBytesPerSecond = 1e9;
    rates.downloadBytesPerSecond = 1e9;
    session.setTransferRates(rates);

    run.context->setWantsRootOnDevice(false);
    const double downloaded = inputs.regionGain(whole, whole.regions[0]);
    run.context->setWantsRootOnDevice(true);
    const double kept = inputs.regionGain(whole, whole.regions[0]);
    ASSERT_TRUE(std::isfinite(downloaded) && std::isfinite(kept));
    const double rootMs = session.transferMs(0., (double)whole.tasks[colorCorrectTask].estimatedBytes);
    ASSERT_GT(rootMs, 0.);
    EXPECT_NEAR(rootMs, kept - downloaded, rootMs * 1e-6);
}

// A lone Grade over a UHD frame, seeded heavy enough that its downloaded root would run as overlapped strips, stays
// whole once the root is kept on the device: stripping would only download it.
TEST_F(ViewerRootOnDevice, ARootKeptOnTheDeviceIsNeverStrippedToHideItsTransfer)
{
    if (!gpuOrSkip()) {
        return;
    }
    if (!appPTR->getGpuContext()->runtime()->device().info().pushDescriptor) {
        std::printf("[  SKIPPED ] the device has no push descriptors, so strips never run pipelined\n");

        return;
    }
    const int width = 3840;
    const int height = 2160;
    NodePtr source = makeNode(PLUGINID_NATRON_CONSTANT, PLUGIN_MAJOR_NATRON_CONSTANT);
    NodePtr grade = makeNode(PLUGINID_NATRON_GRADE, PLUGIN_MAJOR_NATRON_GRADE);
    ASSERT_TRUE(source && grade);
    ASSERT_TRUE(setKnobValue(source, kNativeGeneratorParamExtent, kNativeGeneratorExtentSize));
    ASSERT_TRUE(setKnobValues(source, kNativeGeneratorParamBottomLeft, { 0., 0. }));
    ASSERT_TRUE(setKnobValues(source, kNativeGeneratorParamSize, { (double)width, (double)height }));
    ASSERT_TRUE(setKnobValues(source, kConstantParamColor, { 0.25, 0.5, 0.75, 1. }));
    ASSERT_TRUE(setKnobValues(grade, "multiply", { 1.5, 1.5, 1.5, 1.5 }));
    connectNodes(source, grade, 0, true);
    const RectI window(0, 0, width, height);

    GpuCostModel& session = GpuCostModel::session();
    const GpuTransferRates savedRates = session.transferRates();
    const double savedKernel = session.gpuMs(GpuTaskTraits(grade->getPluginID()), 1.);
    struct CostBack {
        GpuCostModel& model;
        GpuTransferRates rates;
        std::string pluginId;
        double kernelMsPerMpx;
        ~CostBack()
        {
            model.setTransferRates(rates);
            if (std::isfinite(kernelMsPerMpx)) {
                model.setGpuSeed(pluginId, kernelMsPerMpx);
            }
        }
    } costBack { session, savedRates, grade->getPluginID(), savedKernel };
    GpuTransferRates rates;
    rates.uploadBytesPerSecond = 10e9;
    rates.downloadBytesPerSecond = 10e9;
    session.setTransferRates(rates);
    session.setGpuSeed(grade->getPluginID(), 40.);
    setGpuStripPipeliningForTests(GpuStripPipelining::On);

    std::vector<float> stripped;
    for (int rootOnDevice = 0; rootOnDevice < 2; ++rootOnDevice) {
        SCOPED_TRACE(rootOnDevice ? "root kept on the device" : "root downloaded");
        FrameRun run;
        std::string error;
        ASSERT_TRUE(prepareFrame(grade, window, &run, &error)) << error;
        placeGraph(run.graph, gpuOnly(run.context, { grade }));
        ASSERT_EQ(1u, run.graph.regions.size());
        run.context->setWantsRootOnDevice(rootOnDevice != 0);
        ASSERT_TRUE(render(&run));
        const RenderScheduler::RegionReport report = *_last;
        EXPECT_EQ(0, cpuFallbacksOf(run));
        if (!rootOnDevice) {
            EXPECT_GE(report.strips, kGpuStripMaxInFlight);
            EXPECT_TRUE(report.overlapStrips);
            const std::map<ImageLayerDesc, ImagePtr> planes = run.future->getRootPlanes();
            ASSERT_FALSE(planes.empty());
            stripped = readRgba(planes.begin()->second, window);
        } else {
            EXPECT_EQ(0, report.strips);
            EXPECT_FALSE(report.overlapStrips);
            EXPECT_EQ(0u, report.downloadBytes);
            EXPECT_EQ(0, run.stats->getDownloadCount());
            EXPECT_EQ(0, legacyFallbacksOf(run, kStripsReason));
            std::map<ImageLayerDesc, GpuDeviceImagePtr> devicePlanes = run.context->takeRootDevicePlanes();
            ASSERT_EQ(1u, devicePlanes.size());
            const ImagePtr downloaded = devicePlanes.begin()->second->downloadToRam();
            ASSERT_TRUE(bool(downloaded));
            EXPECT_EQ(0.f, worstDiff(stripped, readRgba(downloaded, window)));
        }
    }
}

#endif // NATRON_HAVE_GPU
