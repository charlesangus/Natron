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

#ifndef Engine_Nodes_Raw_RawKernels_h
#define Engine_Nodes_Raw_RawKernels_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <cstdint>
#include <functional>

NATRON_NAMESPACE_ENTER

/**
 * @brief The CPU kernels of the raw develop chain, ported from darktable's C paths. They know
 * nothing of Natron: every buffer is row-major and top-down, row 0 being the top row of the
 * image as the camera stores it, the way darktable lays its buffers out. A node turns its
 * y-up window into such a buffer and back.
 *
 * A mosaic is one float per photosite. A demosaiced buffer is four floats per pixel, red, green,
 * blue and a fourth channel the kernels use as scratch or leave at zero; the node writes alpha
 * itself.
 *
 * A Bayer pattern is described by dcraw's `filters` word, read through fcBayer(); an X-Trans one
 * by filters == kFiltersXTrans and the 6x6 colour array `xtrans`, read through fcXTrans(). Both
 * give the colour of buffer position (row, col), 0 red, 1 green, 2 blue, so they describe the
 * pattern as seen from the buffer's own top-left photosite.
 *
 * Every loop a kernel parallelises goes through a ParallelFor, which runs body(i) for each i in
 * [0, count) on any threads in any order. A kernel's result never depends on how the calls are
 * spread over threads: reductions sum fixed partials in a fixed order, and scratch a body reuses
 * is cleared before each use.
 **/
namespace RawKernels {
using ParallelFor = std::function<void(int count, const std::function<void(int)>& body)>;

/**
 * @brief A ParallelFor running every body on the calling thread, in order.
 **/
void serialFor(int count, const std::function<void(int)>& body);

const std::uint32_t kFiltersXTrans = 9u;

inline int
fcBayer(std::size_t row,
        std::size_t col,
        std::uint32_t filters)
{
    return (int)((filters >> ((((row << 1) & 14) + (col & 1)) << 1)) & 3);
}

inline int
fcXTrans(int row,
         int col,
         const std::uint8_t (*xtrans)[6])
{
    // Callers pass offsets down to -17 (Markesteijn 3-pass); 600 is a multiple of 6 that keeps
    // the index non-negative.
    return xtrans[(row + 600) % 6][(col + 600) % 6];
}

inline int
fcol(int row,
     int col,
     std::uint32_t filters,
     const std::uint8_t (*xtrans)[6])
{
    return (filters == kFiltersXTrans) ? fcXTrans(row, col, xtrans) : fcBayer((std::size_t)row, (std::size_t)col, filters);
}

/**
 * @brief The `filters` word of a Bayer pattern whose top-left 2x2 block holds colours[row][col],
 * each 0, 1 or 2.
 **/
std::uint32_t bayerFilters(const int colours[2][2]);

/**
 * @brief darktable's demosaic_ppg(): Patterned Pixel Grouping on a Bayer mosaic, with the
 * pre-median of the green channel when thrs > 0. margin is the border width darktable's RCD asks
 * PPG to fill; every other caller passes a margin larger than the image.
 **/
void ppg(float* out,
         const float* in,
         int width,
         int height,
         std::uint32_t filters,
         float thrs,
         int margin,
         const ParallelFor& parallel);

/**
 * @brief darktable's rcd_demosaic(): Ratio Corrected Demosaicing on a Bayer mosaic, scaled by
 * 1/scaler while it works. darktable passes the processed maximum as scaler.
 **/
void rcd(float* out,
         const float* in,
         int width,
         int height,
         std::uint32_t filters,
         float scaler,
         const ParallelFor& parallel);

/**
 * @brief darktable's amaze_demosaic(): AMaZE on a Bayer mosaic. clipPt is the level above which
 * a photosite counts as clipped; darktable passes the processed minimum.
 **/
void amaze(const float* in,
           float* out,
           int width,
           int height,
           std::uint32_t filters,
           float clipPt,
           const ParallelFor& parallel);

/**
 * @brief darktable's vng_interpolate(): VNG on a Bayer (VNG4) or X-Trans mosaic, or only its
 * linear interpolation when onlyLinear.
 **/
void vng(float* out,
         const float* in,
         int width,
         int height,
         std::uint32_t filters,
         const std::uint8_t (*xtrans)[6],
         bool onlyLinear,
         const ParallelFor& parallel);

/**
 * @brief darktable's _vng_lininterpolate(): each missing colour the weighted mean of its 3x3
 * neighbours of that colour, filters being kFiltersXTrans or a VNG4 word whose second green is
 * colour 3. Pixels at least border photosites from every edge are left untouched; pass a border
 * larger than the image to interpolate every pixel.
 **/
void vngLinearInterpolate(float* out,
                          const float* in,
                          int width,
                          int height,
                          std::uint32_t filters,
                          const std::uint8_t (*xtrans)[6],
                          int border,
                          const ParallelFor& parallel);

/**
 * @brief darktable's xtrans_markesteijn_interpolate(): Frank Markesteijn's X-Trans demosaic in
 * 1 or 3 passes. Photosites mirrored across the edges reach 18 photosites in, so width and
 * height must be at least 14 for 1 pass and 19 for 3.
 **/
void markesteijn(float* out,
                 const float* in,
                 int width,
                 int height,
                 const std::uint8_t (*xtrans)[6],
                 int passes,
                 const ParallelFor& parallel);

/**
 * @brief darktable's passthrough_monochrome(): every photosite's value in all three channels.
 **/
void passthroughMonochrome(float* out,
                           const float* in,
                           int width,
                           int height,
                           const ParallelFor& parallel);

/**
 * @brief darktable's color_smoothing(): passes of a 3x3 median on the red and blue differences
 * to green, leaving the outermost row and column of the buffer untouched.
 **/
void colorSmoothing(float* out,
                    int width,
                    int height,
                    int passes,
                    const ParallelFor& parallel);

/**
 * @brief darktable's green_equilibration_lavg(): each green photosite scaled by the ratio of its
 * diagonal to its orthogonal green neighbours where the neighbourhood is flat.
 **/
void greenEquilibrationLocal(float* out,
                             const float* in,
                             int width,
                             int height,
                             std::uint32_t filters,
                             float threshold,
                             const ParallelFor& parallel);

/**
 * @brief The first half of darktable's green_equilibration_favg(): the ratio of the mean of the
 * second greens to the first over the whole image, or 0 when either sum is not positive, in
 * which case nothing is to be scaled.
 **/
double greenEquilibrationFullRatio(const float* in,
                                   int width,
                                   int height,
                                   std::uint32_t filters,
                                   const ParallelFor& parallel);

/**
 * @brief The second half of green_equilibration_favg() over part of an image: buffer is the
 * width x height region whose top-left photosite is (originRow, originCol) of an image of
 * imageWidth x imageHeight, and filters describes the pattern from the image's top-left. Scales
 * the first greens by ratio in place, exactly where the whole-image function would.
 **/
void greenEquilibrationFullApply(float* buffer,
                                 int width,
                                 int height,
                                 int originRow,
                                 int originCol,
                                 int imageWidth,
                                 int imageHeight,
                                 std::uint32_t imageFilters,
                                 double ratio,
                                 const ParallelFor& parallel);
} // namespace RawKernels

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Raw_RawKernels_h
