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

#include "Global/Macros.h"

#include <cmath>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/VectorFollow.h"
#include "Engine/VectorFollowModels.h"

NATRON_NAMESPACE_ENTER

namespace {

constexpr double kRecovered = 1e-3;

std::vector<FollowPoint>
gridOfPoints(int n)
{
    std::vector<FollowPoint> points;
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            points.push_back(FollowPoint(100. + x * 10., 80. + y * 10.));
        }
    }

    return points;
}

std::vector<FollowPoint>
moveAll(const FollowMatrix& matrix,
        const std::vector<FollowPoint>& points)
{
    std::vector<FollowPoint> out;
    out.reserve(points.size());
    for (const FollowPoint& p : points) {
        out.push_back(matrix.apply(p));
    }

    return out;
}

// Every fifth correspondence goes somewhere unrelated.
std::vector<FollowPoint>
withOutliers(const std::vector<FollowPoint>& points)
{
    std::mt19937 random(42);
    std::uniform_real_distribution<double> offset(30., 90.);
    std::vector<FollowPoint> out(points);
    for (std::size_t i = 0; i < out.size(); i += 5) {
        out[i].x += offset(random);
        out[i].y -= offset(random);
    }

    return out;
}

void
expectSameMotion(const FollowMatrix& fitted,
                 const FollowMatrix& expected,
                 const std::vector<FollowPoint>& points)
{
    for (const FollowPoint& p : points) {
        const FollowPoint a = fitted.apply(p);
        const FollowPoint b = expected.apply(p);
        EXPECT_NEAR(a.x, b.x, kRecovered);
        EXPECT_NEAR(a.y, b.y, kRecovered);
    }
}

FollowMatrix
sampleAffine()
{
    FollowMatrix m;
    m.m[0] = 1.02;
    m.m[1] = -0.05;
    m.m[2] = 4.;
    m.m[3] = 0.03;
    m.m[4] = 0.98;
    m.m[5] = -2.5;

    return m;
}

FollowMatrix
sampleHomography()
{
    FollowMatrix m = sampleAffine();
    m.m[6] = 2e-4;
    m.m[7] = -1e-4;

    return m;
}

// A field whose motion is a matrix, read through a mask of confidence.
class MatrixField
    : public VectorField {
public:
    explicit MatrixField(const FollowMatrix& matrix)
        : _matrix(matrix)
    {
        matrix.inverted(&_inverse);
    }

    // Everything at x >= this is occluded.
    double occludedFromX = 1e30;
    int noneConfidentFrom = 1 << 30;

    virtual bool prepareFrame(int /*frame*/) OVERRIDE FINAL
    {
        return true;
    }

    virtual VectorSample sample(int frame,
                                double x,
                                double y) OVERRIDE FINAL
    {
        const FollowPoint p(x, y);
        const FollowPoint next = _matrix.apply(p);
        const FollowPoint previous = _inverse.apply(p);
        VectorSample s;
        if ((x >= occludedFromX) || (frame >= noneConfidentFrom)) {
            s.forwardConfidence = 0.;
            s.backwardConfidence = 0.;
            // What an occluded region really reads: wrong vectors, not missing ones.
            s.forwardX = 50.;
            s.backwardX = -50.;

            return s;
        }
        s.forwardX = next.x - x;
        s.forwardY = next.y - y;
        s.backwardX = previous.x - x;
        s.backwardY = previous.y - y;

        return s;
    }

private:
    const FollowMatrix _matrix;
    FollowMatrix _inverse;
};

std::vector<FollowPoint>
squareOutline()
{
    std::vector<FollowPoint> points;
    points.push_back(FollowPoint(100., 100.));
    points.push_back(FollowPoint(160., 100.));
    points.push_back(FollowPoint(160., 160.));
    points.push_back(FollowPoint(100., 160.));

    return points;
}

VectorFollowAnchor
anchorOf(int frame,
         bool withPoints)
{
    VectorFollowAnchor anchor;
    anchor.frame = frame;
    const std::vector<FollowPoint> outline = squareOutline();
    if (withPoints) {
        anchor.points = outline;
    }
    anchor.samples = followGridSamplesInside(outline);
    anchor.samples.insert(anchor.samples.end(), outline.begin(), outline.end());

    return anchor;
}

FollowMatrix
rotationAboutTheOutlineCentre(double degrees)
{
    const double radians = degrees * M_PI / 180.;
    FollowMatrix rotation;
    rotation.m[0] = std::cos(radians);
    rotation.m[1] = -std::sin(radians);
    rotation.m[3] = std::sin(radians);
    rotation.m[4] = std::cos(radians);

    return FollowMatrix::translation(130., 130.) * rotation * FollowMatrix::translation(-130., -130.);
}

std::vector<VectorFollowFrame>
bridgeOnStillPicture(VectorFollowModel model,
                     const FollowMatrix& atFrame0,
                     const FollowMatrix& atFrame10)
{
    MatrixField field { FollowMatrix() };

    VectorFollowShapeSetup setup;
    setup.first = 0;
    setup.last = 10;
    setup.model = model;
    setup.corners = squareOutline();
    VectorFollowAnchor a = anchorOf(0, false);
    VectorFollowAnchor b = anchorOf(10, false);
    a.transform = atFrame0;
    b.transform = atFrame10;
    setup.anchors.push_back(a);
    setup.anchors.push_back(b);

    std::vector<VectorFollowFrame> frames;
    followVectorShape(field, setup, [&frames](const VectorFollowFrame& f) { frames.push_back(f); }, []() { return false; });

    return frames;
}

} // anonymous namespace

TEST(VectorFollowModels, ExactTranslateAffineAndHomographyAreRecovered)
{
    const std::vector<FollowPoint> src = gridOfPoints(6);

    const FollowMatrix t = FollowMatrix::translation(3.25, -1.5);
    VectorFollowFit fit = solveFollowMotion(VectorFollowModel::eTranslate, src, moveAll(t, src));
    ASSERT_TRUE(fit.valid);
    expectSameMotion(fit.matrix, t, src);

    fit = solveFollowMotion(VectorFollowModel::eAffine, src, moveAll(sampleAffine(), src));
    ASSERT_TRUE(fit.valid);
    EXPECT_EQ(fit.model, VectorFollowModel::eAffine);
    expectSameMotion(fit.matrix, sampleAffine(), src);

    fit = solveFollowMotion(VectorFollowModel::ePerspective, src, moveAll(sampleHomography(), src));
    ASSERT_TRUE(fit.valid);
    EXPECT_EQ(fit.model, VectorFollowModel::ePerspective);
    expectSameMotion(fit.matrix, sampleHomography(), src);
}

TEST(VectorFollowModels, AffineSurvivesTwentyPercentOutliers)
{
    const std::vector<FollowPoint> src = gridOfPoints(10);
    const std::vector<FollowPoint> dst = withOutliers(moveAll(sampleAffine(), src));

    const VectorFollowFit fit = fitFollowMotion(VectorFollowModel::eAffine, src, dst, std::vector<double>(src.size(), 1.));
    ASSERT_TRUE(fit.valid);
    expectSameMotion(fit.matrix, sampleAffine(), src);
}

TEST(VectorFollowModels, HomographySurvivesTwentyPercentOutliers)
{
    const std::vector<FollowPoint> src = gridOfPoints(10);
    const std::vector<FollowPoint> dst = withOutliers(moveAll(sampleHomography(), src));

    const VectorFollowFit fit = fitFollowMotion(VectorFollowModel::ePerspective, src, dst, std::vector<double>(src.size(), 1.));
    ASSERT_TRUE(fit.valid);
    expectSameMotion(fit.matrix, sampleHomography(), src);
}

TEST(VectorFollowModels, ZeroWeightSamplesAreLeftOut)
{
    const std::vector<FollowPoint> src = gridOfPoints(4);
    std::vector<FollowPoint> dst = moveAll(sampleAffine(), src);
    std::vector<double> weights(src.size(), 1.);
    for (std::size_t i = 0; i < src.size(); i += 2) {
        dst[i].x += 1000.;
        weights[i] = 0.;
    }
    const VectorFollowFit fit = fitFollowMotion(VectorFollowModel::eAffine, src, dst, weights);
    ASSERT_TRUE(fit.valid);
    expectSameMotion(fit.matrix, sampleAffine(), src);
}

TEST(VectorFollowModels, DegenerateSamplesFallBack)
{
    std::vector<FollowPoint> collinear;
    for (int i = 0; i < 8; ++i) {
        collinear.push_back(FollowPoint(i * 5., i * 5.));
    }
    const FollowMatrix t = FollowMatrix::translation(2., 1.);

    const VectorFollowFit fit = solveFollowMotion(VectorFollowModel::ePerspective, collinear, moveAll(t, collinear));
    ASSERT_TRUE(fit.valid);
    EXPECT_EQ(fit.model, VectorFollowModel::eTranslate);
    expectSameMotion(fit.matrix, t, collinear);

    const VectorFollowFit single = fitFollowMotion(VectorFollowModel::eAffine,
                                                   std::vector<FollowPoint>(1, FollowPoint(1., 1.)),
                                                   std::vector<FollowPoint>(1, FollowPoint(3., 2.)),
                                                   std::vector<double>(1, 1.));
    ASSERT_TRUE(single.valid);
    EXPECT_EQ(single.model, VectorFollowModel::eTranslate);

    const VectorFollowFit none = fitFollowMotion(VectorFollowModel::eAffine,
                                                 std::vector<FollowPoint>(1, FollowPoint(1., 1.)),
                                                 std::vector<FollowPoint>(1, FollowPoint(3., 2.)),
                                                 std::vector<double>(1, 0.));
    EXPECT_FALSE(none.valid);
}

TEST(VectorFollowModels, GridStaysInsideThePolygon)
{
    std::vector<FollowPoint> triangle;
    triangle.push_back(FollowPoint(0., 0.));
    triangle.push_back(FollowPoint(160., 0.));
    triangle.push_back(FollowPoint(0., 160.));

    const std::vector<FollowPoint> samples = followGridSamplesInside(triangle);
    EXPECT_GT(samples.size(), 50u);
    for (const FollowPoint& p : samples) {
        EXPECT_GE(p.x, 0.);
        EXPECT_GE(p.y, 0.);
        EXPECT_LE(p.x + p.y, 160.);
    }
    EXPECT_TRUE(followGridSamplesInside(std::vector<FollowPoint>(2, FollowPoint())).empty());
}

TEST(VectorFollowModels, AnOccludedHalfFollowsTheVisibleHalf)
{
    const FollowMatrix motion = FollowMatrix::translation(1., 0.5);
    MatrixField field(motion);
    field.occludedFromX = 145.;

    VectorFollowShapeSetup setup;
    setup.first = 0;
    setup.last = 10;
    setup.model = VectorFollowModel::ePerPoint;
    setup.anchors.push_back(anchorOf(0, true));

    std::vector<VectorFollowFrame> frames;
    const VectorFollowShapeOutcome outcome = followVectorShape(
        field, setup, [&frames](const VectorFollowFrame& f) { frames.push_back(f); }, []() { return false; });

    ASSERT_FALSE(frames.empty());
    EXPECT_FALSE(outcome.stopped);
    ASSERT_EQ(frames.back().frame, 10);

    FollowMatrix expected;
    for (int i = 0; i < 10; ++i) {
        expected = motion * expected;
    }
    const std::vector<FollowPoint> outline = squareOutline();
    for (std::size_t i = 0; i < outline.size(); ++i) {
        const FollowPoint e = expected.apply(outline[i]);
        EXPECT_NEAR(frames.back().points[i].x, e.x, 1e-2);
        EXPECT_NEAR(frames.back().points[i].y, e.y, 1e-2);
    }

    ASSERT_EQ(outcome.inferred.size(), 1u);
    EXPECT_EQ(outcome.inferred[0].first, 1);
    EXPECT_EQ(outcome.inferred[0].last, 10);
}

TEST(VectorFollowModels, AFitFollowsItsMatrix)
{
    const FollowMatrix motion = sampleAffine();
    MatrixField field(motion);

    VectorFollowShapeSetup setup;
    setup.first = 0;
    setup.last = 5;
    setup.model = VectorFollowModel::eAffine;
    setup.anchors.push_back(anchorOf(0, false));

    std::vector<VectorFollowFrame> frames;
    followVectorShape(field, setup, [&frames](const VectorFollowFrame& f) { frames.push_back(f); }, []() { return false; });
    ASSERT_EQ(frames.size(), 5u);

    FollowMatrix expected;
    for (int i = 0; i < 5; ++i) {
        expected = motion * expected;
    }
    expectSameMotion(frames.back().transform, expected, squareOutline());
    EXPECT_TRUE(frames.back().points.empty());
}

TEST(VectorFollowModels, FitsBlendAsARotationAboutTheShapeNotAsMatrixEntries)
{
    for (VectorFollowModel model : { VectorFollowModel::eAffine, VectorFollowModel::ePerspective }) {
        const std::vector<VectorFollowFrame> frames = bridgeOnStillPicture(model, FollowMatrix(), rotationAboutTheOutlineCentre(40.));
        ASSERT_EQ(frames.size(), 9u);
        expectSameMotion(frames[4].transform, rotationAboutTheOutlineCentre(20.), squareOutline());
        expectSameMotion(frames[1].transform, rotationAboutTheOutlineCentre(40. * followBridgeWeight(2, 0, 10)), squareOutline());
    }
}

TEST(VectorFollowModels, ABlendTakesTheShortWayAroundARotation)
{
    const std::vector<VectorFollowFrame> frames = bridgeOnStillPicture(VectorFollowModel::eAffine, rotationAboutTheOutlineCentre(170.), rotationAboutTheOutlineCentre(-170.));
    ASSERT_EQ(frames.size(), 9u);
    expectSameMotion(frames[4].transform, rotationAboutTheOutlineCentre(180.), squareOutline());
}

TEST(VectorFollowModels, BlendingATransformWithItselfGivesItBack)
{
    FollowMatrix affine;
    affine.m[0] = 1.3;
    affine.m[1] = 0.4;
    affine.m[2] = 12.;
    affine.m[3] = -0.2;
    affine.m[4] = 0.8;
    affine.m[5] = -7.;
    FollowMatrix perspective = affine;
    perspective.m[6] = 1e-4;
    perspective.m[7] = -2e-4;

    const std::vector<VectorFollowFrame> fromAffine = bridgeOnStillPicture(VectorFollowModel::eAffine, affine, affine);
    const std::vector<VectorFollowFrame> fromPerspective = bridgeOnStillPicture(VectorFollowModel::ePerspective, perspective, perspective);
    ASSERT_EQ(fromAffine.size(), 9u);
    ASSERT_EQ(fromPerspective.size(), 9u);
    expectSameMotion(fromAffine[4].transform, affine, squareOutline());
    expectSameMotion(fromPerspective[4].transform, perspective, squareOutline());
}

TEST(VectorFollowModels, AFitBlendsPositionLinearlyAcrossTheBridge)
{
    const std::vector<VectorFollowFrame> frames = bridgeOnStillPicture(VectorFollowModel::eTranslate, FollowMatrix::translation(2., 4.), FollowMatrix::translation(6., -4.));
    ASSERT_EQ(frames.size(), 9u);
    EXPECT_NEAR(frames[4].transform.m[2], 4., 1e-9);
    EXPECT_NEAR(frames[4].transform.m[5], 0., 1e-9);
}

TEST(VectorFollowModels, AFitIsExactOnBothAnchorsOfABridge)
{
    MatrixField field(FollowMatrix::translation(1., 0.));

    VectorFollowShapeSetup setup;
    setup.first = 0;
    setup.last = 10;
    setup.model = VectorFollowModel::eTranslate;
    setup.corners = squareOutline();

    VectorFollowAnchor a = anchorOf(0, false);
    VectorFollowAnchor b = anchorOf(10, false);
    // Off the vectors' prediction of ten pixels, so the crossfade has something to absorb.
    b.transform = FollowMatrix::translation(14., 0.);
    setup.anchors.push_back(a);
    setup.anchors.push_back(b);

    std::vector<VectorFollowFrame> frames;
    followVectorShape(field, setup, [&frames](const VectorFollowFrame& f) { frames.push_back(f); }, []() { return false; });
    ASSERT_EQ(frames.size(), 9u);
    EXPECT_EQ(frames.front().frame, 1);
    EXPECT_NEAR(frames.front().transform.m[2], 1.0 + followBridgeWeight(1, 0, 10) * 4., 1e-9);
    EXPECT_NEAR(frames.back().transform.m[2], 9.0 + followBridgeWeight(9, 0, 10) * 4., 1e-9);
}

TEST(VectorFollowModels, StopRuleFiresBelowAQuarterConfident)
{
    MatrixField field(FollowMatrix::translation(1., 0.));
    field.occludedFromX = 112.;

    VectorFollowShapeSetup setup;
    setup.first = 0;
    setup.last = 5;
    setup.model = VectorFollowModel::eAffine;
    VectorFollowAnchor anchor = anchorOf(0, false);
    setup.anchors.push_back(anchor);

    std::vector<VectorFollowFrame> frames;
    const VectorFollowShapeOutcome outcome = followVectorShape(
        field, setup, [&frames](const VectorFollowFrame& f) { frames.push_back(f); }, []() { return false; });
    EXPECT_TRUE(outcome.stopped);
    EXPECT_EQ(outcome.stopFrame, 0);
    EXPECT_TRUE(frames.empty());
}

TEST(VectorFollowModels, StopRuleStopsAtTheFrameConfidenceRanOut)
{
    MatrixField field(FollowMatrix::translation(1., 0.));
    field.noneConfidentFrom = 3;

    VectorFollowShapeSetup setup;
    setup.first = 0;
    setup.last = 8;
    setup.model = VectorFollowModel::eTranslate;
    setup.anchors.push_back(anchorOf(0, false));

    std::vector<VectorFollowFrame> frames;
    const VectorFollowShapeOutcome outcome = followVectorShape(
        field, setup, [&frames](const VectorFollowFrame& f) { frames.push_back(f); }, []() { return false; });
    EXPECT_TRUE(outcome.stopped);
    EXPECT_EQ(outcome.stopFrame, 3);
    ASSERT_FALSE(frames.empty());
    EXPECT_EQ(frames.back().frame, 3);
    EXPECT_NEAR(frames.back().transform.m[2], 3., 1e-9);
}

TEST(VectorFollowModels, InferredFramesOfAFitAreReported)
{
    MatrixField field(FollowMatrix::translation(1., 0.));
    field.occludedFromX = 145.;

    VectorFollowShapeSetup setup;
    setup.first = 0;
    setup.last = 3;
    setup.model = VectorFollowModel::eTranslate;
    setup.anchors.push_back(anchorOf(0, false));

    const VectorFollowShapeOutcome outcome = followVectorShape(
        field, setup, [](const VectorFollowFrame&) {}, []() { return false; });
    ASSERT_EQ(outcome.inferred.size(), 1u);
    EXPECT_EQ(outcome.inferred[0].first, 1);
    EXPECT_EQ(outcome.inferred[0].last, 3);
}

NATRON_NAMESPACE_EXIT
