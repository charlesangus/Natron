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

#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/Image/TransformMath.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

namespace {

using Resampler::SourceImage;
using Resampler::WrapMode;

const Resampler::FilterEnum kAllFilters[] = {
    Resampler::eFilterImpulse,
    Resampler::eFilterBox,
    Resampler::eFilterBilinear,
    Resampler::eFilterCubic,
    Resampler::eFilterKeys,
    Resampler::eFilterSimon,
    Resampler::eFilterRifman,
    Resampler::eFilterMitchell,
    Resampler::eFilterParzen,
    Resampler::eFilterNotch,
};

std::vector<float>
makePattern(const RectI& bounds,
            int nComps)
{
    std::vector<float> data((std::size_t)bounds.width() * bounds.height() * nComps);
    std::size_t k = 0;
    for (int y = bounds.y1; y < bounds.y2; ++y) {
        for (int x = bounds.x1; x < bounds.x2; ++x) {
            for (int c = 0; c < nComps; ++c, ++k) {
                data[k] = (float)(0.5 + 0.5 * std::sin(0.37 * x + 0.21 * y * y + 1.3 * c) + 0.01 * ((x * 7 + y * 13) % 5));
            }
        }
    }
    return data;
}

// A 4x4 single-channel ramp, value x + 4 * y at pixel (x, y).
std::vector<float>
makeRamp()
{
    std::vector<float> data(16);
    for (int k = 0; k < 16; ++k) {
        data[k] = (float)k;
    }
    return data;
}

int
repeatIndex(int p,
            int w)
{
    return ((p % w) + w) % w;
}

int
mirrorIndex(int p,
            int w)
{
    const int q = ((p % (2 * w)) + 2 * w) % (2 * w);
    return q < w ? q : 2 * w - 1 - q;
}

int
clampIndex(int p,
           int w)
{
    return p < 0 ? 0 : (p >= w ? w - 1 : p);
}

// Samples the 4x4 ramp along x (y fixed at row 1) for pixels -6..9 and returns the values.
std::vector<float>
sampleRampRow(WrapMode wrap,
              bool blackOutside,
              Resampler::FilterEnum filter)
{
    const std::vector<float> ramp = makeRamp();
    const SourceImage src(ramp.data(), RectI(0, 0, 4, 4), 1);
    std::vector<float> sx, sy;
    for (int p = -6; p < 10; ++p) {
        sx.push_back((float)p + 0.5f);
        sy.push_back(1.5f);
    }
    std::vector<float> out(sx.size(), -1.f);
    Resampler::resampleRowFromCoords(src, sx.data(), sy.data(), 0, (int)sx.size(), filter, false, blackOutside, wrap, Resampler::eWrapNone, out.data(), 1);
    return out;
}
} // anon namespace

TEST(ResamplerCoordMap, IdentityMapCopiesTheSourceWithImpulse)
{
    const RectI bounds(-3, 2, 6, 9);
    for (int nComps = 1; nComps <= 4; ++nComps) {
        const std::vector<float> data = makePattern(bounds, nComps);
        const SourceImage src(data.data(), bounds, nComps);
        const int n = bounds.width();
        std::vector<float> sx(n), sy(n), out((std::size_t)n * nComps);
        for (int y = bounds.y1; y < bounds.y2; ++y) {
            for (int i = 0; i < n; ++i) {
                sx[i] = (float)(bounds.x1 + i) + 0.5f;
                sy[i] = (float)y + 0.5f;
            }
            for (int wrap = Resampler::eWrapNone; wrap <= Resampler::eWrapMirror; ++wrap) {
                Resampler::resampleRowFromCoords(src, sx.data(), sy.data(), 0, n, Resampler::eFilterImpulse, false, true, (WrapMode)wrap, (WrapMode)wrap, out.data(), nComps);
                const float* row = data.data() + (std::size_t)(y - bounds.y1) * src.rowStride;
                for (int k = 0; k < n * nComps; ++k) {
                    ASSERT_EQ(out[k], row[k]) << "nComps " << nComps << " wrap " << wrap << " y " << y << " k " << k;
                }
            }
        }
    }
}

TEST(ResamplerCoordMap, ExtraOutputComponentsAreZero)
{
    const RectI bounds(0, 0, 4, 4);
    const std::vector<float> data = makePattern(bounds, 2);
    const SourceImage src(data.data(), bounds, 2);
    const float sx[2] = { 0.5f, 3.5f };
    const float sy[2] = { 2.5f, 1.5f };
    float out[8];
    Resampler::resampleRowFromCoords(src, sx, sy, 0, 2, Resampler::eFilterImpulse, false, true, Resampler::eWrapNone, Resampler::eWrapNone, out, 4);
    EXPECT_EQ(out[0], src.pixel(0, 2)[0]);
    EXPECT_EQ(out[1], src.pixel(0, 2)[1]);
    EXPECT_EQ(out[2], 0.f);
    EXPECT_EQ(out[3], 0.f);
    EXPECT_EQ(out[4], src.pixel(3, 1)[0]);
    EXPECT_EQ(out[5], src.pixel(3, 1)[1]);
    EXPECT_EQ(out[6], 0.f);
    EXPECT_EQ(out[7], 0.f);
}

TEST(ResamplerCoordMap, WrapModesOnARamp)
{
    for (int bo = 0; bo < 2; ++bo) {
        const bool blackOutside = bo != 0;
        const std::vector<float> none = sampleRampRow(Resampler::eWrapNone, blackOutside, Resampler::eFilterImpulse);
        const std::vector<float> clamped = sampleRampRow(Resampler::eWrapClamp, blackOutside, Resampler::eFilterImpulse);
        const std::vector<float> repeat = sampleRampRow(Resampler::eWrapRepeat, blackOutside, Resampler::eFilterImpulse);
        const std::vector<float> mirror = sampleRampRow(Resampler::eWrapMirror, blackOutside, Resampler::eFilterImpulse);
        for (int p = -6; p < 10; ++p) {
            const std::size_t i = (std::size_t)(p + 6);
            const bool inside = p >= 0 && p < 4;
            const float noneWant = (inside || !blackOutside) ? (float)(4 + clampIndex(p, 4)) : 0.f;
            EXPECT_EQ(none[i], noneWant) << "none p " << p << " blackOutside " << blackOutside;
            EXPECT_EQ(clamped[i], (float)(4 + clampIndex(p, 4))) << "clamp p " << p << " blackOutside " << blackOutside;
            EXPECT_EQ(repeat[i], (float)(4 + repeatIndex(p, 4))) << "repeat p " << p << " blackOutside " << blackOutside;
            EXPECT_EQ(mirror[i], (float)(4 + mirrorIndex(p, 4))) << "mirror p " << p << " blackOutside " << blackOutside;
        }
    }
}

TEST(ResamplerCoordMap, WrapModesApplyPerAxis)
{
    const std::vector<float> ramp = makeRamp();
    const SourceImage src(ramp.data(), RectI(0, 0, 4, 4), 1);
    std::vector<float> sx, sy;
    for (int p = -5; p < 9; ++p) {
        sx.push_back(2.5f);
        sy.push_back((float)p + 0.5f);
    }
    const int n = (int)sx.size();
    std::vector<float> repeat(n), mirror(n), clamped(n), none(n);
    Resampler::resampleRowFromCoords(src, sx.data(), sy.data(), 0, n, Resampler::eFilterImpulse, false, true, Resampler::eWrapNone, Resampler::eWrapRepeat, repeat.data(), 1);
    Resampler::resampleRowFromCoords(src, sx.data(), sy.data(), 0, n, Resampler::eFilterImpulse, false, true, Resampler::eWrapNone, Resampler::eWrapMirror, mirror.data(), 1);
    Resampler::resampleRowFromCoords(src, sx.data(), sy.data(), 0, n, Resampler::eFilterImpulse, false, true, Resampler::eWrapNone, Resampler::eWrapClamp, clamped.data(), 1);
    Resampler::resampleRowFromCoords(src, sx.data(), sy.data(), 0, n, Resampler::eFilterImpulse, false, true, Resampler::eWrapRepeat, Resampler::eWrapNone, none.data(), 1);
    for (int p = -5; p < 9; ++p) {
        const std::size_t i = (std::size_t)(p + 5);
        EXPECT_EQ(repeat[i], (float)(2 + 4 * repeatIndex(p, 4))) << "p " << p;
        EXPECT_EQ(mirror[i], (float)(2 + 4 * mirrorIndex(p, 4))) << "p " << p;
        EXPECT_EQ(clamped[i], (float)(2 + 4 * clampIndex(p, 4))) << "p " << p;
        EXPECT_EQ(none[i], (p >= 0 && p < 4) ? (float)(2 + 4 * p) : 0.f) << "p " << p;
    }
}

TEST(ResamplerCoordMap, WrapDomainOverridesTheBounds)
{
    const std::vector<float> ramp = makeRamp();
    const SourceImage src(ramp.data(), RectI(0, 0, 4, 4), 1);
    const RectI domain(1, 0, 3, 4);
    std::vector<float> sx, sy;
    for (int p = -2; p < 6; ++p) {
        sx.push_back((float)p + 0.5f);
        sy.push_back(0.5f);
    }
    const int n = (int)sx.size();
    std::vector<float> out(n);
    Resampler::resampleRowFromCoords(src, sx.data(), sy.data(), 0, n, Resampler::eFilterImpulse, false, true, Resampler::eWrapRepeat, Resampler::eWrapNone, out.data(), 1, &domain);
    for (int p = -2; p < 6; ++p) {
        EXPECT_EQ(out[(std::size_t)(p + 2)], (float)(1 + repeatIndex(p - 1, 2))) << "p " << p;
    }
}

TEST(ResamplerCoordMap, WrappedFilteredSamplesMatchTheWrappedCoordinate)
{
    const RectI bounds(0, 0, 8, 6);
    const std::vector<float> data = makePattern(bounds, 3);
    const SourceImage src(data.data(), bounds, 3);
    const float sx[2] = { 8.f + 2.3f, -8.f + 5.7f };
    const float sy[2] = { 6.f + 3.1f, 1.9f };
    const float wx[2] = { 2.3f, 5.7f };
    const float wy[2] = { 3.1f, 1.9f };
    for (Resampler::FilterEnum filter : kAllFilters) {
        float got[6], want[6];
        Resampler::resampleRowFromCoords(src, sx, sy, 0, 2, filter, false, true, Resampler::eWrapRepeat, Resampler::eWrapRepeat, got, 3);
        Resampler::resampleRowFromCoords(src, wx, wy, 0, 2, filter, false, true, Resampler::eWrapNone, Resampler::eWrapNone, want, 3);
        for (int k = 0; k < 6; ++k) {
            EXPECT_NEAR(got[k], want[k], 1e-5) << "filter " << filter << " k " << k;
        }
    }
}

TEST(ResamplerCoordMap, NonFiniteCoordinatesReadOutside)
{
    const RectI bounds(0, 0, 4, 4);
    const std::vector<float> data = makePattern(bounds, 4);
    const SourceImage src(data.data(), bounds, 4);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const float sx[4] = { nan, 1.5f, inf, -inf };
    const float sy[4] = { 1.5f, nan, 1.5f, 2.5f };
    const float jac[16] = { nan, 0.f, 0.f, nan, 3.f, inf, 0.f, 3.f, 2.f, 0.f, 0.f, 2.f, 2.f, 0.f, 0.f, 2.f };
    for (Resampler::FilterEnum filter : kAllFilters) {
        for (int bo = 0; bo < 2; ++bo) {
            for (int wrap = Resampler::eWrapNone; wrap <= Resampler::eWrapMirror; ++wrap) {
                float out[16];
                Resampler::resampleRowFromCoords(src, sx, sy, jac, 4, filter, false, bo != 0, (WrapMode)wrap, (WrapMode)wrap, out, 4);
                for (int k = 0; k < 16; ++k) {
                    ASSERT_TRUE(std::isfinite(out[k])) << "filter " << filter << " wrap " << wrap << " k " << k;
                }
                for (int k = 0; k < 8; ++k) {
                    EXPECT_EQ(out[k], 0.f) << "NaN: filter " << filter << " wrap " << wrap << " k " << k;
                }
                if (wrap == Resampler::eWrapClamp) {
                    // Infinities clamp to the edge pixel centres.
                    if (filter == Resampler::eFilterImpulse) {
                        for (int c = 0; c < 4; ++c) {
                            EXPECT_EQ(out[8 + c], src.pixel(3, 1)[c]);
                            EXPECT_EQ(out[12 + c], src.pixel(0, 2)[c]);
                        }
                    }
                } else {
                    for (int k = 8; k < 16; ++k) {
                        EXPECT_EQ(out[k], 0.f) << "inf: filter " << filter << " wrap " << wrap << " k " << k;
                    }
                }
            }
        }
    }
}

TEST(ResamplerCoordMap, JacobianMinificationMatchesResampleRow)
{
    const RectI srcBounds(-2, -1, 62, 58);
    const double scale = 4.;
    const double offX = 1.25;
    const double offY = -0.75;
    const RectI window(-2, -2, 18, 17);
    const int nComps = 3;
    const std::vector<float> data = makePattern(srcBounds, nComps);
    const SourceImage src(data.data(), srcBounds, nComps);
    const TransformMath::Mat3 inv(scale, 0., offX, 0., scale, offY, 0., 0., 1.);
    const int n = window.width();

    auto coordRow = [&](int y, std::vector<float>* sx, std::vector<float>* sy) {
        sx->resize(n);
        sy->resize(n);
        for (int i = 0; i < n; ++i) {
            (*sx)[i] = (float)(scale * ((window.x1 + i) + 0.5) + offX);
            (*sy)[i] = (float)(scale * (y + 0.5) + offY);
        }
    };

    for (Resampler::FilterEnum filter : kAllFilters) {
        for (int bo = 0; bo < 2; ++bo) {
            const bool blackOutside = bo != 0;
            Resampler::ResampleParams params;
            params.invTransforms = &inv;
            params.count = 1;
            params.filter = filter;
            params.clamp = false;
            params.blackOutside = blackOutside;
            std::vector<float> want((std::size_t)n * nComps), got((std::size_t)n * nComps), jac((std::size_t)n * 4);
            std::vector<float> sxP, syP, sx, sy, sxN, syN;
            for (int y = window.y1; y < window.y2; ++y) {
                coordRow(y - 1, &sxP, &syP);
                coordRow(y, &sx, &sy);
                coordRow(y + 1, &sxN, &syN);
                Resampler::coordJacobians(sxP.data(), sx.data(), sxN.data(), syP.data(), sy.data(), syN.data(), n, jac.data());
                for (int i = 0; i < n; ++i) {
                    ASSERT_EQ(jac[4 * i + 0], (float)scale);
                    ASSERT_EQ(jac[4 * i + 1], 0.f);
                    ASSERT_EQ(jac[4 * i + 2], 0.f);
                    ASSERT_EQ(jac[4 * i + 3], (float)scale);
                }
                Resampler::resampleRow(params, src, y, window.x1, window.x2, want.data());
                Resampler::resampleRowFromCoords(src, sx.data(), sy.data(), jac.data(), n, filter, false, blackOutside, Resampler::eWrapNone, Resampler::eWrapNone, got.data(), nComps);
                for (std::size_t k = 0; k < want.size(); ++k) {
                    ASSERT_NEAR(got[k], want[k], 1e-5) << "filter " << filter << " blackOutside " << blackOutside << " y " << y << " k " << k;
                }
            }
        }
    }
}

TEST(ResamplerCoordMap, CoordJacobiansUseCentralDifferences)
{
    const float sxP[4] = { 0.f, 1.f, 3.f, 6.f };
    const float sx[4] = { 1.f, 2.f, 4.f, 8.f };
    const float sxN[4] = { 3.f, 4.f, 6.f, 10.f };
    const float sy[4] = { 5.f, 5.f, 5.f, std::numeric_limits<float>::infinity() };
    float jac[16];

    Resampler::coordJacobians(sxP, sx, sxN, 0, sy, 0, 4, jac);
    EXPECT_EQ(jac[0], 1.f);
    EXPECT_EQ(jac[4], 1.5f);
    EXPECT_EQ(jac[8], 3.f);
    EXPECT_EQ(jac[12], 4.f);
    EXPECT_EQ(jac[1], 1.5f);
    EXPECT_EQ(jac[5], 1.5f);
    EXPECT_EQ(jac[9], 1.5f);
    EXPECT_EQ(jac[13], 2.f);
    EXPECT_EQ(jac[2], 0.f);
    EXPECT_EQ(jac[6], 0.f);
    EXPECT_EQ(jac[10], 0.f);
    EXPECT_EQ(jac[14], 0.f);
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(jac[4 * i + 3], 0.f);
    }

    Resampler::coordJacobians(0, sx, sxN, 0, sy, 0, 4, jac);
    EXPECT_EQ(jac[1], 2.f);
    EXPECT_EQ(jac[13], 2.f);
    Resampler::coordJacobians(sxP, sx, 0, 0, sy, 0, 1, jac);
    EXPECT_EQ(jac[0], 0.f);
    EXPECT_EQ(jac[1], 1.f);
}

TEST(ResamplerCoordMap, MissingSourceIsBlack)
{
    const SourceImage src;
    const float sx[2] = { 0.5f, 1.5f };
    const float sy[2] = { 0.5f, 0.5f };
    float out[8] = { 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f };
    Resampler::resampleRowFromCoords(src, sx, sy, 0, 2, Resampler::eFilterCubic, false, false, Resampler::eWrapClamp, Resampler::eWrapClamp, out, 4);
    for (int k = 0; k < 8; ++k) {
        EXPECT_EQ(out[k], 0.f);
    }
}

NATRON_NAMESPACE_EXIT
