/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
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
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "BaseTest.h"

#include "Engine/Nodes/Filter/DepthOfField.h"

#include "DefocusReference/AnalyticScene.h"
#include "DefocusReference/DefocusMetric.h"
#include "DefocusReference/NiiklCandidate.h"
#include "DefocusReference/Scenes.h"
#include "DefocusReference/ZDefocusCandidate.h"

NATRON_NAMESPACE_USING

using namespace DefocusReference;

namespace {

// The world x (or y) at planar depth `depth` that projects to the pixel boundary `p` along an
// axis of `size` pixels.
double
worldAtPixel(const Camera& camera,
             double p,
             int size,
             double depth)
{
    return (p - size / 2.) / camera.pixelsPerMillimetre() / camera.imageDistance() * depth;
}

double
maxAbsDifference(const std::vector<float>& a,
                 const std::vector<float>& b)
{
    double d = (a.size() == b.size()) ? 0. : HUGE_VAL;

    for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
        d = std::max(d, (double)std::abs(a[i] - b[i]));
    }

    return d;
}

} // namespace

// A surface at the focus distance is sharp: every aperture point sees it at the pinhole's film
// point, so with its edges on pixel boundaries the thin lens gives the pinhole image.
TEST(DefocusReference, PlaneAtFocusMatchesPinhole)
{
    AnalyticScene scene;
    Camera& c = scene.camera;

    c.width = 64;
    c.height = 36;
    c.focusDistance = 2.;
    c.fStop = 1.4;
    const double s = c.focusDistance;
    const double left = worldAtPixel(c, -8., c.width, s);
    const double split = worldAtPixel(c, 24., c.width, s);
    const double right = worldAtPixel(c, 72., c.width, s);
    const double bottom = worldAtPixel(c, -8., c.height, s);
    const double top = worldAtPixel(c, 30., c.height, s);
    scene.addCard(Vec3((left + split) / 2., (bottom + top) / 2., -s), Vec3(0., 0., 1.), Vec3(1., 0., 0.), (split - left) / 2., (top - bottom) / 2., Colour(0.9f, 0.3f, 0.1f));
    scene.addCard(Vec3((split + right) / 2., (bottom + top) / 2., -s), Vec3(0., 0., 1.), Vec3(1., 0., 0.), (right - split) / 2., (top - bottom) / 2., Colour(0.1f, 0.4f, 0.8f, 0.5f));

    const PinholeImage pinhole = scene.renderPinhole();
    const std::vector<float> lens = scene.renderThinLens(64, 7);
    EXPECT_LT(maxAbsDifference(pinhole.rgba, lens), 1e-4);
    // The sky above row 30 is empty in both.
    EXPECT_EQ(0.f, lens[((std::size_t)33 * c.width + 10) * 4 + 3]);
    EXPECT_NEAR(1.f, lens[((std::size_t)10 * c.width + 10) * 4 + 3], 1e-6);
}

// An emissive point off focus spreads to a uniform disc of the circle of confusion ZDefocus's
// Lens mode gives its depth: the disc's second moment about its centre is R^2 / 2.
TEST(DefocusReference, PointDiscMatchesLensCircleOfConfusion)
{
    AnalyticScene scene;
    Camera& c = scene.camera;

    c.width = 256;
    c.height = 144;
    c.focusDistance = 1.;
    c.fStop = 1.4;
    const double depth = 3.;
    const double energy = 100.;
    scene.addPoint(Vec3(0., 0., -depth), 0.5, Colour((float)energy, (float)energy, (float)energy));

    const std::vector<float> image = scene.renderThinLens(16, 3);
    double sum = 0.;
    double moment = 0.;
    for (int y = 0; y < c.height; ++y) {
        for (int x = 0; x < c.width; ++x) {
            const double v = image[((std::size_t)y * c.width + x) * 4];
            const double dx = x + 0.5 - c.width / 2.;
            const double dy = y + 0.5 - c.height / 2.;
            sum += v;
            moment += v * (dx * dx + dy * dy);
        }
    }
    ASSERT_GT(sum, 0.);
    EXPECT_NEAR(energy, sum, energy * 0.02);
    // A pixel's own extent adds 1/6 to the second moment of what lands in it.
    const double radius = std::sqrt(2. * (moment / sum - 1. / 6.));

    DepthOfFieldParams params;
    params.units = DepthUnits::Distance;
    params.mode = DepthOfFieldMode::Lens;
    params.focalPlane = c.focusDistance;
    params.focalLength = c.focalLength;
    params.fStop = c.fStop;
    params.filmBackWidth = c.filmBackWidth;
    params.imageWidth = c.width;
    params.millimetresPerUnit = c.millimetresPerUnit;
    params.maxSize = 500.;
    const DepthOfField dof(params);
    EXPECT_NEAR(dof.coc(depth) / 2., radius, 0.5);
    EXPECT_NEAR(dof.coc(depth), c.cocDiameter(depth), 1e-6);
    EXPECT_GT(radius, 3.);
}

// The truth is a function of the seed alone.
TEST(DefocusReference, SameSeedIsBitIdentical)
{
    const FixtureScene f = makeFixtureScene("eclipsedBokeh", 48, 27);
    const std::vector<float> a = f.scene.renderThinLens(16, 11);
    const std::vector<float> b = f.scene.renderThinLens(16, 11);
    const std::vector<float> other = f.scene.renderThinLens(16, 12);

    ASSERT_EQ(a.size(), b.size());
    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a == other);
}

// Every fixture scene sees its near surface, stays within its depth bounds, spans a real depth
// range and leaves part of the frame uncovered.
TEST(DefocusReference, FixtureScenesCoverTheirDepthRange)
{
    for (const std::string& name : fixtureSceneNames()) {
        const FixtureScene f = makeFixtureScene(name, 64, 36);
        ASSERT_EQ(name, f.name);
        ASSERT_FALSE(f.scene.primitives.empty()) << name;
        const PinholeImage p = f.scene.renderPinhole(f.pinhole);
        float nearest = HUGE_VALF;
        float farthest = 0.f;
        int covered = 0;
        for (std::size_t i = 0; i < p.depth.size(); ++i) {
            if (p.depth[i] > 0.f) {
                nearest = std::min(nearest, p.depth[i]);
                farthest = std::max(farthest, p.depth[i]);
            }
            if (p.rgba[i * 4 + 3] > 0.5f) {
                ++covered;
            }
        }
        const double n = (double)p.depth.size();
        EXPECT_GE(nearest, f.nearDepth * (1. - 1e-4)) << name;
        EXPECT_LE(nearest, f.nearDepth * 1.1) << name;
        EXPECT_LE(farthest, f.farDepth * (1. + 1e-4)) << name;
        EXPECT_GT(farthest, 2. * nearest) << name;
        EXPECT_GT(covered, 0.05 * n) << name;
        EXPECT_LT(covered, 0.95 * n) << name;
    }
}

// An image scores 0 against itself.
TEST(DefocusReference, IdenticalImagesScoreZero)
{
    const FixtureScene f = makeFixtureScene("sharpFgBlurBg", 64, 36);
    const PinholeImage p = f.scene.renderPinhole(f.pinhole);
    const std::vector<unsigned char> band = edgeBand(f.scene.camera, p.depth);
    const DefocusScore score = scoreDefocus(p.width, p.height, p.rgba, p.rgba, band);

    EXPECT_GT(score.edgePixels, 0);
    EXPECT_EQ(0., score.rmse);
    EXPECT_EQ(0., score.edgeRmse);
    EXPECT_EQ(0., score.halo);
    EXPECT_EQ(0., score.hole);
}

// Moving a depth edge by a pixel is an error inside the edge band only.
TEST(DefocusReference, ShiftedEdgeRaisesOnlyEdgeBandError)
{
    Camera c;
    c.width = 64;
    c.height = 36;
    c.focusDistance = 1.;
    c.fStop = 1.4;
    const int n = c.width * c.height;
    std::vector<float> depth(n);
    std::vector<float> truth((std::size_t)n * 4);
    std::vector<float> shifted((std::size_t)n * 4);
    for (int y = 0; y < c.height; ++y) {
        for (int x = 0; x < c.width; ++x) {
            const std::size_t i = (std::size_t)y * c.width + x;
            depth[i] = (x < 30) ? 1.f : 4.f;
            for (int k = 0; k < 4; ++k) {
                truth[i * 4 + k] = (x < 30) ? 0.8f : 0.2f;
                shifted[i * 4 + k] = (x < 31) ? 0.8f : 0.2f;
            }
        }
    }
    const std::vector<unsigned char> band = edgeBand(c, depth);
    std::vector<unsigned char> outside(band.size());
    for (std::size_t i = 0; i < band.size(); ++i) {
        outside[i] = band[i] ? 0 : 1;
    }

    const DefocusScore inBand = scoreDefocus(c.width, c.height, truth, shifted, band);
    const DefocusScore outOfBand = scoreDefocus(c.width, c.height, truth, shifted, outside);
    EXPECT_GT(inBand.edgePixels, 0);
    EXPECT_LT(inBand.edgePixels, n);
    EXPECT_GT(inBand.rmse, 0.);
    EXPECT_GT(inBand.edgeRmse, inBand.rmse);
    EXPECT_EQ(0., outOfBand.edgeRmse);
}

// The scatter engine defocuses a blurred foreground into a finite, lit image with alpha in [0, 1].
TEST(DefocusReference, NiiklCandidateRenders)
{
    const FixtureScene f = makeFixtureScene("blurFgSharpBg", 64, 36);
    const PinholeImage p = f.scene.renderPinhole(f.pinhole);
    NiiklSettings settings;
    std::vector<float> out;
    std::string error;

    ASSERT_TRUE(runNiiklDefocus(f.scene.camera, p, settings, &out, &error)) << error;
    ASSERT_EQ(p.rgba.size(), out.size());
    double lit = 0.;
    for (std::size_t i = 0; i < out.size(); ++i) {
        ASSERT_TRUE(std::isfinite(out[i])) << i;
        if (i % 4 == 3) {
            EXPECT_GE(out[i], -1e-5f) << i;
            EXPECT_LE(out[i], 1.f + 1e-5f) << i;
        } else {
            lit += out[i];
        }
    }
    EXPECT_GT(lit, 0.);
    EXPECT_GT(maxAbsDifference(p.rgba, out), 1e-3);
}

class DefocusReferenceZDefocusTest
    : public BaseTest {
};

// ZDefocus at the scene's lens leaves the in-focus foreground as the pinhole saw it, at every
// layer count the harness scores.
TEST_F(DefocusReferenceZDefocusTest, InFocusForegroundMatchesPinhole)
{
    const FixtureScene f = makeFixtureScene("sharpFgBlurBg", 64, 36);
    const PinholeImage p = f.scene.renderPinhole(f.pinhole);
    ZDefocusCandidate candidate(getApp());

    // The foreground card covers x < 32, y < 27; the background's blur reaches under 2 pixels in.
    for (int layers : { 8, 16, 32 }) {
        std::vector<float> out;
        std::string error;
        ASSERT_TRUE(candidate.render(f.scene.camera, p, layers, &out, &error)) << layers << ": " << error;
        ASSERT_EQ(p.rgba.size(), out.size()) << layers;
        double worst = 0.;
        for (int y = 0; y < 23; ++y) {
            for (int x = 0; x < 28; ++x) {
                const std::size_t i = (std::size_t)y * p.width + x;
                ASSERT_NEAR(1., p.depth[i], 1e-4) << x << "," << y;
                for (int k = 0; k < 4; ++k) {
                    worst = std::max(worst, (double)std::abs(out[i * 4 + k] - p.rgba[i * 4 + k]));
                }
            }
        }
        EXPECT_LT(worst, 1e-3) << layers;
        EXPECT_GT(maxAbsDifference(p.rgba, out), 1e-3) << layers;
    }
}
