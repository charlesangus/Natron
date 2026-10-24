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

#ifndef Engine_GpuPlacement_h
#define Engine_GpuPlacement_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/RenderScheduler.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief What placeGraph() asks about the tasks of a graph. Every callback is optional.
 **/
struct PlacementInputs {
    // Whether the task's effect can render it on the GPU. Unset: no task can.
    std::function<bool(const FrameGraph::Task&)> canRenderOnGpu;

    // Whether the task's output is already in the image cache, so rendering it costs nothing on the CPU. Unset: none is.
    std::function<bool(const FrameGraph::Task&)> isCached;

    // Whether the task passes its single dependency's images through. Unset: Task::isIdentity.
    std::function<bool(const FrameGraph::Task&)> isIdentity;

    // What running a region on the GPU gains over the CPU; it is accepted when positive, else shrunk. The graph it is
    // given has the members of the regions accepted so far on the GPU and every other task on the CPU. Unset: every
    // region is accepted.
    std::function<double(const FrameGraph&, const FrameGraph::Region&)> regionGain;
};

const int kMaxRegionShrinkSteps = 16;

/**
 * @brief Places the tasks of graph on the CPU or the GPU and groups the GPU ones into graph.regions, replacing any
 * previous placement.
 *
 * A task is a candidate when it is not cached and its effect can render it on the GPU. An identity task is never asked:
 * it joins the region of its dependency, whose device image it aliases, or stays on the CPU.
 *
 * Regions are grown greedily in topological order: a candidate joins the regions of all its candidate dependencies at
 * once when that keeps the graph with each region contracted to one node acyclic, else the largest one of them that
 * does, else starts a region of its own. The invariant holds after every step, so each region is convex: no path leaves
 * it, through a CPU task or another region, and comes back, which is what lets a region run as one task. Each region is
 * then evaluated with regionGain, in topological order.
 *
 * A region that gains nothing is shrunk greedily: each step drops the member on its boundary (with a dependency or a
 * consumer outside it, or the root) whose removal leaves the most gain, among those whose removal keeps the
 * contraction acyclic, and splits what is left into its connected parts. After kMaxRegionShrinkSteps steps, or when
 * no member can go, what is left stays on the CPU, as do the dropped members.
 **/
void placeGraph(FrameGraph& graph, const PlacementInputs& inputs);

/**
 * @brief Whether a GPU is ready for placement. Null, the default, means there is none.
 **/
typedef bool (*GpuPlacementAvailableFunc)();

/**
 * @brief Installs func and returns the one it replaces.
 **/
GpuPlacementAvailableFunc setGpuPlacementAvailableFunc(GpuPlacementAvailableFunc func);

bool isGpuPlacementAvailable();

/**
 * @brief The query the effect of task is asked through canRenderOnGpu() for its single plane. False when the task
 * cannot go to the GPU whatever the effect answers: it is not a native image effect, or renders more than one plane.
 **/
bool makeGpuQueryForTask(const FrameGraph::Task& task,
                         const FrameRenderContext& context,
                         GpuQuery* query);

/**
 * @brief Like makeGpuQueryForTask(), for plane, one of the planes of task.
 **/
bool makeGpuQueryForTaskPlane(const FrameGraph::Task& task,
                              const ImageLayerDesc& plane,
                              const FrameRenderContext& context,
                              GpuQuery* query);

/**
 * @brief How a GPU region produces one plane of a task: the effect renders it with query, or, for a plane the node
 * passes through, it is the image of input passThroughInput in passThroughLayer, read at passThroughTime and
 * passThroughView.
 **/
struct GpuTaskPlane {
    ImageLayerDesc plane;
    bool passThrough = false;
    GpuQuery query;
    int passThroughInput = -1;
    ImageLayerDesc passThroughLayer;
    double passThroughTime = 0.;
    ViewIdx passThroughView;
};

/**
 * @brief The planes of task a GPU region produces, in task.components order. A plane the node neither produces nor
 * passes through has no entry, as renderRoI() leaves it out of its output. False when the task cannot go to the GPU:
 * it is not a native image effect, the effect refuses the query of a plane it produces, or cannot say which layers it
 * reads, a layer it reads beyond getGpuInputLayer()'s is not one its input is asked for and carries, or a plane it
 * passes through is not asked of the pass-through input at the time and view it is read at. Placement and the region
 * engine both ask it, so they agree.
 **/
bool makeGpuTaskPlanes(const FrameGraph::Task& task,
                       const FrameRenderContext& context,
                       std::vector<GpuTaskPlane>* planes);

/**
 * @brief The inputs placeGraph() takes in a render: the effects and the image cache are asked, and, while the GPU
 * context is available, GpuCostModel::session() judges the regions at the context's calibrated bandwidth; a task
 * whose plugin has no GPU cost is then not a candidate. Must be called on the thread that built the graph, which has
 * the frame args of the render installed.
 **/
PlacementInputs makeRenderPlacementInputs(const FrameRenderContextPtr& context);

/**
 * @brief Places graph with makeRenderPlacementInputs() when isGpuPlacementAvailable(), and leaves it unchanged
 * otherwise. Returns whether it placed. A render starts here, so a GPU runtime lost by an earlier one is recreated
 * first when nothing holds it any more. While the GPU context is still initializing, it first waits for it, for at most
 * what the graph's regions would gain with free transfers, so that the first frame of a process is placed as a later
 * one would be. Same thread requirement as makeRenderPlacementInputs().
 **/
bool placeRenderGraph(FrameGraph& graph, const FrameRenderContextPtr& context);

NATRON_NAMESPACE_EXIT

#endif // Engine_GpuPlacement_h
