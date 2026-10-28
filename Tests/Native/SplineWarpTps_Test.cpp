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
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Transform/SplineWarp/SplineWarpSampling.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpTps.h"

NATRON_NAMESPACE_USING

namespace {
// A deterministic generator, so a failure reproduces.
class Lcg {
public:
    explicit Lcg(std::uint64_t seed)
        : _state(seed)
    {
    }

    // Uniform in [lo, hi).
    double next(double lo,
                double hi)
    {
        _state = _state * 6364136223846793005ULL + 1442695040888963407ULL;

        return lo + (hi - lo) * (double)(_state >> 11) / 9007199254740992.;
    }

private:
    std::uint64_t _state;
};

SplineWarpSamplePair
makePair(double dx,
         double dy,
         double sx,
         double sy)
{
    SplineWarpSamplePair p;

    p.destination.x = dx;
    p.destination.y = dy;
    p.source.x = sx;
    p.source.y = sy;

    return p;
}

// Scattered destinations over a 400 px square, with a smooth non-affine displacement.
std::vector<SplineWarpSamplePair>
scattered(int n,
          std::uint64_t seed)
{
    Lcg rng(seed);
    std::vector<SplineWarpSamplePair> pairs;

    for (int i = 0; i < n; ++i) {
        const double x = rng.next(0., 400.);
        const double y = rng.next(0., 400.);
        pairs.push_back(makePair(x, y, x + 12. * std::sin(x / 60.), y + 8. * std::cos(y / 45.) + 0.01 * x));
    }

    return pairs;
}

// source = A destination + b
const double kA[2][2] = { { 1.1, 0.2 }, { -0.15, 0.9 } };
const double kB[2] = { 7., -3. };

Point
affine(double x,
       double y)
{
    Point p;

    p.x = kA[0][0] * x + kA[0][1] * y + kB[0];
    p.y = kA[1][0] * x + kA[1][1] * y + kB[1];

    return p;
}
} // anonymous namespace

TEST(SplineWarpTps, InterpolatesExactlyWithoutSmoothness)
{
    const std::vector<SplineWarpSamplePair> pairs = scattered(60, 1);
    const SplineWarpTps tps = SplineWarpTps::solve(pairs, 0.);

    ASSERT_EQ(SplineWarpTps::eModelThinPlate, tps.getModel());
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        const Point s = tps.evaluate(pairs[i].destination.x, pairs[i].destination.y);
        EXPECT_NEAR(pairs[i].source.x, s.x, 1e-6) << i;
        EXPECT_NEAR(pairs[i].source.y, s.y, 1e-6) << i;
    }
}

TEST(SplineWarpTps, ReproducesAnAffineMapEverywhere)
{
    Lcg rng(2);
    std::vector<SplineWarpSamplePair> pairs;
    for (int i = 0; i < 40; ++i) {
        const double x = rng.next(-200., 200.);
        const double y = rng.next(-100., 300.);
        const Point s = affine(x, y);
        pairs.push_back(makePair(x, y, s.x, s.y));
    }

    // Smoothness only damps the bending part, which an affine set does not have.
    const double smoothness[2] = { 0., 0.5 };
    for (int k = 0; k < 2; ++k) {
        const SplineWarpTps tps = SplineWarpTps::solve(pairs, smoothness[k]);
        EXPECT_NEAR(0., tps.getBendingEnergy(), 1e-9) << smoothness[k];
        for (int j = 0; j < 50; ++j) {
            const double x = rng.next(-400., 400.);
            const double y = rng.next(-400., 400.);
            const Point expected = affine(x, y);
            const Point s = tps.evaluate(x, y);
            EXPECT_NEAR(expected.x, s.x, 1e-6) << smoothness[k] << " " << x << " " << y;
            EXPECT_NEAR(expected.y, s.y, 1e-6) << smoothness[k] << " " << x << " " << y;
        }
    }
}

TEST(SplineWarpTps, NoPairsIsTheIdentity)
{
    const SplineWarpTps tps = SplineWarpTps::solve(std::vector<SplineWarpSamplePair>(), 0.);

    EXPECT_EQ(SplineWarpTps::eModelIdentity, tps.getModel());
    EXPECT_TRUE(tps.isIdentity());
    const Point s = tps.evaluate(13.5, -7.25);
    EXPECT_EQ(13.5, s.x);
    EXPECT_EQ(-7.25, s.y);
}

TEST(SplineWarpTps, OnePairIsATranslation)
{
    std::vector<SplineWarpSamplePair> pairs;
    pairs.push_back(makePair(10., 20., 13., 16.));
    const SplineWarpTps tps = SplineWarpTps::solve(pairs, 0.);

    EXPECT_EQ(SplineWarpTps::eModelTranslation, tps.getModel());
    const Point s = tps.evaluate(-100., 250.);
    EXPECT_NEAR(-97., s.x, 1e-12);
    EXPECT_NEAR(246., s.y, 1e-12);
}

TEST(SplineWarpTps, CoincidentDestinationsAreATranslationByTheMean)
{
    std::vector<SplineWarpSamplePair> pairs;
    pairs.push_back(makePair(10., 20., 12., 20.));
    pairs.push_back(makePair(10., 20., 14., 24.));
    pairs.push_back(makePair(10., 20., 10., 22.));
    const SplineWarpTps tps = SplineWarpTps::solve(pairs, 0.);

    EXPECT_EQ(SplineWarpTps::eModelTranslation, tps.getModel());
    const Point s = tps.evaluate(0., 0.);
    EXPECT_NEAR(2., s.x, 1e-12);
    EXPECT_NEAR(2., s.y, 1e-12);
}

TEST(SplineWarpTps, TwoPairsAreASimilarity)
{
    // A rotation by 90 degrees, scale 2, about (10, 10), then a shift of (5, 0).
    std::vector<SplineWarpSamplePair> pairs;
    pairs.push_back(makePair(10., 10., 15., 10.));
    pairs.push_back(makePair(20., 10., 15., 30.));
    const SplineWarpTps tps = SplineWarpTps::solve(pairs, 0.);

    EXPECT_EQ(SplineWarpTps::eModelSimilarity, tps.getModel());
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        const Point s = tps.evaluate(pairs[i].destination.x, pairs[i].destination.y);
        EXPECT_NEAR(pairs[i].source.x, s.x, 1e-9) << i;
        EXPECT_NEAR(pairs[i].source.y, s.y, 1e-9) << i;
    }
    // (10, 20) is 10 above the centre: rotated to 20 left of it.
    const Point s = tps.evaluate(10., 20.);
    EXPECT_NEAR(-5., s.x, 1e-9);
    EXPECT_NEAR(10., s.y, 1e-9);
}

TEST(SplineWarpTps, CollinearDestinationsAreAnAffineAlongTheLine)
{
    std::vector<SplineWarpSamplePair> pairs;
    for (int i = 0; i < 8; ++i) {
        const double x = 20. * i;
        pairs.push_back(makePair(x, 50., 1.1 * x + 2., 53.));
    }
    const SplineWarpTps tps = SplineWarpTps::solve(pairs, 0.);

    EXPECT_EQ(SplineWarpTps::eModelAffine, tps.getModel());
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        const Point s = tps.evaluate(pairs[i].destination.x, pairs[i].destination.y);
        EXPECT_NEAR(pairs[i].source.x, s.x, 1e-9) << i;
        EXPECT_NEAR(pairs[i].source.y, s.y, 1e-9) << i;
    }
    // Off the line the displacement is that of the line's nearest point.
    const Point off = tps.evaluate(60., 90.);
    EXPECT_NEAR(1.1 * 60. + 2., off.x, 1e-9);
    EXPECT_NEAR(93., off.y, 1e-9);
}

TEST(SplineWarpTps, SmoothnessReducesBendingEnergy)
{
    const std::vector<SplineWarpSamplePair> pairs = scattered(80, 3);
    const SplineWarpTps exact = SplineWarpTps::solve(pairs, 0.);
    const SplineWarpTps smooth = SplineWarpTps::solve(pairs, 0.05);
    const SplineWarpTps smoother = SplineWarpTps::solve(pairs, 1.);

    ASSERT_EQ(SplineWarpTps::eModelThinPlate, exact.getModel());
    ASSERT_EQ(SplineWarpTps::eModelThinPlate, smooth.getModel());
    ASSERT_EQ(SplineWarpTps::eModelThinPlate, smoother.getModel());
    EXPECT_GT(exact.getBendingEnergy(), 0.);
    EXPECT_LT(smooth.getBendingEnergy(), exact.getBendingEnergy());
    EXPECT_LT(smoother.getBendingEnergy(), smooth.getBendingEnergy());

    // It no longer interpolates.
    double worst = 0.;
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        const Point s = smoother.evaluate(pairs[i].destination.x, pairs[i].destination.y);
        worst = std::max(worst, std::hypot(s.x - pairs[i].source.x, s.y - pairs[i].source.y));
    }
    EXPECT_GT(worst, 1e-3);
}

TEST(SplineWarpTps, TheCacheSolvesOncePerKey)
{
    const std::vector<SplineWarpSamplePair> pairs = scattered(20, 4);
    SplineWarpTpsCache cache;

    const SplineWarpTpsConstPtr a = cache.get(42, 0., pairs);
    const SplineWarpTpsConstPtr b = cache.get(42, 0., pairs);
    const SplineWarpTpsConstPtr c = cache.get(42, 0.5, pairs);
    ASSERT_TRUE(bool(a));
    ASSERT_TRUE(bool(c));
    EXPECT_EQ(a.get(), b.get());
    EXPECT_NE(a.get(), c.get());
}

TEST(SplineWarpTps, SolveTiming)
{
    // An unoptimized build takes minutes on the largest dense solve: time only the smallest there.
#ifdef __OPTIMIZE__
    const int sizes[] = { 500, 1000, 2000 };
#else
    const int sizes[] = { 500 };
#endif
    for (int n : sizes) {
        const std::vector<SplineWarpSamplePair> pairs = scattered(n, 5 + n);
        const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        const SplineWarpTps tps = SplineWarpTps::solve(pairs, 0.);
        const std::chrono::steady_clock::time_point solved = std::chrono::steady_clock::now();
        double sum = 0.;
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                sum += tps.evaluate(x * 6.25, y * 6.25).x;
            }
        }
        const std::chrono::steady_clock::time_point evaluated = std::chrono::steady_clock::now();
        EXPECT_EQ(SplineWarpTps::eModelThinPlate, tps.getModel()) << n;
        EXPECT_TRUE(std::isfinite(sum)) << n;
        std::cout << "SplineWarp TPS " << n << " samples: solve "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(solved - start).count() << " ms, 4096 evaluations "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(evaluated - solved).count() << " ms" << std::endl;
    }
}
