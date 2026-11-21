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

#include "Global/Macros.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/RectI.h"
#include "Engine/RotoContextPrivate.h"

NATRON_NAMESPACE_USING

namespace {

const RectI kRoi(10, -20, 330, 190);

std::vector<RotoContextPrivate::StrokeDab>
makeDabs()
{
    std::vector<RotoContextPrivate::StrokeDab> dabs;
    for (int i = 0; i < 160; ++i) {
        RotoContextPrivate::StrokeDab dab;
        const double t = i / 159.;
        dab.center.x = 30. + 270. * t;
        dab.center.y = 80. + 70. * std::sin(t * 9.) + 0.37 * i;
        dab.pressure = 0.2 + 0.8 * std::fabs(std::sin(t * 5.));
        dab.externalRadius = 3. + 14. * dab.pressure;
        dab.internalRadius = 0.5 * dab.externalRadius;
        if (i % 3 != 0) {
            for (int s = 0; s <= 8; ++s) {
                const double d = s / 8.;
                dab.opacityStops.push_back(std::make_pair(d, (1. - d * d) * 0.8 * dab.pressure));
            }
        }
        dabs.push_back(dab);
    }

    return dabs;
}

std::vector<RectI>
makeBands(int rowsPerBand)
{
    std::vector<RectI> bands;
    for (int y = kRoi.y1; y < kRoi.y2; y += rowsPerBand) {
        bands.push_back(RectI(kRoi.x1, y, kRoi.x2, std::min(y + rowsPerBand, kRoi.y2)));
    }

    return bands;
}

std::vector<unsigned char>
render(cairo_format_t format,
       bool doBuildup,
       const std::vector<RectI>& bands,
       int nThreads)
{
    const int stride = cairo_format_stride_for_width(format, kRoi.width());
    std::vector<unsigned char> pixels((std::size_t)stride * (std::size_t)kRoi.height(), 0);
    RenderCancellation cancel(std::function<bool()>([] { return false; }));
    EXPECT_TRUE(RotoContextPrivate::renderStrokeDabBands(&pixels.front(), stride, format, kRoi, bands, makeDabs(), doBuildup, 0.7, nThreads, cancel));

    return pixels;
}

void
expectSameForAnySplit(cairo_format_t format,
                      bool doBuildup)
{
    const std::vector<unsigned char> whole = render(format, doBuildup, makeBands(kRoi.height()), 1);
    bool anyInk = false;
    for (std::size_t i = 0; i < whole.size(); ++i) {
        anyInk = anyInk || (whole[i] != 0);
    }
    EXPECT_TRUE(anyInk);
    const int splits[] = { 1, 7, 16, 53 };
    for (int split : splits) {
        const std::vector<unsigned char> banded = render(format, doBuildup, makeBands(split), 8);
        ASSERT_EQ(whole.size(), banded.size());
        EXPECT_EQ(0, std::memcmp(&whole.front(), &banded.front(), whole.size())) << split << " rows per band";
    }
}

} // namespace

TEST(RotoStrokeBands, BuildUpBandsMatchOneBand)
{
    expectSameForAnySplit(CAIRO_FORMAT_A8, true);
}

TEST(RotoStrokeBands, BuildUpRgbaBandsMatchOneBand)
{
    expectSameForAnySplit(CAIRO_FORMAT_ARGB32, true);
}

TEST(RotoStrokeBands, LightenBandsMatchOneBand)
{
    expectSameForAnySplit(CAIRO_FORMAT_ARGB32, false);
}

TEST(RotoStrokeBands, AnAbortedRenderReportsIt)
{
    const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_A8, kRoi.width());
    std::vector<unsigned char> pixels((std::size_t)stride * (std::size_t)kRoi.height(), 0);
    RenderCancellation cancel(std::function<bool()>([] { return true; }));
    EXPECT_FALSE(RotoContextPrivate::renderStrokeDabBands(&pixels.front(), stride, CAIRO_FORMAT_A8, kRoi, makeBands(16), makeDabs(), true, 1., 4, cancel));
}
