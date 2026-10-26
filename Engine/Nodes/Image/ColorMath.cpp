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

#include "ColorMath.h"

#include "Engine/AppInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Project.h"
#include "Engine/ProjectColorManagement.h"

NATRON_NAMESPACE_ENTER

namespace ColorMath {
namespace {

    bool
    invert3(const double* m,
            double* o)
    {
        const double c0 = m[4] * m[8] - m[5] * m[7];
        const double c1 = m[5] * m[6] - m[3] * m[8];
        const double c2 = m[3] * m[7] - m[4] * m[6];
        const double det = m[0] * c0 + m[1] * c1 + m[2] * c2;

        if (!std::isfinite(det) || (std::fabs(det) < 1e-12)) {
            return false;
        }
        const double inv = 1. / det;
        o[0] = c0 * inv;
        o[1] = (m[2] * m[7] - m[1] * m[8]) * inv;
        o[2] = (m[1] * m[5] - m[2] * m[4]) * inv;
        o[3] = c1 * inv;
        o[4] = (m[0] * m[8] - m[2] * m[6]) * inv;
        o[5] = (m[2] * m[3] - m[0] * m[5]) * inv;
        o[6] = c2 * inv;
        o[7] = (m[1] * m[6] - m[0] * m[7]) * inv;
        o[8] = (m[0] * m[4] - m[1] * m[3]) * inv;

        return true;
    }

    void
    mul3v(const double* m,
          const double* v,
          double* o)
    {
        for (int r = 0; r < 3; ++r) {
            o[r] = m[r * 3] * v[0] + m[r * 3 + 1] * v[1] + m[r * 3 + 2] * v[2];
        }
    }

    // o = a·b, row-major.
    void
    mul33(const double* a,
          const double* b,
          double* o)
    {
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                o[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
            }
        }
    }

} // anonymous namespace

void
workingToXYZ(const KnobHolder* holder,
             double m[9])
{
    AppInstancePtr app = holder ? holder->getApp() : AppInstancePtr();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    ProjectColorManagementPtr cm = project ? project->getColorManagement() : ProjectColorManagementPtr();

    if (cm && cm->getWorkingToXYZ(m)) {
        return;
    }
    const double* fallback = acesCgToXYZ();
    for (int i = 0; i < 9; ++i) {
        m[i] = fallback[i];
    }
}

ColorspaceSide
colorspaceSide(ColorspaceModelEnum model)
{
    ColorspaceSide s = {};

    s.model = model;
    double kr;
    double kb;
    switch (model) {
    case eColorspaceModelYPbPr601:
    case eColorspaceModelYCbCr601:
    case eColorspaceModelYUV601:
        kr = 0.299;
        kb = 0.114;
        break;
    case eColorspaceModelYPbPr709:
    case eColorspaceModelYCbCr709:
    case eColorspaceModelYUV709:
        kr = 0.2126;
        kb = 0.0722;
        break;
    case eColorspaceModelYPbPr2020:
    case eColorspaceModelYCbCr2020:
        kr = 0.2627;
        kb = 0.0593;
        break;
    default:
        return s;
    }
    double yScale = 1.;
    double yOffset = 0.;
    double cOffset = 0.;
    double cbScale = 1.;
    double crScale = 1.;
    if ((model == eColorspaceModelYCbCr601) || (model == eColorspaceModelYCbCr709) || (model == eColorspaceModelYCbCr2020)) {
        // Studio range on a 0 to 1 scale: Y' from 16 to 235 and Cb, Cr from 16 to 240 of 255.
        yScale = 219. / 255.;
        yOffset = 16. / 255.;
        cOffset = 128. / 255.;
        cbScale = 224. / 255.;
        crScale = 224. / 255.;
    } else if ((model == eColorspaceModelYUV601) || (model == eColorspaceModelYUV709)) {
        // U and V span ±0.436 and ±0.615 where Pb and Pr span ±0.5.
        cbScale = 2. * 0.436;
        crScale = 2. * 0.615;
    }
    const double kg = 1. - kr - kb;
    const double cb = cbScale * 0.5 / (1. - kb);
    const double cr = crScale * 0.5 / (1. - kr);
    s.kr = (float)kr;
    s.kg = (float)kg;
    s.kb = (float)kb;
    s.kgInv = (float)(1. / kg);
    s.yScale = (float)yScale;
    s.yOffset = (float)yOffset;
    s.yInv = (float)(1. / yScale);
    s.cOffset = (float)cOffset;
    s.cb = (float)cb;
    s.cr = (float)cr;
    s.cbInv = (float)(1. / cb);
    s.crInv = (float)(1. / cr);

    return s;
}

ColorspaceColorimetry
colorspaceColorimetry(const double toXYZ[9])
{
    const double* m = toXYZ;
    double inverse[9];

    if (!invert3(m, inverse)) {
        m = acesCgToXYZ();
        invert3(m, inverse);
    }
    ColorspaceColorimetry c = {};
    for (int i = 0; i < 9; ++i) {
        c.toXYZ[i] = (float)m[i];
        c.fromXYZ[i] = (float)inverse[i];
    }
    double white[3];
    for (int r = 0; r < 3; ++r) {
        white[r] = m[r * 3] + m[r * 3 + 1] + m[r * 3 + 2];
        c.white[r] = (float)white[r];
        c.whiteInv[r] = (float)(1. / white[r]);
    }
    const double sum = white[0] + white[1] + white[2];
    c.whiteXy[0] = (float)(white[0] / sum);
    c.whiteXy[1] = (float)(white[1] / sum);

    return c;
}

OkLabBasis
okLabBasis(const double toXYZ[9])
{
    static const double bradford[9] = {
        0.8951, 0.2664, -0.1614,
        -0.7502, 1.7135, 0.0367,
        0.0389, -0.0685, 1.0296
    };
    static const double m1[9] = {
        0.8189330101, 0.3618667424, -0.1288597137,
        0.0329845436, 0.9293118715, 0.0361456387,
        0.0482003018, 0.2643662691, 0.6338517070
    };
    static const double d65[3] = { 0.95047, 1.0, 1.08883 };
    const double* m = toXYZ;
    double inverse[9];

    if (!invert3(m, inverse)) {
        m = acesCgToXYZ();
    }
    double white[3];
    for (int r = 0; r < 3; ++r) {
        white[r] = m[r * 3] + m[r * 3 + 1] + m[r * 3 + 2];
    }
    double coneWhite[3];
    double coneD65[3];
    mul3v(bradford, white, coneWhite);
    mul3v(bradford, d65, coneD65);
    // Von Kries scaling in Bradford's cone space carries the working white onto D65 exactly,
    // luminance included, so the working white has OkLab lightness 1.
    double scaled[9];
    for (int r = 0; r < 3; ++r) {
        const double k = (coneWhite[r] != 0.) ? (coneD65[r] / coneWhite[r]) : 1.;
        for (int c = 0; c < 3; ++c) {
            scaled[r * 3 + c] = bradford[r * 3 + c] * k;
        }
    }
    double bradfordInv[9];
    invert3(bradford, bradfordInv);
    double adapt[9];
    mul33(bradfordInv, scaled, adapt);
    double xyzToLms[9];
    mul33(m1, adapt, xyzToLms);
    double toLms[9];
    mul33(xyzToLms, m, toLms);
    double fromLms[9];
    if (!invert3(toLms, fromLms)) {
        std::fill(fromLms, fromLms + 9, 0.);
    }
    OkLabBasis b = {};
    for (int i = 0; i < 9; ++i) {
        b.toLms[i] = (float)toLms[i];
        b.fromLms[i] = (float)fromLms[i];
    }

    return b;
}

void
keyModelToWorking(KeyModelEnum model,
                  const OkLabBasis& basis,
                  const float* comps,
                  float* rgb)
{
    float v[3] = { comps[0], comps[1], comps[2] };

    if (model == eKeyModelHSV) {
        colorspaceHsvToRgb(v);
    } else if (model == eKeyModelHSL) {
        colorspaceHslToRgb(v);
    } else {
        static double m2Inverse[9];
        static const bool m2Inverted = [] {
            double m2[9];
            for (int i = 0; i < 9; ++i) {
                m2[i] = okLabM2()[i];
            }

            return invert3(m2, m2Inverse);
        }();
        Q_UNUSED(m2Inverted);
        const double angle = 6.283185307179586 * comps[0];
        const double lab[3] = { comps[2], comps[1] * std::cos(angle), comps[1] * std::sin(angle) };
        double lms[3];
        mul3v(m2Inverse, lab, lms);
        for (int i = 0; i < 3; ++i) {
            v[i] = (float)(lms[i] * lms[i] * lms[i]);
        }
        colorspaceMul3(basis.fromLms, v);
    }
    rgb[0] = v[0];
    rgb[1] = v[1];
    rgb[2] = v[2];
}

} // namespace ColorMath

NATRON_NAMESPACE_EXIT
