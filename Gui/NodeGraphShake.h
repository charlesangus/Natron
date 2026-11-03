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

#ifndef Gui_NodeGraphShake_h
#define Gui_NodeGraphShake_h

#include "Global/Macros.h"

#include <deque>

NATRON_NAMESPACE_ENTER

// Recognises a shake in the positions of a node drag: on either axis, reversalsToFire direction
// reversals whose turning points are no older than windowMs when the last of them is counted, every
// swing being at least minAmplitude long. A reversal is only counted once the motion has come back
// minAmplitude from its turning point, so jitter below the amplitude never counts.
class NodeGraphShakeDetector {
public:
    NodeGraphShakeDetector(double minAmplitude = 40.,
                           double windowMs = 500.,
                           int reversalsToFire = 3);

    // Forgets every sample; the next one starts afresh.
    void reset();

    // Returns true for the sample that completes a shake, after which the detector starts afresh
    // from that sample, so firing again needs a whole new shake.
    bool addSample(double timestampMs,
                   double x,
                   double y);

private:
    struct Axis {
        // 0 until the motion has gone minAmplitude one way, then the sign of the current swing.
        int direction;
        // The range seen while there is no direction yet.
        double low, high;
        // The furthest point of the current swing, its turning point if it reverses.
        double extreme, extremeTime;
        std::deque<double> reversalTimes;

        Axis()
            : direction(0)
            , low(0.)
            , high(0.)
            , extreme(0.)
            , extremeTime(0.)
            , reversalTimes()
        {
        }
    };

    void restart(double x,
                 double y);

    bool addAxisSample(Axis& axis,
                       double timestampMs,
                       double v) const;

    double _minAmplitude;
    double _windowMs;
    int _reversalsToFire;
    bool _started;
    Axis _x, _y;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_NodeGraphShake_h
