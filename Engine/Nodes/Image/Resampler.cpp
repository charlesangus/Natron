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
#include <cstddef>
#include <limits>

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
        { "Lanczos 2", "(windowed sinc, 2 lobes) Sharp, with little ringing (*).", "lanczos2" },
        { "Lanczos 3", "(windowed sinc, 3 lobes) Sharper, with moderate ringing (*).", "lanczos3" },
        { "Lanczos 4", "(windowed sinc, 4 lobes) Sharpest of the Lanczos filters, with the most ringing (*).", "lanczos4" },
        { "Sinc", "(Hann-windowed sinc) Sharpness and ringing grow with the Lobes control (*).", "sinc" },
        { "Blackman Harris", "(4-term Blackman-Harris window) Soft, with very little aliasing (+).", "blackmanHarris" },
        { "Gaussian", "(sigma of 0.5 pixels) Soft, without overshoot (+).", "gaussian" },
        { "Bicubic", "(Mitchell-Netravali) A cubic shaped by the Bicubic B and Bicubic C controls (*)(+).", "bicubic" },
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

namespace {
    inline double
    sincPi(double x)
    {
        if (x == 0.) {
            return 1.;
        }
        // sin(pi * n) is not exactly 0 in doubles, and a 1:1 render must reproduce its source.
        if (x == std::floor(x)) {
            return 0.;
        }
        const double px = M_PI * x;
        return std::sin(px) / px;
    }

    inline double
    blackmanHarrisWindow(double t)
    {
        return 0.35875 + 0.48829 * std::cos(M_PI * t) + 0.14128 * std::cos(2. * M_PI * t) + 0.01168 * std::cos(3. * M_PI * t);
    }

    inline double
    gaussianBell(double x)
    {
        return std::exp(-2. * x * x);
    }

    // Lowers a bell peaking at 1 so it reaches 0 at its support edge: a tap entering the
    // footprint then starts from no weight, without a jump.
    inline double
    taperedToEdge(double value,
                  double edge)
    {
        return (value - edge) / (1. - edge);
    }

    inline double
    mitchellNetravali(double a,
                      double B,
                      double C)
    {
        const double a2 = a * a;
        const double a3 = a2 * a;

        if (a < 1.) {
            return ((12. - 9. * B - 6. * C) * a3 + (-18. + 12. * B + 6. * C) * a2 + (6. - 2. * B)) / 6.;
        }
        return ((-B - 6. * C) * a3 + (6. * B + 30. * C) * a2 + (-12. * B - 48. * C) * a + (8. * B + 24. * C)) / 6.;
    }

    inline int
    usableLobes(int lobes)
    {
        return (std::max)(2, (std::min)(lobes, 8));
    }
} // anon namespace

double
FilterSpec::radius() const
{
    switch (filter) {
    case eFilterImpulse:
    case eFilterBox:
        return 0.5;
    case eFilterBilinear:
    case eFilterCubic:
        return 1.;
    case eFilterGaussian:
        return 1.5;
    case eFilterKeys:
    case eFilterSimon:
    case eFilterRifman:
    case eFilterMitchell:
    case eFilterParzen:
    case eFilterNotch:
    case eFilterLanczos2:
    case eFilterBlackmanHarris:
    case eFilterBicubic:
        return 2.;
    case eFilterLanczos3:
        return 3.;
    case eFilterLanczos4:
        return 4.;
    case eFilterSinc:
        return (double)usableLobes(lobes);
    }

    return 2.;
}

double
FilterSpec::weight(double x) const
{
    const double a = std::fabs(x);

    if (!(a < radius())) {
        return 0.;
    }
    switch (filter) {
    case eFilterLanczos2:
        return sincPi(a) * sincPi(a / 2.);
    case eFilterLanczos3:
        return sincPi(a) * sincPi(a / 3.);
    case eFilterLanczos4:
        return sincPi(a) * sincPi(a / 4.);
    case eFilterSinc:
        return sincPi(a) * (0.5 + 0.5 * std::cos(M_PI * a / usableLobes(lobes)));
    case eFilterBlackmanHarris:
        return taperedToEdge(blackmanHarrisWindow(a / 2.), blackmanHarrisWindow(1.));
    case eFilterGaussian:
        return taperedToEdge(gaussianBell(a), gaussianBell(1.5));
    case eFilterBicubic:
        return mitchellNetravali(a, B, C);
    default:
        break;
    }
    // Ic is the sample at distance d, Ip the one at distance d + 1. One filter1D() call, rather
    // than filterWeights()'s four, keeps this cheap at one call per tap per pixel.
    if (a < 1.) {
        return filter1D(filter, 0., 1., 0., 0., a, false);
    }
    return filter1D(filter, 1., 0., 0., 0., a - 1., false);
}

int
filterTaps(const FilterSpec& filter)
{
    return (int)std::ceil(2. * filter.radius());
}

bool
filterHonoursClamp(FilterEnum filter)
{
    switch (filter) {
    case eFilterKeys:
    case eFilterSimon:
    case eFilterRifman:
    case eFilterMitchell:
    case eFilterLanczos2:
    case eFilterLanczos3:
    case eFilterLanczos4:
    case eFilterSinc:
    case eFilterBicubic:
        return true;
    default:
        return false;
    }
}

double
filterRoIExpansion(const FilterSpec& filter,
                   double scale)
{
    if ((filter.filter == eFilterImpulse) || (filter.filter == eFilterBox)) {
        return 0.;
    }
    // The pixel centre sits half a pixel inside the pixel the window's edge touches.
    return filter.radius() * kernelScale(scale, 0.) - 0.5;
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
    case eFilterLanczos2:
    case eFilterLanczos3:
    case eFilterLanczos4:
    case eFilterSinc:
    case eFilterBlackmanHarris:
    case eFilterGaussian:
    case eFilterBicubic: {
        const FilterSpec spec(filter);
        const double wp = spec.weight(d + 1.);
        const double wc = spec.weight(d);
        const double wn = spec.weight(1. - d);
        const double wa = spec.weight(2. - d);
        const double I = (wp * Ip + wc * Ic + wn * In + wa * Ia) / (wp + wc + wn + wa);
        return clamp ? clampValue(I, Ic, In) : I;
    }
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
    const double kCoordLimit = 1073741824.;

    inline int
    clampCoord(int v,
               int lo,
               int hiExclusive)
    {
        return (std::max)(lo, (std::min)(v, hiExclusive - 1));
    }

    /*
     * floor(v), saturated at +-2^30, far outside any source, NaN on the low side: converting a
     * double outside int's range is undefined. The GPU's toPos (resamplecore.slang) saturates alike.
     */
    inline int
    floorToInt(double v)
    {
        if (v > kCoordLimit) {
            return (int)kCoordLimit;
        }
        if (!(v >= -kCoordLimit)) {
            return -(int)kCoordLimit;
        }
        return (int)std::floor(v);
    }

    /*
     * The samples a variance-to-error quotient asks for: none more when it is negative, not
     * finite, or at or past 2^31, as x86's INT_MIN for those always gave.
     */
    inline int
    samplesForVariance(double q)
    {
        return (q >= 0. && q < 2147483648.) ? (int)q : 0;
    }

    template <int N>
    inline void
    zeroPixel(float* out)
    {
        for (int c = 0; c < N; ++c) {
            out[c] = 0.f;
        }
    }

    struct AxisTaps {
        int count;
        int lo[kFilterMaxTapsPerAxis];
        // Share of pixel lo + 1 in a bilinear read; 0 reads pixel lo alone.
        float frac[kFilterMaxTapsPerAxis];
        float weight[kFilterMaxTapsPerAxis];
    };

    void
    buildAxisTaps(const FilterSpec& filter,
                  double coord,
                  double scale,
                  AxisTaps* taps)
    {
        const double centre = (coord > kCoordLimit ? kCoordLimit : (coord >= -kCoordLimit ? coord : -kCoordLimit)) - 0.5;
        const double reach = filter.radius() * scale;
        // An open interval: every kernel is 0 on its support edge, so a tap exactly there adds a
        // read that contributes nothing.
        const double first = std::floor(centre - reach) + 1.;
        const double exactCount = std::ceil(centre + reach) - first;
        double weights[kFilterMaxTapsPerAxis];
        double sum = 0.;

        if (exactCount <= kFilterMaxTapsPerAxis) {
            taps->count = (int)exactCount;
            for (int i = 0; i < taps->count; ++i) {
                const double pixel = first + i;
                taps->lo[i] = (int)pixel;
                taps->frac[i] = 0.f;
                weights[i] = filter.weight((pixel - centre) / scale);
                sum += weights[i];
            }
        } else {
            taps->count = kFilterMaxTapsPerAxis;
            const double step = 2. * reach / kFilterMaxTapsPerAxis;
            for (int i = 0; i < taps->count; ++i) {
                const double offset = (i + 0.5) * step - reach;
                const double position = centre + offset;
                taps->lo[i] = floorToInt(position);
                taps->frac[i] = (float)(position - std::floor(position));
                weights[i] = filter.weight(offset / scale);
                sum += weights[i];
            }
        }
        for (int i = 0; i < taps->count; ++i) {
            taps->weight[i] = (sum != 0.) ? (float)(weights[i] / sum) : 0.f;
        }
    }

    // The offset of one read from the start of the source along an axis, or -1 for black.
    inline std::ptrdiff_t
    readOffset(int v,
               int lo,
               int hiExclusive,
               bool blackOutside,
               std::ptrdiff_t unit)
    {
        if ((v < lo) || (v >= hiExclusive)) {
            if (blackOutside) {
                return -1;
            }
            v = clampCoord(v, lo, hiExclusive);
        }
        return (std::ptrdiff_t)(v - lo) * unit;
    }

    // Returns whether any read lands on a source pixel.
    bool
    resolveReads(const AxisTaps& taps,
                 int lo,
                 int hiExclusive,
                 bool blackOutside,
                 std::ptrdiff_t unit,
                 std::ptrdiff_t* nearReads,
                 std::ptrdiff_t* farReads)
    {
        bool any = false;

        for (int i = 0; i < taps.count; ++i) {
            nearReads[i] = readOffset(taps.lo[i], lo, hiExclusive, blackOutside, unit);
            farReads[i] = (taps.frac[i] != 0.f) ? readOffset(taps.lo[i] + 1, lo, hiExclusive, blackOutside, unit) : -1;
            any = any || (nearReads[i] >= 0) || (farReads[i] >= 0);
        }
        return any;
    }

    template <int N>
    inline void
    readAlongRow(const float* row,
                 std::ptrdiff_t nearRead,
                 std::ptrdiff_t farRead,
                 float frac,
                 float* v)
    {
        const float* a = (row && (nearRead >= 0)) ? row + nearRead : 0;

        if (frac == 0.f) {
            for (int c = 0; c < N; ++c) {
                v[c] = a ? a[c] : 0.f;
            }
            return;
        }
        const float* b = (row && (farRead >= 0)) ? row + farRead : 0;
        for (int c = 0; c < N; ++c) {
            v[c] = (a ? a[c] : 0.f) * (1.f - frac) + (b ? b[c] : 0.f) * frac;
        }
    }

    /*
     * The weighted footprint at (fx, fy), widened per axis by scaleX and scaleY. Black reads
     * count towards the clamp range: leaving them out would clamp a fading edge back up to the
     * source's values. Zero-weight taps do not count, so a 1:1 render stays exact.
     */
    template <int N>
    bool
    kernelSampleT(const FilterSpec& filter,
                  bool clamp,
                  double fx,
                  double fy,
                  double scaleX,
                  double scaleY,
                  const SourceImage& src,
                  bool blackOutside,
                  float* out)
    {
        AxisTaps tx;
        AxisTaps ty;
        std::ptrdiff_t xNear[kFilterMaxTapsPerAxis];
        std::ptrdiff_t xFar[kFilterMaxTapsPerAxis];
        std::ptrdiff_t yNear[kFilterMaxTapsPerAxis];
        std::ptrdiff_t yFar[kFilterMaxTapsPerAxis];
        const RectI& b = src.bounds;

        buildAxisTaps(filter, fx, scaleX, &tx);
        buildAxisTaps(filter, fy, scaleY, &ty);
        const bool anyX = resolveReads(tx, b.x1, b.x2, blackOutside, (std::ptrdiff_t)src.nComps, xNear, xFar);
        const bool anyY = resolveReads(ty, b.y1, b.y2, blackOutside, (std::ptrdiff_t)src.rowStride, yNear, yFar);
        if (!anyX || !anyY) {
            zeroPixel<N>(out);
            return false;
        }

        const bool useRange = clamp && filterHonoursClamp(filter.filter);
        float acc[N];
        float lowest[N];
        float highest[N];
        for (int c = 0; c < N; ++c) {
            acc[c] = 0.f;
            lowest[c] = std::numeric_limits<float>::infinity();
            highest[c] = -std::numeric_limits<float>::infinity();
        }
        for (int j = 0; j < ty.count; ++j) {
            const float wy = ty.weight[j];
            if (wy == 0.f) {
                continue;
            }
            const float* rowNear = (yNear[j] >= 0) ? src.data + yNear[j] : 0;
            const float* rowFar = (yFar[j] >= 0) ? src.data + yFar[j] : 0;
            const float fracY = ty.frac[j];
            float rowAcc[N];
            zeroPixel<N>(rowAcc);
            for (int i = 0; i < tx.count; ++i) {
                const float wx = tx.weight[i];
                if (wx == 0.f) {
                    continue;
                }
                float v[N];
                readAlongRow<N>(rowNear, xNear[i], xFar[i], tx.frac[i], v);
                if (fracY != 0.f) {
                    float above[N];
                    readAlongRow<N>(rowFar, xNear[i], xFar[i], tx.frac[i], above);
                    for (int c = 0; c < N; ++c) {
                        v[c] = v[c] * (1.f - fracY) + above[c] * fracY;
                    }
                }
                for (int c = 0; c < N; ++c) {
                    rowAcc[c] += wx * v[c];
                }
                if (useRange) {
                    for (int c = 0; c < N; ++c) {
                        lowest[c] = (std::min)(lowest[c], v[c]);
                        highest[c] = (std::max)(highest[c], v[c]);
                    }
                }
            }
            for (int c = 0; c < N; ++c) {
                acc[c] += wy * rowAcc[c];
            }
        }
        for (int c = 0; c < N; ++c) {
            out[c] = (useRange && (lowest[c] <= highest[c])) ? (std::max)(lowest[c], (std::min)(acc[c], highest[c])) : acc[c];
        }
        return true;
    }

    template <int N>
    bool
    nearestT(double fx,
             double fy,
             const SourceImage& src,
             bool blackOutside,
             float* tmpPix)
    {
        const RectI& b = src.bounds;
        int mx = floorToInt(fx);
        int my = floorToInt(fy);

        if (!blackOutside) {
            mx = clampCoord(mx, b.x1, b.x2);
            my = clampCoord(my, b.y1, b.y2);
        }
        const float* p = src.pixel(mx, my);
        if (!p) {
            zeroPixel<N>(tmpPix);
            return false;
        }
        for (int c = 0; c < N; ++c) {
            tmpPix[c] = p[c];
        }
        return true;
    }

    template <int N>
    bool
    interpolateT(const FilterSpec& filter,
                 bool clamp,
                 double fx,
                 double fy,
                 const SourceImage& src,
                 bool blackOutside,
                 float* tmpPix)
    {
        if (!src.isValid()) {
            zeroPixel<N>(tmpPix);
            return false;
        }
        if ((filter.filter == eFilterImpulse) || (filter.filter == eFilterBox)) {
            return nearestT<N>(fx, fy, src, blackOutside, tmpPix);
        }
        return kernelSampleT<N>(filter, clamp, fx, fy, 1., 1., src, blackOutside, tmpPix);
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

    template <int N>
    void
    interpolateSuperT(const FilterSpec& filter,
                      bool clamp,
                      double fx,
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
            zeroPixel<N>(tmpPix);
            return;
        }
        if ((filter.filter == eFilterImpulse) || ((Jxx == 0.) && (Jxy == 0.) && (Jyx == 0.) && (Jyy == 0.))) {
            interpolateT<N>(filter, clamp, fx, fy, src, blackOutside, tmpPix);
            return;
        }
        if (filter.filter == eFilterBox) {
            zeroPixel<N>(tmpPix);
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
            // NaN corners fall back to the centre too, before integrate2d floors them.
            if (!(x1 < x2) || !(y1 < y2)) {
                interpolateT<N>(filter, clamp, fx, fy, src, blackOutside, tmpPix);
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
        kernelSampleT<N>(filter, clamp, fx, fy, kernelScale(Jxx, Jxy), kernelScale(Jyx, Jyy), src, blackOutside, tmpPix);
    }

    /*
     * One sample of the destination pixel (x, y) through the inverse pixel matrix H: the
     * Transform3x3 processor's per-sample body.
     */
    template <int N>
    inline void
    samplePixel(const FilterSpec& filter,
                bool clamp,
                const Mat3& H,
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
        if (filter.filter == eFilterImpulse) {
            interpolateT<N>(filter, clamp, fx, fy, src, blackOutside, tmpPix);
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
        interpolateSuperT<N>(filter, clamp, fx, fy, Jxx, Jxy, Jyx, Jyy, src, blackOutside, tmpPix);
    }

    /*
     * samplePixel() for a matrix whose last row is exactly (0, 0, 1). Then z is exactly 1, so
     * fx, fy and the Jacobian come out bit for bit as samplePixel() computes them, while only
     * the column products that vary along the row are recomputed per pixel. rowX and rowY are
     * H(0, 1) * py and H(1, 1) * py for the row's pixel-centre ordinate py.
     */
    template <int N>
    inline void
    sampleAffinePixel(const FilterSpec& filter,
                      bool clamp,
                      const Mat3& H,
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
        if (filter.filter == eFilterImpulse) {
            interpolateT<N>(filter, clamp, fx, fy, src, blackOutside, tmpPix);
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
        interpolateSuperT<N>(filter, clamp, fx, fy, Jxx, Jxy, Jyx, Jyy, src, blackOutside, tmpPix);
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

    template <int N>
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
                samplePixel<N>(params.filter, params.clamp, params.invTransforms[t], x, y, src, srcValid, params.blackOutside, tmpPix);
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
                            maxsamples = (std::max)(maxsamples, (std::min)(samplesForVariance(var[c] / maxErr2), maxIt));
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
                            maxsamples = (std::max)(maxsamples, (std::min)(samplesForVariance(var[c] / maxErr2), maxIt));
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

    template <int N>
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
                    sampleAffinePixel<N>(params.filter, params.clamp, H, rowX, rowY, x, src, params.blackOutside, dst);
                }
                return;
            }
            for (int x = x1; x < x2; ++x, dst += N) {
                samplePixel<N>(params.filter, params.clamp, H, x, y, src, srcValid, params.blackOutside, dst);
            }
        } else {
            for (int x = x1; x < x2; ++x, dst += N) {
                motionBlurPixelT<N>(params, src, x, y, dst, 0);
            }
        }
    }

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

    template <int N>
    void
    resampleCoordsT(const FilterSpec& filter,
                    bool clamp,
                    const SourceImage& src,
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
            if ((filter.filter == eFilterImpulse) || !jacobian) {
                interpolateT<N>(filter, clamp, fx, fy, src, blackOutside, tmpPix);
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
                interpolateSuperT<N>(filter, clamp, fx, fy, Jxx, Jxy, Jyx, Jyy, src, blackOutside, tmpPix);
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

    typedef bool (*InterpolateFn)(const FilterSpec&, bool, double, double, const SourceImage&, bool, float*);
    typedef void (*InterpolateSuperFn)(const FilterSpec&, bool, double, double, double, double, double, double, const SourceImage&, bool, float*);
    typedef void (*RowFn)(const ResampleParams&, const SourceImage&, int, int, int, float*);
    typedef void (*MotionBlurPixelFn)(const ResampleParams&, const SourceImage&, int, int, float*, int*);
    typedef void (*CoordsFn)(const FilterSpec&, bool, const SourceImage&, const float*, const float*, const float*, int, bool, WrapMode, WrapMode, const RectI&, float*, int);

    struct KernelTable {
        InterpolateFn interpolate;
        InterpolateSuperFn interpolateSuper;
        RowFn row;
        MotionBlurPixelFn motionBlurPixel;
        CoordsFn coords;
    };

    template <int N>
    KernelTable
    makeTable()
    {
        KernelTable t;

        t.interpolate = &interpolateT<N>;
        t.interpolateSuper = &interpolateSuperT<N>;
        t.row = &resampleRowT<N>;
        t.motionBlurPixel = &motionBlurPixelT<N>;
        t.coords = &resampleCoordsT<N>;

        return t;
    }

    KernelTable
    selectKernels(int nComps)
    {
        switch (nComps) {
        case 1:
            return makeTable<1>();
        case 2:
            return makeTable<2>();
        case 3:
            return makeTable<3>();
        default:
            return makeTable<4>();
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

double
kernelScale(double a,
            double b)
{
    const double norm = std::hypot(a, b);

    if (!(norm > 1.) || !std::isfinite(norm)) {
        return 1.;
    }
    // Coordinates saturate at this limit, so a wider footprint reaches nothing further, and the
    // bound keeps radius * scale from overflowing.
    return (std::min)(norm, kCoordLimit);
}

bool
interpolate(const FilterSpec& filter,
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
    return selectKernels(src.nComps).interpolate(filter, clamp, fx, fy, src, blackOutside, out);
}

void
interpolateSuper(const FilterSpec& filter,
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
    selectKernels(src.nComps).interpolateSuper(filter, clamp, fx, fy, Jxx, Jxy, Jyx, Jyy, src, blackOutside, out);
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
                   const FilterSpec& filter,
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
    selectKernels(src.nComps).row(params, src, y, x1, x2, dst);
}

void
resampleRowFromCoords(const SourceImage& src,
                      const float* sx,
                      const float* sy,
                      const float* jacobian,
                      int n,
                      const FilterSpec& filter,
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
    selectKernels(src.nComps).coords(filter, clamp, src, sx, sy, jacobian, n, blackOutside, wrapU, wrapV, domain, out, nComps);
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
    selectKernels(src.nComps).motionBlurPixel(params, src, x, y, out, samplesTaken);
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

namespace {
    typedef std::function<void(const Mat3&)> MatrixVisitor;

    void
    transformRegionVisiting(const CanonicalTransformFn& fn,
                            const RectD& rectFrom,
                            double time,
                            bool invert,
                            const BlurSettings& blur,
                            bool isIdentity,
                            RectD* rectTo,
                            const MatrixVisitor& visit)
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
            if (visit) {
                visit(transform);
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
} // anon namespace

void
transformRegion(const CanonicalTransformFn& fn,
                const RectD& rectFrom,
                double time,
                bool invert,
                const BlurSettings& blur,
                bool isIdentity,
                RectD* rectTo)
{
    transformRegionVisiting(fn, rectFrom, time, invert, blur, isIdentity, rectTo, MatrixVisitor());
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
          const FilterSpec& filter,
          bool doMasking,
          double mix,
          RectD* srcRoI,
          double scaleX,
          double scaleY)
{
    if ((roi.x2 <= roi.x1) || (roi.y2 <= roi.y1)) {
        *srcRoI = roi;
        return;
    }
    const double pixelSizeX = par / sx;
    const double pixelSizeY = 1. / sy;
    const double ex = filterRoIExpansion(filter, scaleX) * pixelSizeX;
    const double ey = filterRoIExpansion(filter, scaleY) * pixelSizeY;
    if (ex > 0.) {
        if (srcRoI->x1 > kOfxFlagInfiniteMin) {
            srcRoI->x1 -= ex;
        }
        if (srcRoI->x2 < kOfxFlagInfiniteMax) {
            srcRoI->x2 += ex;
        }
    }
    if (ey > 0.) {
        if (srcRoI->y1 > kOfxFlagInfiniteMin) {
            srcRoI->y1 -= ey;
        }
        if (srcRoI->y2 < kOfxFlagInfiniteMax) {
            srcRoI->y2 += ey;
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
    if (params.doMasking || (params.mix != 1.)) {
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
                    double dstPar,
                    double sx,
                    double sy,
                    const RegionParams& params,
                    RectD* srcRoI)
{
    if (params.doMasking && (params.mix == 0.)) {
        *srcRoI = roi;
        return;
    }

    const bool kernelFilter = (params.filter.filter != eFilterImpulse) && (params.filter.filter != eFilterBox);
    double scaleX = 1.;
    double scaleY = 1.;
    bool cornerBehind = false;
    MatrixVisitor visit;
    if (kernelFilter && (roi.x2 > roi.x1) && (roi.y2 > roi.y1)) {
        const Mat3 toSrc = TransformMath::canonicalToPixel(srcPar, sx, sy);
        const Mat3 fromDst = TransformMath::pixelToCanonical(dstPar, sx, sy);
        // The window's corners in output pixels.
        const double cx[2] = { roi.x1 * sx / dstPar, roi.x2 * sx / dstPar };
        const double cy[2] = { roi.y1 * sy, roi.y2 * sy };
        visit = [&](const Mat3& M) {
            const Mat3 H = toSrc * M * fromDst;
            for (int i = 0; i < 2; ++i) {
                for (int j = 0; j < 2; ++j) {
                    const Point3 p = H * Point3(cx[i], cy[j], 1);
                    if (!(p.z > 0.)) {
                        cornerBehind = true;
                        continue;
                    }
                    const double z2 = p.z * p.z;
                    scaleX = (std::max)(scaleX, kernelScale((H(0, 0) * p.z - p.x * H(2, 0)) / z2, (H(0, 1) * p.z - p.x * H(2, 1)) / z2));
                    scaleY = (std::max)(scaleY, kernelScale((H(1, 0) * p.z - p.y * H(2, 0)) / z2, (H(1, 1) * p.z - p.y * H(2, 1)) / z2));
                }
            }
        };
    }
    transformRegionVisiting(fn, roi, time, params.invert, params.blur, params.isIdentity, srcRoI, visit);
    if (cornerBehind && !rectIsEmpty(srcRoD)) {
        *srcRoI = srcRoD;
        if (params.doMasking || (params.mix != 1.)) {
            rectBoundingBox(*srcRoI, roi, srcRoI);
        }
    } else {
        expandRoI(roi, srcPar, sx, sy, params.filter, params.doMasking, params.mix, srcRoI, scaleX, scaleY);
    }

    if (rectIsInfinite(*srcRoI)) {
        // An RoI cannot be infinite: fall back to what the source and the project can show.
        rectIntersection(*srcRoI, srcRoD, srcRoI);
        rectBoundingBox(*srcRoI, projectRect, srcRoI);
    }
}
} // namespace Resampler

NATRON_NAMESPACE_EXIT
