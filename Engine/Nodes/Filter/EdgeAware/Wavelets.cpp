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
 * src/common/dwt.c and src/common/bspline.h, C paths only.
 * darktable is distributed under the GNU GPL, version 3 or (at your option) any later version,
 * and so is this port: GPL-3.0-or-later.
 *
 *     This file is part of darktable,
 *     Copyright (C) 2017-2026 darktable developers.
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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Wavelets.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

NATRON_NAMESPACE_ENTER

namespace EdgeAware {
namespace {
    const int kRowBlock = 8;

    inline float
    sqf(float x)
    {
        return x * x;
    }

    void
    addImage(float* accum,
             const float* layer,
             std::size_t samples,
             const ParallelFor& parallel)
    {
        const int blocks = (int)((samples + 65535) / 65536);

        parallel(blocks, [&](int b) {
            const std::size_t end = std::min(samples, (std::size_t)(b + 1) * 65536);
            for (std::size_t k = (std::size_t)b * 65536; k < end; ++k) {
                accum[k] += layer[k];
            }
        });
    }

    // _get_max_scale()
    int
    getMaxScale(int width,
                int height,
                float previewScale)
    {
        int maxscale = 0;
        unsigned int size = (unsigned int)std::min(width, height);
        float sizeTmp = ((size >>= 1) * previewScale);

        while (sizeTmp > 0.f) {
            sizeTmp = ((size >>= 1) * previewScale);
            ++maxscale;
        }
        size = (unsigned int)std::min(width, height);
        while ((maxscale > 0) && ((1 << maxscale) * previewScale >= size)) {
            --maxscale;
        }

        return maxscale;
    }

    // dwt_decompose_vert(): 2 center + above + below, mirrored at the edges.
    void
    hatVertical(float* out,
                const float* in,
                std::size_t height,
                std::size_t width,
                unsigned int lev,
                const ParallelFor& parallel)
    {
        const std::size_t vscale = std::min<std::size_t>((std::size_t)1 << lev, height - 1);

        forBlocks(parallel, (int)height, kRowBlock, [&](int begin, int end) {
            for (std::size_t row = (std::size_t)begin; row < (std::size_t)end; ++row) {
                const std::size_t rowstart = 4 * row * width;
                const std::size_t aboveRow = (row > vscale) ? row - vscale : vscale - row;
                const std::size_t belowRow = (row + vscale < height) ? (row + vscale) : 2 * (height - 1) - (row + vscale);
                const float* center = in + rowstart;
                const float* above = in + 4 * aboveRow * width;
                const float* below = in + 4 * belowRow * width;
                float* temprow = out + rowstart;
                for (std::size_t col = 0; col < 4 * width; ++col) {
                    temprow[col] = 2.f * center[col] + above[col] + below[col];
                }
            }
        });
    }

    // dwt_decompose_horiz(): turns the vertical sums in coarse into the hat low-pass, and
    // subtracts it from details, which holds the scale's input.
    void
    hatHorizontal(float* coarse,
                  float* details,
                  std::size_t height,
                  std::size_t width,
                  unsigned int lev,
                  const ParallelFor& parallel)
    {
        const int w = (int)width;
        const int hscale = std::min(1 << lev, w - 1);

        forBlocks(parallel, (int)height, kRowBlock, [&](int begin, int end) {
            std::vector<float> temprow(4 * width);
            for (int row = begin; row < end; ++row) {
                const std::size_t rowindex = 4 * ((std::size_t)row * width);
                float* detailsRow = details + rowindex;
                float* coarseRow = coarse + rowindex;
                for (int col = 0; col < w; ++col) {
                    const std::size_t leftpos = (std::size_t)4 * std::abs(col - hscale);
                    const std::size_t rightpos = (col < w - hscale) ? (std::size_t)4 * (col + hscale) : (std::size_t)4 * (2 * w - 2 - (col + hscale));
                    for (int c = 0; c < 4; ++c) {
                        const float hat = (2.f * coarseRow[4 * col + c] + coarseRow[leftpos + c] + coarseRow[rightpos + c]) / 16.f;
                        temprow[4 * col + c] = hat;
                        detailsRow[4 * col + c] -= hat;
                    }
                }
                std::memcpy(coarseRow, temprow.data(), sizeof(float) * 4 * width);
            }
        });
    }

    inline void
    sparseScalarProduct(const float* buf,
                        const std::size_t indices[5],
                        float* result,
                        bool clipNegatives)
    {
        static const float filter[5] = { 1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f };

        for (int c = 0; c < 4; ++c) {
            const float v = filter[0] * buf[indices[0] + c] + filter[1] * buf[indices[1] + c] + filter[2] * buf[indices[2] + c] + filter[3] * buf[indices[3] + c] + filter[4] * buf[indices[4] + c];
            result[c] = (clipNegatives && (0.0f > v)) ? 0.0f : v;
        }
    }

    void
    bsplineVerticalPass(const float* in,
                        float* temp,
                        std::size_t row,
                        std::size_t width,
                        std::size_t height,
                        int mult,
                        bool clipNegatives)
    {
        const std::size_t indices[5] = { 4 * width * (std::size_t)std::max((int)row - 2 * mult, 0),
                                         4 * width * (std::size_t)std::max((int)row - mult, 0),
                                         4 * width * row,
                                         4 * width * std::min(row + (std::size_t)mult, height - 1),
                                         4 * width * std::min(row + 2 * (std::size_t)mult, height - 1) };

        for (std::size_t j = 0; j < width; ++j) {
            sparseScalarProduct(in + j * 4, indices, temp + j * 4, clipNegatives);
        }
    }

    void
    bsplineHorizontal(const float* temp,
                      float* out,
                      std::size_t col,
                      std::size_t width,
                      int mult,
                      bool clipNegatives)
    {
        const std::size_t indices[5] = { 4 * (std::size_t)std::max((int)col - 2 * mult, 0),
                                         4 * (std::size_t)std::max((int)col - mult, 0),
                                         4 * col,
                                         4 * std::min(col + (std::size_t)mult, width - 1),
                                         4 * std::min(col + 2 * (std::size_t)mult, width - 1) };

        sparseScalarProduct(temp, indices, out, clipNegatives);
    }

    void
    zeroAt(const NonFiniteSamples& samples,
           float* buf)
    {
        for (std::size_t i : samples.indices()) {
            buf[i] = 0.f;
        }
    }
} // namespace

int
dwtMaxScale(const DwtParams& params)
{
    return getMaxScale((int)(params.width / params.previewScale), (int)(params.height / params.previewScale), params.previewScale);
}

int
dwtFirstScaleVisible(const DwtParams& params)
{
    int firstScale = 0;

    for (int lev = 0; lev < params.scales; ++lev) {
        int sc = 1 << lev;
        sc *= params.previewScale;
        if (sc > 0) {
            firstScale = lev + 1;
            break;
        }
    }

    return firstScale;
}

void
dwtDecompose(float* image,
             DwtParams& p,
             const DwtLayerFn& layerFn,
             const ParallelFor& parallel)
{
    if ((p.width <= 0) || (p.height <= 0)) {
        return;
    }
    if (p.previewScale <= 0.f) {
        p.previewScale = 1.f;
    }
    if (p.returnLayer > p.scales + 1) {
        p.returnLayer = p.scales + 1;
    }
    const int maxScale = dwtMaxScale(p);
    if (p.scales > maxScale) {
        if (p.returnLayer > p.scales) {
            p.returnLayer = maxScale + 1;
        } else if (p.returnLayer > maxScale) {
            p.returnLayer = maxScale;
        }
        p.scales = maxScale;
    }

    const std::size_t width = (std::size_t)p.width;
    const std::size_t height = (std::size_t)p.height;
    const std::size_t samples = 4 * width * height;
    NonFiniteSamples nonFinite;
    nonFinite.take(image, samples);

    if (layerFn) {
        layerFn(image, p, 0);
    }
    if (p.scales <= 0) {
        nonFinite.restore(image);

        return;
    }

    std::vector<float> second(samples);
    std::vector<float> layers(samples, 0.f);
    std::vector<float> mergedLayers;
    const bool doMerge = p.mergeFromScale > 0;
    if (doMerge) {
        mergedLayers.assign(samples, 0.f);
    }
    float* buffer[2] = { image, second.data() };
    const auto returnLayer = [&](const float* layer) {
        if (layer != image) {
            std::copy(layer, layer + samples, image);
        }
    };

    unsigned int hpass = 0;
    bool bcontinue = true;
    for (unsigned int lev = 0; (lev < (unsigned int)p.scales) && bcontinue; ++lev) {
        const unsigned int lpass = (1 - (lev & 1));
        hatVertical(buffer[lpass], buffer[hpass], height, width, lev, parallel);
        hatHorizontal(buffer[lpass], buffer[hpass], height, width, lev, parallel);

        if ((p.mergeFromScale == 0) || (p.mergeFromScale > (int)lev + 1)) {
            if (layerFn) {
                layerFn(buffer[hpass], p, lev + 1);
            }
            if (p.returnLayer == (int)lev + 1) {
                returnLayer(buffer[hpass]);
                bcontinue = false;
            } else if (p.returnLayer == 0) {
                addImage(layers.data(), buffer[hpass], samples, parallel);
            }
        } else {
            addImage(mergedLayers.data(), buffer[hpass], samples, parallel);
            if (layerFn) {
                layerFn(mergedLayers.data(), p, lev + 1);
            }
            if (p.returnLayer == (int)lev + 1) {
                returnLayer(mergedLayers.data());
                bcontinue = false;
            }
        }
        hpass = lpass;
    }

    if (bcontinue) {
        if (layerFn) {
            layerFn(buffer[hpass], p, p.scales + 1);
        }
        if (p.returnLayer == p.scales + 1) {
            returnLayer(buffer[hpass]);
        } else if (p.returnLayer == 0) {
            if (doMerge) {
                addImage(layers.data(), mergedLayers.data(), samples, parallel);
            }
            addImage(layers.data(), buffer[hpass], samples, parallel);
            if (layerFn) {
                layerFn(layers.data(), p, p.scales + 2);
            }
            returnLayer(layers.data());
        }
    }
    nonFinite.restore(image);
} // dwtDecompose

int
dwtDecomposeScales(const float* in,
                   int width,
                   int height,
                   int scales,
                   WaveletScales& out,
                   const ParallelFor& parallel)
{
    out.details.clear();
    out.residual.clear();
    if ((width <= 0) || (height <= 0)) {
        return 0;
    }
    const std::size_t samples = 4 * (std::size_t)width * (std::size_t)height;
    DwtParams params;
    params.width = width;
    params.height = height;
    const int bands = std::max(0, std::min(scales, dwtMaxScale(params)));

    out.residual.assign(in, in + samples);
    NonFiniteSamples nonFinite;
    nonFinite.take(out.residual.data(), samples);
    std::vector<float> coarse(samples);
    for (int lev = 0; lev < bands; ++lev) {
        hatVertical(coarse.data(), out.residual.data(), (std::size_t)height, (std::size_t)width, (unsigned int)lev, parallel);
        hatHorizontal(coarse.data(), out.residual.data(), (std::size_t)height, (std::size_t)width, (unsigned int)lev, parallel);
        zeroAt(nonFinite, out.residual.data());
        out.details.emplace_back(std::move(out.residual));
        out.residual = coarse;
    }
    nonFinite.restore(out.residual.data());

    return bands;
}

void
bsplineDecomposeScales(const float* in,
                       int width,
                       int height,
                       int scales,
                       WaveletScales& out,
                       const ParallelFor& parallel)
{
    out.details.clear();
    out.residual.clear();
    if ((width <= 0) || (height <= 0)) {
        return;
    }
    const std::size_t samples = 4 * (std::size_t)width * (std::size_t)height;

    out.residual.assign(in, in + samples);
    NonFiniteSamples nonFinite;
    nonFinite.take(out.residual.data(), samples);
    std::vector<float> lf(samples);
    for (int s = 0; s < scales; ++s) {
        std::vector<float> hf(samples);
        decompose2DBspline(out.residual.data(), hf.data(), lf.data(), width, height, 1 << s, parallel);
        zeroAt(nonFinite, hf.data());
        out.details.emplace_back(std::move(hf));
        std::swap(out.residual, lf);
    }
    nonFinite.restore(out.residual.data());
}

void
waveletRecompose(const WaveletScales& scales,
                 int width,
                 int height,
                 float* out,
                 const ParallelFor& parallel)
{
    if ((width <= 0) || (height <= 0)) {
        return;
    }
    const std::size_t samples = 4 * (std::size_t)width * (std::size_t)height;

    std::fill(out, out + samples, 0.f);
    for (const std::vector<float>& band : scales.details) {
        addImage(out, band.data(), samples, parallel);
    }
    addImage(out, scales.residual.data(), samples, parallel);
}

void
blur2DBspline(const float* in,
              float* out,
              int width,
              int height,
              int mult,
              bool clipNegatives,
              const ParallelFor& parallel)
{
    if ((width <= 0) || (height <= 0)) {
        return;
    }
    const std::size_t w = (std::size_t)width;
    const std::size_t h = (std::size_t)height;

    forBlocks(parallel, height, kRowBlock, [&](int begin, int end) {
        std::vector<float> temp(4 * w);
        for (std::size_t i = (std::size_t)begin; i < (std::size_t)end; ++i) {
            bsplineVerticalPass(in, temp.data(), i, w, h, mult, clipNegatives);
            for (std::size_t j = 0; j < w; ++j) {
                bsplineHorizontal(temp.data(), out + (i * w + j) * 4, j, w, mult, clipNegatives);
            }
        }
    });
}

void
decompose2DBspline(const float* in,
                   float* HF,
                   float* LF,
                   int width,
                   int height,
                   int mult,
                   const ParallelFor& parallel)
{
    if ((width <= 0) || (height <= 0)) {
        return;
    }
    const std::size_t w = (std::size_t)width;
    const std::size_t h = (std::size_t)height;

    forBlocks(parallel, height, kRowBlock, [&](int begin, int end) {
        std::vector<float> temp(4 * w);
        for (std::size_t i = (std::size_t)begin; i < (std::size_t)end; ++i) {
            bsplineVerticalPass(in, temp.data(), i, w, h, mult, true);
            for (std::size_t j = 0; j < w; ++j) {
                const std::size_t index = 4 * (i * w + j);
                bsplineHorizontal(temp.data(), LF + index, j, w, mult, true);
                for (int c = 0; c < 4; ++c) {
                    HF[index + c] = in[index + c] - LF[index + c];
                }
            }
        }
    });
}

float
equivalentSigmaAtStep(float sigma,
                      unsigned int s)
{
    float radius = sigma;

    for (unsigned int k = 1; k <= s; ++k) {
        radius = std::sqrt(sqf(radius) + sqf(std::exp2((float)k) * sigma));
    }

    return radius;
}

unsigned int
numStepsToReachEquivalentSigma(float sigmaFilter,
                               float sigmaFinal)
{
    unsigned int s = 0;
    float radius = sigmaFilter;

    while (radius < sigmaFinal) {
        ++s;
        radius = std::sqrt(sqf(radius) + sqf((float)(1 << s) * sigmaFilter));
    }

    return s + 1;
}
} // namespace EdgeAware

NATRON_NAMESPACE_EXIT
