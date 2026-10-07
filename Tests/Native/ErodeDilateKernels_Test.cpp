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
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Filter/ErodeDilate.h"

#if defined(NATRON_TESTS_CIMG_HEADER)
#if __has_include(NATRON_TESTS_CIMG_HEADER)
#define NATRON_ERODEDILATE_HAVE_CIMG 1
#endif
#endif

#ifdef NATRON_ERODEDILATE_HAVE_CIMG
#define cimg_display 0
#define cimg_verbosity 0
#define cimg_use_openmp 0
#define cimg_namespace_suffix natron_tests
#include NATRON_TESTS_CIMG_HEADER
#endif

NATRON_NAMESPACE_USING

#ifdef NATRON_ERODEDILATE_HAVE_CIMG

namespace {
typedef cimg_library_suffixed::CImg<float> CImgF;

std::vector<float>
randomValues(std::size_t n,
             std::uint32_t seed)
{
    std::vector<float> v(n);
    std::uint32_t state = 0x9e3779b9u + seed;

    for (std::size_t i = 0; i < n; ++i) {
        state = state * 1664525u + 1013904223u;
        v[i] = static_cast<float>(state >> 8) / 16777216.f * 2.f - 0.5f;
    }

    return v;
}

void
expectSameLine(const std::vector<float>& expected,
               const std::vector<float>& got,
               const std::string& what)
{
    ASSERT_EQ(expected.size(), got.size()) << what;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(expected[i], got[i]) << what << " at " << i;
    }
}
} // namespace

// Every line length from 1 to 41 against every half width from 0 to 8: the lines shorter than
// the window, the one value past it where CImg collapses the line, and the long lines.
TEST(NativeErodeDilateKernels, RowsMatchCImgErodeAndDilate)
{
    ErodeDilateKernels::LineScratch scratch;

    for (int takeMax = 0; takeMax <= 1; ++takeMax) {
        for (int length = 1; length <= 41; ++length) {
            for (int halfWidth = 0; halfWidth <= 8; ++halfWidth) {
                const std::vector<float> values = randomValues(length, length * 31 + halfWidth);
                CImgF image(length, 1, 1, 1);
                std::copy(values.begin(), values.end(), image.data());
                const unsigned int size = (unsigned int)halfWidth * 2 + 1;
                if (takeMax) {
                    image.dilate(size, 1);
                } else {
                    image.erode(size, 1);
                }
                std::vector<float> expected(image.data(), image.data() + length);
                std::vector<float> got = values;
                ErodeDilateKernels::filterLine(&got[0], length, 1, halfWidth, takeMax != 0, scratch);
                expectSameLine(expected, got, std::string(takeMax ? "dilate" : "erode") + " length " + std::to_string(length) + " half " + std::to_string(halfWidth));
            }
        }
    }
}

TEST(NativeErodeDilateKernels, StridedColumnsMatchCImg)
{
    ErodeDilateKernels::LineScratch scratch;
    const int width = 3;

    for (int takeMax = 0; takeMax <= 1; ++takeMax) {
        for (int height = 2; height <= 30; ++height) {
            for (int halfWidth = 1; halfWidth <= 6; ++halfWidth) {
                const std::vector<float> values = randomValues((std::size_t)width * height, height * 7 + halfWidth);
                CImgF image(width, height, 1, 1);
                std::copy(values.begin(), values.end(), image.data());
                const unsigned int size = (unsigned int)halfWidth * 2 + 1;
                if (takeMax) {
                    image.dilate(1, size);
                } else {
                    image.erode(1, size);
                }
                std::vector<float> got = values;
                for (int x = 0; x < width; ++x) {
                    ErodeDilateKernels::filterLine(&got[x], height, width, halfWidth, takeMax != 0, scratch);
                }
                expectSameLine(std::vector<float>(image.data(), image.data() + (std::size_t)width * height), got, std::string(takeMax ? "dilate" : "erode") + " height " + std::to_string(height) + " half " + std::to_string(halfWidth));
            }
        }
    }
}

TEST(NativeErodeDilateKernels, TwoDimensionalOrderMatchesCImg)
{
    ErodeDilateKernels::LineScratch scratch;
    const int width = 23;
    const int height = 17;
    const int halfX = 4;
    const int halfY = 2;
    const std::vector<float> values = randomValues((std::size_t)width * height, 5);

    // A positive X size with a negative Y size erodes along X before it dilates along Y, and the
    // two do not commute.
    CImgF image(width, height, 1, 1);
    std::copy(values.begin(), values.end(), image.data());
    image.erode((unsigned int)halfX * 2 + 1, 1);
    image.dilate(1, (unsigned int)halfY * 2 + 1);

    std::vector<float> got = values;
    for (int y = 0; y < height; ++y) {
        ErodeDilateKernels::filterLine(&got[(std::size_t)y * width], width, 1, halfX, false, scratch);
    }
    for (int x = 0; x < width; ++x) {
        ErodeDilateKernels::filterLine(&got[x], height, width, halfY, true, scratch);
    }
    expectSameLine(std::vector<float>(image.data(), image.data() + (std::size_t)width * height), got, "erode X then dilate Y");
}

#else // NATRON_ERODEDILATE_HAVE_CIMG

TEST(NativeErodeDilateKernels, CImgReferenceAvailable)
{
    ADD_FAILURE() << "CImg.h was not found (NATRON_TESTS_CIMG_HEADER); run tools/ci/local/fetch-assets.sh so the "
                     "ErodeDilate reference comparison can run.";
}

#endif // NATRON_ERODEDILATE_HAVE_CIMG
