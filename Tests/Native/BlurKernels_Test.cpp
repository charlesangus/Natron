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
#include <cstdint>
#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Filter/BlurKernels.h"

#if defined(NATRON_TESTS_CIMG_HEADER)
#if __has_include(NATRON_TESTS_CIMG_HEADER)
#define NATRON_BLURKERNELS_HAVE_CIMG 1
#endif
#endif

#ifdef NATRON_BLURKERNELS_HAVE_CIMG
#define cimg_display 0
#define cimg_verbosity 0
#define cimg_use_openmp 0
#define cimg_namespace_suffix natron_tests
#include NATRON_TESTS_CIMG_HEADER
#endif

NATRON_NAMESPACE_USING

namespace {
using namespace BlurKernels;

// Planar x, y, c layout, the layout CImg<float>(w, h, 1, c) stores.
struct Planar {
    int w = 0, h = 0, c = 0;
    std::vector<float> v;

    float* row(int y,
               int ch)
    {
        return v.data() + (static_cast<std::size_t>(ch) * h + y) * w;
    }

    float* column(int x,
                  int ch)
    {
        return v.data() + static_cast<std::size_t>(ch) * h * w + x;
    }
};

// Random values in [-0.5, 1.5] plus hard edges and an isolated impulse, so both boundaries and
// the IIR tails are exercised.
Planar
makeSource(int w,
           int h,
           int c)
{
    Planar p;
    p.w = w;
    p.h = h;
    p.c = c;
    p.v.resize(static_cast<std::size_t>(w) * h * c);
    std::uint32_t state = 0x9e3779b9u + static_cast<std::uint32_t>(w * 131 + h * 17 + c);
    for (int ch = 0; ch < c; ++ch) {
        for (int y = 0; y < h; ++y) {
            float* r = p.row(y, ch);
            for (int x = 0; x < w; ++x) {
                state = state * 1664525u + 1013904223u;
                const float noise = static_cast<float>(state >> 8) / 16777216.f;
                float value = noise * 2.f - 0.5f;
                if ((x < w / 4) && (y < h / 3)) {
                    value = 1.f;
                } else if (x > (3 * w) / 4) {
                    value = 0.f;
                }
                r[x] = value;
            }
        }
        p.row(h / 2, ch)[w / 2] = 8.f;
    }

    return p;
}

void
applyPlanar(const LineFilter& fx,
            const LineFilter& fy,
            Planar& img,
            LineScratch& scratch)
{
    for (int ch = 0; ch < img.c; ++ch) {
        for (int y = 0; y < img.h; ++y) {
            fx.apply(img.row(y, ch), img.w, 1, scratch);
        }
    }
    for (int ch = 0; ch < img.c; ++ch) {
        for (int x = 0; x < img.w; ++x) {
            fy.apply(img.column(x, ch), img.h, img.w, scratch);
        }
    }
}

} // namespace

TEST(BlurKernels, IdentityCases)
{
    EXPECT_TRUE(LineFilter().isIdentity());
    EXPECT_TRUE(LineFilter::deriche(0.05f, 0, true).isIdentity());
    EXPECT_FALSE(LineFilter::deriche(0.05f, 1, true).isIdentity());
    EXPECT_TRUE(LineFilter::vanVliet(0.05f, 0, false).isIdentity());
    EXPECT_FALSE(LineFilter::vanVliet(0.5f, 0, false).isIdentity());
    EXPECT_TRUE(LineFilter::box(0.f, 1, true, 1).isIdentity());
    EXPECT_TRUE(LineFilter::box(1.f, 0, true, 3).isIdentity());
    EXPECT_FALSE(LineFilter::box(1.f, 1, true, 3).isIdentity());
    EXPECT_FALSE(LineFilter::box(1.5f, 0, true, 1).isIdentity());
    EXPECT_TRUE(LineFilter::forFilter(eFilterGaussian, 0.f, 0, true).isIdentity());

    std::vector<float> line = { 1.f, -2.f, 3.f };
    const std::vector<float> before = line;
    LineFilter::box(1.f, 0, true, 3).apply(line.data(), static_cast<int>(line.size()), 1);
    EXPECT_EQ(line, before);
    LineFilter::vanVliet(3.f, 0, true).apply(line.data(), 0, 1);
    EXPECT_EQ(line, before);
}

// The same filter over an interleaved buffer (stride = components) must give exactly the planar
// result, so a node can filter its RGBA rows and columns in place.
TEST(BlurKernels, InterleavedStrideMatchesPlanar)
{
    const int w = 33, h = 21, c = 4;
    const Planar src = makeSource(w, h, c);
    const LineFilter filters[] = {
        LineFilter::vanVliet(3.f, 0, true),
        LineFilter::vanVliet(2.f, 1, false),
        LineFilter::deriche(4.f, 0, false),
        LineFilter::box(5.5f, 0, true, 3),
        LineFilter::box(3.f, 1, false, 1),
    };
    LineScratch scratch;
    for (const LineFilter& f : filters) {
        Planar planar = src;
        applyPlanar(f, f, planar, scratch);

        std::vector<float> inter(static_cast<std::size_t>(w) * h * c);
        for (int ch = 0; ch < c; ++ch) {
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    inter[(static_cast<std::size_t>(y) * w + x) * c + ch] = src.v[(static_cast<std::size_t>(ch) * h + y) * w + x];
                }
            }
        }
        for (int y = 0; y < h; ++y) {
            for (int ch = 0; ch < c; ++ch) {
                f.apply(inter.data() + static_cast<std::size_t>(y) * w * c + ch, w, c, scratch);
            }
        }
        for (int x = 0; x < w; ++x) {
            for (int ch = 0; ch < c; ++ch) {
                f.apply(inter.data() + static_cast<std::size_t>(x) * c + ch, h, static_cast<std::ptrdiff_t>(w) * c,
                        scratch);
            }
        }
        for (int ch = 0; ch < c; ++ch) {
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    ASSERT_EQ(inter[(static_cast<std::size_t>(y) * w + x) * c + ch],
                              planar.v[(static_cast<std::size_t>(ch) * h + y) * w + x]);
                }
            }
        }
    }
}

TEST(BlurKernels, ForFilterMapsCImgBlurChoices)
{
    const Planar src = makeSource(24, 16, 1);
    struct Pair {
        Filter filter;
        LineFilter expected;
    };
    const Pair pairs[] = {
        { eFilterQuasiGaussian, LineFilter::deriche(2.5f, 1, true) },
        { eFilterGaussian, LineFilter::vanVliet(2.5f, 1, true) },
        { eFilterBox, LineFilter::box(2.5f, 1, true, 1) },
        { eFilterTriangle, LineFilter::box(2.5f, 1, true, 2) },
        { eFilterQuadratic, LineFilter::box(2.5f, 1, true, 3) },
    };
    LineScratch scratch;
    for (const Pair& p : pairs) {
        Planar a = src, b = src;
        const LineFilter f = LineFilter::forFilter(p.filter, 2.5f, 1, true);
        applyPlanar(f, f, a, scratch);
        applyPlanar(p.expected, p.expected, b, scratch);
        EXPECT_EQ(a.v, b.v) << "filter " << static_cast<int>(p.filter);
        EXPECT_NE(a.v, src.v) << "filter " << static_cast<int>(p.filter);
    }
}

#ifdef NATRON_BLURKERNELS_HAVE_CIMG

namespace {
typedef cimg_library_suffixed::CImg<float> CImgF;

const double kMaxAbsDiff = 1e-6;

double
maxAbsDiff(const std::vector<float>& a,
           const std::vector<float>& b)
{
    double m = a.size() == b.size() ? 0. : INFINITY;
    for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
        const double d = std::fabs(static_cast<double>(a[i]) - static_cast<double>(b[i]));
        if (!(d <= m)) {
            m = d;
        }
    }

    return m;
}

struct Size {
    int w, h, c;
};

// 7x5 makes sigma 25 and box 25 far wider than every line.
const Size kSizes[] = { { 64, 48, 2 }, { 7, 5, 1 } };
const float kSigmas[] = { 0.05f, 0.3f, 0.5f, 3.f, 25.f };
const float kBoxSizes[] = { 0.5f, 1.f, 3.f, 4.7f, 25.f };

std::string
caseName(const char* filter,
         float size,
         unsigned int order,
         bool neumann,
         const Size& s,
         unsigned int iterations = 0)
{
    std::ostringstream os;
    os << filter << " size=" << size << " order=" << order << " neumann=" << neumann << " image=" << s.w << "x" << s.h
       << "x" << s.c;
    if (iterations) {
        os << " iterations=" << iterations;
    }

    return os.str();
}

CImgF
toCImg(const Planar& p)
{
    CImgF img(p.w, p.h, 1, p.c);
    std::copy(p.v.begin(), p.v.end(), img.data());

    return img;
}

std::vector<float>
fromCImg(const CImgF& img)
{
    return std::vector<float>(img.data(), img.data() + img.size());
}
} // namespace

TEST(BlurKernels, DericheMatchesCImg)
{
    LineScratch scratch;
    for (const Size& s : kSizes) {
        const Planar src = makeSource(s.w, s.h, s.c);
        for (float sigma : kSigmas) {
            for (unsigned int order = 0; order <= 2; ++order) {
                for (bool neumann : { false, true }) {
                    CImgF ref = toCImg(src);
                    ref.deriche(sigma, order, 'x', neumann);
                    ref.deriche(sigma, order, 'y', neumann);

                    Planar got = src;
                    const LineFilter f = LineFilter::deriche(sigma, order, neumann);
                    applyPlanar(f, f, got, scratch);

                    EXPECT_LE(maxAbsDiff(got.v, fromCImg(ref)), kMaxAbsDiff)
                        << caseName("deriche", sigma, order, neumann, s);
                }
            }
        }
    }
}

TEST(BlurKernels, VanVlietMatchesCImg)
{
    LineScratch scratch;
    for (const Size& s : kSizes) {
        const Planar src = makeSource(s.w, s.h, s.c);
        for (float sigma : kSigmas) {
            for (unsigned int order = 0; order <= 3; ++order) {
                // Below sigma 0.5 CImg hands over to deriche, which has no order 3 and throws.
                if ((order == 3) && (sigma < 0.5f)) {
                    continue;
                }
                for (bool neumann : { false, true }) {
                    CImgF ref = toCImg(src);
                    ref.vanvliet(sigma, order, 'x', neumann);
                    ref.vanvliet(sigma, order, 'y', neumann);

                    Planar got = src;
                    const LineFilter f = LineFilter::vanVliet(sigma, order, neumann);
                    applyPlanar(f, f, got, scratch);

                    EXPECT_LE(maxAbsDiff(got.v, fromCImg(ref)), kMaxAbsDiff)
                        << caseName("vanvliet", sigma, order, neumann, s);
                }
            }
        }
    }
}

TEST(BlurKernels, BoxTriangleQuadraticMatchCImg)
{
    LineScratch scratch;
    for (const Size& s : kSizes) {
        const Planar src = makeSource(s.w, s.h, s.c);
        for (float size : kBoxSizes) {
            for (int order = 0; order <= 2; ++order) {
                for (unsigned int iterations = 1; iterations <= 3; ++iterations) {
                    for (bool neumann : { false, true }) {
                        CImgF ref = toCImg(src);
                        ref.boxfilter(size, order, 'x', neumann, iterations);
                        ref.boxfilter(size, order, 'y', neumann, iterations);

                        Planar got = src;
                        const LineFilter f = LineFilter::box(size, order, neumann, iterations);
                        applyPlanar(f, f, got, scratch);

                        EXPECT_LE(maxAbsDiff(got.v, fromCImg(ref)), kMaxAbsDiff)
                            << caseName("box", size, static_cast<unsigned int>(order), neumann, s, iterations);
                    }
                }
            }
        }
    }
}

// Separate X and Y parameters, as CImgBlur uses for a non-uniform size.
TEST(BlurKernels, AnisotropicGaussianMatchesCImg)
{
    const Planar src = makeSource(64, 48, 2);
    CImgF ref = toCImg(src);
    ref.vanvliet(40.f / 2.4f, 1, 'x', true);
    ref.vanvliet(5.f / 2.4f, 0, 'y', true);

    Planar got = src;
    LineScratch scratch;
    applyPlanar(LineFilter::vanVliet(40.f / 2.4f, 1, true), LineFilter::vanVliet(5.f / 2.4f, 0, true), got, scratch);
    EXPECT_LE(maxAbsDiff(got.v, fromCImg(ref)), kMaxAbsDiff);
}

#else // NATRON_BLURKERNELS_HAVE_CIMG

TEST(BlurKernels, CImgReferenceAvailable)
{
    ADD_FAILURE() << "CImg.h was not found (NATRON_TESTS_CIMG_HEADER); run tools/ci/local/fetch-assets.sh so the "
                     "BlurKernels reference comparison can run.";
}

#endif // NATRON_BLURKERNELS_HAVE_CIMG
