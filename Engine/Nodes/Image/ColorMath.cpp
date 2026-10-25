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

} // namespace ColorMath

NATRON_NAMESPACE_EXIT
