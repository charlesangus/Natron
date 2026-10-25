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

#ifndef Engine_Nodes_Raw_RawGpuParams_h
#define Engine_Nodes_Raw_RawGpuParams_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstdint>

#include "Engine/Nodes/IO/RawMetadata.h"

NATRON_NAMESPACE_ENTER

/**
 * The parameter blocks of the raw develop kernels, each mirrored by a struct of the Slang module
 * named beside it, in Engine/Gpu/Kernels. A block is read in 32-bit words, so every member is one
 * word or an array of them.
 **/

/**
 * @brief A RawLayout in a fixed size, whatever the file: period, origin, and the period x period
 * colours (0 red, 1 green, 2 blue) row by row from the origin, four 8-bit cells to a word, the
 * first in the low byte. Mirrors RawLayout in rawlayout.slang; a raw op's block starts with it.
 **/
struct RawLayoutGpu {
    std::int32_t period;
    std::int32_t originX;
    std::int32_t originY;
    std::uint32_t reserved;
    std::uint32_t cells[(RawLayout::kMaxPeriod * RawLayout::kMaxPeriod + 3) / 4];
    std::uint32_t padding[3];
};
static_assert(sizeof(RawLayoutGpu) == 64);

inline RawLayoutGpu
toRawLayoutGpu(const RawLayout& layout)
{
    RawLayoutGpu g = {};
    const int period = layout.period();

    g.period = period;
    g.originX = layout.originX();
    g.originY = layout.originY();
    for (int row = 0; row < period; ++row) {
        for (int col = 0; col < period; ++col) {
            const int i = row * period + col;
            const std::uint32_t colour = (std::uint32_t)layout.colourAt(layout.originX() + col, layout.originY() + row);
            g.cells[i >> 2] |= (colour & 0xffu) << ((i & 3) * 8);
        }
    }

    return g;
}

/**
 * @brief A RawLayoutGpu at origin (0, 0) of the period x period colours, colours[row * period +
 * col] being that of photosite (col, row). A colour may be any 8-bit value, as VNG4's colour 3.
 **/
inline RawLayoutGpu
toRawLayoutGpu(int period,
               const std::uint8_t* colours)
{
    RawLayoutGpu g = {};

    g.period = period;
    for (int i = 0; i < period * period; ++i) {
        g.cells[i >> 2] |= (std::uint32_t)colours[i] << ((i & 3) * 8);
    }

    return g;
}

// Mirrors PrepareOp in rawprepare.slang.
struct RawPrepareGpuParams {
    RawLayoutGpu layout;
    float sub[4];
    float div[4];
    std::uint32_t hasCfa;
    std::uint32_t padding[3];
};
static_assert(sizeof(RawPrepareGpuParams) == 112);

// Mirrors TemperatureOp in rawprepare.slang.
struct RawTemperatureGpuParams {
    RawLayoutGpu layout;
    float coeffs[3];
    std::uint32_t padding;
};
static_assert(sizeof(RawTemperatureGpuParams) == 80);

// Mirrors HighlightsClipOp and HighlightsLChOp in rawhighlights.slang. rodX1 to rodY2 are the
// source's region of definition, which LCh counts its rows and columns in.
struct RawHighlightsGpuParams {
    RawLayoutGpu layout;
    float clip;
    std::int32_t rodX1;
    std::int32_t rodY1;
    std::int32_t rodX2;
    std::int32_t rodY2;
    std::uint32_t padding[3];
};
static_assert(sizeof(RawHighlightsGpuParams) == 96);

// Mirrors InputColorOp in rawinputcolor.slang: the row-major camera to working space matrix.
struct RawInputColorGpuParams {
    float matrix[9];
    std::uint32_t padding[3];
};
static_assert(sizeof(RawInputColorGpuParams) == 48);

/**
 * @brief Mirrors CalibrationOp in rawcolorcalibration.slang: RawColorCalibrationMath::Kernel with
 * its matrices row-major, plus the adaptation's XYZ to LMS and LMS to XYZ matrices and D50 in its
 * LMS space, which the CPU keeps as constants. The matrices are zero for the XYZ and RGB
 * adaptations, which have no LMS space. adaptation and version are the enums' values.
 **/
struct RawColorCalibrationGpuParams {
    float rgbToXyz[9];
    float xyzToRgb[9];
    float rgbToLms[9];
    float mixToXyz[9];
    float xyzToLms[9];
    float lmsToXyz[9];
    float illuminant[3];
    float d50[3];
    float saturation[3];
    float lightness[3];
    float grey[3];
    float p;
    float gamut;
    std::uint32_t adaptation;
    std::uint32_t version;
    std::uint32_t clip;
    std::uint32_t applyGrey;
    std::uint32_t padding;
};
static_assert(sizeof(RawColorCalibrationGpuParams) == 304);

/**
 * @brief The push constants of both kernels of rawhotpixels.slang, which mirrors it as
 * HotPixelsParams. domX1 to domY2 are the CPU's buffer, the photosites the passes run over.
 * rectX1 to rectY2 are, for the fill, the source pixels that exist (empty when there are none)
 * with offset and stride the float index of the first and the row length, and for the detection
 * the rect written with the same two members for the result.
 **/
struct RawHotPixelsGpuParams {
    RawLayoutGpu layout;
    float threshold;
    float multiplier;
    std::uint32_t minNeighbours;
    std::uint32_t channels;
    std::int32_t domX1;
    std::int32_t domY1;
    std::int32_t domX2;
    std::int32_t domY2;
    std::int32_t rectX1;
    std::int32_t rectY1;
    std::int32_t rectX2;
    std::int32_t rectY2;
    std::uint32_t offset;
    std::uint32_t stride;
    std::uint32_t padding[2];
};
static_assert(sizeof(RawHotPixelsGpuParams) == 128);

inline constexpr std::uint32_t kRawDemosaicGpuModePpg = 0;
inline constexpr std::uint32_t kRawDemosaicGpuModeRcd = 1;
inline constexpr std::uint32_t kRawDemosaicGpuModePassThrough = 2;

/**
 * @brief The push constants of every kernel of demosaic_bayer.slang, which mirrors it as
 * DemosaicParams. The kernels work over a top-down buffer of width x height photosites: buffer
 * column 0 is pixel column bufX1 and buffer row 0 pixel row bufY2 - 1. stride is the even row
 * length of the RCD planes. The source view reads zero outside [srcX1, srcX2) x [srcY1, srcY2);
 * the destination holds the outWidth x outHeight RGBA pixels at (outX, outY).
 **/
struct RawDemosaicGpuPush {
    std::int32_t bufX1;
    std::int32_t bufY2;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t stride;
    std::uint32_t filters;
    float threshold;
    float revScaler;
    float scaler;
    std::uint32_t margin;
    std::uint32_t mode;
    std::uint32_t srcOffset;
    std::uint32_t srcStride;
    std::int32_t srcX1;
    std::int32_t srcY1;
    std::int32_t srcX2;
    std::int32_t srcY2;
    std::uint32_t srcComps;
    std::uint32_t dstOffset;
    std::uint32_t dstStride;
    std::int32_t outX;
    std::int32_t outY;
    std::uint32_t outWidth;
    std::uint32_t outHeight;
};
static_assert(sizeof(RawDemosaicGpuPush) == 96);

/**
 * @brief The push constants of every kernel of demosaic_vng.slang, which mirrors it as VngParams.
 * The kernels work over a top-down buffer of width x height photosites whose pattern, as the
 * buffer sees it from its top-left photosite, is layout, VNG4's second green being colour 3.
 * Buffer rows [srcRow1, srcRow2) and columns [srcCol1, srcCol2) are the source's pixels: the
 * source stores its rows bottom-up from srcOffset, so photosite (col, row) is at srcOffset +
 * (srcRow2 - 1 - row) * srcStride + (col - srcCol1) * srcComps. Every other photosite reads
 * zero. The destination holds the outWidth x outHeight RGBA pixels from buffer column outCol,
 * its first row of dstStride floats being buffer row outRow and each next one the row above.
 **/
struct RawVngGpuPush {
    RawLayoutGpu layout;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t srcOffset;
    std::uint32_t srcStride;
    std::uint32_t srcComps;
    std::int32_t srcCol1;
    std::int32_t srcRow1;
    std::int32_t srcCol2;
    std::int32_t srcRow2;
    std::uint32_t dstOffset;
    std::uint32_t dstStride;
    std::int32_t outCol;
    std::int32_t outRow;
    std::uint32_t outWidth;
    std::uint32_t outHeight;
    std::uint32_t padding;
};
static_assert(sizeof(RawVngGpuPush) == 128);

/**
 * @brief The push constants of every kernel of demosaic_markesteijn.slang, which mirrors it as
 * MarkesteijnParams. The buffer, source and output are as RawVngGpuPush's. pattern holds the
 * buffer's 6x6 X-Trans colours from its top-left photosite, 2 bits each, photosite row * 6 + col
 * in bits (i % 16) * 2 of word i / 16. A dispatch works on tileCount of the buffer's tiles from
 * firstTile, numbered as RawKernels::markesteijn() numbers them over nLefts columns of tiles;
 * pass is the pass the per-pass kernels work on.
 **/
struct RawMarkesteijnGpuPush {
    std::uint32_t pattern[3];
    std::uint32_t passes;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t srcOffset;
    std::uint32_t srcStride;
    std::uint32_t srcComps;
    std::int32_t srcCol1;
    std::int32_t srcRow1;
    std::int32_t srcCol2;
    std::int32_t srcRow2;
    std::uint32_t firstTile;
    std::uint32_t nLefts;
    std::uint32_t tileCount;
    std::uint32_t pass;
    std::uint32_t dstOffset;
    std::uint32_t dstStride;
    std::int32_t outCol;
    std::int32_t outRow;
    std::uint32_t outWidth;
    std::uint32_t outHeight;
    std::uint32_t padding[5];
};
static_assert(sizeof(RawMarkesteijnGpuPush) == 112);

/**
 * @brief The push constants of every kernel of demosaic_amaze.slang, which mirrors it as
 * AmazeParams. The buffer, source and output are as RawVngGpuPush's. filters is the buffer's Bayer
 * pattern from its top-left photosite, as RawKernels::amaze() takes it, and clipPt its clip
 * point. A dispatch works on tileCount of the buffer's tiles from firstTile, numbered as
 * RawKernels::amaze() numbers them over nLefts columns of tiles.
 **/
struct RawAmazeGpuPush {
    std::uint32_t filters;
    float clipPt;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t srcOffset;
    std::uint32_t srcStride;
    std::uint32_t srcComps;
    std::int32_t srcCol1;
    std::int32_t srcRow1;
    std::int32_t srcCol2;
    std::int32_t srcRow2;
    std::uint32_t firstTile;
    std::uint32_t nLefts;
    std::uint32_t tileCount;
    std::uint32_t dstOffset;
    std::uint32_t dstStride;
    std::int32_t outCol;
    std::int32_t outRow;
    std::uint32_t outWidth;
    std::uint32_t outHeight;
    std::uint32_t padding[4];
};
static_assert(sizeof(RawAmazeGpuPush) == 96);

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Raw_RawGpuParams_h
