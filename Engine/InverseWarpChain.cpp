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

#include "InverseWarpChain.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "Engine/OverlayPointMap.h"

NATRON_NAMESPACE_ENTER

namespace {
// Grid points per axis pullBack() samples at most, which bounds its cost on large windows.
const double kPullBackSamplesPerAxis = 128.;

bool
isFiniteRegion(const RectD& r)
{
    return std::isfinite(r.x1) && std::isfinite(r.y1) && std::isfinite(r.x2) && std::isfinite(r.y2) && !r.isInfinite();
}
} // namespace

namespace InverseWarp {
template <typename T>
void
mapRowThroughMatrix(const double* m,
                    std::size_t n,
                    T* sx,
                    T* sy,
                    T* jacobian)
{
    const T nan = std::numeric_limits<T>::quiet_NaN();

    for (std::size_t i = 0; i < n; ++i) {
        const double x = sx[i];
        const double y = sy[i];
        const double w = m[6] * x + m[7] * y + m[8];
        if (!(w > 0.)) {
            sx[i] = nan;
            sy[i] = nan;
            continue;
        }
        const double qx = (m[0] * x + m[1] * y + m[2]) / w;
        const double qy = (m[3] * x + m[4] * y + m[5]) / w;
        sx[i] = (T)qx;
        sy[i] = (T)qy;
        if (!jacobian) {
            continue;
        }
        const double d00 = (m[0] - qx * m[6]) / w;
        const double d01 = (m[1] - qx * m[7]) / w;
        const double d10 = (m[3] - qy * m[6]) / w;
        const double d11 = (m[4] - qy * m[7]) / w;
        T* J = &jacobian[i * 4];
        const double j0 = J[0], j1 = J[1], j2 = J[2], j3 = J[3];
        J[0] = (T)(d00 * j0 + d01 * j2);
        J[1] = (T)(d00 * j1 + d01 * j3);
        J[2] = (T)(d10 * j0 + d11 * j2);
        J[3] = (T)(d10 * j1 + d11 * j3);
    }
}

template void mapRowThroughMatrix<float>(const double*, std::size_t, float*, float*, float*);
template void mapRowThroughMatrix<double>(const double*, std::size_t, double*, double*, double*);

double
largestSingularValue(double a,
                     double b,
                     double c,
                     double d)
{
    const double sum = a * a + b * b + c * c + d * d;
    const double det = a * d - b * c;
    const double disc = std::sqrt(std::max(0., sum * sum - 4. * det * det));

    return std::sqrt(std::max(0., (sum + disc) / 2.));
}

void
pushGpuDouble(std::vector<uint32_t>* words,
              double v)
{
    const float hi = (float)v;
    const float lo = (float)(v - (double)hi);
    uint32_t u;
    std::memcpy(&u, &hi, sizeof(u));
    words->push_back(u);
    std::memcpy(&u, &lo, sizeof(u));
    words->push_back(u);
}
} // namespace InverseWarp

InverseWarpMap::InverseWarpMap(const RenderScale& scale,
                               double par)
    : _scale(scale)
    , _par(par > 0. ? par : 1.)
{
}

InverseWarpMap::~InverseWarpMap()
{
}

RectD
InverseWarpMap::canonicalToPixel(const RectD& canonical) const
{
    const OfxPointD s = _scale.toOfxPointD();

    return RectD(canonical.x1 * s.x / _par, canonical.y1 * s.y, canonical.x2 * s.x / _par, canonical.y2 * s.y);
}

RectD
InverseWarpMap::pixelToCanonical(const RectD& pixel) const
{
    const OfxPointD s = _scale.toOfxPointD();

    return RectD(pixel.x1 * _par / s.x, pixel.y1 / s.y, pixel.x2 * _par / s.x, pixel.y2 / s.y);
}

uint32_t
InverseWarpMap::packGpu(std::vector<uint32_t>* /*words*/) const
{
    return 0;
}

RectD
InverseWarpMap::pullBack(const RectD& canonical,
                         double filterSupport) const
{
    if (canonical.isNull()) {
        return RectD();
    }
    if (!isFiniteRegion(canonical)) {
        return RectD(kOfxFlagInfiniteMin, kOfxFlagInfiniteMin, kOfxFlagInfiniteMax, kOfxFlagInfiniteMax);
    }
    const RectD pixel = canonicalToPixel(canonical);
    const double step = std::max(1., std::max(pixel.width(), pixel.height()) / kPullBackSamplesPerAxis);
    const std::vector<Point> grid = OverlayPointMap::sampleGrid(pixel, step);
    const std::size_t n = grid.size();
    std::vector<double> sx(n), sy(n), J(n * 4);
    for (std::size_t i = 0; i < n; ++i) {
        sx[i] = grid[i].x;
        sy[i] = grid[i].y;
        J[i * 4] = 1.;
        J[i * 4 + 1] = 0.;
        J[i * 4 + 2] = 0.;
        J[i * 4 + 3] = 1.;
    }
    mapRow(n, sx.data(), sy.data(), J.data());

    RectD bbox;
    bool any = false;
    // A kernel never covers less than a source pixel, however much the map magnifies.
    double kernelScale = 1.;
    for (std::size_t i = 0; i < n; ++i) {
        if (!std::isfinite(sx[i]) || !std::isfinite(sy[i])) {
            continue;
        }
        if (any) {
            bbox.merge(sx[i], sy[i], sx[i], sy[i]);
        } else {
            bbox.set(sx[i], sy[i], sx[i], sy[i]);
            any = true;
        }
        const double* d = &J[i * 4];
        if (std::isfinite(d[0]) && std::isfinite(d[1]) && std::isfinite(d[2]) && std::isfinite(d[3])) {
            kernelScale = std::max(kernelScale, InverseWarp::largestSingularValue(d[0], d[1], d[2], d[3]));
        }
    }
    if (!any) {
        return RectD();
    }
    // Every point of the region lies within step / sqrt(2) of a grid point, so its source lies
    // within that distance times the map's largest stretch of the grid point's source.
    const double pad = kernelScale * (step * M_SQRT1_2 + std::max(0., filterSupport));
    bbox.x1 -= pad;
    bbox.y1 -= pad;
    bbox.x2 += pad;
    bbox.y2 += pad;

    return pixelToCanonical(bbox);
} // InverseWarpMap::pullBack

MatrixInverseWarpMap::MatrixInverseWarpMap(const Transform::Matrix3x3& forward,
                                           const RenderScale& scale,
                                           double par)
    : InverseWarpMap(scale, par)
    , _forward(forward)
    , _inverse()
    , _invertible(false)
{
    const double det = Transform::matDeterminant(_forward);
    if (det != 0.) {
        _inverse = Transform::matInverse(_forward, det);
        _invertible = true;
    }
    _m[0] = _inverse.a;
    _m[1] = _inverse.b;
    _m[2] = _inverse.c;
    _m[3] = _inverse.d;
    _m[4] = _inverse.e;
    _m[5] = _inverse.f;
    _m[6] = _inverse.g;
    _m[7] = _inverse.h;
    _m[8] = _inverse.i;
}

void
MatrixInverseWarpMap::mapRow(std::size_t n,
                             double* sx,
                             double* sy,
                             double* J) const
{
    if (!_invertible) {
        std::fill(sx, sx + n, std::numeric_limits<double>::quiet_NaN());
        std::fill(sy, sy + n, std::numeric_limits<double>::quiet_NaN());

        return;
    }
    InverseWarp::mapRowThroughMatrix(_m, n, sx, sy, J);
}

uint32_t
MatrixInverseWarpMap::packGpu(std::vector<uint32_t>* words) const
{
    for (int k = 0; k < 9; ++k) {
        InverseWarp::pushGpuDouble(words, _invertible ? _m[k] : 0.);
    }

    return InverseWarpChain::kGpuTagMatrix;
}

RectD
MatrixInverseWarpMap::pullBack(const RectD& canonical,
                               double /*filterSupport*/) const
{
    if (!_invertible || canonical.isNull()) {
        return RectD();
    }
    // Corners at the infinity flags or NaN would transform into a finite-looking region that a
    // map further upstream would then be built over.
    if (!isFiniteRegion(canonical)) {
        return RectD(kOfxFlagInfiniteMin, kOfxFlagInfiniteMin, kOfxFlagInfiniteMax, kOfxFlagInfiniteMax);
    }
    const OfxPointD s = getScale().toOfxPointD();
    const Transform::Matrix3x3 canonicalToPixel = Transform::matCanonicalToPixel(getPixelAspectRatio(), s.x, s.y, false);
    const Transform::Matrix3x3 pixelToCanonical = Transform::matPixelToCanonical(getPixelAspectRatio(), s.x, s.y, false);
    const Transform::Matrix3x3 inverse = Transform::matMul(Transform::matMul(pixelToCanonical, _inverse), canonicalToPixel);
    RectD region;
    Transform::transformRegionFromRoD(canonical, inverse, region);

    return region;
}

InverseWarpChain::InverseWarpChain(const RenderScale& scale,
                                   double par)
    : _scale(scale)
    , _par(par > 0. ? par : 1.)
{
}

void
InverseWarpChain::appendMatrix(const Transform::Matrix3x3& forward)
{
    if (!_steps.empty() && (_steps.back().kind == eStepMatrix)) {
        Step& last = _steps.back();
        last.forward = Transform::matMul(last.forward, forward);
        last.map = std::make_shared<MatrixInverseWarpMap>(last.forward, _scale, _par);

        return;
    }
    Step step;
    step.kind = eStepMatrix;
    step.gpuStep = eGpuStepMatrix;
    step.forward = forward;
    step.map = std::make_shared<MatrixInverseWarpMap>(forward, _scale, _par);
    _steps.push_back(step);
}

void
InverseWarpChain::appendMap(const MapFactory& factory,
                            unsigned gpuStep,
                            const std::optional<GpuWeight>& gpuWeight)
{
    Step step;
    step.kind = eStepMap;
    step.gpuStep = gpuStep;
    step.gpuWeight = gpuWeight;
    step.factory = factory;
    _steps.push_back(step);
}

void
InverseWarpChain::appendMap(const InverseWarpMapConstPtr& map,
                            unsigned gpuStep)
{
    Step step;
    step.kind = eStepMap;
    step.gpuStep = gpuStep;
    step.map = map;
    _steps.push_back(step);
}

unsigned
InverseWarpChain::getGpuSteps() const
{
    unsigned steps = 0;
    for (const Step& step : _steps) {
        steps |= step.gpuStep;
    }

    return steps;
}

GpuWeight
InverseWarpChain::addMapGpuWeights(GpuWeight weight) const
{
    for (const Step& step : _steps) {
        if ((step.kind != eStepMap) || !step.gpuWeight || weight.isHeavy()) {
            continue;
        }
        if (step.gpuWeight->isHeavy()) {
            weight = GpuWeight::heavy();
            continue;
        }
        weight = GpuWeight::filter(weight.weight + step.gpuWeight->weight);
    }

    return weight;
}

bool
InverseWarpChain::hasMapStep() const
{
    for (const Step& step : _steps) {
        if (step.kind == eStepMap) {
            return true;
        }
    }

    return false;
}

RectI
InverseWarpChain::getSourcePixels(double par) const
{
    const RectD& r = _sourceRegion;
    if (!((r.x2 > r.x1) && (r.y2 > r.y1))) {
        return RectI();
    }
    // Keeps an unbounded region's pixels inside int range.
    const double kMax = 1e9;
    const auto clampCoord = [kMax](double v) {
        return std::max(-kMax, std::min(v, kMax));
    };

    return RectD(clampCoord(r.x1), clampCoord(r.y1), clampCoord(r.x2), clampCoord(r.y2)).toPixelEnclosing(_scale, par);
}

const std::string&
InverseWarpChain::getInfiniteWindowError()
{
    static const std::string error = "A warp concatenated into this node would be built over an infinite region. "
                                     "Give the image upstream of the warps a finite region of definition, for instance with a Crop.";

    return error;
}

InverseWarpChain
InverseWarpChain::prepare(const RectD& window,
                          double filterSupport) const
{
    InverseWarpChain prepared(*this);

    prepared._windows.clear();
    prepared._windows.reserve(prepared._steps.size());
    prepared._error.clear();
    RectD region = window;
    if (_sourceInfinite && hasMapStep()) {
        prepared._error = getInfiniteWindowError();
    }
    for (Step& step : prepared._steps) {
        prepared._windows.push_back(region);
        // A map over an infinite region would be built or sampled at a size nothing can hold;
        // the region of definition upstream has to be finite, which only the user can make it.
        if (prepared.hasError() || ((step.kind == eStepMap) && !region.isNull() && !isFiniteRegion(region))) {
            if (!prepared.hasError()) {
                prepared._error = getInfiniteWindowError();
            }
            if (step.kind == eStepMap) {
                step.map.reset();
            }
            region = RectD();
            continue;
        }
        if (step.factory) {
            step.map = region.isNull() ? InverseWarpMapConstPtr() : step.factory(region);
        }
        region = step.map ? step.map->pullBack(region, filterSupport) : RectD();
    }
    prepared._sourceRegion = region;
    prepared._prepared = true;

    return prepared;
}

void
InverseWarpChain::mapRow(std::size_t n,
                         double* sx,
                         double* sy,
                         double* J) const
{
    for (const Step& step : _steps) {
        if (!step.map) {
            std::fill(sx, sx + n, std::numeric_limits<double>::quiet_NaN());
            std::fill(sy, sy + n, std::numeric_limits<double>::quiet_NaN());

            return;
        }
        step.map->mapRow(n, sx, sy, J);
    }
}

void
InverseWarpChain::packGpuMatrix(const Transform::Matrix3x3& forward,
                                std::vector<uint32_t>* words)
{
    words->push_back(1u);
    words->push_back(kGpuTagMatrix);
    words->push_back(3u);
    MatrixInverseWarpMap(forward, RenderScale(), 1.).packGpu(words);
}

bool
InverseWarpChain::packGpu(std::vector<uint32_t>* words) const
{
    words->clear();
    const std::size_t n = _steps.size();
    words->assign(1 + 2 * n, 0u);
    (*words)[0] = (uint32_t)n;
    for (std::size_t i = 0; i < n; ++i) {
        const Step& step = _steps[i];
        const std::size_t offset = words->size();
        const uint32_t tag = step.map ? step.map->packGpu(words) : 0u;
        if ((tag == 0) || (offset > UINT32_MAX) || (words->size() > UINT32_MAX)) {
            words->clear();

            return false;
        }
        (*words)[1 + 2 * i] = tag;
        (*words)[2 + 2 * i] = (uint32_t)offset;
    }

    return true;
}

NATRON_NAMESPACE_EXIT
