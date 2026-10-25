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

#ifndef Engine_Nodes_Raw_RawAmazeGpu_h
#define Engine_Nodes_Raw_RawAmazeGpu_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#ifdef NATRON_HAVE_GPU

#include <algorithm>
#include <cstdint>

#include "Engine/Gpu/GpuRecord.h"
#include "Engine/Nodes/Raw/RawGpuParams.h"

NATRON_NAMESPACE_ENTER

/**
 * The recording of RawKernels::amaze() through demosaic_amaze.slang, one dispatch a stage over
 * batches of the CPU's 160 x 160 tiles.
 **/

// RawKernels::amaze()'s tile size and tile step, as demosaic_amaze.slang's TS and STEP.
inline constexpr int kAmazeGpuTile = 160;
inline constexpr int kAmazeGpuStep = kAmazeGpuTile - 32;

// The floats of one tile's scratch: demosaic_amaze.slang's kPlanes planes and its Nyquist bounds.
inline constexpr std::uint64_t kAmazeGpuTileFloats = 15ull * kAmazeGpuTile * kAmazeGpuTile + 16;

// The tiles of a width x height buffer and, in nLefts, the tiles of a row of them.
inline int
amazeGpuTiles(int width,
              int height,
              int& nLefts)
{
    nLefts = 0;
    if ((width <= 0) || (height <= 0)) {
        return 0;
    }
    nLefts = (width + 16 + kAmazeGpuStep - 1) / kAmazeGpuStep;

    return ((height + 16 + kAmazeGpuStep - 1) / kAmazeGpuStep) * nLefts;
}

// The tiles a batch holds, for a scratch of batch * kAmazeGpuTileFloats floats of at most 64 MB
// and maxScratchBytes that needs no more than maxTiles; 0 when not even one tile fits.
inline std::uint32_t
amazeGpuBatch(int maxTiles,
              std::uint64_t maxScratchBytes = UINT64_MAX)
{
    const std::uint64_t maxScratchFloats = std::min<std::uint64_t>(std::uint64_t(1) << 24, maxScratchBytes / sizeof(float));
    if (maxScratchFloats < kAmazeGpuTileFloats) {
        return 0;
    }

    return (std::uint32_t)std::min<std::uint64_t>(maxScratchFloats / kAmazeGpuTileFloats, (std::uint64_t)std::max(1, maxTiles));
}

/**
 * @brief Records RawKernels::amaze() of the push's buffer from src into its outWidth x outHeight
 * RGBA window of dst, each step ordered after the last by a compute barrier. plane holds the
 * buffer's 4 * width * height floats of RGBA photosites, tiles batch * kAmazeGpuTileFloats
 * floats; push's firstTile, nLefts and tileCount are set here. False when a dispatch fails.
 **/
inline bool
recordAmazeGpu(gpu::GpuRecordContext& ctx,
               RawAmazeGpuPush push,
               const gpu::GpuBuffer* src,
               gpu::GpuBuffer* plane,
               gpu::GpuBuffer* tiles,
               std::uint32_t batch,
               gpu::GpuBuffer* dst)
{
    int nLefts = 0;
    const int nTiles = amazeGpuTiles((int)push.width, (int)push.height, nLefts);
    push.nLefts = (std::uint32_t)std::max(1, nLefts);
    batch = std::max<std::uint32_t>(1, batch);
    const auto run = [&](gpu::GpuKernelId kernel, std::uint32_t x, std::uint32_t y, std::uint32_t z) {
        if (!ctx.dispatch(kernel, { src, plane, tiles, dst }, push, { std::max<std::uint32_t>(x, 1), std::max<std::uint32_t>(y, 1), std::max<std::uint32_t>(z, 1) })) {
            return false;
        }
        ctx.computeBarrier();

        return true;
    };
    using K = gpu::GpuKernelId;
    // A thread per tile photosite, but for the kernels that walk a tile's rows or chains in order,
    // whose threads split a row or a tile's chains.
    const std::uint32_t ts = (std::uint32_t)kAmazeGpuTile;
    const K perPhotosite[] = { K::amaze_fill, K::amaze_gradients, K::amaze_cd };
    const K nyquist[] = { K::amaze_cddiffsq, K::amaze_hvwt, K::amaze_nyqutest };
    const K area[] = { K::amaze_nyvote, K::amaze_nyarea };
    const K diagonal[] = { K::amaze_nyrefine, K::amaze_diagprep, K::amaze_diag };
    const K chroma[] = { K::amaze_grefine, K::amaze_split, K::amaze_chroma, K::amaze_output };
    const auto runAll = [&](const auto& kernels, std::uint32_t n) {
        for (K kernel : kernels) {
            if (!run(kernel, ts, ts, n)) {
                return false;
            }
        }

        return true;
    };
    for (int first = 0; first < nTiles; first += (int)batch) {
        push.firstTile = (std::uint32_t)first;
        push.tileCount = std::min<std::uint32_t>(batch, (std::uint32_t)(nTiles - first));
        const std::uint32_t n = push.tileCount;
        if (!runAll(perPhotosite, n) || !run(K::amaze_variance, 2 * ts, 2, n) || !runAll(nyquist, n) || !run(K::amaze_nybounds, ts, 1, n) || !runAll(area, n)
            || !run(K::amaze_populate, 64, 1, n) || !runAll(diagonal, n) || !run(K::amaze_pmwt, 64, 1, n) || !runAll(chroma, n)) {
            return false;
        }
    }

    return run(K::amaze_write, push.outWidth, push.outHeight, 1);
}

NATRON_NAMESPACE_EXIT

#endif // NATRON_HAVE_GPU

#endif // Engine_Nodes_Raw_RawAmazeGpu_h
