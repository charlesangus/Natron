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

#include "Resampler.h"

#include <algorithm>
#include <cmath>

NATRON_NAMESPACE_ENTER

namespace Resampler {
using TransformMath::Mat3;
using TransformMath::Point3;

const std::vector<ChoiceOption>&
filterOptions()
{
    static const std::vector<ChoiceOption> options = {
        { "Impulse", "(nearest neighbor / box) Use original values.", "impulse" },
        { "Box", "Integrate the source image over the bounding box of the back-transformed pixel.", "box" },
        { "Bilinear", "(tent / triangle) Bilinear interpolation between original values.", "bilinear" },
        { "Cubic", "(cubic spline) Some smoothing.", "cubic" },
        { "Keys", "(Catmull-Rom / Hermite spline) Some smoothing, plus minor sharpening (*).", "keys" },
        { "Simon", "Some smoothing, plus medium sharpening (*).", "simon" },
        { "Rifman", "Some smoothing, plus significant sharpening (*).", "rifman" },
        { "Mitchell", "Some smoothing, plus blurring to hide pixelation (*)(+).", "mitchell" },
        { "Parzen", "(cubic B-spline) Greatest smoothing of all filters (+).", "parzen" },
        { "Notch", "Flat smoothing (which tends to hide moire' patterns) (+).", "notch" },
    };

    return options;
}

const std::vector<ChoiceOption>&
shutterOffsetOptions()
{
    static const std::vector<ChoiceOption> options = {
        { "Centered", "Centers the shutter around the frame (from t-shutter/2 to t+shutter/2)", "centered" },
        { "Start", "Open the shutter at the frame (from t to t+shutter)", "start" },
        { "End", "Close the shutter at the frame (from t-shutter to t)", "end" },
        { "Custom", "Open the shutter at t+shuttercustomoffset (from t+shuttercustomoffset to t+shuttercustomoffset+shutter)", "custom" },
    };

    return options;
}

int
filterTaps(FilterEnum filter)
{
    switch (filter) {
    case eFilterImpulse:
    case eFilterBox:
        return 1;
    case eFilterBilinear:
    case eFilterCubic:
        return 2;
    default:
        return 4;
    }
}

bool
filterHonoursClamp(FilterEnum filter)
{
    return filter == eFilterKeys || filter == eFilterSimon || filter == eFilterRifman || filter == eFilterMitchell;
}

double
filterRoIExpansion(FilterEnum filter)
{
    switch (filter) {
    case eFilterImpulse:
    case eFilterBox:
        return 0.;
    case eFilterBilinear:
    case eFilterCubic:
        return 0.5;
    default:
        return 1.5;
    }
}

double
filter1D(FilterEnum filter,
         double Ip,
         double Ic,
         double In,
         double Ia,
         double d,
         bool clamp)
{
    clamp = clamp && filterHonoursClamp(filter);
    switch (filter) {
    case eFilterImpulse:
    case eFilterBox:
        return d < 0.5 ? Ic : In;
    case eFilterBilinear:
        return filterLinear(Ic, In, d);
    case eFilterCubic:
        return filterCubic(Ic, In, d, false);
    case eFilterKeys:
        return filterKeys(Ip, Ic, In, Ia, d, clamp);
    case eFilterSimon:
        return filterSimon(Ip, Ic, In, Ia, d, clamp);
    case eFilterRifman:
        return filterRifman(Ip, Ic, In, Ia, d, clamp);
    case eFilterMitchell:
        return filterMitchell(Ip, Ic, In, Ia, d, clamp);
    case eFilterParzen:
        return filterParzen(Ip, Ic, In, Ia, d);
    case eFilterNotch:
        return filterNotch(Ip, Ic, In, Ia, d);
    }

    return Ic;
}

void
filterWeights(FilterEnum filter,
              double d,
              double w[4])
{
    for (int k = 0; k < 4; ++k) {
        double e[4] = { 0., 0., 0., 0. };
        e[k] = 1.;
        w[k] = filter1D(filter, e[0], e[1], e[2], e[3], d, false);
    }
}

namespace {
    template <FilterEnum filter, bool clamp>
    inline double
    cubic2D(double Ipp,
            double Icp,
            double Inp,
            double Iap,
            double Ipc,
            double Icc,
            double Inc,
            double Iac,
            double Ipn,
            double Icn,
            double Inn,
            double Ian,
            double Ipa,
            double Ica,
            double Ina,
            double Iaa,
            double dx,
            double dy)
    {
        switch (filter) {
        case eFilterKeys:
            return filterKeys(filterKeys(Ipp, Icp, Inp, Iap, dx, clamp), filterKeys(Ipc, Icc, Inc, Iac, dx, clamp), filterKeys(Ipn, Icn, Inn, Ian, dx, clamp), filterKeys(Ipa, Ica, Ina, Iaa, dx, clamp), dy, clamp);
        case eFilterSimon:
            return filterSimon(filterSimon(Ipp, Icp, Inp, Iap, dx, clamp), filterSimon(Ipc, Icc, Inc, Iac, dx, clamp), filterSimon(Ipn, Icn, Inn, Ian, dx, clamp), filterSimon(Ipa, Ica, Ina, Iaa, dx, clamp), dy, clamp);
        case eFilterRifman:
            return filterRifman(filterRifman(Ipp, Icp, Inp, Iap, dx, clamp), filterRifman(Ipc, Icc, Inc, Iac, dx, clamp), filterRifman(Ipn, Icn, Inn, Ian, dx, clamp), filterRifman(Ipa, Ica, Ina, Iaa, dx, clamp), dy, clamp);
        case eFilterMitchell:
            return filterMitchell(filterMitchell(Ipp, Icp, Inp, Iap, dx, clamp), filterMitchell(Ipc, Icc, Inc, Iac, dx, clamp), filterMitchell(Ipn, Icn, Inn, Ian, dx, clamp), filterMitchell(Ipa, Ica, Ina, Iaa, dx, clamp), dy, clamp);
        case eFilterParzen:
            return filterParzen(filterParzen(Ipp, Icp, Inp, Iap, dx), filterParzen(Ipc, Icc, Inc, Iac, dx), filterParzen(Ipn, Icn, Inn, Ian, dx), filterParzen(Ipa, Ica, Ina, Iaa, dx), dy);
        case eFilterNotch:
            return filterNotch(filterNotch(Ipp, Icp, Inp, Iap, dx), filterNotch(Ipc, Icc, Inc, Iac, dx), filterNotch(Ipn, Icn, Inn, Ian, dx), filterNotch(Ipa, Ica, Ina, Iaa, dx), dy);
        default:
            return 0.;
        }
    }

    inline double
    comp(const float* p,
         int c)
    {
        return p ? (double)p[c] : 0.;
    }

    inline int
    clampCoord(int v,
               int lo,
               int hiExclusive)
    {
        return (std::max)(lo, (std::min)(v, hiExclusive - 1));
    }

    template <int N, FilterEnum filter, bool clamp>
    bool
    interpolateT(double fx,
                 double fy,
                 const SourceImage& src,
                 bool blackOutside,
                 float* tmpPix)
    {
        if (!src.isValid()) {
            for (int c = 0; c < N; ++c) {
                tmpPix[c] = 0;
            }
            return false;
        }
        const RectI& b = src.bounds;
        bool inside = true;

        switch (filter) {
        case eFilterImpulse:
        case eFilterBox: {
            int mx = (int)std::floor(fx);
            int my = (int)std::floor(fy);
            if (!blackOutside) {
                mx = clampCoord(mx, b.x1, b.x2);
                my = clampCoord(my, b.y1, b.y2);
            }
            const float* Pmm = src.pixel(mx, my);
            if (Pmm) {
                for (int c = 0; c < N; ++c) {
                    tmpPix[c] = Pmm[c];
                }
            } else {
                for (int c = 0; c < N; ++c) {
                    tmpPix[c] = 0;
                }
                inside = false;
            }
            break;
        }
        case eFilterBilinear:
        case eFilterCubic: {
            int cx = (int)std::floor(fx - 0.5);
            int cy = (int)std::floor(fy - 0.5);
            int nx = cx + 1;
            int ny = cy + 1;
            if (!blackOutside) {
                cx = clampCoord(cx, b.x1, b.x2);
                cy = clampCoord(cy, b.y1, b.y2);
                nx = clampCoord(nx, b.x1, b.x2);
                ny = clampCoord(ny, b.y1, b.y2);
            }
            const double dx = (std::max)(0., (std::min)(fx - 0.5 - cx, 1.));
            const double dy = (std::max)(0., (std::min)(fy - 0.5 - cy, 1.));

            const float* Pcc = src.pixel(cx, cy);
            const float* Pnc = src.pixel(nx, cy);
            const float* Pcn = src.pixel(cx, ny);
            const float* Pnn = src.pixel(nx, ny);
            if (Pcc || Pnc || Pcn || Pnn) {
                for (int c = 0; c < N; ++c) {
                    const double Icc = comp(Pcc, c);
                    const double Inc = comp(Pnc, c);
                    const double Icn = comp(Pcn, c);
                    const double Inn = comp(Pnn, c);
                    if (filter == eFilterBilinear) {
                        double Ic = filterLinear(Icc, Inc, dx);
                        double In = filterLinear(Icn, Inn, dx);
                        tmpPix[c] = (float)filterLinear(Ic, In, dy);
                    } else {
                        double Ic = filterCubic(Icc, Inc, dx, clamp);
                        double In = filterCubic(Icn, Inn, dx, clamp);
                        tmpPix[c] = (float)filterCubic(Ic, In, dy, clamp);
                    }
                }
            } else {
                for (int c = 0; c < N; ++c) {
                    tmpPix[c] = 0;
                }
                inside = false;
            }
            break;
        }
        default: {
            int cx = (int)std::floor(fx - 0.5);
            int cy = (int)std::floor(fy - 0.5);
            int px = cx - 1;
            int py = cy - 1;
            int nx = cx + 1;
            int ny = cy + 1;
            int ax = cx + 2;
            int ay = cy + 2;
            if (!blackOutside) {
                cx = clampCoord(cx, b.x1, b.x2);
                cy = clampCoord(cy, b.y1, b.y2);
                px = clampCoord(px, b.x1, b.x2);
                py = clampCoord(py, b.y1, b.y2);
                nx = clampCoord(nx, b.x1, b.x2);
                ny = clampCoord(ny, b.y1, b.y2);
                ax = clampCoord(ax, b.x1, b.x2);
                ay = clampCoord(ay, b.y1, b.y2);
            }
            const double dx = (std::max)(0., (std::min)(fx - 0.5 - cx, 1.));
            const double dy = (std::max)(0., (std::min)(fy - 0.5 - cy, 1.));

            const float* Ppp = src.pixel(px, py);
            const float* Pcp = src.pixel(cx, py);
            const float* Pnp = src.pixel(nx, py);
            const float* Pap = src.pixel(ax, py);
            const float* Ppc = src.pixel(px, cy);
            const float* Pcc = src.pixel(cx, cy);
            const float* Pnc = src.pixel(nx, cy);
            const float* Pac = src.pixel(ax, cy);
            const float* Ppn = src.pixel(px, ny);
            const float* Pcn = src.pixel(cx, ny);
            const float* Pnn = src.pixel(nx, ny);
            const float* Pan = src.pixel(ax, ny);
            const float* Ppa = src.pixel(px, ay);
            const float* Pca = src.pixel(cx, ay);
            const float* Pna = src.pixel(nx, ay);
            const float* Paa = src.pixel(ax, ay);
            if (Ppp || Pcp || Pnp || Pap || Ppc || Pcc || Pnc || Pac || Ppn || Pcn || Pnn || Pan || Ppa || Pca || Pna || Paa) {
                for (int c = 0; c < N; ++c) {
                    double I = cubic2D<filter, clamp>(comp(Ppp, c), comp(Pcp, c), comp(Pnp, c), comp(Pap, c),
                                                      comp(Ppc, c), comp(Pcc, c), comp(Pnc, c), comp(Pac, c),
                                                      comp(Ppn, c), comp(Pcn, c), comp(Pnn, c), comp(Pan, c),
                                                      comp(Ppa, c), comp(Pca, c), comp(Pna, c), comp(Paa, c),
                                                      dx, dy);
                    tmpPix[c] = (float)I;
                }
            } else {
                for (int c = 0; c < N; ++c) {
                    tmpPix[c] = 0;
                }
                inside = false;
            }
            break;
        }
        }

        return inside;
    }

    /*
     * Box-filter integration over a piecewise-constant signal: x = 0 is the left edge of the first
     * sample. Outside the data is zero (zeroOutside) or the nearest sample.
     */
    void
    integrate1d(const float* l,
                const size_t nsamples,
                const size_t stride,
                const size_t depth,
                const double x1,
                const double x2,
                const bool zeroOutside,
                float* v)
    {
        size_t ifirst, ilast;
        double fracfirst, fraclast;

        if (x1 < 0.) {
            ifirst = 0;
            fracfirst = 0.;
        } else if (nsamples <= x1) {
            ifirst = nsamples - 1;
            fracfirst = 0.;
        } else {
            ifirst = (size_t)std::floor(x1);
            fracfirst = x1 - ifirst;
        }
        if (x2 < 0.) {
            ilast = 0;
            fraclast = 0.;
        } else if (nsamples <= x2) {
            ilast = nsamples - 1;
            fraclast = 0.;
        } else {
            ilast = (size_t)std::floor(x2);
            fraclast = ilast + 1 - x2;
        }
        if ((x1 < ifirst) && !zeroOutside) {
            for (size_t j = 0; j < depth; ++j) {
                v[j] += l[ifirst * stride + j] * (float)(ifirst - x1);
            }
        }
        if (fracfirst > 0.) {
            for (size_t j = 0; j < depth; ++j) {
                v[j] -= l[ifirst * stride + j] * (float)fracfirst;
            }
        }
        for (size_t i = ifirst; i <= ilast; ++i) {
            for (size_t j = 0; j < depth; ++j) {
                v[j] += l[i * stride + j];
            }
        }
        if (fraclast > 0.) {
            for (size_t j = 0; j < depth; ++j) {
                v[j] -= l[ilast * stride + j] * (float)fraclast;
            }
        }
        if ((x2 > nsamples) && !zeroOutside) {
            for (size_t j = 0; j < depth; ++j) {
                v[j] += l[ilast * stride + j] * (float)(x2 - nsamples);
            }
        }
    }

    void
    integrate2d(const float* a,
                const size_t awidth,
                const size_t aheight,
                const size_t axstride,
                const size_t aystride,
                const size_t depth,
                double x1,
                double y1,
                double x2,
                double y2,
                const bool zeroOutside,
                float* p,
                float* v)
    {
        size_t ifirst, ilast;
        double fracfirst, fraclast;

        if (y1 < 0.) {
            ifirst = 0;
            fracfirst = 0.;
        } else if (aheight <= y1) {
            ifirst = aheight - 1;
            fracfirst = 0.;
        } else {
            ifirst = (size_t)std::floor(y1);
            fracfirst = y1 - ifirst;
        }
        if (y2 < 0.) {
            ilast = 0;
            fraclast = 0.;
        } else if (aheight <= y2) {
            ilast = aheight - 1;
            fraclast = 0.;
        } else {
            ilast = (size_t)std::floor(y2);
            fraclast = ilast + 1 - y2;
        }

        for (size_t j = 0; j < depth; ++j) {
            p[j] = 0.;
        }
        integrate1d(&a[ifirst * aystride], awidth, axstride, depth, x1, x2, zeroOutside, p);
        if ((y1 < ifirst) && !zeroOutside) {
            for (size_t j = 0; j < depth; ++j) {
                v[j] += p[j] * (float)(ifirst - y1);
            }
        }
        if (fracfirst > 0.) {
            for (size_t j = 0; j < depth; ++j) {
                v[j] -= p[j] * (float)fracfirst;
            }
        }
        for (size_t j = 0; j < depth; ++j) {
            v[j] += p[j];
        }
        for (size_t i = ifirst + 1; i < ilast; ++i) {
            integrate1d(&a[i * aystride], awidth, axstride, depth, x1, x2, zeroOutside, v);
        }
        if (ilast > ifirst) {
            for (size_t j = 0; j < depth; ++j) {
                p[j] = 0.;
            }
            integrate1d(&a[ilast * aystride], awidth, axstride, depth, x1, x2, zeroOutside, p);
            for (size_t j = 0; j < depth; ++j) {
                v[j] += p[j];
            }
        }
        if (fraclast > 0.) {
            for (size_t j = 0; j < depth; ++j) {
                v[j] -= p[j] * (float)fraclast;
            }
        }
        if ((y2 > aheight) && !zeroOutside) {
            for (size_t j = 0; j < depth; ++j) {
                v[j] += p[j] * (float)(y2 - aheight);
            }
        }
    }

    inline bool
    outsideBounds(double x,
                  double y,
                  const RectI& bounds)
    {
        return x < bounds.x1 || bounds.x2 <= x || y < bounds.y1 || bounds.y2 <= y;
    }

    inline int
    pow3(int exp)
    {
        int base = 3;
        int result = 1;

        while (exp) {
            if (exp & 1) {
                result *= base;
            }
            exp >>= 1;
            base *= base;
        }

        return result;
    }

    /*
     * Averages 3^isx x 3^isy bilinear supersamples spread over the back-transformed pixel; tmpPix
     * holds the centre sample on entry, which counts as one of them.
     */
    template <int N>
    void
    supersample(double fx,
                double fy,
                double Jxx,
                double Jxy,
                double Jyx,
                double Jyy,
                int isx,
                int isy,
                const SourceImage& src,
                bool blackOutside,
                float* tmpPix)
    {
        const int nisx = pow3(isx);
        const int nisy = pow3(isy);

        for (int y = -nisy / 2; y <= nisy / 2; ++y) {
            for (int x = -nisx / 2; x <= nisx / 2; ++x) {
                if ((x != 0) || (y != 0)) {
                    double sfx = fx + (Jxx * x) / nisx + (Jxy * y) / nisy;
                    double sfy = fy + (Jyx * x) / nisx + (Jyy * y) / nisy;
                    float tmp[N];
                    interpolateT<N, eFilterBilinear, false>(sfx, sfy, src, blackOutside, tmp);
                    for (int c = 0; c < N; ++c) {
                        tmpPix[c] += tmp[c];
                    }
                }
            }
        }

        const int insamples = nisx * nisy;
        for (int c = 0; c < N; ++c) {
            tmpPix[c] /= insamples;
        }
    }

    template <int N, FilterEnum filter, bool clamp>
    void
    interpolateSuperT(double fx,
                      double fy,
                      double Jxx,
                      double Jxy,
                      double Jyx,
                      double Jyy,
                      const SourceImage& src,
                      bool blackOutside,
                      float* tmpPix)
    {
        if (!src.isValid()) {
            for (int c = 0; c < N; ++c) {
                tmpPix[c] = 0.;
            }
            return;
        }
        if ((Jxx == 0.) && (Jxy == 0.) && (Jyx == 0.) && (Jyy == 0.)) {
            interpolateT<N, filter, clamp>(fx, fy, src, blackOutside, tmpPix);
            return;
        }
        if (filter == eFilterBox) {
            for (int c = 0; c < N; ++c) {
                tmpPix[c] = 0.;
            }
            double x, y;
            double x1, x2, y1, y2;
            x1 = x2 = fx - Jxx * 0.5 - Jxy * 0.5;
            y1 = y2 = fy - Jyx * 0.5 - Jyy * 0.5;
            x = fx + Jxx * 0.5 - Jxy * 0.5;
            y = fy + Jyx * 0.5 - Jyy * 0.5;
            x1 = (std::min)(x1, x);
            y1 = (std::min)(y1, y);
            x2 = (std::max)(x2, x);
            y2 = (std::max)(y2, y);
            x = fx - Jxx * 0.5 + Jxy * 0.5;
            y = fy - Jyx * 0.5 + Jyy * 0.5;
            x1 = (std::min)(x1, x);
            y1 = (std::min)(y1, y);
            x2 = (std::max)(x2, x);
            y2 = (std::max)(y2, y);
            x = fx + Jxx * 0.5 + Jxy * 0.5;
            y = fy + Jyx * 0.5 + Jyy * 0.5;
            x1 = (std::min)(x1, x);
            y1 = (std::min)(y1, y);
            x2 = (std::max)(x2, x);
            y2 = (std::max)(y2, y);
            if ((x2 <= x1) || (y2 <= y1)) {
                interpolateT<N, filter, clamp>(fx, fy, src, blackOutside, tmpPix);
                return;
            }

            const RectI& b = src.bounds;
            const size_t awidth = b.x2 - b.x1;
            const size_t aheight = b.y2 - b.y1;
            x1 -= b.x1;
            y1 -= b.y1;
            x2 -= b.x1;
            y2 -= b.y1;
            float p[N];
            integrate2d(src.data, awidth, aheight, (size_t)src.nComps, src.rowStride, N, x1, y1, x2, y2, blackOutside, p, tmpPix);
            float s = (float)((x2 - x1) * (y2 - y1));
            if (s != 0.f) {
                for (int c = 0; c < N; ++c) {
                    tmpPix[c] /= s;
                }
            }
            return;
        }

        bool inside = interpolateT<N, filter, clamp>(fx, fy, src, blackOutside, tmpPix);
        if (!inside) {
            // A black centre is only kept when the whole back-transformed pixel is outside.
            const RectI& bounds = src.bounds;
            if (outsideBounds(fx - Jxx * 0.5 - Jxy * 0.5, fy - Jyx * 0.5 - Jyy * 0.5, bounds) && outsideBounds(fx + Jxx * 0.5 - Jxy * 0.5, fy + Jyx * 0.5 - Jyy * 0.5, bounds) && outsideBounds(fx - Jxx * 0.5 + Jxy * 0.5, fy - Jyx * 0.5 + Jyy * 0.5, bounds) && outsideBounds(fx + Jxx * 0.5 + Jxy * 0.5, fy + Jyx * 0.5 + Jyy * 0.5, bounds)) {
                return;
            }
        }

        int isx, isy;
        if (!supersampleLevels(Jxx, Jxy, Jyx, Jyy, &isx, &isy)) {
            return;
        }
        supersample<N>(fx, fy, Jxx, Jxy, Jyx, Jyy, isx, isy, src, blackOutside, tmpPix);
    }

    /*
     * One sample of the destination pixel (x, y) through the inverse pixel matrix H: the
     * Transform3x3 processor's per-sample body.
     */
    template <int N, FilterEnum filter, bool clamp>
    inline void
    samplePixel(const Mat3& H,
                int x,
                int y,
                const SourceImage& src,
                bool srcValid,
                bool blackOutside,
                float* tmpPix)
    {
        Point3 canonicalCoords((double)x + 0.5, (double)y + 0.5, 1.);
        Point3 transformed = H * canonicalCoords;

        if (!srcValid || (transformed.z <= 0.)) {
            for (int c = 0; c < N; ++c) {
                tmpPix[c] = 0;
            }
            return;
        }
        double fx = transformed.z != 0 ? transformed.x / transformed.z : transformed.x;
        double fy = transformed.z != 0 ? transformed.y / transformed.z : transformed.y;
        if (filter == eFilterImpulse) {
            interpolateT<N, filter, clamp>(fx, fy, src, blackOutside, tmpPix);
            return;
        }
        const int x1 = src.bounds.x1;
        const int x2 = src.bounds.x2;
        const int y1 = src.bounds.y1;
        const int y2 = src.bounds.y2;
        bool xinside = (x1 <= fx + 0.5 && fx - 0.5 < x2);
        bool yinside = (y1 <= fy + 0.5 && fy - 0.5 < y2);
        if (blackOutside && !(xinside && yinside)) {
            xinside = yinside = false;
        }
        const double z2 = transformed.z * transformed.z;
        double Jxx = xinside ? (H(0, 0) * transformed.z - transformed.x * H(2, 0)) / z2 : 0.;
        double Jxy = xinside ? (H(0, 1) * transformed.z - transformed.x * H(2, 1)) / z2 : 0.;
        double Jyx = yinside ? (H(1, 0) * transformed.z - transformed.y * H(2, 0)) / z2 : 0.;
        double Jyy = yinside ? (H(1, 1) * transformed.z - transformed.y * H(2, 1)) / z2 : 0.;
        interpolateSuperT<N, filter, clamp>(fx, fy, Jxx, Jxy, Jyx, Jyy, src, blackOutside, tmpPix);
    }

    /*
     * samplePixel() for a matrix whose last row is exactly (0, 0, 1). Then z is exactly 1, so
     * fx, fy and the Jacobian come out bit for bit as samplePixel() computes them, while only
     * the column products that vary along the row are recomputed per pixel. rowX and rowY are
     * H(0, 1) * py and H(1, 1) * py for the row's pixel-centre ordinate py.
     */
    template <int N, FilterEnum filter, bool clamp>
    inline void
    sampleAffinePixel(const Mat3& H,
                      double rowX,
                      double rowY,
                      int x,
                      const SourceImage& src,
                      bool blackOutside,
                      float* tmpPix)
    {
        const double px = (double)x + 0.5;
        const double fx = H(0, 0) * px + rowX + H(0, 2);
        const double fy = H(1, 0) * px + rowY + H(1, 2);
        if (filter == eFilterImpulse) {
            interpolateT<N, filter, clamp>(fx, fy, src, blackOutside, tmpPix);
            return;
        }
        bool xinside = (src.bounds.x1 <= fx + 0.5 && fx - 0.5 < src.bounds.x2);
        bool yinside = (src.bounds.y1 <= fy + 0.5 && fy - 0.5 < src.bounds.y2);
        if (blackOutside && !(xinside && yinside)) {
            xinside = yinside = false;
        }
        const double Jxx = xinside ? H(0, 0) : 0.;
        const double Jxy = xinside ? H(0, 1) : 0.;
        const double Jyx = yinside ? H(1, 0) : 0.;
        const double Jyy = yinside ? H(1, 1) : 0.;
        interpolateSuperT<N, filter, clamp>(fx, fy, Jxx, Jxy, Jyx, Jyy, src, blackOutside, tmpPix);
    }

    inline bool
    isAffine(const Mat3& H)
    {
        if ((H(2, 0) != 0.) || (H(2, 1) != 0.) || (H(2, 2) != 1.)) {
            return false;
        }
        for (int k = 0; k < 6; ++k) {
            if (!std::isfinite(H.m[k])) {
                return false;
            }
        }
        return true;
    }

    template <int N, FilterEnum filter, bool clamp>
    void
    motionBlurPixelT(const ResampleParams& params,
                     const SourceImage& src,
                     int x,
                     int y,
                     float* out,
                     int* samplesTaken)
    {
        const bool srcValid = src.isValid();
        const double maxErr = motionBlurMaxError(params.motionBlur);
        const double maxErr2 = maxErr * maxErr;
        const int maxIt = motionBlurMaxIterations(params.motionBlur);
        const int minsamples = motionBlurMinSamples(params.motionBlur);
        const size_t count = params.count;
        float tmpPix[N];
        double acc = 0.;
        double accPix[N];
        double accPix2[N];
        double mean[N];
        double var[N];

        for (int c = 0; c < N; ++c) {
            accPix[c] = 0;
            accPix2[c] = 0;
            mean[c] = 0.;
            var[c] = 1.;
        }
        unsigned int seed = motionBlurHash(motionBlurHash(x + (unsigned int)(0x10000 * params.motionBlur)) + y);
        int sample = 0;
        int maxsamples = minsamples;
        while (sample < maxsamples) {
            for (; sample < maxsamples; ++sample, ++seed) {
                int t;
                if (sample < minsamples) {
                    t = (int)((sample + vanDerCorput2(seed)) * count / (double)minsamples);
                } else {
                    t = (int)(vanDerCorput2(seed) * count);
                }
                samplePixel<N, filter, clamp>(params.invTransforms[t], x, y, src, srcValid, params.blackOutside, tmpPix);
                if (!params.alphas) {
                    for (int c = 0; c < N; ++c) {
                        accPix[c] += tmpPix[c];
                        accPix2[c] += tmpPix[c] * tmpPix[c];
                    }
                } else {
                    acc += params.alphas[t];
                    for (int c = 0; c < N; ++c) {
                        accPix[c] += tmpPix[c] * params.alphas[t];
                        accPix2[c] += tmpPix[c] * tmpPix[c] * params.alphas[t];
                    }
                }
            }
            if (!params.alphas) {
                for (int c = 0; c < N; ++c) {
                    mean[c] = accPix[c] / sample;
                    if (sample <= 1) {
                        var[c] = 1.;
                    } else {
                        var[c] = (accPix2[c] - mean[c] * mean[c] * sample) / (sample - 1);
                        if (maxsamples < maxIt) {
                            maxsamples = (std::max)(maxsamples, (std::min)((int)(var[c] / maxErr2), maxIt));
                        }
                    }
                }
            } else if (acc > 0.) {
                for (int c = 0; c < N; ++c) {
                    mean[c] = accPix[c] / acc;
                    if (sample <= 1) {
                        var[c] = 1.;
                    } else {
                        var[c] = accPix2[c] / acc - mean[c] * mean[c];
                        if (maxsamples < maxIt) {
                            maxsamples = (std::max)(maxsamples, (std::min)((int)(var[c] / maxErr2), maxIt));
                        }
                    }
                }
            }
        }
        for (int c = 0; c < N; ++c) {
            out[c] = (float)mean[c];
        }
        if (samplesTaken) {
            *samplesTaken = sample;
        }
    }

    template <int N, FilterEnum filter, bool clamp>
    void
    resampleRowT(const ResampleParams& params,
                 const SourceImage& src,
                 int y,
                 int x1,
                 int x2,
                 float* dst)
    {
        const bool srcValid = src.isValid();

        if ((params.motionBlur == 0.) || (params.count <= 1)) {
            const Mat3& H = params.invTransforms[0];
            if (srcValid && isAffine(H)) {
                const double py = (double)y + 0.5;
                const double rowX = H(0, 1) * py;
                const double rowY = H(1, 1) * py;
                for (int x = x1; x < x2; ++x, dst += N) {
                    sampleAffinePixel<N, filter, clamp>(H, rowX, rowY, x, src, params.blackOutside, dst);
                }
                return;
            }
            for (int x = x1; x < x2; ++x, dst += N) {
                samplePixel<N, filter, clamp>(H, x, y, src, srcValid, params.blackOutside, dst);
            }
        } else {
            for (int x = x1; x < x2; ++x, dst += N) {
                motionBlurPixelT<N, filter, clamp>(params, src, x, y, dst, 0);
            }
        }
    }

    const double kCoordLimit = 1073741824.;

    /*
     * Wraps one coordinate into [lo, hi) per WrapMode, setting *moved when eWrapClamp changed
     * it. Returns false when the result is not finite (the pixel is outside); otherwise the
     * coordinate is also clamped to +-kCoordLimit so the samplers' int conversions stay defined.
     */
    inline bool
    wrapCoord(double* s,
              WrapMode mode,
              double lo,
              double hi,
              bool* moved)
    {
        double v = *s;
        const double w = hi - lo;

        *moved = false;
        if (w > 0.) {
            switch (mode) {
            case eWrapClamp:
                if (v < lo + 0.5) {
                    v = lo + 0.5;
                    *moved = true;
                } else if (v > hi - 0.5) {
                    v = hi - 0.5;
                    *moved = true;
                }
                break;
            case eWrapRepeat:
                if (std::isfinite(v)) {
                    v -= w * std::floor((v - lo) / w);
                }
                break;
            case eWrapMirror:
                if (std::isfinite(v)) {
                    const double r = (v - lo) - 2. * w * std::floor((v - lo) / (2. * w));
                    v = lo + ((r <= w) ? r : (2. * w - r));
                }
                break;
            case eWrapNone:
                break;
            }
        }
        if (!std::isfinite(v)) {
            return false;
        }
        *s = (std::max)(-kCoordLimit, (std::min)(v, kCoordLimit));
        return true;
    }

    inline double
    finiteOrZero(double v)
    {
        return std::isfinite(v) ? v : 0.;
    }

    inline float
    finiteFloatOrZero(double v)
    {
        const float f = (float)v;

        return std::isfinite(f) ? f : 0.f;
    }

    template <int N, FilterEnum filter, bool clamp>
    void
    resampleCoordsT(const SourceImage& src,
                    const float* sx,
                    const float* sy,
                    const float* jacobian,
                    int n,
                    bool blackOutside,
                    WrapMode wrapU,
                    WrapMode wrapV,
                    const RectI& domain,
                    float* out,
                    int nComps)
    {
        const RectI& b = src.bounds;
        float tmpPix[N];

        for (int i = 0; i < n; ++i, out += nComps) {
            double fx = sx[i];
            double fy = sy[i];
            bool movedX = false;
            bool movedY = false;
            if (!wrapCoord(&fx, wrapU, domain.x1, domain.x2, &movedX) || !wrapCoord(&fy, wrapV, domain.y1, domain.y2, &movedY)) {
                for (int c = 0; c < nComps; ++c) {
                    out[c] = 0.f;
                }
                continue;
            }
            if ((filter == eFilterImpulse) || !jacobian) {
                interpolateT<N, filter, clamp>(fx, fy, src, blackOutside, tmpPix);
            } else {
                const float* J = jacobian + (std::size_t)i * 4;
                double Jxx = finiteOrZero(J[0]);
                double Jxy = finiteOrZero(J[1]);
                double Jyx = finiteOrZero(J[2]);
                double Jyy = finiteOrZero(J[3]);
                bool xinside = (b.x1 <= fx + 0.5 && fx - 0.5 < b.x2);
                bool yinside = (b.y1 <= fy + 0.5 && fy - 0.5 < b.y2);
                if (blackOutside && !(xinside && yinside)) {
                    xinside = yinside = false;
                }
                if (!xinside || movedX) {
                    Jxx = Jxy = 0.;
                }
                if (!yinside || movedY) {
                    Jyx = Jyy = 0.;
                }
                interpolateSuperT<N, filter, clamp>(fx, fy, Jxx, Jxy, Jyx, Jyy, src, blackOutside, tmpPix);
            }
            const int nc = (std::min)(N, nComps);
            for (int c = 0; c < nc; ++c) {
                out[c] = tmpPix[c];
            }
            for (int c = nc; c < nComps; ++c) {
                out[c] = 0.f;
            }
        }
    }

    typedef bool (*InterpolateFn)(double, double, const SourceImage&, bool, float*);
    typedef void (*InterpolateSuperFn)(double, double, double, double, double, double, const SourceImage&, bool, float*);
    typedef void (*RowFn)(const ResampleParams&, const SourceImage&, int, int, int, float*);
    typedef void (*MotionBlurPixelFn)(const ResampleParams&, const SourceImage&, int, int, float*, int*);
    typedef void (*CoordsFn)(const SourceImage&, const float*, const float*, const float*, int, bool, WrapMode, WrapMode, const RectI&, float*, int);

    struct KernelTable {
        InterpolateFn interpolate;
        InterpolateSuperFn interpolateSuper;
        RowFn row;
        MotionBlurPixelFn motionBlurPixel;
        CoordsFn coords;
    };

    template <int N, FilterEnum filter, bool clamp>
    KernelTable
    makeTable()
    {
        KernelTable t;

        t.interpolate = &interpolateT<N, filter, clamp>;
        t.interpolateSuper = &interpolateSuperT<N, filter, clamp>;
        t.row = &resampleRowT<N, filter, clamp>;
        t.motionBlurPixel = &motionBlurPixelT<N, filter, clamp>;
        t.coords = &resampleCoordsT<N, filter, clamp>;

        return t;
    }

    template <int N>
    KernelTable
    tableForFilter(FilterEnum filter,
                   bool clamp)
    {
        switch (filter) {
        case eFilterImpulse:
            return makeTable<N, eFilterImpulse, false>();
        case eFilterBox:
            return makeTable<N, eFilterBox, false>();
        case eFilterBilinear:
            return makeTable<N, eFilterBilinear, false>();
        case eFilterCubic:
            return makeTable<N, eFilterCubic, false>();
        case eFilterKeys:
            return clamp ? makeTable<N, eFilterKeys, true>() : makeTable<N, eFilterKeys, false>();
        case eFilterSimon:
            return clamp ? makeTable<N, eFilterSimon, true>() : makeTable<N, eFilterSimon, false>();
        case eFilterRifman:
            return clamp ? makeTable<N, eFilterRifman, true>() : makeTable<N, eFilterRifman, false>();
        case eFilterMitchell:
            return clamp ? makeTable<N, eFilterMitchell, true>() : makeTable<N, eFilterMitchell, false>();
        case eFilterParzen:
            return makeTable<N, eFilterParzen, false>();
        case eFilterNotch:
            return makeTable<N, eFilterNotch, false>();
        }

        return makeTable<N, eFilterCubic, false>();
    }

    KernelTable
    selectKernels(int nComps,
                  FilterEnum filter,
                  bool clamp)
    {
        switch (nComps) {
        case 1:
            return tableForFilter<1>(filter, clamp);
        case 2:
            return tableForFilter<2>(filter, clamp);
        case 3:
            return tableForFilter<3>(filter, clamp);
        default:
            return tableForFilter<4>(filter, clamp);
        }
    }

    inline void
    zeroPixels(float* out,
               int n)
    {
        for (int c = 0; c < n; ++c) {
            out[c] = 0.f;
        }
    }
} // anon namespace

bool
interpolate(FilterEnum filter,
            bool clamp,
            double fx,
            double fy,
            const SourceImage& src,
            bool blackOutside,
            float* out)
{
    if (!src.isValid()) {
        zeroPixels(out, src.nComps);
        return false;
    }
    return selectKernels(src.nComps, filter, clamp).interpolate(fx, fy, src, blackOutside, out);
}

bool
supersampleLevels(double Jxx,
                  double Jxy,
                  double Jyx,
                  double Jyy,
                  int* levelX,
                  int* levelY)
{
    *levelX = 0;
    *levelY = 0;
    double dx = Jxx * Jxx + Jyx * Jyx;
    double dy = Jxy * Jxy + Jyy * Jyy;
    if ((dx <= 1.) && (dy <= 1.)) {
        return false;
    }
    double sx = (dx <= 1.) ? 0. : (std::min)(std::log(dx) / (2 * std::log(3.)), 4.);
    double sy = (dy <= 1.) ? 0. : (std::min)(std::log(dy) / (2 * std::log(3.)), 4.);
    // Rounding rather than ceil puts the jump to the next level at a scale of sqrt(3), so a
    // scale just above 1 is not supersampled.
    *levelX = (int)std::ceil(sx - 0.5);
    *levelY = (int)std::ceil(sy - 0.5);

    return *levelX > 0 || *levelY > 0;
}

void
interpolateSuper(FilterEnum filter,
                 bool clamp,
                 double fx,
                 double fy,
                 double Jxx,
                 double Jxy,
                 double Jyx,
                 double Jyy,
                 const SourceImage& src,
                 bool blackOutside,
                 float* out)
{
    if (!src.isValid()) {
        zeroPixels(out, src.nComps);
        return;
    }
    selectKernels(src.nComps, filter, clamp).interpolateSuper(fx, fy, Jxx, Jxy, Jyx, Jyy, src, blackOutside, out);
}

double
vanDerCorput2(unsigned int seed)
{
    const int base = 2;
    double base_inv = 1.0 / ((double)base);
    double r = 0.0;

    while (seed != 0) {
        int digit = seed % base;
        r = r + ((double)digit) * base_inv;
        base_inv = base_inv / ((double)base);
        seed = seed / base;
    }

    return r;
}

unsigned int
motionBlurHash(unsigned int a)
{
    a = (a ^ 61) ^ (a >> 16);
    a = a + (a << 3);
    a = a ^ (a >> 4);
    a = a * 0x27d4eb2d;
    a = a ^ (a >> 15);

    return a;
}

void
shutterRange(double time,
             double shutter,
             ShutterOffsetEnum offset,
             double customOffset,
             double* tMin,
             double* tMax)
{
    switch (offset) {
    case eShutterOffsetCentered:
        *tMin = time - shutter / 2;
        *tMax = time + shutter / 2;
        break;
    case eShutterOffsetStart:
        *tMin = time;
        *tMax = time + shutter;
        break;
    case eShutterOffsetEnd:
        *tMin = time - shutter;
        *tMax = time;
        break;
    case eShutterOffsetCustom:
        *tMin = time + customOffset;
        *tMax = time + customOffset + shutter;
        break;
    default:
        *tMin = time;
        *tMax = time;
        break;
    }
}

namespace {
    // Samples nothing: every destination pixel maps to z = 0 and reads as black.
    inline Mat3
    nullSamplingMatrix()
    {
        return Mat3(0., 0., 0., 0., 0., 0., 0., 0., 1.);
    }
} // anon namespace

void
buildSamplingTransforms(const CanonicalTransformFn& fn,
                        double time,
                        bool invert,
                        const BlurSettings& blur,
                        double sx,
                        double sy,
                        bool fielded,
                        double srcPar,
                        double dstPar,
                        SamplingTransforms* out)
{
    out->invTransforms.clear();
    out->alphas.clear();
    out->motionBlur = blur.motionBlur;

    const Mat3 canonicalToPixel = TransformMath::canonicalToPixel(srcPar, sx, sy, fielded);
    const Mat3 pixelToCanonical = TransformMath::pixelToCanonical(dstPar, sx, sy, fielded);
    const size_t alloc = (size_t)kMotionBlurTransformCount;
    Mat3 canonical;

    if ((blur.shutter != 0.) && (blur.motionBlur != 0.)) {
        double tStart, tEnd;
        shutterRange(time, blur.shutter, blur.shutterOffset, blur.shutterCustomOffset, &tStart, &tEnd);
        out->invTransforms.resize(alloc);
        bool allEqual = true;
        for (size_t i = 0; i < alloc; ++i) {
            double t = (i == 0) ? tStart : (tStart + i * (tEnd - tStart) / (double)(alloc - 1));
            if (fn(t, 1., invert, &canonical)) {
                out->invTransforms[i] = canonicalToPixel * canonical * pixelToCanonical;
            } else {
                out->invTransforms[i] = nullSamplingMatrix();
            }
            allEqual = allEqual && (out->invTransforms[i] == out->invTransforms[0]);
        }
        if (allEqual) {
            out->invTransforms.resize(1);
        }
    } else if (blur.directionalBlur) {
        out->invTransforms.reserve(alloc);
        out->alphas.reserve(alloc);
        bool allEqual = true;
        for (size_t i = 0; i < alloc; ++i) {
            double a = 1. - (i + 1) / (double)(alloc);
            double amt = blur.amountFrom + (blur.amountTo - blur.amountFrom) * a;
            if (fn(time, amt, invert, &canonical)) {
                out->alphas.push_back(amt);
                out->invTransforms.push_back(canonicalToPixel * canonical * pixelToCanonical);
                allEqual = allEqual && (out->invTransforms.back() == out->invTransforms.front());
            }
        }
        if (!out->invTransforms.empty() && allEqual) {
            out->invTransforms.resize(1);
            out->alphas.resize(1);
        }
        if (out->invTransforms.empty()) {
            out->invTransforms.push_back(nullSamplingMatrix());
            out->alphas.clear();
        } else if (blur.fading <= 0.) {
            std::fill(out->alphas.begin(), out->alphas.end(), 1.);
        } else {
            for (size_t i = 0; i < out->alphas.size(); ++i) {
                out->alphas[i] = std::pow(1. - std::abs(out->alphas[i]) / blur.amountTo, blur.fading);
            }
        }
    } else {
        if (fn(time, 1., invert, &canonical)) {
            out->invTransforms.push_back(canonicalToPixel * canonical * pixelToCanonical);
        } else {
            out->invTransforms.push_back(nullSamplingMatrix());
        }
    }

    if (out->invTransforms.size() == 1) {
        out->motionBlur = 0.;
    }
}

void
concatenateInputTransform(const Mat3& inputTransformPixel,
                          SamplingTransforms* transforms)
{
    Mat3 inverse;

    if (!inputTransformPixel.inverse(&inverse)) {
        return;
    }
    for (size_t i = 0; i < transforms->invTransforms.size(); ++i) {
        transforms->invTransforms[i] = inverse * transforms->invTransforms[i];
    }
}

ResampleParams
makeResampleParams(const SamplingTransforms& transforms,
                   FilterEnum filter,
                   bool clamp,
                   bool blackOutside)
{
    ResampleParams p;

    p.invTransforms = transforms.invTransforms.empty() ? 0 : &transforms.invTransforms.front();
    p.alphas = transforms.alphas.empty() ? 0 : &transforms.alphas.front();
    p.count = transforms.invTransforms.size();
    p.filter = filter;
    p.clamp = clamp;
    p.blackOutside = blackOutside;
    p.motionBlur = transforms.motionBlur;

    return p;
}

void
resampleRow(const ResampleParams& params,
            const SourceImage& src,
            int y,
            int x1,
            int x2,
            float* dst)
{
    if ((x2 <= x1) || !params.invTransforms || (params.count == 0)) {
        if (x2 > x1) {
            zeroPixels(dst, (x2 - x1) * src.nComps);
        }
        return;
    }
    if (!src.isValid()) {
        zeroPixels(dst, (x2 - x1) * src.nComps);
        return;
    }
    selectKernels(src.nComps, params.filter, params.clamp).row(params, src, y, x1, x2, dst);
}

void
resampleRowFromCoords(const SourceImage& src,
                      const float* sx,
                      const float* sy,
                      const float* jacobian,
                      int n,
                      FilterEnum filter,
                      bool clamp,
                      bool blackOutside,
                      WrapMode wrapU,
                      WrapMode wrapV,
                      float* out,
                      int nComps,
                      const RectI* wrapDomain)
{
    if ((n <= 0) || (nComps <= 0)) {
        return;
    }
    if (!src.isValid()) {
        zeroPixels(out, n * nComps);
        return;
    }
    const RectI& domain = (wrapDomain && (wrapDomain->x2 > wrapDomain->x1) && (wrapDomain->y2 > wrapDomain->y1)) ? *wrapDomain : src.bounds;
    selectKernels(src.nComps, filter, clamp).coords(src, sx, sy, jacobian, n, blackOutside, wrapU, wrapV, domain, out, nComps);
}

namespace {
    inline double
    rowDerivative(const float* s,
                  int i,
                  int n)
    {
        if (n < 2) {
            return 0.;
        }
        if (i == 0) {
            return (double)s[1] - (double)s[0];
        }
        if (i == n - 1) {
            return (double)s[n - 1] - (double)s[n - 2];
        }
        return ((double)s[i + 1] - (double)s[i - 1]) / 2.;
    }

    inline double
    columnDerivative(const float* prev,
                     const float* row,
                     const float* next,
                     int i)
    {
        if (prev && next) {
            return ((double)next[i] - (double)prev[i]) / 2.;
        }
        if (next) {
            return (double)next[i] - (double)row[i];
        }
        if (prev) {
            return (double)row[i] - (double)prev[i];
        }
        return 0.;
    }
} // anon namespace

void
coordJacobians(const float* sxPrev,
               const float* sx,
               const float* sxNext,
               const float* syPrev,
               const float* sy,
               const float* syNext,
               int n,
               float* jacobian)
{
    for (int i = 0; i < n; ++i, jacobian += 4) {
        jacobian[0] = finiteFloatOrZero(rowDerivative(sx, i, n));
        jacobian[1] = finiteFloatOrZero(columnDerivative(sxPrev, sx, sxNext, i));
        jacobian[2] = finiteFloatOrZero(rowDerivative(sy, i, n));
        jacobian[3] = finiteFloatOrZero(columnDerivative(syPrev, sy, syNext, i));
    }
}

namespace {
    inline bool
    pixelInside(const RectI& bounds,
                int x,
                int y)
    {
        return (x >= bounds.x1) && (x < bounds.x2) && (y >= bounds.y1) && (y < bounds.y2);
    }

    // Redoes the central differences of one coordinate at pixel i of a row (0 < i < n - 1) when
    // the pixel is inside bounds and a neighbour is not, standing the pixel's own value, moved by
    // the constant-UV step, in for that neighbour.
    void
    differencesWithinBounds(const float* prev,
                            const float* row,
                            const float* next,
                            int i,
                            int x,
                            int y,
                            const RectI& bounds,
                            double stepX,
                            double stepY,
                            float* dx,
                            float* dy)
    {
        if (!pixelInside(bounds, x, y)) {
            return;
        }
        const double centre = (double)row[i];
        const bool left = pixelInside(bounds, x - 1, y);
        const bool right = pixelInside(bounds, x + 1, y);
        if (!left || !right) {
            const double before = left ? (double)row[i - 1] : centre - stepX;
            const double after = right ? (double)row[i + 1] : centre + stepX;
            *dx = finiteFloatOrZero((after - before) / 2.);
        }
        if (!prev || !next) {
            return;
        }
        const bool below = pixelInside(bounds, x, y - 1);
        const bool above = pixelInside(bounds, x, y + 1);
        if (!below || !above) {
            const double before = below ? (double)prev[i] : centre - stepY;
            const double after = above ? (double)next[i] : centre + stepY;
            *dy = finiteFloatOrZero((after - before) / 2.);
        }
    }
} // namespace

void
coordJacobiansWithinBounds(const float* sxPrev,
                           const float* sx,
                           const float* sxNext,
                           const float* syPrev,
                           const float* sy,
                           const float* syNext,
                           int x0,
                           int y,
                           int n,
                           const RectI& uBounds,
                           const RectI& vBounds,
                           double stepX,
                           double stepY,
                           float* jacobian)
{
    coordJacobians(sxPrev, sx, sxNext, syPrev, sy, syNext, n, jacobian);
    for (int i = 1; i < n - 1; ++i) {
        float* J = jacobian + (std::size_t)4 * i;
        const int x = x0 + i;
        differencesWithinBounds(sxPrev, sx, sxNext, i, x, y, uBounds, stepX, 0., &J[0], &J[1]);
        differencesWithinBounds(syPrev, sy, syNext, i, x, y, vBounds, 0., stepY, &J[2], &J[3]);
    }
}

void
motionBlurPixel(const ResampleParams& params,
                const SourceImage& src,
                int x,
                int y,
                float* out,
                int* samplesTaken)
{
    if (!params.invTransforms || (params.count == 0) || (src.nComps <= 0)) {
        zeroPixels(out, src.nComps);
        if (samplesTaken) {
            *samplesTaken = 0;
        }
        return;
    }
    selectKernels(src.nComps, params.filter, params.clamp).motionBlurPixel(params, src, x, y, out, samplesTaken);
}

///////////////////////////////////////////////////////////////////////////////
// Regions

namespace {
    inline bool
    rectIsEmpty(const RectD& r)
    {
        return (r.x2 <= r.x1) || (r.y2 <= r.y1);
    }

    inline bool
    rectIsInfinite(const RectD& r)
    {
        return (r.x1 <= kOfxFlagInfiniteMin) || (r.x2 >= kOfxFlagInfiniteMax) || (r.y1 <= kOfxFlagInfiniteMin) || (r.y2 >= kOfxFlagInfiniteMax);
    }

    // OFX semantics: an empty operand yields the other one unchanged.
    void
    rectBoundingBox(const RectD& a,
                    const RectD& b,
                    RectD* bbox)
    {
        if (rectIsEmpty(a)) {
            *bbox = b;
            return;
        }
        if (rectIsEmpty(b)) {
            *bbox = a;
            return;
        }
        const double x1 = (std::min)(a.x1, b.x1);
        const double x2 = (std::max)(x1, (std::max)(a.x2, b.x2));
        const double y1 = (std::min)(a.y1, b.y1);
        const double y2 = (std::max)(y1, (std::max)(a.y2, b.y2));
        bbox->x1 = x1;
        bbox->x2 = x2;
        bbox->y1 = y1;
        bbox->y2 = y2;
    }

    // OFX semantics: no overlap (or an empty operand) yields the all-zero rectangle.
    void
    rectIntersection(const RectD& r1,
                     const RectD& r2,
                     RectD* out)
    {
        if (rectIsEmpty(r1) || rectIsEmpty(r2) || (r1.x1 > r2.x2) || (r2.x1 > r1.x2) || (r1.y1 > r2.y2) || (r2.y1 > r1.y2)) {
            out->x1 = out->x2 = out->y1 = out->y2 = 0.;
            return;
        }
        const double x1 = (std::max)(r1.x1, r2.x1);
        const double x2 = (std::max)(x1, (std::min)(r1.x2, r2.x2));
        const double y1 = (std::max)(r1.y1, r2.y1);
        const double y2 = (std::max)(y1, (std::min)(r1.y2, r2.y2));
        out->x1 = x1;
        out->x2 = x2;
        out->y1 = y1;
        out->y2 = y2;
    }

    void
    regionFromPoints(const Point3 p[4],
                     RectD* rod)
    {
        double x1 = 0., y1 = 0., x2 = 0., y2 = 0.;
        bool empty = true;

        for (int i = 0, j = 1; i < 4; ++i, j = (j + 1) % 4) {
            if (p[i].z > 0) {
                double x = p[i].x / p[i].z;
                double y = p[i].y / p[i].z;
                if (empty) {
                    empty = false;
                    x1 = x2 = x;
                    y1 = y2 = y;
                } else {
                    if (x < x1) {
                        x1 = x;
                    } else if (x > x2) {
                        x2 = x;
                    }
                    if (y < y1) {
                        y1 = y;
                    } else if (y > y2) {
                        y2 = y;
                    }
                }
            } else if (p[j].z > 0) {
                if (empty) {
                    empty = false;
                    x1 = x2 = p[j].x / p[j].z;
                    y1 = y2 = p[j].y / p[j].z;
                }
            }
            if (((p[i].z > 0) && (p[j].z <= 0)) || ((p[i].z <= 0) && (p[j].z > 0))) {
                // The edge crosses z = 0: the direction of the crossing point goes to infinity.
                double a = -p[i].z / (p[j].z - p[i].z);
                double dx = p[i].x + a * (p[j].x - p[i].x);
                double dy = p[i].y + a * (p[j].y - p[i].y);
                if (empty) {
                    empty = false;
                    x1 = x2 = p[j].x / p[j].z;
                    y1 = y2 = p[j].y / p[j].z;
                }
                if (dx < 0) {
                    x1 = kOfxFlagInfiniteMin;
                } else if (dx > 0) {
                    x2 = kOfxFlagInfiniteMax;
                }
                if (dy < 0) {
                    y1 = kOfxFlagInfiniteMin;
                } else if (dy > 0) {
                    y2 = kOfxFlagInfiniteMax;
                }
            }
        }

        if (empty) {
            rod->x1 = rod->x2 = rod->y1 = rod->y2 = 0;
        } else {
            rod->x1 = x1;
            rod->x2 = x2;
            rod->y1 = y1;
            rod->y2 = y2;
        }
    }

    void
    regionFromRect(const RectD& rect,
                   const Mat3& transform,
                   Point3 p[4],
                   RectD* out)
    {
        p[0] = transform * Point3(rect.x1, rect.y1, 1);
        p[1] = transform * Point3(rect.x1, rect.y2, 1);
        p[2] = transform * Point3(rect.x2, rect.y2, 1);
        p[3] = transform * Point3(rect.x2, rect.y1, 1);
        regionFromPoints(p, out);
    }
} // anon namespace

void
transformRegionFromRect(const RectD& rect,
                        const Mat3& transform,
                        RectD* out)
{
    Point3 p[4];

    regionFromRect(rect, transform, p, out);
}

void
transformRegion(const CanonicalTransformFn& fn,
                const RectD& rectFrom,
                double time,
                bool invert,
                const BlurSettings& blur,
                bool isIdentity,
                RectD* rectTo)
{
    double rangeMin, rangeMax;
    const bool hasmotionblur = ((blur.shutter != 0. || blur.directionalBlur) && blur.motionBlur != 0.);

    if (hasmotionblur && !blur.directionalBlur) {
        shutterRange(time, blur.shutter, blur.shutterOffset, blur.shutterCustomOffset, &rangeMin, &rangeMax);
    } else {
        if (isIdentity) {
            *rectTo = rectFrom;
            return;
        }
        rangeMin = rangeMax = time;
    }

    // Min and max swapped: an "empty" start that the first region replaces.
    rectTo->x1 = kOfxFlagInfiniteMax;
    rectTo->x2 = kOfxFlagInfiniteMin;
    rectTo->y1 = kOfxFlagInfiniteMax;
    rectTo->y2 = kOfxFlagInfiniteMin;
    double t = rangeMin;
    bool first = true;
    bool last = !hasmotionblur;
    bool finished = false;
    double expand = 0.;
    double amount = 1.;
    int dirBlurIter = 0;
    Point3 pPrev[4];
    while (!finished) {
        Mat3 transform;
        if (!fn(t, blur.amountFrom + amount * (blur.amountTo - blur.amountFrom), invert, &transform)) {
            rectTo->x1 = kOfxFlagInfiniteMin;
            rectTo->x2 = kOfxFlagInfiniteMax;
            rectTo->y1 = kOfxFlagInfiniteMin;
            rectTo->y2 = kOfxFlagInfiniteMax;
            return;
        }
        Point3 p[4];
        RectD thisRoD;
        regionFromRect(rectFrom, transform, p, &thisRoD);
        rectBoundingBox(*rectTo, thisRoD, rectTo);

        if (first) {
            first = false;
        } else {
            for (int k = 0; k < 4; ++k) {
                expand = (std::max)(expand, std::fabs(pPrev[k].x - p[k].x));
                expand = (std::max)(expand, std::fabs(pPrev[k].y - p[k].y));
            }
        }

        if (last) {
            finished = true;
        } else {
            for (int k = 0; k < 4; ++k) {
                pPrev[k] = p[k];
            }
            if (blur.directionalBlur) {
                const int dirBlurIterMax = 8;
                ++dirBlurIter;
                amount = 1. - dirBlurIter / (double)dirBlurIterMax;
                last = dirBlurIter == dirBlurIterMax;
            } else {
                t = std::floor(t * 4 + 1) / 4;
                if (t >= rangeMax) {
                    t = rangeMax;
                    last = true;
                }
            }
        }
    }
    if (rectTo->x1 > kOfxFlagInfiniteMin) {
        rectTo->x1 -= expand;
    }
    if (rectTo->x2 < kOfxFlagInfiniteMax) {
        rectTo->x2 += expand;
    }
    if (rectTo->y1 > kOfxFlagInfiniteMin) {
        rectTo->y1 -= expand;
    }
    if (rectTo->y2 < kOfxFlagInfiniteMax) {
        rectTo->y2 += expand;
    }
}

void
expandRoD(double par,
          double sx,
          double sy,
          bool blackOutside,
          RectD* rod)
{
    if ((rod->x2 <= rod->x1) || (rod->y2 <= rod->y1)) {
        return;
    }
    if (!blackOutside) {
        return;
    }
    const double pixelSizeX = par / sx;
    const double pixelSizeY = 1. / sy;
    if (rod->x1 > kOfxFlagInfiniteMin) {
        rod->x1 = rod->x1 - pixelSizeX;
    }
    if (rod->x2 < kOfxFlagInfiniteMax) {
        rod->x2 = rod->x2 + pixelSizeX;
    }
    if (rod->y1 > kOfxFlagInfiniteMin) {
        rod->y1 = rod->y1 - pixelSizeY;
    }
    if (rod->y2 < kOfxFlagInfiniteMax) {
        rod->y2 = rod->y2 + pixelSizeY;
    }
}

void
expandRoI(const RectD& roi,
          double par,
          double sx,
          double sy,
          FilterEnum filter,
          bool doMasking,
          double mix,
          RectD* srcRoI)
{
    if ((roi.x2 <= roi.x1) || (roi.y2 <= roi.y1)) {
        *srcRoI = roi;
        return;
    }
    const double pixelSizeX = par / sx;
    const double pixelSizeY = 1. / sy;
    const double e = filterRoIExpansion(filter);
    if (e > 0.) {
        if (srcRoI->x1 > kOfxFlagInfiniteMin) {
            srcRoI->x1 -= e * pixelSizeX;
        }
        if (srcRoI->x2 < kOfxFlagInfiniteMax) {
            srcRoI->x2 += e * pixelSizeX;
        }
        if (srcRoI->y1 > kOfxFlagInfiniteMin) {
            srcRoI->y1 -= e * pixelSizeY;
        }
        if (srcRoI->y2 < kOfxFlagInfiniteMax) {
            srcRoI->y2 += e * pixelSizeY;
        }
    }
    if (doMasking || (mix != 1.)) {
        srcRoI->x1 = (std::min)(srcRoI->x1, roi.x1);
        srcRoI->x2 = (std::max)(srcRoI->x2, roi.x2);
        srcRoI->y1 = (std::min)(srcRoI->y1, roi.y1);
        srcRoI->y2 = (std::max)(srcRoI->y2, roi.y2);
    }
}

void
getRegionOfDefinition(const CanonicalTransformFn& fn,
                      const RectD& srcRoD,
                      double time,
                      double dstPar,
                      double sx,
                      double sy,
                      const RegionParams& params,
                      RectD* rod)
{
    if (rectIsInfinite(srcRoD)) {
        rod->x1 = kOfxFlagInfiniteMin;
        rod->x2 = kOfxFlagInfiniteMax;
        rod->y1 = kOfxFlagInfiniteMin;
        rod->y2 = kOfxFlagInfiniteMax;
        return;
    }
    if (rectIsEmpty(srcRoD)) {
        rod->x1 = rod->x2 = rod->y1 = rod->y2 = 0.;
        return;
    }
    if (params.doMasking && (params.mix == 0.)) {
        *rod = srcRoD;
        return;
    }

    transformRegion(fn, srcRoD, time, !params.invert, params.blur, params.isIdentity, rod);
    if (!params.isIdentity) {
        expandRoD(dstPar, sx, sy, params.blackOutside, rod);
    }
    if (params.doMasking) {
        rectBoundingBox(*rod, srcRoD, rod);
    }
}

void
getRegionOfInterest(const CanonicalTransformFn& fn,
                    const RectD& roi,
                    const RectD& srcRoD,
                    const RectD& projectRect,
                    double time,
                    double srcPar,
                    double sx,
                    double sy,
                    const RegionParams& params,
                    RectD* srcRoI)
{
    const double mix = params.doMasking ? params.mix : 1.;

    if (params.doMasking && (mix == 0.)) {
        *srcRoI = roi;
        return;
    }

    transformRegion(fn, roi, time, params.invert, params.blur, params.isIdentity, srcRoI);
    expandRoI(roi, srcPar, sx, sy, params.filter, params.doMasking, mix, srcRoI);

    if (rectIsInfinite(*srcRoI)) {
        // An RoI cannot be infinite: fall back to what the source and the project can show.
        rectIntersection(*srcRoI, srcRoD, srcRoI);
        rectBoundingBox(*srcRoI, projectRect, srcRoI);
    }
}
} // namespace Resampler

NATRON_NAMESPACE_EXIT
