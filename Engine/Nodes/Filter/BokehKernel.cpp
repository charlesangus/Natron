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

#include "BokehKernel.h"

#include <algorithm>
#include <cmath>
#include <utility>

NATRON_NAMESPACE_ENTER

namespace {
// Samples per side of an edge pixel's supersampling grid.
const int kEdgeSamples = 16;

// A radius beyond which no kernel grows, which keeps the extents' integer conversions finite.
const double kMaxRadius = 1e5;

// The shape in coordinates where the circumscribed circle has radius 1.
struct ShapeTest {
    bool polygon = false;
    int blades = 0;
    double roundness = 1.;
    double angle0 = 0.;
    double apothem = 1.;
    double sector = 0.;
    // The largest radius the shape contains whole in every direction.
    double inner = 1.;

    bool inside(double u,
                double v) const
    {
        const double rho = std::sqrt(u * u + v * v);
        if (!polygon) {
            return rho <= 1.;
        }
        double phi = std::atan2(v, u) - angle0;
        phi -= sector * std::floor(phi / sector);
        const double edge = apothem / std::cos(phi - sector / 2.);
        // At roundness 1 this is exactly 1, so the shape is exactly the disc.
        const double boundary = (1. - roundness) * edge + roundness;

        return rho <= boundary;
    }
};

ShapeTest
makeShapeTest(const BokehShapeParams& params)
{
    ShapeTest test;

    if (params.shape != BokehShape::Blades) {
        return test;
    }
    test.polygon = true;
    test.blades = std::max(3, params.blades);
    test.roundness = std::min(1., std::max(0., std::isfinite(params.roundness) ? params.roundness : 0.));
    test.sector = 2. * M_PI / test.blades;
    test.apothem = std::cos(M_PI / test.blades);
    // A corner points up at rotation 0.
    test.angle0 = M_PI / 2. + (std::isfinite(params.rotation) ? params.rotation : 0.) * M_PI / 180.;
    test.inner = (1. - test.roundness) * test.apothem + test.roundness;

    return test;
}

// The distance from 0 to the nearest and to the farthest point of [a, b].
void
intervalDistances(double a,
                  double b,
                  double* nearest,
                  double* farthest)
{
    *nearest = (a > 0.) ? a : ((b < 0.) ? -b : 0.);
    *farthest = std::max(std::fabs(a), std::fabs(b));
}

double
clampRadius(double r)
{
    return std::isfinite(r) ? std::min(kMaxRadius, std::max(0.5, r)) : 0.5;
}

float
kernelValue(float v)
{
    return (std::isfinite(v) && (v > 0.f)) ? v : 0.f;
}

double
finiteOr(double v,
         double fallback)
{
    return std::isfinite(v) ? v : fallback;
}

double
clamp01(double v,
        double fallback)
{
    return std::min(1., std::max(0., finiteOr(v, fallback)));
}

// The clip disc in the coordinates of ShapeTest.
struct ClipTest {
    bool active = false;
    double cx = 0.;
    double cy = 0.;
    double radius = 0.;

    bool inside(double u,
                double v) const
    {
        const double du = u - cx;
        const double dv = v - cy;

        return du * du + dv * dv <= radius * radius;
    }

    // How much of the box [u0, u1] x [v0, v1] the clip covers: 0 none, 2 all, 1 part of it.
    int classify(double u0,
                 double u1,
                 double v0,
                 double v1) const
    {
        if (!active) {
            return 2;
        }
        double nearU, farU, nearV, farV;
        intervalDistances(u0 - cx, u1 - cx, &nearU, &farU);
        intervalDistances(v0 - cy, v1 - cy, &nearV, &farV);
        if (nearU * nearU + nearV * nearV >= radius * radius) {
            return 0;
        }

        return (farU * farU + farV * farV <= radius * radius) ? 2 : 1;
    }
};

// A clip that holds the disc of radius reach about 0 changes nothing, and is left out.
ClipTest
makeClipTest(const BokehClip& clip,
             double reach)
{
    ClipTest test;

    if (std::isnan(clip.radius) || !std::isfinite(clip.offsetX) || !std::isfinite(clip.offsetY)) {
        return test;
    }
    if (std::isfinite(clip.radius) && (std::hypot(clip.offsetX, clip.offsetY) + reach > clip.radius)) {
        test.active = true;
        test.cx = clip.offsetX;
        test.cy = clip.offsetY;
        test.radius = std::max(0., clip.radius);
    }

    return test;
}

// The texture over the shape: rho is the radius in ShapeTest's coordinates, s the radius as a
// fraction of the shape's boundary in that direction.
struct Texture {
    bool active = false;
    bool gaussian = false;
    bool hole = false;
    double holeRadius = 0.;
    bool ring = false;
    double ringInner = 0.;
    double ringOuter = 0.;
    double ringBrightness = 1.;
    double spherical = 0.;
    double discMix = 1.;
    double gaussianScale = 0.;

    double value(double rho,
                 double s) const
    {
        if (hole && (rho <= holeRadius)) {
            return 0.;
        }
        double t = 1.;
        if (ring) {
            if (s <= ringInner) {
                t = ringBrightness;
            } else if (s < ringOuter) {
                t = ringBrightness + (1. - ringBrightness) * (s - ringInner) / (ringOuter - ringInner);
            }
        }
        if (spherical != 0.) {
            t *= std::max(0., 1. + spherical * (2. * rho * rho - 1.));
        }

        return t;
    }

    double gauss(double rho) const
    {
        return std::exp(-rho * rho * gaussianScale);
    }

    // Whether the texture has an edge or a kink between radii rho0 and rho1, or s0 and s1.
    bool changesWithin(double rho0,
                       double rho1,
                       double s0,
                       double s1) const
    {
        const auto straddles = [](double a, double b, double e) {
            return (a < e) && (b > e);
        };

        return (hole && straddles(rho0, rho1, holeRadius))
            || (ring && (straddles(s0, s1, ringInner) || straddles(s0, s1, ringOuter)));
    }
};

// The Gaussian's standard deviation as a fraction of the radius: 3 sigma reaches the rim.
const double kGaussianSigma = 1. / 3.;

Texture
makeTexture(const BokehShapeParams& params)
{
    Texture tex;

    tex.discMix = clamp01(params.filterShape, 1.);
    tex.gaussian = (tex.discMix < 1.);
    tex.gaussianScale = 1. / (2. * kGaussianSigma * kGaussianSigma);
    tex.hole = params.catadioptric && (finiteOr(params.catadioptricSize, 0.) > 0.);
    tex.holeRadius = tex.hole ? clamp01(params.catadioptricSize, 0.) : 0.;
    const double brightness = std::max(0., finiteOr(params.innerBrightness, 1.));
    tex.ring = (brightness != 1.);
    if (tex.ring) {
        const double size = clamp01(params.innerSize, 0.8);
        const double feather = clamp01(params.innerFeather, 0.5);
        tex.ringBrightness = brightness;
        tex.ringInner = (feather < 0.5) ? 2. * feather * size : size;
        tex.ringOuter = (feather > 0.5) ? size + (2. * feather - 1.) * (1. - size) : size;
    }
    tex.spherical = std::min(1., std::max(-1., finiteOr(params.sphericalAberration, 0.)));
    tex.active = tex.hole || tex.ring || (tex.spherical != 0.);

    return tex;
}

// The radius of (u, v) as a fraction of the shape's boundary in its direction.
double
shapeRadius(const ShapeTest& test,
            double u,
            double v)
{
    const double rho = std::sqrt(u * u + v * v);

    if (!test.polygon) {
        return rho;
    }
    double phi = std::atan2(v, u) - test.angle0;
    phi -= test.sector * std::floor(phi / test.sector);
    const double edge = test.apothem / std::cos(phi - test.sector / 2.);

    return rho / ((1. - test.roundness) * edge + test.roundness);
}

// The kernel weights of shaped(), unnormalised; false when they sum to 0.
bool
shapeWeights(const ShapeTest& test,
             const ClipTest& clip,
             const Texture& tex,
             double rx,
             double ry,
             int hx,
             int hy,
             std::vector<float>* out)
{
    const int w = 2 * hx + 1;
    const int h = 2 * hy + 1;
    const bool plain = !clip.active && !tex.active && !tex.gaussian;
    std::vector<float>& weights = *out;
    weights.assign((std::size_t)w * h, 0.f);
    std::vector<double> gaussWeights;
    if (tex.gaussian) {
        gaussWeights.assign((std::size_t)w * h, 0.);
    }
    double sum = 0.;
    double gaussSum = 0.;

    for (int dy = -hy; dy <= hy; ++dy) {
        const double v0 = (dy - 0.5) / ry;
        const double v1 = (dy + 0.5) / ry;
        double nearV, farV;
        intervalDistances(v0, v1, &nearV, &farV);
        for (int dx = -hx; dx <= hx; ++dx) {
            const double u0 = (dx - 0.5) / rx;
            const double u1 = (dx + 0.5) / rx;
            double nearU, farU;
            intervalDistances(u0, u1, &nearU, &farU);
            const std::size_t index = (std::size_t)(dy + hy) * w + (dx + hx);
            if (nearU * nearU + nearV * nearV >= 1.) {
                // The shape lies within the unit circle.
                continue;
            }
            const int clipCover = clip.classify(u0, u1, v0, v1);
            if (clipCover == 0) {
                continue;
            }
            const double rhoNear = std::sqrt(nearU * nearU + nearV * nearV);
            const double rhoFar = std::sqrt(farU * farU + farV * farV);
            const bool whole = (rhoFar <= test.inner) && (clipCover == 2);
            if (plain) {
                float coverage;
                if (whole) {
                    coverage = 1.f;
                } else {
                    // Offsets from the pixel's centre, symmetric bit for bit, so that a symmetric
                    // shape gives a symmetric kernel.
                    int count = 0;
                    for (int j = 0; j < kEdgeSamples; ++j) {
                        const double v = (dy + ((j + 0.5) / kEdgeSamples - 0.5)) / ry;
                        for (int i = 0; i < kEdgeSamples; ++i) {
                            const double u = (dx + ((i + 0.5) / kEdgeSamples - 0.5)) / rx;
                            if (test.inside(u, v)) {
                                ++count;
                            }
                        }
                    }
                    coverage = (float)count / (float)(kEdgeSamples * kEdgeSamples);
                }
                weights[index] = coverage;
                sum += coverage;
                continue;
            }
            // The fraction of the boundary is at least rho and at most rho over the inner radius.
            if (whole && !tex.changesWithin(rhoNear, rhoFar, rhoNear, rhoFar / test.inner)) {
                // Smooth over the pixel: its centre stands for it.
                const double u = dx / rx;
                const double v = dy / ry;
                const double rho = std::sqrt(u * u + v * v);
                const double t = tex.active ? tex.value(rho, shapeRadius(test, u, v)) : 1.;
                weights[index] = (float)t;
                sum += (float)t;
                if (tex.gaussian) {
                    gaussWeights[index] = t * tex.gauss(rho);
                    gaussSum += gaussWeights[index];
                }
                continue;
            }
            double acc = 0.;
            double gaussAcc = 0.;
            for (int j = 0; j < kEdgeSamples; ++j) {
                const double v = (dy + ((j + 0.5) / kEdgeSamples - 0.5)) / ry;
                for (int i = 0; i < kEdgeSamples; ++i) {
                    const double u = (dx + ((i + 0.5) / kEdgeSamples - 0.5)) / rx;
                    if (!test.inside(u, v) || ((clipCover == 1) && !clip.inside(u, v))) {
                        continue;
                    }
                    const double rho = std::sqrt(u * u + v * v);
                    const double t = tex.active ? tex.value(rho, shapeRadius(test, u, v)) : 1.;
                    acc += t;
                    if (tex.gaussian) {
                        gaussAcc += t * tex.gauss(rho);
                    }
                }
            }
            const double samples = (double)(kEdgeSamples * kEdgeSamples);
            weights[index] = (float)(acc / samples);
            sum += weights[index];
            if (tex.gaussian) {
                gaussWeights[index] = gaussAcc / samples;
                gaussSum += gaussWeights[index];
            }
        }
    }
    if (!(sum > 0.)) {
        return false;
    }
    if (tex.gaussian && (gaussSum > 0.)) {
        // Each normalised on its own, so that the dissolve keeps both at full strength.
        for (std::size_t i = 0; i < weights.size(); ++i) {
            weights[i] = (float)(tex.discMix * weights[i] / sum + (1. - tex.discMix) * gaussWeights[i] / gaussSum);
        }
    }

    return true;
} // shapeWeights
} // anonymous namespace

BokehKernel::BokehKernel()
    : _haloX(0)
    , _haloY(0)
    , _weights(1, 1.f)
{
}

BokehKernel::BokehKernel(int haloX,
                         int haloY,
                         std::vector<float> weights)
    : _haloX(haloX)
    , _haloY(haloY)
    , _weights(std::move(weights))
{
}

int
BokehKernel::extent(double radius)
{
    if (!std::isfinite(radius) || (radius < 0.5)) {
        return 0;
    }
    radius = std::min(radius, kMaxRadius);

    // The pixel at offset d spans [d - 0.5, d + 0.5], so it overlaps the radius while d - 0.5 < r.
    return std::max(0, (int)std::ceil(radius + 0.5) - 1);
}

bool
BokehKernel::isIdentityRadius(double radiusX,
                              double radiusY)
{
    const bool x = std::isfinite(radiusX) && (radiusX >= 0.5);
    const bool y = std::isfinite(radiusY) && (radiusY >= 0.5);

    return !x && !y;
}

bool
BokehKernel::isIdentity() const
{
    return (_haloX == 0) && (_haloY == 0);
}

float
BokehKernel::at(int dx,
                int dy) const
{
    if ((dx < -_haloX) || (dx > _haloX) || (dy < -_haloY) || (dy > _haloY)) {
        return 0.f;
    }

    return _weights[(std::size_t)(dy + _haloY) * width() + (dx + _haloX)];
}

bool
BokehKernel::normalise()
{
    double sum = 0.;

    for (float w : _weights) {
        sum += w;
    }
    if (!(sum > 0.) || !std::isfinite(sum)) {
        return false;
    }
    for (float& w : _weights) {
        w = (float)(w / sum);
    }

    return true;
}

BokehKernel
BokehKernel::shaped(const BokehShapeParams& shape,
                    double radiusX,
                    double radiusY)
{
    if (isIdentityRadius(radiusX, radiusY)) {
        return BokehKernel();
    }
    // A radius under half a pixel on one axis only reads as half a pixel, so that the shape still
    // covers a row or column of sample points.
    const double rx = clampRadius(radiusX);
    const double ry = clampRadius(radiusY);
    const int hx = extent(rx);
    const int hy = extent(ry);
    const ShapeTest test = makeShapeTest(shape);
    const ClipTest clip = makeClipTest(shape.clip, 1.);
    const Texture tex = makeTexture(shape);
    std::vector<float> weights;

    bool filled = shapeWeights(test, clip, tex, rx, ry, hx, hy, &weights);
    if (!filled && clip.active) {
        filled = shapeWeights(test, ClipTest(), tex, rx, ry, hx, hy, &weights);
    }
    if (!filled && (tex.active || tex.gaussian)) {
        filled = shapeWeights(test, ClipTest(), Texture(), rx, ry, hx, hy, &weights);
    }
    if (!filled) {
        return BokehKernel();
    }
    BokehKernel kernel(hx, hy, std::move(weights));
    if (!kernel.normalise()) {
        return BokehKernel();
    }

    return kernel;
} // BokehKernel::shaped

BokehKernel
BokehKernel::fromImage(const float* values,
                       int width,
                       int height,
                       double radiusX,
                       double radiusY,
                       bool* empty,
                       const BokehClip& clip)
{
    if (empty) {
        *empty = false;
    }
    if (isIdentityRadius(radiusX, radiusY)) {
        return BokehKernel();
    }
    if (!values || (width <= 0) || (height <= 0)) {
        if (empty) {
            *empty = true;
        }

        return BokehKernel();
    }
    const double rx = clampRadius(radiusX);
    const double ry = clampRadius(radiusY);
    const int hx = extent(rx);
    const int hy = extent(ry);
    const int kw = 2 * hx + 1;
    const int kh = 2 * hy + 1;
    const double longer = (double)std::max(width, height);
    // An image pixel spans stepX kernel pixels from x0 + i * stepX, the image centred on 0.
    const double stepX = 2. * rx / longer;
    const double stepY = 2. * ry / longer;
    const double x0 = -width * rx / longer;
    const double y0 = -height * ry / longer;

    // overlap(k, i): how much of kernel pixel k the image pixel i covers, along one axis.
    struct Span {
        int first = 0;
        std::vector<double> overlap;
    };
    const auto spans = [](int halo, int count, double origin, double step) {
        std::vector<Span> out((std::size_t)(2 * halo + 1));
        for (int k = 0; k < 2 * halo + 1; ++k) {
            const double a = (k - halo) - 0.5;
            const double b = a + 1.;
            const int first = std::max(0, (int)std::floor((a - origin) / step));
            const int last = std::min(count - 1, (int)std::ceil((b - origin) / step));
            Span& span = out[(std::size_t)k];
            span.first = first;
            for (int i = first; i <= last; ++i) {
                const double lo = std::max(a, origin + i * step);
                const double hi = std::min(b, origin + (i + 1) * step);
                span.overlap.push_back(std::max(0., hi - lo));
            }
        }

        return out;
    };
    const std::vector<Span> spansX = spans(hx, width, x0, stepX);
    const std::vector<Span> spansY = spans(hy, height, y0, stepY);

    // Resampled along x first, one image row at a time.
    std::vector<double> rows((std::size_t)height * kw, 0.);
    for (int j = 0; j < height; ++j) {
        const float* src = values + (std::size_t)j * width;
        for (int k = 0; k < kw; ++k) {
            const Span& span = spansX[(std::size_t)k];
            double sum = 0.;
            for (std::size_t n = 0; n < span.overlap.size(); ++n) {
                sum += span.overlap[n] * kernelValue(src[span.first + (int)n]);
            }
            rows[(std::size_t)j * kw + k] = sum;
        }
    }
    std::vector<float> weights((std::size_t)kw * kh, 0.f);
    for (int k = 0; k < kh; ++k) {
        const Span& span = spansY[(std::size_t)k];
        for (int c = 0; c < kw; ++c) {
            double sum = 0.;
            for (std::size_t n = 0; n < span.overlap.size(); ++n) {
                sum += span.overlap[n] * rows[(std::size_t)(span.first + (int)n) * kw + c];
            }
            weights[(std::size_t)k * kw + c] = (float)sum;
        }
    }
    // The image reaches the corners of [-width, width] x [-height, height] / longer.
    const ClipTest clipTest = makeClipTest(clip, std::hypot(width / longer, height / longer));
    if (clipTest.active) {
        std::vector<float> clipped(weights);
        double clippedSum = 0.;
        for (int dy = -hy; dy <= hy; ++dy) {
            const double v0 = (dy - 0.5) / ry;
            const double v1 = (dy + 0.5) / ry;
            for (int dx = -hx; dx <= hx; ++dx) {
                float& weight = clipped[(std::size_t)(dy + hy) * kw + (dx + hx)];
                if (weight == 0.f) {
                    continue;
                }
                const int cover = clipTest.classify((dx - 0.5) / rx, (dx + 0.5) / rx, v0, v1);
                if (cover == 0) {
                    weight = 0.f;
                } else if (cover == 1) {
                    int count = 0;
                    for (int j = 0; j < kEdgeSamples; ++j) {
                        const double v = (dy + ((j + 0.5) / kEdgeSamples - 0.5)) / ry;
                        for (int i = 0; i < kEdgeSamples; ++i) {
                            const double u = (dx + ((i + 0.5) / kEdgeSamples - 0.5)) / rx;
                            if (clipTest.inside(u, v)) {
                                ++count;
                            }
                        }
                    }
                    weight *= (float)count / (float)(kEdgeSamples * kEdgeSamples);
                }
                clippedSum += weight;
            }
        }
        if (clippedSum > 0.) {
            weights = std::move(clipped);
        }
    }
    BokehKernel kernel(hx, hy, std::move(weights));
    if (!kernel.normalise()) {
        if (empty) {
            *empty = true;
        }

        return BokehKernel();
    }

    return kernel;
} // BokehKernel::fromImage

std::vector<BokehRun>
BokehKernel::runs() const
{
    std::vector<BokehRun> out;
    const int w = width();

    for (int dy = -_haloY; dy <= _haloY; ++dy) {
        const float* row = &_weights[(std::size_t)(dy + _haloY) * w];
        int k = 0;
        while (k < w) {
            const float weight = row[k];
            if (weight == 0.f) {
                ++k;
                continue;
            }
            const int start = k;
            while ((k < w) && (row[k] == weight)) {
                ++k;
            }
            BokehRun run;
            run.dy = dy;
            run.dx0 = start - _haloX;
            run.length = k - start;
            run.weight = weight;
            out.push_back(run);
        }
    }

    return out;
}

void
bokehPrefixRow(const float* src,
               int width,
               double* table)
{
    double sum = 0.;

    table[0] = 0.;
    for (int x = 0; x < width; ++x) {
        // As on the GPU, so that a NaN or an infinity neither spreads along the row nor differs
        // between the two.
        if (std::isfinite(src[x])) {
            sum += src[x];
        }
        table[x + 1] = sum;
    }
}

void
bokehGatherRow(const double* tables,
               int width,
               int height,
               const std::vector<BokehRun>& runs,
               int y,
               int x1,
               int x2,
               double* acc,
               float* dst)
{
    const int n = x2 - x1;
    if (n <= 0) {
        return;
    }
    std::fill(acc, acc + n, 0.);
    const std::size_t stride = (std::size_t)width + 1;
    for (const BokehRun& run : runs) {
        const int yy = y - run.dy;
        if ((yy < 0) || (yy >= height)) {
            continue;
        }
        const double* table = tables + (std::size_t)yy * stride;
        const double weight = run.weight;
        // Pixel x sums the source over [x - dx0 - length + 1, x - dx0 + 1).
        const int lo = run.dx0 + run.length - 1;
        const int hi = run.dx0 - 1;
        for (int i = 0; i < n; ++i) {
            const int x = x1 + i;
            const int a = std::min(width, std::max(0, x - lo));
            const int b = std::min(width, std::max(0, x - hi));
            if (a < b) {
                acc[i] += weight * (table[b] - table[a]);
            }
        }
    }
    for (int i = 0; i < n; ++i) {
        dst[i] = (float)acc[i];
    }
}

NATRON_NAMESPACE_EXIT
