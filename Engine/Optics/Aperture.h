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

// The aperture model is ported from realflare (https://github.com/beatreichenbach/realflare,
// GPL-3.0-or-later, v1.0.3 at 9ce30b0): flare/engine/tasks/shaders/aperture.frag and
// flare/engine/tasks/aperture.py.

#ifndef Engine_Optics_Aperture_h
#define Engine_Optics_Aperture_h

// Kept free of Engine, Qt and Python headers so the NatronGpu library can include it.

#include "Global/Macros.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

NATRON_NAMESPACE_ENTER

namespace Optics {

/**
 * @brief A procedural diaphragm: a polygon of blades, multiplied by grating, scratches, dust and an
 * image layer. Values are in knob units: rotations in degrees, and widths, radii and softnesses as
 * the knobs show them (the rasteriser scales widths and radii by 0.1 and divides softnesses by 10).
 * Sizes are fractions of the mask's half extent.
 **/
struct ApertureParams {
    double sizeX = 0.75;
    double sizeY = 0.75;
    int blades = 6;
    double roundness = 0.;
    double rotation = 0.;
    double softness = 0.;

    double gratingStrength = 0.;
    double gratingDensity = 0.5;
    double gratingLength = 0.5;
    double gratingWidth = 0.25;
    double gratingSoftness = 0.;

    double scratchesStrength = 0.;
    double scratchesDensity = 0.5;
    double scratchesLength = 0.5;
    double scratchesWidth = 0.25;
    double scratchesRotation = 0.;
    double scratchesRotationVariation = 0.;
    double scratchesSoftness = 0.;
    double scratchesParallaxX = 1.;
    double scratchesParallaxY = 1.;

    double dustStrength = 0.;
    double dustDensity = 0.5;
    double dustRadius = 0.5;
    double dustSoftness = 0.;
    double dustParallaxX = 1.;
    double dustParallaxY = 1.;

    double imageStrength = 0.;
    double imageSizeX = 0.75;
    double imageSizeY = 0.75;
    double imageBlack = 0.;
    double imageWhite = 1.;
};

/**
 * @brief The image layer's source: one float channel, width x height, rows packed from the bottom
 * one. Sampled bilinearly, reading 0 outside.
 **/
struct ApertureImage {
    const float* pixels = nullptr;
    int width = 0;
    int height = 0;

    bool empty() const { return !pixels || width <= 0 || height <= 0; }
};

/**
 * @brief What a mask is rasterised over: width x height pixels spanning [-1, 1] on both axes, the
 * first row at -1. The light position, scaled per axis by the scratches' and dust's parallax, shifts
 * where they sit; a shift of 1 moves them by the mask's full extent, since their placement wraps.
 **/
struct ApertureFrame {
    int width = 0;
    int height = 0;
    double lightX = 0.;
    double lightY = 0.;
};

/**
 * @brief Words of the tables buildApertureTables() makes, which the CPU rasteriser and the GPU kernel
 * (Engine/Gpu/Kernels/aperture.slang) both read: a header of these indices, floats stored as their
 * bits, then the per-blade, per-bar, per-scratch and per-particle entries at the header's offsets.
 * Everything the two sides could round differently (trigonometry, divisions, hashes) is computed
 * here once, so they evaluate the same arithmetic.
 **/
enum ApertureWord : uint32_t {
    kApertureWidth = 0,
    kApertureHeight,
    kApertureNdcScaleX,
    kApertureNdcScaleY,
    kApertureInvSizeX,
    kApertureInvSizeY,
    // Half the diagonal of a pixel in the shape's scaled space, which culling widens bounds by.
    kApertureFootprint,
    kApertureBlades,
    kApertureRoundness,
    kApertureShapeCos,
    kApertureShapeSin,
    kApertureShapeEdge,
    kApertureShapeInvSpan,

    kApertureGratingStrength,
    kApertureGratingCount,
    kApertureGratingHalfW,
    kApertureGratingHalfL,
    kApertureGratingEdge,
    kApertureGratingInvSpan,
    // The *Bound words are squared distances: an entry whose centre is farther than its layer's bound
    // from a pixel's centre covers none of the pixel, so skipping it leaves the sum exactly as it was.
    kApertureGratingBound,

    kApertureScratchesStrength,
    kApertureScratchesCount,
    kApertureScratchesHalfW,
    kApertureScratchesHalfL,
    kApertureScratchesEdge,
    kApertureScratchesInvSpan,
    kApertureScratchesBound,
    kApertureScratchesCos,
    kApertureScratchesSin,

    kApertureDustStrength,
    kApertureDustCount,
    kApertureDustRadius,
    kApertureDustRadiusSquared,
    kApertureDustEdge,
    kApertureDustInvSpan,
    kApertureDustBound,

    kApertureImageStrength,
    kApertureImageWidth,
    kApertureImageHeight,
    kApertureImageScaleX,
    kApertureImageScaleY,
    kApertureImageBiasX,
    kApertureImageBiasY,
    kApertureImageBlack,
    kApertureImageWhite,

    kApertureBladeOffset,
    kApertureGratingOffset,
    kApertureScratchesOffset,
    kApertureDustOffset,

    kApertureHeaderWords
};

// Each blade: its axis (x, y).
inline constexpr uint32_t kApertureBladeWords = 2;
// Each grating bar: cos, sin of its angle, then the bar's centre in the shape's scaled space (x, y).
inline constexpr uint32_t kApertureGratingWords = 4;
// Each scratch: its centre (x, y), cos, sin of its own angle, then its centre in the scratches'
// rotated space (x, y).
inline constexpr uint32_t kApertureScratchWords = 6;
// Each dust particle: its centre (x, y).
inline constexpr uint32_t kApertureDustWords = 2;

inline constexpr int kApertureSamples = 8;
inline constexpr uint32_t kApertureMaxGratingBars = 360;
inline constexpr uint32_t kApertureMaxScratches = 1000;
inline constexpr uint32_t kApertureMaxDust = 1000;

/**
 * @brief The tables of params over frame, the image layer reading an image of imageWidth x
 * imageHeight; no image (a size of 0) turns the image layer off. Empty when the frame is.
 **/
std::vector<uint32_t> buildApertureTables(const ApertureParams& params,
                                          const ApertureFrame& frame,
                                          int imageWidth,
                                          int imageHeight);

/**
 * @brief Rasterises rows [y1, y2) of the mask the tables describe, 8 x 8 supersampled, into out,
 * whose rows are outStride floats apart, the first row written being y1's. The image must be the
 * size the tables were built for.
 **/
void rasteriseApertureRows(const std::vector<uint32_t>& tables,
                           const ApertureImage& image,
                           int y1,
                           int y2,
                           float* out,
                           std::size_t outStride);

/**
 * @brief The whole mask of params over frame into out (frame.width x frame.height floats, rows packed
 * from the bottom), on up to maxThreads threads of the global pool. Rows stop starting once aborted
 * returns true, and the function then returns false with the mask incomplete.
 **/
bool rasteriseAperture(const ApertureParams& params,
                       const ApertureFrame& frame,
                       const ApertureImage& image,
                       float* out,
                       int maxThreads,
                       const std::function<bool()>& aborted = std::function<bool()>());

} // namespace Optics

NATRON_NAMESPACE_EXIT

#endif // Engine_Optics_Aperture_h
