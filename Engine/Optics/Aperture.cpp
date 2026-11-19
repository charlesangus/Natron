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

// Ported from realflare (https://github.com/beatreichenbach/realflare, GPL-3.0-or-later, v1.0.3 at
// 9ce30b0): flare/engine/tasks/shaders/aperture.frag and flare/engine/tasks/aperture.py.

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Aperture.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <vector>

#include "Engine/PoolParallelFor.h"

NATRON_NAMESPACE_ENTER

namespace Optics {

namespace {
    constexpr double kPi = 3.14159265358979323846;
    constexpr int kSamplesPerPixel = kApertureSamples * kApertureSamples;
    constexpr uint32_t kScratchesSalt = 0u;
    constexpr uint32_t kDustSalt = 0x9E3779B9u;

    uint32_t
    pcgHash(uint32_t value)
    {
        const uint32_t state = value * 747796405u + 2891336453u;
        const uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;

        return (word >> 22u) ^ word;
    }

    double
    random01(uint32_t salt,
             uint32_t index,
             uint32_t component)
    {
        return pcgHash(salt + index * 4u + component) / 4294967296.0;
    }

    double
    wrap01(double value)
    {
        const double w = value - std::floor(value);

        return w < 1. ? w : 0.;
    }

    double
    finiteOr(double value,
             double fallback)
    {
        return std::isfinite(value) ? value : fallback;
    }

    uint32_t
    layerCount(double density,
               uint32_t maximum)
    {
        const double d = std::clamp(finiteOr(density, 0.), 0., 1.);

        return (uint32_t)(d * maximum);
    }

    struct Edge {
        float edge;
        float invSpan;
    };

    // 1 - smoothstep(threshold - softness, threshold + softness, sdf) as edge and 1 / span; at no softness
    // realflare's smoothstep divides by zero, which is taken as the hard step sdf < threshold.
    Edge
    makeEdge(double threshold,
             double softness)
    {
        if (!(softness > 0.)) {
            return { (float)threshold, 0.f };
        }

        return { (float)(threshold - softness), (float)(1. / (2. * softness)) };
    }

    // The margin keeps a skipped entry's coverage at exactly 0 through either side's rounding of its sdf.
    float
    squaredBound(double radius,
                 double softness,
                 double footprint)
    {
        const double bound = (radius + std::max(softness, 0.) + footprint) * (1. + 1e-3) + 1e-4;

        return (float)(bound * bound);
    }

    class Words {
    public:
        explicit Words(std::vector<uint32_t>& words)
            : _w(words)
        {
        }

        void setFloat(uint32_t index,
                      double value)
        {
            _w[index] = std::bit_cast<uint32_t>((float)value);
        }

        void setUint(uint32_t index,
                     uint32_t value)
        {
            _w[index] = value;
        }

    private:
        std::vector<uint32_t>& _w;
    };

    class Reader {
    public:
        explicit Reader(const uint32_t* words)
            : _w(words)
        {
        }

        float f(uint32_t index) const
        {
            return std::bit_cast<float>(_w[index]);
        }

        uint32_t u(uint32_t index) const
        {
            return _w[index];
        }

    private:
        const uint32_t* _w;
    };

    float
    coverage(float sdf,
             float edge,
             float invSpan)
    {
        if (invSpan > 0.f) {
            float t = (sdf - edge) * invSpan;
            t = std::min(std::max(t, 0.f), 1.f);
            const float tt = t * t;
            const float s = 3.f - 2.f * t;

            return 1.f - tt * s;
        }

        return sdf < edge ? 1.f : 0.f;
    }

    float
    rectangleSdf(float px,
                 float py,
                 float hx,
                 float hy)
    {
        const float ex = std::fabs(px) - hx;
        const float ey = std::fabs(py) - hy;
        const float ox = std::max(ex, 0.f);
        const float oy = std::max(ey, 0.f);
        const float oxx = ox * ox;
        const float oyy = oy * oy;
        const float outside = std::sqrt(oxx + oyy);
        const float inside = std::min(std::max(ex, ey), 0.f);

        return outside + inside;
    }

    // realflare's sin(pi * fract(blades * atan(-x, -y) / (2 pi) + blades / 2 + 1 / 2)), without the
    // trigonometry: with a the half angle, that is |cos(blades a)| for an even count and |sin(blades a)|
    // for an odd one, and the power of (cos a, sin a) gives both.
    float
    roundnessGradient(float px,
                      float py,
                      uint32_t blades)
    {
        const float pxx = px * px;
        const float pyy = py * py;
        const float r2 = pxx + pyy;
        float c = 1.f;
        if (r2 > 0.f) {
            const float r = std::sqrt(r2);
            c = -py / r;
            c = std::min(std::max(c, -1.f), 1.f);
        }
        const float ca = std::sqrt(std::max((1.f + c) * 0.5f, 0.f));
        const float sa = std::sqrt(std::max((1.f - c) * 0.5f, 0.f));
        float re = 1.f;
        float im = 0.f;
        for (uint32_t i = 0; i < blades; ++i) {
            const float a = re * ca;
            const float b = im * sa;
            const float d = re * sa;
            const float e = im * ca;
            re = a - b;
            im = d + e;
        }

        return (blades % 2u) == 0u ? std::fabs(re) : std::fabs(im);
    }

    float
    shapeIntensity(const Reader& t,
                   float px,
                   float py)
    {
        const uint32_t blades = t.u(kApertureBlades);
        const uint32_t bladeOffset = t.u(kApertureBladeOffset);
        float sdf = 0.f;
        for (uint32_t i = 0; i < blades; ++i) {
            const uint32_t e = bladeOffset + i * kApertureBladeWords;
            const float ax = t.f(e) * px;
            const float ay = t.f(e + 1) * py;
            sdf = std::max(sdf, ax + ay);
        }
        const float roundness = t.f(kApertureRoundness);
        if (roundness != 0.f) {
            const float g = roundnessGradient(px, py, blades) * roundness;
            sdf = sdf + g;
        }

        return coverage(sdf, t.f(kApertureShapeEdge), t.f(kApertureShapeInvSpan));
    }

    float
    imageTexel(const ApertureImage& image,
               int x,
               int y)
    {
        if (x < 0 || y < 0 || x >= image.width || y >= image.height) {
            return 0.f;
        }

        return image.pixels[(std::size_t)y * image.width + x];
    }

    float
    imageIntensity(const Reader& t,
                   const ApertureImage& image,
                   float nx,
                   float ny)
    {
        const float w = (float)image.width;
        const float h = (float)image.height;
        const float sx = nx * t.f(kApertureImageScaleX);
        const float sy = ny * t.f(kApertureImageScaleY);
        const float tx = std::min(std::max(sx + t.f(kApertureImageBiasX), -2.f), w + 1.f);
        const float ty = std::min(std::max(sy + t.f(kApertureImageBiasY), -2.f), h + 1.f);
        const float x0 = std::floor(tx);
        const float y0 = std::floor(ty);
        const float fx = tx - x0;
        const float fy = ty - y0;
        const int ix = (int)x0;
        const int iy = (int)y0;
        const float a = imageTexel(image, ix, iy);
        const float b = imageTexel(image, ix + 1, iy);
        const float c = imageTexel(image, ix, iy + 1);
        const float d = imageTexel(image, ix + 1, iy + 1);
        const float r0 = a + (b - a) * fx;
        const float r1 = c + (d - c) * fx;
        float v = r0 + (r1 - r0) * fy;
        if (v < t.f(kApertureImageBlack)) {
            v = 0.f;
        }
        if (v > t.f(kApertureImageWhite)) {
            v = 1.f;
        }
        const float lost = t.f(kApertureImageStrength) * (1.f - v);

        return 1.f - std::min(std::max(lost, 0.f), 1.f);
    }

    void
    applyLayer(float strength,
               const float* sums,
               float* values)
    {
        for (int s = 0; s < kSamplesPerPixel; ++s) {
            const float lost = sums[s] * strength;
            values[s] = values[s] * (1.f - std::min(std::max(lost, 0.f), 1.f));
        }
    }

    bool
    culled(float cx,
           float cy,
           float bx,
           float by,
           float bound)
    {
        const float dx = cx - bx;
        const float dy = cy - by;
        const float dxx = dx * dx;
        const float dyy = dy * dy;

        return dxx + dyy > bound;
    }

    void
    rasterisePixel(const Reader& t,
                   const ApertureImage& image,
                   int x,
                   int y,
                   float* out)
    {
        const float ndcScaleX = t.f(kApertureNdcScaleX);
        const float ndcScaleY = t.f(kApertureNdcScaleY);
        const float invSizeX = t.f(kApertureInvSizeX);
        const float invSizeY = t.f(kApertureInvSizeY);
        const float shapeCos = t.f(kApertureShapeCos);
        const float shapeSin = t.f(kApertureShapeSin);
        const float scratchesCos = t.f(kApertureScratchesCos);
        const float scratchesSin = t.f(kApertureScratchesSin);

        float nx[kApertureSamples];
        float ny[kApertureSamples];
        for (int k = 0; k < kApertureSamples; ++k) {
            const float ox = (float)x + (float)k * 0.125f;
            const float oy = (float)y + (float)k * 0.125f;
            nx[k] = ox * ndcScaleX - 1.f;
            ny[k] = oy * ndcScaleY - 1.f;
        }

        float px[kSamplesPerPixel];
        float py[kSamplesPerPixel];
        float values[kSamplesPerPixel];
        float sums[kSamplesPerPixel];
        for (int sy = 0; sy < kApertureSamples; ++sy) {
            for (int sx = 0; sx < kApertureSamples; ++sx) {
                const int s = sy * kApertureSamples + sx;
                px[s] = nx[sx] * invSizeX;
                py[s] = ny[sy] * invSizeY;
                const float a = shapeCos * px[s];
                const float b = shapeSin * py[s];
                const float c = shapeSin * px[s];
                const float d = shapeCos * py[s];
                values[s] = shapeIntensity(t, a - b, c + d);
            }
        }

        const float centreX = ((float)x + 0.5f) * ndcScaleX - 1.f;
        const float centreY = ((float)y + 0.5f) * ndcScaleY - 1.f;
        const float cx = centreX * invSizeX;
        const float cy = centreY * invSizeY;

        const float gratingStrength = t.f(kApertureGratingStrength);
        if (gratingStrength > 0.f) {
            const uint32_t count = t.u(kApertureGratingCount);
            const uint32_t offset = t.u(kApertureGratingOffset);
            const float halfW = t.f(kApertureGratingHalfW);
            const float halfL = t.f(kApertureGratingHalfL);
            const float edge = t.f(kApertureGratingEdge);
            const float invSpan = t.f(kApertureGratingInvSpan);
            const float bound = t.f(kApertureGratingBound);
            std::fill(sums, sums + kSamplesPerPixel, 0.f);
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t e = offset + i * kApertureGratingWords;
                if (culled(cx, cy, t.f(e + 2), t.f(e + 3), bound)) {
                    continue;
                }
                const float bc = t.f(e);
                const float bs = t.f(e + 1);
                for (int s = 0; s < kSamplesPerPixel; ++s) {
                    const float a = bc * px[s];
                    const float b = bs * py[s];
                    const float c = bs * px[s];
                    const float d = bc * py[s];
                    const float rx = a - b;
                    const float ry = (c + d) - 1.5f;
                    sums[s] = sums[s] + coverage(rectangleSdf(rx, ry, halfW, halfL), edge, invSpan);
                }
            }
            applyLayer(gratingStrength, sums, values);
        }

        const float scratchesStrength = t.f(kApertureScratchesStrength);
        if (scratchesStrength > 0.f) {
            const uint32_t count = t.u(kApertureScratchesCount);
            const uint32_t offset = t.u(kApertureScratchesOffset);
            const float halfW = t.f(kApertureScratchesHalfW);
            const float halfL = t.f(kApertureScratchesHalfL);
            const float edge = t.f(kApertureScratchesEdge);
            const float invSpan = t.f(kApertureScratchesInvSpan);
            const float bound = t.f(kApertureScratchesBound);
            float qx[kSamplesPerPixel];
            float qy[kSamplesPerPixel];
            for (int s = 0; s < kSamplesPerPixel; ++s) {
                const float a = scratchesCos * px[s];
                const float b = scratchesSin * py[s];
                const float c = scratchesSin * px[s];
                const float d = scratchesCos * py[s];
                qx[s] = a - b;
                qy[s] = c + d;
            }
            const float ca = scratchesCos * cx;
            const float cb = scratchesSin * cy;
            const float cc = scratchesSin * cx;
            const float cd = scratchesCos * cy;
            const float qcx = ca - cb;
            const float qcy = cc + cd;
            std::fill(sums, sums + kSamplesPerPixel, 0.f);
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t e = offset + i * kApertureScratchWords;
                if (culled(qcx, qcy, t.f(e + 4), t.f(e + 5), bound)) {
                    continue;
                }
                const float ox = t.f(e);
                const float oy = t.f(e + 1);
                const float rc = t.f(e + 2);
                const float rs = t.f(e + 3);
                for (int s = 0; s < kSamplesPerPixel; ++s) {
                    const float a = rc * qx[s];
                    const float b = rs * qy[s];
                    const float c = rs * qx[s];
                    const float d = rc * qy[s];
                    const float rx = (a - b) - ox;
                    const float ry = (c + d) - oy;
                    sums[s] = sums[s] + coverage(rectangleSdf(rx, ry, halfW, halfL), edge, invSpan);
                }
            }
            applyLayer(scratchesStrength, sums, values);
        }

        const float dustStrength = t.f(kApertureDustStrength);
        if (dustStrength > 0.f) {
            const uint32_t count = t.u(kApertureDustCount);
            const uint32_t offset = t.u(kApertureDustOffset);
            const float radius = t.f(kApertureDustRadius);
            const float radius2 = t.f(kApertureDustRadiusSquared);
            const float edge = t.f(kApertureDustEdge);
            const float invSpan = t.f(kApertureDustInvSpan);
            const float bound = t.f(kApertureDustBound);
            std::fill(sums, sums + kSamplesPerPixel, 0.f);
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t e = offset + i * kApertureDustWords;
                const float ox = t.f(e);
                const float oy = t.f(e + 1);
                if (culled(cx, cy, ox, oy, bound)) {
                    continue;
                }
                for (int s = 0; s < kSamplesPerPixel; ++s) {
                    const float rx = px[s] - ox;
                    const float ry = py[s] - oy;
                    const float rxx = rx * rx;
                    const float ryy = ry * ry;
                    const float d2 = rxx + ryy;
                    float cover;
                    if (invSpan > 0.f) {
                        cover = coverage(std::sqrt(d2) - radius, edge, invSpan);
                    } else {
                        // Squared, so that the hard edge does not depend on either side's square root.
                        cover = d2 < radius2 ? 1.f : 0.f;
                    }
                    sums[s] = sums[s] + cover;
                }
            }
            applyLayer(dustStrength, sums, values);
        }

        if (t.f(kApertureImageStrength) > 0.f && t.u(kApertureImageWidth) > 0u && !image.empty()) {
            for (int sy = 0; sy < kApertureSamples; ++sy) {
                for (int sx = 0; sx < kApertureSamples; ++sx) {
                    const int s = sy * kApertureSamples + sx;
                    values[s] = values[s] * imageIntensity(t, image, nx[sx], ny[sy]);
                }
            }
        }

        float total = 0.f;
        for (int s = 0; s < kSamplesPerPixel; ++s) {
            total = total + values[s];
        }
        *out = total * (1.f / (float)kSamplesPerPixel);
    }
} // anonymous namespace

std::vector<uint32_t>
buildApertureTables(const ApertureParams& p,
                    const ApertureFrame& frame,
                    int imageWidth,
                    int imageHeight)
{
    if (frame.width <= 0 || frame.height <= 0) {
        return std::vector<uint32_t>();
    }
    const uint32_t blades = (uint32_t)std::max(p.blades, 2);
    const uint32_t gratingCount = p.gratingStrength > 0. ? layerCount(p.gratingDensity, kApertureMaxGratingBars) : 0u;
    const uint32_t scratchesCount = p.scratchesStrength > 0. ? layerCount(p.scratchesDensity, kApertureMaxScratches) : 0u;
    const uint32_t dustCount = p.dustStrength > 0. ? layerCount(p.dustDensity, kApertureMaxDust) : 0u;
    const bool hasImage = p.imageStrength > 0. && imageWidth > 0 && imageHeight > 0;

    const uint32_t bladeOffset = kApertureHeaderWords;
    const uint32_t gratingOffset = bladeOffset + blades * kApertureBladeWords;
    const uint32_t scratchesOffset = gratingOffset + gratingCount * kApertureGratingWords;
    const uint32_t dustOffset = scratchesOffset + scratchesCount * kApertureScratchWords;
    std::vector<uint32_t> words(dustOffset + dustCount * kApertureDustWords, 0u);
    Words w(words);

    const double invSizeX = 1. / std::max(finiteOr(p.sizeX, 0.), 1e-8);
    const double invSizeY = 1. / std::max(finiteOr(p.sizeY, 0.), 1e-8);
    const double pixelX = 2. / frame.width * invSizeX;
    const double pixelY = 2. / frame.height * invSizeY;
    const double footprint = 0.5 * std::sqrt(pixelX * pixelX + pixelY * pixelY);

    w.setUint(kApertureWidth, (uint32_t)frame.width);
    w.setUint(kApertureHeight, (uint32_t)frame.height);
    w.setFloat(kApertureNdcScaleX, 2. / frame.width);
    w.setFloat(kApertureNdcScaleY, 2. / frame.height);
    w.setFloat(kApertureInvSizeX, invSizeX);
    w.setFloat(kApertureInvSizeY, invSizeY);
    w.setFloat(kApertureFootprint, footprint);

    const double shapeRotation = finiteOr(p.rotation, 0.) * kPi / 180.;
    w.setUint(kApertureBlades, blades);
    w.setFloat(kApertureRoundness, finiteOr(p.roundness, 0.));
    w.setFloat(kApertureShapeCos, std::cos(shapeRotation));
    w.setFloat(kApertureShapeSin, std::sin(shapeRotation));
    const Edge shapeEdge = makeEdge(1., finiteOr(p.softness, 0.) / 10.);
    w.setFloat(kApertureShapeEdge, shapeEdge.edge);
    w.setFloat(kApertureShapeInvSpan, shapeEdge.invSpan);
    for (uint32_t i = 0; i < blades; ++i) {
        const double angle = ((double)i / blades + 0.25) * 2. * kPi;
        w.setFloat(bladeOffset + i * kApertureBladeWords, std::cos(angle));
        w.setFloat(bladeOffset + i * kApertureBladeWords + 1, std::sin(angle));
    }

    {
        const double halfW = std::max(finiteOr(p.gratingWidth, 0.), 0.) * 0.1 / 2.;
        const double halfL = std::max(finiteOr(p.gratingLength, 0.), 0.);
        const double softness = finiteOr(p.gratingSoftness, 0.) / 10.;
        const Edge edge = makeEdge(0., softness);
        w.setFloat(kApertureGratingStrength, gratingCount > 0 ? finiteOr(p.gratingStrength, 0.) : 0.);
        w.setUint(kApertureGratingCount, gratingCount);
        w.setFloat(kApertureGratingHalfW, halfW);
        w.setFloat(kApertureGratingHalfL, halfL);
        w.setFloat(kApertureGratingEdge, edge.edge);
        w.setFloat(kApertureGratingInvSpan, edge.invSpan);
        w.setFloat(kApertureGratingBound, squaredBound(std::sqrt(halfW * halfW + halfL * halfL), softness, footprint));
        for (uint32_t i = 0; i < gratingCount; ++i) {
            const double angle = (double)(i + 1) / gratingCount * 2. * kPi;
            const uint32_t e = gratingOffset + i * kApertureGratingWords;
            w.setFloat(e, std::cos(angle));
            w.setFloat(e + 1, std::sin(angle));
            w.setFloat(e + 2, 1.5 * std::sin(angle));
            w.setFloat(e + 3, 1.5 * std::cos(angle));
        }
    }

    {
        const double halfW = std::max(finiteOr(p.scratchesWidth, 0.), 0.) * 0.1 / 2.;
        const double halfL = std::max(finiteOr(p.scratchesLength, 0.), 0.) / 2.;
        const double softness = finiteOr(p.scratchesSoftness, 0.) / 10.;
        const Edge edge = makeEdge(0., softness);
        const double rotation = finiteOr(p.scratchesRotation, 0.) * kPi / 180.;
        const double rc = std::cos(rotation);
        const double rs = std::sin(rotation);
        const double parallaxX = finiteOr(frame.lightX * p.scratchesParallaxX, 0.);
        const double parallaxY = finiteOr(frame.lightY * p.scratchesParallaxY, 0.);
        const double shiftX = rc * parallaxX - rs * parallaxY;
        const double shiftY = rs * parallaxX + rc * parallaxY;
        const double variation = finiteOr(p.scratchesRotationVariation, 0.) * kPi;
        w.setFloat(kApertureScratchesStrength, scratchesCount > 0 ? finiteOr(p.scratchesStrength, 0.) : 0.);
        w.setUint(kApertureScratchesCount, scratchesCount);
        w.setFloat(kApertureScratchesHalfW, halfW);
        w.setFloat(kApertureScratchesHalfL, halfL);
        w.setFloat(kApertureScratchesEdge, edge.edge);
        w.setFloat(kApertureScratchesInvSpan, edge.invSpan);
        w.setFloat(kApertureScratchesBound, squaredBound(std::sqrt(halfW * halfW + halfL * halfL), softness, footprint));
        w.setFloat(kApertureScratchesCos, rc);
        w.setFloat(kApertureScratchesSin, rs);
        for (uint32_t i = 0; i < scratchesCount; ++i) {
            const double ox = wrap01(random01(kScratchesSalt, i, 0) + shiftX) * 2. - 1.;
            const double oy = wrap01(random01(kScratchesSalt, i, 1) + shiftY) * 2. - 1.;
            const double angle = (random01(kScratchesSalt, i, 2) - 0.5) * variation;
            const double ac = std::cos(angle);
            const double as = std::sin(angle);
            const uint32_t e = scratchesOffset + i * kApertureScratchWords;
            w.setFloat(e, ox);
            w.setFloat(e + 1, oy);
            w.setFloat(e + 2, ac);
            w.setFloat(e + 3, as);
            w.setFloat(e + 4, ac * ox + as * oy);
            w.setFloat(e + 5, -as * ox + ac * oy);
        }
    }

    {
        const double radius = std::max(finiteOr(p.dustRadius, 0.), 0.) * 0.1;
        const double softness = finiteOr(p.dustSoftness, 0.) / 10.;
        const Edge edge = makeEdge(0., softness);
        const double shiftX = finiteOr(frame.lightX * p.dustParallaxX, 0.);
        const double shiftY = finiteOr(frame.lightY * p.dustParallaxY, 0.);
        const float radiusF = (float)radius;
        w.setFloat(kApertureDustStrength, dustCount > 0 ? finiteOr(p.dustStrength, 0.) : 0.);
        w.setUint(kApertureDustCount, dustCount);
        w.setFloat(kApertureDustRadius, radiusF);
        w.setFloat(kApertureDustRadiusSquared, radiusF * radiusF);
        w.setFloat(kApertureDustEdge, edge.edge);
        w.setFloat(kApertureDustInvSpan, edge.invSpan);
        w.setFloat(kApertureDustBound, squaredBound(radius, softness, footprint));
        for (uint32_t i = 0; i < dustCount; ++i) {
            const uint32_t e = dustOffset + i * kApertureDustWords;
            w.setFloat(e, wrap01(random01(kDustSalt, i, 0) + shiftX) * 2. - 1.);
            w.setFloat(e + 1, wrap01(random01(kDustSalt, i, 1) + shiftY) * 2. - 1.);
        }
    }

    if (hasImage) {
        const double invX = 1. / std::max(finiteOr(p.imageSizeX, 0.), 1e-8);
        const double invY = 1. / std::max(finiteOr(p.imageSizeY, 0.), 1e-8);
        w.setFloat(kApertureImageStrength, finiteOr(p.imageStrength, 0.));
        w.setUint(kApertureImageWidth, (uint32_t)imageWidth);
        w.setUint(kApertureImageHeight, (uint32_t)imageHeight);
        w.setFloat(kApertureImageScaleX, 0.5 * imageWidth * invX);
        w.setFloat(kApertureImageScaleY, 0.5 * imageHeight * invY);
        w.setFloat(kApertureImageBiasX, 0.5 * imageWidth - 0.5);
        w.setFloat(kApertureImageBiasY, 0.5 * imageHeight - 0.5);
        w.setFloat(kApertureImageBlack, finiteOr(p.imageBlack, 0.));
        w.setFloat(kApertureImageWhite, finiteOr(p.imageWhite, 1.));
    }

    w.setUint(kApertureBladeOffset, bladeOffset);
    w.setUint(kApertureGratingOffset, gratingOffset);
    w.setUint(kApertureScratchesOffset, scratchesOffset);
    w.setUint(kApertureDustOffset, dustOffset);

    return words;
} // buildApertureTables

void
rasteriseApertureRows(const std::vector<uint32_t>& tables,
                      const ApertureImage& image,
                      int y1,
                      int y2,
                      float* out,
                      std::size_t outStride)
{
    if (tables.size() < kApertureHeaderWords) {
        return;
    }
    const Reader t(tables.data());
    const int width = (int)t.u(kApertureWidth);
    const int height = (int)t.u(kApertureHeight);
    const bool imageMatches = (int)t.u(kApertureImageWidth) == image.width && (int)t.u(kApertureImageHeight) == image.height;
    const ApertureImage used = imageMatches ? image : ApertureImage();
    for (int y = std::max(y1, 0); y < std::min(y2, height); ++y) {
        float* row = out + (std::size_t)(y - y1) * outStride;
        for (int x = 0; x < width; ++x) {
            rasterisePixel(t, used, x, y, row + x);
        }
    }
}

bool
rasteriseAperture(const ApertureParams& params,
                  const ApertureFrame& frame,
                  const ApertureImage& image,
                  float* out,
                  int maxThreads,
                  const std::function<bool()>& aborted)
{
    if (frame.width <= 0 || frame.height <= 0) {
        return true;
    }
    const std::vector<uint32_t> tables = buildApertureTables(params, frame, image.empty() ? 0 : image.width, image.empty() ? 0 : image.height);
    std::atomic<bool> stopped(false);
    parallelForOnGlobalPool(frame.height, maxThreads, [&](int y) {
        if (stopped.load(std::memory_order_relaxed)) {
            return;
        }
        if (aborted && aborted()) {
            stopped.store(true, std::memory_order_relaxed);

            return;
        }
        rasteriseApertureRows(tables, image, y, y + 1, out + (std::size_t)y * frame.width, (std::size_t)frame.width);
    });

    return !stopped.load();
}

} // namespace Optics

NATRON_NAMESPACE_EXIT
