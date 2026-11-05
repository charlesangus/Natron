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

#include "NodeGraphShake.h"

NATRON_NAMESPACE_ENTER

NodeGraphShakeDetector::NodeGraphShakeDetector(double minAmplitude,
                                               double windowMs,
                                               int reversalsToFire)
    : _minAmplitude(minAmplitude)
    , _windowMs(windowMs)
    , _reversalsToFire(reversalsToFire)
    , _started(false)
    , _x()
    , _y()
{
}

void
NodeGraphShakeDetector::reset()
{
    _started = false;
    _x = Axis();
    _y = Axis();
}

void
NodeGraphShakeDetector::restart(double x,
                                double y)
{
    _started = true;
    _x = Axis();
    _x.low = _x.high = x;
    _y = Axis();
    _y.low = _y.high = y;
}

bool
NodeGraphShakeDetector::addAxisSample(Axis& axis,
                                      double timestampMs,
                                      double v) const
{
    if (axis.direction == 0) {
        if (v < axis.low) {
            axis.low = v;
        }
        if (v > axis.high) {
            axis.high = v;
        }
        if (v - axis.low >= _minAmplitude) {
            axis.direction = 1;
        } else if (axis.high - v >= _minAmplitude) {
            axis.direction = -1;
        } else {
            return false;
        }
        axis.extreme = v;
        axis.extremeTime = timestampMs;

        return false;
    }

    const double travel = (v - axis.extreme) * axis.direction;
    if (travel >= 0.) {
        axis.extreme = v;
        axis.extremeTime = timestampMs;

        return false;
    }
    if (-travel < _minAmplitude) {
        return false;
    }

    // The motion has come back far enough from the turning point to count it as a reversal.
    axis.reversalTimes.push_back(axis.extremeTime);
    axis.direction = -axis.direction;
    axis.extreme = v;
    axis.extremeTime = timestampMs;
    while (!axis.reversalTimes.empty() && (timestampMs - axis.reversalTimes.front() > _windowMs)) {
        axis.reversalTimes.pop_front();
    }

    return (int)axis.reversalTimes.size() >= _reversalsToFire;
}

bool
NodeGraphShakeDetector::addSample(double timestampMs,
                                  double x,
                                  double y)
{
    if (!_started) {
        restart(x, y);

        return false;
    }
    const bool firedX = addAxisSample(_x, timestampMs, x);
    const bool firedY = addAxisSample(_y, timestampMs, y);
    if (firedX || firedY) {
        restart(x, y);

        return true;
    }

    return false;
}

NATRON_NAMESPACE_EXIT
