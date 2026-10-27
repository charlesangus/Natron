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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "RetouchAlgorithms.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

#include "Engine/Nodes/Filter/EdgeAware/Eigf.h"
#include "Engine/Nodes/Filter/EdgeAware/PoissonHeal.h"

NATRON_NAMESPACE_ENTER

namespace RetouchCore {
namespace {
    // darktable's linear Rec. 709 work profile, adapted to D50, and its inverse (src/common/colorspaces_inline_conversions.h).
    const float kRgbToXyz[3][3] = {
        { 0.4360747f, 0.3850649f, 0.1430804f },
        { 0.2225045f, 0.7168786f, 0.0606169f },
        { 0.0139322f, 0.0971045f, 0.7141733f },
    };
    const float kXyzToRgb[3][3] = {
        { 3.1338561f, -1.6168667f, -0.4906146f },
        { -0.9787684f, 1.9161415f, 0.0334540f },
        { 0.0719453f, -0.2289914f, 1.4052427f },
    };
    const float kD50[3] = { 0.9642f, 1.0f, 0.8249f };

    // Bilateral grid limits (src/common/bilateral.h).
    const int kBilateralMaxResS = 3000;
    const int kBilateralMaxResR = 50;

    inline float
    labF(float x)
    {
        const float epsilon = 216.0f / 24389.0f;
        const float kappa = 24389.0f / 27.0f;

        return (x > epsilon) ? std::cbrt(x) : (kappa * x + 16.0f) / 116.0f;
    }

    inline float
    labFInv(float x)
    {
        const float epsilon = 0.20689655172413796f;
        const float kappa = 24389.0f / 27.0f;

        return (x > epsilon) ? x * x * x : (116.0f * x - 16.0f) / kappa;
    }

    inline void
    pixelRgbToLab(float* p)
    {
        float f[3];
        for (int i = 0; i < 3; ++i) {
            const float xyz = kRgbToXyz[i][0] * p[0] + kRgbToXyz[i][1] * p[1] + kRgbToXyz[i][2] * p[2];
            f[i] = labF(xyz / kD50[i]);
        }
        p[0] = 116.0f * f[1] - 16.0f;
        p[1] = 500.0f * (f[0] - f[1]);
        p[2] = -200.0f * (f[2] - f[1]);
    }

    inline void
    pixelLabToRgb(float* p)
    {
        const float fy = (p[0] + 16.0f) / 116.0f;
        const float fx = p[1] / 500.0f + fy;
        const float fz = fy - p[2] / 200.0f;
        const float xyz[3] = { kD50[0] * labFInv(fx), kD50[1] * labFInv(fy), kD50[2] * labFInv(fz) };
        for (int i = 0; i < 3; ++i) {
            p[i] = kXyzToRgb[i][0] * xyz[0] + kXyzToRgb[i][1] * xyz[1] + kXyzToRgb[i][2] * xyz[2];
        }
    }

    // rt_copy_in_to_out(): out (outRoi) from in (inRoi), each row shifted by dx, dy.
    void
    copyInToOut(const float* in,
                const Region& inRoi,
                float* out,
                const Region& outRoi,
                int dx,
                int dy)
    {
        const std::size_t rowFloats = (std::size_t)4 * std::min(outRoi.width, inRoi.width);
        const int xoffs = outRoi.x - inRoi.x - dx;
        const int yoffs = outRoi.y - inRoi.y - dy;
        const int yTo = std::min(outRoi.height, inRoi.height);

        for (int y = 0; y < yTo; ++y) {
            const float* src = in + ((std::size_t)(y + yoffs) * inRoi.width + xoffs) * 4;
            float* dst = out + (std::size_t)y * outRoi.width * 4;
            std::memcpy(dst, src, rowFloats * sizeof(float));
        }
    }

    // rt_copy_image_masked(): src covers maskRoi.
    void
    copyImageMasked(const float* src,
                    float* dest,
                    const Region& destRoi,
                    const float* mask,
                    const Region& maskRoi,
                    float opacity)
    {
        for (int yy = 0; yy < maskRoi.height; ++yy) {
            const std::size_t maskIndex = (std::size_t)yy * maskRoi.width;
            const float* s = src + 4 * maskIndex;
            const float* m = mask + maskIndex;
            float* d = dest + 4 * ((std::size_t)(yy + maskRoi.y - destRoi.y) * destRoi.width + (maskRoi.x - destRoi.x));
            for (int xx = 0; xx < maskRoi.width; ++xx) {
                const float f = m[xx] * opacity;
                const float f1 = 1.0f - f;
                for (int c = 0; c < 4; ++c) {
                    d[4 * xx + c] = d[4 * xx + c] * f1 + s[4 * xx + c] * f;
                }
            }
        }
    }

    // bilateral.c's blur_line(): [1 4 6 4 1] / 16 along offset3.
    void
    blurLine(float* buf,
             int offset1,
             int offset2,
             int offset3,
             int size1,
             int size2,
             int size3)
    {
        const float w0 = 6.f / 16.f;
        const float w1 = 4.f / 16.f;
        const float w2 = 1.f / 16.f;

        for (int k = 0; k < size1; ++k) {
            std::size_t index = (std::size_t)k * offset1;
            for (int j = 0; j < size2; ++j) {
                float tmp1 = buf[index];
                buf[index] = buf[index] * w0 + w1 * buf[index + offset3] + w2 * buf[index + 2 * offset3];
                index += offset3;
                float tmp2 = buf[index];
                buf[index] = buf[index] * w0 + w1 * (buf[index + offset3] + tmp1) + w2 * buf[index + 2 * offset3];
                index += offset3;
                for (int i = 2; i < size3 - 2; ++i) {
                    const float tmp3 = buf[index];
                    buf[index] = buf[index] * w0 + w1 * (buf[index + offset3] + tmp2) + w2 * (buf[index + 2 * offset3] + tmp1);
                    index += offset3;
                    tmp1 = tmp2;
                    tmp2 = tmp3;
                }
                const float tmp3 = buf[index];
                buf[index] = buf[index] * w0 + w1 * (buf[index + offset3] + tmp2) + w2 * tmp1;
                index += offset3;
                buf[index] = buf[index] * w0 + w1 * tmp3 + w2 * tmp2;
                index += offset3;
                index += offset2 - offset3 * size3;
            }
        }
    }

    // bilateral.c's blur_line_z(): the derivative of the gaussian along offset3.
    void
    blurLineZ(float* buf,
              int offset1,
              int offset2,
              int offset3,
              int size1,
              int size2,
              int size3)
    {
        const float w1 = 4.f / 16.f;
        const float w2 = 2.f / 16.f;

        for (int k = 0; k < size1; ++k) {
            std::size_t index = (std::size_t)k * offset1;
            for (int j = 0; j < size2; ++j) {
                float tmp1 = buf[index];
                buf[index] = w1 * buf[index + offset3] + w2 * buf[index + 2 * offset3];
                index += offset3;
                float tmp2 = buf[index];
                buf[index] = w1 * (buf[index + offset3] - tmp1) + w2 * buf[index + 2 * offset3];
                index += offset3;
                for (int i = 2; i < size3 - 2; ++i) {
                    const float tmp3 = buf[index];
                    buf[index] = +w1 * (buf[index + offset3] - tmp2) + w2 * (buf[index + 2 * offset3] - tmp1);
                    index += offset3;
                    tmp1 = tmp2;
                    tmp2 = tmp3;
                }
                const float tmp3 = buf[index];
                buf[index] = w1 * (buf[index + offset3] - tmp2) - w2 * tmp1;
                index += offset3;
                buf[index] = -w1 * tmp3 - w2 * tmp2;
                index += offset3;
                index += offset2 - offset3 * size3;
            }
        }
    }

    inline float
    clampf(float v,
           float lo,
           float hi)
    {
        return std::min(std::max(v, lo), hi);
    }
} // anonymous namespace

void
rgbToLab(float* pixels,
         std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i) {
        pixelRgbToLab(pixels + 4 * i);
    }
}

void
labToRgb(float* pixels,
         std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i) {
        pixelLabToRgb(pixels + 4 * i);
    }
}

bool
buildRegionMask(const Shape& shape,
                const Region& layerRoi,
                int dx,
                int dy,
                std::vector<float>* mask,
                Region* maskRoi)
{
    const int padding = (shape.settings.algorithm == eAlgorithmHeal) ? 1 : 0;
    const Region& r1 = shape.maskRoi;
    const Region& r2 = layerRoi;
    // rt_intersect_2_rois(), with its one-pixel shift.
    const int xFrom = std::max(std::max(r1.x + 1 - padding, r2.x), r2.x + dx);
    const int xTo = std::min(std::min(r1.x + r1.width + 1 + padding, r2.x + r2.width), r2.x + r2.width + dx);
    const int yFrom = std::max(std::max(r1.y + 1 - padding, r2.y), r2.y + dy);
    const int yTo = std::min(std::min(r1.y + r1.height + 1 + padding, r2.y + r2.height), r2.y + r2.height + dy);

    maskRoi->x = xFrom;
    maskRoi->y = yFrom;
    maskRoi->width = xTo - xFrom;
    maskRoi->height = yTo - yFrom;
    if ((maskRoi->width < 1) || (maskRoi->height < 1)) {
        mask->clear();

        return false;
    }
    mask->assign((std::size_t)maskRoi->width * maskRoi->height, 0.f);
    const bool hasMask = shape.mask.size() >= (std::size_t)std::max(0, r1.width) * std::max(0, r1.height);
    for (int yy = yFrom; yy < yTo; ++yy) {
        const int my = yy - r1.y;
        if (!hasMask || (my < 0) || (my >= r1.height)) {
            continue;
        }
        const float* m = &shape.mask[(std::size_t)my * r1.width];
        float* ms = &(*mask)[(std::size_t)(yy - yFrom) * maskRoi->width];
        for (int xx = xFrom; xx < xTo; ++xx) {
            const int mx = xx - r1.x;
            if ((mx < 0) || (mx >= r1.width)) {
                continue;
            }
            ms[xx - xFrom] = m[mx];
        }
    }

    return true;
}

void
retouchClone(float* layer,
             const Region& layerRoi,
             const float* mask,
             const Region& maskRoi,
             int dx,
             int dy,
             float opacity)
{
    // A copy, as the shape and its source may overlap.
    std::vector<float> src((std::size_t)4 * maskRoi.width * maskRoi.height);
    copyInToOut(layer, layerRoi, src.data(), maskRoi, dx, dy);
    copyImageMasked(src.data(), layer, layerRoi, mask, maskRoi, opacity);
}

void
retouchHeal(float* layer,
            const Region& layerRoi,
            const float* mask,
            const Region& maskRoi,
            int dx,
            int dy,
            float opacity,
            int maxIterations,
            const EdgeAware::ParallelFor& parallel)
{
    const std::size_t size = (std::size_t)4 * maskRoi.width * maskRoi.height;
    std::vector<float> src(size);
    std::vector<float> dest(size);

    copyInToOut(layer, layerRoi, src.data(), maskRoi, dx, dy);
    copyInToOut(layer, layerRoi, dest.data(), maskRoi, 0, 0);
    EdgeAware::poissonHeal(src.data(), dest.data(), mask, maskRoi.width, maskRoi.height, maxIterations, parallel);
    copyImageMasked(dest.data(), layer, layerRoi, mask, maskRoi, opacity);
}

void
retouchFill(float* layer,
            const Region& layerRoi,
            const float* mask,
            const Region& maskRoi,
            float opacity,
            FillModeEnum mode,
            const float color[3],
            float brightness)
{
    float fill[4];
    if (mode == eFillModeErase) {
        fill[0] = fill[1] = fill[2] = brightness;
    } else {
        for (int c = 0; c < 3; ++c) {
            fill[c] = color[c] + brightness;
        }
    }
    fill[3] = 0.f;

    for (int yy = 0; yy < maskRoi.height; ++yy) {
        const float* m = mask + (std::size_t)yy * maskRoi.width;
        float* d = layer + 4 * ((std::size_t)(yy + maskRoi.y - layerRoi.y) * layerRoi.width + (maskRoi.x - layerRoi.x));
        for (int xx = 0; xx < maskRoi.width; ++xx) {
            const float f = m[xx] * opacity;
            for (int c = 0; c < 4; ++c) {
                d[4 * xx + c] = d[4 * xx + c] * (1.0f - f) + fill[c] * f;
            }
        }
    }
}

void
retouchBlur(float* layer,
            const Region& layerRoi,
            const float* mask,
            const Region& maskRoi,
            float opacity,
            BlurTypeEnum type,
            float radius,
            float renderScale,
            const EdgeAware::ParallelFor& parallel)
{
    if (std::fabs(radius) <= 0.1f) {
        return;
    }
    const float sigma = radius * renderScale;
    const std::size_t count = (std::size_t)maskRoi.width * maskRoi.height;
    std::vector<float> img(4 * count);
    copyInToOut(layer, layerRoi, img.data(), maskRoi, 0, 0);

    if (type == eBlurTypeGaussian) {
        const float maxValue[4] = { FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX };
        const float minValue[4] = { -FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX };
        std::vector<float> blurred(4 * count);
        EdgeAware::recursiveGaussianBlur(img.data(), blurred.data(), maskRoi.width, maskRoi.height, 4,
                                         EdgeAware::clampGaussianSigma(sigma, maskRoi.width, maskRoi.height),
                                         minValue, maxValue, parallel);
        img.swap(blurred);
    } else {
        rgbToLab(img.data(), count);
        bilateralBaseLayer(img.data(), maskRoi.width, maskRoi.height, sigma, 100.0f);
        labToRgb(img.data(), count);
    }
    copyImageMasked(img.data(), layer, layerRoi, mask, maskRoi, opacity);
}

void
adjustLevels(float* image,
             int width,
             int height,
             const float levels[3],
             const EdgeAware::ParallelFor& parallel)
{
    const float left = levels[0];
    const float middle = levels[1];
    const float right = levels[2];

    if ((left == kPreviewLevelMin) && (middle == 0.f) && (right == kPreviewLevelMax)) {
        return;
    }
    const float delta = (right - left) / 2.0f;
    const float mid = left + delta;
    const float tmp = (middle - mid) / delta;
    const float inInvGamma = std::pow(10.f, tmp);

    parallel(height, [&](int y) {
        float* row = image + (std::size_t)y * width * 4;
        for (int x = 0; x < width; ++x) {
            float* p = row + 4 * x;
            pixelRgbToLab(p);
            const float lIn = p[0] / 100.0f;
            if (lIn <= left) {
                p[0] = 0.f;
            } else {
                const float percentage = (lIn - left) / (right - left);
                p[0] = 100.0f * std::pow(percentage, inInvGamma);
            }
            pixelLabToRgb(p);
        }
    });
}

void
bilateralBaseLayer(float* image,
                   int width,
                   int height,
                   float sigmaS,
                   float sigmaR)
{
    const float lRange = 100.0f;

    // dt_bilateral_grid_size()
    if (sigmaS < 0.5f) {
        sigmaS = 0.5f;
    }
    const float gx = (float)std::min(std::max((int)std::round(width / sigmaS), 4), kBilateralMaxResS);
    const float gy = (float)std::min(std::max((int)std::round(height / sigmaS), 4), kBilateralMaxResS);
    const float gz = (float)std::min(std::max((int)std::round(lRange / sigmaR), 4), kBilateralMaxResR);
    const float gridSigmaS = std::max(height / gy, width / gx);
    const float gridSigmaR = lRange / gz;
    const float sInv = 1.0f / gridSigmaS;
    const float rInv = 1.0f / gridSigmaR;
    const int sizeX = (int)std::ceil(width * sInv) + 1;
    const int sizeY = (int)std::ceil(height * sInv) + 1;
    const int sizeZ = (int)std::ceil(lRange * rInv) + 1;

    const int ox = sizeZ;
    const int oy = sizeX * sizeZ;
    const int oz = 1;
    // darktable's line blurs read two samples past a line shorter than 4: keep those reads in the buffer.
    std::vector<float> buf((std::size_t)sizeX * sizeY * sizeZ + 2 * (std::size_t)(oy + ox + oz), 0.f);

    // dt_bilateral_splat()
    const float sigmaS2 = gridSigmaS * gridSigmaS;
    const std::size_t offsets[8] = { 0, (std::size_t)ox, (std::size_t)oy, (std::size_t)(ox + oy),
                                     (std::size_t)oz, (std::size_t)(oz + ox), (std::size_t)(oz + oy), (std::size_t)(oz + oy + ox) };
    for (int j = 0; j < height; ++j) {
        const float y = clampf(j * sInv, 0, sizeY - 1);
        const int yi = std::min((int)y, sizeY - 2);
        const float yf = y - yi;
        for (int i = 0; i < width; ++i) {
            const float L = image[4 * ((std::size_t)j * width + i)];
            const float x = clampf(i * sInv, 0, sizeX - 1);
            const float z = clampf(L * rInv, 0, sizeZ - 1);
            const int xi = std::min((int)x, sizeX - 2);
            const int zi = std::min((int)z, sizeZ - 2);
            const float xf = x - xi;
            const float zf = z - zi;
            const std::size_t gridIndex = (std::size_t)yi * oy + (std::size_t)xi * ox + zi;
            const float contrib[4] = {
                (1.0f - xf) * (1.0f - yf) * 100.0f / sigmaS2,
                xf * (1.0f - yf) * 100.0f / sigmaS2,
                (1.0f - xf) * yf * 100.0f / sigmaS2,
                xf * yf * 100.0f / sigmaS2,
            };
            for (int k = 0; k < 4; ++k) {
                buf[gridIndex + offsets[k]] += contrib[k] * (1.0f - zf);
                buf[gridIndex + offsets[k + 4]] += contrib[k] * zf;
            }
        }
    }

    // dt_bilateral_blur()
    blurLine(buf.data(), oz, oy, ox, sizeZ, sizeY, sizeX);
    blurLine(buf.data(), oz, ox, oy, sizeZ, sizeX, sizeY);
    blurLineZ(buf.data(), ox, oy, oz, sizeX, sizeY, sizeZ);

    // dt_bilateral_slice() with detail -1
    const float norm = gridSigmaR * 0.04f;
    for (int j = 0; j < height; ++j) {
        const float y = clampf(j * sInv, 0, sizeY - 1);
        const int yi = std::min((int)y, sizeY - 2);
        const float yf = y - yi;
        for (int i = 0; i < width; ++i) {
            float* p = image + 4 * ((std::size_t)j * width + i);
            const float L = p[0];
            const float x = clampf(i * sInv, 0, sizeX - 1);
            const float z = clampf(L * rInv, 0, sizeZ - 1);
            const int xi = std::min((int)x, sizeX - 2);
            const int zi = std::min((int)z, sizeZ - 2);
            const float xf = x - xi;
            const float zf = z - zi;
            const float* g = &buf[(std::size_t)(xi + yi * sizeX) * sizeZ + zi];
            p[0] = std::fmax(0.0f, L + norm * (g[0] * (1.0f - xf) * (1.0f - yf) * (1.0f - zf) + g[ox] * xf * (1.0f - yf) * (1.0f - zf) + g[oy] * (1.0f - xf) * yf * (1.0f - zf) + g[ox + oy] * xf * yf * (1.0f - zf) + g[oz] * (1.0f - xf) * (1.0f - yf) * zf + g[ox + oz] * xf * (1.0f - yf) * zf + g[oy + oz] * (1.0f - xf) * yf * zf + g[ox + oy + oz] * xf * yf * zf));
        }
    }
} // bilateralBaseLayer
} // namespace RetouchCore

NATRON_NAMESPACE_EXIT
