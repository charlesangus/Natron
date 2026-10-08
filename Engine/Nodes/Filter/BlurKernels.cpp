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
 * Ports of CImg's CImg<float>::deriche(), CImg<float>::vanvliet() and CImg<float>::boxfilter()
 * (CImg 2.9.9, as bundled with openfx-misc), by David Tschumperle and contributors,
 * <http://cimg.eu>. CImg is distributed under the CeCILL-C licence, which is compatible with
 * the GNU GPL.
 */

#include "BlurKernels.h"

#include <algorithm>
#include <cmath>

NATRON_NAMESPACE_ENTER

namespace BlurKernels {
// The expressions below keep CImg's exact mix of float and double operands and its evaluation
// order on purpose: the native Blur must round where CImgBlur does.

LineFilter::LineFilter()
    : _kind(eKindIdentity)
    , _order(0)
    , _neumann(false)
    , _a0(0)
    , _a1(0)
    , _a2(0)
    , _a3(0)
    , _b1(0)
    , _b2(0)
    , _coefp(0)
    , _coefn(0)
    , _filter { 0, 0, 0, 0 }
    , _triggs { 0, 0, 0, 0, 0, 0, 0, 0, 0 }
    , _boxSize(0)
    , _iterations(0)
{
}

LineFilter
LineFilter::deriche(float sigma,
                    unsigned int order,
                    bool neumann)
{
    LineFilter f;
    const double nsigma = std::max(0.f, sigma);
    if (((nsigma < 0.1f) && !order) || (order > 2)) {
        return f;
    }
    const double nnsigma = nsigma < 0.1f ? 0.1f : nsigma, alpha = 1.695f / nnsigma, ema = std::exp(-alpha),
                 ema2 = std::exp(-2 * alpha), b1 = -2 * ema, b2 = ema2;
    double a0 = 0, a1 = 0, a2 = 0, a3 = 0;
    switch (order) {
    case 0: {
        const double k = (1 - ema) * (1 - ema) / (1 + 2 * alpha * ema - ema2);
        a0 = k;
        a1 = k * (alpha - 1) * ema;
        a2 = k * (alpha + 1) * ema;
        a3 = -k * ema2;
        break;
    }
    case 1: {
        const double k = -(1 - ema) * (1 - ema) * (1 - ema) / (2 * (ema + 1) * ema);
        a0 = a3 = 0;
        a1 = k * ema;
        a2 = -a1;
        break;
    }
    default: {
        const double ea = std::exp(-alpha), k = -(ema2 - 1) / (2 * alpha * ema),
                     kn = (-2 * (-1 + 3 * ea - 3 * ea * ea + ea * ea * ea) / (3 * ea + 1 + 3 * ea * ea + ea * ea * ea));
        a0 = kn;
        a1 = -kn * (1 + k * alpha) * ema;
        a2 = kn * (1 - k * alpha) * ema;
        a3 = -kn * ema2;
        break;
    }
    }
    f._kind = eKindDeriche;
    f._order = order;
    f._neumann = neumann;
    f._a0 = a0;
    f._a1 = a1;
    f._a2 = a2;
    f._a3 = a3;
    f._b1 = b1;
    f._b2 = b2;
    f._coefp = (a0 + a1) / (1 + b1 + b2);
    f._coefn = (a2 + a3) / (1 + b1 + b2);

    return f;
}

LineFilter
LineFilter::vanVliet(float sigma,
                     unsigned int order,
                     bool neumann)
{
    const float nsigma = std::max(0.f, sigma);
    if (nsigma < 0.5f) {
        return deriche(nsigma, order, neumann);
    }
    LineFilter f;
    if (order > 3) {
        return f;
    }
    const double nnsigma = nsigma < 0.5f ? 0.5f : nsigma, m0 = 1.16680, m1 = 1.10783, m2 = 1.40586, m1sq = m1 * m1,
                 m2sq = m2 * m2,
                 q = (nnsigma < 3.556 ? -0.2568 + 0.5784 * nnsigma + 0.0561 * nnsigma * nnsigma
                                      : 2.5091 + 0.9804 * (nnsigma - 3.556)),
                 qsq = q * q, scale = (m0 + q) * (m1sq + m2sq + 2 * m1 * q + qsq),
                 b1 = -q * (2 * m0 * m1 + m1sq + m2sq + (2 * m0 + 4 * m1) * q + 3 * qsq) / scale,
                 b2 = qsq * (m0 + 2 * m1 + 3 * q) / scale, b3 = -qsq * q / scale, B = (m0 * (m1sq + m2sq)) / scale;
    f._kind = eKindVanVliet;
    f._order = order;
    f._neumann = neumann;
    f._filter[0] = B;
    f._filter[1] = -b1;
    f._filter[2] = -b2;
    f._filter[3] = -b3;

    const double a1 = f._filter[1], a2 = f._filter[2], a3 = f._filter[3],
                 scaleM = 1. / ((1. + a1 - a2 + a3) * (1. - a1 - a2 - a3) * (1. + a2 + (a1 - a3) * a3));
    double* M = f._triggs;
    M[0] = scaleM * (-a3 * a1 + 1. - a3 * a3 - a2);
    M[1] = scaleM * (a3 + a1) * (a2 + a3 * a1);
    M[2] = scaleM * a3 * (a1 + a3 * a2);
    M[3] = scaleM * (a1 + a3 * a2);
    M[4] = -scaleM * (a2 - 1.) * (a2 + a3 * a1);
    M[5] = -scaleM * a3 * (a3 * a1 + a3 * a3 + a2 - 1.);
    M[6] = scaleM * (a3 * a1 + a2 + a1 * a1 - a2 * a2);
    M[7] = scaleM * (a1 * a2 + a3 * a2 * a2 - a1 * a3 * a3 - a3 * a3 * a3 - a3 * a2 + a3);
    M[8] = scaleM * a3 * (a1 + a3 * a2);

    return f;
}

LineFilter
LineFilter::box(float boxSize,
                int order,
                bool neumann,
                unsigned int iterations)
{
    LineFilter f;
    const float nboxSize = std::max(0.f, boxSize);
    if (!nboxSize || ((nboxSize <= 1) && !order)) {
        return f;
    }
    const bool smooths = (nboxSize > 1) && iterations;
    if (!smooths && ((order < 1) || (order > 2))) {
        return f;
    }
    f._kind = eKindBox;
    f._order = order < 0 ? 0u : static_cast<unsigned int>(order);
    f._neumann = neumann;
    f._boxSize = nboxSize;
    f._iterations = iterations;

    return f;
}

LineFilter
LineFilter::forFilter(Filter filter,
                      float size,
                      unsigned int order,
                      bool neumann)
{
    switch (filter) {
    case eFilterQuasiGaussian:
        return deriche(size, order, neumann);
    case eFilterGaussian:
        return vanVliet(size, order, neumann);
    case eFilterBox:
        return box(size, static_cast<int>(order), neumann, 1);
    case eFilterTriangle:
        return box(size, static_cast<int>(order), neumann, 2);
    case eFilterQuadratic:
        return box(size, static_cast<int>(order), neumann, 3);
    }

    return LineFilter();
}

void
LineFilter::apply(float* data,
                  int n,
                  std::ptrdiff_t stride,
                  LineScratch& scratch) const
{
    if (!data || (n <= 0)) {
        return;
    }
    switch (_kind) {
    case eKindIdentity:
        return;
    case eKindDeriche:
        applyDeriche(data, n, stride, scratch);

        return;
    case eKindVanVliet:
        applyVanVliet(data, n, stride);

        return;
    case eKindBox:
        applyBox(data, n, stride, scratch);

        return;
    }
}

void
LineFilter::apply(float* data,
                  int n,
                  std::ptrdiff_t stride) const
{
    LineScratch scratch;

    apply(data, n, stride, scratch);
}

void
LineFilter::applyDeriche(float* data,
                         int N,
                         std::ptrdiff_t off,
                         LineScratch& scratch) const
{
    if (scratch.recursive.size() < static_cast<std::size_t>(N)) {
        scratch.recursive.resize(N);
    }
    double* Y = scratch.recursive.data();
    const double a0 = _a0, a1 = _a1, a2 = _a2, a3 = _a3, b1 = _b1, b2 = _b2;

    double yb = 0, yp = 0;
    float xp = 0.f;
    if (_neumann) {
        xp = data[0];
        yb = yp = (double)(_coefp * xp);
    }
    for (int m = 0; m < N; ++m) {
        const float xc = data[m * off];
        const double yc = Y[m] = (double)(a0 * xc + a1 * xp - b1 * yp - b2 * yb);
        xp = xc;
        yb = yp;
        yp = yc;
    }

    float xn = 0.f, xa = 0.f;
    double yn = 0, ya = 0;
    if (_neumann) {
        xn = xa = data[(N - 1) * off];
        yn = ya = (double)_coefn * xn;
    }
    for (int k = N - 1; k >= 0; --k) {
        const float xc = data[k * off];
        const double yc = (double)(a2 * xn + a3 * xa - b1 * yn - b2 * ya);
        xa = xn;
        xn = xc;
        ya = yn;
        yn = yc;
        data[k * off] = (float)(Y[k] + yc);
    }
}

void
LineFilter::applyVanVliet(float* data,
                          int N,
                          std::ptrdiff_t off) const
{
    const double* filter = _filter;
    const double* M = _triggs;
    const double sumsq = filter[0], sum = sumsq * sumsq, a1 = filter[1], a2 = filter[2], a3 = filter[3];
    // The k loops below are unrolled by pragma because -O2 leaves them as loops, which keeps
    // val[] in memory and nearly doubles the cost of each sample; the operations and their order
    // are unchanged.
    double val[4] = { 0, 0, 0, 0 };
    // Line position of the sample being written. CImg walks a pointer one step past either end
    // after the last write of a pass; i only reaches -1 or N in the same places and is never
    // dereferenced there.
    std::ptrdiff_t i = 0;

    if (_order == 0) {
        const double iplus = (_neumann ? data[(N - 1) * off] : 0.f);
        for (int pass = 0; pass < 2; ++pass) {
            if (!pass) {
#pragma GCC unroll 4
                for (int k = 1; k < 4; ++k) {
                    val[k] = (_neumann ? data[i * off] / sumsq : 0);
                }
            } else {
                const double uplus = iplus / (1. - a1 - a2 - a3), vplus = uplus / (1. - a1 - a2 - a3),
                             unp = val[1] - uplus, unp1 = val[2] - uplus, unp2 = val[3] - uplus;
                val[0] = (M[0] * unp + M[1] * unp1 + M[2] * unp2 + vplus) * sum;
                val[1] = (M[3] * unp + M[4] * unp1 + M[5] * unp2 + vplus) * sum;
                val[2] = (M[6] * unp + M[7] * unp1 + M[8] * unp2 + vplus) * sum;
                data[i * off] = (float)val[0];
                --i;
#pragma GCC unroll 4
                for (int k = 3; k > 0; --k) {
                    val[k] = val[k - 1];
                }
            }
            for (int n = pass; n < N; ++n) {
                val[0] = data[i * off];
                if (pass) {
                    val[0] *= sum;
                }
#pragma GCC unroll 4
                for (int k = 1; k < 4; ++k) {
                    val[0] += val[k] * filter[k];
                }
                data[i * off] = (float)val[0];
                if (!pass) {
                    ++i;
                } else {
                    --i;
                }
#pragma GCC unroll 4
                for (int k = 3; k > 0; --k) {
                    val[k] = val[k - 1];
                }
            }
            if (!pass) {
                --i;
            }
        }

        return;
    }

    // Orders 1 to 3 share the Triggs step and differ only in the input difference fed to the
    // recursion. x holds [front, centre, back].
    double x[3];
    for (int pass = 0; pass < 2; ++pass) {
        if (!pass) {
#pragma GCC unroll 4
            for (int k = 0; k < 3; ++k) {
                x[k] = (_neumann ? data[i * off] : 0.f);
            }
#pragma GCC unroll 4
            for (int k = 0; k < 4; ++k) {
                val[k] = 0;
            }
        } else {
            const double unp = val[1], unp1 = val[2], unp2 = val[3];
            val[0] = (M[0] * unp + M[1] * unp1 + M[2] * unp2) * sum;
            val[1] = (M[3] * unp + M[4] * unp1 + M[5] * unp2) * sum;
            val[2] = (M[6] * unp + M[7] * unp1 + M[8] * unp2) * sum;
            data[i * off] = (float)val[0];
            --i;
#pragma GCC unroll 4
            for (int k = 3; k > 0; --k) {
                val[k] = val[k - 1];
            }
        }
        for (int n = pass; n < N - 1; ++n) {
            if (!pass) {
                x[0] = data[(i + 1) * off];
                switch (_order) {
                case 1:
                    val[0] = 0.5f * (x[0] - x[2]);
                    break;
                case 2:
                    val[0] = (x[1] - x[2]);
                    break;
                default:
                    val[0] = (x[0] - 2 * x[1] + x[2]);
                    break;
                }
            } else {
                switch (_order) {
                case 1:
                    val[0] = data[i * off] * sum;
                    break;
                case 2:
                    x[0] = data[(i - 1) * off];
                    val[0] = (x[2] - x[1]) * sum;
                    break;
                default:
                    x[0] = data[(i - 1) * off];
                    val[0] = 0.5f * (x[2] - x[0]) * sum;
                    break;
                }
            }
#pragma GCC unroll 4
            for (int k = 1; k < 4; ++k) {
                val[0] += val[k] * filter[k];
            }
            data[i * off] = (float)val[0];
            if (!pass) {
                ++i;
            } else {
                --i;
            }
#pragma GCC unroll 4
            for (int k = 2; k > 0; --k) {
                x[k] = x[k - 1];
            }
#pragma GCC unroll 4
            for (int k = 3; k > 0; --k) {
                val[k] = val[k - 1];
            }
        }
        // CImg writes one sample before the line here when N is 1; that write is dropped.
        if (i >= 0) {
            data[i * off] = 0.f;
        }
    }
}

namespace {
    inline float
    boxSample(const float* ptr,
              int N,
              std::ptrdiff_t off,
              bool neumann,
              int x)
    {
        if (x < 0) {
            return neumann ? ptr[0] : 0.f;
        }
        if (x >= N) {
            return neumann ? ptr[(N - 1) * off] : 0.f;
        }

        return ptr[x * off];
    }
} // namespace

void
LineFilter::applyBox(float* ptr,
                     int N,
                     std::ptrdiff_t off,
                     LineScratch& scratch) const
{
    const float boxsize = _boxSize;
    const bool neumann = _neumann;

    if ((boxsize > 1) && _iterations) {
        const int w2 = (int)(boxsize - 1) / 2;
        const unsigned int winsize = 2 * w2 + 1U;
        const double frac = (boxsize - winsize) / 2.;
        if (scratch.window.size() < winsize) {
            scratch.window.resize(winsize);
        }
        float* win = scratch.window.data();
        for (unsigned int iter = 0; iter < _iterations; ++iter) {
            double sum = 0;
            for (int x = -w2; x <= w2; ++x) {
                win[x + w2] = boxSample(ptr, N, off, neumann, x);
                sum += win[x + w2];
            }
            int ifirst = 0, ilast = 2 * w2;
            float prev = boxSample(ptr, N, off, neumann, -w2 - 1), next = boxSample(ptr, N, off, neumann, w2 + 1);
            for (int x = 0; x < N - 1; ++x) {
                const double sum2 = sum + frac * (prev + next);
                ptr[x * off] = (float)(sum2 / boxsize);
                prev = win[ifirst];
                sum -= prev;
                ifirst = (int)((ifirst + 1) % winsize);
                ilast = (int)((ilast + 1) % winsize);
                win[ilast] = next;
                sum += next;
                next = boxSample(ptr, N, off, neumann, x + w2 + 2);
            }
            const double sum2 = sum + frac * (prev + next);
            ptr[(N - 1) * off] = (float)(sum2 / boxsize);
        }
    }

    switch (_order) {
    case 1: {
        float p = boxSample(ptr, N, off, neumann, -1), c = boxSample(ptr, N, off, neumann, 0),
              n = boxSample(ptr, N, off, neumann, 1);
        for (int x = 0; x < N - 1; ++x) {
            ptr[x * off] = (float)((n - p) / 2.);
            p = c;
            c = n;
            n = boxSample(ptr, N, off, neumann, x + 2);
        }
        ptr[(N - 1) * off] = (float)((n - p) / 2.);
        break;
    }
    case 2: {
        float p = boxSample(ptr, N, off, neumann, -1), c = boxSample(ptr, N, off, neumann, 0),
              n = boxSample(ptr, N, off, neumann, 1);
        for (int x = 0; x < N - 1; ++x) {
            ptr[x * off] = (float)(n - 2 * c + p);
            p = c;
            c = n;
            n = boxSample(ptr, N, off, neumann, x + 2);
        }
        ptr[(N - 1) * off] = (float)(n - 2 * c + p);
        break;
    }
    default:
        break;
    }
}

void
deriche(float* data,
        int n,
        std::ptrdiff_t stride,
        float sigma,
        unsigned int order,
        bool neumann)
{
    LineFilter::deriche(sigma, order, neumann).apply(data, n, stride);
}

void
vanVliet(float* data,
         int n,
         std::ptrdiff_t stride,
         float sigma,
         unsigned int order,
         bool neumann)
{
    LineFilter::vanVliet(sigma, order, neumann).apply(data, n, stride);
}

void
boxFilter(float* data,
          int n,
          std::ptrdiff_t stride,
          float boxSize,
          int order,
          bool neumann,
          unsigned int iterations)
{
    LineFilter::box(boxSize, order, neumann, iterations).apply(data, n, stride);
}
} // namespace BlurKernels

NATRON_NAMESPACE_EXIT
