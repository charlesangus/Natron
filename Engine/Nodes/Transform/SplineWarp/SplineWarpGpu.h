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

#ifndef Engine_Nodes_Transform_SplineWarp_SplineWarpGpu_h
#define Engine_Nodes_Transform_SplineWarp_SplineWarpGpu_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstdint>

#include "Engine/Nodes/Transform/SplineWarp/SplineWarpField.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The parameter block of splinewarp.slang, word for word: the sampling settings, the
 * field's window and the layout of its tables (SplineWarpField::getGpuTables()), and the inverse
 * of a concatenated upstream matrix that the field's coordinates go through.
 **/
struct SplineWarpGpuParams {
    uint32_t filter = 0;
    uint32_t flags = 0;
    int32_t x1 = 0;
    int32_t y1 = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t nx = 0;
    uint32_t nodes = 0;
    uint32_t columns = 0;
    uint32_t rows = 0;
    uint32_t feathers = 0;
    uint32_t featherStride = 0;
    // Row-major, each entry a two-float pair (hi, lo).
    float matrix[18] = {};
    // With motion blur, the number of fields averaged. The bound buffer then starts with one
    // kSplineWarpGpuSubFrameWords entry per field (setSplineWarpGpuSubFrame()), which replaces the
    // field words above, and the field flags, for that field.
    uint32_t subFrames = 0;
};

static_assert(sizeof(SplineWarpGpuParams) == 31 * 4, "splinewarp.slang reads 31 words");

inline constexpr uint32_t kSplineWarpGpuSubFrameWords = 11;

// Set only for the filters Resampler::filterHonoursClamp() names.
inline constexpr uint32_t kSplineWarpGpuClamp = 1;
inline constexpr uint32_t kSplineWarpGpuBlackOutside = 2;
inline constexpr uint32_t kSplineWarpGpuJacobian = 4;
// The Source is bound; without it every pixel is black.
inline constexpr uint32_t kSplineWarpGpuSample = 8;
// The field's tables are bound; without them every pixel is its own source.
inline constexpr uint32_t kSplineWarpGpuField = 16;
inline constexpr uint32_t kSplineWarpGpuFeathers = 32;
inline constexpr uint32_t kSplineWarpGpuMatrix = 64;

inline void
setSplineWarpGpuField(const SplineWarpFieldGpuLayout& layout,
                      const RectI& window,
                      SplineWarpGpuParams* params)
{
    params->flags |= kSplineWarpGpuField | ((layout.featherStride > 0) ? kSplineWarpGpuFeathers : 0u);
    params->x1 = window.x1;
    params->y1 = window.y1;
    params->width = (uint32_t)window.width();
    params->height = (uint32_t)window.height();
    params->nx = layout.nx;
    params->nodes = layout.nodes;
    params->columns = layout.columns;
    params->rows = layout.rows;
    params->feathers = layout.feathers;
    params->featherStride = layout.featherStride;
}

// One sub-frame entry: the field flags, window and table offsets of a field whose tables start
// at word `base` of the bound buffer. A null layout leaves every pixel its own source.
inline void
setSplineWarpGpuSubFrame(const SplineWarpFieldGpuLayout* layout,
                         const RectI& window,
                         uint32_t base,
                         uint32_t entry[kSplineWarpGpuSubFrameWords])
{
    for (uint32_t k = 0; k < kSplineWarpGpuSubFrameWords; ++k) {
        entry[k] = 0;
    }
    if (!layout) {
        return;
    }
    entry[0] = kSplineWarpGpuField | ((layout->featherStride > 0) ? kSplineWarpGpuFeathers : 0u);
    entry[1] = (uint32_t)window.x1;
    entry[2] = (uint32_t)window.y1;
    entry[3] = (uint32_t)window.width();
    entry[4] = (uint32_t)window.height();
    entry[5] = layout->nx;
    entry[6] = base + layout->nodes;
    entry[7] = base + layout->columns;
    entry[8] = base + layout->rows;
    entry[9] = (layout->featherStride > 0) ? base + layout->feathers : 0u;
    entry[10] = layout->featherStride;
}

// inverse is row-major, mapping this node's input pixels to the upstream image's.
inline void
setSplineWarpGpuMatrix(const double inverse[9],
                       SplineWarpGpuParams* params)
{
    params->flags |= kSplineWarpGpuMatrix;
    for (int k = 0; k < 9; ++k) {
        params->matrix[2 * k] = (float)inverse[k];
        params->matrix[2 * k + 1] = (float)(inverse[k] - (double)params->matrix[2 * k]);
    }
}

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Transform_SplineWarp_SplineWarpGpu_h
