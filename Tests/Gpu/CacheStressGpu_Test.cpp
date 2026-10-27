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

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QThread>

#include "BaseTest.h"
#include "CacheStress.h"
#include "Native/OcioNodeTestSupport.h"
#include "NativeParity.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/FrameRenderContext.h"
#include "Engine/GpuContext.h"
#include "Engine/GpuPlacement.h"
#include "Engine/GpuRegionTask.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/ColorLookup.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Filter/IBlur.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/OCIO/OCIOFileTransform.h"
#include "Engine/Nodes/OCIO/OcioFileKnobs.h"
#include "Engine/Nodes/Transform/STMap.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RenderScheduler.h"
#include "Engine/RenderStats.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

using namespace CacheStress;

const RectI kWindow(0, 0, kParitySourceWidth, kParitySourceHeight);
const std::vector<std::uint32_t> kSeeds = { 1u, 2u };
const int kSteps = 10;
const qint64 kFrameTimeoutMs = 120000;

// GPU kernels are not bit-exact with the CPU; a stale image differs by far more than this.
ParityTolerance
gpuTolerance()
{
    return ParityTolerance::make(1e-4, 1e-4);
}

bool
fail(std::string* error,
     const char* what)
{
    *error = what;

    return false;
}

void
setFormat(const AppInstancePtr& app)
{
    app->getProject()->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "cacheStressGpu", 1.));
}

int
paritySource(const AppInstancePtr& app,
             StressGraph* g,
             int x,
             int y)
{
    const NodePtr source = createIn(app, app->getProject(), kTestPluginIDParitySource);

    if (source) {
        setParitySourceOrigin(source, x, y);
    }

    return addNode(g, source);
}

bool
connect(const StressGraph& g,
        int from,
        int to,
        int input)
{
    return g.nodes[to]->connectInput(g.nodes[from], input);
}

// GPU tasks run per node script name over every GPU render of a case, so a case can tell that
// the device took part at all.
typedef std::map<std::string, int> GpuTaskCounts;

std::vector<float>
readWindow(const ImagePtr& image,
           const RectI& window,
           int nComps)
{
    std::vector<float> out((std::size_t)window.area() * nComps, 0.f);
    Image::ReadAccess access = image->getReadRights();

    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            const float* pixel = (const float*)access.pixelAt(x, y);
            if (!pixel) {
                continue;
            }
            float* dst = &out[((std::size_t)(y - window.y1) * window.width() + (x - window.x1)) * nComps];
            for (int c = 0; c < nComps; ++c) {
                dst[c] = pixel[c];
            }
        }
    }

    return out;
}

// One frame through the render scheduler, every task the effects accept placed on the GPU as a
// render places it but without the cost model, through the caches as they are.
bool
renderOnGpu(const NodePtr& root,
            double time,
            unsigned mipmapLevel,
            const RectI& window,
            GpuTaskCounts* counts,
            RenderedPlane* out,
            std::string* error)
{
    if (mipmapLevel != 0) {
        return fail(error, "the GPU runner renders at full scale only");
    }
    const AppInstancePtr app = root->getApp();
    const AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(true, 0);
    const RenderStatsPtr stats = std::make_shared<RenderStats>(false);
    FrameRenderContextPtr context;
    FrameGraph graph;
    {
        ParallelRenderArgsSetter frameArgs(time, ViewIdx(0), false, false, abortInfo, root, 0, app->getTimeLine().get(), NodePtr(), false, false, stats);
        std::shared_ptr<FrameRequestMap> request = std::make_shared<FrameRequestMap>();
        const RectD rod(window.x1, window.y1, window.x2, window.y2);
        if (EffectInstance::computeRequestPass(time, ViewIdx(0), 0, rod, root, *request) == eStatusFailed) {
            return fail(error, "the request pass failed");
        }
        frameArgs.updateNodesRequest(*request);
        context = FrameRenderContext::create(time, ViewIdx(0), false, false, abortInfo, root, 0, app->getTimeLine().get(), NodePtr(), false, false, stats);
        context->setRequest(request);
        graph = RenderScheduler::buildGraph(context, root, time, ViewIdx(0), 0);
        if (graph.tasks.empty()) {
            return fail(error, "the frame graph is empty");
        }
        // The render's own inputs, so cached tasks stay on the CPU, with every region accepted.
        PlacementInputs inputs = makeRenderPlacementInputs(context);
        inputs.isPriced = std::function<bool(const FrameGraph::Task&)>();
        inputs.regionGain = std::function<double(const FrameGraph&, const FrameGraph::Region&)>();
        placeGraph(graph, inputs);
    }

    RenderScheduler* scheduler = appPTR->getRenderScheduler();
    FrameFuturePtr future = scheduler->submit(context, std::move(graph), RenderScheduler::Priority::Interactive);
    QElapsedTimer timer;
    timer.start();
    while (!future->isFinished()) {
        if (timer.elapsed() > kFrameTimeoutMs) {
            scheduler->abort(context);
            future->wait();

            return fail(error, "the GPU frame timed out");
        }
        QThread::msleep(1);
    }
    if (future->wait() != EffectInstance::eRenderRoIRetCodeOk) {
        return fail(error, "the GPU frame failed");
    }
    const std::map<ImageLayerDesc, ImagePtr> planes = future->getRootPlanes();
    std::map<ImageLayerDesc, ImagePtr>::const_iterator it = planes.find(ImageLayerDesc::getRGBAComponents());
    if (it == planes.end()) {
        it = planes.begin();
    }
    if ((it == planes.end()) || !it->second || (it->second->getBitDepth() != eImageBitDepthFloat)) {
        return fail(error, "the GPU frame has no float root plane");
    }
    out->layer = it->first;
    out->window = window;
    out->channels = it->second->getComponents().getChannels();
    out->pixels = readWindow(it->second, window, (int)out->channels.size());
    if (counts) {
        const GpuTaskCounts ran = stats->getGpuTasksPerNode();
        for (GpuTaskCounts::const_iterator r = ran.begin(); r != ran.end(); ++r) {
            (*counts)[r->first] += r->second;
        }
    }

    return true;
} // renderOnGpu

// "" when both planes have the same window, layer and channels, and every sample is within
// tolerance.
std::string
compareWithin(const RenderedPlane& reference,
              const RenderedPlane& got)
{
    const ParityTolerance tolerance = gpuTolerance();
    const std::size_t refComps = reference.channels.size();
    const std::size_t gotComps = got.channels.size();
    const std::size_t pixels = (std::size_t)reference.window.area();

    if ((reference.window.x1 != got.window.x1) || (reference.window.y1 != got.window.y1) || (reference.window.x2 != got.window.x2) || (reference.window.y2 != got.window.y2)) {
        return "windows differ";
    }
    if (!(reference.layer == got.layer)) {
        return "layers differ (" + reference.layer.getLayerID() + " vs " + got.layer.getLayerID() + ")";
    }
    if ((refComps == 0) || (refComps != gotComps) || (reference.pixels.size() != pixels * refComps) || (got.pixels.size() != pixels * gotComps)) {
        return "plane sizes differ (" + std::to_string(refComps) + " vs " + std::to_string(gotComps) + " channels)";
    }
    std::vector<std::size_t> matching(refComps, gotComps);
    for (std::size_t c = 0; c < refComps; ++c) {
        for (std::size_t d = 0; d < gotComps; ++d) {
            if (got.channels[d] == reference.channels[c]) {
                matching[c] = d;
                break;
            }
        }
        if (matching[c] == gotComps) {
            return "channel " + reference.channels[c] + " is missing from the GPU plane";
        }
    }
    std::size_t differing = 0;
    std::ostringstream first;
    for (std::size_t c = 0; c < refComps; ++c) {
        const std::size_t d = matching[c];
        for (std::size_t p = 0; p < pixels; ++p) {
            const float r = reference.pixels[p * refComps + c];
            const float g = got.pixels[p * gotComps + d];
            if (!tolerance.accepts(r, g)) {
                if (differing == 0) {
                    first << "pixel (" << (reference.window.x1 + (int)(p % reference.window.width())) << ","
                          << (reference.window.y1 + (int)(p / reference.window.width())) << ") " << reference.channels[c]
                          << ": CPU reference " << r << ", cached GPU " << g;
                }
                ++differing;
            }
        }
    }
    if (differing == 0) {
        return std::string();
    }
    std::ostringstream os;
    os << differing << " samples outside the tolerance; first at " << first.str();

    return os.str();
}

// A 1D cube LUT whose curve is x^power on every channel.
bool
writePowerLut(const std::string& path,
              double power)
{
    const int size = 1024;
    std::ofstream out(path.c_str());

    out << "LUT_1D_SIZE " << size << "\n";
    for (int i = 0; i < size; ++i) {
        const double x = (double)i / (size - 1);
        const double y = std::pow(x, power);
        out << y << " " << y << " " << y << "\n";
    }
    out.close();

    return bool(out);
}

// ParitySource -> Grade -> ColorLookup -> Blur, every node but the source GPU-capable.
bool
buildGpuChain(const AppInstancePtr& app,
              StressGraph* g,
              std::string* error)
{
    setFormat(app);
    const NodeCollectionPtr root = app->getProject();
    const int source = paritySource(app, g, 0, 0);
    const int other = addNode(g, createConstantIn(app, root, kWindow, { 0.3, 0.6, 0.9, 1. }));
    const int grade = addNode(g, createIn(app, root, PLUGINID_NATRON_GRADE, PLUGIN_MAJOR_NATRON_GRADE));
    const int lookup = addNode(g, createIn(app, root, PLUGINID_NATRON_COLORLOOKUP, PLUGIN_MAJOR_NATRON_COLORLOOKUP));
    const int blur = addNode(g, createIn(app, root, PLUGINID_NATRON_BLUR, PLUGIN_MAJOR_NATRON_BLUR));

    if ((source < 0) || (other < 0) || (grade < 0) || (lookup < 0) || (blur < 0)) {
        return fail(error, "could not create the nodes");
    }
    if (!connect(*g, source, grade, 0) || !connect(*g, grade, lookup, 0) || !connect(*g, lookup, blur, 0)) {
        return fail(error, "could not connect the chain");
    }
    addSlot(g, grade, 0, { source, other }, false);
    addSlot(g, blur, 0, { lookup, grade }, false);
    const int white = addKnob(g, grade, kGradeParamWhite, 0, { 1., 0.5, 2. });
    const int multiply = addKnob(g, grade, kGradeParamMultiply, 1, { 1., 0.8, 1.25 });
    addKnob(g, blur, kBlurParamSize, 0, { 0., 1., 3. }, false);
    addKnob(g, other, kConstantParamColor, 0, { 0.3, 0.7 });
    g->exprTargets = { white, multiply };
    g->links.push_back(std::make_pair(white, multiply));
    g->toggles = { grade, lookup, blur };
    g->probe = blur;
    g->window = kWindow;

    return true;
}

// ParitySource -> Grade (GPU) -> STMap (CPU) -> IBlur (GPU) with a Map, so images cross
// between the devices both ways.
bool
buildMixedDevices(const AppInstancePtr& app,
                  StressGraph* g,
                  std::string* error)
{
    setFormat(app);
    const NodeCollectionPtr root = app->getProject();
    const int source = paritySource(app, g, 0, 0);
    const int uv = paritySource(app, g, 4, 4);
    const int map = paritySource(app, g, 12, 0);
    const int flat = addNode(g, createConstantIn(app, root, kWindow, { 0.25, 0.75, 0., 1. }));
    const int grade = addNode(g, createIn(app, root, PLUGINID_NATRON_GRADE, PLUGIN_MAJOR_NATRON_GRADE));
    const int stmap = addNode(g, createIn(app, root, PLUGINID_NATRON_STMAP, PLUGIN_MAJOR_NATRON_STMAP));
    const int iblur = addNode(g, createIn(app, root, PLUGINID_NATRON_IBLUR, PLUGIN_MAJOR_NATRON_IBLUR));

    if ((source < 0) || (uv < 0) || (map < 0) || (flat < 0) || (grade < 0) || (stmap < 0) || (iblur < 0)) {
        return fail(error, "could not create the nodes");
    }
    if (!connect(*g, source, grade, 0) || !connect(*g, grade, stmap, kSTMapInputSource) || !connect(*g, uv, stmap, kSTMapInputUV) || !connect(*g, stmap, iblur, kIBlurInputSource) || !connect(*g, map, iblur, kIBlurInputMap)) {
        return fail(error, "could not connect the graph");
    }
    KnobChannelSelectPtr u = std::dynamic_pointer_cast<KnobChannelSelect>(g->nodes[stmap]->getKnobByName(kSTMapParamChannelU));
    KnobChannelSelectPtr v = std::dynamic_pointer_cast<KnobChannelSelect>(g->nodes[stmap]->getKnobByName(kSTMapParamChannelV));
    if (!u || !v) {
        return fail(error, "could not pick the UV channels");
    }
    u->set("rgba.R");
    v->set("rgba.G");
    addSlot(g, stmap, kSTMapInputUV, { uv, flat }, false);
    addSlot(g, iblur, kIBlurInputMap, { map, flat, uv });
    addKnob(g, grade, kGradeParamWhite, 0, { 1., 0.5, 2. });
    const int scale = addKnob(g, stmap, kSTMapParamUVScale, 0, { 1., 0.5, 1.5 });
    addKnob(g, flat, kConstantParamColor, 0, { 0.25, 0.6 });
    g->exprTargets = { scale };
    g->toggles = { grade, stmap, iblur };
    g->probe = iblur;
    g->window = kWindow;

    return true;
}

Options
gpuOptions(GpuTaskCounts* counts)
{
    Options options;

    options.ops = defaultOps();
    options.steps = stepBudget(kSteps);
    options.taskGraph = false;
    options.cachedRender = [counts](const NodePtr& node, double time, unsigned mipmapLevel, const RectI& window, RenderedPlane* out, std::string* error) {
        return renderOnGpu(node, time, mipmapLevel, window, counts, out, error);
    };
    options.compare = compareWithin;
    options.cachedRenderReference = true;
    options.cachedRenderName = "GPU";

    return options;
}

typedef std::function<bool(const StressGraph& g, std::string* error)> Edit;

// Applies the edits in order to a fresh graph twice: once rendering each state from empty
// caches on the CPU and on the GPU, once on the GPU through caches nothing clears. The cached GPU
// frame must equal the cache-off one exactly and the CPU one within tolerance. Returns the
// failure, or "".
std::string
runScripted(const AppInstancePtr& app,
            const GraphBuilder& builder,
            const std::vector<std::pair<std::string, Edit>>& edits,
            GpuTaskCounts* counts)
{
    std::vector<RenderedPlane> references;
    std::vector<RenderedPlane> gpuReferences;
    for (int pass = 0; pass < 2; ++pass) {
        app->getProject()->reset(false, true);
        clearEverything(app);
        StressGraph g;
        std::string error;
        if (!builder(app, &g, &error)) {
            return "the graph builder failed: " + error;
        }
        app->getTimeLine()->seekFrame(1, false, NULL, eTimelineChangeReasonOtherSeek);
        for (std::size_t i = 0; i <= edits.size(); ++i) {
            const std::string what = (i == 0) ? std::string("(initial state)") : edits[i - 1].first;
            if ((i > 0) && !edits[i - 1].second(g, &error)) {
                return "step " + std::to_string(i) + " (" + what + ") could not be applied: " + error;
            }
            const NodePtr probe = g.nodes[g.probe];
            if (pass == 0) {
                clearEverything(app);
                RenderedPlane reference;
                if (!renderProbe(probe, 1., 0, g.window, eRenderSchedulerModeLegacy, true, &reference, &error)) {
                    return "the CPU reference of step " + std::to_string(i) + " (" + what + ") failed: " + error;
                }
                references.push_back(reference);
                clearEverything(app);
                RenderedPlane gpuReference;
                if (!renderOnGpu(probe, 1., 0, g.window, counts, &gpuReference, &error)) {
                    return "the cache-off GPU render of step " + std::to_string(i) + " (" + what + ") failed: " + error;
                }
                gpuReferences.push_back(gpuReference);
                continue;
            }
            RenderedPlane got;
            if (!renderOnGpu(probe, 1., 0, g.window, counts, &got, &error)) {
                return "the GPU render of step " + std::to_string(i) + " (" + what + ") failed: " + error;
            }
            std::string diff = compareExact(gpuReferences[i], got);
            if (!diff.empty()) {
                diff = "against the cache-off GPU frame: " + diff;
            } else {
                diff = compareWithin(references[i], got);
                if (!diff.empty()) {
                    diff = "against the CPU reference: " + diff;
                }
            }
            if (!diff.empty()) {
                std::ostringstream os;
                os << "cache stress GPU mismatch at step " << i << " (" << what << "): " << diff << "\nedits:";
                for (std::size_t k = 0; k < i; ++k) {
                    os << "\n  " << (k + 1) << ": " << edits[k].first;
                }

                return os.str();
            }
        }
    }

    return std::string();
}

bool
setCurvePoint(const StressGraph& g,
              int lookup,
              int nth,
              double key,
              double value,
              std::string* error)
{
    KnobParametricPtr table = std::dynamic_pointer_cast<KnobParametric>(g.nodes[lookup]->getKnobByName(kColorLookupParamLookupTable));

    if (!table || (table->setNthControlPoint(eValueChangedReasonUserEdited, ColorLookupOp::eCurveMaster, nth, key, value) != eStatusOK)) {
        return fail(error, "could not move the control point");
    }

    return true;
}

} // namespace

class CacheStressGpuTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        OcioNodeTest::setConfig(getApp(), OcioNodeTest::kStudioURI);
        appPTR->clearAllCaches();

        _scheduler = appPTR->getRenderScheduler();
        _savedBody = _scheduler->getRegionBody();
        _scheduler->setRegionBody(&runGpuRegion);

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
        _previousContext = appPTR->swapGpuContextForTests(std::move(forced));
        _installed = true;
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        appPTR->clearAllCaches();
        if (_installed) {
            std::unique_ptr<GpuContext> forced = appPTR->swapGpuContextForTests(std::move(_previousContext));
            forced.reset();
            _installed = false;
        }
        _scheduler->setRegionBody(_savedBody);
        BaseTest::TearDown();
    }

    // The bundled gtest has no GTEST_SKIP, so each test returns early on false.
    bool gpuOrSkip()
    {
        if (_installed) {
            return true;
        }
        const char* require = std::getenv("NATRON_TEST_REQUIRE_GPU");
        if (require && (std::string(require) == "1")) {
            ADD_FAILURE() << "NATRON_TEST_REQUIRE_GPU=1 but no GPU: " << _skipReason;
        } else {
            std::printf("[  SKIPPED ] no GPU: %s\n", _skipReason.c_str());
        }

        return false;
    }

    RenderScheduler* _scheduler = nullptr;
    RenderScheduler::RegionBody _savedBody;
    std::unique_ptr<GpuContext> _previousContext;
    bool _installed = false;
    std::string _skipReason;
};

TEST_F(CacheStressGpuTest, SeededEditsOnAGpuChain)
{
    if (!gpuOrSkip()) {
        return;
    }
    GpuTaskCounts counts;
    EXPECT_EQ(0, expectClean(getApp(), buildGpuChain, kSeeds, gpuOptions(&counts)));
    EXPECT_FALSE(counts.empty()) << "no task ran on the GPU, so nothing was tested";
}

TEST_F(CacheStressGpuTest, SeededEditsAcrossDevices)
{
    if (!gpuOrSkip()) {
        return;
    }
    GpuTaskCounts counts;
    EXPECT_EQ(0, expectClean(getApp(), buildMixedDevices, kSeeds, gpuOptions(&counts)));
    EXPECT_FALSE(counts.empty()) << "no task ran on the GPU, so nothing was tested";
}

// The ColorLookup curve reaches the kernel as packed words, not through a knob value: moving a
// control point and moving it back must each reach the cached GPU frame.
TEST_F(CacheStressGpuTest, CurveEditsReachTheGpuFrame)
{
    if (!gpuOrSkip()) {
        return;
    }
    const int lookup = 3;
    std::vector<std::pair<std::string, Edit>> edits;
    edits.push_back(std::make_pair(std::string("master curve end to 0.6"), Edit([lookup](const StressGraph& g, std::string* error) {
                                       return setCurvePoint(g, lookup, 1, 1., 0.6, error);
                                   })));
    edits.push_back(std::make_pair(std::string("master curve start to 0.2"), Edit([lookup](const StressGraph& g, std::string* error) {
                                       return setCurvePoint(g, lookup, 0, 0., 0.2, error);
                                   })));
    edits.push_back(std::make_pair(std::string("master curve end back to 1"), Edit([lookup](const StressGraph& g, std::string* error) {
                                       return setCurvePoint(g, lookup, 1, 1., 1., error);
                                   })));
    edits.push_back(std::make_pair(std::string("master curve start back to 0"), Edit([lookup](const StressGraph& g, std::string* error) {
                                       return setCurvePoint(g, lookup, 0, 0., 0., error);
                                   })));
    GpuTaskCounts counts;
    EXPECT_EQ(std::string(), runScripted(getApp(), buildGpuChain, edits, &counts));
    EXPECT_FALSE(counts.empty()) << "no task ran on the GPU, so nothing was tested";
}

// An OCIO file transform hands its LUT to the GPU as a texture the GpuTextureCache keeps by
// content: swapping the file and back must neither reuse the old texture nor the old frame.
TEST_F(CacheStressGpuTest, LutSwapsReachTheGpuFrame)
{
    if (!gpuOrSkip()) {
        return;
    }
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::string square = tmp.path().toStdString() + "/square.cube";
    const std::string root = tmp.path().toStdString() + "/root.cube";
    ASSERT_TRUE(writePowerLut(square, 2.));
    ASSERT_TRUE(writePowerLut(root, 0.5));

    const GraphBuilder builder = [square](const AppInstancePtr& app, StressGraph* g, std::string* error) {
        setFormat(app);
        const int source = paritySource(app, g, 0, 0);
        const int file = addNode(g, createIn(app, app->getProject(), PLUGINID_NATRON_OCIOFILETRANSFORM, PLUGIN_MAJOR_NATRON_OCIOFILETRANSFORM));
        const int grade = addNode(g, createIn(app, app->getProject(), PLUGINID_NATRON_GRADE, PLUGIN_MAJOR_NATRON_GRADE));
        if ((source < 0) || (file < 0) || (grade < 0)) {
            return fail(error, "could not create the nodes");
        }
        if (!connect(*g, source, file, 0) || !connect(*g, file, grade, 0)) {
            return fail(error, "could not connect the chain");
        }
        if (!setKnobValue(g->nodes[file], kOcioFileParamFile, square)) {
            return fail(error, "could not set the LUT file");
        }
        g->probe = grade;
        g->window = kWindow;

        return true;
    };
    const int file = 1;
    const int grade = 2;
    std::vector<std::pair<std::string, Edit>> edits;
    edits.push_back(std::make_pair(std::string("file to root.cube"), Edit([file, root](const StressGraph& g, std::string* error) {
                                       return setKnobValue(g.nodes[file], kOcioFileParamFile, root) || fail(error, "could not set the LUT file");
                                   })));
    edits.push_back(std::make_pair(std::string("grade white to 0.5"), Edit([grade](const StressGraph& g, std::string* error) {
                                       return setKnobValues(g.nodes[grade], kGradeParamWhite, std::vector<double>(1, 0.5)) || fail(error, "could not set white");
                                   })));
    edits.push_back(std::make_pair(std::string("file back to square.cube"), Edit([file, square](const StressGraph& g, std::string* error) {
                                       return setKnobValue(g.nodes[file], kOcioFileParamFile, square) || fail(error, "could not set the LUT file");
                                   })));
    edits.push_back(std::make_pair(std::string("grade white back to 1"), Edit([grade](const StressGraph& g, std::string* error) {
                                       return setKnobValues(g.nodes[grade], kGradeParamWhite, std::vector<double>(1, 1.)) || fail(error, "could not set white");
                                   })));
    edits.push_back(std::make_pair(std::string("file to root.cube again"), Edit([file, root](const StressGraph& g, std::string* error) {
                                       return setKnobValue(g.nodes[file], kOcioFileParamFile, root) || fail(error, "could not set the LUT file");
                                   })));
    GpuTaskCounts counts;
    EXPECT_EQ(std::string(), runScripted(getApp(), builder, edits, &counts));
    EXPECT_FALSE(counts.empty()) << "no task ran on the GPU, so nothing was tested";
}
