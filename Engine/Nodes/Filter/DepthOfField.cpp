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

#include "DepthOfField.h"

#include <algorithm>
#include <cmath>
#include <limits>

NATRON_NAMESPACE_ENTER

namespace {
const double kInfinity = std::numeric_limits<double>::infinity();

double
finiteOr(double value,
         double fallback)
{
    return std::isfinite(value) ? value : fallback;
}

double
inverse(double distance)
{
    return std::isinf(distance) ? 0. : 1. / distance;
}
} // anonymous namespace

double
depthDistance(double raw,
              DepthUnits units)
{
    if (!std::isfinite(raw) || (raw <= 0.)) {
        return kInfinity;
    }
    if (units == DepthUnits::Distance) {
        return raw;
    }
    const double distance = 1. / raw;

    return std::isfinite(distance) ? distance : kInfinity;
}

DepthOfField::DepthOfField(const DepthOfFieldParams& params)
    : _units(params.units)
{
    _maxSize = std::max(0., finiteOr(params.maxSize, 0.));
    const double focal = finiteOr(params.focalPlane, 0.);

    if (params.mode == DepthOfFieldMode::Manual) {
        const double half = std::max(0., finiteOr(params.depthOfField, 0.)) / 2.;
        // The slice's raw edges, nearer first; with inverse distances the larger value is nearer.
        const double rawNear = (params.units == DepthUnits::Distance) ? focal - half : focal + half;
        const double rawFar = (params.units == DepthUnits::Distance) ? focal + half : focal - half;
        _far = depthDistance(rawFar, params.units);
        // A slice reaching distance 0 leaves nothing in front of it.
        _near = ((params.units == DepthUnits::Distance) && (rawNear <= 0.) && (rawFar > 0.)) ? 0. : depthDistance(rawNear, params.units);
        const double size = std::max(0., finiteOr(params.size, 0.));
        // size at infinite distance behind the slice: a / far.
        if (std::isinf(_far)) {
            _scale = std::isinf(_near) ? kInfinity : size * _near;
        } else {
            _scale = size * _far;
        }
        if (size <= 0.) {
            _scale = 0.;
        }
    } else {
        const double f = std::max(0., finiteOr(params.focalLength, 0.));
        const double n = finiteOr(params.fStop, 0.);
        const double filmBack = finiteOr(params.filmBackWidth, 0.);
        const double width = std::max(0., finiteOr(params.imageWidth, 0.));
        const double unit = std::max(0., finiteOr(params.millimetresPerUnit, 0.));
        const double focus = depthDistance(focal, params.units);
        _near = focus;
        _far = focus;
        if ((f <= 0.) || (n <= 0.) || (filmBack <= 0.) || (width <= 0.) || (unit <= 0.)) {
            _scale = 0.;
        } else {
            _minDistance = f / unit;
            // f^2 / (N (S - f)) * S in millimetres, S in millimetres; f / S -> 0 as S -> infinity.
            const double fOverS = std::isinf(focus) ? 0. : f / (focus * unit);
            if (fOverS >= 1.) {
                _scale = kInfinity;
            } else {
                // Converted to canonical pixels, and from 1/mm to 1/unit for the distances.
                _scale = f * f / n / (1. - fOverS) * (width / filmBack) / unit;
            }
        }
    }

    if (_scale <= 0.) {
        _frontMax = 0.;
        _backMax = 0.;
    } else {
        _frontMax = (_near > 0.) ? _maxSize : 0.;
        _backMax = std::isinf(_far) ? 0. : std::min(_maxSize, _scale * inverse(_far));
    }
}

double
DepthOfField::cocOfDistance(double distance) const
{
    if (_scale <= 0.) {
        return 0.;
    }
    if (distance < _near) {
        if ((distance < _minDistance) || std::isinf(_scale)) {
            return -_maxSize;
        }

        return -std::min(_maxSize, _scale * (1. / distance - inverse(_near)));
    }
    if (distance > _far) {
        if (std::isinf(_scale)) {
            return _maxSize;
        }

        return std::min(_maxSize, _scale * (1. / _far - inverse(distance)));
    }

    return 0.;
}

double
DepthOfField::coc(double raw) const
{
    return cocOfDistance(depthDistance(raw, _units));
}

NATRON_NAMESPACE_EXIT
