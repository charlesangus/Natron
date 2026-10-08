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

#ifndef Engine_Nodes_Image_CurveSnapshot_h
#define Engine_Nodes_Image_CurveSnapshot_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <algorithm>
#include <limits>
#include <vector>

#include "Engine/Curve.h"
#include "Engine/EngineFwd.h"
#include "Engine/Interpolation.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief An immutable copy of a non-periodic curve, evaluated without the Curve's mutex. Render
 * threads share one kernel and a curve evaluated per pixel would otherwise take that lock for
 * every value. valueAt() repeats Curve::getValueAt() exactly: the same segment selection and
 * extrapolation, Interpolation::interpolate(), and the clamp to the curve's Y range.
 **/
class CurveSnapshot {
public:
    CurveSnapshot()
        : _yMin(-std::numeric_limits<double>::infinity())
        , _yMax(std::numeric_limits<double>::infinity())
    {
    }

    explicit CurveSnapshot(const CurvePtr& curve)
        : _yMin(-std::numeric_limits<double>::infinity())
        , _yMax(std::numeric_limits<double>::infinity())
    {
        if (!curve) {
            return;
        }
        const KeyFrameSet keys = curve->getKeyFrames_mt_safe();
        _keys.assign(keys.begin(), keys.end());
        const Curve::YRange yRange = curve->getCurveYRange();
        _yMin = yRange.min;
        _yMax = yRange.max;
    }

    double valueAt(double t) const
    {
        if (_keys.empty()) {
            return 0.;
        }
        std::vector<KeyFrame>::const_iterator next = std::upper_bound(_keys.begin(), _keys.end(), t, isBefore);
        double tcur, vcur, vcurDerivRight, tnext, vnext, vnextDerivLeft;
        KeyframeTypeEnum interp, interpNext;
        if (next == _keys.begin()) {
            tnext = next->getTime();
            vnext = next->getValue();
            vnextDerivLeft = next->getLeftDerivative();
            interpNext = next->getInterpolation();
            tcur = tnext - 1.;
            vcur = vnext;
            vcurDerivRight = 0.;
            interp = eKeyframeTypeNone;
        } else if (next == _keys.end()) {
            const KeyFrame& last = _keys.back();
            tcur = last.getTime();
            vcur = last.getValue();
            vcurDerivRight = last.getRightDerivative();
            interp = last.getInterpolation();
            tnext = tcur + 1.;
            vnext = vcur;
            vnextDerivLeft = 0.;
            interpNext = eKeyframeTypeNone;
        } else {
            const KeyFrame& cur = *(next - 1);
            tcur = cur.getTime();
            vcur = cur.getValue();
            vcurDerivRight = cur.getRightDerivative();
            interp = cur.getInterpolation();
            tnext = next->getTime();
            vnext = next->getValue();
            vnextDerivLeft = next->getLeftDerivative();
            interpNext = next->getInterpolation();
        }
        const double v = Interpolation::interpolate(tcur, vcur, vcurDerivRight, vnextDerivLeft, tnext, vnext, t, interp, interpNext);
        if (v > _yMax) {
            return _yMax;
        } else if (v < _yMin) {
            return _yMin;
        }

        return v;
    }

private:
    static bool isBefore(double t,
                         const KeyFrame& key)
    {
        return t < key.getTime();
    }

    std::vector<KeyFrame> _keys;
    double _yMin;
    double _yMax;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Image_CurveSnapshot_h
