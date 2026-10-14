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
#include <cmath>
#include <cstdint>
#include <cstring>
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

    const std::vector<KeyFrame>& keys() const
    {
        return _keys;
    }

    double yMin() const
    {
        return _yMin;
    }

    double yMax() const
    {
        return _yMax;
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

    // The block curvesnapshot.slang evaluates the curve from: the key times, then the cubic of each
    // gap between keys, with one before the first key and one after the last.
    static const int kGpuMaxKeys = 128;
    static const int kGpuSegmentWords = 6;
    static const int kGpuWords = kGpuMaxKeys + (kGpuMaxKeys + 1) * kGpuSegmentWords;
    // The range of a table sampled from the curve, as curvesnapshot.slang's CurveTableRange.
    static const int kGpuTableRangeWords = 6;

    // Writes kGpuWords words, or returns false when the curve has more keys than they hold.
    bool packGpu(std::uint32_t* out) const
    {
        if ((int)_keys.size() > kGpuMaxKeys) {
            return false;
        }
        std::fill(out, out + kGpuWords, 0u);
        for (std::size_t i = 0; i < _keys.size(); ++i) {
            putFloat(out + i, ceilToFloat(_keys[i].getTime()));
        }
        for (std::size_t segment = 0; segment <= _keys.size(); ++segment) {
            packSegment(segment, out + kGpuMaxKeys + segment * kGpuSegmentWords);
        }

        return true;
    }

    // A table of `intervals` + 1 samples over [rangeMin, rangeMax]: the bounds a float is outside
    // the range beyond, then the range start and the table scale each as a float and its remainder,
    // so that the shader finds a table position to the precision of the double arithmetic on the CPU.
    static void packGpuTableRange(double rangeMin,
                                  double rangeMax,
                                  int intervals,
                                  std::uint32_t* out)
    {
        const double scale = intervals / (rangeMax - rangeMin);

        putFloat(out, ceilToFloat(rangeMin));
        putFloat(out + 1, floorToFloat(rangeMax));
        putFloat(out + 2, (float)rangeMin);
        putFloat(out + 3, (float)(rangeMin - (double)(float)rangeMin));
        putFloat(out + 4, (float)scale);
        putFloat(out + 5, (float)(scale - (double)(float)scale));
    }

private:
    static void putFloat(std::uint32_t* out,
                         float value)
    {
        std::memcpy(out, &value, sizeof(float));
    }

    // The smallest float not below d: a float v is below d exactly when it is below this.
    static float ceilToFloat(double d)
    {
        float f = (float)d;

        if ((double)f < d) {
            f = std::nextafter(f, std::numeric_limits<float>::infinity());
        }

        return f;
    }

    // The largest float not above d: a float v is above d exactly when it is above this.
    static float floorToFloat(double d)
    {
        float f = (float)d;

        if ((double)f > d) {
            f = std::nextafter(f, -std::numeric_limits<float>::infinity());
        }

        return f;
    }

    // The Hermite cubic valueAt() evaluates between the keys before and after `segment`, as t
    // offset, span and the four coefficients over (t - offset) / span.
    void packSegment(std::size_t segment,
                     std::uint32_t* out) const
    {
        double c[4] = { 0., 0., 0., 0. };
        double tcur = 0.;
        double span = 1.;

        if (!_keys.empty()) {
            double vcur, vcurDerivRight, tnext, vnext, vnextDerivLeft;
            KeyframeTypeEnum interp, interpNext;
            if (segment == 0) {
                const KeyFrame& next = _keys.front();
                tnext = next.getTime();
                vnext = next.getValue();
                vnextDerivLeft = next.getLeftDerivative();
                interpNext = next.getInterpolation();
                tcur = tnext - 1.;
                vcur = vnext;
                vcurDerivRight = 0.;
                interp = eKeyframeTypeNone;
            } else if (segment == _keys.size()) {
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
                const KeyFrame& cur = _keys[segment - 1];
                const KeyFrame& next = _keys[segment];
                tcur = cur.getTime();
                vcur = cur.getValue();
                vcurDerivRight = cur.getRightDerivative();
                interp = cur.getInterpolation();
                tnext = next.getTime();
                vnext = next.getValue();
                vnextDerivLeft = next.getLeftDerivative();
                interpNext = next.getInterpolation();
            }
            // Interpolation::interpolate() up to the cubic's evaluation.
            double P0 = vcur;
            double P3 = vnext;
            double P0pr = vcurDerivRight * (tnext - tcur);
            double P3pl = vnextDerivLeft * (tnext - tcur);
            if (interp == eKeyframeTypeNone) {
                P0 = P3 - P3pl;
                P0pr = P3pl;
                tcur = tnext - 1.;
            } else if (interp == eKeyframeTypeConstant) {
                P0pr = 0.;
                P3pl = 0.;
                P3 = P0;
            }
            if (interpNext == eKeyframeTypeNone) {
                P3pl = P0pr;
                P3 = P0 + P0pr;
                tnext = tcur + 1;
            }
            c[0] = P0;
            c[1] = P0pr;
            c[2] = 3 * (P3 - P0) - 2 * P0pr - P3pl;
            c[3] = -2 * (P3 - P0) + P0pr + P3pl;
            span = tnext - tcur;
        }
        const float values[kGpuSegmentWords] = { (float)tcur, (float)span, (float)c[0], (float)c[1], (float)c[2], (float)c[3] };
        std::memcpy(out, values, sizeof(values));
    }

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
