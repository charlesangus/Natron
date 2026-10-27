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

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/FrameRenderContext.h"
#include "Engine/GpuCostModel.h"
#include "Engine/GpuPlacement.h"
#include "Engine/RenderScheduler.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuDevice.h"
#endif

NATRON_NAMESPACE_USING

namespace {

const std::size_t kBytes = 1000;

/**
 * @brief A graph of nTasks tasks, task 0 being the root, with an edge per (consumer, dependency) pair in the order
 * given. The nodes are null: placement only looks at the edges and asks the callbacks.
 **/
FrameGraph
makeGraph(int nTasks,
          const std::vector<std::pair<int, int>>& edges)
{
    FrameGraph graph;

    graph.tasks.resize(nTasks);
    for (int i = 0; i < nTasks; ++i) {
        graph.tasks[i].estimatedBytes = kBytes * (std::size_t)(i + 1);
    }
    for (const std::pair<int, int>& edge : edges) {
        graph.tasks[edge.first].dependencies.push_back(edge.second);
        graph.tasks[edge.second].consumerTasks.push_back(edge.first);
    }
    for (FrameGraph::Task& task : graph.tasks) {
        task.consumers = (int)task.consumerTasks.size();
    }

    return graph;
}

PlacementInputs
gpuTasks(const FrameGraph& graph,
         const std::set<int>& gpu)
{
    PlacementInputs inputs;
    const FrameGraph::Task* first = graph.tasks.data();

    inputs.canRenderOnGpu = [first, gpu](const FrameGraph::Task& task) {
        return gpu.count((int)(&task - first)) != 0;
    };

    return inputs;
}

int
indexOf(const FrameGraph& graph,
        const FrameGraph::Task& task)
{
    return (int)(&task - graph.tasks.data());
}

bool
reaches(const FrameGraph& graph,
        int from,
        int to)
{
    std::vector<int> stack(1, from);
    std::vector<bool> seen(graph.tasks.size(), false);

    while (!stack.empty()) {
        const int t = stack.back();
        stack.pop_back();
        if (t == to) {
            return true;
        }
        if (seen[t]) {
            continue;
        }
        seen[t] = true;
        stack.insert(stack.end(), graph.tasks[t].consumerTasks.begin(), graph.tasks[t].consumerTasks.end());
    }

    return false;
}

/**
 * @brief No path leaves a member of the region through a non-member and comes back to a member.
 **/
bool
isConvex(const FrameGraph& graph,
         int regionId)
{
    const FrameGraph::Region& region = graph.regions[regionId];

    for (int m : region.members) {
        for (int c : graph.tasks[m].consumerTasks) {
            if (graph.tasks[c].regionId == regionId) {
                continue;
            }
            for (int other : region.members) {
                if (reaches(graph, c, other)) {
                    return false;
                }
            }
        }
    }

    return true;
}

/**
 * @brief The graph with each region contracted to one node has no cycle.
 **/
bool
isContractionAcyclic(const FrameGraph& graph)
{
    const int n = (int)graph.tasks.size();
    const auto nodeOf = [&](int t) {
        return (graph.tasks[t].regionId >= 0) ? n + graph.tasks[t].regionId : t;
    };
    const int nNodes = n + (int)graph.regions.size();
    std::vector<std::set<int>> out(nNodes);

    for (int t = 0; t < n; ++t) {
        for (int d : graph.tasks[t].dependencies) {
            if (nodeOf(d) != nodeOf(t)) {
                out[nodeOf(d)].insert(nodeOf(t));
            }
        }
    }

    // 0 unvisited, 1 on the DFS path, 2 done.
    std::vector<int> state(nNodes, 0);
    std::vector<std::pair<int, std::set<int>::const_iterator>> stack;
    for (int root = 0; root < nNodes; ++root) {
        if (state[root] != 0) {
            continue;
        }
        state[root] = 1;
        stack.push_back(std::make_pair(root, out[root].begin()));
        while (!stack.empty()) {
            const int u = stack.back().first;
            if (stack.back().second == out[u].end()) {
                state[u] = 2;
                stack.pop_back();
                continue;
            }
            const int v = *stack.back().second;
            ++stack.back().second;
            if (state[v] == 1) {
                return false;
            }
            if (state[v] == 0) {
                state[v] = 1;
                stack.push_back(std::make_pair(v, out[v].begin()));
            }
        }
    }

    return true;
}

const FrameGraph::Region::Output*
findOutput(const FrameGraph::Region& region,
           int task)
{
    for (const FrameGraph::Region::Output& output : region.outputs) {
        if (output.task == task) {
            return &output;
        }
    }

    return nullptr;
}

} // namespace

// root 0 <- 1 <- 2 <- 3 <- reader 4 (CPU)
TEST(GpuPlacement, ChainBecomesOneRegion)
{
    FrameGraph graph = makeGraph(5, { { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 4 } });

    placeGraph(graph, gpuTasks(graph, { 0, 1, 2, 3 }));

    ASSERT_EQ(graph.regions.size(), 1u);
    const FrameGraph::Region& region = graph.regions[0];
    EXPECT_EQ(region.members, std::vector<int>({ 3, 2, 1, 0 }));
    EXPECT_EQ(region.externalInputs, std::vector<int>({ 4 }));
    for (int t = 0; t < 4; ++t) {
        EXPECT_EQ(graph.tasks[t].device, FrameGraph::Device::Gpu) << t;
        EXPECT_EQ(graph.tasks[t].regionId, 0) << t;
    }
    EXPECT_EQ(graph.tasks[4].device, FrameGraph::Device::Cpu);
    EXPECT_EQ(graph.tasks[4].regionId, -1);

    ASSERT_EQ(region.outputs.size(), 4u);
    const FrameGraph::Region::Output* root = findOutput(region, 0);
    ASSERT_TRUE(root);
    EXPECT_TRUE(root->isRoot);
    EXPECT_EQ(root->internalConsumers, 0);
    EXPECT_EQ(root->externalConsumers, 0);
    for (int t = 1; t < 4; ++t) {
        const FrameGraph::Region::Output* output = findOutput(region, t);
        ASSERT_TRUE(output) << t;
        EXPECT_FALSE(output->isRoot);
        EXPECT_EQ(output->internalConsumers, 1) << t;
        EXPECT_EQ(output->externalConsumers, 0) << t;
    }

    // Members 0..3 hold 1..4 kBytes, the uploaded reader 5.
    EXPECT_EQ(region.workingSetBytes, kBytes * (1 + 2 + 3 + 4 + 5));
    EXPECT_TRUE(isConvex(graph, 0));
}

// Merge 0 reads GPU 1 and CPU 2, which both read GPU 3, which reads reader 4 (CPU). Merge cannot join 3's region:
// that region would wait on 2, which waits on it.
TEST(GpuPlacement, DiamondWithCpuSideSplitsIntoConvexRegions)
{
    FrameGraph graph = makeGraph(5, { { 0, 1 }, { 0, 2 }, { 1, 3 }, { 2, 3 }, { 3, 4 } });

    placeGraph(graph, gpuTasks(graph, { 0, 1, 3 }));

    ASSERT_EQ(graph.regions.size(), 2u);
    EXPECT_EQ(graph.tasks[2].device, FrameGraph::Device::Cpu);
    EXPECT_EQ(graph.tasks[4].device, FrameGraph::Device::Cpu);
    EXPECT_EQ(graph.tasks[1].regionId, graph.tasks[3].regionId);
    EXPECT_NE(graph.tasks[0].regionId, graph.tasks[1].regionId);
    for (int t : { 0, 1, 3 }) {
        EXPECT_EQ(graph.tasks[t].device, FrameGraph::Device::Gpu) << t;
        EXPECT_GE(graph.tasks[t].regionId, 0) << t;
    }
    for (int r = 0; r < (int)graph.regions.size(); ++r) {
        EXPECT_TRUE(isConvex(graph, r)) << r;
    }
    EXPECT_TRUE(isContractionAcyclic(graph));

    const FrameGraph::Region& upstream = graph.regions[graph.tasks[3].regionId];
    EXPECT_EQ(upstream.members, std::vector<int>({ 3, 1 }));
    EXPECT_EQ(upstream.externalInputs, std::vector<int>({ 4 }));
    const FrameGraph::Region::Output* three = findOutput(upstream, 3);
    ASSERT_TRUE(three);
    EXPECT_EQ(three->internalConsumers, 1);
    EXPECT_EQ(three->externalConsumers, 1);

    const FrameGraph::Region& merge = graph.regions[graph.tasks[0].regionId];
    EXPECT_EQ(merge.members, std::vector<int>({ 0 }));
    EXPECT_EQ(merge.externalInputs, std::vector<int>({ 1, 2 }));
}

// GPU 3 feeds GPU 1 directly and through CPU 2, so they are in different regions. Root 0 (GPU) reads 3 first, then 1.
// Joining 3's region would have that region wait on 1's, which waits on it, with no CPU task on the cycle.
TEST(GpuPlacement, RegionsNeverWaitOnEachOther)
{
    FrameGraph graph = makeGraph(5, { { 0, 3 }, { 0, 1 }, { 1, 3 }, { 1, 2 }, { 2, 3 }, { 3, 4 } });

    placeGraph(graph, gpuTasks(graph, { 0, 1, 3 }));

    ASSERT_EQ(graph.regions.size(), 2u);
    EXPECT_NE(graph.tasks[1].regionId, graph.tasks[3].regionId);
    EXPECT_EQ(graph.tasks[0].regionId, graph.tasks[1].regionId);
    for (int r = 0; r < (int)graph.regions.size(); ++r) {
        EXPECT_TRUE(isConvex(graph, r)) << r;
    }
    EXPECT_TRUE(isContractionAcyclic(graph));
}

// GPU 3 feeds GPU 1 through two inputs and CPU 2 through one; the root 0 (CPU) reads 1 and 2.
TEST(GpuPlacement, MultiConsumerNodeIsOneMemberWithConsumerCounts)
{
    FrameGraph graph = makeGraph(4, { { 0, 1 }, { 0, 2 }, { 1, 3 }, { 1, 3 }, { 2, 3 } });

    placeGraph(graph, gpuTasks(graph, { 1, 3 }));

    ASSERT_EQ(graph.regions.size(), 1u);
    const FrameGraph::Region& region = graph.regions[0];
    EXPECT_EQ(region.members, std::vector<int>({ 3, 1 }));
    EXPECT_TRUE(region.externalInputs.empty());

    const FrameGraph::Region::Output* three = findOutput(region, 3);
    ASSERT_TRUE(three);
    EXPECT_EQ(three->internalConsumers, 2);
    EXPECT_EQ(three->externalConsumers, 1);
    const FrameGraph::Region::Output* one = findOutput(region, 1);
    ASSERT_TRUE(one);
    EXPECT_EQ(one->internalConsumers, 0);
    EXPECT_EQ(one->externalConsumers, 1);
    EXPECT_EQ(region.workingSetBytes, kBytes * (4 + 2));
    EXPECT_TRUE(isConvex(graph, 0));
}

// root 0 <- 1 (cached) <- 2
TEST(GpuPlacement, CacheHitStaysOnCpu)
{
    FrameGraph graph = makeGraph(3, { { 0, 1 }, { 1, 2 } });
    PlacementInputs inputs = gpuTasks(graph, { 0, 1, 2 });
    const FrameGraph::Task* cached = &graph.tasks[1];
    int capabilityCalls = 0;
    const auto canRender = inputs.canRenderOnGpu;

    inputs.isCached = [cached](const FrameGraph::Task& task) {
        return &task == cached;
    };
    inputs.canRenderOnGpu = [&](const FrameGraph::Task& task) {
        ++capabilityCalls;
        EXPECT_NE(&task, cached);

        return canRender(task);
    };
    placeGraph(graph, inputs);

    EXPECT_EQ(graph.tasks[1].device, FrameGraph::Device::Cpu);
    EXPECT_EQ(graph.tasks[1].regionId, -1);
    EXPECT_EQ(capabilityCalls, 2);
    ASSERT_EQ(graph.regions.size(), 2u);
    EXPECT_NE(graph.tasks[0].regionId, graph.tasks[2].regionId);
    EXPECT_EQ(graph.regions[graph.tasks[0].regionId].externalInputs, std::vector<int>({ 1 }));
}

// root 0 (GPU) <- identity 1 <- 2 (GPU) <- identity 3 <- reader 4 (CPU)
TEST(GpuPlacement, IdentityJoinsItsInputsRegionOnly)
{
    FrameGraph graph = makeGraph(5, { { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 4 } });
    graph.tasks[1].isIdentity = true;
    graph.tasks[3].isIdentity = true;

    placeGraph(graph, gpuTasks(graph, { 0, 1, 2, 3 }));

    EXPECT_EQ(graph.tasks[3].device, FrameGraph::Device::Cpu);
    ASSERT_EQ(graph.regions.size(), 1u);
    EXPECT_EQ(graph.regions[0].members, std::vector<int>({ 2, 1, 0 }));
    EXPECT_EQ(graph.regions[0].externalInputs, std::vector<int>({ 3 }));

    // Identity 1 aliases 2's image; the uploaded identity 3 counts.
    EXPECT_EQ(graph.regions[0].workingSetBytes, kBytes * (1 + 3 + 4));
}

TEST(GpuPlacement, RejectedRegionStaysOnCpu)
{
    FrameGraph graph = makeGraph(3, { { 0, 1 }, { 1, 2 } });
    PlacementInputs inputs = gpuTasks(graph, { 0, 1, 2 });

    inputs.regionGain = [](const FrameGraph&, const FrameGraph::Region&) {
        return -1.;
    };
    placeGraph(graph, inputs);

    EXPECT_TRUE(graph.regions.empty());
    for (const FrameGraph::Task& task : graph.tasks) {
        EXPECT_EQ(task.device, FrameGraph::Device::Cpu) << indexOf(graph, task);
        EXPECT_EQ(task.regionId, -1) << indexOf(graph, task);
    }
}

namespace {
bool
gpuUnavailable()
{
    return false;
}
}

TEST(GpuPlacement, NoAvailabilityLeavesGraphUnchanged)
{
    const FrameGraph original = makeGraph(3, { { 0, 1 }, { 1, 2 } });

    // The application's render scheduler installed its own, which later tests place with.
    struct RestoreAvailableFunc {
        GpuPlacementAvailableFunc installed = setGpuPlacementAvailableFunc(nullptr);
        ~RestoreAvailableFunc() { setGpuPlacementAvailableFunc(installed); }
    } restore;

    for (GpuPlacementAvailableFunc func : { GpuPlacementAvailableFunc(nullptr), &gpuUnavailable }) {
        setGpuPlacementAvailableFunc(func);
        EXPECT_FALSE(isGpuPlacementAvailable());

        FrameGraph graph = original;
        EXPECT_FALSE(placeRenderGraph(graph, FrameRenderContextPtr()));
        EXPECT_TRUE(graph.regions.empty());
        ASSERT_EQ(graph.tasks.size(), original.tasks.size());
        for (std::size_t i = 0; i < graph.tasks.size(); ++i) {
            EXPECT_EQ(graph.tasks[i].device, FrameGraph::Device::Cpu) << i;
            EXPECT_EQ(graph.tasks[i].regionId, -1) << i;
            EXPECT_EQ(graph.tasks[i].dependencies, original.tasks[i].dependencies) << i;
            EXPECT_EQ(graph.tasks[i].consumerTasks, original.tasks[i].consumerTasks) << i;
            EXPECT_EQ(graph.tasks[i].estimatedBytes, original.tasks[i].estimatedBytes) << i;
        }
    }
    setGpuPlacementAvailableFunc(nullptr);

    // Without a capability callback nothing is a candidate either.
    FrameGraph graph = original;
    placeGraph(graph, PlacementInputs());
    EXPECT_TRUE(graph.regions.empty());
    for (const FrameGraph::Task& task : graph.tasks) {
        EXPECT_EQ(task.device, FrameGraph::Device::Cpu);
    }
}

// root 0 (GPU) <- 1 <- 3 (GPU) <- reader 4 (CPU), and 1 (GPU) also feeds the CPU sink 2. The region loses most by
// keeping 1, but dropping it would leave {3, 0} waiting on itself through 1: the shrink drops the root first, then 1.
TEST(GpuPlacement, ShrinkingNeverBreaksConvexity)
{
    FrameGraph graph = makeGraph(5, { { 0, 1 }, { 1, 3 }, { 2, 1 }, { 3, 4 } });
    PlacementInputs inputs = gpuTasks(graph, { 0, 1, 3 });
    const std::map<int, double> memberGain = { { 0, 1. }, { 1, -10. }, { 3, 2. } };

    inputs.regionGain = [&memberGain](const FrameGraph&, const FrameGraph::Region& region) {
        double gain = 0.;
        for (int m : region.members) {
            gain += memberGain.at(m);
        }

        return gain;
    };
    placeGraph(graph, inputs);

    ASSERT_EQ(graph.regions.size(), 1u);
    EXPECT_EQ(graph.regions[0].members, std::vector<int>({ 3 }));
    EXPECT_EQ(graph.tasks[0].device, FrameGraph::Device::Cpu);
    EXPECT_EQ(graph.tasks[1].device, FrameGraph::Device::Cpu);
    EXPECT_TRUE(isConvex(graph, 0));
    EXPECT_TRUE(isContractionAcyclic(graph));
}

namespace {

const RectI kWeightRoI(0, 0, 3840, 2160);

/**
 * @brief A graph of UHD tasks placed by the weight rule, where the tasks in weights are the GPU candidates. taskWeight
 * answers Heavy for any other task, so that a weight asked of a task that should weigh nothing sends its region to
 * the GPU.
 **/
struct WeightFixture {
    std::map<int, GpuWeight> weights;
    FrameGraph graph;
    bool rootOnDevice = false;

    WeightFixture(int nTasks,
                  const std::vector<std::pair<int, int>>& edges)
        : graph(makeGraph(nTasks, edges))
    {
        for (FrameGraph::Task& task : graph.tasks) {
            task.roi = kWeightRoI;
            task.renderedRoI = kWeightRoI;
        }
    }

    std::function<GpuWeight(const FrameGraph::Task&)> taskWeight() const
    {
        return [this](const FrameGraph::Task& task) {
            const std::map<int, GpuWeight>::const_iterator found = weights.find(indexOf(graph, task));

            return (found == weights.end()) ? GpuWeight::heavy() : found->second;
        };
    }

    void place()
    {
        PlacementInputs inputs;

        inputs.canRenderOnGpu = [this](const FrameGraph::Task& task) {
            return weights.count(indexOf(graph, task)) != 0;
        };
        inputs.taskWeight = taskWeight();
        inputs.rootOnDevice = rootOnDevice;
        placeGraph(graph, inputs);
    }

    void setRoI(const RectI& roi)
    {
        for (FrameGraph::Task& task : graph.tasks) {
            task.roi = roi;
            task.renderedRoI = roi;
        }
    }
};

/**
 * @brief sink 0 (CPU) <- 1 <- ... <- nMiddle <- reader nMiddle + 1 (CPU), in a graph of nMiddle + 2 tasks.
 **/
std::vector<std::pair<int, int>>
chainEdges(int nMiddle)
{
    std::vector<std::pair<int, int>> edges;

    for (int t = 0; t <= nMiddle; ++t) {
        edges.push_back(std::make_pair(t, t + 1));
    }

    return edges;
}

// The fewest Point nodes that weigh kGpuRegionMinWeight together.
int
pointsReachingMinWeight()
{
    return (int)std::ceil(kGpuRegionMinWeight / kGpuPointWeight);
}

} // namespace

// sink 0 (CPU) <- Heavy 1 <- reader 2 (CPU)
TEST(GpuWeightPlacement, HeavyAloneGoes)
{
    WeightFixture fixture(3, chainEdges(1));

    fixture.weights[1] = GpuWeight::heavy();
    fixture.place();

    ASSERT_EQ(fixture.graph.regions.size(), 1u);
    EXPECT_EQ(fixture.graph.regions[0].members, std::vector<int>({ 1 }));
    EXPECT_EQ(fixture.graph.tasks[1].device, FrameGraph::Device::Gpu);
}

TEST(GpuWeightPlacement, PointsBelowMinWeightStay)
{
    const int nPoints = pointsReachingMinWeight() - 1;
    ASSERT_GE(nPoints, 1);
    WeightFixture fixture(nPoints + 2, chainEdges(nPoints));

    for (int t = 1; t <= nPoints; ++t) {
        fixture.weights[t] = GpuWeight::point();
    }
    fixture.place();

    EXPECT_TRUE(fixture.graph.regions.empty());
    for (const FrameGraph::Task& task : fixture.graph.tasks) {
        EXPECT_EQ(task.device, FrameGraph::Device::Cpu) << indexOf(fixture.graph, task);
    }
}

TEST(GpuWeightPlacement, PointsAtMinWeightGo)
{
    const int nPoints = pointsReachingMinWeight();
    WeightFixture fixture(nPoints + 2, chainEdges(nPoints));

    for (int t = 1; t <= nPoints; ++t) {
        fixture.weights[t] = GpuWeight::point();
    }
    fixture.place();

    ASSERT_EQ(fixture.graph.regions.size(), 1u);
    EXPECT_EQ(fixture.graph.regions[0].members.size(), (std::size_t)nPoints);
    EXPECT_EQ(fixture.graph.tasks[0].device, FrameGraph::Device::Cpu);
    EXPECT_EQ(fixture.graph.tasks[nPoints + 1].device, FrameGraph::Device::Cpu);
}

// sink 0 (CPU) <- Filter 1 <- Point 2 <- reader 3 (CPU), the Filter one Point short of kGpuRegionMinWeight.
TEST(GpuWeightPlacement, FilterAndPointsReachingMinWeightGo)
{
    const GpuWeight filter = GpuWeight::filter(kGpuRegionMinWeight - kGpuPointWeight);
    ASSERT_GT(filter.weight, 0.);

    WeightFixture pair(4, chainEdges(2));
    pair.weights[1] = filter;
    pair.weights[2] = GpuWeight::point();
    pair.place();
    ASSERT_EQ(pair.graph.regions.size(), 1u);
    EXPECT_EQ(pair.graph.regions[0].members, std::vector<int>({ 2, 1 }));

    WeightFixture alone(4, chainEdges(2));
    alone.weights[1] = filter;
    alone.place();
    EXPECT_TRUE(alone.graph.regions.empty());
    EXPECT_EQ(alone.graph.tasks[1].device, FrameGraph::Device::Cpu);
}

// sink 0 (CPU) <- Heavy 1 <- reader 2 (CPU), and a lone generator root on the device, one pixel short of the floor.
TEST(GpuWeightPlacement, RegionBelowPixelFloorStays)
{
    const int floorWidth = (int)std::ceil(kGpuRegionMinPixels);

    WeightFixture heavy(3, chainEdges(1));
    heavy.weights[1] = GpuWeight::heavy();
    heavy.setRoI(RectI(0, 0, floorWidth - 1, 1));
    heavy.place();
    EXPECT_TRUE(heavy.graph.regions.empty());
    EXPECT_EQ(heavy.graph.tasks[1].device, FrameGraph::Device::Cpu);

    heavy.setRoI(RectI(0, 0, floorWidth, 1));
    heavy.place();
    ASSERT_EQ(heavy.graph.regions.size(), 1u);

    WeightFixture generator(1, {});
    generator.weights[0] = GpuWeight::heavy();
    generator.rootOnDevice = true;
    generator.setRoI(RectI(0, 0, floorWidth - 1, 1));
    generator.place();
    EXPECT_TRUE(generator.graph.regions.empty());
}

// A lone Point root with no input: it moves nothing between the devices when its consumer takes the root on the
// device, so it goes there however little it weighs. Fed by a CPU reader, it would upload, and stays.
TEST(GpuWeightPlacement, FreeTransferRegionBelowMinWeightGoes)
{
    ASSERT_LT(kGpuPointWeight, kGpuRegionMinWeight);

    WeightFixture generator(1, {});
    generator.weights[0] = GpuWeight::point();
    generator.rootOnDevice = true;
    generator.place();
    ASSERT_EQ(generator.graph.regions.size(), 1u);
    EXPECT_EQ(generator.graph.tasks[0].device, FrameGraph::Device::Gpu);

    generator.rootOnDevice = false;
    generator.place();
    EXPECT_TRUE(generator.graph.regions.empty());

    WeightFixture read(2, { { 0, 1 } });
    read.weights[0] = GpuWeight::point();
    read.rootOnDevice = true;
    read.place();
    EXPECT_TRUE(read.graph.regions.empty());
}

// sink 0 (CPU) <- Point 1 <- identity 2 <- Point 3 <- reader 4 (CPU): the identity joins the region and adds nothing.
TEST(GpuWeightPlacement, IdentityMembersWeighNothing)
{
    ASSERT_LT(2. * kGpuPointWeight, kGpuRegionMinWeight);
    WeightFixture fixture(5, chainEdges(3));

    fixture.graph.tasks[2].isIdentity = true;
    fixture.weights[1] = GpuWeight::point();
    fixture.weights[3] = GpuWeight::point();
    fixture.place();

    EXPECT_TRUE(fixture.graph.regions.empty());
    for (const FrameGraph::Task& task : fixture.graph.tasks) {
        EXPECT_EQ(task.device, FrameGraph::Device::Cpu) << indexOf(fixture.graph, task);
    }
}

// sink 0 (CPU) <- Heavy 1 <- X 2 <- Heavy 3 <- reader 4 (CPU), X unable to render on the GPU.
TEST(GpuWeightPlacement, NodeThatCannotRenderOnGpuIsNeverPlaced)
{
    WeightFixture fixture(5, chainEdges(3));

    fixture.weights[1] = GpuWeight::heavy();
    fixture.weights[3] = GpuWeight::heavy();
    fixture.place();

    EXPECT_EQ(fixture.graph.regions.size(), 2u);
    EXPECT_EQ(fixture.graph.tasks[1].device, FrameGraph::Device::Gpu);
    EXPECT_EQ(fixture.graph.tasks[2].device, FrameGraph::Device::Cpu);
    EXPECT_EQ(fixture.graph.tasks[3].device, FrameGraph::Device::Gpu);
}

// root 0 <- 1 <- 2, judging region {1} and then the root alone.
TEST(GpuWeightPlacement, TransfersAreFreeOnlyWithoutUploadOrDownload)
{
    WeightFixture fixture(3, { { 0, 1 }, { 1, 2 } });
    FrameGraph& graph = fixture.graph;

    FrameGraph::Region middle;
    middle.members = { 1 };
    middle.externalInputs = { 2 };
    FrameGraph::Region::Output middleOutput;
    middleOutput.task = 1;
    middleOutput.externalConsumers = 1;
    middle.outputs.push_back(middleOutput);

    EXPECT_FALSE(regionTransfersAreFree(graph, middle, true));
    graph.tasks[2].device = FrameGraph::Device::Gpu;
    EXPECT_FALSE(regionTransfersAreFree(graph, middle, true));
    graph.tasks[0].device = FrameGraph::Device::Gpu;
    EXPECT_TRUE(regionTransfersAreFree(graph, middle, false));
    graph.tasks[2].device = FrameGraph::Device::Cpu;
    EXPECT_FALSE(regionTransfersAreFree(graph, middle, false));

    FrameGraph::Region root;
    root.members = { 0 };
    root.externalInputs = { 1 };
    FrameGraph::Region::Output rootOutput;
    rootOutput.task = 0;
    rootOutput.isRoot = true;
    root.outputs.push_back(rootOutput);

    graph.tasks[1].device = FrameGraph::Device::Gpu;
    const auto point = [](const FrameGraph::Task&) {
        return GpuWeight::point();
    };
    EXPECT_TRUE(regionTransfersAreFree(graph, root, true));
    EXPECT_GT(regionWeightGain(graph, root, point, true), 0.);
    EXPECT_FALSE(regionTransfersAreFree(graph, root, false));
    EXPECT_LT(regionWeightGain(graph, root, point, false), 0.);
}

namespace {

const char* const kGradeId = "net.sf.openfx.GradePlugin";
const char* const kBlurId = "net.sf.cimg.CImgBlur";
// A plug-in ID with no seed row.
const char* const kUnseededId = "fr.natron.test.Unseeded";
const RectI kUhd(0, 0, 3840, 2160);
const std::size_t kUhdBytes = (std::size_t)3840 * 2160 * 4 * sizeof(float);
const double kUhdMpx = 3840. * 2160. / 1e6;

GpuTransferRates
rates(double bytesPerSecond)
{
    GpuTransferRates r;

    r.uploadBytesPerSecond = bytesPerSecond;
    r.downloadBytesPerSecond = bytesPerSecond;

    return r;
}

/**
 * @brief A model on 16 threads, as the seeds were measured, whose traits come from a per-task table, over a graph of
 * UHD tasks.
 **/
struct CostFixture {
    GpuCostModel model;
    std::map<int, GpuTaskTraits> traits;
    FrameGraph graph;

    CostFixture(int nTasks,
                const std::vector<std::pair<int, int>>& edges)
        : graph(makeGraph(nTasks, edges))
    {
        for (FrameGraph::Task& task : graph.tasks) {
            task.roi = kUhd;
            task.renderedRoI = kUhd;
            task.estimatedBytes = kUhdBytes;
        }
        model.setThreadCount(16);
        model.setTransferRates(rates(25e9));
        model.setCpuSeed(kGradeId, 16., 16.);
        const FrameGraph* g = &graph;
        const std::map<int, GpuTaskTraits>* table = &traits;
        model.setTraitsHint([g, table](const FrameGraph::Task& task) {
            std::map<int, GpuTaskTraits>::const_iterator found = table->find(indexOf(*g, task));

            return (found == table->end()) ? GpuTaskTraits() : found->second;
        });
    }
};

} // namespace

TEST(GpuCostModel, GradeCostsFollowSeedOverride)
{
    GpuCostModel model;

    model.setThreadCount(16);
    model.setCpuSeed(kGradeId, 16., 16.);
    EXPECT_NEAR(model.cpuMs(GpuTaskTraits(kGradeId), kUhdMpx), kUhdMpx, 1e-9);
    EXPECT_NEAR(model.gpuMs(GpuTaskTraits(kGradeId), kUhdMpx), 0.497, 1e-3);
    EXPECT_FALSE(model.hasGpuCost(GpuTaskTraits(kUnseededId)));
}

TEST(GpuCostModel, SerialSeedScalesToPoolSize)
{
    GpuCostModel model;

    // Blur FIR up to sigma 25: 84.6 ms/Mpx serial, 6.9458 at 16 threads.
    const GpuTaskTraits blur(kBlurId, 25.);
    model.setThreadCount(16);
    EXPECT_NEAR(model.cpuMs(blur, 1.), 84.6 / 6.9458, 1e-6);
    model.setThreadCount(1);
    EXPECT_NEAR(model.cpuMs(blur, 1.), 84.6, 1e-6);
    model.setThreadCount(4);
    const double four = model.cpuMs(blur, 1.);
    EXPECT_GT(four, 84.6 / 6.9458);
    EXPECT_LT(four, 84.6);
    EXPECT_GE(four, 84.6 / 4.);
}

TEST(GpuCostModel, MeasurementsRefineByMovingAverage)
{
    GpuCostModel model;

    model.setThreadCount(16);
    model.setCpuSeed(kGradeId, 16., 16.);

    // 32 thread-ms per Mpx against a seed of 16.
    model.observeCpu(GpuTaskTraits(kGradeId), (std::int64_t)(2e6 * kUhdMpx), 16, kUhdMpx);
    EXPECT_NEAR(model.cpuMs(GpuTaskTraits(kGradeId), 1.), (16. + GpuCostModel::kEmaWeight * 16.) / 16., 1e-9);

    const double seedGpu = model.gpuMs(GpuTaskTraits(kGradeId), 1.);
    model.observeGpu(GpuTaskTraits(kGradeId), (std::int64_t)(1e6 * kUhdMpx), kUhdMpx);
    EXPECT_NEAR(model.gpuMs(GpuTaskTraits(kGradeId), 1.), seedGpu + GpuCostModel::kEmaWeight * (1. - seedGpu), 1e-9);

    // A measured GPU run makes a plugin without a GPU seed eligible.
    model.observeGpu(GpuTaskTraits(kUnseededId), 1000000, 1.);
    EXPECT_TRUE(model.hasGpuCost(GpuTaskTraits(kUnseededId)));
}

namespace {

GpuTransferRates
pipelinedRates(double bytesPerSecond)
{
    GpuTransferRates r = rates(bytesPerSecond);

    r.pipelinedStrips = true;

    return r;
}

} // namespace

TEST(GpuCostModel, PipeliningNeedsDeviceSupportBytesKernelHeightAndNoOutputKeptOnDevice)
{
    CostFixture fixture(3, { { 0, 1 }, { 1, 2 } });
    FrameGraph::Region region;
    region.members = { 1 };
    region.externalInputs = { 2 };
    FrameGraph::Region::Output output;
    output.task = 1;
    output.externalConsumers = 1;
    region.outputs.push_back(output);
    const double bytes = (double)kUhdBytes;
    // At 25 GB/s the bytes take 10.6 ms, of which a kernel must run a quarter.
    const double kernelMs = 5.;

    fixture.model.setTransferRates(rates(25e9));
    EXPECT_FALSE(fixture.model.pipelinesTransfers(fixture.graph, region, bytes, bytes, kernelMs));

    fixture.model.setTransferRates(pipelinedRates(25e9));
    EXPECT_TRUE(fixture.model.pipelinesTransfers(fixture.graph, region, bytes, bytes, kernelMs));
    EXPECT_FALSE(fixture.model.pipelinesTransfers(fixture.graph, region, 1 << 20, 1 << 20, kernelMs));
    EXPECT_FALSE(fixture.model.pipelinesTransfers(fixture.graph, region, bytes, bytes, 2.5));

    fixture.graph.tasks[0].device = FrameGraph::Device::Gpu;
    EXPECT_FALSE(fixture.model.pipelinesTransfers(fixture.graph, region, bytes, bytes, kernelMs));
    fixture.graph.tasks[0].device = FrameGraph::Device::Cpu;

    fixture.graph.tasks[1].renderedRoI = RectI(0, 0, 3840, 128);
    EXPECT_FALSE(fixture.model.pipelinesTransfers(fixture.graph, region, bytes, bytes, kernelMs));
}

TEST(GpuCostModel, ObservedTransfersMoveRatesByMovingAverage)
{
    GpuCostModel model;

    model.setTransferRates(rates(25e9));
    EXPECT_NEAR(model.transferMs(1e9, 0.), 40., 1e-9);

    // 80 ms where the rates give 40.
    model.observeTransfer(1e9, 0., 80.);
    const double scale = 1. + GpuCostModel::kEmaWeight;
    EXPECT_NEAR(model.transferMs(1e9, 0.), 40. * scale, 1e-9);
    EXPECT_NEAR(model.transferMs(0., 1e9), 40. * scale, 1e-9);

    // Placement sets the calibrated rates again for every render.
    model.setTransferRates(rates(25e9));
    EXPECT_NEAR(model.transferMs(1e9, 1e9), 80. * scale, 1e-9);

    GpuTransferRates unified;
    unified.unifiedMemory = true;
    model.setTransferRates(unified);
    model.observeTransfer(1e9, 0., 1000.);
    EXPECT_EQ(model.transferMs(1e9, 0.), 0.);
    model.setTransferRates(rates(25e9));
    EXPECT_NEAR(model.transferMs(1e9, 0.), 40. * scale, 1e-9);

    model.observeTransfer(1e9, 0., 0.);
    EXPECT_NEAR(model.transferMs(1e9, 0.), 40. * scale, 1e-9);
}

#ifdef NATRON_HAVE_GPU
TEST(GpuDevice, DiscreteDeviceWithResizableBarIsNotUnifiedMemory)
{
    EXPECT_FALSE(gpu::isUnifiedMemory(VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU));
    EXPECT_TRUE(gpu::isUnifiedMemory(VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU));
    EXPECT_TRUE(gpu::isUnifiedMemory(VK_PHYSICAL_DEVICE_TYPE_CPU));
}
#endif

namespace {

GpuTaskTraits
unseededTraits()
{
    return GpuTaskTraits("test.NoSeedRows");
}

} // namespace

// A plugin without rows has no price, not another plugin's: it is priced once it has measured both a CPU and a GPU row.
TEST(GpuCostModel, UnseededPluginIsPricedOnlyByItsOwnRows)
{
    GpuCostModel model;

    model.setThreadCount(16);
    const GpuTaskTraits unseeded = unseededTraits();
    EXPECT_FALSE(model.isPriced(unseeded));
    EXPECT_FALSE(model.hasGpuCost(unseeded));
    EXPECT_EQ(model.cpuMs(unseeded, 1.), 0.);
    EXPECT_EQ(model.gpuMs(unseeded, 1.), std::numeric_limits<double>::infinity());

    model.observeCpu(unseeded, 9000000, 16, 1.);
    EXPECT_FALSE(model.isPriced(unseeded));
    EXPECT_NEAR(model.cpuMs(unseeded, 1.), 9., 1e-9);

    model.observeGpu(unseeded, 7000000, 1.);
    EXPECT_TRUE(model.isPriced(unseeded));
    EXPECT_NEAR(model.gpuMs(unseeded, 1.), 7., 1e-9);

    EXPECT_TRUE(model.isPriced(GpuTaskTraits(kGradeId)));
    EXPECT_TRUE(model.isPriced(GpuTaskTraits(kBlurId, 25.)));
}

// The seed prices a Transform that renders with motion blur apart from a plain one, and measurements of either leave
// the other alone.
TEST(GpuCostModel, MotionBlurRowsPriceOnlyMotionBlur)
{
    GpuCostModel model;

    model.setThreadCount(1);
    GpuTaskTraits plain("net.sf.openfx.TransformPlugin");
    GpuTaskTraits blurred = plain;
    blurred.motionBlur = true;
    const double plainCpu = model.cpuMs(plain, 1.);
    const double blurredCpu = model.cpuMs(blurred, 1.);
    const double plainGpu = model.gpuMs(plain, 1.);
    const double blurredGpu = model.gpuMs(blurred, 1.);
    EXPECT_NE(plainCpu, model.cpuMs(GpuTaskTraits(kBlurId, 25.), 1.));
    EXPECT_NE(plainGpu, model.gpuMs(GpuTaskTraits(kBlurId, 25.), 1.));
    EXPECT_GT(blurredCpu, plainCpu);
    EXPECT_GT(blurredGpu, plainGpu);

    const double weight = GpuCostModel::kEmaWeight;
    model.observeCpu(plain, 7000000, 1, 1.);
    EXPECT_NEAR(model.cpuMs(plain, 1.), plainCpu + weight * (7. - plainCpu), 1e-9);
    EXPECT_NEAR(model.cpuMs(blurred, 1.), blurredCpu, 1e-9);

    model.observeGpu(blurred, 50000000, 1.);
    EXPECT_NEAR(model.gpuMs(blurred, 1.), blurredGpu + weight * (50. - blurredGpu), 1e-9);
    EXPECT_NEAR(model.gpuMs(plain, 1.), plainGpu, 1e-12);
    const double blurredAfter = model.gpuMs(blurred, 1.);
    model.observeGpu(plain, 2000000, 1.);
    EXPECT_NEAR(model.gpuMs(plain, 1.), plainGpu + weight * (2. - plainGpu), 1e-9);
    EXPECT_NEAR(model.gpuMs(blurred, 1.), blurredAfter, 1e-12);
}

// The recursive Blur filters have a GPU row of their own, which a measurement of the FIR rows does not touch.
TEST(GpuCostModel, RecursiveBlurFiltersHaveTheirOwnGpuRow)
{
    GpuCostModel model;

    model.setThreadCount(1);
    const GpuTaskTraits fir(kBlurId, 100., false);
    const GpuTaskTraits iir(kBlurId, 100., true);
    const double firGpu = model.gpuMs(fir, 1.);
    const double iirGpu = model.gpuMs(iir, 1.);
    EXPECT_GT(firGpu, 0.);
    EXPECT_NE(firGpu, iirGpu);

    model.observeGpu(iir, 4000000, 1.);
    EXPECT_NEAR(model.gpuMs(iir, 1.), iirGpu + GpuCostModel::kEmaWeight * (4. - iirGpu), 1e-9);
    EXPECT_NEAR(model.gpuMs(fir, 1.), firGpu, 1e-12);
}

TEST(GpuCostModel, ErodeSizeSelectsItsSeedRow)
{
    GpuCostModel model;
    const std::string erode = "net.sf.cimg.CImgErode";

    GpuTaskTraits small(erode);
    small.size = 6;
    GpuTaskTraits large(erode);
    large.size = 40;
    GpuTaskTraits unsized(erode);

    EXPECT_NEAR(model.gpuMs(small, kUhdMpx), 6.827, 1e-3);
    EXPECT_NEAR(model.gpuMs(large, kUhdMpx), 4.170, 1e-3);
    EXPECT_NE(model.gpuMs(small, 1.), model.gpuMs(large, 1.));

    GpuTaskTraits tiny = small;
    tiny.size = 1;
    GpuTaskTraits huge = large;
    huge.size = 500;
    EXPECT_NEAR(model.gpuMs(tiny, 1.), model.gpuMs(small, 1.), 1e-12);
    EXPECT_NEAR(model.gpuMs(huge, 1.), model.gpuMs(large, 1.), 1e-12);
    EXPECT_NEAR(model.gpuMs(unsized, 1.), model.gpuMs(small, 1.), 1e-12);

    const double largeGpu = model.gpuMs(large, 1.);
    model.observeGpu(large, 2000000, 1.);
    EXPECT_NEAR(model.gpuMs(large, 1.), largeGpu + GpuCostModel::kEmaWeight * (2. - largeGpu), 1e-9);
    EXPECT_NEAR(model.gpuMs(small, kUhdMpx), 6.827, 1e-3);
}

namespace {

GpuTaskTraits
seedTraits(const char* pluginId,
           double sigma = 0.,
           bool iir = false,
           GpuBlurFilter filter = GpuBlurFilter::eAny,
           int size = 0,
           bool motionBlur = false)
{
    GpuTaskTraits traits(pluginId, sigma, iir, filter);

    traits.size = size;
    traits.motionBlur = motionBlur;

    return traits;
}

} // namespace

// The seeded cost of every row a lookup can reach, including the rows that stand in when none covers the traits, so
// that the seed fragments and the lookup order cannot change a cost unnoticed.
TEST(GpuCostModel, SeededCostsArePinned)
{
    GpuCostModel model;

    model.setThreadCount(1);
    const char* const erode = "net.sf.cimg.CImgErode";
    const char* const dilate = "net.sf.cimg.CImgDilate";
    const char* const transform = "net.sf.openfx.TransformPlugin";
    const char* const radial = "net.sf.openfx.Radial";
    const char* const despill = "Despill";
    const double beyond = 2e9;

    const std::vector<std::pair<GpuTaskTraits, double>> gpu = {
        { seedTraits(kGradeId), 0.0599 },
        { seedTraits(kBlurId, 2.), 1.1470 },
        { seedTraits(kBlurId, 3.), 1.1470 },
        { seedTraits(kBlurId, 10.), 0.8527 },
        { seedTraits(kBlurId, 25.), 0.8527 },
        { seedTraits(kBlurId, 100.), 2.2600 },
        { seedTraits(kBlurId, beyond), 2.2600 },
        { seedTraits(kBlurId, 25., false, GpuBlurFilter::eFirGaussian), 0.8527 },
        { seedTraits(kBlurId, 100., true), 2.4130 },
        { seedTraits(kBlurId, beyond, true), 2.4130 },
        { seedTraits(kBlurId, 25., false, GpuBlurFilter::eBox), 2.6886 },
        { seedTraits(kBlurId, beyond, false, GpuBlurFilter::eBox), 2.6886 },
        { seedTraits(kBlurId, 25., false, GpuBlurFilter::eTriangle), 3.8870 },
        { seedTraits(kBlurId, 25., false, GpuBlurFilter::eQuadratic), 4.7803 },
        { seedTraits(kBlurId, 25., true, GpuBlurFilter::eGaussian), 3.4855 },
        { seedTraits(kBlurId, 25., true, GpuBlurFilter::eQuasiGaussian), 4.2040 },
        { seedTraits(kBlurId, 25., true, GpuBlurFilter::eBox), 2.4130 },
        { seedTraits(erode), 0.8231 },
        { seedTraits(erode, 0., false, GpuBlurFilter::eAny, 6), 0.8231 },
        { seedTraits(erode, 0., false, GpuBlurFilter::eAny, 7), 0.5027 },
        { seedTraits(erode, 0., false, GpuBlurFilter::eAny, 40), 0.5027 },
        { seedTraits(erode, 0., false, GpuBlurFilter::eAny, 2000000), 0.5027 },
        { seedTraits(dilate), 0.8337 },
        { seedTraits(dilate, 0., false, GpuBlurFilter::eAny, 40), 0.8337 },
        { seedTraits(transform), 0.0808 },
        { seedTraits(transform, 0., false, GpuBlurFilter::eAny, 0, true), 1.1803 },
    };
    for (std::size_t i = 0; i < gpu.size(); ++i) {
        EXPECT_DOUBLE_EQ(model.gpuMs(gpu[i].first, 1.), gpu[i].second) << i;
    }
    EXPECT_FALSE(model.hasGpuCost(seedTraits(radial)));
    EXPECT_FALSE(model.hasGpuCost(seedTraits(despill)));

    const std::vector<std::pair<GpuTaskTraits, double>> cpu = {
        { seedTraits(kGradeId), 11.48 },
        { seedTraits(kBlurId, 2.), 30.13 },
        { seedTraits(kBlurId, 25.), 84.6 },
        { seedTraits(kBlurId, 100.), 252.8 },
        { seedTraits(kBlurId, beyond), 46.9759 },
        { seedTraits(kBlurId, 100., true), 52.84 },
        { seedTraits(erode), 54.6268 },
        { seedTraits(dilate), 50.1112 },
        { seedTraits(transform), 26.126 },
        { seedTraits(transform, 0., false, GpuBlurFilter::eAny, 0, true), 996.1061 },
        { seedTraits(radial), 8.7415 },
        { seedTraits(despill), 22.5313 },
    };
    for (std::size_t i = 0; i < cpu.size(); ++i) {
        EXPECT_DOUBLE_EQ(model.cpuMs(cpu[i].first, 1.), cpu[i].second) << i;
    }
}

// Dilate has one seed row for every size, and the FIR Gaussian, the one Blur filter with no row of its own, prices like
// the rows for any filter.
TEST(GpuCostModel, RowsWithoutSizeOrFilterHoldForAny)
{
    GpuCostModel model;

    GpuTaskTraits dilate("net.sf.cimg.CImgDilate");
    const double any = model.gpuMs(dilate, 1.);
    dilate.size = 40;
    EXPECT_NEAR(model.gpuMs(dilate, 1.), any, 1e-12);

    for (double sigma : { 2., 25., 100. }) {
        const GpuTaskTraits fir(kBlurId, sigma, false, GpuBlurFilter::eFirGaussian);
        EXPECT_NEAR(model.gpuMs(fir, 1.), model.gpuMs(GpuTaskTraits(kBlurId, sigma), 1.), 1e-12) << sigma;
    }
}

// Each Blur filter is seeded apart, at one cost for every sigma, and what one filter measures leaves the others' costs
// as they were.
TEST(GpuCostModel, BlurFiltersAreSeededAndLearnedApart)
{
    GpuCostModel model;

    const GpuTaskTraits fir(kBlurId, 25., false, GpuBlurFilter::eFirGaussian);
    const GpuTaskTraits box(kBlurId, 25., false, GpuBlurFilter::eBox);
    const GpuTaskTraits triangle(kBlurId, 25., false, GpuBlurFilter::eTriangle);
    const GpuTaskTraits quadratic(kBlurId, 25., false, GpuBlurFilter::eQuadratic);
    const GpuTaskTraits gaussian(kBlurId, 25., true, GpuBlurFilter::eGaussian);
    const GpuTaskTraits quasiGaussian(kBlurId, 25., true, GpuBlurFilter::eQuasiGaussian);
    const std::vector<GpuTaskTraits> filters = { fir, box, triangle, quadratic, gaussian, quasiGaussian };

    EXPECT_NEAR(model.gpuMs(box, kUhdMpx), 22.30, 1e-3);
    EXPECT_NEAR(model.gpuMs(triangle, kUhdMpx), 32.24, 1e-3);
    EXPECT_NEAR(model.gpuMs(quadratic, kUhdMpx), 39.65, 1e-3);
    EXPECT_NEAR(model.gpuMs(gaussian, kUhdMpx), 28.91, 1e-3);
    EXPECT_NEAR(model.gpuMs(quasiGaussian, kUhdMpx), 34.87, 1e-3);
    for (const GpuTaskTraits& traits : filters) {
        if (traits.filter == GpuBlurFilter::eFirGaussian) {
            continue;
        }
        for (double sigma : { 2., 100. }) {
            GpuTaskTraits other = traits;
            other.sigma = sigma;
            EXPECT_NEAR(model.gpuMs(other, 1.), model.gpuMs(traits, 1.), 1e-12) << (int)traits.filter << " at " << sigma;
        }
    }

    for (std::size_t observed = 0; observed < filters.size(); ++observed) {
        std::vector<double> before;
        for (const GpuTaskTraits& traits : filters) {
            before.push_back(model.gpuMs(traits, 1.));
        }
        model.observeGpu(filters[observed], 100000000, 1.);
        for (std::size_t i = 0; i < filters.size(); ++i) {
            if (i == observed) {
                EXPECT_NEAR(model.gpuMs(filters[i], 1.), before[i] + GpuCostModel::kEmaWeight * (100. - before[i]), 1e-9) << i;
            } else {
                EXPECT_NEAR(model.gpuMs(filters[i], 1.), before[i], 1e-12) << i << " after " << observed;
            }
        }
    }
}

// Every node with a GPU kernel has seed rows of its own, so each is priced from the start.
TEST(GpuCostModel, EveryGpuNodeIsSeeded)
{
    GpuCostModel model;

    model.setThreadCount(16);
    const char* const ids[] = {
        "net.sf.openfx.GradePlugin",
        "net.sf.openfx.ColorCorrectPlugin",
        "net.sf.openfx.Invert",
        "net.sf.openfx.Clamp",
        "net.sf.openfx.AddPlugin",
        "net.sf.openfx.MultiplyPlugin",
        "net.sf.openfx.GammaPlugin",
        "net.sf.openfx.SaturationPlugin",
        "net.sf.openfx.ColorLookupPlugin",
        "net.sf.openfx.KeyerPlugin",
        "net.sf.openfx.ChromaKeyerPlugin",
        "net.sf.cimg.CImgErode",
        "net.sf.cimg.CImgDilate",
        "net.sf.openfx.ConstantPlugin",
        "net.sf.openfx.Solid",
        "net.sf.openfx.CheckerBoardPlugin",
        "net.sf.openfx.TransformPlugin",
        "net.sf.cimg.CImgBlur",
    };
    for (const char* id : ids) {
        GpuTaskTraits traits(id);
        EXPECT_TRUE(model.isPriced(traits)) << id;
        EXPECT_GT(model.cpuMs(traits, 1.), 0.) << id;
    }
}
