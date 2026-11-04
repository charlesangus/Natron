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

#include <algorithm>
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
#include "Engine/GpuPlacement.h"
#include "Engine/GpuTransferModel.h"
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

// Grade 0 (GPU, t) <- time offset 1 (identity at t, of t + 3) <- Grade 2 (GPU, t + 3) <- reader 3 (CPU, t + 3): the
// identity at another time joins the region, and the Grade at t + 3 feeds it on the device, with no download.
TEST(GpuPlacement, IdentityAtAnotherTimeKeepsTheRegionResident)
{
    const double t = 1.;
    FrameGraph graph = makeGraph(4, { { 0, 1 }, { 1, 2 }, { 2, 3 } });
    graph.tasks[0].key.time = t;
    graph.tasks[1].key.time = t;
    graph.tasks[1].isIdentity = true;
    graph.tasks[2].key.time = t + 3.;
    graph.tasks[3].key.time = t + 3.;

    placeGraph(graph, gpuTasks(graph, { 0, 2 }));

    ASSERT_EQ(graph.regions.size(), 1u);
    EXPECT_EQ(graph.regions[0].members, std::vector<int>({ 2, 1, 0 }));
    EXPECT_EQ(graph.regions[0].externalInputs, std::vector<int>({ 3 }));
    for (const FrameGraph::Region::Output& output : graph.regions[0].outputs) {
        if (output.task != 0) {
            EXPECT_EQ(output.externalConsumers, 0) << "task " << output.task << " leaves the device";
        }
    }
}

// A consumer 0 (GPU, t) reading producer 1 (GPU, t - 1) and producer 2 (GPU, t + 1), each over a reader at its time.
TEST(GpuPlacement, ConsumerOfTwoTimesSharesTheRegionOfBoth)
{
    const double t = 1.;
    FrameGraph graph = makeGraph(5, { { 0, 1 }, { 0, 2 }, { 1, 3 }, { 2, 4 } });
    graph.tasks[0].key.time = t;
    graph.tasks[1].key.time = t - 1.;
    graph.tasks[3].key.time = t - 1.;
    graph.tasks[2].key.time = t + 1.;
    graph.tasks[4].key.time = t + 1.;

    placeGraph(graph, gpuTasks(graph, { 0, 1, 2 }));

    ASSERT_EQ(graph.regions.size(), 1u);
    std::vector<int> members = graph.regions[0].members;
    std::sort(members.begin(), members.end());
    EXPECT_EQ(members, std::vector<int>({ 0, 1, 2 }));
    EXPECT_EQ(graph.regions[0].externalInputs, std::vector<int>({ 3, 4 }));
    for (const FrameGraph::Region::Output& output : graph.regions[0].outputs) {
        if (output.task != 0) {
            EXPECT_EQ(output.externalConsumers, 0) << "task " << output.task << " leaves the device";
        }
    }
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

const RectI kUhd(0, 0, 3840, 2160);
const std::size_t kUhdBytes = (std::size_t)3840 * 2160 * 4 * sizeof(float);

GpuTransferRates
rates(double bytesPerSecond)
{
    GpuTransferRates r;

    r.uploadBytesPerSecond = bytesPerSecond;
    r.downloadBytesPerSecond = bytesPerSecond;

    return r;
}

GpuTransferRates
pipelinedRates(double bytesPerSecond)
{
    GpuTransferRates r = rates(bytesPerSecond);

    r.pipelinedStrips = true;

    return r;
}

} // namespace

TEST(GpuTransferModel, PipeliningNeedsDeviceSupportBytesHeightAndNoOutputKeptOnDevice)
{
    FrameGraph graph = makeGraph(3, { { 0, 1 }, { 1, 2 } });
    for (FrameGraph::Task& task : graph.tasks) {
        task.roi = kUhd;
        task.renderedRoI = kUhd;
        task.estimatedBytes = kUhdBytes;
    }
    FrameGraph::Region region;
    region.members = { 1 };
    region.externalInputs = { 2 };
    FrameGraph::Region::Output output;
    output.task = 1;
    output.externalConsumers = 1;
    region.outputs.push_back(output);
    const double bytes = (double)kUhdBytes;
    GpuTransferModel model;

    model.setTransferRates(rates(25e9));
    EXPECT_FALSE(model.pipelinesTransfers(graph, region, bytes, bytes));

    model.setTransferRates(pipelinedRates(25e9));
    EXPECT_TRUE(model.pipelinesTransfers(graph, region, bytes, bytes));
    EXPECT_FALSE(model.pipelinesTransfers(graph, region, 1 << 20, 1 << 20));

    GpuTransferRates unified = pipelinedRates(25e9);
    unified.unifiedMemory = true;
    model.setTransferRates(unified);
    EXPECT_FALSE(model.pipelinesTransfers(graph, region, bytes, bytes));
    model.setTransferRates(pipelinedRates(0.));
    EXPECT_FALSE(model.pipelinesTransfers(graph, region, bytes, bytes));
    model.setTransferRates(pipelinedRates(25e9));

    graph.tasks[0].device = FrameGraph::Device::Gpu;
    EXPECT_FALSE(model.pipelinesTransfers(graph, region, bytes, bytes));
    graph.tasks[0].device = FrameGraph::Device::Cpu;

    graph.tasks[1].renderedRoI = RectI(0, 0, 3840, 128);
    EXPECT_FALSE(model.pipelinesTransfers(graph, region, bytes, bytes));
}

TEST(GpuTransferModel, ObservedTransfersMoveRatesByMovingAverage)
{
    GpuTransferModel model;

    model.setTransferRates(rates(25e9));
    EXPECT_NEAR(model.transferMs(1e9, 0.), 40., 1e-9);

    // 80 ms where the rates give 40.
    model.observeTransfer(1e9, 0., 80.);
    const double scale = 1. + GpuTransferModel::kEmaWeight;
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
