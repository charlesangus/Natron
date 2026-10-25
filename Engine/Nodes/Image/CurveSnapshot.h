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
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "Engine/Curve.h"
#include "Engine/EngineFwd.h"
#include "Engine/Interpolation.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief An immutable copy of a non-periodic curve, evaluated without the Curve's mutex. Render
 * threads share one kernel and a curve evaluated per pixel would otherwise take that lock for
 * every value. valueAt() repeats Curve::getValueAt() exactly: the same segment selection and
 * extrapolation, Interpolation::interpolate() with the keys' tangent weights, and the clamp to the
 * curve's Y range. The GPU evaluates Hermite cubics only, so packGpu() refuses a curve with a
 * weighted segment and that curve is evaluated on the CPU.
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
        double vcurWeightRight = Interpolation::kDefaultTangentWeight;
        double vnextWeightLeft = Interpolation::kDefaultTangentWeight;
        KeyframeTypeEnum interp, interpNext;
        if (next == _keys.begin()) {
            tnext = next->getTime();
            vnext = next->getValue();
            vnextDerivLeft = next->getLeftDerivative();
            vnextWeightLeft = next->getLeftWeight();
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
            vcurWeightRight = last.getRightWeight();
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
            vcurWeightRight = cur.getRightWeight();
            interp = cur.getInterpolation();
            tnext = next->getTime();
            vnext = next->getValue();
            vnextDerivLeft = next->getLeftDerivative();
            vnextWeightLeft = next->getLeftWeight();
            interpNext = next->getInterpolation();
        }
        const double v = Interpolation::interpolate(tcur, vcur, vcurDerivRight, vnextDerivLeft, vcurWeightRight, vnextWeightLeft,
                                                    tnext, vnext, t, interp, interpNext);
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
    static const int kGpuSegmentWords = 12;
    static const int kGpuWords = kGpuMaxKeys + (kGpuMaxKeys + 1) * kGpuSegmentWords;
    // The range of a table sampled from the curve, as curvesnapshot.slang's CurveTableRange.
    static const int kGpuTableRangeWords = 6;

    // Writes kGpuWords words, or returns false when the curve has more keys than they hold, a
    // weighted segment, or a segment the shader cannot evaluate within kGpuSegmentErrorBound of
    // the CPU.
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
            if (!packSegment(segment, out + kGpuMaxKeys + segment * kGpuSegmentWords)) {
                return false;
            }
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
    // The largest difference from valueAt() a packed segment may evaluate to on the GPU: a tenth of
    // transcendental(), so that the nodes' own float steps after the curve stay within it.
    static constexpr double kGpuSegmentErrorBound = 1e-6;
    // The relative error of the shader's two-float evaluation of a cubic, with margin: six
    // two-float operations of about 2^-47 each.
    static constexpr double kGpuTwoFloatError = 0x1p-44;
    // The relative error of a double split into a float and its float remainder.
    static constexpr double kGpuSplitError = 0x1p-48;
    // The relative error of the CPU's own double evaluation of a segment, with margin.
    static constexpr double kCpuDoubleError = 0x1p-51;

    static void putFloat(std::uint32_t* out,
                         float value)
    {
        std::memcpy(out, &value, sizeof(float));
    }

    // d as a float and the float nearest its remainder, or false when the pair does not hold it.
    static bool putSplit(std::uint32_t* out,
                         double d)
    {
        const float hi = (float)d;
        const float lo = (float)(d - (double)hi);

        putFloat(out, hi);
        putFloat(out + 1, lo);

        return std::isfinite(hi) && std::isfinite(lo);
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

    // The Hermite cubic valueAt() evaluates between the keys before and after `segment`, as its t
    // origin, the reciprocal of its span and the four coefficients over (t - origin) / span, each
    // a float and its remainder. False when the shader's evaluation could stray further than
    // kGpuSegmentErrorBound from valueAt(), or when the segment is weighted.
    bool packSegment(std::size_t segment,
                     std::uint32_t* out) const
    {
        double c[4] = { 0., 0., 0., 0. };
        double tcur = 0.;
        double span = 1.;
        // The largest |u| the shader meets over the segment, whether u has no bound, and the
        // rounding of valueAt() that the shader cannot repeat.
        double uMax = 1.;
        bool unbounded = false;
        double cpuError = 0.;

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
                if (Interpolation::isWeightedSegment(cur.getInterpolation(), next.getInterpolation(),
                                                     cur.getRightWeight(), next.getLeftWeight())) {
                    return false;
                }
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
            if ((segment == 0) || (segment == _keys.size())) {
                // Both are lines, up to the rounding of the coefficients.
                unbounded = true;
            } else {
                const double first = (_keys[segment - 1].getTime() - tcur) / span;
                const double last = (_keys[segment].getTime() - tcur) / span;
                uMax = (std::max)(std::fabs(first), std::fabs(last));
            }
            if (segment == 0) {
                // About the virtual key one before the first, the line's value near that key is a
                // difference of two terms its slope scales, which cancel in the shader's precision:
                // the same cubic about the first key, in u - 1, has no such terms. The CPU's own
                // rounding of that difference cannot be repeated, so it counts against the bound.
                cpuError = kCpuDoubleError * (std::fabs(c[0]) + std::fabs(c[1]) + std::fabs(c[2]) + std::fabs(c[3])) * (std::max)(1., std::fabs(tnext));
                const double shifted[4] = { c[0] + c[1] + c[2] + c[3], c[1] + 2 * c[2] + 3 * c[3], c[2] + 3 * c[3], c[3] };
                std::copy(shifted, shifted + 4, c);
                tcur = _keys.front().getTime();
            }
        }
        const double invSpan = 1. / span;
        double magnitude, slope;
        if (unbounded) {
            // On a line v = c0 + c1 u the terms sum to at most |v| + 2 |c0|, and an error relative
            // to |v| is within the relative bound.
            magnitude = 2 * std::fabs(c[0]) + std::fabs(c[2]) + std::fabs(c[3]);
            slope = std::fabs(c[1]) + 2 * std::fabs(c[2]) + 3 * std::fabs(c[3]);
        } else {
            magnitude = std::fabs(c[0]) + uMax * (std::fabs(c[1]) + uMax * (std::fabs(c[2]) + uMax * std::fabs(c[3])));
            slope = std::fabs(c[1]) + uMax * (2 * std::fabs(c[2]) + uMax * 3 * std::fabs(c[3]));
        }
        // The origin's remainder rounds to a float, which moves t - origin by up to
        // kGpuSplitError |origin|, and the slope scales that.
        const double originError = kGpuSplitError * std::fabs(tcur) * std::fabs(invSpan) * slope;
        const double error = kGpuTwoFloatError * magnitude + originError + cpuError;

        bool ok = putSplit(out, tcur);
        ok = putSplit(out + 2, invSpan) && ok;
        for (int i = 0; i < 4; ++i) {
            ok = putSplit(out + 4 + 2 * i, c[i]) && ok;
        }

        return ok && (error <= kGpuSegmentErrorBound);
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

/**
 * @brief The immutable tables an op builds from the curves of one knob, kept for as long as the
 * knob's content hash stays the same. An op's bake() runs for every identity, region, cost and
 * render query, and tables sampled from curves are too costly to rebuild each time; Params hold
 * the shared_ptr get() returns, so a render keeps its tables whatever the knob does meanwhile.
 * MT-safe: concurrent bakes may each build on a miss, and the last one to finish is kept.
 **/
template <typename TABLES>
class CurveTablesCache {
public:
    /**
     * @brief The tables build() returns for knob's current content: a TABLES built from the
     * curves build() reads from knob, or the ones an earlier call built for the same content.
     **/
    template <typename KNOB, typename BUILD>
    std::shared_ptr<const TABLES> get(const KNOB& knob,
                                      BUILD&& build) const
    {
        const auto before = knob.getContentHash();
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_tables && (_hash == before)) {
                return _tables;
            }
        }
        std::shared_ptr<const TABLES> tables = std::make_shared<const TABLES>(std::forward<BUILD>(build)());
        // Tables built while the knob was edited may hold its later content, which must not be
        // kept under the earlier hash: undoing the edit would bring that hash back.
        if (knob.getContentHash() == before) {
            std::lock_guard<std::mutex> lock(_mutex);
            _hash = before;
            _tables = tables;
        }

        return tables;
    }

private:
    mutable std::mutex _mutex;
    mutable std::uint64_t _hash = 0;
    mutable std::shared_ptr<const TABLES> _tables;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Image_CurveSnapshot_h
