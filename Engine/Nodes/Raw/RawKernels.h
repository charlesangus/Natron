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

/**
 * @brief The smallest width and height caCorrect() works on. darktable's tiles mirror up to 16
 * photosites in from the image's edges, which a smaller image does not have.
 **/
const int kCaCorrectMinimumSize = 32;

/**
 * @brief darktable's cacorrect process(): estimates the lateral shift of red and blue against
 * green in 128-photosite blocks over the whole Bayer mosaic, fits a polynomial to the block
 * shifts and moves red and blue back by it, iterations times, in place. With avoidShift the
 * colour cast the correction leaves is measured, blurred and divided out. scaler is the processed
 * maximum. Returns whether a correction was applied: an image smaller than
 * kCaCorrectMinimumSize is left untouched, and one whose shifts cannot be fitted (too few blocks
 * with detail, a singular system) only goes through the division by scaler and back.
 **/
bool caCorrect(float* image,
               int width,
               int height,
               std::uint32_t filters,
               int iterations,
               bool avoidShift,
               float scaler,
               const ParallelFor& parallel);

/**
 * @brief The wavelet bands rawDenoiseBayer() and rawDenoiseXTrans() threshold, finest first.
 **/
const int kRawDenoiseBands = 5;

/**
 * @brief darktable's wavelet_denoise(): each of the four colours of the 2x2 Bayer block on its
 * own, square-rooted, decomposed in kRawDenoiseBands à trous bands whose details are soft
 * thresholded, and squared back. force[channel][band] is darktable's curve value per channel
 * (all, red, green, blue) and band (coarsest first), 0.5 being neutral; threshold scales them all.
 **/
void rawDenoiseBayer(const float* in,
                     float* out,
                     int width,
                     int height,
                     std::uint32_t filters,
                     float threshold,
                     const float force[4][kRawDenoiseBands],
                     const ParallelFor& parallel);

/**
 * @brief darktable's wavelet_denoise_xtrans(): rawDenoiseBayer() for an X-Trans mosaic, each
 * colour filled in at the other photosites from its nearest neighbours before the wavelets.
 **/
void rawDenoiseXTrans(const float* in,
                      float* out,
                      int width,
                      int height,
                      const std::uint8_t (*xtrans)[6],
                      float threshold,
                      const float force[4][kRawDenoiseBands],
                      const ParallelFor& parallel);

/**
 * @brief darktable's Reconstruct Color highlights mode (a1ex's colour inpainting) over a whole
 * mosaic: four scans, along each row both ways and then along each column both ways, rebuild a
 * clipped photosite from the next one of another colour by a ratio learned from the unclipped
 * photosites passed on the way; the result is the mean of the four rebuilds. in is read in
 * place, photosite (row, col) at in[row * inRowStride + col * inPixelStride]; out holds one float
 * per photosite, row-major and width wide. clip is the clipping level of red, green and blue;
 * xtrans is read when filters == kFiltersXTrans, filters when it is a Bayer word.
 **/
void inpaintColor(const float* in,
                  std::ptrdiff_t inRowStride,
                  std::ptrdiff_t inPixelStride,
                  float* out,
                  int width,
                  int height,
                  std::uint32_t filters,
                  const std::uint8_t (*xtrans)[6],
                  const float clip[3],
                  const ParallelFor& parallel);

// darktable's DS_FACTOR: Guided Laplacians diffuses on the mosaic downsampled this many times.
const int kLaplacianDownsampling = 4;

/**
 * @brief The settings of darktable's Guided Laplacians highlights mode. clips is the clipping
 * level of red, green and blue, wb the white balance multipliers already applied; scales is the
 * Diameter of Reconstruction index (0 for 2 px to 11 for 4096 px).
 **/
struct LaplacianParams {
    float clips[3] = { 1.f, 1.f, 1.f };
    float wb[3] = { 1.f, 1.f, 1.f };
    float noiseLevel = 0.f;
    int iterations = 30;
    int scales = 6;
    float solidColor = 0.f;
};

int laplacianDownsampledWidth(int width) WARN_UNUSED_RETURN;

int laplacianDownsampledHeight(int height) WARN_UNUSED_RETURN;

/**
 * @brief The first half of darktable's process_laplacian_bayer() over a whole Bayer mosaic: the
 * bilinear RGB and its clipping mask, downsampled, then iterations of guided Laplacians and heat
 * diffusion. in is read in place, photosite (row, col) at in[row * inRowStride + col *
 * inPixelStride]. reconstructed receives laplacianDownsampledWidth(width) x
 * laplacianDownsampledHeight(height) pixels of RGB and norm, four floats each; nothing is written
 * when either is 0.
 **/
void laplacianReconstruct(const float* in,
                          std::ptrdiff_t inRowStride,
                          std::ptrdiff_t inPixelStride,
                          int width,
                          int height,
                          std::uint32_t filters,
                          const LaplacianParams& params,
                          float* reconstructed,
                          const ParallelFor& parallel);

/**
 * @brief The second half: rows [row0, row0 + rows) and columns [col0, col0 + cols) of the width x
 * height mosaic, upsampled from reconstructed and blended over the input by the blurred clipping
 * mask, into out, cols wide. in holds photosite (row, col) at in[(row - inRow0) * inRowStride +
 * (col - inCol0) * inPixelStride] for the region grown by 3 photosites and clipped to the mosaic.
 * A region depends only on its own photosites and reconstructed, so tiles match the whole frame.
 **/
void laplacianRemosaic(const float* in,
                       std::ptrdiff_t inRowStride,
                       std::ptrdiff_t inPixelStride,
                       int inRow0,
                       int inCol0,
                       int width,
                       int height,
                       std::uint32_t filters,
                       const LaplacianParams& params,
                       const float* reconstructed,
                       int row0,
                       int col0,
                       int rows,
                       int cols,
                       float* out,
                       const ParallelFor& parallel);
} // namespace RawKernels

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Raw_RawKernels_h
