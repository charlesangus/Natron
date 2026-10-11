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

#include "Engine/EngineFwd.h"
#include "Engine/RenderScheduler.h"

NATRON_NAMESPACE_ENTER

struct GpuQuery;

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

    // Whether a formed region is worth running on the GPU; a rejected region's members stay on the CPU. Unset:
    // every region is accepted.
    std::function<bool(const FrameGraph&, const FrameGraph::Region&)> acceptRegion;
};

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
 * then offered to acceptRegion.
 **/
void placeGraph(FrameGraph& graph, const PlacementInputs& inputs);

/**
 * @brief Whether a GPU is ready for placement. Null, the default, means there is none.
 **/
typedef bool (*GpuPlacementAvailableFunc)();

void setGpuPlacementAvailableFunc(GpuPlacementAvailableFunc func);

bool isGpuPlacementAvailable();

/**
 * @brief The query the effect of task is asked through canRenderOnGpu(). False when the task cannot go to the GPU
 * whatever the effect answers: it is not a native image effect, or renders more than one plane.
 **/
bool makeGpuQueryForTask(const FrameGraph::Task& task,
                         const FrameRenderContext& context,
                         GpuQuery* query);

/**
 * @brief The inputs placeGraph() takes in a render: the effects and the image cache are asked. Must be called on the
 * thread that built the graph, which has the frame args of the render installed.
 **/
PlacementInputs makeRenderPlacementInputs(const FrameRenderContextPtr& context);

/**
 * @brief Places graph with makeRenderPlacementInputs() when isGpuPlacementAvailable(), and leaves it unchanged
 * otherwise. Returns whether it placed. Same thread requirement as makeRenderPlacementInputs().
 **/
bool placeRenderGraph(FrameGraph& graph, const FrameRenderContextPtr& context);

NATRON_NAMESPACE_EXIT

#endif // Engine_GpuPlacement_h
