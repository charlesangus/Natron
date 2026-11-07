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

#include "GlobalModeStroke.h"

#include <cmath>
#include <vector>

#include <QtCore/qnamespace.h>

NATRON_NAMESPACE_ENTER

namespace {

bool
mapsAxesToDimensions(GlobalModeKindEnum kind)
{
    return (kind == eGlobalModeKindTranslate) || (kind == eGlobalModeKindSkew) || (kind == eGlobalModeKindGenericSpatial2D);
}

double
incrementOf(const GlobalModeValueStrokeSpec& spec,
            int dimension)
{
    if ((dimension < (int)spec.increments.size()) && (spec.increments[dimension] > 0.)) {
        return spec.increments[dimension];
    }

    return 1.;
}

} // namespace

GlobalModeValueStroke::GlobalModeValueStroke()
    : _start()
    , _dx(0.0)
    , _dy(0.0)
{
}

void
GlobalModeValueStroke::begin(const std::vector<double>& start)
{
    _start = start;
    _dx = 0.0;
    _dy = 0.0;
}

void
GlobalModeValueStroke::addMove(double dx,
                               double dy,
                               double gearFactor)
{
    _dx += dx * gearFactor;
    _dy += dy * gearFactor;
}

std::vector<double>
GlobalModeValueStroke::evaluate(const GlobalModeValueStrokeSpec& spec,
                                const GlobalModeValueRates& rates) const
{
    std::vector<double> values = _start;
    const int nDims = (int)values.size();

    if ((spec.kind == eGlobalModeKindColor) || (spec.kind == eGlobalModeKindIneligible)) {
        return values;
    }

    const int lock = ((spec.lockDimension >= 0) && (spec.lockDimension < nDims)) ? spec.lockDimension : kGlobalModeLockAll;
    const double up = -_dy;
    const double either = _dx + up;
    const bool axes = mapsAxesToDimensions(spec.kind) && (nDims > 1);

    for (int d = 0; d < nDims; ++d) {
        if ((lock != kGlobalModeLockAll) && (d != lock)) {
            continue;
        }

        double travel = either;
        if (axes && (d == 0)) {
            travel = _dx;
        } else if (axes && (d == 1)) {
            travel = up;
        }

        const double start = _start[d];
        if (spec.kind == eGlobalModeKindScale) {
            const double scaled = start * std::pow(1. + rates.scaleFractionPerPixel, travel);
            values[d] = spec.isInt ? std::round(scaled) : scaled;
            continue;
        }

        double delta = 0.;
        switch (spec.kind) {
        case eGlobalModeKindTranslate:
        case eGlobalModeKindGenericSpatial2D:
            delta = travel * rates.translateUnitsPerPixel;
            break;
        case eGlobalModeKindRotate:
            delta = travel * rates.rotateDegreesPerPixel;
            break;
        default:
            delta = travel * incrementOf(spec, d);
            break;
        }
        values[d] = start + (spec.isInt ? std::trunc(delta) : delta);
    }

    return values;
}

GlobalModeValueKeyAction
classifyGlobalModeValueKey(int key,
                           bool inGroup,
                           int dimensionCount)
{
    GlobalModeValueKeyAction result;

    if (key == Qt::Key_A) {
        result.action = eGlobalModeValueKeyLockAll;

        return result;
    }

    int lock = -1;
    if (inGroup) {
        switch (key) {
        case Qt::Key_Q:
            result.member = eGlobalModeGroupMemberTranslate;
            break;
        case Qt::Key_W:
            result.member = eGlobalModeGroupMemberRotate;
            break;
        case Qt::Key_E:
            result.member = eGlobalModeGroupMemberScale;
            break;
        case Qt::Key_R:
            result.member = eGlobalModeGroupMemberSkew;
            break;
        case Qt::Key_T:
            result.member = eGlobalModeGroupMemberCenter;
            break;
        case Qt::Key_1:
            lock = 0;
            break;
        case Qt::Key_2:
            lock = 1;
            break;
        default:
            return result;
        }
        if (lock < 0) {
            result.action = eGlobalModeValueKeySwitchMember;

            return result;
        }
    } else {
        switch (key) {
        case Qt::Key_Q:
            lock = 0;
            break;
        case Qt::Key_W:
            lock = 1;
            break;
        case Qt::Key_E:
            lock = 2;
            break;
        case Qt::Key_R:
            lock = 3;
            break;
        default:
            return result;
        }
    }

    if (lock < dimensionCount) {
        result.action = eGlobalModeValueKeyLockDimension;
        result.dimension = lock;
    }

    return result;
}

NATRON_NAMESPACE_EXIT
