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

namespace {

BokehClip
clipAt(double offsetX,
       double offsetY,
       double radius)
{
    BokehClip clip;

    clip.offsetX = offsetX;
    clip.offsetY = offsetY;
    clip.radius = radius;

    return clip;
}

BokehShapeParams
clipped(BokehShapeParams shape,
        const BokehClip& clip)
{
    shape.clip = clip;

    return shape;
}

std::size_t
nonZeroTaps(const BokehKernel& kernel)
{
    std::size_t n = 0;

    for (float w : kernel.weights()) {
        n += (w != 0.f) ? 1 : 0;
    }

    return n;
}

float
maxWeight(const BokehKernel& kernel)
{
    float m = 0.f;

    for (float w : kernel.weights()) {
        m = std::max(m, w);
    }

    return m;
}

} // namespace

// A clip holding the whole shape, the default included, leaves every kernel as it was, bit for bit.
TEST(BokehKernel, ClipHoldingTheShapeChangesNothing)
{
    const BokehClip holding[] = { BokehClip(), clipAt(0., 0., 1.), clipAt(0., 0., 1.5), clipAt(0.2, -0.3, 2.) };
    for (const BokehShapeParams& shape : { BokehShapeParams(), blades(6, 10., 0.2), blades(5, 0., 1.) }) {
        for (double r : { 0.7, 3.4, 17.9 }) {
            const BokehKernel plain = BokehKernel::shaped(shape, r, r * 0.6);
            for (const BokehClip& clip : holding) {
                const BokehKernel kernel = BokehKernel::shaped(clipped(shape, clip), r, r * 0.6);
                ASSERT_EQ(plain.haloX(), kernel.haloX());
                ASSERT_EQ(plain.haloY(), kernel.haloY());
                EXPECT_EQ(plain.weights(), kernel.weights()) << "radius " << r << ", clip radius " << clip.radius;
            }
        }
    }

    // The 4 x 2 image spans the unit circle's width and reaches its corners at sqrt(1.25).
    const std::vector<float> image = { 1.f, 2.f, 3.f, 4.f, 4.f, 3.f, 2.f, 1.f };
    const BokehKernel plain = BokehKernel::fromImage(image.data(), 4, 2, 9., 9.);
    for (const BokehClip& clip : { BokehClip(), clipAt(0., 0., 1.2), clipAt(0.1, 0., 2.) }) {
        EXPECT_EQ(plain.weights(), BokehKernel::fromImage(image.data(), 4, 2, 9., 9., nullptr, clip).weights()) << clip.radius;
    }
}

// A clip offset along +x cuts a lens symmetric about the x axis out of the disc: the far left is
// gone, the right is kept, and the weights still sum to 1.
TEST(BokehKernel, ClipCutsALens)
{
    const double r = 15.3;
    const BokehKernel disc = BokehKernel::shaped(BokehShapeParams(), r, r);
    const BokehKernel lens = BokehKernel::shaped(clipped(BokehShapeParams(), clipAt(0.6, 0., 1.)), r, r);
    ASSERT_EQ(disc.haloX(), lens.haloX());
    ASSERT_EQ(disc.haloY(), lens.haloY());
    EXPECT_NEAR(1., kernelSum(lens), 1e-5);
    const int h = lens.haloX();
    for (int dy = -h; dy <= h; ++dy) {
        for (int dx = -h; dx <= h; ++dx) {
            EXPECT_EQ(lens.at(dx, dy), lens.at(dx, -dy)) << dx << "," << dy;
            if (disc.at(dx, dy) == 0.f) {
                EXPECT_EQ(0.f, lens.at(dx, dy)) << dx << "," << dy;
            }
        }
    }
    // At x = -0.5 r the clip, centred at 0.6 r, is 1.1 r away.
    EXPECT_GT(disc.at(-8, 0), 0.f);
    EXPECT_EQ(0.f, lens.at(-8, 0));
    EXPECT_GT(lens.at(14, 0), 0.f);
    // Within both, the weights are flat and larger than the disc's.
    EXPECT_EQ(lens.at(5, 0), lens.at(8, 3));
    EXPECT_GT(lens.at(5, 0), disc.at(5, 0));

    // The clip squeezes with the kernel: offset up, on an ellipse half as tall as it is wide.
    const BokehKernel oval = BokehKernel::shaped(clipped(BokehShapeParams(), clipAt(0., 0.5, 1.)), 20., 10.);
    EXPECT_EQ(0.f, oval.at(0, -9));
    EXPECT_GT(oval.at(0, 9), 0.f);
    EXPECT_GT(oval.at(0, -4), 0.f);
    for (int dy = -oval.haloY(); dy <= oval.haloY(); ++dy) {
        for (int dx = -oval.haloX(); dx <= oval.haloX(); ++dx) {
            EXPECT_EQ(oval.at(dx, dy), oval.at(-dx, dy)) << dx << "," << dy;
        }
    }
}

// The further the clip moves off the shape's centre, the less of the shape it leaves.
TEST(BokehKernel, ClipAreaShrinksWithOffset)
{
    for (const BokehShapeParams& shape : { BokehShapeParams(), blades(7, 20., 0.3) }) {
        std::size_t taps = std::numeric_limits<std::size_t>::max();
        float peak = 0.f;
        for (double offset : { 0., 0.15, 0.3, 0.5, 0.8, 1.2, 1.6 }) {
            // Along a diagonal, so neither axis is special.
            const double o = offset / std::sqrt(2.);
            const BokehKernel kernel = BokehKernel::shaped(clipped(shape, clipAt(o, -o, 1.)), 24., 24.);
            EXPECT_NEAR(1., kernelSum(kernel), 1e-5) << offset;
            EXPECT_LE(nonZeroTaps(kernel), taps) << offset;
            if (offset > 0.) {
                EXPECT_GT(maxWeight(kernel), peak) << offset;
            }
            taps = nonZeroTaps(kernel);
            peak = maxWeight(kernel);
        }
    }
}

// A clip that leaves nothing gives the unclipped kernel.
TEST(BokehKernel, EmptyClipFallsBackToTheShape)
{
    const BokehShapeParams shape = blades(6, 0., 0.25);
    const BokehKernel plain = BokehKernel::shaped(shape, 11., 7.);
    for (const BokehClip& clip : { clipAt(3., 0., 1.), clipAt(0., 0., 0.), clipAt(0.2, 0.2, -1.) }) {
        EXPECT_EQ(plain.weights(), BokehKernel::shaped(clipped(shape, clip), 11., 7.).weights()) << clip.offsetX << " " << clip.radius;
    }
    // A NaN anywhere reads as no clip.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    EXPECT_EQ(plain.weights(), BokehKernel::shaped(clipped(shape, clipAt(nan, 0., 0.5)), 11., 7.).weights());
    EXPECT_EQ(plain.weights(), BokehKernel::shaped(clipped(shape, clipAt(0.5, 0., nan)), 11., 7.).weights());

    const std::vector<float> flat(16, 1.f);
    const BokehKernel image = BokehKernel::fromImage(flat.data(), 4, 4, 10., 10.);
    bool empty = true;
    EXPECT_EQ(image.weights(), BokehKernel::fromImage(flat.data(), 4, 4, 10., 10., &empty, clipAt(4., 0., 1.)).weights());
    EXPECT_FALSE(empty);
}

// An image kernel is clipped, then renormalised.
TEST(BokehKernel, ClipCutsTheImageKernel)
{
    const double r = 10.;
    const std::vector<float> flat(16, 1.f);
    const BokehKernel square = BokehKernel::fromImage(flat.data(), 4, 4, r, r);
    bool empty = true;
    const BokehKernel cut = BokehKernel::fromImage(flat.data(), 4, 4, r, r, &empty, clipAt(0.7, 0., 1.));
    EXPECT_FALSE(empty);
    EXPECT_NEAR(1., kernelSum(cut), 1e-5);
    for (int dy = -cut.haloY(); dy <= cut.haloY(); ++dy) {
        for (int dx = -cut.haloX(); dx <= cut.haloX(); ++dx) {
            EXPECT_FLOAT_EQ(cut.at(dx, dy), cut.at(dx, -dy)) << dx << "," << dy;
            if (square.at(dx, dy) == 0.f) {
                EXPECT_EQ(0.f, cut.at(dx, dy));
            }
        }
    }
    // x = -0.5 r is 1.2 r from the clip's centre; x = 0.9 r is 0.2 r from it.
    EXPECT_GT(square.at(-5, 0), 0.f);
    EXPECT_EQ(0.f, cut.at(-5, 0));
    EXPECT_GT(cut.at(9, 0), square.at(9, 0));
    EXPECT_FLOAT_EQ(cut.at(3, 0) / square.at(3, 0), cut.at(6, 2) / square.at(6, 2));
}

// Texture settings that leave the shape flat leave the kernel as it was, bit for bit.
TEST(BokehKernel, FlatTexturesChangeNothing)
{
    for (const BokehShapeParams& base : { BokehShapeParams(), blades(5, 33., 0.1) }) {
        BokehShapeParams flat = base;
        flat.innerSize = 0.35;
        flat.innerFeather = 0.;
        flat.innerBrightness = 1.;
        flat.catadioptric = false;
        flat.catadioptricSize = 0.6;
        flat.sphericalAberration = 0.;
        flat.filterShape = 1.;
        for (double r : { 0.8, 4.2, 21.5 }) {
            EXPECT_EQ(BokehKernel::shaped(base, r, r).weights(), BokehKernel::shaped(flat, r, r).weights()) << r;
        }
    }
    // A filter shape beyond the disc reads as the disc.
    BokehShapeParams beyond;
    beyond.filterShape = 3.;
    EXPECT_EQ(BokehKernel::shaped(BokehShapeParams(), 9., 9.).weights(), BokehKernel::shaped(beyond, 9., 9.).weights());
}

// Negative spherical aberration brightens the centre, positive the rim; both stay symmetric.
TEST(BokehKernel, SphericalAberrationProfile)
{
    const double r = 30.;
    for (double k : { -1., -0.4, 0.4, 1. }) {
        BokehShapeParams shape;
        shape.sphericalAberration = k;
        const BokehKernel kernel = BokehKernel::shaped(shape, r, r);
        EXPECT_NEAR(1., kernelSum(kernel), 1e-5) << k;
        for (int dx = 0; dx < 26; ++dx) {
            EXPECT_EQ(kernel.at(dx, 0), kernel.at(-dx, 0));
            EXPECT_EQ(kernel.at(dx, 0), kernel.at(0, dx));
            if (k < 0.) {
                EXPECT_GE(kernel.at(dx, 0), kernel.at(dx + 1, 0)) << k << " " << dx;
            } else {
                EXPECT_LE(kernel.at(dx, 0), kernel.at(dx + 1, 0)) << k << " " << dx;
            }
        }
        // 1 + k (2 rho^2 - 1) at the pixel centres inside.
        const double rho = 20. / r;
        const double expected = (1. + k * (2. * rho * rho - 1.)) / (1. - k);
        if (k < 1.) {
            EXPECT_NEAR(expected, kernel.at(20, 0) / kernel.at(0, 0), 1e-5) << k;
        } else {
            EXPECT_EQ(0.f, kernel.at(0, 0));
        }
    }
}

// The catadioptric hole leaves a ring: nothing inside the hole, flat between it and the rim.
TEST(BokehKernel, CatadioptricHole)
{
    const double r = 20.;
    for (const BokehShapeParams& base : { BokehShapeParams(), blades(6, 0., 0.) }) {
        BokehShapeParams shape = base;
        shape.catadioptric = true;
        shape.catadioptricSize = 0.4;
        const BokehKernel ring = BokehKernel::shaped(shape, r, r);
        EXPECT_NEAR(1., kernelSum(ring), 1e-5);
        EXPECT_EQ(0.f, ring.at(0, 0));
        EXPECT_EQ(0.f, ring.at(5, 3));
        EXPECT_EQ(0.f, ring.at(-7, 0));
        // The hole is round, in the hexagon too, and anti-aliased.
        EXPECT_EQ(ring.at(0, 12), ring.at(12, 0));
        EXPECT_EQ(ring.at(12, 0), ring.at(10, 9));
        EXPECT_GT(ring.at(8, 0), 0.f);
        EXPECT_LT(ring.at(8, 0), ring.at(12, 0));
        if (base.shape == BokehShape::Disc) {
            for (int dy = -ring.haloY(); dy <= ring.haloY(); ++dy) {
                for (int dx = -ring.haloX(); dx <= ring.haloX(); ++dx) {
                    EXPECT_EQ(ring.at(dx, dy), ring.at(-dx, dy)) << dx << "," << dy;
                    EXPECT_EQ(ring.at(dx, dy), ring.at(dy, dx)) << dx << "," << dy;
                }
            }
        }
    }

    // A hole as large as the shape leaves nothing, and the flat shape is used.
    BokehShapeParams whole;
    whole.catadioptric = true;
    whole.catadioptricSize = 1.;
    EXPECT_EQ(BokehKernel::shaped(BokehShapeParams(), 9., 6.).weights(), BokehKernel::shaped(whole, 9., 6.).weights());
}

// The inner polygon: innerBrightness inside innerSize, 1 at the rim, the feather blending the two
// outward above 0.5 and inward below it.
TEST(BokehKernel, InnerPolygonProfile)
{
    const double r = 40.;
    BokehShapeParams hard;
    hard.innerSize = 0.5;
    hard.innerFeather = 0.5;
    hard.innerBrightness = 0.25;
    const BokehKernel step = BokehKernel::shaped(hard, r, r);
    EXPECT_NEAR(1., kernelSum(step), 1e-5);
    EXPECT_FLOAT_EQ(0.25f, step.at(0, 0) / step.at(30, 0));
    EXPECT_EQ(step.at(0, 0), step.at(15, 0));
    EXPECT_EQ(step.at(30, 0), step.at(25, 0));
    // The step is anti-aliased.
    EXPECT_GT(step.at(20, 0), step.at(0, 0));
    EXPECT_LT(step.at(20, 0), step.at(30, 0));

    BokehShapeParams outward = hard;
    outward.innerFeather = 1.;
    const BokehKernel out = BokehKernel::shaped(outward, r, r);
    EXPECT_EQ(out.at(0, 0), out.at(18, 0));
    for (int dx = 21; dx < 38; ++dx) {
        EXPECT_LT(out.at(dx, 0), out.at(dx + 1, 0)) << dx;
    }

    BokehShapeParams inward = hard;
    inward.innerFeather = 0.;
    const BokehKernel in = BokehKernel::shaped(inward, r, r);
    EXPECT_EQ(in.at(22, 0), in.at(35, 0));
    for (int dx = 1; dx < 19; ++dx) {
        EXPECT_LT(in.at(dx, 0), in.at(dx + 1, 0)) << dx;
    }

    // A brighter inner polygon is allowed too.
    BokehShapeParams bright = hard;
    bright.innerBrightness = 3.;
    const BokehKernel lit = BokehKernel::shaped(bright, r, r);
    EXPECT_FLOAT_EQ(3.f, lit.at(0, 0) / lit.at(30, 0));

    // The inner polygon follows the blades: a hexagon with a corner up reaches 40 up but only its
    // apothem, about 34.6, sideways, so 18 up is inside the inner hexagon and 18 sideways is not.
    BokehShapeParams hexagon = blades(6, 0., 0.);
    hexagon.innerSize = 0.5;
    hexagon.innerFeather = 0.5;
    hexagon.innerBrightness = 0.5;
    const BokehKernel hex = BokehKernel::shaped(hexagon, r, r);
    EXPECT_FLOAT_EQ(0.5f, hex.at(0, 18) / hex.at(18, 0));
}

// The filter shape dissolves from a Gaussian, 3 sigma at the rim, to the flat shape.
TEST(BokehKernel, FilterShapeDissolvesToAGaussian)
{
    const double r = 20.;
    BokehShapeParams gaussian;
    gaussian.filterShape = 0.;
    const BokehKernel g = BokehKernel::shaped(gaussian, r, r);
    EXPECT_NEAR(1., kernelSum(g), 1e-5);
    for (int dx : { 3, 6, 10, 15 }) {
        const double rho = dx / r;
        EXPECT_NEAR(std::exp(-rho * rho * 4.5), g.at(dx, 0) / g.at(0, 0), 1e-5) << dx;
        EXPECT_EQ(g.at(dx, 0), g.at(0, -dx));
    }

    const BokehKernel disc = BokehKernel::shaped(BokehShapeParams(), r, r);
    BokehShapeParams half;
    half.filterShape = 0.5;
    const BokehKernel mixed = BokehKernel::shaped(half, r, r);
    EXPECT_NEAR(1., kernelSum(mixed), 1e-5);
    for (int dx : { 0, 5, 12, 19, 20 }) {
        EXPECT_NEAR(0.5 * (disc.at(dx, 0) + g.at(dx, 0)), mixed.at(dx, 0), 1e-7) << dx;
    }
}

// Clips and textures together still give kernels that sum to 1, on every shape and aspect.
TEST(BokehKernel, TexturedClippedKernelsSumToOne)
{
    BokehShapeParams all = blades(5, 12., 0.3);
    all.innerSize = 0.6;
    all.innerFeather = 0.7;
    all.innerBrightness = 0.4;
    all.catadioptric = true;
    all.catadioptricSize = 0.25;
    all.sphericalAberration = 0.3;
    all.filterShape = 0.6;
    all.clip = clipAt(-0.4, 0.3, 1.);
    BokehShapeParams disc = all;
    disc.shape = BokehShape::Disc;
    for (const BokehShapeParams& shape : { all, disc }) {
        for (double r : { 0.6, 1.3, 5., 18.2 }) {
            const BokehKernel kernel = BokehKernel::shaped(shape, r, r * 0.7);
            EXPECT_NEAR(1., kernelSum(kernel), 1e-5) << (int)shape.shape << " " << r;
            for (float w : kernel.weights()) {
                EXPECT_TRUE(std::isfinite(w));
                EXPECT_GE(w, 0.f);
            }
        }
    }
}

TEST(BokehKernel, GatherKernelOfAnUntexturedShapeIsTheShape)
{
    const BokehShapeParams shapes[] = { BokehShapeParams(), blades(5, 20., 0.3), clipped(BokehShapeParams(), clipAt(0.5, 0., 1.)) };

    for (const BokehShapeParams& shape : shapes) {
        EXPECT_FALSE(BokehKernel::isTextured(shape));
        const BokehKernel exact = BokehKernel::shaped(shape, 17.3, 9.6);
        const BokehKernel gather = BokehKernel::gatherKernel(shape, 17.3, 9.6);
        EXPECT_EQ(exact.weights(), gather.weights());
    }
}

TEST(BokehKernel, QuantisedTextureStaysNearTheTexture)
{
    std::vector<BokehShapeParams> shapes(4);
    shapes[0].sphericalAberration = 1.;
    shapes[1].filterShape = 0.;
    shapes[2].innerSize = 0.5;
    shapes[2].innerBrightness = 0.2;
    shapes[3].filterShape = 0.3;
    shapes[3].sphericalAberration = -0.7;
    shapes[3].catadioptric = true;

    for (const BokehShapeParams& shape : shapes) {
        ASSERT_TRUE(BokehKernel::isTextured(shape));
        for (double r : { 3., 20., 60. }) {
            const BokehKernel exact = BokehKernel::shaped(shape, r, r);
            const BokehKernel gather = BokehKernel::gatherKernel(shape, r, r);
            ASSERT_EQ(exact.weights().size(), gather.weights().size());
            const float peak = *std::max_element(exact.weights().begin(), exact.weights().end());
            double maxError = 0.;
            for (std::size_t i = 0; i < exact.weights().size(); ++i) {
                maxError = std::max(maxError, (double)std::fabs(exact.weights()[i] - gather.weights()[i]));
            }
            EXPECT_LE(maxError, peak / BokehKernel::kTextureLevels) << "radius " << r;
            EXPECT_NEAR(kernelSum(gather), 1., 1e-5);
        }
    }
}

// Unquantised, a textured row is a run per pixel and the gather O(r^2) per pixel.
TEST(BokehKernel, TexturedRunsStayBounded)
{
    std::vector<BokehShapeParams> shapes(5);
    shapes[0].sphericalAberration = 1.;
    shapes[1].sphericalAberration = -1.;
    shapes[2].filterShape = 0.;
    shapes[3].innerSize = 0.5;
    shapes[3].innerBrightness = 0.2;
    shapes[4] = clipped(blades(5, 0., 0.), clipAt(0.4, 0., 1.));
    shapes[4].innerBrightness = 0.3;
    shapes[4].innerFeather = 0.8;
    shapes[4].sphericalAberration = 0.5;
    shapes[4].filterShape = 0.6;
    shapes[4].catadioptric = true;

    const std::size_t perRow = 3 * BokehKernel::kTextureLevels;
    for (const BokehShapeParams& shape : shapes) {
        for (double r : { 100., 250. }) {
            const BokehKernel kernel = BokehKernel::gatherKernel(shape, r, r);
            const std::vector<BokehRun> runs = kernel.runs();
            std::vector<std::size_t> rows(kernel.height(), 0);
            for (const BokehRun& run : runs) {
                ++rows[run.dy + kernel.haloY()];
            }
            EXPECT_LE(*std::max_element(rows.begin(), rows.end()), perRow) << "radius " << r;
            EXPECT_LE(runs.size(), (std::size_t)kernel.height() * (2 * BokehKernel::kTextureLevels + 8)) << "radius " << r;
        }
    }
}
