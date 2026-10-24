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
 * A port of darktable's highlight reconstruction module, DARKTABLE_REF release-5.6.2 (commit
 * 20891f6c6fa7), src/iop/highlights.c with src/iop/hlreconstruct/opposed.c, lch.c,
 * inpaint.c and the reference average of segbased.c, the segmentation itself being ported in
 * RawSegmentation.cpp and RawHighlightsSegments.cpp, Copyright (C) 2009-2026 darktable developers. The opposed
 * algorithm was developed by garagecoder and Iain of the G'MIC team with Hanno Schwalm. darktable
 * is distributed under the GNU GPL version 3 or later, so this file is too.
 */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "RawHighlights.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/StagePlan.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/Nodes/Raw/RawGpuParams.h"
#include "Engine/Nodes/Raw/RawSegmentation.h"
#include "Engine/RectD.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {
inline constexpr InputSpec kRawHighlightsInputs[] = {
    InputSpec { .label = "Source", .kind = eDataKindRaw },
};

constexpr NodeSpec kRawHighlightsNodeSpec = {
    .id = PLUGINID_NATRON_RAWHIGHLIGHTS,
    .label = "RawHighlights",
    .description = "Reconstructs the highlights the sensor clipped, before demosaicing: clips them, leaves them, inpaints them "
                   "from the opposed colours around them (the default), or rebuilds them keeping lightness (LCh). Goes after "
                   "RawTemperature.",
    .grouping = PLUGIN_GROUP_RAW,
    .majorVersion = PLUGIN_MAJOR_NATRON_RAWHIGHLIGHTS,
    .minorVersion = 0,
    .icon = NATRON_IMAGES_PATH "NativeNodes/" PLUGINID_NATRON_RAWHIGHLIGHTS ".png",
    .outputKind = eDataKindRaw,
    .inputs = kRawHighlightsInputs,
};

constexpr ImagePipelineSpec kRawHighlightsPipeline = {
    .layers = LayerPolicy::none(),
    .op = OpKind::Custom,
    .rawStage = RawStageRequirement::Prepared,
};

// darktable's highlights_clip_magics[] for the modes ported here: some algorithms clip below
// the threshold the user sets.
const float kClipMagicClip = 1.0f;
const float kClipMagicLCh = 1.0f;
const float kClipMagicOpposed = 0.987f;
const float kClipMagicInpaint = 1.0f;

const char* const kModeUnavailable = "highlights: mode not available for this type of image. falling back to inpaint opposed.";

struct RecoveryChoice {
    const char* id;
    const char* label;
    RawKernels::SegmentsRecoveryEnum recovery;
};

// darktable's Rebuild menu, in its order, which is not that of the mode values.
const RecoveryChoice kRecoveryChoices[] = {
    { "off", "Off", RawKernels::eSegmentsRecoveryOff },
    { "generic", "Generic", RawKernels::eSegmentsRecoveryAdapt },
    { "flatGeneric", "Flat Generic", RawKernels::eSegmentsRecoveryAdaptFlat },
    { "small", "Small Segments", RawKernels::eSegmentsRecoverySmall },
    { "large", "Large Segments", RawKernels::eSegmentsRecoveryLarge },
    { "flatSmall", "Flat Small Segments", RawKernels::eSegmentsRecoverySmallFlat },
    { "flatLarge", "Flat Large Segments", RawKernels::eSegmentsRecoveryLargeFlat },
};

RawKernels::SegmentsRecoveryEnum
recoveryFromIndex(int index)
{
    const int count = (int)(sizeof(kRecoveryChoices) / sizeof(kRecoveryChoices[0]));

    return ((index >= 0) && (index < count)) ? kRecoveryChoices[index].recovery : RawKernels::eSegmentsRecoveryOff;
}

const float kSqrt3 = (float)1.7320508075688772935274463415058723669L;
const float kSqrt12 = (float)3.4641016151377545870548926830117447339L;

inline float
fcube(float a)
{
    return a * a * a;
}

/**
 * @brief The colour of photosite (row, col) of the image counted from its top-left photosite.
 **/
struct CfaLookup {
    int period = 0;
    unsigned char cells[6][6] = {};

    CfaLookup(const RawLayout& layout,
              const RectI& rod)
        : period(layout.period())
    {
        for (int r = 0; r < period; ++r) {
            for (int c = 0; c < period; ++c) {
                cells[r][c] = (unsigned char)layout.colourAt(rod.x1 + c, rod.y2 - 1 - r);
            }
        }
    }

    int at(int row,
           int col) const
    {
        return cells[row % period][col % period];
    }
};

/**
 * @brief Pixels read with image coordinates: (row, col) counted from the top-left of the
 * image's region of definition, from a top-down buffer or in place from a source image.
 **/
struct ImageView {
    const float* base = nullptr;
    std::ptrdiff_t rowStride = 0;
    int nComps = 0;
    int row0 = 0;
    int col0 = 0;
    int imageWidth = 0;
    int imageHeight = 0;

    ImageView() = default;

    ImageView(const RawNodeSupport::TopDownBuffer& b,
              const RectI& rod)
        : base(b.data.data())
        , rowStride((std::ptrdiff_t)b.width() * b.nComps)
        , nComps(b.nComps)
        , row0(rod.y2 - b.rect.y2)
        , col0(b.rect.x1 - rod.x1)
        , imageWidth(rod.width())
        , imageHeight(rod.height())
    {
    }

    /**
     * @brief A view of rod read in place from source, or nullopt when the source does not hold
     * all of rod.
     **/
    static std::optional<ImageView> inPlace(const RawNodeSupport::LockedSource& source,
                                            const RectI& rod)
    {
        if (!source.access || rod.isNull() || !source.bounds.contains(rod)) {
            return std::nullopt;
        }
        const float* top = (const float*)source.access->pixelAt(rod.x1, rod.y2 - 1);
        if (!top) {
            return std::nullopt;
        }
        std::ptrdiff_t stride = 0;
        if (rod.height() > 1) {
            const float* next = (const float*)source.access->pixelAt(rod.x1, rod.y2 - 2);
            if (!next) {
                return std::nullopt;
            }
            stride = next - top;
        }
        ImageView view;
        view.base = top;
        view.rowStride = stride;
        view.nComps = source.nComps;
        view.imageWidth = rod.width();
        view.imageHeight = rod.height();

        return view;
    }

    const float* pixel(int row,
                       int col) const
    {
        return base + (std::ptrdiff_t)(row - row0) * rowStride + (std::ptrdiff_t)(col - col0) * nComps;
    }

    float at(int row,
             int col) const
    {
        return *pixel(row, col);
    }
};

/**
 * @brief The whole of rod read in place from source: a copy would hold a second whole frame
 * during the computation. A source that does not hold the whole region is copied into buffer
 * instead, with zeros where it has no pixels.
 **/
void
viewWholeSource(const RawNodeSupport::LockedSource& source,
                const RectI& rod,
                const RawKernels::ParallelFor& parallel,
                std::optional<ImageView>* view,
                RawNodeSupport::TopDownBuffer* buffer)
{
    *view = ImageView::inPlace(source, rod);
    if (!*view) {
        buffer->allocate(rod, source.nComps);
        RawNodeSupport::readTopDown(source, buffer, parallel);
        view->emplace(*buffer, rod);
    }
}

// segbased.c's _calc_refavg() with linear output and no late correction.
float
calcRefavg(const ImageView& in,
           const CfaLookup& cfa,
           int row,
           int col)
{
    const int color = cfa.at(row, col);
    float mean[4] = { 0.f, 0.f, 0.f, 0.f };
    float cnt[4] = { 0.f, 0.f, 0.f, 0.f };

    const int dymin = std::max(0, row - 1);
    const int dxmin = std::max(0, col - 1);
    const int dymax = std::min(in.imageHeight - 1, row + 2);
    const int dxmax = std::min(in.imageWidth - 1, col + 2);

    for (int dy = dymin; dy < dymax; ++dy) {
        for (int dx = dxmin; dx < dxmax; ++dx) {
            const float val = std::fmax(0.0f, in.at(dy, dx));
            const int c = cfa.at(dy, dx);
            mean[c] += val;
            cnt[c] += 1.0f;
        }
    }
    for (int c = 0; c < 3; ++c) {
        mean[c] = (cnt[c] > 0.0f) ? std::cbrt((1.0f * mean[c]) / cnt[c]) : 0.0f;
    }

    const float croot_refavg[3] = { 0.5f * (mean[1] + mean[2]),
                                    0.5f * (mean[0] + mean[2]),
                                    0.5f * (mean[0] + mean[1]) };

    return fcube(croot_refavg[color]);
}

float
calcLinearRefavg(const float* in,
                 int color)
{
    const float ins[3] = { std::cbrt(std::fmax(0.0f, in[0])), std::cbrt(std::fmax(0.0f, in[1])), std::cbrt(std::fmax(0.0f, in[2])) };
    const float opp[3] = { 0.5f * (ins[1] + ins[2]), 0.5f * (ins[0] + ins[2]), 0.5f * (ins[0] + ins[1]) };

    return fcube(opp[color]);
}

inline std::size_t
rawToCmap(std::size_t width,
          std::size_t row,
          std::size_t col)
{
    return (row / 3) * width + (col / 3);
}

inline std::size_t
roundSize(std::size_t size,
          std::size_t alignment)
{
    return ((size % alignment) == 0) ? size : (size - 1 + alignment - ((size - 1) % alignment));
}

char
maskDilated(const char* in,
            std::size_t w1)
{
    if (in[0]) {
        return 1;
    }
    if (in[-(std::ptrdiff_t)w1 - 1] | in[-(std::ptrdiff_t)w1] | in[-(std::ptrdiff_t)w1 + 1] | in[-1] | in[1] | in[w1 - 1] | in[w1] | in[w1 + 1]) {
        return 1;
    }
    const std::ptrdiff_t W1 = (std::ptrdiff_t)w1;
    const std::ptrdiff_t W2 = 2 * W1;
    const std::ptrdiff_t W3 = 3 * W1;

    return (in[-W3 - 2] | in[-W3 - 1] | in[-W3] | in[-W3 + 1] | in[-W3 + 2] | in[-W2 - 3] | in[-W2 - 2] | in[-W2 - 1] | in[-W2] | in[-W2 + 1] | in[-W2 + 2] | in[-W2 + 3] | in[-W1 - 3] | in[-W1 - 2] | in[-W1 + 2] | in[-W1 + 3] | in[-3] | in[-2] | in[2] | in[3] | in[W1 - 3] | in[W1 - 2] | in[W1 + 2] | in[W1 + 3] | in[W2 - 3] | in[W2 - 2] | in[W2 - 1] | in[W2] | in[W2 + 1] | in[W2 + 2] | in[W2 + 3] | in[W3 - 2] | in[W3 - 1] | in[W3] | in[W3 + 1] | in[W3 + 2]) ? 1 : 0;
}

// The chrominance offsets of _process_opposed() over the whole mosaic.
std::array<float, 3>
opposedChroma(const ImageView& in,
              const CfaLookup& cfa,
              const float clips[3],
              const RawKernels::ParallelFor& parallel)
{
    const std::size_t width = (std::size_t)in.imageWidth;
    const std::size_t height = (std::size_t)in.imageHeight;
    const std::size_t mwidth = width / 3;
    const std::size_t mheight = height / 3;
    const std::size_t msize = roundSize(mwidth, 8) * roundSize(mheight, 8);
    std::array<float, 3> chrominance = { 0.f, 0.f, 0.f };

    if ((mwidth < 1) || (mheight < 1)) {
        return chrominance;
    }
    // The last row and column of photosites past the last whole 3x3 block map one block row and
    // one block further, as in darktable, which reads up to mwidth + 1 past the last plane when no
    // rounding pads it: those reads see zeros here.
    std::vector<char> mask(6 * msize + mwidth + 1, 0);
    std::vector<char> clippedRows(mheight, 0);
    parallel((int)mheight - 1, [&](int mrow) {
        for (std::size_t mcol = 0; mcol < mwidth - 1; ++mcol) {
            char mbuff[3] = { 0, 0, 0 };
            for (int y = 0; y < 3; ++y) {
                for (int x = 0; x < 3; ++x) {
                    const int row = 3 * mrow + y;
                    const int col = 3 * (int)mcol + x;
                    const int color = cfa.at(row, col);
                    mbuff[color] += (in.at(row, col) >= clips[color]) ? 1 : 0;
                }
            }
            for (int c = 0; c < 3; ++c) {
                mask[c * msize + mrow * mwidth + mcol] = mbuff[c] ? 1 : 0;
                clippedRows[mrow] |= mbuff[c] ? 1 : 0;
            }
        }
    });
    const bool anyClipped = std::find(clippedRows.begin(), clippedRows.end(), 1) != clippedRows.end();
    if (!anyClipped) {
        return chrominance;
    }

    parallel((int)mheight, [&](int r) {
        const std::size_t row = (std::size_t)r;
        for (std::size_t col = 0; col < mwidth; ++col) {
            const std::size_t mx = row * mwidth + col;
            const bool safe = (col >= 3) && (row >= 3) && (col + 4 < mwidth) && (row + 4 < mheight);
            mask[3 * msize + mx] = safe ? maskDilated(&mask[mx], mwidth) : mask[mx];
            mask[4 * msize + mx] = safe ? maskDilated(&mask[msize + mx], mwidth) : mask[mx + msize];
            mask[5 * msize + mx] = safe ? maskDilated(&mask[2 * msize + mx], mwidth) : mask[mx + 2 * msize];
        }
    });

    const float loClips[3] = { 0.2f * clips[0], 0.2f * clips[1], 0.2f * clips[2] };
    std::vector<double> sums(3 * height, 0.);
    std::vector<double> cnts(3 * height, 0.);
    parallel((int)height, [&](int row) {
        for (std::size_t col = 0; col < width; ++col) {
            const int color = cfa.at(row, (int)col);
            const float inval = in.at(row, (int)col);
            // Only unclipped photosites right next to clipped ones measure the offset.
            if ((inval < clips[color]) && (inval > loClips[color]) && mask[(color + 3) * msize + rawToCmap(mwidth, row, col)]) {
                sums[3 * row + color] += inval - calcRefavg(in, cfa, row, (int)col);
                cnts[3 * row + color] += 1.0;
            }
        }
    });
    double sum[3] = { 0., 0., 0. };
    double cnt[3] = { 0., 0., 0. };
    for (std::size_t row = 0; row < height; ++row) {
        for (int c = 0; c < 3; ++c) {
            sum[c] += sums[3 * row + c];
            cnt[c] += cnts[3 * row + c];
        }
    }
    for (int c = 0; c < 3; ++c) {
        chrominance[c] = (cnt[c] > 100.0) ? (float)(sum[c] / cnt[c]) : 0.0f;
    }

    return chrominance;
} // opposedChroma

// The chrominance offsets of _process_linear_opposed() over the whole image.
std::array<float, 3>
linearOpposedChroma(const ImageView& in,
                    const float clips[3],
                    const RawKernels::ParallelFor& parallel)
{
    const std::size_t width = (std::size_t)in.imageWidth;
    const std::size_t height = (std::size_t)in.imageHeight;
    const std::size_t mwidth = width / 3;
    const std::size_t mheight = height / 3;
    const std::size_t msize = roundSize(mwidth, 8) * roundSize(mheight, 8);
    std::array<float, 3> chrominance = { 0.f, 0.f, 0.f };

    if ((width < 7) || (height < 7) || (mwidth < 7) || (mheight < 7)) {
        return chrominance;
    }
    // Padded as in opposedChroma().
    std::vector<char> mask(6 * msize + mwidth + 1, 0);
    bool anyClipped = false;
    for (std::size_t row = 0; row < height - 1; ++row) {
        for (std::size_t col = 0; col < width - 1; ++col) {
            const float* pixel = in.pixel((int)row, (int)col);
            const std::size_t mdx = rawToCmap(mwidth, row, col);
            for (int c = 0; c < 3; ++c) {
                // darktable tests the first channel for every colour here; kept for parity.
                if ((pixel[0] >= clips[c]) && (mask[c * msize + mdx] == 0)) {
                    mask[c * msize + mdx] |= 1;
                    anyClipped = true;
                }
            }
        }
    }
    if (!anyClipped) {
        return chrominance;
    }
    for (std::size_t row = 3; row < mheight - 3; ++row) {
        for (std::size_t col = 3; col < mwidth - 3; ++col) {
            const std::size_t mx = row * mwidth + col;
            mask[3 * msize + mx] = maskDilated(&mask[mx], mwidth);
            mask[4 * msize + mx] = maskDilated(&mask[msize + mx], mwidth);
            mask[5 * msize + mx] = maskDilated(&mask[2 * msize + mx], mwidth);
        }
    }

    std::vector<double> sums(3 * height, 0.);
    std::vector<double> cnts(3 * height, 0.);
    parallel((int)height - 6, [&](int r) {
        const std::size_t row = (std::size_t)r + 3;
        for (std::size_t col = 3; col < width - 3; ++col) {
            const float* pixel = in.pixel((int)row, (int)col);
            for (int c = 0; c < 3; ++c) {
                const float inval = pixel[c];
                if ((inval > 0.2f * clips[c]) && (inval < clips[c]) && mask[(c + 3) * msize + rawToCmap(mwidth, row, col)]) {
                    sums[3 * row + c] += inval - calcLinearRefavg(pixel, c);
                    cnts[3 * row + c] += 1.0;
                }
            }
        }
    });
    double sum[3] = { 0., 0., 0. };
    double cnt[3] = { 0., 0., 0. };
    for (std::size_t row = 0; row < height; ++row) {
        for (int c = 0; c < 3; ++c) {
            sum[c] += sums[3 * row + c];
            cnt[c] += cnts[3 * row + c];
        }
    }
    for (int c = 0; c < 3; ++c) {
        chrominance[c] = (cnt[c] > 30.0) ? (float)(sum[c] / cnt[c]) : 0.0f;
    }

    return chrominance;
} // linearOpposedChroma

// process_lch_bayer() for photosite (j, i) of the image.
float
lchBayer(const ImageView& in,
         const CfaLookup& cfa,
         int j,
         int i,
         float clip)
{
    const float v = in.at(j, i);

    if ((i == in.imageWidth - 1) || (j == in.imageHeight - 1)) {
        return std::min(clip, v);
    }
    bool clipped = false;
    float R = 0.0f, Gmin = FLT_MAX, Gmax = -FLT_MAX, B = 0.0f;
    for (int jj = 0; jj <= 1; ++jj) {
        for (int ii = 0; ii <= 1; ++ii) {
            const float val = in.at(j + jj, i + ii);
            clipped = (clipped || (val > clip));
            switch (cfa.at(j + jj, i + ii)) {
            case 0:
                R = val;
                break;
            case 1:
                Gmin = std::min(Gmin, val);
                Gmax = std::max(Gmax, val);
                break;
            case 2:
                B = val;
                break;
            }
        }
    }
    if (!clipped) {
        return v;
    }
    const float Ro = std::min(R, clip);
    const float Go = std::min(Gmin, clip);
    const float Bo = std::min(B, clip);

    const float L = (R + Gmax + B) / 3.0f;

    float C = kSqrt3 * (R - Gmax);
    float H = 2.0f * B - Gmax - R;

    const float Co = kSqrt3 * (Ro - Go);
    const float Ho = 2.0f * Bo - Go - Ro;

    if ((R != Gmax) && (Gmax != B)) {
        const float ratio = std::sqrt((Co * Co + Ho * Ho) / (C * C + H * H));
        C *= ratio;
        H *= ratio;
    }

    const float RGB[3] = { L - H / 6.0f + C / kSqrt12, L - H / 6.0f - C / kSqrt12, L + H / 3.0f };

    return RGB[cfa.at(j, i)];
}

// process_lch_xtrans() for photosite (j, i) of the image. darktable's ring buffer of clipped
// columns covers columns i - 2 to i of rows j - 1 to j + 1, which is evaluated here directly.
float
lchXTrans(const ImageView& in,
          const CfaLookup& cfa,
          int j,
          int i,
          float clip)
{
    const float v = in.at(j, i);

    if ((i < 2) || (i > in.imageWidth - 3) || (j < 2) || (j > in.imageHeight - 3)) {
        return std::min(clip, v);
    }
    bool clipped = v > clip;
    if (!clipped) {
        bool ring = false;
        for (int ii = i - 2; ii <= i; ++ii) {
            ring = ring || (in.at(j - 1, ii) > clip) || (in.at(j, ii) > clip) || (in.at(j + 1, ii) > clip);
        }
        clipped = ring;
        if (clipped) {
            // Reconstruct only when no 3x3 block touching the photosite is free of clipping,
            // which avoids zippering at the edge of clipped areas.
            for (int offsetJ = -2; offsetJ <= 0; ++offsetJ) {
                for (int offsetI = -2; offsetI <= 0; ++offsetI) {
                    if (clipped) {
                        clipped = false;
                        for (int jj = offsetJ; jj <= offsetJ + 2; ++jj) {
                            for (int ii = offsetI; ii <= offsetI + 2; ++ii) {
                                clipped = (clipped || (in.at(j + jj, i + ii) > clip));
                            }
                        }
                    }
                }
            }
        }
    }
    if (!clipped) {
        return v;
    }
    float mean[3] = { 0.0f, 0.0f, 0.0f };
    float RGBmax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    int cnt[3] = { 0, 0, 0 };
    for (int jj = -1; jj <= 1; ++jj) {
        for (int ii = -1; ii <= 1; ++ii) {
            const float val = in.at(j + jj, i + ii);
            const int c = cfa.at(j + jj, i + ii);
            mean[c] += val;
            cnt[c]++;
            RGBmax[c] = std::max(RGBmax[c], val);
        }
    }

    const float Ro = std::min(mean[0] / cnt[0], clip);
    const float Go = std::min(mean[1] / cnt[1], clip);
    const float Bo = std::min(mean[2] / cnt[2], clip);

    const float R = RGBmax[0];
    const float G = RGBmax[1];
    const float B = RGBmax[2];

    const float L = (R + G + B) / 3.0f;

    float C = kSqrt3 * (R - G);
    float H = 2.0f * B - G - R;

    const float Co = kSqrt3 * (Ro - Go);
    const float Ho = 2.0f * Bo - Go - Ro;

    if ((R != G) && (G != B)) {
        const float ratio = std::sqrt((Co * Co + Ho * Ho) / (C * C + H * H));
        C *= ratio;
        H *= ratio;
    }

    const float RGB[3] = { L - H / 6.0f + C / kSqrt12, L - H / 6.0f - C / kSqrt12, L + H / 3.0f };

    return RGB[cfa.at(j, i)];
} // lchXTrans
} // namespace

RawHighlightsRequirementEnum
rawHighlightsModeRequirement(RawHighlightsModeEnum mode)
{
    switch (mode) {
    case eRawHighlightsModeLCh:
    case eRawHighlightsModeInpaint:
    case eRawHighlightsModeSegments:
        return eRawHighlightsRequiresCfa;
    case eRawHighlightsModeClip:
    case eRawHighlightsModeUnclipped:
    case eRawHighlightsModeOpposed:
        break;
    }

    return eRawHighlightsRequiresAny;
}

bool
rawHighlightsModeAvailable(RawHighlightsModeEnum mode,
                           const RawLayout& layout)
{
    switch (rawHighlightsModeRequirement(mode)) {
    case eRawHighlightsRequiresCfa:
        return layout.hasCfa();
    case eRawHighlightsRequiresBayer:
        return layout.period() == 2;
    case eRawHighlightsRequiresAny:
        break;
    }

    return true;
}

RawHighlightsModeEnum
sanitiseRawHighlightsMode(RawHighlightsModeEnum mode,
                          const RawLayout& layout,
                          bool* fellBack)
{
    *fellBack = !rawHighlightsModeAvailable(mode, layout);

    return *fellBack ? eRawHighlightsModeOpposed : mode;
}

struct RawHighlights::Settings {
    RawHighlightsModeEnum mode = eRawHighlightsModeOpposed;
    bool fellBack = false;
    float clip = 1.f;
    RawLayout layout;
    std::array<float, 3> coeffs = { 1.f, 1.f, 1.f };
    RectI sourceRoD;
    int combine = 2;
    float candidating = 0.4f;
    RawKernels::SegmentsRecoveryEnum recovery = RawKernels::eSegmentsRecoveryOff;
    float strength = 0.f;
    float noiseLevel = 0.f;
};

RawHighlights::RawHighlights(NodePtr node)
    : StagedImageEffect(node)
    , _mode()
    , _clip()
    , _clipOverride()
    , _combine()
    , _candidating()
    , _recovery()
    , _strength()
    , _noiseLevel()
    , _message()
    , _chromaMutex()
    , _chromaValid(false)
    , _chromaKey(0)
    , _chroma { 0.f, 0.f, 0.f }
    , _frameMutex()
    , _frameValid(false)
    , _frameKey(0)
    , _frame()
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

RawHighlights::~RawHighlights()
{
}

const NodeSpec*
RawHighlights::nodeSpec() const
{
    return staticNodeSpec<kRawHighlightsNodeSpec>();
}

const ImagePipelineSpec&
RawHighlights::imagePipelineSpec() const
{
    return staticImagePipelineSpec<kRawHighlightsPipeline, kRawHighlightsNodeSpec>();
}

void
RawHighlights::initializeKnobs()
{
    KnobPagePtr page = createControlsPage();

    KnobChoicePtr mode = createKnob<KnobChoice>(tr("Method"));
    mode->setName(kRawHighlightsParamMode);
    mode->setHintToolTip(tr("How clipped photosites are reconstructed."));
    std::vector<ChoiceOption> modes;
    modes.push_back(ChoiceOption("clip", tr("Clip Highlights").toStdString(), tr("Clips every photosite to the threshold, which makes clipped areas neutral.").toStdString()));
    modes.push_back(ChoiceOption("unclipped", tr("Unclipped").toStdString(), tr("Leaves the data as it is.").toStdString()));
    modes.push_back(ChoiceOption("opposed", tr("Inpaint Opposed").toStdString(), tr("Rebuilds a clipped photosite from the opposed colours around it, corrected by the chrominance measured next to the clipped areas of the whole image.").toStdString()));
    modes.push_back(ChoiceOption("lch", tr("Reconstruct in LCh").toStdString(), tr("Rebuilds clipped photosites keeping their lightness and scaling their chroma. Inpaint Opposed on data without a colour filter array.").toStdString()));
    modes.push_back(ChoiceOption("inpaint", tr("Reconstruct Color").toStdString(), tr("Rebuilds a clipped photosite from the next photosite of another colour along its row and column, by the colour ratios of the unclipped photosites before it. Inpaint Opposed on data without a colour filter array.").toStdString()));
    modes.push_back(ChoiceOption("segments", tr("Segmentation Based").toStdString(), tr("Inpaint Opposed, then rebuilds each clipped segment of a colour from a candidate found next to it, and can rebuild the areas where every colour is clipped. Inpaint Opposed on data without a colour filter array.").toStdString()));
    mode->populateChoices(modes);
    mode->setDefaultValue((int)eRawHighlightsModeOpposed);
    mode->setAnimationEnabled(false);
    page->addKnob(mode);
    _mode = mode;

    KnobDoublePtr clip = createKnob<KnobDouble>(tr("Clipping Threshold"));
    clip->setName(kRawHighlightsParamClip);
    clip->setHintToolTip(tr("The fraction of the white level above which a photosite counts as clipped. Until overridden it shows the limit the raw file says the sensor responds linearly up to, or 1."));
    clip->setDefaultValue(1.);
    clip->setMinimum(0.);
    clip->setMaximum(2.);
    clip->setDisplayMinimum(0.);
    clip->setDisplayMaximum(2.);
    page->addKnob(clip);
    _clip = clip;

    KnobBoolPtr clipOverride = createKnob<KnobBool>(tr("Override Clipping Threshold"));
    clipOverride->setName(kRawHighlightsParamClipOverride);
    clipOverride->setHintToolTip(tr("Use the clipping threshold above instead of the one the raw file gives."));
    clipOverride->setDefaultValue(false);
    clipOverride->setAnimationEnabled(false);
    page->addKnob(clipOverride);
    _clipOverride = clipOverride;

    KnobIntPtr combine = createKnob<KnobInt>(tr("Combine"));
    combine->setName(kRawHighlightsParamCombine);
    combine->setHintToolTip(tr("Combine closely related clipped segments by morphological operations.\nThis often leads to improved color reconstruction for tiny segments before dark background."));
    combine->setDefaultValue(2);
    combine->setMinimum(0);
    combine->setMaximum(8);
    combine->setDisplayMinimum(0);
    combine->setDisplayMaximum(8);
    page->addKnob(combine);
    _combine = combine;

    KnobDoublePtr candidating = createKnob<KnobDouble>(tr("Candidating"));
    candidating->setName(kRawHighlightsParamCandidating);
    candidating->setHintToolTip(tr("Select inpainting after segmentation analysis.\nIncrease to favor candidates found in segmentation analysis, decrease for opposed means inpainting."));
    candidating->setDefaultValue(0.4);
    candidating->setMinimum(0.);
    candidating->setMaximum(1.);
    candidating->setDisplayMinimum(0.);
    candidating->setDisplayMaximum(1.);
    page->addKnob(candidating);
    _candidating = candidating;

    KnobChoicePtr recovery = createKnob<KnobChoice>(tr("Rebuild"));
    recovery->setName(kRawHighlightsParamRecovery);
    recovery->setHintToolTip(tr("Approximate lost data in regions with all photosites clipped, the effect depends on segment size and border gradients.\n"
                                "Choose a mode tuned for segment size or the generic mode that tries to find best settings for every segment.\n"
                                "Small means areas with a diameter less than 25 pixels, large is best for greater than 100.\n"
                                "The flat modes ignore narrow unclipped structures (like powerlines) to keep highlights rebuilt and avoid gradients."));
    std::vector<ChoiceOption> recoveries;
    for (const RecoveryChoice& choice : kRecoveryChoices) {
        recoveries.push_back(ChoiceOption(choice.id, tr(choice.label).toStdString(), std::string()));
    }
    recovery->populateChoices(recoveries);
    recovery->setDefaultValue(0);
    recovery->setAnimationEnabled(false);
    page->addKnob(recovery);
    _recovery = recovery;

    KnobDoublePtr strength = createKnob<KnobDouble>(tr("Strength"));
    strength->setName(kRawHighlightsParamStrength);
    strength->setHintToolTip(tr("Set strength of rebuilding in regions with all photosites clipped."));
    strength->setDefaultValue(0.);
    strength->setMinimum(0.);
    strength->setMaximum(1.);
    strength->setDisplayMinimum(0.);
    strength->setDisplayMaximum(1.);
    page->addKnob(strength);
    _strength = strength;

    KnobDoublePtr noiseLevel = createKnob<KnobDouble>(tr("Noise Level"));
    noiseLevel->setName(kRawHighlightsParamNoiseLevel);
    noiseLevel->setHintToolTip(tr("Add noise to visually blend the reconstructed areas\ninto the rest of the noisy image. Useful at high ISO."));
    noiseLevel->setDefaultValue(0.);
    noiseLevel->setMinimum(0.);
    noiseLevel->setMaximum(0.5);
    noiseLevel->setDisplayMinimum(0.);
    noiseLevel->setDisplayMaximum(0.5);
    page->addKnob(noiseLevel);
    _noiseLevel = noiseLevel;

    refreshOverride();
    refreshSegmentsVisibility();
}

// darktable's gui_changed(): the segmentation knobs follow the requested method, so they stay put
// while a source without a CFA makes the node fall back.
void
RawHighlights::refreshSegmentsVisibility()
{
    const KnobChoicePtr mode = _mode.lock();
    const KnobChoicePtr recovery = _recovery.lock();

    if (!mode || !recovery) {
        return;
    }
    const bool useSegmentation = (mode->getValue() == (int)eRawHighlightsModeSegments);
    const bool useRecovery = useSegmentation && (recoveryFromIndex(recovery->getValue()) != RawKernels::eSegmentsRecoveryOff);
    if (const KnobIntPtr knob = _combine.lock()) {
        knob->setSecret(!useSegmentation);
    }
    if (const KnobDoublePtr knob = _candidating.lock()) {
        knob->setSecret(!useSegmentation);
    }
    recovery->setSecret(!useSegmentation);
    if (const KnobDoublePtr knob = _strength.lock()) {
        knob->setSecret(!useRecovery);
    }
    if (const KnobDoublePtr knob = _noiseLevel.lock()) {
        knob->setSecret(!useRecovery);
    }
}

void
RawHighlights::refreshOverride()
{
    const KnobBoolPtr overrideKnob = _clipOverride.lock();
    const KnobDoublePtr clip = _clip.lock();

    if (overrideKnob && clip) {
        clip->setAllDimensionsEnabled(overrideKnob->getValue());
    }
}

void
RawHighlights::showSourceClip(double time)
{
    const KnobBoolPtr overrideKnob = _clipOverride.lock();
    const KnobDoublePtr clip = _clip.lock();

    if (!overrideKnob || !clip || overrideKnob->getValue() || !getInput(getSourceInput())) {
        return;
    }
    clip->setValue(std::min(1., RawMetadata::getLinearResponseLimit(getSourceMetadata(time, ViewIdx(0)))));
}

void
RawHighlights::refreshModeWarning(double time)
{
    const KnobChoicePtr mode = _mode.lock();

    if (!mode || !getInput(getSourceInput())) {
        _message.withdraw(getNode());

        return;
    }
    const int value = mode->getValue();
    const RawLayout layout = RawLayout::fromMetadata(getSourceMetadata(time, ViewIdx(0))).value_or(RawLayout());
    bool fellBack = false;
    if ((value >= (int)eRawHighlightsModeClip) && (value <= (int)eRawHighlightsModeSegments)) {
        (void)sanitiseRawHighlightsMode((RawHighlightsModeEnum)value, layout, &fellBack);
    }
    if (fellBack) {
        _message.post(getNode(), eMessageTypeWarning, tr(kModeUnavailable).toStdString());
    } else {
        _message.withdraw(getNode());
    }
}

void
RawHighlights::onInputChanged(int /*inputNo*/)
{
    showSourceClip(getCurrentTime());
    refreshModeWarning(getCurrentTime());
}

void
RawHighlights::onKnobsLoaded()
{
    refreshOverride();
    refreshSegmentsVisibility();
    showSourceClip(getCurrentTime());
    refreshModeWarning(getCurrentTime());
}

bool
RawHighlights::knobChanged(KnobI* k,
                           ValueChangedReasonEnum /*reason*/,
                           ViewSpec /*view*/,
                           double time,
                           bool /*originatedFromMainThread*/)
{
    const KnobBoolPtr overrideKnob = _clipOverride.lock();
    const KnobChoicePtr mode = _mode.lock();
    const KnobChoicePtr recovery = _recovery.lock();

    if (mode && (k == mode.get())) {
        refreshSegmentsVisibility();
        refreshModeWarning(time);

        return true;
    }
    if (recovery && (k == recovery.get())) {
        refreshSegmentsVisibility();

        return true;
    }
    if (!overrideKnob || (k != overrideKnob.get())) {
        return false;
    }
    refreshOverride();
    if (!overrideKnob->getValue()) {
        showSourceClip(time);
    }

    return true;
}

std::optional<RawHighlights::Settings>
RawHighlights::resolveSettings(double time,
                               ViewIdx view)
{
    Settings s;

    if (!RawNodeSupport::getInputPixelRoD(*this, getSourceInput(), time, view, &s.sourceRoD)) {
        return std::nullopt;
    }
    const ImageMetadata source = getSourceMetadata(time, view);
    const KnobReader reader(time, view);
    const int mode = reader.value(_mode);
    s.layout = RawLayout::fromMetadata(source).value_or(RawLayout());
    const RawHighlightsModeEnum requested = ((mode >= (int)eRawHighlightsModeClip) && (mode <= (int)eRawHighlightsModeSegments)) ? (RawHighlightsModeEnum)mode : eRawHighlightsModeOpposed;
    s.mode = sanitiseRawHighlightsMode(requested, s.layout, &s.fellBack);
    const double threshold = reader.value(_clipOverride) ? reader.value(_clip) : std::min(1., RawMetadata::getLinearResponseLimit(source));
    s.clip = (float)(threshold * RawMetadata::getWhiteLevel(source).value_or(1.));
    if (const std::optional<std::array<double, 3>> applied = RawMetadata::getWbApplied(source)) {
        for (int c = 0; c < 3; ++c) {
            s.coeffs[c] = (float)(*applied)[c];
        }
    }
    if (s.mode == eRawHighlightsModeSegments) {
        s.combine = reader.value(_combine);
        s.candidating = (float)reader.value(_candidating);
        s.recovery = recoveryFromIndex(reader.value(_recovery));
        s.strength = (float)reader.value(_strength);
        s.noiseLevel = (float)reader.value(_noiseLevel);
    }

    return s;
}

bool
RawHighlights::isOpIdentity(double time,
                            const RenderScale& /*scale*/,
                            const RectI& /*roi*/,
                            ViewIdx view)
{
    const KnobReader reader(time, view);

    return reader.value(_mode) == (int)eRawHighlightsModeUnclipped;
}

void
RawHighlights::getRegionsOfInterest(double time,
                                    const RenderScale& scale,
                                    const RectD& outputRoD,
                                    const RectD& renderWindow,
                                    ViewIdx view,
                                    RoIMap* ret)
{
    StagedImageEffect::getRegionsOfInterest(time, scale, outputRoD, renderWindow, view, ret);

    EffectInstancePtr input = getInput(getSourceInput());
    const std::optional<Settings> settings = resolveSettings(time, view);
    if (!input || !settings) {
        return;
    }
    const double par = getAspectRatio(getSourceInput());
    const bool lchOnCfa = (settings->mode == eRawHighlightsModeLCh) && settings->layout.hasCfa();
    RectI need;
    if ((settings->mode == eRawHighlightsModeOpposed) || (settings->mode == eRawHighlightsModeInpaint) || (settings->mode == eRawHighlightsModeSegments) || ((settings->mode == eRawHighlightsModeLCh) && !lchOnCfa)) {
        need = settings->sourceRoD;
    } else if (lchOnCfa) {
        const RectI window = PassExec::toPixelEnclosing(renderWindow, scale, par);
        need = RectI(window.x1 - 2, window.y1 - 2, window.x2 + 2, window.y2 + 2).intersect(settings->sourceRoD);
    } else {
        return;
    }
    (*ret)[input] = PassExec::toCanonical(need, scale, par);
}

ImageMetadata
RawHighlights::deriveOutputMetadata(double time,
                                    ViewIdx view)
{
    ImageMetadata metadata = StagedImageEffect::deriveOutputMetadata(time, view);
    const KnobReader reader(time, view);
    const int mode = reader.value(_mode);

    if ((mode != (int)eRawHighlightsModeClip) && (mode != (int)eRawHighlightsModeLCh) && (mode != (int)eRawHighlightsModeInpaint)) {
        return metadata;
    }
    // Clipping moves the white point to one level for every channel, as darktable records it.
    const std::optional<std::array<double, 3>> maximum = RawMetadata::getProcessedMaximum(metadata);
    if (!maximum) {
        return metadata;
    }
    const std::optional<RawLayout> layout = RawLayout::fromMetadata(metadata);
    const bool linear = !layout || !layout->hasCfa();
    if (linear && (mode != (int)eRawHighlightsModeClip)) {
        return metadata;
    }
    const double m = linear ? std::fmax(1., std::min({ (*maximum)[0], (*maximum)[1], (*maximum)[2] }))
                            : std::fmax(1., std::max({ (*maximum)[0], (*maximum)[1], (*maximum)[2] }));
    RawMetadata::setProcessedMaximum(&metadata, { m, m, m });

    return metadata;
}

StatusEnum
RawHighlights::renderOp(const RenderActionArgs& args,
                        const StagePlan& /*plan*/)
{
    const std::optional<Settings> found = resolveSettings(args.time, args.view);

    if (!found) {
        return eStatusFailed;
    }
    const Settings& s = *found;
    if (s.fellBack) {
        _message.post(getNode(), eMessageTypeWarning, tr(kModeUnavailable).toStdString());
    } else {
        _message.withdraw(getNode());
    }
    const int sourceInput = getSourceInput();
    const bool hasCfa = s.layout.hasCfa();
    const RawHighlightsModeEnum mode = (!hasCfa && (s.mode == eRawHighlightsModeLCh)) ? eRawHighlightsModeOpposed : s.mode;

    RawNodeSupport::LockedSource source;
    ImageLayerDesc layer;
    if (resolveInputPlaneForRender(sourceInput, args.time, args.view, &layer, NULL)) {
        RectI roiPixel;
        source.image = getImage(sourceInput, args.time, args.mappedScale, args.view, NULL, &layer, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &roiPixel);
    }
    if (!source.image || (source.image->getBitDepth() != eImageBitDepthFloat)) {
        return eStatusFailed;
    }
    source.lock();

    RenderCancellation cancel(this);
    const RawKernels::ParallelFor parallel = RawNodeSupport::makeParallelFor(&cancel, appPTR->getNCPUsAvailableForEffect());
    const RectI& roi = args.roi;

    RectI need = roi;
    if ((mode == eRawHighlightsModeOpposed) || (mode == eRawHighlightsModeLCh)) {
        need = RectI(roi.x1 - 2, roi.y1 - 2, roi.x2 + 2, roi.y2 + 2).intersect(s.sourceRoD);
    }
    RawNodeSupport::TopDownBuffer buffer;
    buffer.allocate(need, source.nComps);
    RawNodeSupport::readTopDown(source, &buffer, parallel);
    const ImageView in(buffer, s.sourceRoD);
    const int nComps = source.nComps;

    std::array<float, 3> chroma = { 0.f, 0.f, 0.f };
    std::shared_ptr<const std::vector<float>> frameMosaic;
    float clips[3] = { s.clip, s.clip, s.clip };
    const U64 key = RawNodeSupport::frameKey(getRenderHash(), args.time, args.view);
    float opposedClips[3];
    for (int c = 0; c < 3; ++c) {
        opposedClips[c] = kClipMagicOpposed * s.clip * s.coeffs[c];
    }
    // Segmentation Based starts from Inpaint Opposed, whose chrominance it shares, as darktable's
    // chrominance cache leaves the mode out. whole is the source when the caller has it.
    const auto opposedChromaOf = [&](const ImageView* whole) {
        std::lock_guard<std::mutex> lock(_chromaMutex);
        if (!_chromaValid || (_chromaKey != key)) {
            std::optional<ImageView> own;
            RawNodeSupport::TopDownBuffer ownBuffer;
            if (!whole) {
                viewWholeSource(source, s.sourceRoD, parallel, &own, &ownBuffer);
                whole = &*own;
            }
            if (hasCfa) {
                _chroma = opposedChroma(*whole, CfaLookup(s.layout, s.sourceRoD), opposedClips, parallel);
            } else if (nComps >= 3) {
                _chroma = linearOpposedChroma(*whole, opposedClips, parallel);
            } else {
                _chroma = { 0.f, 0.f, 0.f };
            }
            _chromaKey = key;
            _chromaValid = !cancel.check();
        }

        return _chroma;
    };
    if (mode == eRawHighlightsModeOpposed) {
        std::copy(opposedClips, opposedClips + 3, clips);
        chroma = opposedChromaOf(NULL);
    } else if ((mode == eRawHighlightsModeInpaint) || (mode == eRawHighlightsModeSegments)) {
        std::lock_guard<std::mutex> lock(_frameMutex);
        if (!_frameValid || (_frameKey != key)) {
            _frame.reset();
            std::optional<ImageView> whole;
            RawNodeSupport::TopDownBuffer wholeBuffer;
            viewWholeSource(source, s.sourceRoD, parallel, &whole, &wholeBuffer);
            const int width = s.sourceRoD.width();
            const int height = s.sourceRoD.height();
            auto mosaic = std::make_shared<std::vector<float>>((std::size_t)width * (std::size_t)height, 0.f);
            std::uint8_t xtrans[6][6] = {};
            std::uint32_t filters = RawKernels::kFiltersXTrans;
            if (s.layout.period() == 2) {
                filters = RawNodeSupport::bayerFiltersAt(s.layout, s.sourceRoD);
            } else {
                RawNodeSupport::xtransAt(s.layout, s.sourceRoD, xtrans);
            }
            if (mode == eRawHighlightsModeInpaint) {
                float inpaintClips[3];
                for (int c = 0; c < 3; ++c) {
                    inpaintClips[c] = kClipMagicInpaint * s.clip * s.coeffs[c];
                }
                RawKernels::inpaintColor(whole->pixel(0, 0), whole->rowStride, whole->nComps, mosaic->data(), width, height, filters, xtrans, inpaintClips, parallel);
            } else {
                const std::array<float, 3> wholeChroma = opposedChromaOf(&*whole);
                const ImageView& view = *whole;
                const CfaLookup cfa(s.layout, s.sourceRoD);
                float* opposed = mosaic->data();
                parallel(height, [&](int row) {
                    float* line = opposed + (std::size_t)row * (std::size_t)width;
                    for (int col = 0; col < width; ++col) {
                        const float v = view.at(row, col);
                        const int color = cfa.at(row, col);
                        line[col] = (v >= opposedClips[color]) ? std::max(v, calcRefavg(view, cfa, row, col) + wholeChroma[color]) : v;
                    }
                });
                RawKernels::SegmentsParams params;
                params.clip = s.clip;
                for (int c = 0; c < 3; ++c) {
                    params.coeffs[c] = s.coeffs[c];
                }
                params.candidating = s.candidating;
                params.combine = s.combine;
                params.recovery = s.recovery;
                params.strength = s.strength;
                params.noiseLevel = s.noiseLevel;
                RawKernels::segmentsHighlights(whole->pixel(0, 0), whole->rowStride, whole->nComps, opposed, width, height, filters, xtrans, params, parallel);
            }
            _frame = std::move(mosaic);
            _frameKey = key;
            _frameValid = !cancel.check();
        }
        frameMosaic = _frame;
    } else if (mode == eRawHighlightsModeLCh) {
        clips[0] = clips[1] = clips[2] = kClipMagicLCh * s.clip;
    } else {
        clips[0] = clips[1] = clips[2] = kClipMagicClip * s.clip;
    }

    const RawNodeSupport::TopDownBuffer* inBuffer = &buffer;
    const RectI rod = s.sourceRoD;
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        const ImagePtr& dst = it->second;
        if (!dst || (dst->getBitDepth() != eImageBitDepthFloat)) {
            return eStatusFailed;
        }
        const int dstComps = (int)dst->getComponentsCount();
        Image::WriteAccess access(dst.get());
        const CfaLookup cfa = hasCfa ? CfaLookup(s.layout, rod) : CfaLookup(RawLayout(), rod);
        parallel(roi.height(), [&](int r) {
            const int y = roi.y1 + r;
            float* out = (float*)access.pixelAt(roi.x1, y);
            if (!out) {
                return;
            }
            const int row = rod.y2 - 1 - y;
            for (int x = roi.x1; x < roi.x2; ++x, out += dstComps) {
                const int col = x - rod.x1;
                const bool inside = (x >= rod.x1) && (x < rod.x2) && (y >= rod.y1) && (y < rod.y2) && (x >= inBuffer->rect.x1) && (x < inBuffer->rect.x2) && (y >= inBuffer->rect.y1) && (y < inBuffer->rect.y2);
                if (!inside) {
                    std::fill(out, out + dstComps, 0.f);
                    continue;
                }
                const float* pixel = in.pixel(row, col);
                if (hasCfa) {
                    float v = pixel[0];
                    const int color = cfa.at(row, col);
                    switch (mode) {
                    case eRawHighlightsModeOpposed:
                        if (v >= clips[color]) {
                            v = std::max(v, calcRefavg(in, cfa, row, col) + chroma[color]);
                        }
                        break;
                    case eRawHighlightsModeInpaint:
                    case eRawHighlightsModeSegments:
                        v = (*frameMosaic)[(std::size_t)row * (std::size_t)rod.width() + (std::size_t)col];
                        break;
                    case eRawHighlightsModeLCh:
                        v = (s.layout.period() == 2) ? lchBayer(in, cfa, row, col, clips[0]) : lchXTrans(in, cfa, row, col, clips[0]);
                        break;
                    default:
                        v = std::fmin(v, clips[color]);
                        break;
                    }
                    for (int c = 0; c < dstComps; ++c) {
                        out[c] = (c < nComps) ? ((c == 0) ? v : pixel[c]) : 0.f;
                    }
                    continue;
                }
                for (int c = 0; c < dstComps; ++c) {
                    const float inval = (c < nComps) ? pixel[c] : 0.f;
                    if (c >= 3) {
                        out[c] = inval;
                    } else if ((mode == eRawHighlightsModeOpposed) && (nComps >= 3)) {
                        const float positive = std::fmax(0.0f, inval);
                        out[c] = (positive >= clips[c]) ? std::fmax(positive, calcLinearRefavg(pixel, c) + chroma[c]) : positive;
                    } else {
                        out[c] = std::fmin(clips[c], inval);
                    }
                }
            }
        });
    }

    return cancel.check() ? eStatusFailed : eStatusOK;
} // RawHighlights::renderOp

bool
RawHighlights::canRenderOpOnGpu(const GpuQuery& query) const
{
#ifdef NATRON_HAVE_GPU
    if ((query.bitDepth != eImageBitDepthFloat) || (query.nComps < 1) || (query.nComps > 4)) {
        return false;
    }
    // Resolving only reads knobs and the source's metadata and region.
    const std::optional<Settings> settings = const_cast<RawHighlights*>(this)->resolveSettings(query.time, query.view);
    if (!settings) {
        return false;
    }

    return (settings->mode == eRawHighlightsModeClip) || ((settings->mode == eRawHighlightsModeLCh) && settings->layout.hasCfa());
#else
    Q_UNUSED(query);

    return false;
#endif
}

bool
RawHighlights::getOpGpuInputLayer(int inputNb,
                                  const ImageLayerDesc& plane,
                                  double time,
                                  ViewIdx view,
                                  ImageLayerDesc* layer) const
{
    stageGpuInputLayer(*this, inputNb, plane, time, view, layer);

    return true;
}

RectI
RawHighlights::getOpGpuInputRoI(int inputNb,
                                const RectI& outputRect,
                                const RenderScale& /*scale*/) const
{
    const std::optional<Settings> settings = const_cast<RawHighlights*>(this)->resolveSettings(getCurrentTime(), getCurrentView());

    if ((inputNb != getSourceInput()) || !settings || (settings->mode != eRawHighlightsModeLCh) || !settings->layout.hasCfa()) {
        return outputRect;
    }

    return RectI(outputRect.x1 - 2, outputRect.y1 - 2, outputRect.x2 + 2, outputRect.y2 + 2).intersect(settings->sourceRoD);
}

StatusEnum
RawHighlights::renderOpGpu(const GpuRenderArgs& args)
{
#ifdef NATRON_HAVE_GPU
    const std::optional<Settings> found = resolveSettings(args.query.time, args.query.view);

    if (!found) {
        return eStatusFailed;
    }
    const Settings& s = *found;
    const bool lch = (s.mode == eRawHighlightsModeLCh) && s.layout.hasCfa();
    if ((s.mode != eRawHighlightsModeClip) && !lch) {
        return eStatusFailed;
    }
    RawHighlightsGpuParams params = {};
    params.layout = toRawLayoutGpu(s.layout);
    params.clip = (lch ? kClipMagicLCh : kClipMagicClip) * s.clip;
    params.rodX1 = s.sourceRoD.x1;
    params.rodY1 = s.sourceRoD.y1;
    params.rodX2 = s.sourceRoD.x2;
    params.rodY2 = s.sourceRoD.y2;
    const int kernel = lch ? (int)gpu::GpuKernelId::rawhighlights_lch : (int)gpu::GpuKernelId::rawhighlights_clip;

    return RawNodeSupport::recordRawPointGpu(args, getSourceInput(), kernel, params);
#else
    Q_UNUSED(args);

    return eStatusFailed;
#endif
}

NATRON_NAMESPACE_EXIT
