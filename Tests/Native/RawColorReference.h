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

#ifndef Tests_Native_RawColorReference_h
#define Tests_Native_RawColorReference_h

#include <array>
#include <cmath>

/**
 * @brief Colour space references for the RawInputColor tests, written from the standards rather
 * than from the node's code: RGB to XYZ from primaries and a white, Bradford, the tone curves'
 * inverses (linear to encoded) and CIE Lab to XYZ.
 **/
namespace RawColorReference {
using Mat3 = std::array<double, 9>;
using Vec3 = std::array<double, 3>;

struct Xy {
    double x, y;
};

const Xy kD65 = { 0.31271, 0.32902 };
const Xy kD50 = { 0.34567, 0.35850 };
const Vec3 kD50Xyz = { 0.9642, 1.0, 0.8249 };
const Vec3 kD65Xyz = { 0.95045471, 1.0, 1.08905029 };

inline Mat3
multiply(const Mat3& a,
         const Mat3& b)
{
    Mat3 out;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            out[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
        }
    }

    return out;
}

inline Vec3
applyMatrix(const Mat3& m,
            const Vec3& v)
{
    return { m[0] * v[0] + m[1] * v[1] + m[2] * v[2],
             m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
             m[6] * v[0] + m[7] * v[1] + m[8] * v[2] };
}

inline Mat3
invert(const Mat3& m)
{
    const double det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);

    return { (m[4] * m[8] - m[5] * m[7]) / det, (m[2] * m[7] - m[1] * m[8]) / det, (m[1] * m[5] - m[2] * m[4]) / det,
             (m[5] * m[6] - m[3] * m[8]) / det, (m[0] * m[8] - m[2] * m[6]) / det, (m[2] * m[3] - m[0] * m[5]) / det,
             (m[3] * m[7] - m[4] * m[6]) / det, (m[1] * m[6] - m[0] * m[7]) / det, (m[0] * m[4] - m[1] * m[3]) / det };
}

inline Vec3
xyzOf(const Xy& c)
{
    return { c.x / c.y, 1., (1. - c.x - c.y) / c.y };
}

inline Mat3
rgbToXyz(const Xy& r,
         const Xy& g,
         const Xy& b,
         const Xy& white)
{
    const Vec3 p[3] = { xyzOf(r), xyzOf(g), xyzOf(b) };
    const Mat3 columns = { p[0][0], p[1][0], p[2][0], p[0][1], p[1][1], p[2][1], p[0][2], p[1][2], p[2][2] };
    const Vec3 s = applyMatrix(invert(columns), xyzOf(white));
    Mat3 out = columns;
    for (int i = 0; i < 9; ++i) {
        out[i] *= s[i % 3];
    }

    return out;
}

inline Mat3
bradford(const Vec3& from,
         const Vec3& to)
{
    const Mat3 cone = { 0.8951, 0.2664, -0.1614, -0.7502, 1.7135, 0.0367, 0.0389, -0.0685, 1.0296 };
    const Vec3 a = applyMatrix(cone, from);
    const Vec3 b = applyMatrix(cone, to);
    const Mat3 gain = { b[0] / a[0], 0., 0., 0., b[1] / a[1], 0., 0., 0., b[2] / a[2] };

    return multiply(invert(cone), multiply(gain, cone));
}

inline double
srgbEncode(double v)
{
    return (v <= 0.0031308) ? 12.92 * v : 1.055 * std::pow(v, 1. / 2.4) - 0.055;
}

inline double
rec709Encode(double v)
{
    return (v < 0.018) ? 4.5 * v : 1.099 * std::pow(v, 0.45) - 0.099;
}

inline double
adobeEncode(double v)
{
    return std::pow(v, 1. / (2. + 51. / 256.));
}

inline double
pqEncode(double v)
{
    const double m1 = 2610. / 16384.;
    const double m2 = 2523. / 4096. * 128.;
    const double c1 = 3424. / 4096.;
    const double c2 = 2413. / 4096. * 32.;
    const double c3 = 2392. / 4096. * 32.;
    const double p = std::pow(v, m1);

    return std::pow((c1 + c2 * p) / (1. + c3 * p), m2);
}

// darktable's HLG curve has an offset (beta 0.04) on the encoded side; this is its inverse.
inline double
hlgEncode(double v)
{
    const double a = 0.17883277;
    const double b = 1. - 4. * a;
    const double c = 0.5 - a * std::log(4. * a);
    const double e = (v <= 1. / 12.) ? std::sqrt(3. * v) : a * std::log(12. * v - b) + c;

    return (e - 0.04) / 0.96;
}

inline Vec3
labToXyzD50(const Vec3& lab)
{
    const double fy = (lab[0] + 16.) / 116.;
    const double f[3] = { fy + lab[1] / 500., fy, fy - lab[2] / 200. };
    const double delta = 6. / 29.;
    Vec3 xyz;
    for (int c = 0; c < 3; ++c) {
        const double t = (f[c] > delta) ? f[c] * f[c] * f[c] : 3. * delta * delta * (f[c] - 4. / 29.);
        xyz[c] = kD50Xyz[c] * t;
    }

    return xyz;
}
} // namespace RawColorReference

#endif // Tests_Native_RawColorReference_h
