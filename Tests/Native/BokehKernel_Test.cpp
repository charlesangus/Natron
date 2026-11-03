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

#include "Engine/Nodes/Filter/BokehKernel.h"

NATRON_NAMESPACE_USING

namespace {

double
kernelSum(const BokehKernel& kernel)
{
    double sum = 0.;

    for (float w : kernel.weights()) {
        sum += w;
    }

    return sum;
}

BokehShapeParams
blades(int count,
       double rotation,
       double roundness)
{
    BokehShapeParams p;

    p.shape = BokehShape::Blades;
    p.blades = count;
    p.rotation = rotation;
    p.roundness = roundness;

    return p;
}

// The direct gather of a width x height plane, 0 outside it, non-finite samples counting as 0.
std::vector<float>
directGather(const std::vector<float>& src,
             int width,
             int height,
             const BokehKernel& kernel)
{
    std::vector<float> out((std::size_t)width * height, 0.f);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            double sum = 0.;
            for (int dy = -kernel.haloY(); dy <= kernel.haloY(); ++dy) {
                for (int dx = -kernel.haloX(); dx <= kernel.haloX(); ++dx) {
                    const int sx = x - dx;
                    const int sy = y - dy;
                    if ((sx < 0) || (sy < 0) || (sx >= width) || (sy >= height)) {
                        continue;
                    }
                    const float v = src[(std::size_t)sy * width + sx];
                    if (std::isfinite(v)) {
                        sum += (double)kernel.at(dx, dy) * v;
                    }
                }
            }
            out[(std::size_t)y * width + x] = (float)sum;
        }
    }

    return out;
}

// The gather of bokehPrefixRow() and bokehGatherRow() over the whole plane.
std::vector<float>
runGather(const std::vector<float>& src,
          int width,
          int height,
          const BokehKernel& kernel)
{
    const std::size_t stride = (std::size_t)width + 1;
    std::vector<double> tables(stride * height, 0.);
    for (int y = 0; y < height; ++y) {
        bokehPrefixRow(&src[(std::size_t)y * width], width, &tables[(std::size_t)y * stride]);
    }
    const std::vector<BokehRun> runs = kernel.runs();
    std::vector<double> acc((std::size_t)width);
    std::vector<float> out((std::size_t)width * height, 0.f);
    for (int y = 0; y < height; ++y) {
        bokehGatherRow(tables.data(), width, height, runs, y, 0, width, acc.data(), &out[(std::size_t)y * width]);
    }

    return out;
}

} // namespace

TEST(BokehKernel, ExtentAndIdentity)
{
    EXPECT_EQ(0, BokehKernel::extent(0.));
    EXPECT_EQ(0, BokehKernel::extent(0.49));
    EXPECT_EQ(0, BokehKernel::extent(0.5));
    EXPECT_EQ(1, BokehKernel::extent(1.));
    EXPECT_EQ(1, BokehKernel::extent(1.5));
    EXPECT_EQ(2, BokehKernel::extent(1.51));
    EXPECT_EQ(250, BokehKernel::extent(250.));
    EXPECT_EQ(0, BokehKernel::extent(std::numeric_limits<double>::quiet_NaN()));
    EXPECT_EQ(0, BokehKernel::extent(-3.));

    EXPECT_TRUE(BokehKernel::isIdentityRadius(0., 0.));
    EXPECT_TRUE(BokehKernel::isIdentityRadius(0.49, 0.2));
    EXPECT_FALSE(BokehKernel::isIdentityRadius(0.5, 0.));
    EXPECT_TRUE(BokehKernel::isIdentityRadius(std::numeric_limits<double>::infinity(), -1.));

    const BokehKernel tap = BokehKernel::shaped(BokehShapeParams(), 0.3, 0.3);
    EXPECT_TRUE(tap.isIdentity());
    EXPECT_EQ(1.f, tap.at(0, 0));
    EXPECT_EQ(0.f, tap.at(1, 0));
    EXPECT_TRUE(BokehKernel().isIdentity());
}

// Every kernel's weights sum to 1.
TEST(BokehKernel, EveryKernelSumsToOne)
{
    std::vector<BokehShapeParams> shapes;
    shapes.push_back(BokehShapeParams());
    for (int n : { 3, 5, 6, 8, 12 }) {
        for (double roundness : { 0., 0.3, 1. }) {
            shapes.push_back(blades(n, 17.5 * n, roundness));
        }
    }
    const double radii[][2] = { { 0.5, 0.5 }, { 0.75, 0.75 }, { 1., 1. }, { 2.5, 2.5 }, { 7.3, 7.3 }, { 40., 40. }, { 6., 2.5 }, { 0.6, 9. } };
    for (const BokehShapeParams& shape : shapes) {
        for (const auto& r : radii) {
            const BokehKernel kernel = BokehKernel::shaped(shape, r[0], r[1]);
            EXPECT_NEAR(1., kernelSum(kernel), 1e-5) << (int)shape.shape << " " << shape.blades << " " << shape.roundness << " radius " << r[0] << "x" << r[1];
            EXPECT_EQ(BokehKernel::extent(r[0]), kernel.haloX());
            EXPECT_EQ(BokehKernel::extent(r[1]), kernel.haloY());
        }
    }

    // A ring, a ramp and a non-square image.
    const int w = 9;
    const int h = 5;
    std::vector<float> image((std::size_t)w * h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            image[(std::size_t)y * w + x] = (float)(0.1 + 0.07 * x + 0.3 * (y % 2));
        }
    }
    for (double r : { 0.8, 3., 11.2, 30. }) {
        bool empty = true;
        const BokehKernel kernel = BokehKernel::fromImage(image.data(), w, h, r, r, &empty);
        EXPECT_FALSE(empty);
        EXPECT_FALSE(kernel.isIdentity()) << r;
        EXPECT_NEAR(1., kernelSum(kernel), 1e-5) << "image, radius " << r;
    }
}

// An n-blade aperture at roundness 1 is the disc, weight for weight.
TEST(BokehKernel, BladesAtRoundnessOneMatchTheDisc)
{
    for (double r : { 0.7, 1.5, 3.2, 9., 24.6 }) {
        const BokehKernel disc = BokehKernel::shaped(BokehShapeParams(), r, r * 0.8);
        for (int n : { 3, 5, 6, 9 }) {
            for (double rotation : { 0., 13., -90. }) {
                const BokehKernel round = BokehKernel::shaped(blades(n, rotation, 1.), r, r * 0.8);
                ASSERT_EQ(disc.haloX(), round.haloX());
                ASSERT_EQ(disc.haloY(), round.haloY());
                EXPECT_EQ(disc.weights(), round.weights()) << "radius " << r << ", " << n << " blades, rotation " << rotation;
            }
        }
    }
}

// The disc is symmetric, flat inside, and anti-aliased at its edge.
TEST(BokehKernel, DiscShape)
{
    const double r = 12.4;
    const BokehKernel disc = BokehKernel::shaped(BokehShapeParams(), r, r);
    const int h = disc.haloX();
    const float centre = disc.at(0, 0);
    for (int dy = -h; dy <= h; ++dy) {
        for (int dx = -h; dx <= h; ++dx) {
            const float w = disc.at(dx, dy);
            EXPECT_EQ(w, disc.at(-dx, dy));
            EXPECT_EQ(w, disc.at(dx, -dy));
            EXPECT_EQ(w, disc.at(dy, dx));
            EXPECT_LE(w, centre);
            const double nearest = std::hypot(std::max(0., std::fabs(dx) - 0.5), std::max(0., std::fabs(dy) - 0.5));
            const double farthest = std::hypot(std::fabs(dx) + 0.5, std::fabs(dy) + 0.5);
            if (farthest <= r) {
                EXPECT_EQ(centre, w) << dx << "," << dy;
            } else if (nearest >= r) {
                EXPECT_EQ(0.f, w) << dx << "," << dy;
            }
        }
    }
    // The edge pixel straddling the circle on the axis is partly covered.
    const int edge = (int)std::floor(r + 0.5);
    EXPECT_GT(disc.at(edge, 0), 0.f);
    EXPECT_LT(disc.at(edge, 0), centre);
}

// A hexagon with a corner up reaches the circle upwards but only its apothem sideways; rotated by
// 90 degrees, the other way round.
TEST(BokehKernel, BladesFollowRotation)
{
    const double r = 20.;
    // Beyond the apothem, r cos(30 deg) ~ 17.3, inside the circle.
    const int far = 19;
    const BokehKernel disc = BokehKernel::shaped(BokehShapeParams(), r, r);
    const BokehKernel hexagon = BokehKernel::shaped(blades(6, 0., 0.), r, r);
    const BokehKernel turned = BokehKernel::shaped(blades(6, 90., 0.), r, r);
    EXPECT_GT(disc.at(far, 0), 0.f);
    EXPECT_GT(disc.at(0, far), 0.f);
    EXPECT_GT(hexagon.at(0, far), 0.f);
    EXPECT_EQ(0.f, hexagon.at(far, 0));
    EXPECT_EQ(0.f, hexagon.at(-far, 0));
    EXPECT_GT(turned.at(far, 0), 0.f);
    EXPECT_EQ(0.f, turned.at(0, far));

    // Rounder blades cover more, so each weight inside the hexagon shrinks.
    const BokehKernel rounder = BokehKernel::shaped(blades(6, 0., 0.5), r, r);
    EXPECT_LT(rounder.at(0, 0), hexagon.at(0, 0));
    EXPECT_GT(rounder.at(far - 1, 0), 0.f);
}

// The runs hold every non-zero weight once, and merge the flat inside.
TEST(BokehKernel, RunsCoverTheWeights)
{
    for (const BokehShapeParams& shape : { BokehShapeParams(), blades(5, 10., 0.2) }) {
        const BokehKernel kernel = BokehKernel::shaped(shape, 15.3, 9.7);
        std::vector<float> rebuilt(kernel.weights().size(), 0.f);
        std::size_t nonZero = 0;
        for (float w : kernel.weights()) {
            nonZero += (w != 0.f) ? 1 : 0;
        }
        const std::vector<BokehRun> runs = kernel.runs();
        for (const BokehRun& run : runs) {
            ASSERT_GT(run.length, 0);
            ASSERT_NE(0.f, run.weight);
            for (int k = 0; k < run.length; ++k) {
                const int dx = run.dx0 + k;
                ASSERT_LE(-kernel.haloX(), dx);
                ASSERT_GE(kernel.haloX(), dx);
                const std::size_t i = (std::size_t)(run.dy + kernel.haloY()) * kernel.width() + (dx + kernel.haloX());
                EXPECT_EQ(0.f, rebuilt[i]);
                rebuilt[i] = run.weight;
            }
        }
        EXPECT_EQ(kernel.weights(), rebuilt);
        EXPECT_LT(runs.size(), nonZero / 2);
    }
}

// The image kernel stretches the longer side over the diameter, keeps the image's orientation, and
// reads negative and non-finite values as 0; an image summing to 0 is the single tap.
TEST(BokehKernel, ImageKernel)
{
    const double r = 10.;
    // Four wide, two high: the image spans [-r, r] x [-r/2, r/2].
    std::vector<float> flat(8, 1.f);
    const BokehKernel wide = BokehKernel::fromImage(flat.data(), 4, 2, r, r);
    EXPECT_NEAR(1., kernelSum(wide), 1e-5);
    for (int dy = -wide.haloY(); dy <= wide.haloY(); ++dy) {
        for (int dx = -wide.haloX(); dx <= wide.haloX(); ++dx) {
            if (std::abs(dy) >= 6) {
                EXPECT_EQ(0.f, wide.at(dx, dy)) << dx << "," << dy;
            }
            EXPECT_EQ(wide.at(dx, dy), wide.at(-dx, -dy));
        }
    }
    EXPECT_GT(wide.at(9, 0), 0.f);
    EXPECT_EQ(wide.at(0, 0), wide.at(9, 4));

    // Only the top-right quarter of a 2 x 2 image is lit: the kernel lies up and to the right.
    const float corner[4] = { 0.f, 0.f, 0.f, 3.f };
    const BokehKernel quarter = BokehKernel::fromImage(corner, 2, 2, r, r);
    double right = 0.;
    double up = 0.;
    for (int dy = -quarter.haloY(); dy <= quarter.haloY(); ++dy) {
        for (int dx = -quarter.haloX(); dx <= quarter.haloX(); ++dx) {
            const float w = quarter.at(dx, dy);
            if ((dx < 0) || (dy < 0)) {
                EXPECT_EQ(0.f, w) << dx << "," << dy;
            }
            right += (dx > 0) ? w : 0.;
            up += (dy > 0) ? w : 0.;
        }
    }
    EXPECT_GT(right, 0.9);
    EXPECT_GT(up, 0.9);

    // NaN, infinities and negatives read as 0.
    const float specials[4] = { std::numeric_limits<float>::quiet_NaN(), -std::numeric_limits<float>::infinity(), -2.f, 3.f };
    const BokehKernel cleaned = BokehKernel::fromImage(specials, 2, 2, r, r);
    EXPECT_EQ(quarter.weights(), cleaned.weights());
    const float inf[4] = { 0.f, 0.f, 0.f, std::numeric_limits<float>::infinity() };
    bool empty = false;
    EXPECT_TRUE(BokehKernel::fromImage(inf, 2, 2, r, r, &empty).isIdentity());
    EXPECT_TRUE(empty);

    // Black, empty and missing images are the single tap, and say so.
    const std::vector<float> black(16, 0.f);
    empty = false;
    EXPECT_TRUE(BokehKernel::fromImage(black.data(), 4, 4, r, r, &empty).isIdentity());
    EXPECT_TRUE(empty);
    empty = false;
    EXPECT_TRUE(BokehKernel::fromImage(nullptr, 0, 0, r, r, &empty).isIdentity());
    EXPECT_TRUE(empty);
    // An identity radius is not the image's fault.
    empty = true;
    EXPECT_TRUE(BokehKernel::fromImage(black.data(), 4, 4, 0.2, 0.2, &empty).isIdentity());
    EXPECT_FALSE(empty);
}

// The unboost undoes the boost to float rounding, and neither touches values at or below the
// threshold.
TEST(BokehKernel, UnboostUndoesBoost)
{
    for (float threshold : { 0.f, 0.5f, 1.f, 4.f }) {
        for (float gain : { 1.f, 1.5f, 2.f, 10.f, 1000.f }) {
            BokehHighlight h;
            h.threshold = threshold;
            h.gain = gain;
            EXPECT_EQ(gain != 1.f, h.isActive());
            for (float v : { -3.f, 0.f, 0.25f, 0.5f, 0.999f, 1.f, 1.001f, 2.f, 7.5f, 100.f, 65504.f }) {
                const float boosted = h.boost(v);
                const float back = h.unboost(boosted);
                if (v <= threshold) {
                    EXPECT_EQ(v, boosted);
                    EXPECT_EQ(v, back);
                } else {
                    EXPECT_GE(boosted, v);
                    EXPECT_NEAR(v, back, 1e-6 * std::fabs(v) + 1e-6 * gain * threshold) << "threshold " << threshold << ", gain " << gain;
                }
            }
        }
    }
}

// The run gather matches the direct convolution, spreads a single pixel into the kernel itself,
// and reads non-finite samples as 0.
TEST(BokehKernel, RunGatherMatchesDirect)
{
    const int width = 41;
    const int height = 33;
    std::vector<float> src((std::size_t)width * height);
    for (std::size_t i = 0; i < src.size(); ++i) {
        src[i] = (float)(std::sin(0.37 * (double)i) * 2. + 0.5);
    }
    src[(std::size_t)7 * width + 11] = std::numeric_limits<float>::quiet_NaN();
    src[(std::size_t)20 * width + 30] = std::numeric_limits<float>::infinity();
    for (const BokehShapeParams& shape : { BokehShapeParams(), blades(6, 15., 0.1) }) {
        const BokehKernel kernel = BokehKernel::shaped(shape, 6.3, 4.1);
        const std::vector<float> expected = directGather(src, width, height, kernel);
        const std::vector<float> actual = runGather(src, width, height, kernel);
        for (std::size_t i = 0; i < expected.size(); ++i) {
            ASSERT_TRUE(std::isfinite(actual[i])) << i;
            EXPECT_NEAR(expected[i], actual[i], 1e-5) << i;
        }
    }

    // A point at (20, 16) gives the kernel around it.
    std::vector<float> point((std::size_t)width * height, 0.f);
    point[(std::size_t)16 * width + 20] = 1.f;
    const BokehKernel kernel = BokehKernel::shaped(blades(5, 0., 0.), 9., 9.);
    const std::vector<float> spread = runGather(point, width, height, kernel);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            EXPECT_NEAR(kernel.at(x - 20, y - 16), spread[(std::size_t)y * width + x], 1e-7) << x << "," << y;
        }
    }

    // Columns x1 .. x2 of a row are those of the whole row.
    std::vector<double> tables((std::size_t)(width + 1) * height);
    for (int y = 0; y < height; ++y) {
        bokehPrefixRow(&src[(std::size_t)y * width], width, &tables[(std::size_t)y * (width + 1)]);
    }
    const std::vector<BokehRun> runs = kernel.runs();
    std::vector<double> acc((std::size_t)width);
    std::vector<float> whole((std::size_t)width);
    std::vector<float> part(10);
    bokehGatherRow(tables.data(), width, height, runs, 12, 0, width, acc.data(), whole.data());
    bokehGatherRow(tables.data(), width, height, runs, 12, 5, 15, acc.data(), part.data());
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(whole[(std::size_t)(5 + i)], part[(std::size_t)i]);
    }
}
