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
 * Ported from darktable, DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7): the temperature and
 * tint conversions of src/iop/temperature.c (Copyright (C) 2009-2026 darktable developers), the
 * camera matrix helpers of src/common/colorspaces.c (dcraw's pseudoinverse() and
 * cam_xyz_coeff()), and the CIE tables of src/external/cie_colorimetric_tables.c, from the CIE
 * 015:2004 selected colorimetric tables. darktable is distributed under the GNU GPL version 3 or
 * later, so this file is too. whitePointFromTemperature() is Little CMS's
 * cmsWhitePointFromTemp() (Copyright (c) 1998-2023 Marti Maria Saguer, MIT licence), which
 * darktable calls.
 */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "RawColorMath.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

NATRON_NAMESPACE_ENTER

namespace RawColorMath {
namespace {
    struct CieObserverEntry {
        unsigned long wavelength;
        double xyz[3];
    };

    struct CieDaylightEntry {
        unsigned long wavelength;
        double S[3];
    };

    const CieObserverEntry kCieObserver[] = {
        { 380, { 0.001368, 0.000039, 0.006450 } },
        { 385, { 0.002236, 0.000064, 0.010550 } },
        { 390, { 0.004243, 0.000120, 0.020050 } },
        { 395, { 0.007650, 0.000217, 0.036210 } },
        { 400, { 0.014310, 0.000396, 0.067850 } },
        { 405, { 0.023190, 0.000640, 0.110200 } },
        { 410, { 0.043510, 0.001210, 0.207400 } },
        { 415, { 0.077630, 0.002180, 0.371300 } },
        { 420, { 0.134380, 0.004000, 0.645600 } },
        { 425, { 0.214770, 0.007300, 1.039050 } },
        { 430, { 0.283900, 0.011600, 1.385600 } },
        { 435, { 0.328500, 0.016840, 1.622960 } },
        { 440, { 0.348280, 0.023000, 1.747060 } },
        { 445, { 0.348060, 0.029800, 1.782600 } },
        { 450, { 0.336200, 0.038000, 1.772110 } },
        { 455, { 0.318700, 0.048000, 1.744100 } },
        { 460, { 0.290800, 0.060000, 1.669200 } },
        { 465, { 0.251100, 0.073900, 1.528100 } },
        { 470, { 0.195360, 0.090980, 1.287640 } },
        { 475, { 0.142100, 0.112600, 1.041900 } },
        { 480, { 0.095640, 0.139020, 0.812950 } },
        { 485, { 0.057950, 0.169300, 0.616200 } },
        { 490, { 0.032010, 0.208020, 0.465180 } },
        { 495, { 0.014700, 0.258600, 0.353300 } },
        { 500, { 0.004900, 0.323000, 0.272000 } },
        { 505, { 0.002400, 0.407300, 0.212300 } },
        { 510, { 0.009300, 0.503000, 0.158200 } },
        { 515, { 0.029100, 0.608200, 0.111700 } },
        { 520, { 0.063270, 0.710000, 0.078250 } },
        { 525, { 0.109600, 0.793200, 0.057250 } },
        { 530, { 0.165500, 0.862000, 0.042160 } },
        { 535, { 0.225750, 0.914850, 0.029840 } },
        { 540, { 0.290400, 0.954000, 0.020300 } },
        { 545, { 0.359700, 0.980300, 0.013400 } },
        { 550, { 0.433450, 0.994950, 0.008750 } },
        { 555, { 0.512050, 1.000000, 0.005750 } },
        { 560, { 0.594500, 0.995000, 0.003900 } },
        { 565, { 0.678400, 0.978600, 0.002750 } },
        { 570, { 0.762100, 0.952000, 0.002100 } },
        { 575, { 0.842500, 0.915400, 0.001800 } },
        { 580, { 0.916300, 0.870000, 0.001650 } },
        { 585, { 0.978600, 0.816300, 0.001400 } },
        { 590, { 1.026300, 0.757000, 0.001100 } },
        { 595, { 1.056700, 0.694900, 0.001000 } },
        { 600, { 1.062200, 0.631000, 0.000800 } },
        { 605, { 1.045600, 0.566800, 0.000600 } },
        { 610, { 1.002600, 0.503000, 0.000340 } },
        { 615, { 0.938400, 0.441200, 0.000240 } },
        { 620, { 0.854450, 0.381000, 0.000190 } },
        { 625, { 0.751400, 0.321000, 0.000100 } },
        { 630, { 0.642400, 0.265000, 0.000050 } },
        { 635, { 0.541900, 0.217000, 0.000030 } },
        { 640, { 0.447900, 0.175000, 0.000020 } },
        { 645, { 0.360800, 0.138200, 0.000010 } },
        { 650, { 0.283500, 0.107000, 0.000000 } },
        { 655, { 0.218700, 0.081600, 0.000000 } },
        { 660, { 0.164900, 0.061000, 0.000000 } },
        { 665, { 0.121200, 0.044580, 0.000000 } },
        { 670, { 0.087400, 0.032000, 0.000000 } },
        { 675, { 0.063600, 0.023200, 0.000000 } },
        { 680, { 0.046770, 0.017000, 0.000000 } },
        { 685, { 0.032900, 0.011920, 0.000000 } },
        { 690, { 0.022700, 0.008210, 0.000000 } },
        { 695, { 0.015840, 0.005723, 0.000000 } },
        { 700, { 0.011359, 0.004102, 0.000000 } },
        { 705, { 0.008111, 0.002929, 0.000000 } },
        { 710, { 0.005790, 0.002091, 0.000000 } },
        { 715, { 0.004109, 0.001484, 0.000000 } },
        { 720, { 0.002899, 0.001047, 0.000000 } },
        { 725, { 0.002049, 0.000740, 0.000000 } },
        { 730, { 0.001440, 0.000520, 0.000000 } },
        { 735, { 0.001000, 0.000361, 0.000000 } },
        { 740, { 0.000690, 0.000249, 0.000000 } },
        { 745, { 0.000476, 0.000172, 0.000000 } },
        { 750, { 0.000332, 0.000120, 0.000000 } },
        { 755, { 0.000235, 0.000085, 0.000000 } },
        { 760, { 0.000166, 0.000060, 0.000000 } },
        { 765, { 0.000117, 0.000042, 0.000000 } },
        { 770, { 0.000083, 0.000030, 0.000000 } },
        { 775, { 0.000059, 0.000021, 0.000000 } },
        { 780, { 0.000042, 0.000015, 0.000000 } },
    };

    const CieDaylightEntry kCieDaylight[] = {
        { 300, { 0.04, 0.02, 0.00 } },
        { 305, { 3.02, 2.26, 1.00 } },
        { 310, { 6.00, 4.50, 2.00 } },
        { 315, { 17.80, 13.45, 3.00 } },
        { 320, { 29.60, 22.40, 4.00 } },
        { 325, { 42.45, 32.20, 6.25 } },
        { 330, { 55.30, 42.00, 8.50 } },
        { 335, { 56.30, 41.30, 8.15 } },
        { 340, { 57.30, 40.60, 7.80 } },
        { 345, { 59.55, 41.10, 7.25 } },
        { 350, { 61.80, 41.60, 6.70 } },
        { 355, { 61.65, 39.80, 6.00 } },
        { 360, { 61.50, 38.00, 5.30 } },
        { 365, { 65.15, 40.20, 5.70 } },
        { 370, { 68.80, 42.40, 6.10 } },
        { 375, { 66.10, 40.45, 4.55 } },
        { 380, { 63.40, 38.50, 3.00 } },
        { 385, { 64.60, 36.75, 2.10 } },
        { 390, { 65.80, 35.00, 1.20 } },
        { 395, { 80.30, 39.20, 0.05 } },
        { 400, { 94.80, 43.40, -1.10 } },
        { 405, { 99.80, 44.85, -0.80 } },
        { 410, { 104.80, 46.30, -0.50 } },
        { 415, { 105.35, 45.10, -0.60 } },
        { 420, { 105.90, 43.90, -0.70 } },
        { 425, { 101.35, 40.50, -0.95 } },
        { 430, { 96.80, 37.10, -1.20 } },
        { 435, { 105.35, 36.90, -1.90 } },
        { 440, { 113.90, 36.70, -2.60 } },
        { 445, { 119.75, 36.30, -2.75 } },
        { 450, { 125.60, 35.90, -2.90 } },
        { 455, { 125.55, 34.25, -2.85 } },
        { 460, { 125.50, 32.60, -2.80 } },
        { 465, { 123.40, 30.25, -2.70 } },
        { 470, { 121.30, 27.90, -2.60 } },
        { 475, { 121.30, 26.10, -2.60 } },
        { 480, { 121.30, 24.30, -2.60 } },
        { 485, { 117.40, 22.20, -2.20 } },
        { 490, { 113.50, 20.10, -1.80 } },
        { 495, { 113.30, 18.15, -1.65 } },
        { 500, { 113.10, 16.20, -1.50 } },
        { 505, { 111.95, 14.70, -1.40 } },
        { 510, { 110.80, 13.20, -1.30 } },
        { 515, { 108.65, 10.90, -1.25 } },
        { 520, { 106.50, 8.60, -1.20 } },
        { 525, { 107.65, 7.35, -1.10 } },
        { 530, { 108.80, 6.10, -1.00 } },
        { 535, { 107.05, 5.15, -0.75 } },
        { 540, { 105.30, 4.20, -0.50 } },
        { 545, { 104.85, 3.05, -0.40 } },
        { 550, { 104.40, 1.90, -0.30 } },
        { 555, { 102.20, 0.95, -0.15 } },
        { 560, { 100.00, 0.00, 0.00 } },
        { 565, { 98.00, -0.80, 0.10 } },
        { 570, { 96.00, -1.60, 0.20 } },
        { 575, { 95.55, -2.55, 0.35 } },
        { 580, { 95.10, -3.50, 0.50 } },
        { 585, { 92.10, -3.50, 1.30 } },
        { 590, { 89.10, -3.50, 2.10 } },
        { 595, { 89.80, -4.65, 2.65 } },
        { 600, { 90.50, -5.80, 3.20 } },
        { 605, { 90.40, -6.50, 3.65 } },
        { 610, { 90.30, -7.20, 4.10 } },
        { 615, { 89.35, -7.90, 4.40 } },
        { 620, { 88.40, -8.60, 4.70 } },
        { 625, { 86.20, -9.05, 4.90 } },
        { 630, { 84.00, -9.50, 5.10 } },
        { 635, { 84.55, -10.20, 5.90 } },
        { 640, { 85.10, -10.90, 6.70 } },
        { 645, { 83.50, -10.80, 7.00 } },
        { 650, { 81.90, -10.70, 7.30 } },
        { 655, { 82.25, -11.35, 7.95 } },
        { 660, { 82.60, -12.00, 8.60 } },
        { 665, { 83.75, -13.00, 9.20 } },
        { 670, { 84.90, -14.00, 9.80 } },
        { 675, { 83.10, -13.80, 10.00 } },
        { 680, { 81.30, -13.60, 10.20 } },
        { 685, { 76.60, -12.80, 9.25 } },
        { 690, { 71.90, -12.00, 8.30 } },
        { 695, { 73.10, -12.65, 8.95 } },
        { 700, { 74.30, -13.30, 9.60 } },
        { 705, { 75.35, -13.10, 9.05 } },
        { 710, { 76.40, -12.90, 8.50 } },
        { 715, { 69.85, -11.75, 7.75 } },
        { 720, { 63.30, -10.60, 7.00 } },
        { 725, { 67.50, -11.10, 7.30 } },
        { 730, { 71.70, -11.60, 7.60 } },
        { 735, { 74.35, -11.90, 7.80 } },
        { 740, { 77.00, -12.20, 8.00 } },
        { 745, { 71.10, -11.20, 7.35 } },
        { 750, { 65.20, -10.20, 6.70 } },
        { 755, { 56.45, -9.00, 5.95 } },
        { 760, { 47.70, -7.80, 5.20 } },
        { 765, { 58.15, -9.50, 6.30 } },
        { 770, { 68.60, -11.20, 7.40 } },
        { 775, { 66.80, -10.80, 7.10 } },
        { 780, { 65.00, -10.40, 6.80 } },
        { 785, { 65.50, -10.50, 6.90 } },
        { 790, { 66.00, -10.60, 7.00 } },
        { 795, { 63.50, -10.15, 6.70 } },
        { 800, { 61.00, -9.70, 6.40 } },
        { 805, { 57.15, -9.00, 5.95 } },
        { 810, { 53.30, -8.30, 5.50 } },
        { 815, { 56.10, -8.80, 5.80 } },
        { 820, { 58.90, -9.30, 6.10 } },
        { 825, { 60.40, -9.55, 6.30 } },
        { 830, { 61.90, -9.80, 6.50 } },
    };

    const std::size_t kCieObserverCount = sizeof(kCieObserver) / sizeof(kCieObserver[0]);

    const double kInitialBlackBodyTemperature = 4000.;
    const double kD65x = 0.31271;
    const double kD65y = 0.32902;

    bool
    whitePointFromTemperature(double tempK,
                              double* x,
                              double* y)
    {
        const double T = tempK;
        const double T2 = T * T;
        const double T3 = T2 * T;
        double wx;

        if ((T >= 4000.) && (T <= 7000.)) {
            wx = -4.6070 * (1E9 / T3) + 2.9678 * (1E6 / T2) + 0.09911 * (1E3 / T) + 0.244063;
        } else if ((T > 7000.0) && (T <= 25000.0)) {
            wx = -2.0064 * (1E9 / T3) + 1.9018 * (1E6 / T2) + 0.24748 * (1E3 / T) + 0.237040;
        } else {
            return false;
        }
        *x = wx;
        *y = -3.000 * (wx * wx) + 2.870 * wx - 0.275;

        return true;
    }

    double
    spdBlackBody(unsigned long wavelength,
                 double tempK)
    {
        const long double lambda = (double)wavelength * 1e-9;
        const long double c1 = 3.7417715246641281639549488324352159753e-16L;
        const long double c2 = 0.014387769599838156481252937624049081933L;

        return (double)(c1 / (powl(lambda, 5) * (expl(c2 / (lambda * tempK)) - 1.0L)));
    }

    double
    spdDaylight(unsigned long wavelength,
                double tempK)
    {
        double x = kD65x;
        double y = kD65y;
        whitePointFromTemperature(tempK, &x, &y);

        const double M = (0.0241 + 0.2562 * x - 0.7341 * y);
        const double m1 = (-1.3515 - 1.7703 * x + 5.9114 * y) / M;
        const double m2 = (0.0300 - 31.4424 * x + 30.0717 * y) / M;

        const unsigned long j = ((wavelength - kCieDaylight[0].wavelength) / (kCieDaylight[1].wavelength - kCieDaylight[0].wavelength));

        return (kCieDaylight[j].S[0] + m1 * kCieDaylight[j].S[1] + m2 * kCieDaylight[j].S[2]);
    }

    Vec3
    spectrumToXyz(double tempK,
                  double (*spd)(unsigned long, double))
    {
        Vec3 source = { 0., 0., 0. };

        for (std::size_t i = 0; i < kCieObserverCount; ++i) {
            const unsigned long lambda = kCieObserver[0].wavelength + (kCieObserver[1].wavelength - kCieObserver[0].wavelength) * i;
            const double P = spd(lambda, tempK);
            source[0] += P * kCieObserver[i].xyz[0];
            source[1] += P * kCieObserver[i].xyz[1];
            source[2] += P * kCieObserver[i].xyz[2];
        }

        const double maximum = std::fmax(std::fmax(source[0], source[1]), source[2]);
        source[0] /= maximum;
        source[1] /= maximum;
        source[2] /= maximum;

        return source;
    }

    // dcraw's pseudoinverse(), as darktable copies it.
    void
    pseudoinverse(const double (*in)[3],
                  double (*out)[3],
                  int size)
    {
        double work[3][6];

        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 6; ++j) {
                work[i][j] = (j == i + 3) ? 1. : 0.;
            }
            for (int j = 0; j < 3; ++j) {
                for (int k = 0; k < size; ++k) {
                    work[i][j] += in[k][i] * in[k][j];
                }
            }
        }
        for (int i = 0; i < 3; ++i) {
            double num = work[i][i];
            for (int j = 0; j < 6; ++j) {
                work[i][j] /= num;
            }
            for (int k = 0; k < 3; ++k) {
                if (k == i) {
                    continue;
                }
                num = work[k][i];
                for (int j = 0; j < 6; ++j) {
                    work[k][j] -= work[i][j] * num;
                }
            }
        }
        for (int i = 0; i < size; ++i) {
            for (int j = 0; j < 3; ++j) {
                out[i][j] = 0.0;
                for (int k = 0; k < 3; ++k) {
                    out[i][j] += work[j][k + 3] * in[i][k];
                }
            }
        }
    }

    Vec3
    xyzToMultipliers(const CameraMatrices& matrices,
                     const Vec3& xyz)
    {
        Vec3 mul;

        for (int k = 0; k < 3; ++k) {
            double cam = 0.0;
            for (int i = 0; i < 3; ++i) {
                cam += matrices.xyzToCam[k][i] * xyz[i];
            }
            mul[k] = 1.0 / cam;
        }

        return mul;
    }

    Vec3
    multipliersToXyz(const CameraMatrices& matrices,
                     const Vec3& coeffs)
    {
        double cam[3];
        for (int k = 0; k < 3; ++k) {
            cam[k] = (coeffs[k] > 0.0) ? (1.0 / coeffs[k]) : 0.0;
        }
        Vec3 xyz;
        for (int k = 0; k < 3; ++k) {
            xyz[k] = 0.0;
            for (int i = 0; i < 3; ++i) {
                xyz[k] += matrices.camToXyz[k][i] * cam[i];
            }
        }

        return xyz;
    }
} // namespace

std::optional<Mat3>
invert(const Mat3& m)
{
    const double c00 = m[4] * m[8] - m[5] * m[7];
    const double c01 = m[5] * m[6] - m[3] * m[8];
    const double c02 = m[3] * m[7] - m[4] * m[6];
    const double det = m[0] * c00 + m[1] * c01 + m[2] * c02;

    if (!std::isfinite(det) || (det == 0.)) {
        return std::nullopt;
    }
    const double inv = 1. / det;

    return Mat3 { c00 * inv,
                  (m[2] * m[7] - m[1] * m[8]) * inv,
                  (m[1] * m[5] - m[2] * m[4]) * inv,
                  c01 * inv,
                  (m[0] * m[8] - m[2] * m[6]) * inv,
                  (m[2] * m[3] - m[0] * m[5]) * inv,
                  c02 * inv,
                  (m[1] * m[6] - m[0] * m[7]) * inv,
                  (m[0] * m[4] - m[1] * m[3]) * inv };
}

Mat3
multiply(const Mat3& a,
         const Mat3& b)
{
    Mat3 out;

    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            double sum = 0.;
            for (int k = 0; k < 3; ++k) {
                sum += a[i * 3 + k] * b[k * 3 + j];
            }
            out[i * 3 + j] = sum;
        }
    }

    return out;
}

Vec3
apply(const Mat3& m,
      const Vec3& v)
{
    return Vec3 { m[0] * v[0] + m[1] * v[1] + m[2] * v[2],
                  m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
                  m[6] * v[0] + m[7] * v[1] + m[8] * v[2] };
}

Mat3
srgbToXyz()
{
    return Mat3 { 0.4124564, 0.3575761, 0.1804375,
                  0.2126729, 0.7151522, 0.0721750,
                  0.0193339, 0.1191920, 0.9503041 };
}

CameraMatrices
cameraMatrices(const Mat3& xyzToCam)
{
    CameraMatrices out;

    for (int i = 0; i < 9; ++i) {
        out.xyzToCam[i / 3][i % 3] = xyzToCam[i];
    }
    for (int i = 0; i < 3; ++i) {
        out.xyzToCam[3][i] = 0.0;
    }
    double inverse[4][3];
    pseudoinverse(out.xyzToCam, inverse, 4);
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j) {
            out.camToXyz[i][j] = inverse[j][i];
        }
    }

    return out;
}

CameraMatrices
srgbCameraMatrices()
{
    CameraMatrices out;
    const double rgbToXyz[3][4] = { { 0.4124564, 0.3575761, 0.1804375, 0 },
                                    { 0.2126729, 0.7151522, 0.0721750, 0 },
                                    { 0.0193339, 0.1191920, 0.9503041, 0 } };
    const double xyzToRgb[4][3] = { { 3.2404542, -1.5371385, -0.4985314 },
                                    { -0.9692660, 1.8760108, 0.0415560 },
                                    { 0.0556434, -0.2040259, 1.0572252 },
                                    { 0, 0, 0 } };

    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 3; ++j) {
            out.xyzToCam[i][j] = xyzToRgb[i][j];
            out.camToXyz[j][i] = rgbToXyz[j][i];
        }
    }

    return out;
}

Vec3
temperatureToXyz(double tempK)
{
    tempK = std::min(std::max(tempK, kLowestTemperature), kHighestTemperature);

    // There is no daylight reference below 4000 K.
    if (tempK < kInitialBlackBodyTemperature) {
        return spectrumToXyz(tempK, spdBlackBody);
    }

    return spectrumToXyz(tempK, spdDaylight);
}

Vec3
multipliersForTemperature(const CameraMatrices& matrices,
                          double tempK,
                          double tint)
{
    Vec3 xyz = temperatureToXyz(tempK);

    // darktable's tint scales Y alone, which is not orthogonal to the Planckian locus.
    xyz[1] /= tint;
    Vec3 mul = xyzToMultipliers(matrices, xyz);
    mul[0] /= mul[1];
    mul[2] /= mul[1];
    mul[1] = 1.0;

    return mul;
}

void
temperatureForMultipliers(const CameraMatrices& matrices,
                          const Vec3& coeffs,
                          float* tempK,
                          float* tint)
{
    const Vec3 XYZ = multipliersToXyz(matrices, coeffs);
    double maxtemp = kHighestTemperature;
    double mintemp = kLowestTemperature;
    Vec3 xyz = { 1., 1., 1. };

    for (*tempK = (float)((maxtemp + mintemp) / 2.0); (maxtemp - mintemp) > 1.0; *tempK = (float)((maxtemp + mintemp) / 2.0)) {
        xyz = temperatureToXyz(*tempK);
        if (xyz[2] / xyz[0] > XYZ[2] / XYZ[0]) {
            maxtemp = *tempK;
        } else {
            mintemp = *tempK;
        }
    }

    *tint = (float)((xyz[1] / xyz[0]) / (XYZ[1] / XYZ[0]));

    *tempK = (float)std::min(std::max((double)*tempK, kLowestTemperature), kHighestTemperature);
    *tint = (float)std::min(std::max((double)*tint, kLowestTint), kHighestTint);
}

std::optional<Vec3>
cameraReferenceMultipliers(const Mat3& xyzToCam)
{
    const Mat3 rgbToXyz = { 0.412453, 0.357580, 0.180423,
                            0.212671, 0.715160, 0.072169,
                            0.019334, 0.119193, 0.950227 };
    const Mat3 rgbToCam = multiply(xyzToCam, rgbToXyz);
    Vec3 mul;

    for (int i = 0; i < 3; ++i) {
        const double num = rgbToCam[i * 3] + rgbToCam[i * 3 + 1] + rgbToCam[i * 3 + 2];
        if (num == 0.) {
            return std::nullopt;
        }
        mul[i] = 1.0 / num;
    }

    return Vec3 { mul[0] / mul[1], 1.0, mul[2] / mul[1] };
}

Vec3
d65White()
{
    return Vec3 { 0.95045471, 1.00000000, 1.08905029 };
}

std::optional<Mat3>
rgbToXyz(const Mat3& primaries,
         const Vec3& white)
{
    // Little CMS's _cmsBuildRGB2XYZtransferMatrix(): each primary at its chromaticity, scaled so
    // RGB (1, 1, 1) is the white's chromaticity at Y = 1.
    Mat3 columns;

    for (int k = 0; k < 3; ++k) {
        const double norm = primaries[k] + primaries[3 + k] + primaries[6 + k];
        if (norm == 0.) {
            return std::nullopt;
        }
        const double x = primaries[k] / norm;
        const double y = primaries[3 + k] / norm;
        if (y == 0.) {
            return std::nullopt;
        }
        columns[k] = x / y;
        columns[3 + k] = 1.;
        columns[6 + k] = (1. - x - y) / y;
    }
    const std::optional<Mat3> inverse = invert(columns);
    const double whiteNorm = white[0] + white[1] + white[2];
    if (!inverse || (whiteNorm == 0.) || (white[1] == 0.)) {
        return std::nullopt;
    }
    const double wx = white[0] / whiteNorm;
    const double wy = white[1] / whiteNorm;
    const Vec3 whiteXyz = { wx / wy, 1., (1. - wx - wy) / wy };
    const Vec3 scale = apply(*inverse, whiteXyz);
    Mat3 out;
    for (int r = 0; r < 3; ++r) {
        for (int k = 0; k < 3; ++k) {
            out[r * 3 + k] = columns[r * 3 + k] * scale[k];
        }
    }

    return out;
}

std::optional<Mat3>
whiteNormalisedCameraToXyz(const Mat3& camToXyz)
{
    return rgbToXyz(camToXyz, d65White());
}

Vec3
lcmsD50White()
{
    return Vec3 { 0.9642, 1.0, 0.8249 };
}

std::optional<Mat3>
bradford(const Vec3& from,
         const Vec3& to)
{
    // Little CMS's ComputeChromaticAdaptation() with its Bradford cone matrix, the linear form
    // without the blue non-linearity.
    const Mat3 cone = { 0.8951, 0.2664, -0.1614,
                        -0.7502, 1.7135, 0.0367,
                        0.0389, -0.0685, 1.0296 };
    const Vec3 coneFrom = apply(cone, from);
    const Vec3 coneTo = apply(cone, to);
    const std::optional<Mat3> coneInverse = invert(cone);

    if (!coneInverse) {
        return std::nullopt;
    }
    Mat3 gain = { 0., 0., 0., 0., 0., 0., 0., 0., 0. };
    for (int c = 0; c < 3; ++c) {
        if (coneFrom[c] == 0.) {
            return std::nullopt;
        }
        gain[c * 4] = coneTo[c] / coneFrom[c];
    }

    return multiply(*coneInverse, multiply(gain, cone));
}

std::optional<Mat3>
profileCameraToXyz(const Mat3& primaries,
                   const Vec3& white)
{
    const std::optional<Mat3> toWhite = rgbToXyz(primaries, white);
    const double whiteNorm = white[0] + white[1] + white[2];

    if (!toWhite || (whiteNorm == 0.) || (white[1] == 0.)) {
        return std::nullopt;
    }
    // cmsCreateRGBProfile() stores the colorants adapted to D50; darktable's working profile
    // takes D50 back to D65 the same way, which is where Natron's XYZ is.
    const Vec3 whiteXyz = { white[0] / white[1], 1., white[2] / white[1] };
    const std::optional<Mat3> toD50 = bradford(whiteXyz, lcmsD50White());
    const std::optional<Mat3> toD65 = bradford(lcmsD50White(), d65White());
    if (!toD50 || !toD65) {
        return std::nullopt;
    }

    return multiply(*toD65, multiply(*toD50, *toWhite));
}
} // namespace RawColorMath

NATRON_NAMESPACE_EXIT
