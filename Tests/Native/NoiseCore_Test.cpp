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
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Generator/NoiseCore.h"

NATRON_NAMESPACE_USING

using noise4d::NoiseAxis;
using noise4d::NoiseCoord;
using NoiseCore::NoiseParams;

namespace {

// One configuration of the core, with a name for the failure messages.
struct NamedParams {
    std::string name;
    NoiseParams params;
};

NoiseParams
basisParams(int type)
{
    NoiseParams p = NoiseCore::defaultNoiseParams();
    p.type = type;
    p.seed = 1337u;

    return p;
}

NoiseParams
cellularParams(int metric,
               int output)
{
    NoiseParams p = basisParams(NoiseCore::kNoiseTypeCellular);
    p.cellularMetric = metric;
    p.cellularOutput = output;

    return p;
}

// Every basis generator, every cellular metric and output, every fractal over a gradient and the
// cellular basis, and both warps.
std::vector<NamedParams>
allConfigurations()
{
    std::vector<NamedParams> out;
    out.push_back({ "perlin", basisParams(NoiseCore::kNoiseTypePerlin) });
    out.push_back({ "simplex", basisParams(NoiseCore::kNoiseTypeSimplex) });
    out.push_back({ "value", basisParams(NoiseCore::kNoiseTypeValue) });
    out.push_back({ "white", basisParams(NoiseCore::kNoiseTypeWhite) });
    for (int metric = 0; metric < 3; ++metric) {
        for (int output = 0; output < 6; ++output) {
            out.push_back({ "cellular metric " + std::to_string(metric) + " output " + std::to_string(output),
                            cellularParams(metric, output) });
        }
    }
    for (int fractal = NoiseCore::kNoiseFractalFBm; fractal <= NoiseCore::kNoiseFractalHybrid; ++fractal) {
        for (int type : { NoiseCore::kNoiseTypeSimplex, NoiseCore::kNoiseTypeCellular }) {
            NoiseParams p = basisParams(type);
            p.fractal = fractal;
            p.octaves = 6;
            p.lacunarity = 2.1f;
            p.gain = 0.6f;
            p.weightedStrength = 0.4f;
            out.push_back({ "fractal " + std::to_string(fractal) + " over type " + std::to_string(type), p });
        }
    }
    for (int warp : { NoiseCore::kNoiseWarpPerlin, NoiseCore::kNoiseWarpSimplex }) {
        NoiseParams p = basisParams(NoiseCore::kNoiseTypePerlin);
        p.fractal = NoiseCore::kNoiseFractalFBm;
        p.warpType = warp;
        p.warpAmount = 0.8f;
        p.warpFrequency = 0.5f;
        out.push_back({ "warp " + std::to_string(warp), p });
    }

    return out;
}

// Points near the origin and far from it, with random fractions.
std::vector<NoiseCoord>
randomPoints(std::size_t count,
             uint32_t seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> fraction(0.f, 1.f);
    std::vector<NoiseCoord> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        NoiseCoord p;
        const bool far = (i % 3) == 0;
        p.cx = far ? (uint32_t)rng() : (uint32_t)((int)(rng() % 64) - 32);
        p.cy = far ? (uint32_t)rng() : (uint32_t)((int)(rng() % 64) - 32);
        p.cz = (uint32_t)((int)(rng() % 16) - 8);
        p.cw = far ? (uint32_t)rng() : 0u;
        p.fx = fraction(rng);
        p.fy = fraction(rng);
        p.fz = fraction(rng);
        p.fw = fraction(rng);
        // uniform_real_distribution may return its upper bound in float.
        p.fx = p.fx < 1.f ? p.fx : 0.f;
        p.fy = p.fy < 1.f ? p.fy : 0.f;
        p.fz = p.fz < 1.f ? p.fz : 0.f;
        p.fw = p.fw < 1.f ? p.fw : 0.f;
        out.push_back(p);
    }

    return out;
}

// p moved by one ulp below the start of the next cell along x, and that cell's start.
void
straddleX(NoiseCoord p,
          NoiseCoord* below,
          NoiseCoord* above)
{
    *below = p;
    below->fx = std::nextafter(1.f, 0.f);
    *above = p;
    above->cx = p.cx + 1u;
    above->fx = 0.f;
}

} // namespace

// The same point and parameters give the same bits, and evaluating other points in between
// changes nothing.
TEST(NoiseCore, Deterministic)
{
    const std::vector<NoiseCoord> points = randomPoints(200, 1u);
    for (const NamedParams& c : allConfigurations()) {
        std::vector<float> first;
        for (const NoiseCoord& p : points) {
            first.push_back(NoiseCore::noise4(c.params, p));
        }
        for (std::size_t i = points.size(); i-- > 0;) {
            const float again = NoiseCore::noise4(c.params, points[i]);
            EXPECT_EQ(first[i], again) << c.name << " point " << i;
        }
    }
}

// Changing the seed changes the noise almost everywhere.
TEST(NoiseCore, SeedChangesResult)
{
    const std::vector<NoiseCoord> points = randomPoints(200, 2u);
    for (const NamedParams& c : allConfigurations()) {
        NoiseParams other = c.params;
        other.seed = c.params.seed + 1u;
        int same = 0;
        for (const NoiseCoord& p : points) {
            if (NoiseCore::noise4(c.params, p) == NoiseCore::noise4(other, p)) {
                ++same;
            }
        }
        EXPECT_LT(same, 10) << c.name;
    }
}

// Every output lies in [-1, 1] and spans a good part of it.
TEST(NoiseCore, OutputRange)
{
    const std::vector<NoiseCoord> points = randomPoints(3000, 3u);
    for (const NamedParams& c : allConfigurations()) {
        float lo = 2.f;
        float hi = -2.f;
        for (const NoiseCoord& p : points) {
            const float v = NoiseCore::noise4(c.params, p);
            ASSERT_TRUE(std::isfinite(v)) << c.name;
            EXPECT_GE(v, -1.f) << c.name;
            EXPECT_LE(v, 1.f) << c.name;
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        EXPECT_GT(hi - lo, 0.3f) << c.name << " spans only [" << lo << ", " << hi << "]";
    }
}

// The smooth generators are continuous across cell boundaries: the last float of a cell and the
// first of the next give nearly the same value, near the origin, far from it and where the cell
// index wraps.
TEST(NoiseCore, ContinuousAcrossCells)
{
    std::vector<NamedParams> smooth;
    smooth.push_back({ "perlin", basisParams(NoiseCore::kNoiseTypePerlin) });
    smooth.push_back({ "simplex", basisParams(NoiseCore::kNoiseTypeSimplex) });
    smooth.push_back({ "value", basisParams(NoiseCore::kNoiseTypeValue) });
    smooth.push_back({ "cellular F1", cellularParams(NoiseCore::kNoiseMetricEuclidean, NoiseCore::kNoiseCellularF1) });
    NoiseParams fbm = basisParams(NoiseCore::kNoiseTypePerlin);
    fbm.fractal = NoiseCore::kNoiseFractalFBm;
    fbm.octaves = 5;
    smooth.push_back({ "perlin fBm", fbm });

    std::vector<NoiseCoord> points = randomPoints(300, 4u);
    NoiseCoord wrap = points[0];
    wrap.cx = 0xFFFFFFFFu; // the next cell is 0
    points.push_back(wrap);
    wrap.cx = 0x7FFFFFFFu; // the next cell is -2^31
    points.push_back(wrap);
    for (const NamedParams& c : smooth) {
        for (std::size_t i = 0; i < points.size(); ++i) {
            NoiseCoord below;
            NoiseCoord above;
            straddleX(points[i], &below, &above);
            const float a = NoiseCore::noise4(c.params, below);
            const float b = NoiseCore::noise4(c.params, above);
            EXPECT_NEAR(a, b, 1e-4f) << c.name << " point " << i;
        }
    }
}

// The fourth axis is a real dimension: moving along w alone changes the noise.
TEST(NoiseCore, FourthAxisChangesResult)
{
    const std::vector<NoiseCoord> points = randomPoints(200, 5u);
    for (const NamedParams& c : allConfigurations()) {
        int same = 0;
        for (const NoiseCoord& p : points) {
            NoiseCoord q = p;
            q.cw = p.cw + 3u;
            q.fw = p.fw < 0.5f ? p.fw + 0.37f : p.fw - 0.37f;
            if (NoiseCore::noise4(c.params, p) == NoiseCore::noise4(c.params, q)) {
                ++same;
            }
        }
        EXPECT_LT(same, 10) << c.name;
    }
}

// White noise is one value per cell, different between cells.
TEST(NoiseCore, WhiteIsConstantPerCell)
{
    const NoiseParams white = basisParams(NoiseCore::kNoiseTypeWhite);
    const std::vector<NoiseCoord> points = randomPoints(200, 6u);
    int sameAcross = 0;
    for (const NoiseCoord& p : points) {
        NoiseCoord corner = p;
        corner.fx = 0.f;
        corner.fy = 0.f;
        corner.fz = 0.f;
        corner.fw = 0.f;
        EXPECT_EQ(NoiseCore::noise4(white, corner), NoiseCore::noise4(white, p));
        NoiseCoord next = p;
        next.cy = p.cy + 1u;
        if (NoiseCore::noise4(white, next) == NoiseCore::noise4(white, p)) {
            ++sameAcross;
        }
    }
    EXPECT_LT(sameAcross, 5);
}

// Smooth F1 never exceeds F1, and the distance to the nearest edge is at least half the gap
// between F1 and F2 (the distance to the bisector of two points is at least half the difference
// of their distances), and at most F2.
TEST(NoiseCore, CellularOutputsAreConsistent)
{
    const std::vector<NoiseCoord> points = randomPoints(500, 7u);
    const NoiseParams f1 = cellularParams(NoiseCore::kNoiseMetricEuclidean, NoiseCore::kNoiseCellularF1);
    const NoiseParams f2 = cellularParams(NoiseCore::kNoiseMetricEuclidean, NoiseCore::kNoiseCellularF2);
    const NoiseParams gap = cellularParams(NoiseCore::kNoiseMetricEuclidean, NoiseCore::kNoiseCellularF2MinusF1);
    const NoiseParams edge = cellularParams(NoiseCore::kNoiseMetricEuclidean, NoiseCore::kNoiseCellularDistanceToEdge);
    const NoiseParams smoothF1 = cellularParams(NoiseCore::kNoiseMetricEuclidean, NoiseCore::kNoiseCellularSmoothF1);
    for (std::size_t i = 0; i < points.size(); ++i) {
        const NoiseCoord& p = points[i];
        // Outputs are distance * scale - 1 with one scale per metric, so compare v + 1.
        const float vF1 = NoiseCore::noise4(f1, p) + 1.f;
        const float vF2 = NoiseCore::noise4(f2, p) + 1.f;
        const float vGap = NoiseCore::noise4(gap, p) + 1.f;
        const float vEdge = NoiseCore::noise4(edge, p) + 1.f;
        const float vSmooth = NoiseCore::noise4(smoothF1, p) + 1.f;
        EXPECT_LE(vF1, vF2 + 1e-6f) << i;
        EXPECT_NEAR(vGap, vF2 - vF1, 1e-5f) << i;
        EXPECT_LE(vSmooth, vF1 + 1e-6f) << i;
        EXPECT_GE(vEdge, vGap * 0.5f - 1e-5f) << i;
        EXPECT_LE(vEdge, vF2 + 1e-5f) << i;
    }
}

// Zero jitter puts every feature point at its cell's centre, so F1 is the distance to the
// nearest lattice point.
TEST(NoiseCore, CellularWithoutJitterIsLattice)
{
    NoiseParams p = cellularParams(NoiseCore::kNoiseMetricEuclidean, NoiseCore::kNoiseCellularF1);
    p.jitter = 0.f;
    const std::vector<NoiseCoord> points = randomPoints(200, 8u);
    for (const NoiseCoord& q : points) {
        const auto d = [](float f) {
            return f < 0.5f ? f : 1.f - f;
        };
        const float expected = std::sqrt(d(q.fx) * d(q.fx) + d(q.fy) * d(q.fy) + d(q.fz) * d(q.fz) + d(q.fw) * d(q.fw));
        const float got = (NoiseCore::noise4(p, q) + 1.f) / noise4d::kCellularEuclideanScale;
        EXPECT_NEAR(expected, got, 1e-5f);
    }
}

// Scaling a split coordinate is exact in the cell and accurate in the fraction, at any distance
// from the origin; double precision is the reference.
TEST(NoiseCore, AxisScaleIsExact)
{
    std::mt19937 rng(9u);
    std::uniform_real_distribution<float> fraction(0.f, 1.f);
    std::uniform_real_distribution<float> scale(0.001f, 64.f);
    for (int i = 0; i < 20000; ++i) {
        const int32_t cell = (i % 2) ? (int32_t)rng() : (int32_t)(rng() % 20000) - 10000;
        float f = fraction(rng);
        f = f < 1.f ? f : 0.f;
        const float m = scale(rng);
        // The product of a 32-bit integer and a float, in units of 2^-k: long double holds 64 bits.
        const long double exact = ((long double)cell + (long double)f) * (long double)m;
        const long double floorExact = std::floor(exact);
        const NoiseAxis got = noise4d::axisScale((uint32_t)cell, f, m);
        const uint32_t expectedCell = (uint32_t)(uint64_t)(int64_t)std::fmod(floorExact, 4294967296.0L);
        const long double expectedFraction = exact - floorExact;
        ASSERT_GE(got.f, 0.f) << i;
        ASSERT_LT(got.f, 1.f) << i;
        // A fraction within rounding of 0 or 1 may land in either cell. f * m rounds to half an ulp
        // of a number below m, the sum of the fractions to a few ulps of 1.
        const long double gotTotal = (long double)(int32_t)(got.c - expectedCell) + (long double)got.f;
        const double tolerance = 1e-6 + m * 6e-8;
        EXPECT_NEAR((double)gotTotal, (double)expectedFraction, tolerance) << "cell " << cell << " f " << f << " m " << m;
    }
}

// The split coordinate of a float matches its floor and remainder, including far from the origin.
TEST(NoiseCore, AxisFromFloat)
{
    const float values[] = { 0.f, -0.f, 0.25f, -0.25f, -1.f, 1.5f, -1e-30f, 8388607.5f, -8388608.f, 3e9f, -3e9f, 1e20f, -1e20f };
    for (float v : values) {
        const NoiseAxis a = noise4d::axisNormalize(noise4d::axisFromFloat(v).c, noise4d::axisFromFloat(v).f);
        const double fl = std::floor((double)v);
        const uint32_t expected = (uint32_t)(uint64_t)(int64_t)std::fmod(fl, 4294967296.0);
        EXPECT_GE(a.f, 0.f) << v;
        EXPECT_LT(a.f, 1.f) << v;
        if (a.f == 0.f && v - fl > 0.5) {
            // -1e-30 rounds up to the next cell.
            EXPECT_EQ(expected + 1u, a.c) << v;
        } else {
            EXPECT_EQ(expected, a.c) << v;
            EXPECT_EQ((float)(v - fl), a.f) << v;
        }
    }
}

// One octave of fBm is the basis itself, and the octave count is capped at 16.
TEST(NoiseCore, FractalOctaves)
{
    const std::vector<NoiseCoord> points = randomPoints(100, 10u);
    NoiseParams basis = basisParams(NoiseCore::kNoiseTypeSimplex);
    NoiseParams one = basis;
    one.fractal = NoiseCore::kNoiseFractalFBm;
    one.octaves = 1;
    NoiseParams sixteen = one;
    sixteen.octaves = 16;
    NoiseParams many = one;
    many.octaves = 100;
    NoiseParams none = one;
    none.octaves = 0;
    for (const NoiseCoord& p : points) {
        EXPECT_EQ(NoiseCore::noise4(basis, p), NoiseCore::noise4(one, p));
        EXPECT_EQ(NoiseCore::noise4(one, p), NoiseCore::noise4(none, p));
        EXPECT_EQ(NoiseCore::noise4(sixteen, p), NoiseCore::noise4(many, p));
    }
}

// A zero warp amount leaves the noise unchanged; a nonzero one changes it.
TEST(NoiseCore, WarpDisplaces)
{
    const std::vector<NoiseCoord> points = randomPoints(100, 11u);
    NoiseParams plain = basisParams(NoiseCore::kNoiseTypePerlin);
    NoiseParams still = plain;
    still.warpType = NoiseCore::kNoiseWarpSimplex;
    still.warpAmount = 0.f;
    NoiseParams warped = still;
    warped.warpAmount = 1.f;
    int same = 0;
    for (const NoiseCoord& p : points) {
        EXPECT_EQ(NoiseCore::noise4(plain, p), NoiseCore::noise4(still, p));
        if (NoiseCore::noise4(plain, p) == NoiseCore::noise4(warped, p)) {
            ++same;
        }
    }
    EXPECT_LT(same, 5);
}
