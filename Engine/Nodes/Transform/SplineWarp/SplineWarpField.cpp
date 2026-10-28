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

#include "SplineWarpField.h"

#include <algorithm>
#include <cmath>
#include <list>

#include "Engine/Nodes/Transform/SplineWarp/SplineWarpTps.h"

NATRON_NAMESPACE_ENTER

namespace {
Point
makePoint(double x,
          double y)
{
    Point p;

    p.x = x;
    p.y = y;

    return p;
}

struct Bounds {
    double x1, y1, x2, y2;

    bool contains(double x, double y) const
    {
        return x >= x1 && x <= x2 && y >= y1 && y <= y2;
    }
};

Bounds
boundsOf(const std::vector<Point>& polygon)
{
    Bounds b = { 0., 0., -1., -1. };

    for (std::size_t i = 0; i < polygon.size(); ++i) {
        if (i == 0) {
            b.x1 = b.x2 = polygon[i].x;
            b.y1 = b.y2 = polygon[i].y;
        } else {
            b.x1 = std::min(b.x1, polygon[i].x);
            b.x2 = std::max(b.x2, polygon[i].x);
            b.y1 = std::min(b.y1, polygon[i].y);
            b.y2 = std::max(b.y2, polygon[i].y);
        }
    }

    return b;
}
} // anonymous namespace

SplineWarpField::SplineWarpField()
    : _window()
    , _scaleX(1.)
    , _scaleY(1.)
    , _step(1.)
    , _identity(true)
    , _boundaries()
    , _i0(0)
    , _j0(0)
    , _nx(0)
    , _ny(0)
{
}

double
SplineWarpField::featherRamp(double t,
                             double fallOff)
{
    if (!(t > 0.)) {
        return 1.;
    }
    if (t >= 1.) {
        return 0.;
    }
    if (!(fallOff > 0.)) {
        fallOff = 1.;
    }
    // The Roto renderer's feather mesh: across the feather, position follows a cubic Bezier with
    // these inner control points while the opacity goes linearly from 1 to 0.
    const double f2 = fallOff * fallOff;
    const double c1 = 1. / (2. * f2 + 1.);
    const double c2 = 2. / (f2 + 2.);
    double lo = 0., hi = 1.;
    for (int i = 0; i < 52; ++i) {
        const double s = 0.5 * (lo + hi);
        const double u = 1. - s;
        const double x = 3. * u * u * s * c1 + 3. * u * s * s * c2 + s * s * s;
        if (x < t) {
            lo = s;
        } else {
            hi = s;
        }
    }

    return 1. - 0.5 * (lo + hi);
}

bool
SplineWarpField::insideAnyFeather(double x,
                                  double y) const
{
    for (std::size_t b = 0; b < _boundaries.size(); ++b) {
        if (splineWarpPointInPolygon(_boundaries[b].feather, x, y)) {
            return true;
        }
    }

    return false;
}

double
SplineWarpField::nodeWeight(double x,
                            double y) const
{
    double weight = 0.;

    for (std::size_t b = 0; b < _boundaries.size() && weight < 1.; ++b) {
        const SplineWarpBoundaryShape& shape = _boundaries[b];
        if (!splineWarpPointInPolygon(shape.feather, x, y)) {
            continue;
        }
        if (splineWarpPointInPolygon(shape.curve, x, y)) {
            weight = 1.;
            break;
        }
        const double dIn = splineWarpDistanceToPolyline(shape.curve, x, y);
        const double dOut = splineWarpDistanceToPolyline(shape.feather, x, y);
        const double sum = dIn + dOut;
        const double t = (sum > 0.) ? dIn / sum : 0.;
        weight = std::max(weight, featherRamp(t, shape.fallOff));
    }

    return weight;
}

SplineWarpField::SplineWarpField(const SplineWarpTps& map,
                                 const std::vector<SplineWarpBoundaryShape>& boundaries,
                                 const RectI& window,
                                 double scaleX,
                                 double scaleY,
                                 double fieldResolution)
    : _window(window)
    , _scaleX((scaleX > 0.) ? scaleX : 1.)
    , _scaleY((scaleY > 0.) ? scaleY : 1.)
    , _step((fieldResolution > 0.) ? fieldResolution : 1.)
    , _identity(true)
    , _boundaries(boundaries)
    , _i0(0)
    , _j0(0)
    , _nx(0)
    , _ny(0)
{
    if (window.isNull() || map.isIdentity()) {
        return;
    }

    // Pixel centres of the window, in canonical coordinates; one grid point beyond them on the
    // low side and two on the high side, so every bilinear cell has central differences.
    const double cx1 = (window.x1 + 0.5) / _scaleX;
    const double cx2 = (window.x2 - 0.5) / _scaleX;
    const double cy1 = (window.y1 + 0.5) / _scaleY;
    const double cy2 = (window.y2 - 0.5) / _scaleY;
    _i0 = (int)std::floor(cx1 / _step) - 1;
    _j0 = (int)std::floor(cy1 / _step) - 1;
    _nx = (int)std::floor(cx2 / _step) + 2 - _i0 + 1;
    _ny = (int)std::floor(cy2 / _step) + 2 - _j0 + 1;

    const std::size_t count = (std::size_t)_nx * (std::size_t)_ny;
    _sourceX.resize(count);
    _sourceY.resize(count);
    _weight.assign(count, _boundaries.empty() ? 1. : 0.);

    std::vector<Bounds> featherBounds;
    for (std::size_t b = 0; b < _boundaries.size(); ++b) {
        featherBounds.push_back(boundsOf(_boundaries[b].feather));
    }

    bool moved = false;
    for (int j = 0; j < _ny; ++j) {
        const double y = (_j0 + j) * _step;
        for (int i = 0; i < _nx; ++i) {
            const double x = (_i0 + i) * _step;
            const std::size_t idx = (std::size_t)j * _nx + i;
            double w = 1.;
            if (!_boundaries.empty()) {
                bool near = false;
                for (std::size_t b = 0; b < featherBounds.size() && !near; ++b) {
                    near = featherBounds[b].contains(x, y);
                }
                w = near ? nodeWeight(x, y) : 0.;
                _weight[idx] = w;
            }
            if (w > 0.) {
                const Point s = map.evaluate(x, y);
                _sourceX[idx] = x + w * (s.x - x);
                _sourceY[idx] = y + w * (s.y - y);
                moved = moved || (_sourceX[idx] != x) || (_sourceY[idx] != y);
            } else {
                _sourceX[idx] = x;
                _sourceY[idx] = y;
            }
        }
    }
    if (!moved) {
        _sourceX.clear();
        _sourceY.clear();
        _weight.clear();
        _nx = _ny = 0;

        return;
    }
    _identity = false;

    _jacobian.resize(count * 4);
    for (int j = 0; j < _ny; ++j) {
        const int jm = std::max(0, j - 1);
        const int jp = std::min(_ny - 1, j + 1);
        for (int i = 0; i < _nx; ++i) {
            const int im = std::max(0, i - 1);
            const int ip = std::min(_nx - 1, i + 1);
            const std::size_t idx = (std::size_t)j * _nx + i;
            const double hx = (ip - im) * _step;
            const double hy = (jp - jm) * _step;
            const std::size_t xm = (std::size_t)j * _nx + im, xp = (std::size_t)j * _nx + ip;
            const std::size_t ym = (std::size_t)jm * _nx + i, yp = (std::size_t)jp * _nx + i;
            double* J = &_jacobian[idx * 4];
            J[0] = (hx > 0.) ? (_sourceX[xp] - _sourceX[xm]) / hx : 1.;
            J[1] = (hy > 0.) ? (_sourceX[yp] - _sourceX[ym]) / hy : 0.;
            J[2] = (hx > 0.) ? (_sourceY[xp] - _sourceY[xm]) / hx : 0.;
            J[3] = (hy > 0.) ? (_sourceY[yp] - _sourceY[ym]) / hy : 1.;
        }
    }
}

void
SplineWarpField::cellOf(double x,
                        double y,
                        int* ix,
                        int* iy,
                        double* fx,
                        double* fy) const
{
    const double gx = x / _step - _i0;
    const double gy = y / _step - _j0;
    int i = (int)std::floor(gx);
    int j = (int)std::floor(gy);

    i = std::max(0, std::min(i, _nx - 2));
    j = std::max(0, std::min(j, _ny - 2));
    *ix = i;
    *iy = j;
    *fx = std::max(0., std::min(1., gx - i));
    *fy = std::max(0., std::min(1., gy - j));
}

RectD
SplineWarpField::getSourceBounds() const
{
    RectD bounds(_window.x1 / _scaleX, _window.y1 / _scaleY, _window.x2 / _scaleX, _window.y2 / _scaleY);

    // The field is bilinear between grid points, so the grid's sources bound the pixels'; each
    // grows by half the pixel footprint its Jacobian gives, which a filter may read.
    for (std::size_t k = 0; k < _sourceX.size(); ++k) {
        double hx = 0.;
        double hy = 0.;
        if (_jacobian.size() == _sourceX.size() * 4) {
            const double* J = &_jacobian[k * 4];
            hx = 0.5 * (std::fabs(J[0]) / _scaleX + std::fabs(J[1]) / _scaleY);
            hy = 0.5 * (std::fabs(J[2]) / _scaleX + std::fabs(J[3]) / _scaleY);
        }
        bounds.merge(_sourceX[k] - hx, _sourceY[k] - hy, _sourceX[k] + hx, _sourceY[k] + hy);
    }

    return bounds;
}

Point
SplineWarpField::getSource(double x,
                           double y) const
{
    if (_identity || (!_boundaries.empty() && !insideAnyFeather(x, y))) {
        return makePoint(x, y);
    }
    int i, j;
    double fx, fy;
    cellOf(x, y, &i, &j, &fx, &fy);
    const std::size_t a = (std::size_t)j * _nx + i;
    const std::size_t b = a + 1;
    const std::size_t c = a + _nx;
    const std::size_t d = c + 1;

    return makePoint((1. - fy) * ((1. - fx) * _sourceX[a] + fx * _sourceX[b]) + fy * ((1. - fx) * _sourceX[c] + fx * _sourceX[d]),
                     (1. - fy) * ((1. - fx) * _sourceY[a] + fx * _sourceY[b]) + fy * ((1. - fx) * _sourceY[c] + fx * _sourceY[d]));
}

void
SplineWarpField::getJacobian(double x,
                             double y,
                             double jacobian[4]) const
{
    if (_identity || (!_boundaries.empty() && !insideAnyFeather(x, y))) {
        jacobian[0] = 1.;
        jacobian[1] = 0.;
        jacobian[2] = 0.;
        jacobian[3] = 1.;

        return;
    }
    int i, j;
    double fx, fy;
    cellOf(x, y, &i, &j, &fx, &fy);
    const double* a = &_jacobian[((std::size_t)j * _nx + i) * 4];
    const double* b = a + 4;
    const double* c = a + (std::size_t)_nx * 4;
    const double* d = c + 4;
    for (int k = 0; k < 4; ++k) {
        jacobian[k] = (1. - fy) * ((1. - fx) * a[k] + fx * b[k]) + fy * ((1. - fx) * c[k] + fx * d[k]);
    }
}

double
SplineWarpField::getWeight(double x,
                           double y) const
{
    if (_boundaries.empty()) {
        return 1.;
    }
    if (!insideAnyFeather(x, y)) {
        return 0.;
    }
    if (_identity) {
        return nodeWeight(x, y);
    }
    int i, j;
    double fx, fy;
    cellOf(x, y, &i, &j, &fx, &fy);
    const std::size_t a = (std::size_t)j * _nx + i;
    const std::size_t b = a + 1;
    const std::size_t c = a + _nx;
    const std::size_t d = c + 1;

    return (1. - fy) * ((1. - fx) * _weight[a] + fx * _weight[b]) + fy * ((1. - fx) * _weight[c] + fx * _weight[d]);
}

void
SplineWarpField::getRow(int y,
                        int x1,
                        int x2,
                        float* sx,
                        float* sy,
                        float* jacobian) const
{
    const int n = x2 - x1;

    if (n <= 0) {
        return;
    }
    const double cy = (y + 0.5) / _scaleY;

    // Even-odd crossings of the row with each feather, sorted: a pixel is inside a feather when an
    // odd number of its crossings lie beyond it, as splineWarpPointInPolygon() decides.
    std::vector<std::vector<double>> crossings(_identity ? 0 : _boundaries.size());
    std::vector<std::size_t> passed(crossings.size(), 0);
    for (std::size_t b = 0; b < crossings.size(); ++b) {
        splineWarpPolygonRowCrossings(_boundaries[b].feather, cy, &crossings[b]);
        std::sort(crossings[b].begin(), crossings[b].end());
    }

    const double ratioXY = _scaleX / _scaleY;
    const double ratioYX = _scaleY / _scaleX;
    for (int k = 0; k < n; ++k) {
        const int x = x1 + k;
        const double cx = (x + 0.5) / _scaleX;
        bool warped = !_identity;
        if (warped && !crossings.empty()) {
            bool inside = false;
            for (std::size_t b = 0; b < crossings.size(); ++b) {
                const std::vector<double>& c = crossings[b];
                while (passed[b] < c.size() && !(cx < c[passed[b]])) {
                    ++passed[b];
                }
                if ((c.size() - passed[b]) % 2 == 1) {
                    inside = true;
                }
            }
            warped = inside;
        }
        if (!warped) {
            sx[k] = (float)(x + 0.5);
            sy[k] = (float)(y + 0.5);
            if (jacobian) {
                float* J = jacobian + 4 * k;
                J[0] = 1.f;
                J[1] = 0.f;
                J[2] = 0.f;
                J[3] = 1.f;
            }
            continue;
        }
        int i, j;
        double fx, fy;
        cellOf(cx, cy, &i, &j, &fx, &fy);
        const std::size_t a = (std::size_t)j * _nx + i;
        const std::size_t b = a + 1;
        const std::size_t c = a + _nx;
        const std::size_t d = c + 1;
        const double srcX = (1. - fy) * ((1. - fx) * _sourceX[a] + fx * _sourceX[b]) + fy * ((1. - fx) * _sourceX[c] + fx * _sourceX[d]);
        const double srcY = (1. - fy) * ((1. - fx) * _sourceY[a] + fx * _sourceY[b]) + fy * ((1. - fx) * _sourceY[c] + fx * _sourceY[d]);
        sx[k] = (float)(srcX * _scaleX);
        sy[k] = (float)(srcY * _scaleY);
        if (jacobian) {
            const double* ja = &_jacobian[a * 4];
            const double* jb = &_jacobian[b * 4];
            const double* jc = &_jacobian[c * 4];
            const double* jd = &_jacobian[d * 4];
            double J[4];
            for (int m = 0; m < 4; ++m) {
                J[m] = (1. - fy) * ((1. - fx) * ja[m] + fx * jb[m]) + fy * ((1. - fx) * jc[m] + fx * jd[m]);
            }
            float* out = jacobian + 4 * k;
            out[0] = (float)J[0];
            out[1] = (float)(J[1] * ratioXY);
            out[2] = (float)(J[2] * ratioYX);
            out[3] = (float)J[3];
        }
    }
} // SplineWarpField::getRow

std::vector<double>
SplineWarpFieldCache::getResolutions() const
{
    std::vector<double> ret;
    const std::list<Key> keys = _cache.getKeys();

    for (std::list<Key>::const_iterator it = keys.begin(); it != keys.end(); ++it) {
        ret.push_back(it->resolution);
    }

    return ret;
}

SplineWarpFieldConstPtr
SplineWarpFieldCache::get(U64 warpHash,
                          double time,
                          const SplineWarpTps& map,
                          const std::vector<SplineWarpBoundaryShape>& boundaries,
                          const RectI& window,
                          double scaleX,
                          double scaleY,
                          double fieldResolution)
{
    Key key;

    key.hash = warpHash;
    key.time = time;
    key.scaleX = scaleX;
    key.scaleY = scaleY;
    key.resolution = fieldResolution;
    key.window = window;

    return _cache.get(key, [&]() {
        return SplineWarpField(map, boundaries, window, scaleX, scaleY, fieldResolution);
    });
}

NATRON_NAMESPACE_EXIT
