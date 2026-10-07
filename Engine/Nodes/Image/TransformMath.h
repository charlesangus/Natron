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

#ifndef Engine_Nodes_Image_TransformMath_h
#define Engine_Nodes_Image_TransformMath_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cfloat>
#include <cmath>

#include "Engine/Transform.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief 2D homogeneous transform maths for the native spatial nodes, with the operation order
 * and double arithmetic of openfx-misc's ofxsMatrix2D.h and Transform.cpp, so matrices built
 * here equal the OpenFX ones bit for bit.
 *
 * Conventions:
 *  - canonical coordinates are the project's (render scale 1, square pixels); pixel coordinates
 *    are those of an image at a render scale and pixel aspect ratio (PAR), where the centre of
 *    pixel (i, j) is (i + 0.5, j + 0.5);
 *  - an "inverse" transform maps a destination point to the source point it samples (what a
 *    renderer needs); a "forward" transform maps source to destination (what a RoD needs);
 *  - rotation angles are in degrees at the knob level and radians in the matrix builders.
 **/
namespace TransformMath {
inline double
toRadians(double deg)
{
    return deg * M_PI / 180.;
}

struct Point3 {
    double x;
    double y;
    double z;

    Point3()
        : x(0.)
        , y(0.)
        , z(0.)
    {
    }

    Point3(double x_,
           double y_,
           double z_)
        : x(x_)
        , y(y_)
        , z(z_)
    {
    }
};

/**
 * @brief A row-major 3x3 matrix: m[0..2] is the first row. Default-constructed to all zeros,
 * as the OpenFX one is; use identity() for the identity.
 **/
struct Mat3 {
    double m[9];

    Mat3()
    {
        for (int k = 0; k < 9; ++k) {
            m[k] = 0.;
        }
    }

    Mat3(double a,
         double b,
         double c,
         double d,
         double e,
         double f,
         double g,
         double h,
         double i)
    {
        m[0] = a;
        m[1] = b;
        m[2] = c;
        m[3] = d;
        m[4] = e;
        m[5] = f;
        m[6] = g;
        m[7] = h;
        m[8] = i;
    }

    static Mat3 identity()
    {
        return Mat3(1., 0., 0., 0., 1., 0., 0., 0., 1.);
    }

    double& operator()(int row,
                       int col)
    {
        return m[row * 3 + col];
    }

    double operator()(int row,
                      int col) const
    {
        return m[row * 3 + col];
    }

    bool isIdentity() const
    {
        return m[0] == 1 && m[1] == 0 && m[2] == 0 && m[3] == 0 && m[4] == 1 && m[5] == 0 && m[6] == 0 && m[7] == 0 && m[8] == 1;
    }

    bool operator==(const Mat3& o) const
    {
        for (int k = 0; k < 9; ++k) {
            if (m[k] != o.m[k]) {
                return false;
            }
        }
        return true;
    }

    bool operator!=(const Mat3& o) const
    {
        return !(*this == o);
    }

    Mat3 operator*(const Mat3& m2) const
    {
        return Mat3(m[0] * m2.m[0] + m[1] * m2.m[3] + m[2] * m2.m[6],
                    m[0] * m2.m[1] + m[1] * m2.m[4] + m[2] * m2.m[7],
                    m[0] * m2.m[2] + m[1] * m2.m[5] + m[2] * m2.m[8],
                    m[3] * m2.m[0] + m[4] * m2.m[3] + m[5] * m2.m[6],
                    m[3] * m2.m[1] + m[4] * m2.m[4] + m[5] * m2.m[7],
                    m[3] * m2.m[2] + m[4] * m2.m[5] + m[5] * m2.m[8],
                    m[6] * m2.m[0] + m[7] * m2.m[3] + m[8] * m2.m[6],
                    m[6] * m2.m[1] + m[7] * m2.m[4] + m[8] * m2.m[7],
                    m[6] * m2.m[2] + m[7] * m2.m[5] + m[8] * m2.m[8]);
    }

    Point3 operator*(const Point3& p) const
    {
        return Point3(m[0] * p.x + m[1] * p.y + m[2] * p.z,
                      m[3] * p.x + m[4] * p.y + m[5] * p.z,
                      m[6] * p.x + m[7] * p.y + m[8] * p.z);
    }

    double determinant() const
    {
        return m[0] * (m[4] * m[8] - m[7] * m[5]) - m[1] * (m[3] * m[8] - m[6] * m[5]) + m[2] * (m[3] * m[7] - m[6] * m[4]);
    }

    /**
     * @brief Writes the inverse to *out and returns true, or returns false (out untouched) for a
     * singular matrix.
     **/
    bool inverse(Mat3* out) const
    {
        double inv[9];

        inv[0] = (m[4] * m[8] - m[7] * m[5]);
        inv[3] = (m[5] * m[6] - m[3] * m[8]);
        inv[6] = (m[3] * m[7] - m[4] * m[6]);

        double det = m[0] * inv[0] + m[1] * inv[3] + m[2] * inv[6];
        if (det == 0) {
            return false;
        }

        inv[1] = (m[2] * m[7] - m[1] * m[8]);
        inv[4] = (m[0] * m[8] - m[2] * m[6]);
        inv[7] = (m[1] * m[6] - m[0] * m[7]);

        inv[2] = (m[1] * m[5] - m[2] * m[4]);
        inv[5] = (m[2] * m[3] - m[0] * m[5]);
        inv[8] = (m[0] * m[4] - m[1] * m[3]);

        det = 1.0 / det;
        for (int k = 0; k < 9; ++k) {
            out->m[k] = inv[k] * det;
        }
        return true;
    }
};

/// Converts to the engine's matrix type, as getTransform() returns it.
inline Transform::Matrix3x3
toEngineMatrix(const Mat3& mat)
{
    return Transform::Matrix3x3(mat.m[0], mat.m[1], mat.m[2], mat.m[3], mat.m[4], mat.m[5], mat.m[6], mat.m[7], mat.m[8]);
}

/// Converts from the engine's matrix type, e.g. a concatenated input transform from getImage().
inline Mat3
fromEngineMatrix(const Transform::Matrix3x3& mat)
{
    return Mat3(mat.a, mat.b, mat.c, mat.d, mat.e, mat.f, mat.g, mat.h, mat.i);
}

inline Mat3
translation(double x,
            double y)
{
    return Mat3(1., 0., x, 0., 1., y, 0., 0., 1.);
}

inline Mat3
scale(double x,
      double y)
{
    return Mat3(x, 0., 0., 0., y, 0., 0., 0., 1.);
}

/// The OpenFX rotation matrix (cos, sin; -sin, cos); transformCanonical() rotates by -rads.
inline Mat3
rotation(double rads)
{
    double c = std::cos(rads);
    double s = std::sin(rads);

    return Mat3(c, s, 0, -s, c, 0, 0, 0, 1);
}

/**
 * @brief The skew matrix. skewOrderYX false is the "XY" order of the skewOrder knob (option 0).
 **/
inline Mat3
skewXY(double skewX,
       double skewY,
       bool skewOrderYX)
{
    return Mat3(skewOrderYX ? 1. : (1. + skewX * skewY), skewX, 0., skewY, skewOrderYX ? (1. + skewX * skewY) : 1, 0., 0., 0., 1.);
}

/**
 * @brief Destination to source, canonical: the inverse of transformCanonical(). Scales below
 * FLT_MIN in magnitude are clamped to it to avoid a division by zero.
 **/
inline Mat3
inverseTransformCanonical(double translateX,
                          double translateY,
                          double scaleX,
                          double scaleY,
                          double skewX,
                          double skewY,
                          bool skewOrderYX,
                          double rads,
                          double centerX,
                          double centerY)
{
    if (std::fabs(scaleX) < FLT_MIN) {
        scaleX = scaleX > 0 ? FLT_MIN : -FLT_MIN;
    }
    if (std::fabs(scaleY) < FLT_MIN) {
        scaleY = scaleY > 0 ? FLT_MIN : -FLT_MIN;
    }

    return translation(centerX, centerY) * scale(1. / scaleX, 1. / scaleY) * skewXY(-skewX, -skewY, !skewOrderYX) * rotation(rads) * translation(-translateX, -translateY) * translation(-centerX, -centerY);
}

/**
 * @brief Source to destination, canonical: about the centre, scale, then skew, then rotate,
 * then translate.
 **/
inline Mat3
transformCanonical(double translateX,
                   double translateY,
                   double scaleX,
                   double scaleY,
                   double skewX,
                   double skewY,
                   bool skewOrderYX,
                   double rads,
                   double centerX,
                   double centerY)
{
    return translation(centerX, centerY) * translation(translateX, translateY) * rotation(-rads) * skewXY(skewX, skewY, skewOrderYX) * scale(scaleX, scaleY) * translation(-centerX, -centerY);
}

/**
 * @brief Pixel to canonical for an image of the given PAR at render scale (sx, sy). fielded
 * halves the vertical scale, for a lower or upper field render.
 **/
inline Mat3
pixelToCanonical(double par,
                 double sx,
                 double sy,
                 bool fielded = false)
{
    return scale(par / sx, 1. / (sy * (fielded ? 0.5 : 1.0)));
}

inline Mat3
canonicalToPixel(double par,
                 double sx,
                 double sy,
                 bool fielded = false)
{
    return scale(sx / par, sy * (fielded ? 0.5 : 1.0));
}

/**
 * @brief A canonical inverse transform (destination to source) in pixel coordinates, the matrix
 * Resampler::resampleRow() samples with: destination pixels in, source pixels out.
 **/
inline Mat3
inverseToPixel(const Mat3& invCanonical,
               double srcPar,
               double dstPar,
               double sx,
               double sy,
               bool fielded = false)
{
    return canonicalToPixel(srcPar, sx, sy, fielded) * invCanonical * pixelToCanonical(dstPar, sx, sy, fielded);
}

/**
 * @brief A canonical forward transform (source to destination) in pixel coordinates, the matrix
 * getTransform() hands the host for concatenation.
 **/
inline Mat3
forwardToPixel(const Mat3& fwdCanonical,
               double srcPar,
               double dstPar,
               double sx,
               double sy,
               bool fielded = false)
{
    return canonicalToPixel(dstPar, sx, sy, fielded) * fwdCanonical * pixelToCanonical(srcPar, sx, sy, fielded);
}

/**
 * @brief The knob values of a Transform node, as read at one time: translate and center in
 * canonical coordinates, rotate in degrees, scale as the 2-D knob (scaleY is ignored when
 * scaleUniform is set), skewOrderYX the skewOrder choice (0 = XY, 1 = YX), amount the
 * transformAmount knob.
 **/
struct TransformParams {
    double translateX;
    double translateY;
    double rotate;
    double scaleX;
    double scaleY;
    bool scaleUniform;
    double skewX;
    double skewY;
    bool skewOrderYX;
    double centerX;
    double centerY;
    double amount;

    TransformParams()
        : translateX(0.)
        , translateY(0.)
        , rotate(0.)
        , scaleX(1.)
        , scaleY(1.)
        , scaleUniform(false)
        , skewX(0.)
        , skewY(0.)
        , skewOrderYX(false)
        , centerX(0.)
        , centerY(0.)
        , amount(1.)
    {
    }
};

/**
 * @brief The scale actually applied: scaleY replaced by scaleX when uniform, and each kept at
 * least 0.0001 in magnitude.
 **/
inline void
effectiveScale(double scaleParamX,
               double scaleParamY,
               bool scaleUniform,
               double* outX,
               double* outY)
{
    const double kScaleMin = 0.0001;

    *outX = scaleParamX;
    if (std::fabs(*outX) < kScaleMin) {
        *outX = (*outX >= 0) ? kScaleMin : -kScaleMin;
    }
    *outY = scaleUniform ? scaleParamX : scaleParamY;
    if (std::fabs(*outY) < kScaleMin) {
        *outY = (*outY >= 0) ? kScaleMin : -kScaleMin;
    }
}

/**
 * @brief Whether the parameters leave the image unchanged: amount 0, or neutral translate,
 * rotate, skew and effective scale.
 **/
inline bool
isIdentity(const TransformParams& p)
{
    if (p.amount == 0.) {
        return true;
    }
    double sx, sy;
    effectiveScale(p.scaleX, p.scaleY, p.scaleUniform, &sx, &sy);

    return sx == 1. && sy == 1. && p.translateX == 0. && p.translateY == 0. && p.rotate == 0. && p.skewX == 0. && p.skewY == 0.;
}

/**
 * @brief The canonical matrix of the parameters at blend factor `amount` (1 for a plain
 * render; motion and directional blur vary it), multiplied by p.amount. Translate, rotate and
 * skew scale linearly with the total amount; scale is interpolated geometrically from 1, or
 * linearly when the scale or the amount is not positive.
 *
 * invert false gives the inverse (destination to source) matrix a renderer samples with;
 * invert true gives the forward one. A node whose "invert" knob is on passes it through, and
 * a RoD, which needs the forward matrix, passes !invert.
 **/
inline Mat3
inverseTransformCanonical(const TransformParams& p,
                          double amount,
                          bool invert)
{
    double translateX = p.translateX;
    double translateY = p.translateY;
    double rotate = p.rotate;
    double skewX = p.skewX;
    double skewY = p.skewY;

    amount *= p.amount;

    double sx, sy;
    effectiveScale(p.scaleX, p.scaleY, p.scaleUniform, &sx, &sy);

    if (amount != 1.) {
        translateX *= amount;
        translateY *= amount;
        if (sx <= 0. || amount <= 0.) {
            sx = 1. + (sx - 1.) * amount;
        } else {
            sx = std::pow(sx, amount);
        }
        if (sy <= 0 || amount <= 0.) {
            sy = 1. + (sy - 1.) * amount;
        } else {
            sy = std::pow(sy, amount);
        }
        rotate *= amount;
        skewX *= amount;
        skewY *= amount;
    }

    double rot = toRadians(rotate);
    if (!invert) {
        return inverseTransformCanonical(translateX, translateY, sx, sy, skewX, skewY, p.skewOrderYX, rot, p.centerX, p.centerY);
    }
    return transformCanonical(translateX, translateY, sx, sy, skewX, skewY, p.skewOrderYX, rot, p.centerX, p.centerY);
}
} // namespace TransformMath

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Image_TransformMath_h
