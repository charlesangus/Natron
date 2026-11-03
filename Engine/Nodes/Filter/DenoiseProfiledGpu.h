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

#ifndef Engine_Nodes_Filter_DenoiseProfiledGpu_h
#define Engine_Nodes_Filter_DenoiseProfiledGpu_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/Nodes/Filter/DenoiseProfiledKernels.h"

#ifdef NATRON_HAVE_GPU

#include "Engine/Gpu/GpuRecord.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief DenoiseProfiled's denoisers on the GPU. Each records into ctx the dispatches that write
 * filtered over its bounds: the colour of the CPU function of the same kind where it denoises, the
 * divided source in every other pixel and channel. divided is the source divided by its divisor
 * over a rect containing both bufferRect, the pixels the CPU fills, and filtered's bounds. The
 * caller orders the writes before reading filtered.
 **/
namespace DenoiseProfiledGpu {
/// The largest patch radius the non-local means kernel's tiles hold.
const int kMaxPatchRadius = 12;

/// denoiseNlm() over bufferRect, denoising window.
gpu::GpuStatus recordNlm(gpu::GpuRecordContext& ctx,
                         const GpuImageView& divided,
                         const RectI& bufferRect,
                         const RectI& window,
                         const DenoiseProfiledKernels::Settings& settings,
                         double renderScale,
                         const GpuImageView& filtered);

/// denoiseWavelets() over the whole of bufferRect. fullSize is as denoiseWavelets() takes it, or 0
/// for the larger side of bufferRect.
gpu::GpuStatus recordWavelets(gpu::GpuRecordContext& ctx,
                              const GpuImageView& divided,
                              const RectI& bufferRect,
                              int fullSize,
                              const DenoiseProfiledKernels::Settings& settings,
                              double renderScale,
                              const GpuImageView& filtered);
} // namespace DenoiseProfiledGpu

NATRON_NAMESPACE_EXIT

#endif // NATRON_HAVE_GPU

#endif // Engine_Nodes_Filter_DenoiseProfiledGpu_h
