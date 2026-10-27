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
 * (commit 20891f6c6fa7), src/iop/ashift.c, with the bilateral grid of src/common/bilateral.c and
 * the Lab conversions of src/common/colorspaces_inline_conversions.h, Copyright (C) 2016-2026
 * darktable developers, distributed under the GNU GPL version 3 or later.
 */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "AshiftLines.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <mutex>
#include <random>
#include <utility>

NATRON_NAMESPACE_ENTER

namespace Ashift {

namespace {
    constexpr double kLsdGamma = 0.45;
    constexpr double kMinLineLength = 5.;
    constexpr double kMaxTangentialDeviation = 30.;
    constexpr int kRansacRuns = 400;
    constexpr double kRansacEpsilon = 2.;
    constexpr double kRansacEpsilonStep = 1.;
    constexpr double kRansacEliminationRatio = 60.;
    constexpr int kRansacOptimizationSteps = 5;
    constexpr int kRansacOptimizationDryRuns = 50;
    constexpr int kRansacHurdle = 5;
    constexpr int kBilateralMaxResS = 3000;
    constexpr int kBilateralMaxResR = 50;

    std::mutex gDetectorMutex;
    SegmentDetector gDetector;

    const double kD50[3] = { 0.9642, 1.0, 0.8249 };
    // darktable's sRGB_to_xyz_transposed and xyz_to_srgb_transposed, as row-major matrices.
    const double kRgbToXyz[3][3] = { { 0.4360747, 0.3850649, 0.1430804 },
                                     { 0.2225045, 0.7168786, 0.0606169 },
                                     { 0.0139322, 0.0971045, 0.7141733 } };
    const double kXyzToRgb[3][3] = { { 3.1338561, -1.6168667, -0.4906146 },
                                     { -0.9787684, 1.9161415, 0.0334540 },
                                     { 0.0719453, -0.2289914, 1.4052427 } };

    void
    xyzToLab(const double* xyz,
             double* lab)
    {
        const double epsilon = 216.0 / 24389.0;
        const double kappa = 24389.0 / 27.0;
        double f[3];

        for (int i = 0; i < 3; ++i) {
            const double x = xyz[i] / kD50[i];
            f[i] = (x > epsilon) ? std::cbrt(x) : (kappa * x + 16.0) / 116.0;
        }
        lab[0] = 116.0 * f[1] - 16.0;
        lab[1] = 500.0 * (f[0] - f[1]);
        lab[2] = -200.0 * (f[2] - f[1]);
    }

    double
    labFInv(double x)
    {
        const double epsilon = 0.20689655172413796;
        const double kappa = 24389.0 / 27.0;

        return (x > epsilon) ? x * x * x : (116.0 * x - 16.0) / kappa;
    }

    void
    labToXyz(const double* lab,
             double* xyz)
    {
        const double fy = (lab[0] + 16.0) / 116.0;
        const double fx = lab[1] / 500.0 + fy;
        const double fz = -lab[2] / 200.0 + fy;

        xyz[0] = kD50[0] * labFInv(fx);
        xyz[1] = kD50[1] * labFInv(fy);
        xyz[2] = kD50[2] * labFInv(fz);
    }

    void
    apply(const double m[3][3],
          const double* in,
          double* out)
    {
        for (int r = 0; r < 3; ++r) {
            out[r] = m[r][0] * in[0] + m[r][1] * in[1] + m[r][2] * in[2];
        }
    }

    // darktable's bilateral grid on the L channel of a 4-channel Lab buffer, single-threaded.
    class BilateralGrid {
    public:
        BilateralGrid(int width,
                      int height,
                      double sigmaS,
                      double sigmaR)
            : _width(width)
            , _height(height)
        {
            const double range = 100.;
            if (sigmaS < 0.5) {
                sigmaS = 0.5;
            }
            const double x = std::clamp((int)std::round(width / sigmaS), 4, kBilateralMaxResS);
            const double y = std::clamp((int)std::round(height / sigmaS), 4, kBilateralMaxResS);
            const double z = std::clamp((int)std::round(range / sigmaR), 4, kBilateralMaxResR);
            _sigmaS = std::max(height / y, width / x);
            _sigmaR = range / z;
            _sizeX = (int)std::ceil(width / _sigmaS) + 1;
            _sizeY = (int)std::ceil(height / _sigmaS) + 1;
            _sizeZ = (int)std::ceil(range / _sigmaR) + 1;
            _buf.assign((std::size_t)_sizeX * _sizeY * _sizeZ, 0.);
        }

        void splat(const std::vector<float>& in)
        {
            const std::size_t ox = _sizeZ;
            const std::size_t oy = (std::size_t)_sizeX * _sizeZ;
            const std::size_t oz = 1;
            const double sigmaS2 = _sigmaS * _sigmaS;
            const std::size_t offsets[8] = { 0, ox, oy, ox + oy, oz, oz + ox, oz + oy, oz + oy + ox };

            for (int j = 0; j < _height; ++j) {
                for (int i = 0; i < _width; ++i) {
                    const double L = in[4 * ((std::size_t)j * _width + i)];
                    double xf, yf, zf;
                    const std::size_t gi = toGrid(i, j, L, &xf, &yf, &zf);
                    const double contrib[4] = { (1. - xf) * (1. - yf) * 100. / sigmaS2,
                                                xf * (1. - yf) * 100. / sigmaS2,
                                                (1. - xf) * yf * 100. / sigmaS2,
                                                xf * yf * 100. / sigmaS2 };
                    for (int k = 0; k < 4; ++k) {
                        _buf[gi + offsets[k]] += contrib[k] * (1. - zf);
                        _buf[gi + offsets[k + 4]] += contrib[k] * zf;
                    }
                }
            }
        }

        void blur()
        {
            const int ox = _sizeZ;
            const int oy = _sizeX * _sizeZ;
            const int oz = 1;

            blurLine(oz, oy, ox, _sizeZ, _sizeY, _sizeX);
            blurLine(oz, ox, oy, _sizeZ, _sizeX, _sizeY);
            blurLineZ(ox, oy, oz, _sizeX, _sizeY, _sizeZ);
        }

        void sliceToOutput(std::vector<float>* out,
                           double detail) const
        {
            const double norm = -detail * _sigmaR * 0.04;
            const std::size_t ox = _sizeZ;
            const std::size_t oy = (std::size_t)_sizeX * _sizeZ;
            const std::size_t oz = 1;

            for (int j = 0; j < _height; ++j) {
                for (int i = 0; i < _width; ++i) {
                    const std::size_t index = 4 * ((std::size_t)j * _width + i);
                    const double L = (*out)[index];
                    double xf, yf, zf;
                    const std::size_t gi = toGrid(i, j, L, &xf, &yf, &zf);
                    const double Lout = norm * (_buf[gi] * (1. - xf) * (1. - yf) * (1. - zf) + _buf[gi + ox] * xf * (1. - yf) * (1. - zf) + _buf[gi + oy] * (1. - xf) * yf * (1. - zf) + _buf[gi + ox + oy] * xf * yf * (1. - zf) + _buf[gi + oz] * (1. - xf) * (1. - yf) * zf + _buf[gi + ox + oz] * xf * (1. - yf) * zf + _buf[gi + oy + oz] * (1. - xf) * yf * zf + _buf[gi + ox + oy + oz] * xf * yf * zf);
                    (*out)[index] = (float)std::max(0., L + Lout);
                }
            }
        }

    private:
        std::size_t toGrid(int i,
                           int j,
                           double L,
                           double* xf,
                           double* yf,
                           double* zf) const
        {
            const double x = std::clamp(i / _sigmaS, 0., (double)(_sizeX - 1));
            const double y = std::clamp(j / _sigmaS, 0., (double)(_sizeY - 1));
            const double z = std::clamp(L / _sigmaR, 0., (double)(_sizeZ - 1));
            const int xi = std::min((int)x, _sizeX - 2);
            const int yi = std::min((int)y, _sizeY - 2);
            const int zi = std::min((int)z, _sizeZ - 2);

            *xf = x - xi;
            *yf = y - yi;
            *zf = z - zi;

            return ((std::size_t)(xi + yi * _sizeX) * _sizeZ) + zi;
        }

        void blurLineZ(int offset1,
                       int offset2,
                       int offset3,
                       int size1,
                       int size2,
                       int size3)
        {
            const double w1 = 4. / 16.;
            const double w2 = 2. / 16.;
            double* buf = _buf.data();

            for (int k = 0; k < size1; ++k) {
                std::size_t index = (std::size_t)k * offset1;
                for (int j = 0; j < size2; ++j) {
                    double tmp1 = buf[index];
                    buf[index] = w1 * buf[index + offset3] + w2 * buf[index + 2 * offset3];
                    index += offset3;
                    double tmp2 = buf[index];
                    buf[index] = w1 * (buf[index + offset3] - tmp1) + w2 * buf[index + 2 * offset3];
                    index += offset3;
                    for (int i = 2; i < size3 - 2; ++i) {
                        const double tmp3 = buf[index];
                        buf[index] = w1 * (buf[index + offset3] - tmp2) + w2 * (buf[index + 2 * offset3] - tmp1);
                        index += offset3;
                        tmp1 = tmp2;
                        tmp2 = tmp3;
                    }
                    const double tmp3 = buf[index];
                    buf[index] = w1 * (buf[index + offset3] - tmp2) - w2 * tmp1;
                    index += offset3;
                    buf[index] = -w1 * tmp3 - w2 * tmp2;
                    index += offset3;
                    index += offset2 - offset3 * size3;
                }
            }
        }

        void blurLine(int offset1,
                      int offset2,
                      int offset3,
                      int size1,
                      int size2,
                      int size3)
        {
            const double w0 = 6. / 16.;
            const double w1 = 4. / 16.;
            const double w2 = 1. / 16.;
            double* buf = _buf.data();

            for (int k = 0; k < size1; ++k) {
                std::size_t index = (std::size_t)k * offset1;
                for (int j = 0; j < size2; ++j) {
                    double tmp1 = buf[index];
                    buf[index] = buf[index] * w0 + w1 * buf[index + offset3] + w2 * buf[index + 2 * offset3];
                    index += offset3;
                    double tmp2 = buf[index];
                    buf[index] = buf[index] * w0 + w1 * (buf[index + offset3] + tmp1) + w2 * buf[index + 2 * offset3];
                    index += offset3;
                    for (int i = 2; i < size3 - 2; ++i) {
                        const double tmp3 = buf[index];
                        buf[index] = buf[index] * w0 + w1 * (buf[index + offset3] + tmp2) + w2 * (buf[index + 2 * offset3] + tmp1);
                        index += offset3;
                        tmp1 = tmp2;
                        tmp2 = tmp3;
                    }
                    const double tmp3 = buf[index];
                    buf[index] = buf[index] * w0 + w1 * (buf[index + offset3] + tmp2) + w2 * tmp1;
                    index += offset3;
                    buf[index] = buf[index] * w0 + w1 * tmp3 + w2 * tmp2;
                    index += offset3;
                    index += offset2 - offset3 * size3;
                }
            }
        }

        int _width;
        int _height;
        double _sigmaS = 1.;
        double _sigmaR = 1.;
        int _sizeX = 0;
        int _sizeY = 0;
        int _sizeZ = 0;
        std::vector<double> _buf;
    };

    void
    edgeEnhance1d(const std::vector<double>& in,
                  std::vector<double>* outv,
                  int width,
                  int height,
                  bool horizontal)
    {
        static const double hkernel[9] = { 1., 0., -1., 2., 0., -2., 1., 0., -1. };
        static const double vkernel[9] = { 1., 2., 1., 0., 0., 0., -1., -2., -1. };
        const double* kernel = horizontal ? hkernel : vkernel;
        std::vector<double>& out = *outv;

        for (int j = 1; j < height - 1; ++j) {
            for (int i = 1; i < width - 1; ++i) {
                double sum = 0.;
                for (int jj = 0; jj < 3; ++jj) {
                    for (int ii = 0; ii < 3; ++ii) {
                        sum += in[(std::size_t)(j + jj - 1) * width + (i + ii - 1)] * kernel[jj * 3 + ii];
                    }
                }
                out[(std::size_t)j * width + i] = sum;
            }
        }
        for (int j = 0; j < height; ++j) {
            for (int i = 0; i < width; ++i) {
                double val = out[(std::size_t)j * width + i];
                if (j < 1) {
                    val = out[(std::size_t)(1 - j) * width + i];
                } else if (j >= height - 1) {
                    val = out[(std::size_t)(j - 1) * width + i];
                } else if (i < 1) {
                    val = out[(std::size_t)j * width + (1 - i)];
                } else if (i >= width - 1) {
                    val = out[(std::size_t)j * width + (i - 1)];
                }
                out[(std::size_t)j * width + i] = val;
                if ((i == 1) && (j >= 1) && (j < height - 1)) {
                    i = width - 1;
                }
            }
        }
    }

    int
    factorial(int n)
    {
        return (n <= 1) ? 1 : n * factorial(n - 1);
    }

    bool
    quickperm(std::vector<int>* a,
              std::vector<int>* p,
              int N,
              int* i)
    {
        if (*i >= N) {
            return false;
        }
        (*p)[*i]--;
        const int j = ((*i % 2) == 1) ? (*p)[*i] : 0;
        std::swap((*a)[j], (*a)[*i]);
        *i = 1;
        while ((*p)[*i] == 0) {
            (*p)[*i] = *i;
            (*i)++;
        }

        return true;
    }

    void
    ransac(const std::vector<Line>& lines,
           std::vector<int>* indexSet,
           std::vector<int>* inoutSet,
           int setCount,
           double totalWeight,
           int xmin,
           int xmax,
           int ymin,
           int ymax,
           std::mt19937* rng)
    {
        if (setCount < 3) {
            return;
        }
        std::vector<int>& index = *indexSet;
        std::vector<int> bestSet(index.begin(), index.begin() + setCount);
        std::vector<int> bestInout(setCount, 0);
        double bestQuality = 0.;
        double epsilon = std::pow(10., -kRansacEpsilon);
        double epsilonStep = kRansacEpsilonStep;
        int linesEliminated = 0;
        int validRuns = 0;
        const int optiruns = kRansacOptimizationSteps * kRansacOptimizationDryRuns;
        const int riter = (setCount > kRansacHurdle) ? kRansacRuns : factorial(setCount);
        std::vector<int> perm(setCount + 1);
        for (int n = 0; n < setCount + 1; ++n) {
            perm[n] = n;
        }
        int piter = 1;
        std::vector<int> inout(setCount, 0);

        for (int r = 0; r < optiruns + riter; ++r) {
            if ((setCount > kRansacHurdle) || (r < optiruns)) {
                for (int i = 0; i < setCount; ++i) {
                    const int j = i + (int)((*rng)() % (unsigned)(setCount - i));
                    std::swap(index[j], index[i]);
                }
            } else {
                (void)quickperm(&index, &perm, setCount, &piter);
            }
            double quality = 0.;
            double V[3];
            vec3prodn(V, lines[index[0]].L, lines[index[1]].L);
            if (vec3isnull(V)
                || ((std::fabs(V[2]) > 0.) && (V[0] / V[2] >= xmin) && (V[1] / V[2] >= ymin) && (V[0] / V[2] <= xmax) && (V[1] / V[2] <= ymax))) {
                quality = 0.;
            } else {
                vec3norm(V, V);
                inout[0] = 1;
                inout[1] = 1;
                for (int n = 2; n < setCount; ++n) {
                    const Line& l3 = lines[index[n]];
                    const double d = std::fabs(vec3scalar(V, l3.L));
                    inout[n] = (d < epsilon) ? 1 : 0;
                    double q;
                    if (inout[n] == 1) {
                        q = 0.33 / (double)setCount + 0.33 * l3.weight / totalWeight
                            + 0.33 * (1. - d / epsilon) * (double)setCount * l3.weight / totalWeight;
                    } else {
                        q = 0.;
                        ++linesEliminated;
                    }
                    quality += q;
                }
                ++validRuns;
            }
            if (r < optiruns) {
                if (((r % kRansacOptimizationDryRuns) == (kRansacOptimizationDryRuns - 1)) && (validRuns > 0)) {
                    const double ratio = 100. * (double)linesEliminated / ((double)setCount * validRuns);
                    if (ratio < kRansacEliminationRatio) {
                        epsilon = std::pow(10., std::log10(epsilon) - epsilonStep);
                    } else if (ratio > kRansacEliminationRatio) {
                        epsilon = std::pow(10., std::log10(epsilon) + epsilonStep);
                    }
                    epsilonStep /= 2.;
                    linesEliminated = 0;
                    validRuns = 0;
                }
            } else if (quality > bestQuality) {
                std::copy(index.begin(), index.begin() + setCount, bestSet.begin());
                bestInout = inout;
                bestQuality = quality;
            }
        }
        std::copy(bestSet.begin(), bestSet.end(), index.begin());
        std::copy(bestInout.begin(), bestInout.end(), inoutSet->begin());
    } // ransac

    void
    makeLine(Line* line,
             double x1,
             double y1,
             double x2,
             double y2)
    {
        line->p1[0] = x1;
        line->p1[1] = y1;
        line->p1[2] = 1.;
        line->p2[0] = x2;
        line->p2[1] = y2;
        line->p2[2] = 1.;
        vec3prodn(line->L, line->p1, line->p2);
        vec3lnorm(line->L, line->L);
        line->length = std::sqrt((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1));
    }
} // anonymous namespace

void
setSegmentDetector(const SegmentDetector& detector)
{
    std::lock_guard<std::mutex> lock(gDetectorMutex);
    gDetector = detector;
}

bool
hasSegmentDetector()
{
    std::lock_guard<std::mutex> lock(gDetectorMutex);

    return (bool)gDetector;
}

void
gammaCorrect(std::vector<float>* rgba)
{
    // darktable applies the gamma to raw files only, whose values are never negative; any
    // scene-linear plate gets it here, negatives clamped first.
    for (std::size_t index = 0; index + 3 < rgba->size(); index += 4) {
        for (int c = 0; c < 3; ++c) {
            const float v = (*rgba)[index + c];
            (*rgba)[index + c] = (float)std::pow(std::max(0.f, v), kLsdGamma);
        }
    }
}

void
detailEnhance(std::vector<float>* rgba,
              int width,
              int height)
{
    const double sigmaR = 5.;
    const double sigmaS = std::min(width, height) * 0.02;
    const double detail = 10.;
    const std::size_t npixels = (std::size_t)width * height;
    std::vector<float>& out = *rgba;

    for (std::size_t p = 0; p < npixels; ++p) {
        const double rgb[3] = { out[4 * p], out[4 * p + 1], out[4 * p + 2] };
        double xyz[3], lab[3];
        apply(kRgbToXyz, rgb, xyz);
        xyzToLab(xyz, lab);
        for (int c = 0; c < 3; ++c) {
            out[4 * p + c] = (float)lab[c];
        }
    }
    BilateralGrid grid(width, height, sigmaS, sigmaR);
    grid.splat(out);
    grid.blur();
    grid.sliceToOutput(&out, detail);
    for (std::size_t p = 0; p < npixels; ++p) {
        const double lab[3] = { out[4 * p], out[4 * p + 1], out[4 * p + 2] };
        double xyz[3], rgb[3];
        labToXyz(lab, xyz);
        apply(kXyzToRgb, xyz, rgb);
        for (int c = 0; c < 3; ++c) {
            out[4 * p + c] = (float)rgb[c];
        }
    }
}

std::vector<double>
rgbToGrey256(const std::vector<float>& rgba,
             int width,
             int height)
{
    const std::size_t npixels = (std::size_t)width * height;
    std::vector<double> grey(npixels);

    for (std::size_t p = 0; p < npixels; ++p) {
        grey[p] = (0.3f * rgba[4 * p] + 0.59f * rgba[4 * p + 1] + 0.11f * rgba[4 * p + 2]) * 256.0;
    }

    return grey;
}

void
edgeEnhance(std::vector<double>* grey,
            int width,
            int height)
{
    if ((width < 3) || (height < 3)) {
        return;
    }
    std::vector<double> gx(grey->size(), 0.), gy(grey->size(), 0.);

    edgeEnhance1d(*grey, &gx, width, height, true);
    edgeEnhance1d(*grey, &gy, width, height, false);
    for (std::size_t k = 0; k < grey->size(); ++k) {
        (*grey)[k] = std::sqrt(gx[k] * gx[k] + gy[k] * gy[k]);
    }
}

bool
detectLines(std::vector<float> rgba,
            int width,
            int height,
            double xScale,
            int enhance,
            const SegmentDetector& detectorArg,
            LineSet* result,
            std::string* error)
{
    SegmentDetector detector = detectorArg;
    if (!detector) {
        std::lock_guard<std::mutex> lock(gDetectorMutex);
        detector = gDetector;
    }
    if (!detector) {
        *error = "no line segment detector is available";

        return false;
    }
    gammaCorrect(&rgba);
    if (enhance & eEnhanceDetail) {
        detailEnhance(&rgba, width, height);
    }
    std::vector<double> grey = rgbToGrey256(rgba, width, height);
    if (enhance & eEnhanceEdges) {
        edgeEnhance(&grey, width, height);
    }
    const std::vector<Segment> segments = detector(grey, width, height);

    LineSet set;
    set.width = (int)std::lround(width * xScale);
    set.height = height;
    for (const Segment& s : segments) {
        const double y1 = s.y1, y2 = s.y2;
        if (((std::fabs(s.x1 - s.x2) < 1) && (std::max(s.x1, s.x2) < 2))
            || ((std::fabs(s.x1 - s.x2) < 1) && (std::min(s.x1, s.x2) > width - 3))
            || ((std::fabs(y1 - y2) < 1) && (std::max(y1, y2) < 2))
            || ((std::fabs(y1 - y2) < 1) && (std::min(y1, y2) > height - 3))) {
            continue;
        }
        const double x1 = s.x1 * xScale;
        const double x2 = s.x2 * xScale;
        Line line;
        makeLine(&line, x1, y1, x2, y2);
        line.width = s.width;
        line.weight = line.length * line.width * s.p;
        const double angle = std::atan2(y2 - y1, x2 - x1) * 180. / M_PI;
        const bool vertical = std::fabs(std::fabs(angle) - 90.) < kMaxTangentialDeviation;
        const bool horizontal = std::fabs(std::fabs(std::fabs(angle) - 90.) - 90.) < kMaxTangentialDeviation;
        const bool relevant = line.length > kMinLineLength;
        line.type = eLineIrrelevant;
        if (vertical && relevant) {
            line.type = eLineVerticalSelected;
            ++set.verticalCount;
            set.verticalWeight += line.weight;
        } else if (horizontal && relevant) {
            line.type = eLineHorizontalSelected;
            ++set.horizontalCount;
            set.horizontalWeight += line.weight;
        }
        set.lines.push_back(line);
    }
    if (set.lines.empty()) {
        *error = "could not detect structural data in image";

        return false;
    }
    *result = set;

    return true;
} // detectLines

void
removeOutliers(LineSet* set,
               unsigned int seed)
{
    const int xmin = 0;
    const int ymin = 0;
    const int xmax = set->width;
    const int ymax = set->height;
    const int count = (int)set->lines.size();

    if (count < 1) {
        return;
    }
    std::mt19937 rng(seed);
    std::vector<int> linesSet(count), inoutSet(count);
    const int dirs[2] = { eLineVerticalSelected, eLineHorizontalSelected };
    for (int d = 0; d < 2; ++d) {
        int nb = 0;
        for (int n = 0; n < count; ++n) {
            if ((set->lines[n].type & eLineMask) != dirs[d]) {
                continue;
            }
            linesSet[nb] = n;
            inoutSet[nb] = 0;
            ++nb;
        }
        if (nb > 2) {
            ransac(set->lines, &linesSet, &inoutSet, nb, (d == 0) ? set->verticalWeight : set->horizontalWeight, xmin, xmax, ymin, ymax, &rng);
        }
        int selected = 0;
        for (int n = 0; n < nb; ++n) {
            Line& line = set->lines[linesSet[n]];
            if (inoutSet[n] == 1) {
                line.type |= eLineSelected;
                ++selected;
            } else {
                line.type &= ~eLineSelected;
            }
        }
        if (d == 0) {
            set->verticalCount = selected;
        } else {
            set->horizontalCount = selected;
        }
    }
}

LineSet
quadLines(const double pts[8],
          int width,
          int height)
{
    LineSet set;

    set.lines.resize(4);
    makeLine(&set.lines[0], pts[0], pts[1], pts[2], pts[3]);
    makeLine(&set.lines[1], pts[4], pts[5], pts[6], pts[7]);
    makeLine(&set.lines[2], pts[0], pts[1], pts[4], pts[5]);
    makeLine(&set.lines[3], pts[2], pts[3], pts[6], pts[7]);
    for (Line& line : set.lines) {
        line.width = 1.;
        line.weight = 1.;
        line.type = (std::fabs(line.p1[0] - line.p2[0]) > std::fabs(line.p1[1] - line.p2[1])) ? eLineHorizontalSelected : eLineVerticalSelected;
    }
    set.width = width;
    set.height = height;
    set.verticalCount = 2;
    set.horizontalCount = 2;
    set.verticalWeight = 2.;
    set.horizontalWeight = 2.;

    return set;
}

} // namespace Ashift

NATRON_NAMESPACE_EXIT
