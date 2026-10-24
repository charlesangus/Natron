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

/*
 * Ported from darktable, DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7),
 * src/iop/hlreconstruct/segmentation.c and src/iop/hlreconstruct/segbased.c.
 * darktable is distributed under the GNU GPL, version 3 or (at your option) any later version,
 * and so is this port: GPL-3.0-or-later.
 *
 *     This file is part of darktable,
 *     Copyright (C) 2022-2026 darktable developers.
 *
 *     darktable is free software: you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation, either version 3 of the License, or
 *     (at your option) any later version.
 *
 *     darktable is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef Engine_Nodes_Raw_RawSegmentation_h
#define Engine_Nodes_Raw_RawSegmentation_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include "Engine/Nodes/Raw/RawKernels.h"

NATRON_NAMESPACE_ENTER

namespace RawKernels {
/**
 * @brief The bit darktable's segmentation sets on a location bordering a segment, beside the
 * segment's id.
 **/
const std::uint32_t kSegmentIdMask = 0x40000;

/**
 * @brief darktable's dt_iop_segmentation_t: an id per location of a plane with a border of
 * unused locations around it, and per segment its size and bounding rectangle, plus two values
 * the user of the segmentation is free to set. Ids start at 2: 0 is free and 1 is to be
 * segmentized.
 **/
struct Segmentation {
    std::vector<std::uint32_t> data;
    std::vector<int> size;
    std::vector<int> xmin;
    std::vector<int> xmax;
    std::vector<int> ymin;
    std::vector<int> ymax;
    std::vector<float> val1;
    std::vector<float> val2;
    int nr = 2;
    int border = 0;
    int slots = 0;
    int width = 0;
    int height = 0;

    /**
     * @brief dt_segmentation_init_struct(): a cleared width x height plane with room for
     * islots segments, kept between 256 and kSegmentIdMask - 2.
     **/
    void init(int width,
              int height,
              int border,
              int islots);

    /**
     * @brief _get_segment_id(): the segment at loc, 0 when there is none.
     **/
    std::uint32_t segmentId(std::size_t loc) const
    {
        if (loc >= (std::size_t)width * (std::size_t)(height - border)) {
            return 0;
        }
        const std::uint32_t id = data[loc] & (kSegmentIdMask - 1);

        return ((id < (std::uint32_t)nr) && (id > 1)) ? id : 0;
    }
};

/**
 * @brief dt_segmentize_plane(): floodfills every location marked 1 into a segment of its own
 * id and marks the locations bordering it, keeping the segments of at least four locations.
 **/
void segmentizePlane(Segmentation* seg);

/**
 * @brief dt_segments_combine(): a morphological closing of the marked locations of radius up
 * to 8, dilating by radius and eroding by radius - 3. tmp holds width x height values.
 **/
void combineSegments(Segmentation* seg,
                     int radius,
                     std::uint32_t* tmp,
                     const ParallelFor& parallel);

/**
 * @brief darktable's rebuild modes, with darktable's values.
 **/
enum SegmentsRecoveryEnum {
    eSegmentsRecoveryOff = 0,
    eSegmentsRecoverySmall = 1,
    eSegmentsRecoveryLarge = 2,
    eSegmentsRecoverySmallFlat = 3,
    eSegmentsRecoveryLargeFlat = 4,
    eSegmentsRecoveryAdapt = 5,
    eSegmentsRecoveryAdaptFlat = 6
};

struct SegmentsParams {
    // The clipping threshold times the white level, before darktable's magic factor.
    float clip = 1.f;
    // The white balance already applied, per colour.
    float coeffs[3] = { 1.f, 1.f, 1.f };
    float candidating = 0.4f;
    int combine = 2;
    SegmentsRecoveryEnum recovery = eSegmentsRecoveryOff;
    float strength = 0.f;
    float noiseLevel = 0.f;
};

/**
 * @brief darktable's segmentation based highlight reconstruction, _process_segmentation(),
 * over a whole mosaic. in is read in place, photosite (row, col) at in[row * inRowStride + col *
 * inPixelStride]; out holds the Inpaint Opposed result of the mosaic on entry, one float per
 * photosite, row-major and width wide, and the reconstruction on return. The colour planes are
 * a third of the mosaic each way plus a border of 8, as in darktable.
 **/
void segmentsHighlights(const float* in,
                        std::ptrdiff_t inRowStride,
                        std::ptrdiff_t inPixelStride,
                        float* out,
                        int width,
                        int height,
                        std::uint32_t filters,
                        const std::uint8_t (*xtrans)[6],
                        const SegmentsParams& params,
                        const ParallelFor& parallel);
} // namespace RawKernels

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Raw_RawSegmentation_h
