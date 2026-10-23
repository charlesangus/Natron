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

#include <cstdint>
#include <cstring>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "Tests/Native/NativeGpuParity.h"

#include "Engine/GpuPlacement.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Raw/RawGpuParams.h"
#include "Engine/RectI.h"
#include "Engine/RenderScheduler.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuBuffer.h"
#include "Engine/Gpu/GpuRecord.h"
#include "Engine/Gpu/GpuRuntime.h"
#include "Engine/Gpu/GpuWrap.h"

#include "raw_test_layout_probe_spirv.h"
#endif

NATRON_NAMESPACE_USING

namespace {

// Tasks 0 to nTasks - 1, with an edge from each (consumer, dependency) pair of edges.
FrameGraph
makeGraph(int nTasks,
          const std::vector<std::pair<int, int>>& edges)
{
    FrameGraph graph;

    graph.tasks.resize(nTasks);
    for (const std::pair<int, int>& edge : edges) {
        graph.tasks[edge.first].dependencies.push_back(edge.second);
        graph.tasks[edge.second].consumerTasks.push_back(edge.first);
    }
    for (FrameGraph::Task& task : graph.tasks) {
        task.consumers = (int)task.consumerTasks.size();
    }

    return graph;
}

int
indexOf(const FrameGraph& graph,
        const FrameGraph::Task& task)
{
    return (int)(&task - graph.tasks.data());
}

PlacementInputs
rawInputs(const FrameGraph& graph,
          const std::set<int>& gpu,
          const std::set<int>& raw,
          const std::set<int>& cached = std::set<int>())
{
    PlacementInputs inputs;
    const FrameGraph* g = &graph;

    inputs.canRenderOnGpu = [g, gpu](const FrameGraph::Task& task) {
        return gpu.count(indexOf(*g, task)) != 0;
    };
    inputs.readsRaw = [g, raw](const FrameGraph::Task& task) {
        return raw.count(indexOf(*g, task)) != 0;
    };
    inputs.isCached = [g, cached](const FrameGraph::Task& task) {
        return cached.count(indexOf(*g, task)) != 0;
    };

    return inputs;
}

// A Read, then RawPrepare, RawTemperature, RawHighlights and RawDemosaic, which read raw data,
// then RawInputColor, which reads an image.
const std::vector<std::pair<int, int>> kDevelopChain = { { 1, 0 }, { 2, 1 }, { 3, 2 }, { 4, 3 }, { 5, 4 } };
const std::set<int> kDevelopRaw = { 1, 2, 3, 4 };

#ifdef NATRON_HAVE_GPU

// Mirrors RawLayoutProbe's block in rawprepare.slang: the layout, then the mode.
struct ProbeParams {
    RawLayoutGpu layout;
    std::uint32_t mode;
    std::uint32_t padding[3];
};
static_assert(sizeof(ProbeParams) == 80);

gpu::GpuKernelDesc
probeKernelDesc()
{
    gpu::GpuKernelDesc d;

    d.spirv = raw_test_layout_probe_spirv;
    d.spirvWords = raw_test_layout_probe_spirv_words;
    d.entry = raw_test_layout_probe_entry;
    d.storageBufferCount = raw_test_layout_probe_storage_buffers;
    d.pushConstantBytes = raw_test_layout_probe_push_bytes;
    d.groupSize = { raw_test_layout_probe_group_size[0], raw_test_layout_probe_group_size[1], raw_test_layout_probe_group_size[2] };

    return d;
}

bool
transfer(gpu::GpuRuntime& runtime,
         gpu::GpuBuffer& buffer,
         std::vector<float>& data,
         bool toDevice)
{
    gpu::TimelineValue done;
    const gpu::HostRange range { data.data(), data.size() * sizeof(float) };
    gpu::GpuStatus s = toDevice ? runtime.upload(range, buffer, 0, done) : runtime.download(buffer, 0, range, done);

    if (s) {
        s = runtime.wait(done);
    }
    EXPECT_TRUE(bool(s)) << s.message;

    return bool(s);
}

/**
 * @brief Uploads `pixels`, a one-channel image of bounds, runs the probe over the whole of it with
 * the layout and the mode, and downloads the result, which ok says is valid.
 **/
std::vector<float>
runProbe(GpuParityRuntime& runtime,
         gpu::GpuKernelId kernel,
         const RawLayout& layout,
         std::uint32_t mode,
         const RectI& bounds,
         std::vector<float> pixels,
         bool* ok)
{
    *ok = false;
    const std::size_t n = (std::size_t)bounds.area();
    EXPECT_EQ(n, pixels.size());
    std::vector<float> result(n, -7.f);
    gpu::GpuBuffer* src = runtime.buffer(0, n);
    gpu::GpuBuffer* dst = runtime.buffer(1, n);
    if (!src || !dst || !transfer(runtime.runtime(), *src, pixels, true) || !transfer(runtime.runtime(), *dst, result, true)) {
        return result;
    }

    std::unique_ptr<gpu::GpuRecordContext> ctx;
    const gpu::GpuStatus created = runtime.runtime().newRecordContext(ctx);
    EXPECT_TRUE(bool(created)) << created.message;
    if (!created) {
        return result;
    }
    ProbeParams params = {};
    params.layout = toRawLayoutGpu(layout);
    params.mode = mode;
    gpu::GpuBuffer* block = nullptr;
    const std::span<const std::byte> bytes = std::as_bytes(std::span<const ProbeParams>(&params, 1));
    if (!ctx->scratch(bytes.size(), block) || !block || !ctx->updateBuffer(*block, 0, bytes)) {
        ADD_FAILURE() << "cannot record the parameter block";

        return result;
    }
    const gpu::GpuWrapRect rect { bounds.x1, bounds.y1, bounds.x2, bounds.y2 };
    gpu::GpuWrap wrap;
    wrap.nComps = 1;
    wrap.channelMask = 0xF;
    wrap.src = gpu::GpuWrapView { src, 0, rect, (std::uint32_t)bounds.width(), 1 };
    wrap.dst = gpu::GpuWrapView { dst, 0, rect, (std::uint32_t)bounds.width(), 1 };
    wrap.output = rect;
    const gpu::GpuStatus recorded = gpu::recordWrapped(*ctx, kernel, wrap, block);
    EXPECT_TRUE(bool(recorded)) << recorded.message;
    if (!recorded) {
        return result;
    }
    const gpu::GpuStatus submitted = ctx->submitAndWait();
    EXPECT_TRUE(bool(submitted)) << submitted.message;
    ctx.reset();
    if (!submitted || !transfer(runtime.runtime(), *dst, result, false)) {
        return result;
    }
    *ok = true;

    return result;
}

// A window around the origin, so the colour lookup wraps negative coordinates too.
const RectI kWindow(-7, -5, 30, 24);

#endif // NATRON_HAVE_GPU

} // namespace

// A run of tasks reading raw data stays whole on the CPU when one of its tasks has no GPU path:
// the image task after it still goes to the GPU on its own.
TEST(RawRegionGpu, ARawRunWithACpuOpStaysOnTheCpu)
{
    FrameGraph graph = makeGraph(6, kDevelopChain);

    placeGraph(graph, rawInputs(graph, { 1, 2, 4, 5 }, kDevelopRaw));
    for (int t : { 0, 1, 2, 3, 4 }) {
        EXPECT_EQ(-1, graph.tasks[t].regionId) << "task " << t;
    }
    ASSERT_EQ(1u, graph.regions.size());
    EXPECT_EQ(std::vector<int>(1, 5), graph.regions[0].members);
}

// With a GPU path for every op, the develop chain is one region from RawPrepare to RawInputColor.
TEST(RawRegionGpu, ACompleteRawRunJoinsOneRegion)
{
    FrameGraph graph = makeGraph(6, kDevelopChain);

    placeGraph(graph, rawInputs(graph, { 1, 2, 3, 4, 5 }, kDevelopRaw));
    ASSERT_EQ(1u, graph.regions.size());
    EXPECT_EQ((std::vector<int> { 1, 2, 3, 4, 5 }), graph.regions[0].members);
    EXPECT_EQ(std::vector<int>(1, 0), graph.regions[0].externalInputs);
}

// A cached task renders nothing on either device, so it does not keep its run off the GPU.
TEST(RawRegionGpu, ACachedTaskDoesNotHoldItsRunBack)
{
    FrameGraph graph = makeGraph(6, kDevelopChain);

    placeGraph(graph, rawInputs(graph, { 1, 2, 4, 5 }, kDevelopRaw, { 3 }));
    EXPECT_EQ(-1, graph.tasks[3].regionId);
    for (int t : { 1, 2, 4, 5 }) {
        EXPECT_GE(graph.tasks[t].regionId, 0) << "task " << t;
    }
}

// Runs are joined by raw dependencies only: an incomplete one leaves another alone.
TEST(RawRegionGpu, RunsAreJudgedApart)
{
    FrameGraph graph = makeGraph(6, { { 1, 0 }, { 2, 1 }, { 4, 3 }, { 5, 4 } });

    placeGraph(graph, rawInputs(graph, { 1, 4, 5 }, { 1, 2, 4, 5 }));
    EXPECT_EQ(-1, graph.tasks[1].regionId);
    EXPECT_EQ(-1, graph.tasks[2].regionId);
    ASSERT_EQ(1u, graph.regions.size());
    EXPECT_EQ((std::vector<int> { 4, 5 }), graph.regions[0].members);
}

#ifdef NATRON_HAVE_GPU

// A one-channel mosaic goes to the device, through a wrapped kernel that keeps it as it is, and
// back with every bit unchanged.
TEST(RawRegionGpu, OneChannelMosaicRoundTripsBitExactly)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    const gpu::GpuKernelId kernel = runtime->runtime().registerKernel(probeKernelDesc());
    const float specials[] = { 0.f, -0.f, 1.f, 65535.f, 0.1f, -3.5f, 1e-30f, 3.0e38f, 0.33333334f, 16383.75f };
    std::vector<float> mosaic((std::size_t)kWindow.area());
    for (std::size_t i = 0; i < mosaic.size(); ++i) {
        mosaic[i] = specials[(i * 7) % (sizeof(specials) / sizeof(specials[0]))] + ((i % 3) == 0 ? 0.f : (float)i * 0.25f);
    }

    bool ok = false;
    const std::vector<float> back = runProbe(*runtime, kernel, *RawLayout::fromPattern("RGGB"), 0, kWindow, mosaic, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(mosaic.size(), back.size());
    for (std::size_t i = 0; i < mosaic.size(); ++i) {
        ASSERT_EQ(0, std::memcmp(&mosaic[i], &back[i], sizeof(float))) << "float " << i << ": sent " << mosaic[i] << ", got " << back[i];
    }
}

// The layout in a parameter block gives a kernel the colour RawLayout::colourAt() gives for every
// photosite of Bayer and X-Trans layouts, at several origins and on both sides of zero.
TEST(RawRegionGpu, KernelColourMatchesRawLayoutEverywhere)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    const gpu::GpuKernelId kernel = runtime->runtime().registerKernel(probeKernelDesc());
    const struct {
        const char* pattern;
        int originX;
        int originY;
    } layouts[] = {
        { "RGGB", 0, 0 },
        { "BGGR", 1, 0 },
        { "GRBG", 0, 1 },
        { "GBRG", 5, -3 },
        { "GGRGGBGGBGGRBRGRBGGGBGGRGGRGGBRBGBRG", 0, 0 },
        { "GGRGGBGGBGGRBRGRBGGGBGGRGGRGGBRBGBRG", 2, 3 },
        { "RBGBRGGGRGGBGGBGGRBRGRBGGGBGGRGGRGGB", -4, 7 },
    };

    for (const auto& l : layouts) {
        const std::optional<RawLayout> layout = RawLayout::fromPattern(l.pattern, l.originX, l.originY);
        ASSERT_TRUE(bool(layout)) << l.pattern;
        bool ok = false;
        const std::vector<float> colours = runProbe(*runtime, kernel, *layout, 1, kWindow, std::vector<float>((std::size_t)kWindow.area(), 0.f), &ok);
        ASSERT_TRUE(ok) << l.pattern;
        for (int y = kWindow.y1; y < kWindow.y2; ++y) {
            for (int x = kWindow.x1; x < kWindow.x2; ++x) {
                const float got = colours[(std::size_t)(y - kWindow.y1) * kWindow.width() + (std::size_t)(x - kWindow.x1)];
                ASSERT_EQ((float)layout->colourAt(x, y), got) << l.pattern << " at origin " << l.originX << "," << l.originY << ", pixel " << x << "," << y;
            }
        }
    }
}

#endif // NATRON_HAVE_GPU
