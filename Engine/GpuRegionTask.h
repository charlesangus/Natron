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

#ifndef Engine_GpuRegionTask_h
#define Engine_GpuRegionTask_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <map>
#include <memory>

#include "Engine/EffectInstance.h"
#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/RenderScheduler.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The RenderScheduler's region body: renders the members of graph.regions[region] on the GPU of the
 * application's GpuContext, as one sequence of device work on the calling thread.
 *
 * Each external input is put on the device once: a device image another region stored is read in place, and a RAM image
 * is uploaded, imported when its memory allows and staged otherwise. The members are recorded in topological order with
 * a barrier between them, an identity member passing its input's buffer on, and the abort is checked between them.
 *
 * Each output read on the CPU, by a task outside every region or as the frame's root, is downloaded once into a new
 * page-aligned RAM image, and the download is issued as soon as the work writing it and reading it has been submitted.
 * An output read only by other regions stays on the device and goes to the store as an evictable GpuDeviceImage. An
 * intermediate is free for the next members once its last reader was recorded, and back in the pool once that work was
 * submitted. Nothing goes to the image cache.
 *
 * A region with a buffer over the device's maxStorageBufferRange, or whose input and output images are over the budget
 * of gpuStripBudgetBytes(), runs as horizontal strips instead (see planGpuStrips()): each strip uploads the rows of its
 * inputs with the halos the members after them need, renders every member over its rows, and downloads only the rows of
 * its band of each output. Every output read after a stripped region is downloaded, since it never exists whole on the
 * device. When not even a strip of a few rows fits the storage buffer range, the members render on the CPU, counted as
 * "too large for the GPU"; when one does not fit the budget, the region is out of device memory.
 *
 * When the device runs out of memory, the least recently used buffers are evicted and the region is run once more. Each
 * time it runs out again, it is run as strips half as tall as the last run's, down to strips of kGpuStripRowQuantum
 * rows. When even those run out, when a member cannot render on the GPU, or when no GPU is available, the members are
 * rendered on the CPU with renderRoI instead, in topological order, as tasks outside every region would be, and the
 * reason is counted in the frame's RenderStats. When the device is lost, it sets report->deviceLost and returns at
 * once, for the scheduler renders the whole frame again on the CPU.
 **/
EffectInstance::RenderRoIRetCode runGpuRegion(const FrameRenderContextPtr& context,
                                              const FrameGraph& graph,
                                              int region,
                                              const RenderScheduler::CpuFallbackLease& leaseCpu,
                                              std::map<ImageLayerDesc, ImagePtr>* rootPlanes,
                                              RenderScheduler::RegionReport* report);

/**
 * @brief Keeps the application's GPU runtime from being recreated while the token is alive; null without a GPU.
 **/
std::shared_ptr<void> holdGpuRegionRuntime();

/**
 * @brief Whether runGpuRegion() has a GPU to render on: the application's GpuContext is available.
 **/
bool isGpuRegionBodyAvailable();

NATRON_NAMESPACE_EXIT

#endif // Engine_GpuRegionTask_h
