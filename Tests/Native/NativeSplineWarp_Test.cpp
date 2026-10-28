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
#include <functional>
#include <list>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativeParity.h"
#include "NativePluginIdentity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Bezier.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/Image/TransformMath.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Transform/CornerPinKnobs.h"
#include "Engine/Nodes/Transform/Crop.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarp.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpContext.h"
#include "Engine/Nodes/Transform/Transform.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/RotoItem.h"
#include "Engine/Settings.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

using TransformMath::Mat3;

namespace {

const double kTime = 1.;

// A warp that is exactly a translation against a Transform of the same offset: both resample
// once, through different code paths.
const double kWarpTolerance = 1e-4;

bool
isNative(const NodePtr& node)
{
    return node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
}

SplineWarpContextPtr
contextOf(const NodePtr& node)
{
    return node ? std::dynamic_pointer_cast<SplineWarpContext>(node->getCurveContext()) : SplineWarpContextPtr();
}

// A closed Bezier through points, keyed at kTime.
BezierPtr
makeCurve(const SplineWarpContextPtr& ctx,
          const std::vector<std::pair<double, double>>& points)
{
    BezierPtr bezier = ctx->makeBezier(points[0].first, points[0].second, kRotoBezierBaseName, kTime, false);
    if (!bezier) {
        return bezier;
    }
    for (std::size_t i = 1; i < points.size(); ++i) {
        bezier->addControlPoint(points[i].first, points[i].second, kTime);
    }
    bezier->setCurveFinished(true);

    return bezier;
}

// A quadrilateral at (x, y), scaled by s about (cx, cy), then moved by (dx, dy).
BezierPtr
makeQuad(const SplineWarpContextPtr& ctx,
         double x,
         double y,
         double size,
         double dx = 0.,
         double dy = 0.,
         double s = 1.)
{
    const double base[4][2] = { { x, y }, { x + size, y }, { x + size * 1.2, y + size }, { x - size * 0.1, y + size * 0.8 } };
    const double cx = x + size * 0.5;
    const double cy = y + size * 0.5;
    std::vector<std::pair<double, double>> points;
    for (int i = 0; i < 4; ++i) {
        points.push_back(std::make_pair(cx + s * (base[i][0] - cx) + dx, cy + s * (base[i][1] - cy) + dy));
    }

    return makeCurve(ctx, points);
}

BezierPtr
makeRectangle(const SplineWarpContextPtr& ctx,
              double x1,
              double y1,
              double x2,
              double y2)
{
    std::vector<std::pair<double, double>> points;
    points.push_back(std::make_pair(x1, y1));
    points.push_back(std::make_pair(x2, y1));
    points.push_back(std::make_pair(x2, y2));
    points.push_back(std::make_pair(x1, y2));

    return makeCurve(ctx, points);
}

// A link moving a quadrilateral by (dx, dy): the warp is that translation everywhere.
SplineWarpLinkPtr
linkTranslation(const SplineWarpContextPtr& ctx,
                double x,
                double y,
                double size,
                double dx,
                double dy)
{
    BezierPtr source = makeQuad(ctx, x, y, size);
    BezierPtr destination = makeQuad(ctx, x, y, size, dx, dy);
    if (!source || !destination) {
        return SplineWarpLinkPtr();
    }

    return ctx->linkWithAlignment(source, destination, 0., false);
}

bool
renderRGBA(const NodePtr& node,
           const RectI& window,
           RenderedPlane* plane)
{
    std::list<ImageLayerDesc> layers;
    layers.push_back(ImageLayerDesc::getRGBAComponents());
    std::vector<RenderedPlane> planes;
    std::string error;
    const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, layers, &planes, &error);
    EXPECT_TRUE(ok) << node->getScriptName() << ": " << error;
    if (!ok || (planes.size() != 1) || (planes[0].channels.size() != 4)) {
        return false;
    }
    *plane = planes[0];

    return true;
}

const float*
pixelAt(const RenderedPlane& plane,
        int x,
        int y)
{
    const std::size_t n = plane.channels.size();

    return &plane.pixels[((std::size_t)(y - plane.window.y1) * plane.window.width() + (x - plane.window.x1)) * n];
}

// The largest difference between a and b over window, which both planes hold.
double
maxDiff(const RenderedPlane& a,
        const RenderedPlane& b,
        const RectI& window)
{
    double diff = 0.;

    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            const float* pa = pixelAt(a, x, y);
            const float* pb = pixelAt(b, x, y);
            for (int c = 0; c < 4; ++c) {
                diff = std::max(diff, (double)std::abs(pa[c] - pb[c]));
            }
        }
    }

    return diff;
}

RectD
rodOf(const NodePtr& node)
{
    EffectInstancePtr effect = node->getEffectInstance();
    RectD rod;
    bool isProjectFormat = false;
    EXPECT_NE(eStatusFailed, effect->getRegionOfDefinition_public(effect->getRenderHash(), kTime, RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat)) << node->getScriptName();

    return rod;
}

bool
isIdentityOnSource(const NodePtr& node)
{
    EffectInstancePtr effect = node->getEffectInstance();
    const RectI window(0, 0, kParitySourceWidth, kParitySourceHeight);
    double inputTime = 0.;
    ViewIdx inputView(0);
    int inputNb = -1;
    const bool identity = effect->isIdentity_public(false, effect->getRenderHash(), kTime, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, &inputNb);

    return identity && (inputNb == kSplineWarpInputSource);
}

void
expectRectNear(const RectD& expected,
               const RectD& actual,
               const char* what)
{
    EXPECT_NEAR(expected.x1, actual.x1, 1e-6) << what;
    EXPECT_NEAR(expected.y1, actual.y1, 1e-6) << what;
    EXPECT_NEAR(expected.x2, actual.x2, 1e-6) << what;
    EXPECT_NEAR(expected.y2, actual.y2, 1e-6) << what;
}

} // namespace

class NativeSplineWarpTest
    : public BaseTest {
protected:
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeSplineWarp64x48", 1.));
    }

    NodePtr makeSource()
    {
        NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
        EXPECT_TRUE(bool(source));
        if (source) {
            setParitySourceOrigin(source, 0, 0);
        }

        return source;
    }

    NodePtr makeNode(const char* id,
                     int major,
                     const NodePtr& input)
    {
        CreateNodeArgs args(id, getApp()->getProject());

        args.setProperty<int>(kCreateNodeArgsPropPluginVersion, major, 0);
        args.setProperty<int>(kCreateNodeArgsPropPluginVersion, -1, 1);
        args.setProperty<bool>(kCreateNodeArgsPropAllowNonUserCreatablePlugins, true);
        NodePtr node = getApp()->createNode(args);

        EXPECT_TRUE(bool(node)) << id;
        if (node && input) {
            connectNodes(input, node, 0, true);
        }

        return node;
    }

    NodePtr makeSplineWarp(const NodePtr& input)
    {
        NodePtr node = makeNode(PLUGINID_NATRON_SPLINEWARP, PLUGIN_MAJOR_NATRON_SPLINEWARP, input);

        EXPECT_TRUE(isNative(node));
        EXPECT_TRUE(bool(contextOf(node)));

        return node;
    }

    NodePtr makeTranslate(const NodePtr& input,
                          double dx,
                          double dy)
    {
        NodePtr node = makeNode(PLUGINID_NATRON_TRANSFORM, PLUGIN_MAJOR_NATRON_TRANSFORM, input);

        if (node) {
            EXPECT_TRUE(setKnobValues(node, kTransformNodeParamTranslate, { dx, dy }));
        }

        return node;
    }
};

TEST_F(NativeSplineWarpTest, RegisteredIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_SPLINEWARP, PLUGIN_MAJOR_NATRON_SPLINEWARP, "SplineWarp");
}

TEST_F(NativeSplineWarpTest, KnobsAndDefaults)
{
    resetProject();
    NodePtr node = makeSplineWarp(NodePtr());
    ASSERT_TRUE(bool(node));

    const char* const names[] = {
        kSplineWarpParamAmount,
        kSplineWarpParamInvert,
        kSplineWarpParamSolver,
        kSplineWarpParamSmoothness,
        kSplineWarpParamCurveResolution,
        kSplineWarpParamFieldResolution,
        kResamplerParamFilterType,
        kResamplerParamFilterClamp,
        kResamplerParamFilterBlackOutside,
        kSplineWarpParamBBox,
    };
    for (const char* name : names) {
        EXPECT_TRUE(bool(node->getKnobByName(name))) << name;
    }

    KnobDoublePtr amount = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kSplineWarpParamAmount));
    ASSERT_TRUE(bool(amount));
    EXPECT_EQ(1., amount->getValue());
    EXPECT_EQ("Amount", amount->getLabel());
    KnobDoublePtr curveResolution = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kSplineWarpParamCurveResolution));
    ASSERT_TRUE(bool(curveResolution));
    EXPECT_EQ("Curve Resolution", curveResolution->getLabel());
    KnobDoublePtr fieldResolution = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kSplineWarpParamFieldResolution));
    ASSERT_TRUE(bool(fieldResolution));
    EXPECT_EQ("Field Resolution", fieldResolution->getLabel());
    KnobBoolPtr blackOutside = std::dynamic_pointer_cast<KnobBool>(node->getKnobByName(kResamplerParamFilterBlackOutside));
    ASSERT_TRUE(bool(blackOutside));
    EXPECT_TRUE(blackOutside->getValue());
    KnobChoicePtr bbox = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kSplineWarpParamBBox));
    ASSERT_TRUE(bool(bbox));
    EXPECT_EQ(0, bbox->getValue());
    EXPECT_EQ("BBox", bbox->getLabel());

    KnobChoicePtr solver = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kSplineWarpParamSolver));
    ASSERT_TRUE(bool(solver));
    const std::vector<ChoiceOption> solvers = solver->getEntries_mt_safe();
    ASSERT_EQ(2u, solvers.size());
    EXPECT_EQ(kSplineWarpSolverTps, solvers[0].id);
    EXPECT_EQ(kSplineWarpSolverMesh, solvers[1].id);
    EXPECT_EQ("Mesh", solvers[1].label);
    EXPECT_EQ(0, solver->getValue());
    KnobDoublePtr smoothness = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kSplineWarpParamSmoothness));
    ASSERT_TRUE(bool(smoothness));
    EXPECT_FALSE(smoothness->getIsSecret());
    ASSERT_TRUE(setKnobValues(node, kSplineWarpParamSolver, { 1. }));
    EXPECT_TRUE(smoothness->getIsSecret());
    ASSERT_TRUE(setKnobValues(node, kSplineWarpParamSolver, { 0. }));
    EXPECT_FALSE(smoothness->getIsSecret());
}

TEST_F(NativeSplineWarpTest, TranslationLinkMatchesTransform)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr warp = makeSplineWarp(source);
    NodePtr transform = makeTranslate(source, 3.5, -2.25);
    ASSERT_TRUE(source && warp && transform);
    ASSERT_TRUE(bool(linkTranslation(contextOf(warp), 20., 15., 16., 3.5, -2.25)));
    EXPECT_FALSE(isIdentityOnSource(warp));

    const RectI window(0, 0, kParitySourceWidth, kParitySourceHeight);
    RenderedPlane warped, moved;
    ASSERT_TRUE(renderRGBA(warp, window, &warped));
    ASSERT_TRUE(renderRGBA(transform, window, &moved));
    EXPECT_LE(maxDiff(warped, moved, RectI(8, 8, 56, 40)), kWarpTolerance);
}

TEST_F(NativeSplineWarpTest, MeshMatchesTpsOnAnAffineLink)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr tps = makeSplineWarp(source);
    NodePtr mesh = makeSplineWarp(source);
    ASSERT_TRUE(source && tps && mesh);
    // A similarity: every sample pair lies on one affine map, which both solvers reproduce inside the quad.
    for (const NodePtr& warp : { tps, mesh }) {
        SplineWarpContextPtr ctx = contextOf(warp);
        ASSERT_TRUE(bool(ctx));
        BezierPtr from = makeQuad(ctx, 16., 12., 28.);
        BezierPtr to = makeQuad(ctx, 16., 12., 28., 1.5, -1., 1.1);
        ASSERT_TRUE(from && to);
        ASSERT_TRUE(bool(ctx->linkWithAlignment(from, to, 0., false)));
        ASSERT_TRUE(setKnobValues(warp, kSplineWarpParamFieldResolution, { 1. }));
    }
    ASSERT_TRUE(setKnobValues(mesh, kSplineWarpParamSolver, { 1. }));
    EXPECT_FALSE(isIdentityOnSource(mesh));

    const RectI window(0, 0, kParitySourceWidth, kParitySourceHeight);
    RenderedPlane fromTps, fromMesh;
    ASSERT_TRUE(renderRGBA(tps, window, &fromTps));
    ASSERT_TRUE(renderRGBA(mesh, window, &fromMesh));
    // Well inside the destination quad, so no field cell reaches past the mesh's hull.
    EXPECT_LE(maxDiff(fromTps, fromMesh, RectI(22, 16, 42, 32)), kWarpTolerance);
    EXPECT_GT(maxDiff(fromTps, fromMesh, window), 0.) << "outside the quad the solvers differ";
}

TEST_F(NativeSplineWarpTest, InvertGivesTheOppositeShift)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr warp = makeSplineWarp(source);
    NodePtr transform = makeTranslate(source, -3.5, 2.25);
    ASSERT_TRUE(source && warp && transform);
    ASSERT_TRUE(bool(linkTranslation(contextOf(warp), 20., 15., 16., 3.5, -2.25)));
    ASSERT_TRUE(setKnobValues(warp, kSplineWarpParamInvert, { 1. }));

    const RectI window(0, 0, kParitySourceWidth, kParitySourceHeight);
    RenderedPlane warped, moved;
    ASSERT_TRUE(renderRGBA(warp, window, &warped));
    ASSERT_TRUE(renderRGBA(transform, window, &moved));
    EXPECT_LE(maxDiff(warped, moved, RectI(8, 8, 56, 40)), kWarpTolerance);
}

TEST_F(NativeSplineWarpTest, IdentityCases)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr warp = makeSplineWarp(source);
    ASSERT_TRUE(source && warp);
    SplineWarpContextPtr ctx = contextOf(warp);
    ASSERT_TRUE(bool(ctx));

    EXPECT_TRUE(isIdentityOnSource(warp)) << "no link";

    SplineWarpLinkPtr link = linkTranslation(ctx, 20., 15., 16., 3., 2.);
    ASSERT_TRUE(bool(link));
    EXPECT_FALSE(isIdentityOnSource(warp)) << "one link";

    ASSERT_TRUE(setKnobValues(warp, kSplineWarpParamAmount, { 0. }));
    EXPECT_TRUE(isIdentityOnSource(warp)) << "global amount 0";
    const RectI window(0, 0, kParitySourceWidth, kParitySourceHeight);
    RenderedPlane warped, original;
    ASSERT_TRUE(renderRGBA(warp, window, &warped));
    ASSERT_TRUE(renderRGBA(source, window, &original));
    EXPECT_EQ(0., maxDiff(warped, original, window)) << "global amount 0";

    ASSERT_TRUE(setKnobValues(warp, kSplineWarpParamAmount, { 1. }));
    ctx->setLinkAmount(link, 0., kTime);
    EXPECT_TRUE(isIdentityOnSource(warp)) << "link amount 0";
    ctx->setLinkAmount(link, 1., kTime);
    EXPECT_FALSE(isIdentityOnSource(warp)) << "link amount 1";

    // A boundary holding none of the link's samples keeps everything still.
    BezierPtr boundary = makeRectangle(ctx, 44., 4., 60., 12.);
    ASSERT_TRUE(bool(boundary));
    ctx->setBoundary(boundary, true);
    EXPECT_TRUE(isIdentityOnSource(warp)) << "boundary away from the link";
}

TEST_F(NativeSplineWarpTest, BoundaryKeepsTheOutsideBitExact)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr warp = makeSplineWarp(source);
    ASSERT_TRUE(source && warp);
    SplineWarpContextPtr ctx = contextOf(warp);
    ASSERT_TRUE(bool(ctx));
    BezierPtr boundary = makeRectangle(ctx, 16., 12., 48., 36.);
    ASSERT_TRUE(bool(boundary));
    ctx->setBoundary(boundary, true);
    ASSERT_TRUE(bool(linkTranslation(ctx, 27., 21., 6., 2., 1.5)));
    EXPECT_FALSE(isIdentityOnSource(warp));

    const RectI window(0, 0, kParitySourceWidth, kParitySourceHeight);
    RenderedPlane warped, original;
    ASSERT_TRUE(renderRGBA(warp, window, &warped));
    ASSERT_TRUE(renderRGBA(source, window, &original));

    std::size_t outsideWrong = 0;
    double insideDiff = 0.;
    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            const double cx = x + 0.5;
            const double cy = y + 0.5;
            const bool outside = (cx < 15.) || (cx > 49.) || (cy < 11.) || (cy > 37.);
            const float* a = pixelAt(warped, x, y);
            const float* b = pixelAt(original, x, y);
            for (int c = 0; c < 4; ++c) {
                if (outside && (a[c] != b[c])) {
                    if (outsideWrong == 0) {
                        ADD_FAILURE() << "outside the boundary at (" << x << ", " << y << ") channel " << c << ": " << a[c] << " expected " << b[c];
                    }
                    ++outsideWrong;
                } else if (!outside) {
                    insideDiff = std::max(insideDiff, (double)std::abs(a[c] - b[c]));
                }
            }
        }
    }
    EXPECT_EQ(0u, outsideWrong);
    EXPECT_GT(insideDiff, 1e-3) << "the link moves nothing inside the boundary";
}

TEST_F(NativeSplineWarpTest, RoDFollowsTheBBox)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr warp = makeSplineWarp(source);
    ASSERT_TRUE(source && warp);
    ASSERT_TRUE(bool(linkTranslation(contextOf(warp), 20., 15., 16., 10., 5.)));

    const RectD sourceRoD = rodOf(source);
    ASSERT_FALSE(sourceRoD.isNull());
    expectRectNear(sourceRoD, rodOf(warp), "bbox source");

    ASSERT_TRUE(setKnobValue(warp, kSplineWarpParamBBox, kSplineWarpBBoxUnion));
    RectD expected = sourceRoD;
    expected.merge(sourceRoD.x1 + 10., sourceRoD.y1 + 5., sourceRoD.x2 + 10., sourceRoD.y2 + 5.);
    expectRectNear(expected, rodOf(warp), "bbox union");

    ASSERT_TRUE(setKnobValues(warp, kSplineWarpParamAmount, { 0. }));
    expectRectNear(sourceRoD, rodOf(warp), "bbox union, amount 0");
}

// Rendering a window from a Source cropped to the window's region of interest gives what the
// whole Source gives.
TEST_F(NativeSplineWarpTest, RoICoversWhatTheWindowReads)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr crop = makeNode(PLUGINID_NATRON_CROP, PLUGIN_MAJOR_NATRON_CROP, source);
    NodePtr full = makeSplineWarp(source);
    NodePtr cropped = makeSplineWarp(crop);
    ASSERT_TRUE(source && crop && full && cropped);

    // A scale and a shift: a thin-plate warp that stretches the middle.
    const std::function<bool(const NodePtr&)> addWarp = [](const NodePtr& node) {
        SplineWarpContextPtr ctx = contextOf(node);
        if (!ctx) {
            return false;
        }
        BezierPtr from = makeQuad(ctx, 24., 18., 12.);
        BezierPtr to = makeQuad(ctx, 24., 18., 12., 3., -2., 1.4);
        BezierPtr pin = makeQuad(ctx, 4., 4., 6.);
        BezierPtr pinTo = makeQuad(ctx, 4., 4., 6.);

        return from && to && pin && pinTo && ctx->linkWithAlignment(from, to, 0., false) && ctx->linkWithAlignment(pin, pinTo, 0., false);
    };
    ASSERT_TRUE(addWarp(full));
    ASSERT_TRUE(addWarp(cropped));

    const RectI window(26, 18, 38, 30);
    EffectInstancePtr effect = full->getEffectInstance();
    RoIMap rois;
    const RectD renderWindow(window.x1, window.y1, window.x2, window.y2);
    effect->getRegionsOfInterest_public(kTime, RenderScale::identity, rodOf(full), renderWindow, ViewIdx(0), &rois);
    RoIMap::const_iterator found = rois.find(source->getEffectInstance());
    ASSERT_TRUE(found != rois.end());
    const RectD roi = found->second;
    const RectD sourceRoD = rodOf(source);
    EXPECT_GE(roi.x1, sourceRoD.x1);
    EXPECT_GE(roi.y1, sourceRoD.y1);
    EXPECT_LE(roi.x2, sourceRoD.x2);
    EXPECT_LE(roi.y2, sourceRoD.y2);
    EXPECT_LT((roi.x2 - roi.x1) * (roi.y2 - roi.y1), (sourceRoD.x2 - sourceRoD.x1) * (sourceRoD.y2 - sourceRoD.y1)) << "the window reads the whole Source";

    const double x1 = std::floor(roi.x1);
    const double y1 = std::floor(roi.y1);
    ASSERT_TRUE(setKnobValues(crop, kNativeGeneratorParamBottomLeft, { x1, y1 }));
    ASSERT_TRUE(setKnobValues(crop, kNativeGeneratorParamSize, { std::ceil(roi.x2) - x1, std::ceil(roi.y2) - y1 }));
    ASSERT_TRUE(setKnobValues(crop, kCropParamSoftness, { 0. }));
    ASSERT_TRUE(setKnobValues(crop, kCropParamBlackOutside, { 0. }));

    const RectI all(0, 0, kParitySourceWidth, kParitySourceHeight);
    RenderedPlane whole, part;
    ASSERT_TRUE(renderRGBA(full, all, &whole));
    ASSERT_TRUE(renderRGBA(cropped, window, &part));
    EXPECT_LE(maxDiff(whole, part, window), 1e-6);
}

// A Transform upstream is applied in the warp's own resample, not resampled twice.
TEST_F(NativeSplineWarpTest, ConcatenatesAPrecedingTransform)
{
    resetProject();
    ASSERT_TRUE(appPTR->getCurrentSettings()->isTransformConcatenationEnabled());
    NodePtr source = makeSource();
    NodePtr transform = makeNode(PLUGINID_NATRON_TRANSFORM, PLUGIN_MAJOR_NATRON_TRANSFORM, source);
    ASSERT_TRUE(source && transform);
    ASSERT_TRUE(setKnobValues(transform, kTransformNodeParamCenter, { 32., 24. }));
    ASSERT_TRUE(setKnobValues(transform, kTransformNodeParamRotate, { 30. }));
    ASSERT_TRUE(setKnobValues(transform, kTransformNodeParamScale, { 0.5, 0.5 }));
    NodePtr dot = makeNode(PLUGINID_NATRON_DOT, -1, transform);
    ASSERT_TRUE(bool(dot));
    NodePtr warp = makeSplineWarp(transform);
    NodePtr twice = makeSplineWarp(dot);
    ASSERT_TRUE(warp && twice);
    ASSERT_TRUE(bool(linkTranslation(contextOf(warp), 26., 18., 10., 2.5, 1.5)));
    ASSERT_TRUE(bool(linkTranslation(contextOf(twice), 26., 18., 10., 2.5, 1.5)));

    InputMatrixMap transforms;
    warp->getEffectInstance()->tryConcatenateTransforms(kTime, false, ViewIdx(0), RenderScale::identity, &transforms);
    ASSERT_EQ(1u, transforms.count(kSplineWarpInputSource));
    ASSERT_TRUE(bool(transforms[kSplineWarpInputSource].cat));
    InputMatrixMap notConcatenated;
    twice->getEffectInstance()->tryConcatenateTransforms(kTime, false, ViewIdx(0), RenderScale::identity, &notConcatenated);
    EXPECT_EQ(0u, notConcatenated.count(kSplineWarpInputSource));

    // One resample through the Transform's matrix after the warp's translation.
    TransformNode* transformEffect = dynamic_cast<TransformNode*>(transform->getEffectInstance().get());
    ASSERT_TRUE(transformEffect != NULL);
    TransformMath::TransformParams p;
    transformEffect->getTransformParams(kTime, ViewIdx(0), &p);
    const Mat3 m = TransformMath::inverseTransformCanonical(p, 1., false) * TransformMath::translation(-2.5, -1.5);
    NodePtr single = makeNode(PLUGINID_NATRON_CORNERPIN, PLUGIN_MAJOR_NATRON_CORNERPIN, source);
    ASSERT_TRUE(bool(single));
    const char* const enables[4] = { kCornerPinNodeParamEnable1, kCornerPinNodeParamEnable2, kCornerPinNodeParamEnable3, kCornerPinNodeParamEnable4 };
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(setKnobValues(single, enables[i], { 0. }));
    }
    ASSERT_TRUE(setKnobValues(single, kCornerPinNodeParamExtraMatrix, std::vector<double>(m.m, m.m + 9)));

    const RectI window(0, 0, kParitySourceWidth, kParitySourceHeight);
    RenderedPlane concatenated, reference, doubled;
    ASSERT_TRUE(renderRGBA(warp, window, &concatenated));
    ASSERT_TRUE(renderRGBA(single, window, &reference));
    ASSERT_TRUE(renderRGBA(twice, window, &doubled));
    // Well inside the scaled Source, away from its edges.
    const RectI inside(28, 21, 38, 29);
    EXPECT_LE(maxDiff(concatenated, reference, inside), kWarpTolerance);
    EXPECT_GT(maxDiff(doubled, reference, inside), 1e-3) << "a double resample should soften the image";
}
