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

#include "GpuPlacement.h"

#include <algorithm>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <list>
#include <queue>
#include <tuple>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/FrameRenderContext.h"
#include "Engine/GpuContext.h"
#include "Engine/GpuCostModel.h"
#include "Engine/GpuRegionStrips.h"
#include "Engine/Image.h"
#include "Engine/ImageKey.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/RenderScale.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRuntime.h"
#endif

NATRON_NAMESPACE_ENTER

NATRON_NAMESPACE_ANONYMOUS_ENTER

std::atomic<GpuPlacementAvailableFunc> gAvailableFunc(nullptr);

/**
 * @brief The tasks in an order where each comes after its dependencies; lower dfsPostOrder, then lower index, first
 * among the ready ones. Empty when the dependencies have a cycle.
 **/
std::vector<int>
topologicalOrder(const FrameGraph& graph)
{
    const int n = (int)graph.tasks.size();
    std::vector<int> pending(n, 0);
    std::vector<std::vector<int>> consumers(n);

    for (int i = 0; i < n; ++i) {
        std::vector<int> deps = graph.tasks[i].dependencies;
        std::sort(deps.begin(), deps.end());
        deps.erase(std::unique(deps.begin(), deps.end()), deps.end());
        for (int d : deps) {
            if ((d < 0) || (d >= n)) {
                return std::vector<int>();
            }
            ++pending[i];
            consumers[d].push_back(i);
        }
    }

    typedef std::pair<int, int> ReadyKey;
    std::priority_queue<ReadyKey, std::vector<ReadyKey>, std::greater<ReadyKey>> ready;
    for (int i = 0; i < n; ++i) {
        if (pending[i] == 0) {
            ready.push(ReadyKey(graph.tasks[i].dfsPostOrder, i));
        }
    }
    std::vector<int> order;
    order.reserve(n);
    while (!ready.empty()) {
        const int i = ready.top().second;
        ready.pop();
        order.push_back(i);
        for (int c : consumers[i]) {
            if (--pending[c] == 0) {
                ready.push(ReadyKey(graph.tasks[c].dfsPostOrder, c));
            }
        }
    }
    if ((int)order.size() != n) {
        return std::vector<int>();
    }

    return order;
}

/**
 * @brief Whether the graph is acyclic once the tasks sharing a non-negative label are contracted to one node.
 **/
bool
isContractionAcyclic(const FrameGraph& graph,
                     const std::vector<int>& label)
{
    const int n = (int)graph.tasks.size();

    // Node n + l stands for label l, so that a task and a region never share a node.
    const auto nodeOf = [&](int task) {
        return (label[task] >= 0) ? n + label[task] : task;
    };
    int maxLabel = -1;
    for (int l : label) {
        maxLabel = std::max(maxLabel, l);
    }
    const int nNodes = n + maxLabel + 1;
    std::vector<std::vector<int>> out(nNodes);
    std::vector<int> indegree(nNodes, 0);
    std::vector<bool> present(nNodes, false);

    for (int i = 0; i < n; ++i) {
        const int v = nodeOf(i);
        present[v] = true;
        for (int d : graph.tasks[i].dependencies) {
            const int u = nodeOf(d);
            if (u != v) {
                out[u].push_back(v);
                ++indegree[v];
            }
        }
    }

    std::vector<int> stack;
    int nPresent = 0;
    for (int v = 0; v < nNodes; ++v) {
        if (present[v]) {
            ++nPresent;
            if (indegree[v] == 0) {
                stack.push_back(v);
            }
        }
    }
    int visited = 0;
    while (!stack.empty()) {
        const int u = stack.back();
        stack.pop_back();
        ++visited;
        for (int v : out[u]) {
            if (--indegree[v] == 0) {
                stack.push_back(v);
            }
        }
    }

    return visited == nPresent;
}

FrameGraph::Region
describeRegion(const FrameGraph& graph,
               const std::vector<int>& members,
               const std::vector<int>& label,
               int regionLabel,
               const std::vector<bool>& identity)
{
    FrameGraph::Region region;

    region.members = members;
    for (int m : members) {
        const FrameGraph::Task& task = graph.tasks[m];
        for (int d : task.dependencies) {
            if (label[d] != regionLabel) {
                region.externalInputs.push_back(d);
            }
        }
        if (!identity[m]) {
            region.workingSetBytes += task.estimatedBytes;
        }

        FrameGraph::Region::Output output;
        output.task = m;
        output.isRoot = task.consumerTasks.empty();
        for (int c : task.consumerTasks) {
            if (label[c] == regionLabel) {
                ++output.internalConsumers;
            } else {
                ++output.externalConsumers;
            }
        }
        region.outputs.push_back(output);
    }
    std::sort(region.externalInputs.begin(), region.externalInputs.end());
    region.externalInputs.erase(std::unique(region.externalInputs.begin(), region.externalInputs.end()), region.externalInputs.end());
    for (int e : region.externalInputs) {
        region.workingSetBytes += graph.tasks[e].estimatedBytes;
    }

    return region;
}

NATRON_NAMESPACE_ANONYMOUS_EXIT

void
placeGraph(FrameGraph& graph,
           const PlacementInputs& inputs)
{
    graph.regions.clear();
    for (FrameGraph::Task& task : graph.tasks) {
        task.device = FrameGraph::Device::Cpu;
        task.storage = eStorageModeRAM;
        task.regionId = -1;
    }

    const int n = (int)graph.tasks.size();
    const std::vector<int> order = topologicalOrder(graph);
    if (order.empty()) {
        return;
    }

    std::vector<bool> identity(n, false);
    for (int i = 0; i < n; ++i) {
        identity[i] = inputs.isIdentity ? inputs.isIdentity(graph.tasks[i]) : graph.tasks[i].isIdentity;
    }

    // Labels are working region ids; a merge empties the regions it absorbs.
    std::vector<int> label(n, -1);
    std::vector<std::vector<int>> regionMembers;

    const auto tryAssign = [&](int task, const std::vector<int>& absorbed, int into) -> bool {
        std::vector<int> trial = label;
        for (int i = 0; i < n; ++i) {
            if ((trial[i] >= 0) && std::find(absorbed.begin(), absorbed.end(), trial[i]) != absorbed.end()) {
                trial[i] = into;
            }
        }
        trial[task] = into;
        if (!isContractionAcyclic(graph, trial)) {
            return false;
        }
        for (int r : absorbed) {
            if (r != into) {
                regionMembers[into].insert(regionMembers[into].end(), regionMembers[r].begin(), regionMembers[r].end());
                regionMembers[r].clear();
            }
        }
        regionMembers[into].push_back(task);
        label.swap(trial);

        return true;
    };

    for (int t : order) {
        const FrameGraph::Task& task = graph.tasks[t];
        std::vector<int> depRegions;
        for (int d : task.dependencies) {
            if ((label[d] >= 0) && (std::find(depRegions.begin(), depRegions.end(), label[d]) == depRegions.end())) {
                depRegions.push_back(label[d]);
            }
        }

        if (identity[t]) {
            std::vector<int> deps = task.dependencies;
            std::sort(deps.begin(), deps.end());
            deps.erase(std::unique(deps.begin(), deps.end()), deps.end());
            if ((deps.size() == 1) && (label[deps.front()] >= 0)) {
                tryAssign(t, std::vector<int>(1, label[deps.front()]), label[deps.front()]);
            }
            continue;
        }

        if (inputs.isCached && inputs.isCached(task)) {
            continue;
        }
        if (!inputs.canRenderOnGpu || !inputs.canRenderOnGpu(task)) {
            continue;
        }

        if (!depRegions.empty()) {
            if (tryAssign(t, depRegions, depRegions.front())) {
                continue;
            }
            std::stable_sort(depRegions.begin(), depRegions.end(), [&](int a, int b) {
                return regionMembers[a].size() > regionMembers[b].size();
            });
            bool joined = false;
            for (int r : depRegions) {
                if (tryAssign(t, std::vector<int>(1, r), r)) {
                    joined = true;
                    break;
                }
            }
            if (joined) {
                continue;
            }
        }
        regionMembers.push_back(std::vector<int>());
        const int fresh = (int)regionMembers.size() - 1;
        label[t] = fresh;
        regionMembers[fresh].push_back(t);
    }

    std::vector<int> position(n, 0);
    for (int i = 0; i < n; ++i) {
        position[order[i]] = i;
    }
    std::vector<int> formed;
    for (int r = 0; r < (int)regionMembers.size(); ++r) {
        if (!regionMembers[r].empty()) {
            std::sort(regionMembers[r].begin(), regionMembers[r].end(), [&](int a, int b) {
                return position[a] < position[b];
            });
            formed.push_back(r);
        }
    }
    std::sort(formed.begin(), formed.end(), [&](int a, int b) {
        return position[regionMembers[a].front()] < position[regionMembers[b].front()];
    });

    const auto accept = [&](int l) {
        FrameGraph::Region region = describeRegion(graph, regionMembers[l], label, l, identity);
        const int regionId = (int)graph.regions.size();
        for (int m : region.members) {
            graph.tasks[m].device = FrameGraph::Device::Gpu;
            graph.tasks[m].regionId = regionId;
        }
        graph.regions.push_back(std::move(region));
    };

    const auto isOnBoundary = [&](int m, int l) {
        const FrameGraph::Task& task = graph.tasks[m];
        if (task.consumerTasks.empty()) {
            return true;
        }
        for (int d : task.dependencies) {
            if (label[d] != l) {
                return true;
            }
        }
        for (int c : task.consumerTasks) {
            if (label[c] != l) {
                return true;
            }
        }

        return false;
    };

    // Each part keeps the order of members, and the parts are ordered by their first member.
    const auto connectedParts = [&](const std::vector<int>& members, int l) {
        std::vector<int> part(n, -1);
        int nParts = 0;
        for (int seed : members) {
            if (part[seed] >= 0) {
                continue;
            }
            std::vector<int> stack(1, seed);
            part[seed] = nParts;
            while (!stack.empty()) {
                const int t = stack.back();
                stack.pop_back();
                for (const std::vector<int>* edges : { &graph.tasks[t].dependencies, &graph.tasks[t].consumerTasks }) {
                    for (int u : *edges) {
                        if ((label[u] == l) && (part[u] < 0)) {
                            part[u] = nParts;
                            stack.push_back(u);
                        }
                    }
                }
            }
            ++nParts;
        }
        std::vector<std::vector<int>> parts(nParts);
        for (int m : members) {
            parts[part[m]].push_back(m);
        }

        return parts;
    };

    // Dropping a whole region keeps the contraction acyclic: a cycle through its members, now separate nodes, would
    // have been one through the region. So does splitting a region into parts with no edge between them. Dropping a
    // single member does not, hence the check.
    for (int r : formed) {
        if (!inputs.regionGain) {
            accept(r);
            continue;
        }
        std::vector<int> queue(1, r);
        int steps = 0;
        while (!queue.empty()) {
            const int q = queue.front();
            queue.erase(queue.begin());
            const std::vector<int> members = regionMembers[q];
            if (inputs.regionGain(graph, describeRegion(graph, members, label, q, identity)) > 0.) {
                accept(q);
                continue;
            }

            int drop = -1;
            double dropGain = 0.;
            if ((members.size() > 1) && (steps < kMaxRegionShrinkSteps)) {
                for (int m : members) {
                    if (!isOnBoundary(m, q)) {
                        continue;
                    }
                    label[m] = -1;
                    if (isContractionAcyclic(graph, label)) {
                        std::vector<int> rest;
                        for (int other : members) {
                            if (other != m) {
                                rest.push_back(other);
                            }
                        }
                        const double gain = inputs.regionGain(graph, describeRegion(graph, rest, label, q, identity));
                        if ((drop < 0) || (gain > dropGain)) {
                            drop = m;
                            dropGain = gain;
                        }
                    }
                    label[m] = q;
                }
            }
            if (drop < 0) {
                for (int m : members) {
                    label[m] = -1;
                }
                regionMembers[q].clear();
                continue;
            }

            ++steps;
            label[drop] = -1;
            std::vector<int> rest;
            for (int m : members) {
                if (m != drop) {
                    rest.push_back(m);
                }
            }
            std::vector<std::vector<int>> parts = connectedParts(rest, q);
            regionMembers[q] = parts.front();
            queue.push_back(q);
            for (std::size_t i = 1; i < parts.size(); ++i) {
                const int fresh = (int)regionMembers.size();
                for (int m : parts[i]) {
                    label[m] = fresh;
                }
                regionMembers.push_back(parts[i]);
                queue.push_back(fresh);
            }
            std::sort(queue.begin(), queue.end(), [&](int a, int b) {
                return position[regionMembers[a].front()] < position[regionMembers[b].front()];
            });
        }
    }
} // placeGraph

GpuPlacementAvailableFunc
setGpuPlacementAvailableFunc(GpuPlacementAvailableFunc func)
{
    return gAvailableFunc.exchange(func);
}

bool
isGpuPlacementAvailable()
{
    const GpuPlacementAvailableFunc func = gAvailableFunc.load();

    return func && func();
}

bool
makeGpuQueryForTask(const FrameGraph::Task& task,
                    const FrameRenderContext& context,
                    GpuQuery* query)
{
    return (task.components.size() == 1) && makeGpuQueryForTaskPlane(task, task.components.front(), context, query);
}

bool
makeGpuQueryForTaskPlane(const FrameGraph::Task& task,
                         const ImageLayerDesc& plane,
                         const FrameRenderContext& context,
                         GpuQuery* query)
{
    const NodePtr& node = task.key.node;
    if (!node || !context.getRequest()) {
        return false;
    }
    EffectInstancePtr effect = node->getEffectInstance();
    NativeEffectBase* native = dynamic_cast<NativeEffectBase*>(effect.get());
    if (!native || !native->opKind()) {
        return false;
    }
    FrameRequestMap::const_iterator nodeRequest = context.getRequest()->find(node);
    if ((nodeRequest == context.getRequest()->end()) || !nodeRequest->second) {
        return false;
    }

    query->time = task.key.time;
    query->view = task.key.view;
    const bool rendersFullScale = (task.key.mipmapLevel != 0) && (native->supportsRenderScaleMaybe() != EffectInstance::eSupportsYes);
    query->mappedScale = RenderScale::fromMipmapLevel(rendersFullScale ? 0 : task.key.mipmapLevel);
    query->plane = plane;
    query->nComps = plane.getNumComponents();
    query->bitDepth = task.bitdepth;

    const GpuStageSummary stages = native->getGpuStageSummary(task.key.time, task.key.view);
    query->hasMask = stages.applyMask;
    query->maskInPlane = stages.applyMask && ((stages.maskChannel < 0) || (Node::getUnPremultSkipChannel(plane, stages.maskLayer, stages.maskChannel) >= 0));
    query->mix = stages.mix;

    const std::bitset<4> channels = native->getProcessChannelsForPlane(nodeRequest->second->nodeHash, task.key.time, task.key.view, plane);
    for (int i = 0; i < 4; ++i) {
        query->processChannels[i] = channels[i];
    }

    // The same redirections renderRoI() hands getImage().
    query->hasConcatenatedInput = false;
    const FrameViewRequest* frameView = nodeRequest->second->getFrameViewRequest(task.key.time, task.key.view);
    if (frameView && frameView->globalData.transforms) {
        for (InputMatrixMap::const_iterator it = frameView->globalData.transforms->begin(); it != frameView->globalData.transforms->end(); ++it) {
            query->hasConcatenatedInput = query->hasConcatenatedInput || bool(it->second.newInputEffect);
        }
    }
    const ParallelRenderArgsPtr frameArgs = context.getArgs(node);
    query->draftMode = frameArgs && frameArgs->draftMode;

    query->unPremultChannel = stages.divisorChannel;
    query->unPremultInPlane = (stages.divisorChannel >= 0) && (Node::getUnPremultSkipChannel(plane, stages.divisorLayer, stages.divisorChannel) >= 0);

    return true;
} // makeGpuQueryForTaskPlane

NATRON_NAMESPACE_ANONYMOUS_ENTER

bool
containsSameLayer(const std::list<ImageLayerDesc>& layers,
                  const ImageLayerDesc& layer)
{
    for (const ImageLayerDesc& other : layers) {
        if ((other == layer) && (other.getChannels() == layer.getChannels())) {
            return true;
        }
    }

    return false;
}

// Whether the request asks the input of edge for layer and the input renders it or passes it through, so that its
// task stores an image of that layer.
bool
inputCarriesLayer(const FrameRequestMap& request,
                  const FrameViewRequest::TaskEdge& edge,
                  const ImageLayerDesc& layer)
{
    const NodePtr input = edge.node.lock();
    const EffectInstancePtr effect = input ? input->getEffectInstance() : EffectInstancePtr();
    const FrameRequestMap::const_iterator inputRequest = input ? request.find(input) : request.end();
    const FrameViewRequest* fvRequest = input ? request.findFrameViewRequest(input, edge.time, edge.view) : 0;
    if (!effect || (inputRequest == request.end()) || !inputRequest->second || !fvRequest || !containsSameLayer(fvRequest->componentsRequested, layer)) {
        return false;
    }
    EffectInstance::ComponentsNeededMap comps;
    std::list<ImageLayerDesc> passThroughLayers;
    double passThroughTime = 0.;
    int passThroughView = 0;
    std::bitset<4> processChannels;
    EffectInstance::ProcessChannelsPerPlaneMap processChannelsPerPlane;
    int passThroughInput = -1;
    effect->getComponentsNeededAndProduced_public(inputRequest->second->nodeHash, edge.time, edge.view, &comps, &passThroughLayers, &passThroughTime,
                                                  &passThroughView, &processChannels, &processChannelsPerPlane, &passThroughInput);
    const EffectInstance::ComponentsNeededMap::const_iterator produced = comps.find(-1);

    return ((produced != comps.end()) && containsSameLayer(produced->second, layer)) || containsSameLayer(passThroughLayers, layer);
}

NATRON_NAMESPACE_ANONYMOUS_EXIT

bool
makeGpuTaskPlanes(const FrameGraph::Task& task,
                  const FrameRenderContext& context,
                  std::vector<GpuTaskPlane>* planes)
{
    planes->clear();
    const NodePtr& node = task.key.node;
    const std::shared_ptr<FrameRequestMap>& request = context.getRequest();
    if (!node || task.components.empty() || !request) {
        return false;
    }
    NativeEffectBase* native = dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
    FrameRequestMap::const_iterator nodeRequest = request->find(node);
    const FrameViewRequest* fvRequest = request->findFrameViewRequest(node, task.key.time, task.key.view);
    if (!native || !native->opKind() || (nodeRequest == request->end()) || !nodeRequest->second || !fvRequest) {
        return false;
    }

    // What renderRoI() renders and passes through.
    EffectInstance::ComponentsNeededMap comps;
    std::list<ImageLayerDesc> passThroughLayers;
    double passThroughTime = 0.;
    int passThroughView = 0;
    std::bitset<4> processChannels;
    EffectInstance::ProcessChannelsPerPlaneMap processChannelsPerPlane;
    int passThroughInput = -1;
    native->getComponentsNeededAndProduced_public(nodeRequest->second->nodeHash, task.key.time, task.key.view, &comps, &passThroughLayers, &passThroughTime,
                                                  &passThroughView, &processChannels, &processChannelsPerPlane, &passThroughInput);
    const EffectInstance::ComponentsNeededMap::const_iterator produced = comps.find(-1);
    const std::list<ImageLayerDesc> outputLayers = (produced == comps.end()) ? std::list<ImageLayerDesc>() : produced->second;
    const int nInputs = native->getNInputs();

    for (const ImageLayerDesc& plane : task.components) {
        GpuTaskPlane entry;
        entry.plane = plane;
        if (ImageLayerDesc::findEquivalentLayer(plane, outputLayers.begin(), outputLayers.end()) != outputLayers.end()) {
            if (!makeGpuQueryForTaskPlane(task, plane, context, &entry.query) || !native->canRenderOnGpu(entry.query)) {
                return false;
            }
            for (const FrameViewRequest::TaskEdge& edge : fvRequest->dependencies) {
                if (!edge.node.lock() || (edge.inputNb < 0) || (edge.inputNb >= nInputs)) {
                    continue;
                }
                std::vector<ImageLayerDesc> extraLayers;
                if (!native->getGpuExtraInputLayers(edge.inputNb, plane, task.key.time, task.key.view, &extraLayers)) {
                    return false;
                }
                for (const ImageLayerDesc& layer : extraLayers) {
                    if (!inputCarriesLayer(*request, edge, layer)) {
                        return false;
                    }
                }
            }
            planes->push_back(entry);
            continue;
        }
        const std::list<ImageLayerDesc>::const_iterator passed = ImageLayerDesc::findEquivalentLayer(plane, passThroughLayers.begin(), passThroughLayers.end());
        if (passed == passThroughLayers.end()) {
            continue;
        }
        entry.passThrough = true;
        entry.passThroughInput = passThroughInput;
        entry.passThroughLayer = *passed;
        entry.passThroughTime = passThroughTime;
        entry.passThroughView = ViewIdx(passThroughView);
        bool resolved = false;
        for (const FrameViewRequest::TaskEdge& edge : fvRequest->dependencies) {
            if ((edge.inputNb == passThroughInput) && (edge.time == passThroughTime) && (edge.view == entry.passThroughView) && edge.node.lock()) {
                const NodePtr input = edge.node.lock();
                const FrameViewRequest* inputRequest = request->findFrameViewRequest(input, edge.time, edge.view);
                resolved = inputRequest && containsSameLayer(inputRequest->componentsRequested, entry.passThroughLayer);
                break;
            }
        }
        if (!resolved) {
            return false;
        }
        planes->push_back(entry);
    }

    return !planes->empty();
} // makeGpuTaskPlanes

NATRON_NAMESPACE_ANONYMOUS_ENTER

// Only a task the model has a GPU cost for is a candidate.
void
judgeWithCostModel(PlacementInputs* inputs,
                   GpuCostModel* model)
{
    const std::function<bool(const FrameGraph::Task&)> effectCanRender = inputs->canRenderOnGpu;
    inputs->canRenderOnGpu = [model, effectCanRender](const FrameGraph::Task& task) -> bool {
        return model->hasGpuCost(model->traitsOf(task)) && effectCanRender(task);
    };
}

// Long enough for any driver to come up; past it, a context still initializing is presumed stuck.
const std::chrono::seconds kGpuInitWaitLimit(10);

/**
 * Before the context is up the transfer rates are unknown, so the graph is placed with free transfers: what its regions
 * gain then bounds what the GPU can save on this frame. The frame waits for the context up to that bound, since
 * waiting longer could only lose to the CPU.
 **/
void
waitForGpuInitIfWorthIt(const GpuContext& gpu,
                        const FrameGraph& graph,
                        const FrameRenderContextPtr& context)
{
    if (!gpu.isInitializing() || graph.tasks.empty() || !context) {
        return;
    }
    const std::chrono::steady_clock::duration left = gpu.createdAt() + kGpuInitWaitLimit - std::chrono::steady_clock::now();
    if (left <= std::chrono::steady_clock::duration::zero()) {
        return;
    }

    GpuCostModel* model = &GpuCostModel::session();
    PlacementInputs inputs = makeRenderPlacementInputs(context);
    judgeWithCostModel(&inputs, model);
    inputs.regionGain = [model](const FrameGraph& trial, const FrameGraph::Region& region) {
        return model->regionGainBoundMs(trial, region);
    };
    FrameGraph trial = graph;
    placeGraph(trial, inputs);
    double gainMs = 0.;
    for (const FrameGraph::Region& region : trial.regions) {
        gainMs += std::max(0., model->regionGainBoundMs(trial, region));
    }
    if (!(gainMs > 0.)) {
        return;
    }
    const double leftMs = std::chrono::duration<double, std::milli>(left).count();
    gpu.waitForInit(std::chrono::milliseconds((std::int64_t)std::ceil(std::min(gainMs, leftMs))));
}

NATRON_NAMESPACE_ANONYMOUS_EXIT

PlacementInputs
makeRenderPlacementInputs(const FrameRenderContextPtr& context)
{
    PlacementInputs inputs;

    inputs.canRenderOnGpu = [context](const FrameGraph::Task& task) -> bool {
        std::vector<GpuTaskPlane> planes;

        return context && makeGpuTaskPlanes(task, *context, &planes);
    };

    GpuContext* gpu = appPTR ? appPTR->getGpuContext() : nullptr;
    if (gpu && gpu->isAvailable()) {
        GpuCostModel* model = &GpuCostModel::session();
        GpuTransferRates rates = GpuTransferRates::fromCalibration(gpu->calibration());
#ifdef NATRON_HAVE_GPU
        if (gpu::GpuRuntime* runtime = gpu->runtime()) {
            const gpu::GpuDeviceInfo& info = runtime->device().info();
            rates.pipelinedStrips = gpuStripPipeliningAllowed(info.pushDescriptor, info.transferSharesComputeQueue);
        }
#endif
        model->setTransferRates(rates);
        judgeWithCostModel(&inputs, model);
        inputs.regionGain = [model](const FrameGraph& graph, const FrameGraph::Region& region) {
            return model->regionGainMs(graph, region);
        };
    }

    // Looks the cache up as renderRoI's first, non-draft, lookup does. Not knowing which full-scale flag renderRoI
    // will key with, either counts; a false hit only keeps a task on the CPU.
    inputs.isCached = [](const FrameGraph::Task& task) -> bool {
        if (!task.sharesCachedOutput || !task.key.node || !appPTR || task.components.empty()) {
            return false;
        }
        for (int fullScaleWithDownscaleInputs = 0; fullScaleWithDownscaleInputs < 2; ++fullScaleWithDownscaleInputs) {
            ImageKey key(task.key.node.get(), task.nodeHash, task.cacheKeyHasTime, task.key.time, task.key.view, 1., false,
                         fullScaleWithDownscaleInputs != 0);
            std::list<ImagePtr> images;
            if (!appPTR->getImage(key, &images)) {
                continue;
            }
            bool allLayers = true;
            for (const ImageLayerDesc& layer : task.components) {
                bool covered = false;
                for (const ImagePtr& image : images) {
                    if (!image || (image->getMipmapLevel() != task.cacheMipmapLevel)) {
                        continue;
                    }
                    const ImageLayerDesc& cached = image->getComponents();
                    if (!((cached == layer) || (cached.isColorLayer() && layer.isColorLayer()))) {
                        continue;
                    }
                    std::list<RectI> rest;
                    image->getRestToRender(task.renderedRoI, rest);
                    if (rest.empty()) {
                        covered = true;
                        break;
                    }
                }
                if (!covered) {
                    allLayers = false;
                    break;
                }
            }
            if (allLayers) {
                return true;
            }
        }

        return false;
    };

    return inputs;
} // makeRenderPlacementInputs

bool
placeRenderGraph(FrameGraph& graph,
                 const FrameRenderContextPtr& context)
{
    GpuContext* gpu = appPTR ? appPTR->getGpuContext() : nullptr;
    if (gpu) {
        waitForGpuInitIfWorthIt(*gpu, graph, context);
        gpu->recoverIfLost();
    }
    if (!isGpuPlacementAvailable() || graph.tasks.empty() || !context) {
        return false;
    }
    placeGraph(graph, makeRenderPlacementInputs(context));

    return true;
}

NATRON_NAMESPACE_EXIT
