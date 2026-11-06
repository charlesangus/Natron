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
#include <cstdio>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/Image/TransformMath.h"

NATRON_NAMESPACE_USING

namespace {
using namespace Resampler;
using TransformMath::Mat3;
using TransformMath::TransformParams;

const FilterEnum kAllFilters[] = {
    eFilterImpulse,
    eFilterBox,
    eFilterBilinear,
    eFilterCubic,
    eFilterKeys,
    eFilterSimon,
    eFilterRifman,
    eFilterMitchell,
    eFilterParzen,
    eFilterNotch,
    eFilterLanczos2,
    eFilterLanczos3,
    eFilterLanczos4,
    eFilterSinc,
    eFilterBlackmanHarris,
    eFilterGaussian,
    eFilterBicubic,
};

// The filters that reproduce the samples exactly at d = 0.
const FilterEnum kInterpolatingFilters[] = {
    eFilterImpulse,
    eFilterBilinear,
    eFilterCubic,
    eFilterKeys,
    eFilterSimon,
    eFilterRifman,
    eFilterLanczos2,
    eFilterLanczos3,
    eFilterLanczos4,
    eFilterSinc,
};

const FilterEnum kNewFilters[] = {
    eFilterLanczos2,
    eFilterLanczos3,
    eFilterLanczos4,
    eFilterSinc,
    eFilterBlackmanHarris,
    eFilterGaussian,
    eFilterBicubic,
};

// Every filter that goes through the weighted kernel path, with the parameter variants that
// change the kernel.
std::vector<FilterSpec>
kernelSpecs()
{
    std::vector<FilterSpec> specs;

    for (FilterEnum f : kAllFilters) {
        if (f != eFilterImpulse && f != eFilterBox) {
            specs.push_back(FilterSpec(f));
        }
    }
    specs.push_back(FilterSpec(eFilterSinc, 2));
    specs.push_back(FilterSpec(eFilterSinc, 8));
    specs.push_back(FilterSpec(eFilterBicubic, 4, 0., 0.5));
    specs.push_back(FilterSpec(eFilterBicubic, 4, 1., 0.));

    return specs;
}

const double kEps = 1e-12;

void
expectMatrixNear(const Mat3& actual,
                 const Mat3& expected,
                 double tol)
{
    for (int k = 0; k < 9; ++k) {
        EXPECT_NEAR(actual.m[k], expected.m[k], tol) << "element " << k;
    }
}

// A one-channel image whose pixel (x, y) holds value(x, y), stored in the Natron float layout.
template <typename F>
std::vector<float>
makeImage(const RectI& bounds,
          F value)
{
    std::vector<float> data((size_t)bounds.width() * bounds.height());

    for (int y = bounds.y1; y < bounds.y2; ++y) {
        for (int x = bounds.x1; x < bounds.x2; ++x) {
            data[(size_t)(y - bounds.y1) * bounds.width() + (x - bounds.x1)] = value(x, y);
        }
    }

    return data;
}

float
checker(int x,
        int y)
{
    return ((x + y) % 2 == 0) ? 1.f : 0.f;
}

// The inverse canonical matrix of a pure translation by (dxPerFrame * time, 0).
CanonicalTransformFn
movingTranslation(double dxPerFrame)
{
    return [dxPerFrame](double time, double amount, bool invert, Mat3* m) {
        TransformParams p;
        p.translateX = dxPerFrame * time;
        *m = TransformMath::inverseTransformCanonical(p, amount, invert);
        return true;
    };
}
} // anon namespace

TEST(Resampler, WeightsSumToOne)
{
    const double ds[] = { 0., 0.25, 0.5, 0.75, 1. };

    for (FilterEnum f : kAllFilters) {
        for (double d : ds) {
            double w[4];
            filterWeights(f, d, w);
            EXPECT_NEAR(w[0] + w[1] + w[2] + w[3], 1., kEps) << "filter " << f << " d " << d;
        }
    }
}

TEST(Resampler, WeightsAtHalfMatchTheBCSplines)
{
    // Mitchell-Netravali (B, C) weights at d = 0.5: Q(1.5), P(0.5), P(0.5), Q(1.5).
    struct Case {
        FilterEnum f;
        double outer;
        double inner;
    };
    const Case cases[] = {
        { eFilterKeys, -1. / 16, 9. / 16 },
        { eFilterSimon, -0.09375, 0.59375 },
        { eFilterRifman, -0.125, 0.625 },
        { eFilterMitchell, -5. / 144, 77. / 144 },
        { eFilterParzen, 1. / 48, 23. / 48 },
        { eFilterNotch, 0.0625, 0.4375 },
        { eFilterBilinear, 0., 0.5 },
        { eFilterCubic, 0., 0.5 },
    };

    for (const Case& c : cases) {
        double w[4];
        filterWeights(c.f, 0.5, w);
        EXPECT_NEAR(w[0], c.outer, kEps) << "filter " << c.f;
        EXPECT_NEAR(w[1], c.inner, kEps) << "filter " << c.f;
        EXPECT_NEAR(w[2], c.inner, kEps) << "filter " << c.f;
        EXPECT_NEAR(w[3], c.outer, kEps) << "filter " << c.f;
    }

    // Keys at d = 0.25, from the OFX formula.
    double w[4];
    filterWeights(eFilterKeys, 0.25, w);
    EXPECT_NEAR(w[0], -0.0703125, kEps);
    EXPECT_NEAR(w[1], 0.8671875, kEps);
    EXPECT_NEAR(w[2], 0.2265625, kEps);
    EXPECT_NEAR(w[3], -0.0234375, kEps);
}

TEST(Resampler, OneDimensionalEdge)
{
    // The edge 0, 0 | 1, 1 sampled a quarter of the way from the last 0 to the first 1.
    struct Case {
        FilterEnum f;
        double expected;
    };
    const Case cases[] = {
        { eFilterImpulse, 0. },
        { eFilterBox, 0. },
        { eFilterBilinear, 0.25 },
        { eFilterCubic, 0.15625 },
        { eFilterKeys, 0.203125 },
        { eFilterSimon, 0.2265625 },
        { eFilterRifman, 0.25 },
        { eFilterMitchell, 0.24131944444444445 },
        { eFilterParzen, 0.3177083333333333 },
        { eFilterNotch, 0.375 },
    };

    for (const Case& c : cases) {
        EXPECT_NEAR(filter1D(c.f, 0., 0., 1., 1., 0.25, false), c.expected, kEps) << "filter " << c.f;
    }
    EXPECT_EQ(filter1D(eFilterImpulse, 0., 0., 1., 1., 0.5, false), 1.);
}

TEST(Resampler, ClampOnlyAppliesToTheSharpeningFilters)
{
    // A plateau 0 | 1, 1 | 0: the sharpening filters overshoot 1 between the two 1s.
    EXPECT_NEAR(filter1D(eFilterKeys, 0., 1., 1., 0., 0.5, false), 1.125, kEps);
    EXPECT_NEAR(filter1D(eFilterSimon, 0., 1., 1., 0., 0.5, false), 1.1875, kEps);
    EXPECT_NEAR(filter1D(eFilterRifman, 0., 1., 1., 0., 0.5, false), 1.25, kEps);
    for (FilterEnum f : { eFilterKeys, eFilterSimon, eFilterRifman }) {
        EXPECT_EQ(filter1D(f, 0., 1., 1., 0., 0.5, true), 1.) << "filter " << f;
    }
    // Mitchell reads its left neighbour even at d = 0; clamping holds it to the centre pair.
    EXPECT_NEAR(filter1D(eFilterMitchell, 1., 0., 0., 0., 0., false), 1. / 18, kEps);
    EXPECT_EQ(filter1D(eFilterMitchell, 1., 0., 0., 0., 0., true), 0.);

    // Parzen and Notch read outside the centre pair too, but are never clamped.
    EXPECT_NEAR(filter1D(eFilterParzen, 1., 0., 0., 0., 0., true), 1. / 6, kEps);
    EXPECT_NEAR(filter1D(eFilterNotch, 1., 0., 0., 0., 0., true), 0.25, kEps);

    EXPECT_TRUE(filterHonoursClamp(eFilterKeys));
    EXPECT_TRUE(filterHonoursClamp(eFilterMitchell));
    EXPECT_FALSE(filterHonoursClamp(eFilterCubic));
    EXPECT_FALSE(filterHonoursClamp(eFilterParzen));
    EXPECT_FALSE(filterHonoursClamp(eFilterNotch));
    EXPECT_FALSE(filterHonoursClamp(eFilterBilinear));

    for (FilterEnum f : { eFilterLanczos2, eFilterLanczos3, eFilterLanczos4, eFilterSinc, eFilterBicubic }) {
        EXPECT_TRUE(filterHonoursClamp(f)) << "filter " << f;
    }
    for (FilterEnum f : { eFilterBlackmanHarris, eFilterGaussian }) {
        EXPECT_FALSE(filterHonoursClamp(f)) << "filter " << f;
    }

    EXPECT_EQ(filterTaps(eFilterImpulse), 1);
    EXPECT_EQ(filterTaps(eFilterCubic), 2);
    EXPECT_EQ(filterTaps(eFilterNotch), 4);
    EXPECT_EQ(filterTaps(eFilterLanczos3), 6);
    EXPECT_EQ(filterTaps(FilterSpec(eFilterSinc, 8)), 16);
    EXPECT_EQ(filterTaps(eFilterGaussian), 3);
    EXPECT_EQ(filterOptions().size(), 17u);
    EXPECT_STREQ(filterOptions()[eFilterCubic].id, "cubic");
    EXPECT_STREQ(filterOptions()[eFilterLanczos2].id, "lanczos2");
    EXPECT_STREQ(filterOptions()[eFilterLanczos3].id, "lanczos3");
    EXPECT_STREQ(filterOptions()[eFilterLanczos4].id, "lanczos4");
    EXPECT_STREQ(filterOptions()[eFilterSinc].id, "sinc");
    EXPECT_STREQ(filterOptions()[eFilterBlackmanHarris].id, "blackmanHarris");
    EXPECT_STREQ(filterOptions()[eFilterGaussian].id, "gaussian");
    EXPECT_STREQ(filterOptions()[eFilterBicubic].id, "bicubic");
    EXPECT_EQ(shutterOffsetOptions().size(), 4u);
}

TEST(Resampler, TwoDimensionalImpulse)
{
    const RectI bounds(0, 0, 8, 8);
    std::vector<float> data = makeImage(bounds, [](int x, int y) {
        return (x == 4 && y == 4) ? 1.f : 0.f;
    });
    const SourceImage src(&data.front(), bounds, 1);

    struct Case {
        FilterEnum f;
        double atCentre;
        double atCorner;
    };
    // At the impulse's centre the 2-D value is w(0)^2 of the centre tap; at its top-right corner
    // (5, 5), half way to the next pixel on both axes, it is w(0.5)^2.
    const Case cases[] = {
        { eFilterImpulse, 1., 0. },
        { eFilterBilinear, 1., 0.25 },
        { eFilterCubic, 1., 0.25 },
        { eFilterKeys, 1., 0.31640625 },
        { eFilterSimon, 1., 0.59375 * 0.59375 },
        { eFilterRifman, 1., 0.390625 },
        { eFilterMitchell, (16. / 18) * (16. / 18), (77. / 144) * (77. / 144) },
        { eFilterParzen, (4. / 6) * (4. / 6), (23. / 48) * (23. / 48) },
        { eFilterNotch, 0.25, 0.4375 * 0.4375 },
    };

    for (const Case& c : cases) {
        float v = -1.f;
        EXPECT_TRUE(interpolate(c.f, false, 4.5, 4.5, src, true, &v));
        EXPECT_NEAR(v, c.atCentre, 1e-6) << "filter " << c.f;
        EXPECT_TRUE(interpolate(c.f, false, 5., 5., src, true, &v));
        EXPECT_NEAR(v, c.atCorner, 1e-6) << "filter " << c.f;
    }
}

TEST(Resampler, BlackOutsideAndEdgeClamp)
{
    const RectI bounds(0, 0, 4, 4);
    std::vector<float> data = makeImage(bounds, [](int, int) {
        return 1.f;
    });
    const SourceImage src(&data.front(), bounds, 1);
    float v = -1.f;

    // On the left edge of the image, half way between pixel -1 and pixel 0.
    EXPECT_TRUE(interpolate(eFilterBilinear, false, 0., 2.5, src, true, &v));
    EXPECT_NEAR(v, 0.5, 1e-7);
    EXPECT_TRUE(interpolate(eFilterBilinear, false, 0., 2.5, src, false, &v));
    EXPECT_NEAR(v, 1., 1e-7);

    // Far outside: black and reported as outside, or the nearest edge pixel.
    EXPECT_FALSE(interpolate(eFilterKeys, false, -10., -10., src, true, &v));
    EXPECT_EQ(v, 0.f);
    EXPECT_TRUE(interpolate(eFilterKeys, false, -10., -10., src, false, &v));
    EXPECT_NEAR(v, 1., 1e-7);

    // A missing source samples as black.
    SourceImage none;
    none.nComps = 1;
    EXPECT_FALSE(interpolate(eFilterCubic, false, 1., 1., none, false, &v));
    EXPECT_EQ(v, 0.f);
}

TEST(Resampler, MinificationWidensTheKernel)
{
    const RectI bounds(0, 0, 32, 32);
    std::vector<float> data = makeImage(bounds, checker);
    const SourceImage src(&data.front(), bounds, 1);
    float v = -1.f;

    // Without minification the centre of pixel (10, 10) reads that pixel.
    interpolateSuper(eFilterBilinear, false, 10.5, 10.5, 0., 0., 0., 0., src, true, &v);
    EXPECT_EQ(v, 1.f);
    interpolateSuper(eFilterBilinear, false, 10.5, 10.5, 1., 0., 0., 1., src, true, &v);
    EXPECT_EQ(v, 1.f);
    interpolateSuper(eFilterBilinear, false, 10.5, 10.5, 0.5, 0., 0., 0.5, src, true, &v);
    EXPECT_EQ(v, 1.f);

    // A 3x minification widens the tent to pixels 8 to 12 with weights 1, 2, 3, 2, 1 over 9 on
    // each axis; the parity of the pixels makes the even ones weigh 5/9 and the odd ones 4/9, so
    // the 2-D mean of the checkerboard is (5/9)^2 + (4/9)^2.
    interpolateSuper(eFilterBilinear, false, 10.5, 10.5, 3., 0., 0., 3., src, true, &v);
    EXPECT_NEAR(v, 41. / 81, 1e-5);

    // The box filter integrates over the back-transformed pixel [9.5, 11.5]^2:
    // (1 + 4 * 0.25) / 4.
    interpolateSuper(eFilterBox, false, 10.5, 10.5, 2., 0., 0., 2., src, true, &v);
    EXPECT_NEAR(v, 0.5, 1e-6);
}

TEST(Resampler, RoIExpansion)
{
    const RectD roi(0., 0., 10., 10.);
    struct Case {
        FilterEnum f;
        double e;
    };
    const Case cases[] = {
        { eFilterImpulse, 0. },
        { eFilterBox, 0. },
        { eFilterBilinear, 0.5 },
        { eFilterCubic, 0.5 },
        { eFilterKeys, 1.5 },
        { eFilterSimon, 1.5 },
        { eFilterRifman, 1.5 },
        { eFilterMitchell, 1.5 },
        { eFilterParzen, 1.5 },
        { eFilterNotch, 1.5 },
        { eFilterLanczos2, 1.5 },
        { eFilterLanczos3, 2.5 },
        { eFilterLanczos4, 3.5 },
        { eFilterSinc, 3.5 },
        { eFilterBlackmanHarris, 1.5 },
        { eFilterGaussian, 1. },
        { eFilterBicubic, 1.5 },
    };

    for (const Case& c : cases) {
        EXPECT_EQ(filterRoIExpansion(c.f), c.e);
        RectD srcRoI = roi;
        expandRoI(roi, 1., 1., 1., c.f, false, 1., &srcRoI);
        EXPECT_EQ(srcRoI.x1, -c.e);
        EXPECT_EQ(srcRoI.y1, -c.e);
        EXPECT_EQ(srcRoI.x2, 10. + c.e);
        EXPECT_EQ(srcRoI.y2, 10. + c.e);

        // PAR 2 at half scale: a pixel is 4 canonical units wide and 2 high.
        srcRoI = roi;
        expandRoI(roi, 2., 0.5, 0.5, c.f, false, 1., &srcRoI);
        EXPECT_EQ(srcRoI.x1, -4. * c.e);
        EXPECT_EQ(srcRoI.y1, -2. * c.e);
    }

    // The reach is radius * scale - 0.5 along each source axis; Impulse and Box ignore the scale.
    EXPECT_EQ(filterRoIExpansion(eFilterLanczos3, 4.), 11.5);
    EXPECT_EQ(filterRoIExpansion(FilterSpec(eFilterSinc, 8), 2.), 15.5);
    EXPECT_EQ(filterRoIExpansion(eFilterLanczos3, 0.25), 2.5);
    EXPECT_EQ(filterRoIExpansion(eFilterImpulse, 4.), 0.);
    EXPECT_EQ(filterRoIExpansion(eFilterBox, 4.), 0.);
    EXPECT_EQ(filterRoIExpansion(eFilterLanczos3, std::numeric_limits<double>::infinity()), 2.5);
    {
        RectD wide = roi;
        expandRoI(roi, 1., 1., 1., eFilterLanczos3, false, 1., &wide, 4., 1.);
        EXPECT_EQ(wide.x1, -11.5);
        EXPECT_EQ(wide.x2, 21.5);
        EXPECT_EQ(wide.y1, -2.5);
        EXPECT_EQ(wide.y2, 12.5);
    }

    // Masking or mixing also needs the source under the output window.
    RectD srcRoI(100., 100., 110., 110.);
    expandRoI(roi, 1., 1., 1., eFilterImpulse, true, 1., &srcRoI);
    EXPECT_EQ(srcRoI.x1, 0.);
    EXPECT_EQ(srcRoI.x2, 110.);
    srcRoI = RectD(100., 100., 110., 110.);
    expandRoI(roi, 1., 1., 1., eFilterImpulse, false, 0.5, &srcRoI);
    EXPECT_EQ(srcRoI.y1, 0.);

    // Infinite edges stay infinite.
    RectD inf(kOfxFlagInfiniteMin, 0., kOfxFlagInfiniteMax, 10.);
    expandRoI(roi, 1., 1., 1., eFilterKeys, false, 1., &inf);
    EXPECT_EQ(inf.x1, (double)kOfxFlagInfiniteMin);
    EXPECT_EQ(inf.x2, (double)kOfxFlagInfiniteMax);
    EXPECT_EQ(inf.y1, -1.5);

    // RoD: one output pixel more on each side when black outside.
    RectD rod(0., 0., 100., 50.);
    expandRoD(2., 0.5, 0.5, true, &rod);
    EXPECT_EQ(rod.x1, -4.);
    EXPECT_EQ(rod.x2, 104.);
    EXPECT_EQ(rod.y1, -2.);
    EXPECT_EQ(rod.y2, 52.);
    rod = RectD(0., 0., 100., 50.);
    expandRoD(1., 1., 1., false, &rod);
    EXPECT_EQ(rod.x1, 0.);
    EXPECT_EQ(rod.x2, 100.);
}

TEST(Resampler, RegionsOfTranslation)
{
    const RectD srcRoD(0., 0., 100., 50.);
    const CanonicalTransformFn fn = movingTranslation(10.);
    RegionParams params;
    RectD rod;

    // At time 1 the image moves 10 to the right, plus one black pixel all round.
    getRegionOfDefinition(fn, srcRoD, 1., 1., 1., 1., params, &rod);
    EXPECT_NEAR(rod.x1, 9., kEps);
    EXPECT_NEAR(rod.x2, 111., kEps);
    EXPECT_NEAR(rod.y1, -1., kEps);
    EXPECT_NEAR(rod.y2, 51., kEps);

    // An identity node keeps its source RoD exactly.
    params.isIdentity = true;
    getRegionOfDefinition(fn, srcRoD, 0., 1., 1., 1., params, &rod);
    EXPECT_EQ(rod.x1, 0.);
    EXPECT_EQ(rod.x2, 100.);
    params.isIdentity = false;

    // Infinite in, infinite out; empty in, empty out.
    getRegionOfDefinition(fn, RectD(kOfxFlagInfiniteMin, kOfxFlagInfiniteMin, kOfxFlagInfiniteMax, kOfxFlagInfiniteMax), 1., 1., 1., 1., params, &rod);
    EXPECT_EQ(rod.x1, (double)kOfxFlagInfiniteMin);
    EXPECT_EQ(rod.y2, (double)kOfxFlagInfiniteMax);
    getRegionOfDefinition(fn, RectD(5., 5., 5., 5.), 1., 1., 1., 1., params, &rod);
    EXPECT_EQ(rod.x1, 0.);
    EXPECT_EQ(rod.x2, 0.);

    // The RoI is the output window moved back, plus the cubic filter's half pixel.
    RectD srcRoI;
    getRegionOfInterest(fn, RectD(0., 0., 10., 10.), srcRoD, RectD(0., 0., 1920., 1080.), 1., 1., 1., 1., 1., params, &srcRoI);
    EXPECT_NEAR(srcRoI.x1, -10.5, kEps);
    EXPECT_NEAR(srcRoI.x2, 0.5, kEps);
    EXPECT_NEAR(srcRoI.y1, -0.5, kEps);
    EXPECT_NEAR(srcRoI.y2, 10.5, kEps);

    // Motion blur over a one-frame shutter opening at 0: the union at each quarter frame,
    // grown by the largest corner move between them (2.5), then the black pixel.
    params.blur.motionBlur = 1.;
    params.blur.shutter = 1.;
    params.blur.shutterOffset = eShutterOffsetStart;
    getRegionOfDefinition(fn, srcRoD, 0., 1., 1., 1., params, &rod);
    EXPECT_NEAR(rod.x1, -3.5, kEps);
    EXPECT_NEAR(rod.x2, 113.5, kEps);
    EXPECT_NEAR(rod.y1, -3.5, kEps);
    EXPECT_NEAR(rod.y2, 53.5, kEps);
}

TEST(Resampler, RegionBehindTheCamera)
{
    // Every corner maps to z < 0: nothing is visible.
    const Mat3 behind(1., 0., 0., 0., 1., 0., 0., 0., -1.);
    RectD out(1., 2., 3., 4.);

    transformRegionFromRect(RectD(0., 0., 10., 10.), behind, &out);
    EXPECT_EQ(out.x1, 0.);
    EXPECT_EQ(out.x2, 0.);
    EXPECT_EQ(out.y1, 0.);
    EXPECT_EQ(out.y2, 0.);

    // A plane crossing z = 0 between x = 0 and x = 10 extends to infinity on that side.
    const Mat3 crossing(1., 0., 0., 0., 1., 0., -0.2, 0., 1.);
    transformRegionFromRect(RectD(0., 0., 10., 10.), crossing, &out);
    EXPECT_EQ(out.x2, (double)kOfxFlagInfiniteMax);
}

TEST(Resampler, MotionBlurSampleCounts)
{
    EXPECT_EQ(motionBlurMaxIterations(1.), 40);
    EXPECT_EQ(motionBlurMinSamples(1.), 13);
    EXPECT_EQ(motionBlurMaxIterations(0.5), 20);
    EXPECT_EQ(motionBlurMinSamples(0.5), 13);
    EXPECT_EQ(motionBlurMaxIterations(4.), 160);
    EXPECT_EQ(motionBlurMinSamples(4.), 53);
    EXPECT_DOUBLE_EQ(motionBlurMaxError(1.), 0.001);
    EXPECT_EQ(vanDerCorput2(0), 0.);
    EXPECT_EQ(vanDerCorput2(1), 0.5);
    EXPECT_EQ(vanDerCorput2(2), 0.25);
    EXPECT_EQ(vanDerCorput2(3), 0.75);

    double t0, t1;
    shutterRange(10., 0.5, eShutterOffsetCentered, 0., &t0, &t1);
    EXPECT_EQ(t0, 9.75);
    EXPECT_EQ(t1, 10.25);
    shutterRange(10., 0.5, eShutterOffsetStart, 0., &t0, &t1);
    EXPECT_EQ(t0, 10.);
    EXPECT_EQ(t1, 10.5);
    shutterRange(10., 0.5, eShutterOffsetEnd, 0., &t0, &t1);
    EXPECT_EQ(t0, 9.5);
    EXPECT_EQ(t1, 10.);
    shutterRange(10., 0.5, eShutterOffsetCustom, -2., &t0, &t1);
    EXPECT_EQ(t0, 8.);
    EXPECT_EQ(t1, 8.5);

    BlurSettings blur;
    blur.motionBlur = 1.;
    blur.shutter = 1.;
    blur.shutterOffset = eShutterOffsetStart;

    // A static transform collapses to one matrix and renders without blur.
    SamplingTransforms still;
    buildSamplingTransforms(movingTranslation(0.), 0., false, blur, 1., 1., false, 1., 1., &still);
    EXPECT_EQ(still.invTransforms.size(), 1u);
    EXPECT_EQ(still.motionBlur, 0.);

    SamplingTransforms moving;
    buildSamplingTransforms(movingTranslation(10.), 0., false, blur, 1., 1., false, 1., 1., &moving);
    ASSERT_EQ(moving.invTransforms.size(), (size_t)kMotionBlurTransformCount);
    EXPECT_EQ(moving.motionBlur, 1.);
    EXPECT_TRUE(moving.alphas.empty());
    // The first matrix is at the shutter's opening, the last at its close.
    EXPECT_NEAR(moving.invTransforms.front()(0, 2), 0., kEps);
    EXPECT_NEAR(moving.invTransforms.back()(0, 2), -10., kEps);

    const ResampleParams params = makeResampleParams(moving, eFilterBilinear, false, true);
    const RectI bounds(0, 0, 64, 64);
    int samples = -1;
    float v = -1.f;

    // A flat image has no variance: the stratified minimum is enough.
    std::vector<float> flat = makeImage(bounds, [](int, int) {
        return 0.5f;
    });
    motionBlurPixel(params, SourceImage(&flat.front(), bounds, 1), 30, 30, &v, &samples);
    EXPECT_EQ(samples, motionBlurMinSamples(1.));
    EXPECT_NEAR(v, 0.5, 1e-6);

    // A checkerboard sweeping under the pixel has a large variance: sampling runs to the cap.
    std::vector<float> board = makeImage(bounds, checker);
    motionBlurPixel(params, SourceImage(&board.front(), bounds, 1), 30, 30, &v, &samples);
    EXPECT_EQ(samples, motionBlurMaxIterations(1.));
    EXPECT_GT(v, 0.f);
    EXPECT_LT(v, 1.f);

    // resampleRow gives the same pixel, whatever the row extent.
    float row[3];
    resampleRow(params, SourceImage(&board.front(), bounds, 1), 30, 29, 32, row);
    EXPECT_EQ(row[1], v);
}

TEST(Resampler, DirectionalBlurTransforms)
{
    BlurSettings blur;
    blur.directionalBlur = true;
    blur.motionBlur = 1.;
    TransformParams p;
    p.translateX = 10.;
    const CanonicalTransformFn fn = [p](double, double amount, bool invert, Mat3* m) {
        *m = TransformMath::inverseTransformCanonical(p, amount, invert);
        return true;
    };

    SamplingTransforms t;
    buildSamplingTransforms(fn, 0., false, blur, 1., 1., false, 1., 1., &t);
    ASSERT_EQ(t.invTransforms.size(), (size_t)kMotionBlurTransformCount);
    ASSERT_EQ(t.alphas.size(), (size_t)kMotionBlurTransformCount);
    EXPECT_EQ(t.alphas.front(), 1.);
    EXPECT_EQ(t.motionBlur, 1.);
    // Amounts run from 1 - 1/1000 down to 0.
    EXPECT_NEAR(t.invTransforms.front()(0, 2), -9.99, 1e-9);
    EXPECT_NEAR(t.invTransforms.back()(0, 2), 0., kEps);
}

TEST(Resampler, ResampleRowIdentityAndShift)
{
    const RectI bounds(0, 0, 16, 4);
    std::vector<float> data = makeImage(bounds, [](int x, int y) {
        return 0.25f * x + 0.125f * y;
    });
    const SourceImage src(&data.front(), bounds, 1);

    SamplingTransforms identity;
    identity.invTransforms.push_back(Mat3::identity());
    for (FilterEnum f : kInterpolatingFilters) {
        const ResampleParams params = makeResampleParams(identity, f, false, true);
        std::vector<float> row(16, -1.f);
        resampleRow(params, src, 2, 0, 16, &row.front());
        for (int x = 0; x < 16; ++x) {
            EXPECT_NEAR(row[x], 0.25f * x + 0.25f, 1e-6) << "filter " << f << " x " << x;
        }
    }

    // A whole-pixel shift right: destination x reads source x - 3, black around the image.
    SamplingTransforms shift;
    buildSamplingTransforms(movingTranslation(3.), 1., false, BlurSettings(), 1., 1., false, 1., 1., &shift);
    {
        const ResampleParams params = makeResampleParams(shift, eFilterCubic, false, true);
        std::vector<float> row(20, -1.f);
        resampleRow(params, src, 1, 0, 20, &row.front());
        EXPECT_EQ(row[0], 0.f);
        EXPECT_EQ(row[1], 0.f);
        for (int x = 3; x < 19; ++x) {
            EXPECT_NEAR(row[x], 0.25f * (x - 3) + 0.125f, 1e-6) << "x " << x;
        }
        EXPECT_EQ(row[19], 0.f);
    }
    {
        // Without black outside the edge pixels repeat.
        const ResampleParams params = makeResampleParams(shift, eFilterCubic, false, false);
        std::vector<float> row(20, -1.f);
        resampleRow(params, src, 1, 0, 20, &row.front());
        EXPECT_NEAR(row[0], 0.125f, 1e-6);
        EXPECT_NEAR(row[19], 0.25f * 15 + 0.125f, 1e-6);
    }

    // A half-pixel shift with bilinear averages neighbours.
    SamplingTransforms half;
    buildSamplingTransforms(movingTranslation(0.5), 1., false, BlurSettings(), 1., 1., false, 1., 1., &half);
    {
        const ResampleParams params = makeResampleParams(half, eFilterBilinear, false, true);
        std::vector<float> row(16, -1.f);
        resampleRow(params, src, 0, 0, 16, &row.front());
        for (int x = 1; x < 16; ++x) {
            EXPECT_NEAR(row[x], 0.25f * (x - 0.5f), 1e-6) << "x " << x;
        }
    }

    // A matrix sending every point behind the camera renders black.
    SamplingTransforms behind;
    behind.invTransforms.push_back(Mat3(1., 0., 0., 0., 1., 0., 0., 0., -1.));
    {
        const ResampleParams params = makeResampleParams(behind, eFilterCubic, false, false);
        std::vector<float> row(16, -1.f);
        resampleRow(params, src, 0, 0, 16, &row.front());
        for (int x = 0; x < 16; ++x) {
            EXPECT_EQ(row[x], 0.f);
        }
    }
}

TEST(Resampler, ResampleRowMultiChannel)
{
    const RectI bounds(0, 0, 8, 8);
    std::vector<float> data((size_t)bounds.width() * bounds.height() * 4);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = (float)(i % 4) * 0.1f + (float)(i / 32) * 0.01f;
    }
    const SourceImage src(&data.front(), bounds, 4);
    SamplingTransforms identity;
    identity.invTransforms.push_back(Mat3::identity());
    const ResampleParams params = makeResampleParams(identity, eFilterKeys, true, true);
    std::vector<float> row(8 * 4, -1.f);

    resampleRow(params, src, 5, 0, 8, &row.front());
    for (int x = 0; x < 8; ++x) {
        for (int c = 0; c < 4; ++c) {
            EXPECT_NEAR(row[x * 4 + c], src.pixel(x, 5)[c], 1e-6);
        }
    }
}

// Scaling a matrix by 2 is exact and leaves every sample unchanged, but its last row is no longer
// (0, 0, 1), so the scaled copy goes through the projective per-pixel path while the original
// takes the affine one. Both must agree bit for bit.
TEST(Resampler, AffinePathMatchesProjectivePath)
{
    const RectI bounds(-3, 2, 37, 31);
    const int nCompsList[] = { 1, 4 };
    for (int nComps : nCompsList) {
        std::vector<float> data((size_t)bounds.width() * bounds.height() * nComps);
        for (size_t i = 0; i < data.size(); ++i) {
            data[i] = (float)std::sin(0.37 * (double)i) * 0.75f + 0.5f;
        }
        const SourceImage src(&data.front(), bounds, nComps);

        std::vector<Mat3> matrices;
        TransformParams p;
        p.translateX = 3.3;
        p.translateY = -1.7;
        p.rotate = 30.;
        p.scaleX = 0.5;
        p.scaleY = 0.5;
        p.centerX = 17.;
        p.centerY = 15.;
        matrices.push_back(TransformMath::inverseTransformCanonical(p, 1., false));
        p.scaleX = 2.25;
        p.scaleY = 1.5;
        p.skewX = 0.3;
        p.skewY = -0.2;
        matrices.push_back(TransformMath::inverseTransformCanonical(p, 1., false));
        p = TransformParams();
        p.scaleX = 0.125;
        p.scaleY = 0.2;
        p.rotate = -12.;
        matrices.push_back(TransformMath::inverseTransformCanonical(p, 1., false));
        matrices.push_back(Mat3(1., 0., -0.25, 0., 1., 0.5, 0., 0., 1.));

        for (const Mat3& affine : matrices) {
            ASSERT_EQ(affine(2, 0), 0.);
            ASSERT_EQ(affine(2, 1), 0.);
            ASSERT_EQ(affine(2, 2), 1.);
            Mat3 projective = affine;
            for (int k = 0; k < 9; ++k) {
                projective.m[k] *= 2.;
            }
            SamplingTransforms a;
            a.invTransforms.push_back(affine);
            SamplingTransforms b;
            b.invTransforms.push_back(projective);
            const int x1 = -8;
            const int x2 = 45;
            const size_t n = (size_t)(x2 - x1) * nComps;
            for (FilterEnum f : kAllFilters) {
                for (int clampIndex = 0; clampIndex < 2; ++clampIndex) {
                    for (int blackIndex = 0; blackIndex < 2; ++blackIndex) {
                        const bool clamp = clampIndex != 0;
                        const bool blackOutside = blackIndex != 0;
                        const ResampleParams pa = makeResampleParams(a, f, clamp, blackOutside);
                        const ResampleParams pb = makeResampleParams(b, f, clamp, blackOutside);
                        for (int y = -2; y < 36; y += 3) {
                            std::vector<float> rowA(n, -1.f);
                            std::vector<float> rowB(n, -2.f);
                            resampleRow(pa, src, y, x1, x2, &rowA.front());
                            resampleRow(pb, src, y, x1, x2, &rowB.front());
                            for (size_t i = 0; i < n; ++i) {
                                ASSERT_EQ(rowA[i], rowB[i]) << "filter " << f << " clamp " << clamp << " blackOutside " << blackOutside << " nComps " << nComps << " y " << y << " i " << i;
                            }
                        }
                    }
                }
            }
        }
    }
}

TEST(TransformMath, ComposeSkewOrders)
{
    TransformParams p;
    p.translateX = 10.;
    p.translateY = 20.;
    p.scaleX = 2.;
    p.scaleY = 3.;
    p.skewX = 0.5;
    p.skewY = 0.25;
    p.centerX = 5.;
    p.centerY = 5.;

    // XY: skew [[1 + sx sy, sx], [sy, 1]] after scale, about the centre, then translate.
    p.skewOrderYX = false;
    const Mat3 fwdXY = TransformMath::inverseTransformCanonical(p, 1., true);
    expectMatrixNear(fwdXY, Mat3(2.25, 1.5, -3.75, 0.5, 3., 7.5, 0., 0., 1.), kEps);

    // YX: skew [[1, sx], [sy, 1 + sx sy]].
    p.skewOrderYX = true;
    const Mat3 fwdYX = TransformMath::inverseTransformCanonical(p, 1., true);
    expectMatrixNear(fwdYX, Mat3(2., 1.5, -2.5, 0.5, 3.375, 5.625, 0., 0., 1.), kEps);

    // The inverse matrix undoes the forward one for both orders.
    for (bool yx : { false, true }) {
        p.skewOrderYX = yx;
        const Mat3 fwd = TransformMath::inverseTransformCanonical(p, 1., true);
        const Mat3 inv = TransformMath::inverseTransformCanonical(p, 1., false);
        expectMatrixNear(inv * fwd, Mat3::identity(), 1e-12);
        Mat3 inverted;
        ASSERT_TRUE(fwd.inverse(&inverted));
        expectMatrixNear(inverted, inv, 1e-12);
    }
}

TEST(TransformMath, RotationAmountAndScale)
{
    // A positive rotation turns counter-clockwise: (1, 0) goes to (0, 1).
    TransformParams r;
    r.rotate = 90.;
    const Mat3 fwd = TransformMath::inverseTransformCanonical(r, 1., true);
    expectMatrixNear(fwd, Mat3(0., -1., 0., 1., 0., 0., 0., 0., 1.), 1e-15);

    // Half the amount: translate halves, scale interpolates geometrically.
    TransformParams p;
    p.translateX = 8.;
    p.scaleX = 4.;
    p.scaleY = 9.;
    p.amount = 0.5;
    TransformParams q;
    q.translateX = 4.;
    q.scaleX = 2.;
    q.scaleY = 3.;
    expectMatrixNear(TransformMath::inverseTransformCanonical(p, 1., true), TransformMath::inverseTransformCanonical(q, 1., true), 1e-12);
    // The blend factor multiplies the amount knob.
    p.amount = 1.;
    expectMatrixNear(TransformMath::inverseTransformCanonical(p, 0.5, true), TransformMath::inverseTransformCanonical(q, 1., true), 1e-12);

    // Uniform scale uses scaleX on both axes; tiny scales are kept away from zero.
    double sx, sy;
    TransformMath::effectiveScale(2., 5., true, &sx, &sy);
    EXPECT_EQ(sx, 2.);
    EXPECT_EQ(sy, 2.);
    TransformMath::effectiveScale(0., -0.00001, false, &sx, &sy);
    EXPECT_EQ(sx, 0.0001);
    EXPECT_EQ(sy, -0.0001);

    TransformParams neutral;
    EXPECT_TRUE(TransformMath::isIdentity(neutral));
    neutral.centerX = 50.;
    EXPECT_TRUE(TransformMath::isIdentity(neutral));
    neutral.skewY = 0.1;
    EXPECT_FALSE(TransformMath::isIdentity(neutral));
    neutral.amount = 0.;
    EXPECT_TRUE(TransformMath::isIdentity(neutral));
}

TEST(TransformMath, PixelConversion)
{
    // PAR 2 at half scale: canonical x = 4 px, canonical y = 2 px.
    const Mat3 invCanonical = TransformMath::translation(-10., -20.);
    const Mat3 invPixel = TransformMath::inverseToPixel(invCanonical, 2., 2., 0.5, 0.5);
    expectMatrixNear(invPixel, Mat3(1., 0., -2.5, 0., 1., -10., 0., 0., 1.), kEps);

    const Mat3 fwdPixel = TransformMath::forwardToPixel(TransformMath::translation(10., 20.), 2., 2., 0.5, 0.5);
    expectMatrixNear(fwdPixel, Mat3(1., 0., 2.5, 0., 1., 10., 0., 0., 1.), kEps);

    // Fielded renders halve the vertical scale.
    expectMatrixNear(TransformMath::canonicalToPixel(1., 1., 1., true), TransformMath::scale(1., 0.5), kEps);

    const Mat3 m(1., 2., 3., 4., 5., 6., 7., 8., 10.);
    EXPECT_TRUE(TransformMath::fromEngineMatrix(TransformMath::toEngineMatrix(m)) == m);
    Mat3 singular(1., 2., 3., 2., 4., 6., 0., 0., 1.);
    Mat3 untouched = Mat3::identity();
    EXPECT_FALSE(singular.inverse(&untouched));
    EXPECT_TRUE(untouched.isIdentity());

    // An input that already shifted by +5 px makes the sampler look 5 px further back.
    SamplingTransforms t;
    t.invTransforms.push_back(Mat3::identity());
    concatenateInputTransform(TransformMath::translation(5., 0.), &t);
    expectMatrixNear(t.invTransforms[0], TransformMath::translation(-5., 0.), kEps);
}

namespace {
// The inverse (destination to source) pixel matrix of a uniform scale about the origin.
Mat3
scaling(double s)
{
    return Mat3(s, 0., 0., 0., s, 0., 0., 0., 1.);
}

void
renderRows(const Mat3& inverse,
           const FilterSpec& filter,
           bool clamp,
           bool blackOutside,
           const SourceImage& src,
           int y,
           int x1,
           int x2,
           std::vector<float>* out)
{
    SamplingTransforms t;

    t.invTransforms.push_back(inverse);
    const ResampleParams params = makeResampleParams(t, filter, clamp, blackOutside);
    out->assign((size_t)(x2 - x1) * src.nComps, -1.f);
    resampleRow(params, src, y, x1, x2, &out->front());
}

CanonicalTransformFn
fixedMatrix(const Mat3& m)
{
    return [m](double, double, bool, Mat3* out) {
        *out = m;
        return true;
    };
}

CanonicalTransformFn
scalingFn(double sx,
          double sy)
{
    return [sx, sy](double, double amount, bool invert, Mat3* out) {
        TransformParams p;
        p.scaleX = sx;
        p.scaleY = sy;
        *out = TransformMath::inverseTransformCanonical(p, amount, invert);
        return true;
    };
}
} // anon namespace

TEST(Resampler, KernelsAreNormalisedAndSymmetric)
{
    for (const FilterSpec& spec : kernelSpecs()) {
        for (double x : { 0.1, 0.5, 0.9, 1.3, 1.7 }) {
            EXPECT_NEAR(spec.weight(x), spec.weight(-x), 1e-12) << "filter " << spec.filter << " x " << x;
        }
        EXPECT_EQ(spec.weight(spec.radius() + 0.1), 0.) << "filter " << spec.filter;
        EXPECT_EQ(spec.weight(-spec.radius() - 0.1), 0.) << "filter " << spec.filter;
    }
}

TEST(Resampler, SincFamilyKernels)
{
    const FilterSpec specs[] = {
        FilterSpec(eFilterLanczos2),
        FilterSpec(eFilterLanczos3),
        FilterSpec(eFilterLanczos4),
        FilterSpec(eFilterSinc, 2),
        FilterSpec(eFilterSinc, 4),
        FilterSpec(eFilterSinc, 8),
    };

    for (const FilterSpec& spec : specs) {
        EXPECT_EQ(spec.weight(0.), 1.) << "filter " << spec.filter << " lobes " << spec.lobes;
        for (int n = 1; n < (int)spec.radius(); ++n) {
            EXPECT_NEAR(spec.weight((double)n), 0., 1e-12) << "filter " << spec.filter << " n " << n;
        }
        EXPECT_NEAR(spec.weight(spec.radius() - 1e-9), 0., 1e-6) << "filter " << spec.filter;
    }

    EXPECT_EQ(FilterSpec(eFilterLanczos2).radius(), 2.);
    EXPECT_EQ(FilterSpec(eFilterLanczos3).radius(), 3.);
    EXPECT_EQ(FilterSpec(eFilterLanczos4).radius(), 4.);
    EXPECT_EQ(FilterSpec(eFilterSinc, 5).radius(), 5.);
    EXPECT_EQ(FilterSpec(eFilterSinc, 1).radius(), 2.);
    EXPECT_EQ(FilterSpec(eFilterSinc, 12).radius(), 8.);
    EXPECT_EQ(filterTaps(FilterSpec(eFilterSinc, 3)), 6);
}

// The clamp range counts any nonzero tap, so these must not depend on a position's last bits.
TEST(Resampler, KernelsWithADoubleRootAtTheEdgeVanishInTheBand)
{
    const FilterSpec specs[] = {
        FilterSpec(eFilterKeys),
        FilterSpec(eFilterSimon),
        FilterSpec(eFilterRifman),
        FilterSpec(eFilterMitchell),
        FilterSpec(eFilterBicubic),
        FilterSpec(eFilterBicubic, 4, 1., 0.),
        FilterSpec(eFilterBicubic, 4, 0.25, 0.),
        FilterSpec(eFilterBicubic, 4, 0., 0.5),
        FilterSpec(eFilterSinc, 2),
        FilterSpec(eFilterSinc, 4),
        FilterSpec(eFilterSinc, 8),
    };

    for (const FilterSpec& spec : specs) {
        const double r = spec.radius();
        for (int k = 1; k < 256; ++k) {
            EXPECT_EQ(spec.weight(r - k * 0x1p-50), 0.) << "filter " << spec.filter << " lobes " << spec.lobes << " k " << k;
        }
        EXPECT_EQ(spec.weight(r - 0x1.fp-27), 0.) << "filter " << spec.filter << " lobes " << spec.lobes;
        for (double outside : { 0x1p-26, 0x1.4p-25, 0x1p-24 }) {
            EXPECT_NE(spec.weight(r - outside), 0.) << "filter " << spec.filter << " lobes " << spec.lobes << " distance " << outside;
        }
    }
    EXPECT_NE(FilterSpec(eFilterLanczos3).weight(3. - 0x1p-50), 0.);
}

TEST(Resampler, BicubicMatchesTheNamedCubics)
{
    struct Case {
        FilterSpec spec;
        FilterEnum named;
    };
    const Case cases[] = {
        { FilterSpec(eFilterBicubic, 4, 0., 0.5), eFilterKeys },
        { FilterSpec(eFilterBicubic, 4, 1. / 3., 1. / 3.), eFilterMitchell },
        { FilterSpec(eFilterBicubic), eFilterMitchell },
    };

    for (const Case& c : cases) {
        for (double d : { 0., 0.2, 0.5, 0.8 }) {
            double w[4];
            filterWeights(c.named, d, w);
            EXPECT_NEAR(c.spec.weight(d + 1.), w[0], 1e-12) << "d " << d;
            EXPECT_NEAR(c.spec.weight(d), w[1], 1e-12) << "d " << d;
            EXPECT_NEAR(c.spec.weight(1. - d), w[2], 1e-12) << "d " << d;
            EXPECT_NEAR(c.spec.weight(2. - d), w[3], 1e-12) << "d " << d;
        }
    }
}

TEST(Resampler, SincFamilyReproducesTheSourceAtOneToOne)
{
    const RectI bounds(0, 0, 40, 40);
    std::vector<float> data = makeImage(bounds, [](int x, int y) {
        return (float)(0.5 + 0.4 * std::sin(0.9 * x + 0.37 * y));
    });
    const SourceImage src(&data.front(), bounds, 1);

    for (FilterEnum f : { eFilterLanczos2, eFilterLanczos3, eFilterLanczos4, eFilterSinc }) {
        for (bool clamp : { false, true }) {
            for (bool black : { false, true }) {
                std::vector<float> row;
                renderRows(Mat3(1., 0., -3., 0., 1., 2., 0., 0., 1.), f, clamp, black, src, 20, 10, 30, &row);
                for (int x = 10; x < 30; ++x) {
                    EXPECT_NEAR(row[(size_t)(x - 10)], data[(size_t)22 * 40 + (x - 3)], 1e-6) << "filter " << f << " clamp " << clamp << " x " << x;
                }
            }
        }
    }
}

TEST(Resampler, ConstantImagesStayConstantWhenMinified)
{
    const RectI bounds(0, 0, 128, 128);
    std::vector<float> data = makeImage(bounds, [](int, int) {
        return 0.75f;
    });
    const SourceImage src(&data.front(), bounds, 1);

    std::vector<Mat3> matrices;
    matrices.push_back(scaling(4.));
    matrices.push_back(scaling(0.5));
    matrices.push_back(scaling(64.));
    matrices.push_back(Mat3(5., 3., 0., -3., 5., 0., 0., 0., 1.));
    TransformParams p;
    p.rotate = 30.;
    p.scaleX = 0.25;
    p.scaleY = 0.25;
    matrices.push_back(TransformMath::inverseTransformCanonical(p, 1., false));

    for (const Mat3& m : matrices) {
        for (const FilterSpec& spec : kernelSpecs()) {
            for (int y : { 8, 16 }) {
                std::vector<float> row;
                renderRows(m, spec, false, false, src, y, -8, 24, &row);
                for (size_t i = 0; i < row.size(); ++i) {
                    ASSERT_NEAR(row[i], 0.75f, 1e-5) << "filter " << spec.filter << " scale " << m(0, 0) << " y " << y << " i " << i;
                }
            }
        }
    }
}

TEST(Resampler, BicubicOuterLobeFollowsItsFactoredForm)
{
    const double pairs[][2] = { { 1., 0. }, { 0.25, 0. }, { 0., 0.5 }, { 1. / 3., 1. / 3. }, { 0.7, 0.2 } };

    for (const auto& pair : pairs) {
        const FilterSpec spec(eFilterBicubic, 4, pair[0], pair[1]);
        for (double e : { 0x1p-25, 0x1p-20, 0x1p-18, 0x1p-10, 0.25, 0.5, 0.75 }) {
            const double expected = e * e * (pair[0] * e + 6. * pair[1] * (e - 1.)) / 6.;
            EXPECT_DOUBLE_EQ(spec.weight(2. - e), expected) << "B " << pair[0] << " C " << pair[1] << " e " << e;
            EXPECT_NE(spec.weight(2. - e), 0.) << "B " << pair[0] << " C " << pair[1] << " e " << e;
        }
    }
}

TEST(Resampler, BicubicWithBAndCZeroHasNoOuterLobe)
{
    const FilterSpec spec(eFilterBicubic, 4, 0., 0.);

    for (double a : { 1., 1. + 0x1p-40, 1.25, 1.5, 1.75, 2. - 0x1p-20, 2. - 0x1p-30 }) {
        EXPECT_EQ(spec.weight(a), 0.) << "a " << a;
    }
    EXPECT_DOUBLE_EQ(spec.weight(0.25), FilterSpec(eFilterCubic).weight(0.25));
    EXPECT_DOUBLE_EQ(spec.weight(0.75), FilterSpec(eFilterCubic).weight(0.75));

    // A bright pixel under the outer lobe has no say: the result is where the plain cubic puts it.
    const int w = 8;
    const int h = 8;
    std::vector<float> pixels((std::size_t)w * h, 0.25f);
    pixels[(std::size_t)4 * w + 3] = 10.f;
    pixels[(std::size_t)4 * w + 5] = 0.75f;
    const SourceImage src(pixels.data(), RectI(0, 0, w, h), 1);
    float clamped = 0.f;
    float plain = 0.f;
    ASSERT_TRUE(interpolate(spec, true, 4.75, 4.5, src, false, &clamped));
    ASSERT_TRUE(interpolate(FilterSpec(eFilterCubic), false, 4.75, 4.5, src, false, &plain));
    EXPECT_NEAR(clamped, plain, 1e-6f);
}

TEST(Resampler, MinifiedBlackOutsideFadesAtTheSourceEdge)
{
    const RectI bounds(0, 0, 128, 128);
    std::vector<float> data = makeImage(bounds, [](int, int) {
        return 1.f;
    });
    const SourceImage src(&data.front(), bounds, 1);
    std::vector<float> row;

    // Output pixel -1 is centred past the source, so its zeroed row takes the plain kernel, which
    // reaches only the nearest source column. Pixel 0, 1.5 source pixels in, loses just the
    // kernel's tail: the fade spans one output pixel, not the wide footprint.
    renderRows(scaling(4.), eFilterLanczos3, false, true, src, 16, -1, 24, &row);
    EXPECT_NEAR(row[0], 0.0245f, 1e-3);
    EXPECT_NEAR(row[1], 0.9353f, 1e-3);
    EXPECT_NEAR(row[17], 1.f, 1e-4);
}

TEST(Resampler, MinificationRemovesAliasing)
{
    const RectI bounds(0, 0, 128, 128);
    std::vector<float> data = makeImage(bounds, checker);
    const SourceImage src(&data.front(), bounds, 1);

    struct Case {
        FilterSpec spec;
        double maxEnergy;
        double minEnergy;
    };
    const Case cases[] = {
        { FilterSpec(eFilterLanczos3), 1e-2, 0. },
        { FilterSpec(eFilterGaussian), 1e-2, 0. },
        { FilterSpec(eFilterImpulse), 1., 0.25 },
    };

    for (const Case& c : cases) {
        double energy = 0.;
        int n = 0;
        for (int y = 8; y < 24; ++y) {
            std::vector<float> row;
            renderRows(scaling(4.), c.spec, false, false, src, y, 8, 24, &row);
            for (float v : row) {
                energy += (v - 0.5) * (v - 0.5);
                ++n;
            }
        }
        energy /= n;
        EXPECT_LE(energy, c.maxEnergy) << "filter " << c.spec.filter;
        EXPECT_GE(energy, c.minEnergy - 1e-9) << "filter " << c.spec.filter;
    }
}

TEST(Resampler, ClampHoldsTheSharpFiltersInsideTheSourceRange)
{
    const RectI bounds(0, 0, 32, 32);
    std::vector<float> data = makeImage(bounds, [](int x, int) {
        return x < 16 ? 0.f : 1.f;
    });
    const SourceImage src(&data.front(), bounds, 1);
    const Mat3 halfPixel(1., 0., -0.5, 0., 1., 0., 0., 0., 1.);
    const FilterSpec specs[] = {
        FilterSpec(eFilterLanczos2),
        FilterSpec(eFilterLanczos3),
        FilterSpec(eFilterLanczos4),
        FilterSpec(eFilterSinc),
        FilterSpec(eFilterBicubic, 4, 0., 0.5),
    };

    for (const FilterSpec& spec : specs) {
        std::vector<float> clamped;
        std::vector<float> loose;
        renderRows(halfPixel, spec, true, false, src, 16, 6, 26, &clamped);
        renderRows(halfPixel, spec, false, false, src, 16, 6, 26, &loose);
        float lo = 0.f;
        float hi = 1.f;
        for (size_t i = 0; i < clamped.size(); ++i) {
            EXPECT_GE(clamped[i], 0.f) << "filter " << spec.filter;
            EXPECT_LE(clamped[i], 1.f) << "filter " << spec.filter;
            lo = (std::min)(lo, loose[i]);
            hi = (std::max)(hi, loose[i]);
        }
        EXPECT_TRUE(lo < -1e-3f || hi > 1.f + 1e-3f) << "filter " << spec.filter;
    }
}

TEST(Resampler, LobesWidenTheFootprint)
{
    const RectI bounds(0, 0, 64, 64);
    std::vector<float> data = makeImage(bounds, [](int x, int y) {
        return (x == 32 && y == 20) ? 1.f : 0.f;
    });
    const SourceImage src(&data.front(), bounds, 1);

    for (int lobes = 2; lobes <= 8; ++lobes) {
        std::vector<float> row;
        renderRows(Mat3(1., 0., -0.5, 0., 1., 0., 0., 0., 1.), FilterSpec(eFilterSinc, lobes), false, false, src, 20, 0, 64, &row);
        EXPECT_NE(row[(size_t)(32 + lobes)], 0.f) << "lobes " << lobes;
        EXPECT_EQ(row[(size_t)(32 + lobes + 1)], 0.f) << "lobes " << lobes;
        EXPECT_NE(row[(size_t)(32 - lobes + 1)], 0.f) << "lobes " << lobes;
        EXPECT_EQ(row[(size_t)(32 - lobes)], 0.f) << "lobes " << lobes;
    }
}

TEST(Resampler, WideMinificationStaysWithinTheTapBudget)
{
    const RectI bounds(0, 0, 742, 1024);
    std::vector<float> data = makeImage(bounds, [](int, int) {
        return 1.f;
    });
    const SourceImage src(&data.front(), bounds, 1);
    const FilterSpec spec(eFilterLanczos3);
    const double scale = 64.;
    const int destX = 10;
    std::vector<float> row;

    renderRows(scaling(scale), spec, false, true, src, 8, destX, destX + 1, &row);

    // The x taps spread over [centre - R, centre + R] at (i + 0.5) * 2R / 81 with bilinear reads;
    // y lies wholly inside the source.
    const double centre = (destX + 0.5) * scale - 0.5;
    const double reach = spec.radius() * scale;
    const int taps = kFilterMaxTapsPerAxis;
    double sum = 0.;
    double inside = 0.;
    for (int i = 0; i < taps; ++i) {
        const double offset = (i + 0.5) * 2. * reach / taps - reach;
        const double position = centre + offset;
        const double w = spec.weight(offset / scale);
        const double lo = std::floor(position);
        const double frac = position - lo;
        auto in = [&](double k) {
            return (k >= 0. && k < 742.) ? 1. : 0.;
        };
        sum += w;
        inside += w * (in(lo) * (1. - frac) + in(lo + 1.) * frac);
    }
    const double expected = inside / sum;

    EXPECT_NEAR(row[0], expected, 1e-3);
    EXPECT_GT(std::fabs(expected - 1.), 0.01);
}

TEST(Resampler, KernelScalePerSourceAxis)
{
    EXPECT_EQ(kernelScale(0., 4.), 4.);
    EXPECT_EQ(kernelScale(1., 0.), 1.);
    EXPECT_NEAR(kernelScale(std::cos(0.785398163397448), -std::sin(0.785398163397448)), 1., 1e-12);
    EXPECT_EQ(kernelScale(0., 0.), 1.);
    EXPECT_EQ(kernelScale(0.3, 0.2), 1.);
    EXPECT_EQ(kernelScale(std::numeric_limits<double>::quiet_NaN(), 3.), 1.);
    EXPECT_EQ(kernelScale(std::numeric_limits<double>::infinity(), 0.), 1.);

    const RectI bounds(0, 0, 64, 64);
    std::vector<float> data = makeImage(bounds, [](int x, int) {
        return (float)(x % 2);
    });
    const SourceImage src(&data.front(), bounds, 1);
    float v = -1.f;

    // Row x wide: the stripes average away. Only the y row wide, or a rotation, leaves x plain.
    interpolateSuper(eFilterLanczos3, false, 20.5, 20.5, 4., 0., 0., 1., src, false, &v);
    EXPECT_NEAR(v, 0.5, 0.1);
    interpolateSuper(eFilterLanczos3, false, 20.5, 20.5, 1., 0., 0., 4., src, false, &v);
    EXPECT_NEAR(v, 0., 1e-6);
    interpolateSuper(eFilterLanczos3, false, 20.5, 20.5, 0.70710678, -0.70710678, 0.70710678, 0.70710678, src, false, &v);
    EXPECT_NEAR(v, 0., 1e-6);
}

TEST(Resampler, RoIGrowsWithTheKernelScale)
{
    RegionParams params;
    params.filter = FilterSpec(eFilterLanczos3);
    const RectD srcRoD(0., 0., 256., 256.);
    const RectD window(16., 16., 48., 48.);
    RectD srcRoI;

    // A 4x minification reads 3 * 4 - 0.5 beyond the back-transformed window on both axes.
    getRegionOfInterest(scalingFn(0.25, 0.25), window, srcRoD, RectD(0., 0., 1920., 1080.), 0., 1., 1., 1., 1., params, &srcRoI);
    EXPECT_NEAR(srcRoI.x1, 52.5, 1e-9);
    EXPECT_NEAR(srcRoI.x2, 203.5, 1e-9);
    EXPECT_NEAR(srcRoI.y1, 52.5, 1e-9);
    EXPECT_NEAR(srcRoI.y2, 203.5, 1e-9);

    // Only the axis that minifies grows.
    getRegionOfInterest(scalingFn(0.25, 1.), window, srcRoD, RectD(0., 0., 1920., 1080.), 0., 1., 1., 1., 1., params, &srcRoI);
    EXPECT_NEAR(srcRoI.x1, 52.5, 1e-9);
    EXPECT_NEAR(srcRoI.x2, 203.5, 1e-9);
    EXPECT_NEAR(srcRoI.y1, 13.5, 1e-9);
    EXPECT_NEAR(srcRoI.y2, 50.5, 1e-9);

    // Magnification keeps the plain reach.
    getRegionOfInterest(scalingFn(2., 2.), window, srcRoD, RectD(0., 0., 1920., 1080.), 0., 1., 1., 1., 1., params, &srcRoI);
    EXPECT_NEAR(srcRoI.x1, 8. - 2.5, 1e-9);
    EXPECT_NEAR(srcRoI.x2, 24. + 2.5, 1e-9);

    // The impulse filter reads one pixel whatever the scale.
    params.filter = FilterSpec(eFilterImpulse);
    getRegionOfInterest(scalingFn(0.25, 0.25), window, srcRoD, RectD(0., 0., 1920., 1080.), 0., 1., 1., 1., 1., params, &srcRoI);
    EXPECT_NEAR(srcRoI.x1, 64., 1e-9);
    EXPECT_NEAR(srcRoI.x2, 192., 1e-9);
}

TEST(Resampler, RoIReadsAreInsideTheRegion)
{
    const RectI bounds(0, 0, 256, 256);
    std::vector<float> data = makeImage(bounds, [](int x, int y) {
        return (float)(0.5 + 0.45 * std::sin(0.31 * x) * std::cos(0.17 * y));
    });
    const SourceImage full(&data.front(), bounds, 1);

    RegionParams params;
    params.filter = FilterSpec(eFilterLanczos3);
    RectD srcRoI;
    getRegionOfInterest(scalingFn(0.25, 0.25), RectD(16., 16., 48., 48.), RectD(0., 0., 256., 256.), RectD(0., 0., 1920., 1080.), 0., 1., 1., 1., 1., params, &srcRoI);

    const RectI crop((int)std::floor(srcRoI.x1), (int)std::floor(srcRoI.y1), (int)std::ceil(srcRoI.x2), (int)std::ceil(srcRoI.y2));
    std::vector<float> cropped = makeImage(crop, [&](int x, int y) {
        return data[(size_t)y * 256 + x];
    });
    const SourceImage part(&cropped.front(), crop, 1);

    for (int y = 16; y < 48; y += 5) {
        std::vector<float> a;
        std::vector<float> b;
        renderRows(scaling(4.), params.filter, false, true, full, y, 16, 48, &a);
        renderRows(scaling(4.), params.filter, false, true, part, y, 16, 48, &b);
        for (size_t i = 0; i < a.size(); ++i) {
            ASSERT_EQ(a[i], b[i]) << "y " << y << " i " << i;
        }
    }
}

TEST(Resampler, RoIBehindTheCameraReadsTheWholeSource)
{
    RegionParams params;
    params.filter = FilterSpec(eFilterLanczos3);
    const RectD srcRoD(0., 0., 100., 50.);
    RectD srcRoI;

    getRegionOfInterest(fixedMatrix(Mat3(1., 0., 0., 0., 1., 0., -0.2, 0., 1.)), RectD(0., 0., 10., 10.), srcRoD, RectD(0., 0., 1920., 1080.), 0., 1., 1., 1., 1., params, &srcRoI);
    EXPECT_EQ(srcRoI.x1, 0.);
    EXPECT_EQ(srcRoI.x2, 100.);
    EXPECT_EQ(srcRoI.y1, 0.);
    EXPECT_EQ(srcRoI.y2, 50.);
}

TEST(Resampler, MinifiedEdgeBandFromZeroedRows)
{
    const RectI bounds(0, 0, 64, 64);

    for (int pattern = 0; pattern < 2; ++pattern) {
        const char* name = pattern == 0 ? "stripes" : "ramp";
        std::vector<float> data = makeImage(bounds, [&](int x, int) {
            return pattern == 0 ? (float)(x % 2) : (float)x / 63.f;
        });
        const SourceImage src(&data.front(), bounds, 1);
        // What a 4x minification settles to: the stripes' mean, or the ramp at the pixel's centre.
        auto expectedAt = [&](int x) {
            return pattern == 0 ? 0.5 : (4. * x + 1.5) / 63.;
        };

        for (int black = 0; black < 2; ++black) {
            std::vector<float> row;
            renderRows(scaling(4.), eFilterLanczos3, false, black != 0, src, 8, 0, 24, &row);
            double outsideDev = 0.;
            double insideDev = 0.;
            for (int x = 16; x < 24; ++x) {
                outsideDev = (std::max)(outsideDev, std::fabs((double)row[(size_t)x] - expectedAt(x)));
            }
            for (int x = 4; x < 12; ++x) {
                insideDev = (std::max)(insideDev, std::fabs((double)row[(size_t)x] - expectedAt(x)));
            }
            std::printf("M270 edge band: %s blackOutside=%d max|v-expected| outside(x>=16)=%.5f interior(x 4..11)=%.5f\n", name, black, outsideDev, insideDev);
            std::printf("M270 edge band: %s blackOutside=%d x12..20:", name, black);
            for (int x = 12; x <= 20; ++x) {
                std::printf(" %.4f", row[(size_t)x]);
            }
            std::printf("\n");
        }
    }
}
