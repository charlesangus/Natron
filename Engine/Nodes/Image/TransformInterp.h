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

#ifndef Engine_Nodes_Image_TransformInterp_h
#define Engine_Nodes_Image_TransformInterp_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <functional>
#include <vector>

#include "Engine/Nodes/Image/TransformMath.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Per-pixel interpolation of a transform for the variable warps, engine free.
 *
 * A variable warp gives each output pixel a Map value m in [0, 1] and samples the source through
 * the transform at m, T(m): the identity at m = 0 and the base node's transform at m = 1. The
 * transform itself is interpolated, never the images of several transforms, so the result has a
 * single edge however the Map varies.
 *
 * T is any matrix function of m (MatrixFn). A Transform's is transformAt(): translate, rotate and
 * skew linear in m, scale geometric from 1 (linear when the scale is not positive), the centre
 * fixed, exactly as Transform's own amount knob blends. A CornerPin's blends the "to" points from
 * the "from" points by m and solves the homography at that blend.
 *
 * Per pixel the matrix comes from a MatrixCurve: the exact matrices at segments + 1 evenly spaced
 * knots of m and their derivatives in m, joined by cubic Hermite interpolation. The curve is
 * exact at its knots (m = 0, 1/2 and 1 among them) and within a small fraction of a pixel
 * between, and it gives the derivative in m the Jacobian needs. The same knots drive the GPU.
 **/
namespace TransformInterp {
/**
 * @brief The matrix at m, false when there is none. For a warp this is the pixel-space inverse
 * matrix (output pixels to source pixels) a render samples with.
 **/
typedef std::function<bool(double m, TransformMath::Mat3* matrix)> MatrixFn;

/// The knot spacing a curve uses unless its transform asks for more.
const int kDefaultSegments = 256;

/// The most segments a curve takes, which bounds the GPU parameter block.
const int kMaxSegments = 512;

/**
 * @brief The floats one knot packs into: its matrix as the high floats of its doubles, then the
 * rest of each double as a float, then its derivative, each row major. The GPU carries the
 * matrix in two floats so that its sample positions round to the CPU's.
 **/
const int kFloatsPerKnot = 27;

/**
 * @brief The canonical matrix of a Transform's knobs at Map value m:
 * TransformMath::inverseTransformCanonical(p, m, invert). m = 0 is the identity and m = 1 the
 * Transform node's own matrix.
 **/
TransformMath::Mat3 transformAt(const TransformMath::TransformParams& p, double m, bool invert);

/**
 * @brief The segments a Transform's curve needs: kDefaultSegments, or more for a rotation (times
 * the amount) of over 1024 degrees, so that no segment turns by more than 4 degrees; at most
 * kMaxSegments.
 **/
int segmentsForTransform(const TransformMath::TransformParams& p);

/**
 * @brief The derivative of fn at m, from finite differences that stay within [0, 1]: central
 * inside, second-order one-sided at the ends (a Transform's scale changes formula at m = 0).
 * False when fn fails at any point it needs.
 **/
bool derivative(const MatrixFn& fn, double m, TransformMath::Mat3* dH);

/**
 * @brief The interpolated matrix curve of a MatrixFn over m in [0, 1].
 **/
class MatrixCurve {
public:
    MatrixCurve() = default;

    /**
     * @brief Samples fn at segments + 1 knots, segments clamped to [1, kMaxSegments], with
     * derivative() at each. False, leaving the curve invalid, when fn fails at a knot or gives a
     * matrix that is not finite.
     **/
    bool build(const MatrixFn& fn, int segments);

    /// A curve whose matrix does not depend on m, the identity at every knot for instance.
    static MatrixCurve constant(const TransformMath::Mat3& matrix);

    bool isValid() const
    {
        return !_h.empty();
    }

    int segments() const
    {
        return _h.empty() ? 0 : (int)_h.size() - 1;
    }

    /**
     * @brief The matrix and its derivative in m at m, clamped to [0, 1]. Exact at the knots. The
     * curve must be valid.
     **/
    void evaluate(double m, TransformMath::Mat3* H, TransformMath::Mat3* dH) const;

    /// Whether every knot holds the same matrix, so the Map does not change the transform.
    bool isConstant() const;

    /// The matrix at knot k, 0 <= k <= segments().
    const TransformMath::Mat3& knot(int k) const
    {
        return _h[k];
    }

    /// Two curves with the same knots and derivatives.
    bool operator==(const MatrixCurve& other) const;

    /**
     * @brief Appends kFloatsPerKnot floats per knot to out, knot 0 first: the matrix's high
     * floats, its low floats (each double less its high float), then the derivative, each row
     * major.
     **/
    void pack(std::vector<float>* out) const;

private:
    std::vector<TransformMath::Mat3> _h;
    std::vector<TransformMath::Mat3> _d;
};

/**
 * @brief Where an output pixel samples the source, and the footprint of that sample.
 **/
struct SourceSample {
    double x = 0.;
    double y = 0.;
    // (Jxx, Jxy, Jyx, Jyy): d(x, y)/d(px, py), as Resampler::resampleRowFromCoords() takes it.
    double jacobian[4] = { 0., 0., 0., 0. };
};

/**
 * @brief The source point H (px, py, 1) and its Jacobian in (px, py), including the change of
 * the matrix with the Map: dH is the derivative of H in m and (dmdx, dmdy) the gradient of m at
 * the pixel. False for a point at or behind infinity (z <= 0) or one that is not finite, which
 * samples nothing.
 **/
bool sourceSample(const TransformMath::Mat3& H,
                  const TransformMath::Mat3& dH,
                  double px,
                  double py,
                  double dmdx,
                  double dmdy,
                  SourceSample* sample);
} // namespace TransformInterp

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Image_TransformInterp_h
