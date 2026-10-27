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

/*
 * The maths are a port of darktable's rotate and perspective module, DARKTABLE_REF release-5.6.2
 * (commit 20891f6c6fa7), src/iop/ashift.c, Copyright (C) 2016-2026 darktable developers,
 * distributed under the GNU GPL version 3 or later. simplex() ports src/iop/ashift_nmsimplex.c,
 * after Michael F. Hutt's Nelder-Mead simplex, Copyright (c) 1997-2011 Michael F. Hutt, under the
 * MIT licence.
 */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "AshiftFit.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <limits>

NATRON_NAMESPACE_ENTER

namespace Ashift {

namespace {
    constexpr double kNmsEpsilon = 1e-3;
    constexpr double kNmsScale = 1.;
    constexpr int kNmsIterations = 400;
    constexpr double kNmsCropEpsilon = 100.;
    constexpr double kNmsCropScale = 0.5;
    constexpr int kNmsCropIterations = 100;
    constexpr double kNmsAlpha = 1.;
    constexpr double kNmsBeta = 0.5;
    constexpr double kNmsGamma = 2.;

    using TransformMath::Mat3;
    using TransformMath::Point3;

    void
    mulv(double* dst,
         const Mat3& m,
         const double* v)
    {
        const Point3 p = m * Point3(v[0], v[1], v[2]);

        dst[0] = p.x;
        dst[1] = p.y;
        dst[2] = p.z;
    }

    // darktable's mat3inv(): fails on |det| < 1e-7.
    bool
    invert(const Mat3& a,
           Mat3* out)
    {
        if (std::fabs(a.determinant()) < 1e-7) {
            return false;
        }

        return a.inverse(out);
    }

    double
    sqr(double x)
    {
        return x * x;
    }

    double
    logit(double x,
          double min,
          double max)
    {
        const double eps = 1.0e-6;
        const double p = std::clamp((x - min) / (max - min), eps, 1.0 - eps);

        return 2.0 * std::atanh(2.0 * p - 1.0);
    }

    double
    ilogit(double L,
           double min,
           double max)
    {
        const double p = 0.5 * (1.0 + std::tanh(0.5 * L));

        return p * (max - min) + min;
    }

    struct CropFit {
        int width = 0;
        int height = 0;
        double x = NAN;
        double y = NAN;
        double alpha = NAN;
        Mat3 homograph;
        double edges[4][3];
    };

    // The image's corners mapped forward, its bounding box size and its edges.
    void
    projectFrame(const Mat3& h,
                 double wd,
                 double ht,
                 double* owd,
                 double* oht,
                 double edges[4][3])
    {
        const double Vc[4][3] = { { 0., 0., 1. }, { 0., ht, 1. }, { wd, ht, 1. }, { wd, 0., 1. } };
        double V[4][3];
        double xmin = DBL_MAX, ymin = DBL_MAX, xmax = -DBL_MAX, ymax = -DBL_MAX;

        for (int n = 0; n < 4; ++n) {
            mulv(V[n], h, Vc[n]);
            V[n][0] /= V[n][2];
            V[n][1] /= V[n][2];
            V[n][2] = 1.;
            xmin = std::min(xmin, V[n][0]);
            xmax = std::max(xmax, V[n][0]);
            ymin = std::min(ymin, V[n][1]);
            ymax = std::max(ymax, V[n][1]);
        }
        *owd = xmax - xmin;
        *oht = ymax - ymin;
        for (int n = 0; n < 4; ++n) {
            vec3prodn(edges[n], V[n], V[(n + 1) % 4]);
        }
    }

    // The squared distance from P to the nearest image edge along the rectangle's diagonals.
    double
    diagonalDistance2(const double edges[4][3],
                      const double* P,
                      double alpha)
    {
        const double Pa[2][3] = { { P[0] + 10. * std::cos(alpha), P[1] + 10. * std::sin(alpha), 1. },
                                  { P[0] + 10. * std::cos(alpha), P[1] - 10. * std::sin(alpha), 1. } };
        double D[2][3];

        vec3prodn(D[0], P, Pa[0]);
        vec3prodn(D[1], P, Pa[1]);
        double d2min = DBL_MAX;
        for (int k = 0; k < 4; ++k) {
            for (int l = 0; l < 2; ++l) {
                double I[3];
                vec3prodn(I, edges[k], D[l]);
                if (vec3isnull(I)) {
                    d2min = 0.;
                    break;
                }
                if (I[2] == 0.) {
                    continue;
                }
                I[0] /= I[2];
                I[1] /= I[2];
                d2min = std::min(d2min, sqr(P[0] - I[0]) + sqr(P[1] - I[1]));
            }
        }

        return d2min;
    }

    double
    cropFitness(const double* params,
                const CropFit& cropfit)
    {
        const double wd = cropfit.width;
        const double ht = cropfit.height;
        const double x = std::isnan(cropfit.x) ? params[0] : cropfit.x;
        const double y = std::isnan(cropfit.y) ? params[1] : cropfit.y;
        const double alpha = std::isnan(cropfit.alpha) ? params[2] : cropfit.alpha;
        const double Pc[3] = { x * wd, y * ht, 1. };
        double P[3];

        mulv(P, cropfit.homograph, Pc);
        P[0] /= P[2];
        P[1] /= P[2];
        P[2] = 1.;
        const double d2min = diagonalDistance2(cropfit.edges, P, alpha);

        return -(2. * d2min * std::sin(2. * alpha));
    }

    void
    cropConstraint(double* params,
                   int pcount)
    {
        if (pcount > 0) {
            params[0] = std::fabs(params[0]);
        }
        if (pcount > 1) {
            params[1] = std::fabs(params[1]);
        }
        if (pcount > 2) {
            params[2] = std::fabs(params[2]);
        }
        if ((pcount > 0) && (params[0] > 1.)) {
            params[0] = 1. - params[0];
        }
        if ((pcount > 1) && (params[1] > 1.)) {
            params[1] = 1. - params[1];
        }
        if ((pcount > 2) && (params[2] > 0.5 * M_PI)) {
            params[2] = 0.5 * M_PI - params[2];
        }
    }

    struct FitParams {
        const LineSet* lines = nullptr;
        int width = 0;
        int height = 0;
        double focalLength = kDefaultFocalLength;
        double orthocorr = 0.;
        double aspect = 1.;
        double rotation = 0.;
        double lensShiftV = 0.;
        double lensShiftH = 0.;
        double shear = 0.;
        double rotationRange = kRotationRange;
        double lensShiftVRange = kLensShiftRange;
        double lensShiftHRange = kLensShiftRange;
        double shearRange = kShearRange;
        int lineType = 0;
        int lineMask = 0;
        int paramsCount = 0;
    };

    Model
    fitModel(const FitParams& fit,
             double rotation,
             double lensShiftV,
             double lensShiftH,
             double shear)
    {
        Model m;

        m.rotation = rotation;
        m.lensShiftV = lensShiftV;
        m.lensShiftH = lensShiftH;
        m.shear = shear;
        m.focalLength = fit.focalLength;
        m.orthocorr = fit.orthocorr;
        m.aspect = fit.aspect;

        return m;
    }

    double
    modelFitness(const double* params,
                 const FitParams& fit)
    {
        double rotation = fit.rotation;
        double lensShiftV = fit.lensShiftV;
        double lensShiftH = fit.lensShiftH;
        double shear = fit.shear;
        int pcount = 0;

        if (std::isnan(rotation)) {
            rotation = ilogit(params[pcount++], -fit.rotationRange, fit.rotationRange);
        }
        if (std::isnan(lensShiftV)) {
            lensShiftV = ilogit(params[pcount++], -fit.lensShiftVRange, fit.lensShiftVRange);
        }
        if (std::isnan(lensShiftH)) {
            lensShiftH = ilogit(params[pcount++], -fit.lensShiftHRange, fit.lensShiftHRange);
        }
        if (std::isnan(shear)) {
            shear = ilogit(params[pcount++], -fit.shearRange, fit.shearRange);
        }
        const double Av[3] = { 1., 0., 0. };
        const double Ah[3] = { 0., 1., 0. };
        const Mat3 h = homography(fitModel(fit, rotation, lensShiftV, lensShiftH, shear), fit.width, fit.height, false);

        double sumsqV = 0., sumsqH = 0., weightV = 0., weightH = 0.;
        int countV = 0, countH = 0, count = 0;
        for (const Line& line : fit.lines->lines) {
            if ((line.type & fit.lineMask) != fit.lineType) {
                continue;
            }
            const bool isVertical = (line.type & eLineDirVert) != 0;
            const double* A = isVertical ? Ah : Av;
            double P1[3], P2[3], L[3];
            mulv(P1, h, line.p1);
            mulv(P2, h, line.p2);
            vec3prodn(L, P1, P2);
            vec3lnorm(L, L);
            const double s = vec3scalar(L, A);
            if (isVertical) {
                sumsqV += s * s * line.weight;
                weightV += line.weight;
                ++countV;
            } else {
                sumsqH += s * s * line.weight;
                weightH += line.weight;
                ++countH;
            }
            ++count;
        }
        const double v = ((weightV > 0.) && (count > 0)) ? sumsqV / weightV * (double)countV / count : 0.;
        const double hh = ((weightH > 0.) && (count > 0)) ? sumsqH / weightH * (double)countH / count : 0.;

        return std::sqrt(1.0 - (1.0 - v) * (1.0 - hh)) * 1.0e6;
    }
} // anonymous namespace

void
vec3prodn(double* dst,
          const double* v1,
          const double* v2)
{
    const double l1 = v1[1] * v2[2] - v1[2] * v2[1];
    const double l2 = v1[2] * v2[0] - v1[0] * v2[2];
    const double l3 = v1[0] * v2[1] - v1[1] * v2[0];
    const double sq = std::sqrt(l1 * l1 + l2 * l2 + l3 * l3);
    const double f = (sq > 0.) ? 1. / sq : 1.;

    dst[0] = l1 * f;
    dst[1] = l2 * f;
    dst[2] = l3 * f;
}

void
vec3norm(double* dst,
         const double* v)
{
    const double sq = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    const double f = (sq > 0.) ? 1. / sq : 1.;

    dst[0] = v[0] * f;
    dst[1] = v[1] * f;
    dst[2] = v[2] * f;
}

void
vec3lnorm(double* dst,
          const double* v)
{
    const double sq = std::hypot(v[0], v[1]);
    const double f = (sq > 0.) ? 1. / sq : 1.;

    dst[0] = v[0] * f;
    dst[1] = v[1] * f;
    dst[2] = v[2] * f;
}

double
vec3scalar(const double* v1,
           const double* v2)
{
    return v1[0] * v2[0] + v1[1] * v2[1] + v1[2] * v2[2];
}

bool
vec3isnull(const double* v)
{
    const double eps = 1e-10;

    return (std::fabs(v[0]) < eps) && (std::fabs(v[1]) < eps) && (std::fabs(v[2]) < eps);
}

Mat3
homography(const Model& model,
           int width,
           int height,
           bool inverse)
{
    const double u = width;
    const double v = height;
    const double phi = model.rotation * M_PI / 180.;
    const double cosi = std::cos(phi);
    const double sini = std::sin(phi);
    const double ascale = std::sqrt(model.aspect);
    const double f = model.focalLength;

    const double horifac = 1. - model.orthocorr / 100.;
    const double exppaV = std::exp(model.lensShiftV);
    const double fdbV = f / (14.4 + (v / u - 1.) * 7.2);
    const double radV = fdbV * (exppaV - 1.) / (exppaV + 1.);
    const double alphaV = std::clamp(std::atan(radV), -1.5, 1.5);
    const double rtV = std::sin(0.5 * alphaV);
    const double rV = std::max(0.1, 2. * (horifac - 1.) * rtV * rtV + 1.);

    const double vertifac = 1. - model.orthocorr / 100.;
    const double exppaH = std::exp(model.lensShiftH);
    const double fdbH = f / (14.4 + (u / v - 1.) * 7.2);
    const double radH = fdbH * (exppaH - 1.) / (exppaH + 1.);
    const double alphaH = std::clamp(std::atan(radH), -1.5, 1.5);
    const double rtH = std::sin(0.5 * alphaH);
    const double rH = std::max(0.1, 2. * (vertifac - 1.) * rtH * rtH + 1.);

    const Mat3 swap(0., 1., 0., 1., 0., 0., 0., 0., 1.);
    Mat3 m = swap;
    m = Mat3(cosi, -sini, -0.5 * v * cosi + 0.5 * u * sini + 0.5 * v,
             sini, cosi, -0.5 * v * sini - 0.5 * u * cosi + 0.5 * u,
             0., 0., 1.)
        * m;
    m = Mat3(1., model.shear, 0.,
             model.shear, 1., 0.,
             0., 0., 1.)
        * m;
    m = Mat3(exppaV, 0., 0.,
             0.5 * ((exppaV - 1.) * u) / v, 2. * exppaV / (exppaV + 1.), -0.5 * ((exppaV - 1.) * u) / (exppaV + 1.),
             (exppaV - 1.) / v, 0., 1.)
        * m;
    m = Mat3(1., 0., 0.,
             0., rV, 0.5 * u * (1. - rV),
             0., 0., 1.)
        * m;
    m = swap * m;
    m = Mat3(exppaH, 0., 0.,
             0.5 * ((exppaH - 1.) * v) / u, 2. * exppaH / (exppaH + 1.), -0.5 * ((exppaH - 1.) * v) / (exppaH + 1.),
             (exppaH - 1.) / u, 0., 1.)
        * m;
    m = Mat3(1., 0., 0.,
             0., rH, 0.5 * v * (1. - rH),
             0., 0., 1.)
        * m;
    m = Mat3(ascale, 0., 0.,
             0., 1. / ascale, 0.,
             0., 0., 1.)
        * m;

    double umin = DBL_MAX, vmin = DBL_MAX;
    const int ys[2] = { 0, std::max(height - 1, 0) };
    const int xs[2] = { 0, std::max(width - 1, 0) };
    for (int y : ys) {
        for (int x : xs) {
            const Point3 po = m * Point3(x, y, 1.);
            umin = std::min(umin, po.x / po.z);
            vmin = std::min(vmin, po.y / po.z);
        }
    }
    m = Mat3(1., 0., -umin,
             0., 1., -vmin,
             0., 0., 1.)
        * m;

    if (!inverse) {
        return m;
    }
    Mat3 inv;
    if (!invert(m, &inv)) {
        return Mat3::identity();
    }

    return inv;
} // homography

bool
isNeutral(const Model& model,
          double cl,
          double cr,
          double ct,
          double cb)
{
    const double eps = 1.0e-4;

    return (std::fabs(model.rotation) < eps) && (std::fabs(model.lensShiftV) < eps) && (std::fabs(model.lensShiftH) < eps)
        && (std::fabs(model.shear) < eps) && (std::fabs(model.aspect - 1.) < eps) && (std::fabs(cl) < eps)
        && (std::fabs(cr - 1.) < eps) && (std::fabs(ct) < eps) && (std::fabs(cb - 1.) < eps);
}

void
outputSize(const Model& model,
           int width,
           int height,
           double cl,
           double cr,
           double ct,
           double cb,
           int* outWidth,
           int* outHeight)
{
    const Mat3 h = homography(model, width, height, false);
    double xm = DBL_MAX, xM = -DBL_MAX, ym = DBL_MAX, yM = -DBL_MAX;
    const int ys[2] = { 0, std::max(height - 1, 0) };
    const int xs[2] = { 0, std::max(width - 1, 0) };

    for (int y : ys) {
        for (int x : xs) {
            const Point3 po = h * Point3(x, y, 1.);
            xm = std::min(xm, po.x / po.z);
            xM = std::max(xM, po.x / po.z);
            ym = std::min(ym, po.y / po.z);
            yM = std::max(yM, po.y / po.z);
        }
    }
    *outWidth = (int)std::floor((xM - xm + 1.) * (cr - cl));
    *outHeight = (int)std::floor((yM - ym + 1.) * (cb - ct));
}

bool
computeCrop(const Model& model,
            int width,
            int height,
            CropModeEnum mode,
            CropBox* box)
{
    *box = CropBox();
    if ((mode == eCropOff) || (width <= 0) || (height <= 0)) {
        return true;
    }
    CropFit cropfit;
    cropfit.width = width;
    cropfit.height = height;
    cropfit.homograph = homography(model, width, height, false);
    const double wd = width;
    const double ht = height;
    double owd = 0., oht = 0.;
    projectFrame(cropfit.homograph, wd, ht, &owd, &oht, cropfit.edges);

    double params[3];
    int pcount;
    if (mode == eCropLargest) {
        params[0] = 0.5;
        params[1] = 0.5;
        params[2] = std::atan2(ht, wd);
        pcount = 3;
    } else {
        params[0] = 0.5;
        params[1] = 0.5;
        params[2] = 0.;
        cropfit.alpha = std::atan2(ht, wd);
        pcount = 2;
    }
    const int iter = simplex([&cropfit](const double* p) { return cropFitness(p, cropfit); },
                             params, pcount, kNmsCropEpsilon, kNmsCropScale, kNmsCropIterations, cropConstraint);
    if (iter >= kNmsCropIterations) {
        return false;
    }
    cropfit.x = std::isnan(cropfit.x) ? params[0] : cropfit.x;
    cropfit.y = std::isnan(cropfit.y) ? params[1] : cropfit.y;
    cropfit.alpha = std::isnan(cropfit.alpha) ? params[2] : cropfit.alpha;
    const double A = std::fabs(cropFitness(params, cropfit));
    if (A == 0.) {
        return false;
    }
    const double d = std::sqrt(A / (2. * std::sin(2. * cropfit.alpha)));
    const double Pc[3] = { cropfit.x * wd, cropfit.y * ht, 1. };
    double P[3];
    mulv(P, cropfit.homograph, Pc);
    P[0] /= P[2];
    P[1] /= P[2];

    CropBox b;
    b.cl = std::clamp((P[0] - d * std::cos(cropfit.alpha)) / owd, 0., 1.);
    b.cr = std::clamp((P[0] + d * std::cos(cropfit.alpha)) / owd, 0., 1.);
    b.ct = std::clamp((P[1] - d * std::sin(cropfit.alpha)) / oht, 0., 1.);
    b.cb = std::clamp((P[1] + d * std::sin(cropfit.alpha)) / oht, 0., 1.);
    if ((b.cr - b.cl <= 0.) || (b.cb - b.ct <= 0.)) {
        return false;
    }
    *box = b;

    return true;
} // computeCrop

bool
adjustCrop(const Model& model,
           int width,
           int height,
           double x,
           double y,
           CropBox* box)
{
    const double wd = width;
    const double ht = height;
    const double alpha = std::atan2(ht, wd);
    const Mat3 h = homography(model, width, height, false);
    double owd = 0., oht = 0.;
    double edges[4][3];

    projectFrame(h, wd, ht, &owd, &oht, edges);
    const double P[3] = { x * owd, y * oht, 1. };
    const double d = std::sqrt(diagonalDistance2(edges, P, alpha));
    const double A = 2. * d * d * std::sin(2. * alpha);
    if (A < 0.01 * wd * ht) {
        return false;
    }
    box->cl = std::clamp((P[0] - d * std::cos(alpha)) / owd, 0., 1.);
    box->cr = std::clamp((P[0] + d * std::cos(alpha)) / owd, 0., 1.);
    box->ct = std::clamp((P[1] - d * std::sin(alpha)) / oht, 0., 1.);
    box->cb = std::clamp((P[1] + d * std::sin(alpha)) / oht, 0., 1.);

    return true;
}

NmsResultEnum
fit(const LineSet& lines,
    int dir,
    Model* model)
{
    if (lines.lines.empty()) {
        return eNmsNotEnoughLines;
    }
    if (dir == eFitNone) {
        return eNmsSuccess;
    }
    double params[4];
    int pcount = 0;
    bool enoughLines = true;

    FitParams fit;
    fit.lines = &lines;
    fit.width = lines.width;
    fit.height = lines.height;
    fit.focalLength = model->focalLength;
    fit.orthocorr = model->orthocorr;
    fit.aspect = model->aspect;
    fit.rotation = model->rotation;
    fit.lensShiftV = model->lensShiftV;
    fit.lensShiftH = model->lensShiftH;
    fit.shear = model->shear;
    fit.lineType = eLineRelevant | eLineSelected;
    fit.lineMask = eLineMask;

    // darktable also flips the lens directions for portrait-flipped images; the orientation is
    // the source's here.
    int mdir = dir;
    if (((mdir & eFitLensBoth) != eFitLensBoth) && ((mdir & eFitLensBoth) != 0)) {
        mdir |= ((mdir & eFitLinesBoth) == 0) ? eFitLinesBoth : 0;
    }
    if (mdir & eFitRotation) {
        ++fit.paramsCount;
        params[pcount++] = logit(fit.rotation, -fit.rotationRange, fit.rotationRange);
        fit.rotation = NAN;
    }
    if (mdir & eFitLensVert) {
        ++fit.paramsCount;
        params[pcount++] = logit(fit.lensShiftV, -fit.lensShiftVRange, fit.lensShiftVRange);
        fit.lensShiftV = NAN;
    }
    if (mdir & eFitLensHor) {
        ++fit.paramsCount;
        params[pcount++] = logit(fit.lensShiftH, -fit.lensShiftHRange, fit.lensShiftHRange);
        fit.lensShiftH = NAN;
    }
    if (mdir & eFitShear) {
        ++fit.paramsCount;
        params[pcount++] = logit(fit.shear, -fit.shearRange, fit.shearRange);
        fit.shear = NAN;
    }
    if (mdir & eFitLinesVert) {
        fit.lineType |= eLineDirVert;
        enoughLines = enoughLines && (lines.verticalCount >= kMinimumFitLines);
    }
    if (mdir & eFitLinesHor) {
        enoughLines = enoughLines && (lines.horizontalCount >= kMinimumFitLines);
    }
    if ((mdir & eFitLinesBoth) == eFitLinesBoth) {
        fit.lineType = eLineRelevant | eLineSelected;
        fit.lineMask = eLineRelevant | eLineSelected;
    }
    if (!enoughLines) {
        return eNmsNotEnoughLines;
    }
    const int iter = simplex([&fit](const double* p) { return modelFitness(p, fit); },
                             params, fit.paramsCount, kNmsEpsilon, kNmsScale, kNmsIterations, nullptr);
    if (iter >= kNmsIterations) {
        return eNmsDidNotConverge;
    }
    pcount = 0;
    if (std::isnan(fit.rotation)) {
        fit.rotation = ilogit(params[pcount++], -fit.rotationRange, fit.rotationRange);
    }
    if (std::isnan(fit.lensShiftV)) {
        fit.lensShiftV = ilogit(params[pcount++], -fit.lensShiftVRange, fit.lensShiftVRange);
    }
    if (std::isnan(fit.lensShiftH)) {
        fit.lensShiftH = ilogit(params[pcount++], -fit.lensShiftHRange, fit.lensShiftHRange);
    }
    if (std::isnan(fit.shear)) {
        fit.shear = ilogit(params[pcount++], -fit.shearRange, fit.shearRange);
    }
    const Model result = fitModel(fit, fit.rotation, fit.lensShiftV, fit.lensShiftH, fit.shear);
    const Mat3 h = homography(result, fit.width, fit.height, false);
    double xm = DBL_MAX, xM = -DBL_MAX, ym = DBL_MAX, yM = -DBL_MAX;
    const int ys[2] = { 0, std::max(fit.height - 1, 0) };
    const int xs[2] = { 0, std::max(fit.width - 1, 0) };
    for (int y : ys) {
        for (int x : xs) {
            const Point3 po = h * Point3(x, y, 1.);
            xm = std::min(xm, po.x / po.z);
            ym = std::min(ym, po.y / po.z);
            xM = std::max(xM, po.x / po.z);
            yM = std::max(yM, po.y / po.z);
        }
    }
    if ((xM - xm) * (yM - ym) > 4. * fit.width * fit.height) {
        return eNmsInsane;
    }
    *model = result;

    return eNmsSuccess;
} // fit

int
simplex(const std::function<double(const double*)>& objfunc,
        double* start,
        int n,
        double epsilon,
        double scale,
        int maxiter,
        const std::function<void(double*, int)>& constrain)
{
    std::vector<std::vector<double>> v(n + 1, std::vector<double>(n));
    std::vector<double> f(n + 1), vr(n), ve(n), vc(n), vm(n);

    const double pn = scale * (std::sqrt(n + 1.) - 1. + n) / (n * std::sqrt(2.));
    const double qn = scale * (std::sqrt(n + 1.) - 1.) / (n * std::sqrt(2.));
    for (int i = 0; i < n; ++i) {
        v[0][i] = start[i];
    }
    for (int i = 1; i <= n; ++i) {
        for (int j = 0; j < n; ++j) {
            v[i][j] = ((i - 1) == j) ? pn + start[j] : qn + start[j];
        }
    }
    // The original constrains only the last initial vertex: its loop index is n there.
    if (constrain) {
        constrain(v[n].data(), n);
    }
    for (int j = 0; j <= n; ++j) {
        f[j] = objfunc(v[j].data());
    }

    int itr;
    for (itr = 1; itr <= maxiter; ++itr) {
        int vg = 0;
        for (int j = 0; j <= n; ++j) {
            if (f[j] > f[vg]) {
                vg = j;
            }
        }
        int vs = 0;
        for (int j = 0; j <= n; ++j) {
            if (f[j] < f[vs]) {
                vs = j;
            }
        }
        int vh = vs;
        for (int j = 0; j <= n; ++j) {
            if ((f[j] > f[vh]) && (f[j] < f[vg])) {
                vh = j;
            }
        }
        for (int j = 0; j <= n - 1; ++j) {
            double cent = 0.;
            for (int m = 0; m <= n; ++m) {
                if (m != vg) {
                    cent += v[m][j];
                }
            }
            vm[j] = cent / n;
        }
        for (int j = 0; j <= n - 1; ++j) {
            vr[j] = vm[j] + kNmsAlpha * (vm[j] - v[vg][j]);
        }
        if (constrain) {
            constrain(vr.data(), n);
        }
        const double fr = objfunc(vr.data());
        if ((fr < f[vh]) && (fr >= f[vs])) {
            v[vg] = vr;
            f[vg] = fr;
        }
        if (fr < f[vs]) {
            for (int j = 0; j <= n - 1; ++j) {
                ve[j] = vm[j] + kNmsGamma * (vr[j] - vm[j]);
            }
            if (constrain) {
                constrain(ve.data(), n);
            }
            const double fe = objfunc(ve.data());
            if (fe < fr) {
                v[vg] = ve;
                f[vg] = fe;
            } else {
                v[vg] = vr;
                f[vg] = fr;
            }
        }
        if (fr >= f[vh]) {
            if ((fr < f[vg]) && (fr >= f[vh])) {
                for (int j = 0; j <= n - 1; ++j) {
                    vc[j] = vm[j] + kNmsBeta * (vr[j] - vm[j]);
                }
            } else {
                for (int j = 0; j <= n - 1; ++j) {
                    vc[j] = vm[j] - kNmsBeta * (vm[j] - v[vg][j]);
                }
            }
            if (constrain) {
                constrain(vc.data(), n);
            }
            const double fc = objfunc(vc.data());
            if (fc < f[vg]) {
                v[vg] = vc;
                f[vg] = fc;
            } else {
                for (int row = 0; row <= n; ++row) {
                    if (row != vs) {
                        for (int j = 0; j <= n - 1; ++j) {
                            v[row][j] = v[vs][j] + (v[row][j] - v[vs][j]) / 2.0;
                        }
                    }
                }
                if (constrain) {
                    constrain(v[vg].data(), n);
                }
                f[vg] = objfunc(v[vg].data());
                if (constrain) {
                    constrain(v[vh].data(), n);
                }
                f[vh] = objfunc(v[vh].data());
            }
        }

        double fsum = 0.;
        for (int j = 0; j <= n; ++j) {
            fsum += f[j];
        }
        const double favg = fsum / (n + 1);
        double s = 0.;
        for (int j = 0; j <= n; ++j) {
            s += std::pow(f[j] - favg, 2.0) / n;
        }
        s = std::sqrt(s);
        if (s < epsilon) {
            break;
        }
    }

    int vs = 0;
    for (int j = 0; j <= n; ++j) {
        if (f[j] < f[vs]) {
            vs = j;
        }
    }
    for (int j = 0; j < n; ++j) {
        start[j] = v[vs][j];
    }

    return itr;
} // simplex

} // namespace Ashift

NATRON_NAMESPACE_EXIT
