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
#include <bitset>
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
#include "SplineWarpTestCurves.h"
#include "WarpChainTestEffects.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Bezier.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/InverseWarpChain.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/EdgeModeKnob.h"
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
#include "Engine/Transform.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

using TransformMath::Mat3;
using namespace SplineWarpTestCurves;

namespace {

// A warp that is exactly a translation against a Transform of the same offset: both resample
// once, through different code paths.
const double kWarpTolerance = 1e-4;

bool
isNative(const NodePtr& node)
{
    return node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
}

bool
renderRGBA(const NodePtr& node,
           const RectI& window,
           RenderedPlane* plane,
           double time = kTime)
{
    std::list<ImageLayerDesc> layers;
    layers.push_back(ImageLayerDesc::getRGBAComponents());
    std::vector<RenderedPlane> planes;
    std::string error;
    const bool ok = renderNodePlanesDirect(node, time, ViewIdx(0), 0, window, layers, &planes, &error);
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

typedef std::function<void(std::size_t n, double* sx, double* sy, double* J)> PointMap;

// source resampled once, unclamped with filter under edgeMode, at the pixel centres of window
// sent through map with the Jacobian it gives.
RenderedPlane
sampleThrough(const PointMap& map,
              const RenderedPlane& source,
              const RectI& window,
              Resampler::FilterEnum filter,
              EdgeMode edgeMode)
{
    RenderedPlane out;
    out.window = window;
    out.channels = source.channels;
    const int width = window.width();
    out.pixels.assign((std::size_t)width * window.height() * 4, 0.f);
    const Resampler::SourceImage image(source.pixels.data(), source.window, 4);
    std::vector<double> x(width), y(width), J((std::size_t)width * 4);
    std::vector<float> sx(width), sy(width), jacobian((std::size_t)width * 4);
    for (int row = window.y1; row < window.y2; ++row) {
        for (int i = 0; i < width; ++i) {
            x[i] = window.x1 + i + 0.5;
            y[i] = row + 0.5;
            J[i * 4] = 1.;
            J[i * 4 + 1] = 0.;
            J[i * 4 + 2] = 0.;
            J[i * 4 + 3] = 1.;
        }
        map((std::size_t)width, x.data(), y.data(), J.data());
        for (int i = 0; i < width; ++i) {
            sx[i] = (float)x[i];
            sy[i] = (float)y[i];
        }
        for (std::size_t k = 0; k < J.size(); ++k) {
            jacobian[k] = (float)J[k];
        }
        Resampler::resampleRowFromCoords(image, sx.data(), sy.data(), jacobian.data(), width, Resampler::FilterSpec(filter), false, edgeMode,
                                         Resampler::eWrapNone, Resampler::eWrapNone, &out.pixels[(std::size_t)(row - window.y1) * width * 4], 4);
    }

    return out;
}

// The inverse of a Transform node's matrix at kTime, in canonical coordinates.
Mat3
inverseOfTransform(const NodePtr& node)
{
    TransformNode* transform = dynamic_cast<TransformNode*>(node->getEffectInstance().get());
    EXPECT_TRUE(transform != NULL);
    if (!transform) {
        return Mat3::identity();
    }
    TransformMath::TransformParams p;
    transform->getTransformParams(kTime, ViewIdx(0), &p);

    return TransformMath::inverseTransformCanonical(p, 1., false);
}

RectD
canonicalOf(const RectI& r)
{
    return RectD(r.x1, r.y1, r.x2, r.y2);
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
        // These tests compare against warps that move the whole image, which the format
        // boundary would pin.
        if (node && (std::string(id) == PLUGINID_NATRON_SPLINEWARP)) {
            EXPECT_TRUE(setKnobValues(node, kSplineWarpParamBoundaryBBox, { 0. }));
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
        kResamplerParamLobes,
        kResamplerParamBicubicB,
        kResamplerParamBicubicC,
        kResamplerParamFilterClamp,
        kEdgeModeParam,
        kSplineWarpParamBBox,
        kSplineWarpParamLayerAmount,
        kSplineWarpParamLinkAmount,
        kSplineWarpParamBoundaryBBox,
        kSplineWarpParamBoundaryCurveResolution,
        kSplineWarpParamOutputMask,
        kSplineWarpParamCropToFormat,
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
    EXPECT_FALSE(bool(node->getKnobByName(kResamplerParamFilterBlackOutside)));
    KnobChoicePtr bbox = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kSplineWarpParamBBox));
    ASSERT_TRUE(bool(bbox));
    EXPECT_EQ(0, bbox->getValue());
    EXPECT_EQ("BBox", bbox->getLabel());

    KnobDoublePtr layerAmount = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kSplineWarpParamLayerAmount));
    ASSERT_TRUE(bool(layerAmount));
    EXPECT_EQ("Layer Amount", layerAmount->getLabel());
    EXPECT_EQ(1., layerAmount->getValue());
    EXPECT_FALSE(layerAmount->getIsPersistent());
    EXPECT_FALSE(layerAmount->isEnabled(0));
    KnobDoublePtr linkAmount = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kSplineWarpParamLinkAmount));
    ASSERT_TRUE(bool(linkAmount));
    EXPECT_EQ("Link Amount", linkAmount->getLabel());
    EXPECT_EQ(1., linkAmount->getValue());
    EXPECT_FALSE(linkAmount->getIsPersistent());
    EXPECT_FALSE(linkAmount->isEnabled(0));
    KnobBoolPtr boundaryBBox = std::dynamic_pointer_cast<KnobBool>(node->getKnobByName(kSplineWarpParamBoundaryBBox));
    ASSERT_TRUE(bool(boundaryBBox));
    EXPECT_EQ("Boundary BBox", boundaryBBox->getLabel());
    // makeNode() turns it off for the render tests; its default is on.
    EXPECT_TRUE(boundaryBBox->getDefaultValue(0));
    KnobDoublePtr boundaryCurveResolution = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kSplineWarpParamBoundaryCurveResolution));
    ASSERT_TRUE(bool(boundaryCurveResolution));
    EXPECT_EQ("Boundary Curve Resolution", boundaryCurveResolution->getLabel());
    EXPECT_EQ(4., boundaryCurveResolution->getValue());
    EXPECT_FALSE(bool(node->getKnobByName("cookieCutInput")));
    KnobChannelSelectPtr outputMask = std::dynamic_pointer_cast<KnobChannelSelect>(node->getKnobByName(kSplineWarpParamOutputMask));
    ASSERT_TRUE(bool(outputMask));
    EXPECT_EQ("Output Mask", outputMask->getLabel());
    EXPECT_EQ(std::string(kSplineWarpOutputMaskLayer) + "." + kSplineWarpOutputMaskChannel, outputMask->get());
    EXPECT_FALSE(outputMask->isEnabled(0)) << "no cookie cutter";
    KnobBoolPtr cropToFormat = std::dynamic_pointer_cast<KnobBool>(node->getKnobByName(kSplineWarpParamCropToFormat));
    ASSERT_TRUE(bool(cropToFormat));
    EXPECT_EQ("Crop To Format", cropToFormat->getLabel());
    EXPECT_TRUE(cropToFormat->getValue());

    // The whole warp's controls on the first page, what transforms the selected curves and layers on
    // the Transform page, how it is rendered on the Render page.
    KnobPagePtr controls = amount->getTopLevelPage();
    ASSERT_TRUE(bool(controls));
    EXPECT_EQ("Warp", controls->getLabel());
    for (const char* name : { kSplineWarpParamInvert, kSplineWarpParamLayerAmount, kSplineWarpParamLinkAmount, kSplineWarpParamBoundaryBBox, kSplineWarpParamOutputMask, kSplineWarpParamCropToFormat }) {
        KnobIPtr knob = node->getKnobByName(name);
        ASSERT_TRUE(bool(knob)) << name;
        EXPECT_EQ(controls, knob->getTopLevelPage()) << name;
    }
    for (const char* name : { "translate", "extraMatrix" }) {
        KnobIPtr knob = node->getKnobByName(name);
        ASSERT_TRUE(bool(knob)) << name;
        KnobPagePtr page = knob->getTopLevelPage();
        ASSERT_TRUE(bool(page)) << name;
        EXPECT_EQ("Transform", page->getLabel()) << name;
    }
    EXPECT_FALSE(bool(node->getKnobByName("RotoTransformInteractive"))) << "the global preference sets how the transform handle updates";
    for (const char* name : { kSplineWarpParamSolver, kSplineWarpParamSmoothness, kSplineWarpParamCurveResolution,
                              kSplineWarpParamBoundaryCurveResolution, kSplineWarpParamFieldResolution, kResamplerParamFilterType,
                              kResamplerParamFilterClamp, kSplineWarpParamBBox,
                              kSplineWarpParamMotionBlur, kSplineWarpParamShutter, kResamplerParamShutterOffset,
                              kResamplerParamShutterCustomOffset }) {
        KnobIPtr knob = node->getKnobByName(name);
        ASSERT_TRUE(bool(knob)) << name;
        KnobPagePtr page = knob->getTopLevelPage();
        ASSERT_TRUE(bool(page)) << name;
        EXPECT_EQ("Render", page->getLabel()) << name;
    }

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

    // A cookie cutter enables Output Mask; removing the last disables them again.
    SplineWarpContextPtr ctx = contextOf(node);
    ASSERT_TRUE(bool(ctx));
    BezierPtr cutter = makeRectangle(ctx, 16., 12., 48., 36.);
    ASSERT_TRUE(bool(cutter));
    ctx->setCookieCutter(cutter, true);
    EXPECT_TRUE(outputMask->isEnabled(0));
    ctx->setCookieCutter(cutter, false);
    EXPECT_FALSE(outputMask->isEnabled(0));
}

TEST_F(NativeSplineWarpTest, TranslationLinkMatchesTransform)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr warp = makeSplineWarp(source);
    NodePtr transform = makeTranslate(source, 3.5, -2.25);
    ASSERT_TRUE(source && warp && transform);
    ASSERT_TRUE(bool(linkQuad(contextOf(warp), 20., 15., 16., 3.5, -2.25)));
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
    ASSERT_TRUE(bool(linkQuad(contextOf(warp), 20., 15., 16., 3.5, -2.25)));
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

    SplineWarpLinkPtr link = linkQuad(ctx, 20., 15., 16., 3., 2.);
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
    ASSERT_TRUE(bool(linkQuad(ctx, 27., 21., 6., 2., 1.5)));
    EXPECT_FALSE(isIdentityOnSource(warp));

    const RectI window(0, 0, kParitySourceWidth, kParitySourceHeight);
    RenderedPlane warped, original;
    ASSERT_TRUE(renderRGBA(warp, window, &warped));
    ASSERT_TRUE(renderRGBA(source, window, &original));

    // The warp eases out across the boundary's feather, so only pixels beyond it must stay put.
    const double feather = std::fabs(boundary->getFeatherDistance(kTime));
    std::size_t outsideWrong = 0;
    double insideDiff = 0.;
    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            const double cx = x + 0.5;
            const double cy = y + 0.5;
            const bool outside = (cx < 16. - feather) || (cx > 48. + feather) || (cy < 12. - feather) || (cy > 36. + feather);
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
    ASSERT_TRUE(bool(linkQuad(contextOf(warp), 20., 15., 16., 10., 5.)));

    const RectD sourceRoD = rodOf(source);
    ASSERT_FALSE(sourceRoD.isNull());
    expectRectNear(sourceRoD, rodOf(warp), "bbox source");

    ASSERT_TRUE(setKnobValue(warp, kSplineWarpParamBBox, kSplineWarpBBoxUnion));
    expectRectNear(sourceRoD, rodOf(warp), "bbox union, cropped to the format");
    ASSERT_TRUE(setKnobValues(warp, kSplineWarpParamCropToFormat, { 0. }));
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

    const RectI all(0, 0, kParitySourceWidth, kParitySourceHeight);
    RenderedPlane whole, part;
    ASSERT_TRUE(renderRGBA(full, all, &whole));
    ASSERT_TRUE(renderRGBA(cropped, window, &part));
    EXPECT_LE(maxDiff(whole, part, window), 1e-6);
}

// A Transform upstream is applied in the warp's own resample, not resampled twice.
TEST_F(NativeSplineWarpTest, TilesMatchTheWholeFrameUnderStrongMinification)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr warp = makeSplineWarp(source);
    ASSERT_TRUE(source && warp);
    ASSERT_TRUE(bool(linkQuad(contextOf(warp), 32., 24., 20., 0., 0., 0.2)));
    ASSERT_TRUE(setKnobValue(warp, kResamplerParamFilterType, std::string(Resampler::filterOptions()[(std::size_t)Resampler::eFilterLanczos3].id)));
    EXPECT_FALSE(isIdentityOnSource(warp));

    const RectI all(0, 0, kParitySourceWidth, kParitySourceHeight);
    const int midX = kParitySourceWidth / 2;
    RenderedPlane whole;
    ASSERT_TRUE(renderRGBA(warp, all, &whole));
    for (const RectI& tile : { RectI(0, 0, midX, kParitySourceHeight), RectI(midX, 0, kParitySourceWidth, kParitySourceHeight) }) {
        RenderedPlane part;
        ASSERT_TRUE(renderRGBA(warp, tile, &part));
        EXPECT_EQ(0., maxDiff(whole, part, tile)) << "tile from x " << tile.x1;
    }
}

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
    // A Dot passes a transform through, so a Crop, which cannot, makes the Transform resample itself.
    NodePtr crop = makeNode(PLUGINID_NATRON_CROP, PLUGIN_MAJOR_NATRON_CROP, transform);
    ASSERT_TRUE(bool(crop));
    ASSERT_TRUE(setKnobValues(crop, kNativeGeneratorParamBottomLeft, { 0., 0. }));
    ASSERT_TRUE(setKnobValues(crop, kNativeGeneratorParamSize, { (double)kParitySourceWidth, (double)kParitySourceHeight }));
    ASSERT_TRUE(setKnobValues(crop, kCropParamSoftness, { 0. }));
    NodePtr warp = makeSplineWarp(transform);
    NodePtr twice = makeSplineWarp(crop);
    ASSERT_TRUE(warp && twice);
    ASSERT_TRUE(bool(linkQuad(contextOf(warp), 26., 18., 10., 2.5, 1.5)));
    ASSERT_TRUE(bool(linkQuad(contextOf(twice), 26., 18., 10., 2.5, 1.5)));

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

TEST_F(NativeSplineWarpTest, OffersItsFieldToAChainConsumer)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr warp = makeSplineWarp(source);
    NodePtr consumer = makeNode(kTestPluginIDWarpChainConsumer, -1, warp);
    ASSERT_TRUE(source && warp && consumer);
    ASSERT_TRUE(setKnobValue(warp, kResamplerParamFilterType, std::string("bilinear")));
    ASSERT_TRUE(setKnobValues(warp, kResamplerParamFilterClamp, { 0. }));
    ASSERT_TRUE(bool(linkQuad(contextOf(warp), 22., 16., 16., 2.5, -1.5, 1.1)));

    InputMatrixMap transforms;
    consumer->getEffectInstance()->tryConcatenateTransforms(kTime, false, ViewIdx(0), RenderScale::identity, &transforms);
    ASSERT_EQ(1u, transforms.count(0));
    const InputMatrix& im = transforms[0];
    ASSERT_TRUE(bool(im.warpChain));
    EXPECT_FALSE(bool(im.cat));
    ASSERT_TRUE(bool(im.newInputEffect));
    EXPECT_EQ(warp->getEffectInstance(), im.newInputEffect);
    EXPECT_EQ(source->getEffectInstance(), im.newInputEffect->getInput(im.newInputNbToFetchFrom));
    const std::optional<EdgeMode> edgeMode = warp->getEffectInstance()->getConcatenationEdgeMode(kTime, ViewIdx(0));
    ASSERT_TRUE(edgeMode.has_value());

    const RectI window(0, 0, kParitySourceWidth, kParitySourceHeight);
    const InverseWarpChain chain = im.warpChain->prepare(canonicalOf(window), kWarpChainTestSupport);
    RenderedPlane image, warped;
    ASSERT_TRUE(renderRGBA(source, window, &image));
    ASSERT_TRUE(renderRGBA(warp, window, &warped));
    const RenderedPlane chained = sampleThrough([&](std::size_t n, double* sx, double* sy, double* J) { chain.mapRow(n, sx, sy, J); },
                                                image, window, Resampler::eFilterBilinear, *edgeMode);
    const RectI inside(8, 8, 56, 40);
    EXPECT_LE(maxDiff(warped, chained, inside), kWarpTolerance);
    EXPECT_GT(maxDiff(image, chained, inside), 1e-3) << "the field should move the Source";
}

// A warp that leaves the Source unchanged is stepped over, so the Transform below it still reaches
// the consumer as a matrix rather than as a chain.
TEST_F(NativeSplineWarpTest, IdentityWarpKeepsAMatrixChainAMatrix)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr transform = makeNode(PLUGINID_NATRON_TRANSFORM, PLUGIN_MAJOR_NATRON_TRANSFORM, source);
    ASSERT_TRUE(source && transform);
    ASSERT_TRUE(setKnobValues(transform, kTransformNodeParamTranslate, { 3., -2. }));
    ASSERT_TRUE(setKnobValues(transform, kTransformNodeParamRotate, { 12. }));
    NodePtr warp = makeSplineWarp(transform);
    NodePtr consumer = makeNode(kTestPluginIDWarpChainConsumer, -1, warp);
    ASSERT_TRUE(warp && consumer);
    // The moved Source crosses the format's edge, so cropping to it would change the image.
    ASSERT_TRUE(setKnobValues(warp, kSplineWarpParamCropToFormat, { 0. }));

    Transform::Matrix3x3 m;
    EffectInstancePtr inputToTransform;
    ASSERT_EQ(eStatusOK, transform->getEffectInstance()->getTransform_public(kTime, RenderScale::identity, false, ViewIdx(0), &inputToTransform, &m));

    const auto expectMatrix = [&](const char* what) {
        InputMatrixMap transforms;
        consumer->getEffectInstance()->tryConcatenateTransforms(kTime, false, ViewIdx(0), RenderScale::identity, &transforms);
        ASSERT_EQ(1u, transforms.count(0)) << what;
        const InputMatrix& im = transforms[0];
        EXPECT_FALSE(bool(im.warpChain)) << what;
        ASSERT_TRUE(bool(im.cat)) << what;
        EXPECT_EQ(m.a, im.cat->a) << what;
        EXPECT_EQ(m.b, im.cat->b) << what;
        EXPECT_EQ(m.c, im.cat->c) << what;
        EXPECT_EQ(m.d, im.cat->d) << what;
        EXPECT_EQ(m.e, im.cat->e) << what;
        EXPECT_EQ(m.f, im.cat->f) << what;
        EXPECT_EQ(transform->getEffectInstance(), im.newInputEffect) << what;
        EXPECT_EQ(source->getEffectInstance(), im.newInputEffect->getInput(im.newInputNbToFetchFrom)) << what;
    };

    ASSERT_TRUE(isIdentityOnSource(warp));
    expectMatrix("no link");

    ASSERT_TRUE(bool(linkQuad(contextOf(warp), 22., 16., 16., 2.5, -1.5)));
    ASSERT_TRUE(setKnobValues(warp, kOfxMixParamName, { 0. }));
    ASSERT_TRUE(isIdentityOnSource(warp));
    expectMatrix("mix 0");

    ASSERT_TRUE(setKnobValues(warp, kOfxMixParamName, { 1. }));
    InputMatrixMap transforms;
    consumer->getEffectInstance()->tryConcatenateTransforms(kTime, false, ViewIdx(0), RenderScale::identity, &transforms);
    ASSERT_EQ(1u, transforms.count(0));
    EXPECT_TRUE(bool(transforms[0].warpChain)) << "a warp that moves the Source is a map";
}

// A consumer sampling through the field alone would drop the mask, the mix and the sub-frames.
TEST_F(NativeSplineWarpTest, MaskMixOrMotionBlurStopsItOfferingItsField)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr maskSource = makeSource();
    NodePtr warp = makeSplineWarp(source);
    ASSERT_TRUE(source && maskSource && warp);
    ASSERT_TRUE(bool(linkQuad(contextOf(warp), 22., 16., 16., 2.5, -1.5)));
    EffectInstancePtr effect = warp->getEffectInstance();
    EXPECT_TRUE(effect->getCanInverseWarp());
    int inputNb = -1;
    EXPECT_TRUE(effect->canOfferInverseWarp(kTime, ViewIdx(0), false, &inputNb));
    EXPECT_EQ(kSplineWarpInputSource, inputNb);
    EXPECT_TRUE(effect->acceptsInverseWarpChain(kSplineWarpInputSource, kTime, ViewIdx(0)));

    ASSERT_TRUE(setKnobValues(warp, kOfxMixParamName, { 0.5 }));
    EXPECT_FALSE(effect->canOfferInverseWarp(kTime, ViewIdx(0), false, &inputNb)) << "mix";
    ASSERT_TRUE(setKnobValues(warp, kOfxMixParamName, { 1. }));
    EXPECT_TRUE(effect->canOfferInverseWarp(kTime, ViewIdx(0), false, &inputNb));

    ASSERT_TRUE(setKnobValues(warp, kSplineWarpParamMotionBlur, { 1. }));
    ASSERT_TRUE(setKnobValues(warp, kSplineWarpParamShutter, { 0.5 }));
    EXPECT_FALSE(effect->canOfferInverseWarp(kTime, ViewIdx(0), false, &inputNb)) << "sub-frames";
    EXPECT_FALSE(effect->acceptsInverseWarpChain(kSplineWarpInputSource, kTime, ViewIdx(0))) << "sub-frames";
    ASSERT_TRUE(setKnobValues(warp, kSplineWarpParamMotionBlur, { 0. }));
    EXPECT_TRUE(effect->canOfferInverseWarp(kTime, ViewIdx(0), false, &inputNb));

    connectNodes(maskSource, warp, kSplineWarpInputMask, true);
    ASSERT_TRUE(setKnobValues(warp, "enableMask_" + warp->getInputLabel(kSplineWarpInputMask), { 1. }));
    EXPECT_FALSE(effect->canOfferInverseWarp(kTime, ViewIdx(0), false, &inputNb)) << "mask";
}

// The test provider never renders, so the warp renders only by sampling the Source through it.
TEST_F(NativeSplineWarpTest, SamplesAChainedProviderOnce)
{
    resetProject();
    // Around the test map's centre, which it pulls every point towards.
    const int x0 = 224;
    const int y0 = 232;
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    setParitySourceOrigin(source, x0, y0);
    NodePtr provider = makeNode(kTestPluginIDWarpChainProvider, -1, source);
    NodePtr warp = makeSplineWarp(provider);
    ASSERT_TRUE(provider && warp);
    ASSERT_TRUE(setKnobValue(warp, kResamplerParamFilterType, std::string("bilinear")));
    ASSERT_TRUE(setKnobValues(warp, kResamplerParamFilterClamp, { 0. }));
    // The project format lies far from the Source.
    ASSERT_TRUE(setKnobValues(warp, kSplineWarpParamCropToFormat, { 0. }));
    ASSERT_TRUE(bool(linkQuad(contextOf(warp), x0 + 22., y0 + 16., 16., 2.5, -1.5, 1.1)));

    InputMatrixMap transforms;
    warp->getEffectInstance()->tryConcatenateTransforms(kTime, false, ViewIdx(0), RenderScale::identity, &transforms);
    ASSERT_EQ(1u, transforms.count(kSplineWarpInputSource));
    const InputMatrix& im = transforms[kSplineWarpInputSource];
    ASSERT_TRUE(bool(im.warpChain));
    ASSERT_TRUE(bool(im.newInputEffect));
    EXPECT_EQ(source->getEffectInstance(), im.newInputEffect->getInput(im.newInputNbToFetchFrom));
    const std::optional<EdgeMode> edgeMode = warp->getEffectInstance()->getConcatenationEdgeMode(kTime, ViewIdx(0));
    ASSERT_TRUE(edgeMode.has_value());

    const RectI window(x0, y0, x0 + kParitySourceWidth, y0 + kParitySourceHeight);
    RenderedPlane image, warped;
    ASSERT_TRUE(renderRGBA(source, window, &image));
    ASSERT_TRUE(renderRGBA(warp, window, &warped));
    const InverseWarpMapConstPtr field = warp->getEffectInstance()->getInverseWarpMap(kTime, ViewIdx(0), RenderScale::identity, false, canonicalOf(window));
    ASSERT_TRUE(bool(field));
    const WarpChainTestMap upstream(RenderScale::identity);
    const RenderedPlane reference = sampleThrough([&](std::size_t n, double* sx, double* sy, double* J) {
        field->mapRow(n, sx, sy, J);
        upstream.mapRow(n, sx, sy, J);
    },
                                                  image, window, Resampler::eFilterBilinear, *edgeMode);
    const RectI inside(x0 + 8, y0 + 8, x0 + kParitySourceWidth - 8, y0 + kParitySourceHeight - 8);
    EXPECT_LE(maxDiff(warped, reference, inside), kWarpTolerance);
}

TEST_F(NativeSplineWarpTest, TransformAfterTheWarpResamplesTheSourceOnce)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr first = makeNode(PLUGINID_NATRON_TRANSFORM, PLUGIN_MAJOR_NATRON_TRANSFORM, source);
    ASSERT_TRUE(source && first);
    ASSERT_TRUE(setKnobValues(first, kTransformNodeParamCenter, { 32., 24. }));
    ASSERT_TRUE(setKnobValues(first, kTransformNodeParamRotate, { 12. }));
    ASSERT_TRUE(setKnobValues(first, kTransformNodeParamTranslate, { 1.5, -0.75 }));
    NodePtr warp = makeSplineWarp(first);
    ASSERT_TRUE(bool(warp));
    ASSERT_TRUE(bool(linkQuad(contextOf(warp), 22., 16., 16., 2.5, -1.5, 1.1)));
    // A Crop, which offers nothing, makes the warp render and the Transform after it resample again.
    NodePtr crop = makeNode(PLUGINID_NATRON_CROP, PLUGIN_MAJOR_NATRON_CROP, warp);
    ASSERT_TRUE(bool(crop));
    ASSERT_TRUE(setKnobValues(crop, kNativeGeneratorParamBottomLeft, { 0., 0. }));
    ASSERT_TRUE(setKnobValues(crop, kNativeGeneratorParamSize, { (double)kParitySourceWidth, (double)kParitySourceHeight }));
    ASSERT_TRUE(setKnobValues(crop, kCropParamSoftness, { 0. }));
    NodePtr second = makeNode(PLUGINID_NATRON_TRANSFORM, PLUGIN_MAJOR_NATRON_TRANSFORM, warp);
    NodePtr twice = makeNode(PLUGINID_NATRON_TRANSFORM, PLUGIN_MAJOR_NATRON_TRANSFORM, crop);
    ASSERT_TRUE(second && twice);
    for (const NodePtr& node : { second, twice }) {
        ASSERT_TRUE(setKnobValues(node, kTransformNodeParamCenter, { 32., 24. }));
        ASSERT_TRUE(setKnobValues(node, kTransformNodeParamScale, { 0.9, 0.95 }));
        ASSERT_TRUE(setKnobValues(node, kTransformNodeParamTranslate, { -1., 0.5 }));
        ASSERT_TRUE(setKnobValue(node, kResamplerParamFilterType, std::string("bilinear")));
        ASSERT_TRUE(setKnobValues(node, kResamplerParamFilterClamp, { 0. }));
    }

    InputMatrixMap transforms;
    second->getEffectInstance()->tryConcatenateTransforms(kTime, false, ViewIdx(0), RenderScale::identity, &transforms);
    ASSERT_EQ(1u, transforms.count(0));
    const InputMatrix& im = transforms[0];
    ASSERT_TRUE(bool(im.warpChain));
    EXPECT_FALSE(bool(im.cat));
    ASSERT_TRUE(bool(im.newInputEffect));
    EXPECT_EQ(source->getEffectInstance(), im.newInputEffect->getInput(im.newInputNbToFetchFrom));
    const std::optional<EdgeMode> edgeMode = second->getEffectInstance()->getConcatenationEdgeMode(kTime, ViewIdx(0));
    ASSERT_TRUE(edgeMode.has_value());

    const RectI window(0, 0, kParitySourceWidth, kParitySourceHeight);
    RenderedPlane image, concatenated, doubled;
    ASSERT_TRUE(renderRGBA(source, window, &image));
    ASSERT_TRUE(renderRGBA(second, window, &concatenated));
    ASSERT_TRUE(renderRGBA(twice, window, &doubled));

    // The second Transform's inverse, the field, then the first Transform's inverse.
    const Mat3 secondInverse = inverseOfTransform(second);
    const Mat3 firstInverse = inverseOfTransform(first);
    const InverseWarpMapConstPtr field = warp->getEffectInstance()->getInverseWarpMap(kTime, ViewIdx(0), RenderScale::identity, false, RectD(-16., -16., 80., 64.));
    ASSERT_TRUE(bool(field));
    const RenderedPlane reference = sampleThrough([&](std::size_t n, double* sx, double* sy, double* J) {
        InverseWarp::mapRowThroughMatrix(secondInverse.m, n, sx, sy, J);
        field->mapRow(n, sx, sy, J);
        InverseWarp::mapRowThroughMatrix(firstInverse.m, n, sx, sy, J);
    },
                                                  image, window, Resampler::eFilterBilinear, *edgeMode);
    const RectI inside(10, 10, 54, 38);
    EXPECT_LE(maxDiff(concatenated, reference, inside), kWarpTolerance);
    EXPECT_GT(maxDiff(doubled, reference, inside), 1e-3) << "a double resample should soften the image";
}

// Each frame's render maps through the field of that frame, never through one prepared for another.
TEST_F(NativeSplineWarpTest, AnimatedWarpUpstreamOfATransformFollowsTheFrame)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr warp = makeSplineWarp(source);
    ASSERT_TRUE(source && warp);
    SplineWarpContextPtr ctx = contextOf(warp);
    ASSERT_TRUE(bool(ctx));
    SplineWarpLinkPtr link = linkQuad(ctx, 22., 16., 16., 2.5, -1.5, 1.1);
    ASSERT_TRUE(bool(link));
    const double frames[2] = { kTime, kTime + 1. };
    ctx->setLinkAmountAtTime(link, frames[0], 1.);
    ctx->setLinkAmountAtTime(link, frames[1], 0.3);
    NodePtr transform = makeNode(PLUGINID_NATRON_TRANSFORM, PLUGIN_MAJOR_NATRON_TRANSFORM, warp);
    ASSERT_TRUE(bool(transform));
    ASSERT_TRUE(setKnobValues(transform, kTransformNodeParamCenter, { 32., 24. }));
    ASSERT_TRUE(setKnobValues(transform, kTransformNodeParamScale, { 0.9, 0.95 }));
    ASSERT_TRUE(setKnobValues(transform, kTransformNodeParamTranslate, { -1., 0.5 }));
    ASSERT_TRUE(setKnobValue(transform, kResamplerParamFilterType, std::string("bilinear")));
    ASSERT_TRUE(setKnobValues(transform, kResamplerParamFilterClamp, { 0. }));
    const std::optional<EdgeMode> edgeMode = transform->getEffectInstance()->getConcatenationEdgeMode(kTime, ViewIdx(0));
    ASSERT_TRUE(edgeMode.has_value());
    const Mat3 transformInverse = inverseOfTransform(transform);

    const RectI window(0, 0, kParitySourceWidth, kParitySourceHeight);
    const RectI inside(10, 10, 54, 38);
    RenderedPlane rendered[2];
    for (int f = 0; f < 2; ++f) {
        InputMatrixMap transforms;
        transform->getEffectInstance()->tryConcatenateTransforms(frames[f], false, ViewIdx(0), RenderScale::identity, &transforms);
        ASSERT_EQ(1u, transforms.count(0));
        ASSERT_TRUE(bool(transforms[0].warpChain)) << "frame " << frames[f];

        RenderedPlane image;
        ASSERT_TRUE(renderRGBA(source, window, &image, frames[f]));
        ASSERT_TRUE(renderRGBA(transform, window, &rendered[f], frames[f]));
        const InverseWarpMapConstPtr field = warp->getEffectInstance()->getInverseWarpMap(frames[f], ViewIdx(0), RenderScale::identity, false, RectD(-16., -16., 80., 64.));
        ASSERT_TRUE(bool(field));
        const RenderedPlane reference = sampleThrough([&](std::size_t n, double* sx, double* sy, double* J) {
            InverseWarp::mapRowThroughMatrix(transformInverse.m, n, sx, sy, J);
            field->mapRow(n, sx, sy, J);
        },
                                                      image, window, Resampler::eFilterBilinear, *edgeMode);
        EXPECT_LE(maxDiff(rendered[f], reference, inside), kWarpTolerance) << "frame " << frames[f];
    }
    EXPECT_GT(maxDiff(rendered[0], rendered[1], inside), 1e-3) << "the warp should move between the frames";
}

// The field moves the whole Source left, so it has values beyond the warp's region of definition,
// which a consumer reading the rendered warp there never sees.
TEST_F(NativeSplineWarpTest, ChainedWarpIsTransparentOutsideItsRegionOfDefinition)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr warp = makeSplineWarp(source);
    ASSERT_TRUE(source && warp);
    ASSERT_TRUE(setKnobValue(warp, kSplineWarpParamBBox, kSplineWarpBBoxSource));
    ASSERT_TRUE(setKnobValues(warp, kSplineWarpParamCropToFormat, { 0. }));
    ASSERT_TRUE(setKnobValue(warp, kEdgeModeParam, std::string("black")));
    ASSERT_TRUE(bool(linkQuad(contextOf(warp), 22., 16., 16., -8., 0.)));
    NodePtr crop = makeNode(PLUGINID_NATRON_CROP, PLUGIN_MAJOR_NATRON_CROP, warp);
    ASSERT_TRUE(bool(crop));
    ASSERT_TRUE(setKnobValues(crop, kNativeGeneratorParamBottomLeft, { 0., 0. }));
    ASSERT_TRUE(setKnobValues(crop, kNativeGeneratorParamSize, { (double)kParitySourceWidth, (double)kParitySourceHeight }));
    ASSERT_TRUE(setKnobValues(crop, kCropParamSoftness, { 0. }));
    NodePtr chained = makeTranslate(warp, 12., 0.);
    NodePtr unchained = makeTranslate(crop, 12., 0.);
    ASSERT_TRUE(chained && unchained);
    for (const NodePtr& node : { chained, unchained }) {
        ASSERT_TRUE(setKnobValue(node, kResamplerParamFilterType, std::string("bilinear")));
        ASSERT_TRUE(setKnobValues(node, kResamplerParamFilterClamp, { 0. }));
        ASSERT_TRUE(setKnobValue(node, kEdgeModeParam, std::string("black")));
    }
    const RectD rod = rodOf(warp);
    expectRectNear(RectD(0., 0., kParitySourceWidth, kParitySourceHeight), rod, "warp");

    InputMatrixMap transforms;
    chained->getEffectInstance()->tryConcatenateTransforms(kTime, false, ViewIdx(0), RenderScale::identity, &transforms);
    ASSERT_EQ(1u, transforms.count(0));
    ASSERT_TRUE(bool(transforms[0].warpChain));

    const RectI window(0, 0, kParitySourceWidth, kParitySourceHeight);
    RenderedPlane image, viaChain, viaRender;
    ASSERT_TRUE(renderRGBA(source, window, &image));
    ASSERT_TRUE(renderRGBA(chained, window, &viaChain));
    ASSERT_TRUE(renderRGBA(unchained, window, &viaRender));
    // Output pixels reading the warp left of x = -1, beyond any bilinear tap of its first column.
    const RectI outside(0, 0, 10, kParitySourceHeight);
    RenderedPlane black;
    black.window = window;
    black.channels = image.channels;
    black.pixels.assign(image.pixels.size(), 0.f);
    EXPECT_GT(maxDiff(image, black, RectI(0, 0, 8, kParitySourceHeight)), 1e-3) << "the field should pull Source values outside the warp's window";
    EXPECT_EQ(0., maxDiff(viaRender, black, outside));
    EXPECT_EQ(0., maxDiff(viaChain, black, outside));
}

// An unchained consumer under a stretch edge holds the warp's edge pixel beyond its region of
// definition, so the chain must clamp there rather than read nothing. A translate's output never
// reaches past its input's region of definition, which the render clips to, so the consumers rotate:
// the corners of the window lie in the rotated region's bounding box but outside the rectangle.
TEST_F(NativeSplineWarpTest, ChainedWarpHoldsItsEdgeBeyondItsRegionOfDefinitionUnderStretch)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr warp = makeSplineWarp(source);
    ASSERT_TRUE(source && warp);
    ASSERT_TRUE(setKnobValue(warp, kSplineWarpParamBBox, kSplineWarpBBoxSource));
    ASSERT_TRUE(setKnobValues(warp, kSplineWarpParamCropToFormat, { 0. }));
    ASSERT_TRUE(setKnobValue(warp, kEdgeModeParam, std::string("stretch")));
    ASSERT_TRUE(bool(linkQuad(contextOf(warp), 22., 16., 16., -8., 0.)));
    NodePtr crop = makeNode(PLUGINID_NATRON_CROP, PLUGIN_MAJOR_NATRON_CROP, warp);
    ASSERT_TRUE(bool(crop));
    ASSERT_TRUE(setKnobValues(crop, kNativeGeneratorParamBottomLeft, { 0., 0. }));
    ASSERT_TRUE(setKnobValues(crop, kNativeGeneratorParamSize, { (double)kParitySourceWidth, (double)kParitySourceHeight }));
    ASSERT_TRUE(setKnobValues(crop, kCropParamSoftness, { 0. }));
    NodePtr chained = makeNode(PLUGINID_NATRON_TRANSFORM, PLUGIN_MAJOR_NATRON_TRANSFORM, warp);
    NodePtr unchained = makeNode(PLUGINID_NATRON_TRANSFORM, PLUGIN_MAJOR_NATRON_TRANSFORM, crop);
    ASSERT_TRUE(chained && unchained);
    for (const NodePtr& node : { chained, unchained }) {
        ASSERT_TRUE(setKnobValues(node, kTransformNodeParamCenter, { 32., 24. }));
        ASSERT_TRUE(setKnobValues(node, kTransformNodeParamRotate, { 30. }));
        ASSERT_TRUE(setKnobValue(node, kResamplerParamFilterType, std::string("bilinear")));
        ASSERT_TRUE(setKnobValues(node, kResamplerParamFilterClamp, { 0. }));
        ASSERT_TRUE(setKnobValue(node, kEdgeModeParam, std::string("stretch")));
    }

    InputMatrixMap transforms;
    chained->getEffectInstance()->tryConcatenateTransforms(kTime, false, ViewIdx(0), RenderScale::identity, &transforms);
    ASSERT_EQ(1u, transforms.count(0));
    ASSERT_TRUE(bool(transforms[0].warpChain));

    const RectI window(0, 0, kParitySourceWidth, kParitySourceHeight);
    RenderedPlane viaChain, viaRender;
    ASSERT_TRUE(renderRGBA(chained, window, &viaChain));
    ASSERT_TRUE(renderRGBA(unchained, window, &viaRender));
    RenderedPlane black;
    black.window = window;
    black.channels = viaRender.channels;
    black.pixels.assign(viaRender.pixels.size(), 0.f);
    // Its pixels read at least two source pixels past the warp's edge, beyond any bilinear tap.
    const RectI outside(0, 0, 4, 4);
    EXPECT_GT(maxDiff(viaRender, black, outside), 1e-3) << "the held edge pixel should not read transparent";
    EXPECT_LE(maxDiff(viaChain, viaRender, outside), kWarpTolerance);
}

// A Transform that averages several matrices takes no chain, so the warp renders as before.
TEST_F(NativeSplineWarpTest, MotionBlurredTransformAfterTheWarpTakesNoChain)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr warp = makeSplineWarp(source);
    ASSERT_TRUE(source && warp);
    ASSERT_TRUE(bool(linkQuad(contextOf(warp), 22., 16., 16., 2.5, -1.5, 1.1)));
    NodePtr crop = makeNode(PLUGINID_NATRON_CROP, PLUGIN_MAJOR_NATRON_CROP, warp);
    ASSERT_TRUE(bool(crop));
    ASSERT_TRUE(setKnobValues(crop, kNativeGeneratorParamBottomLeft, { 0., 0. }));
    ASSERT_TRUE(setKnobValues(crop, kNativeGeneratorParamSize, { (double)kParitySourceWidth, (double)kParitySourceHeight }));
    ASSERT_TRUE(setKnobValues(crop, kCropParamSoftness, { 0. }));
    NodePtr blurred = makeNode(PLUGINID_NATRON_TRANSFORM, PLUGIN_MAJOR_NATRON_TRANSFORM, warp);
    NodePtr separate = makeNode(PLUGINID_NATRON_TRANSFORM, PLUGIN_MAJOR_NATRON_TRANSFORM, crop);
    ASSERT_TRUE(blurred && separate);
    for (const NodePtr& node : { blurred, separate }) {
        ASSERT_TRUE(setKnobValues(node, kTransformNodeParamTranslate, { 1.5, -0.5 }));
        ASSERT_TRUE(setKnobValues(node, kTransformNodeParamMotionBlur, { 1. }));
        ASSERT_TRUE(setKnobValues(node, kTransformNodeParamShutter, { 0.5 }));
    }

    EXPECT_FALSE(blurred->getEffectInstance()->acceptsInverseWarpChain(0, kTime, ViewIdx(0)));
    InputMatrixMap transforms;
    blurred->getEffectInstance()->tryConcatenateTransforms(kTime, false, ViewIdx(0), RenderScale::identity, &transforms);
    EXPECT_EQ(0u, transforms.count(0));

    const RectI window(0, 0, kParitySourceWidth, kParitySourceHeight);
    RenderedPlane fromWarp, fromCrop;
    ASSERT_TRUE(renderRGBA(blurred, window, &fromWarp));
    ASSERT_TRUE(renderRGBA(separate, window, &fromCrop));
    EXPECT_EQ(0., maxDiff(fromWarp, fromCrop, RectI(8, 8, 56, 40)));
}

namespace {

ImageLayerDesc
outputMaskLayer()
{
    return ImageLayerDesc(kSplineWarpOutputMaskLayer, kSplineWarpOutputMaskLayer, "", std::vector<std::string>(1, kSplineWarpOutputMaskChannel));
}

bool
renderLayer(const NodePtr& node,
            const RectI& window,
            const ImageLayerDesc& layer,
            RenderedPlane* plane)
{
    std::vector<RenderedPlane> planes;
    std::string error;
    const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, std::list<ImageLayerDesc>(1, layer), &planes, &error);
    EXPECT_TRUE(ok) << node->getScriptName() << ": " << error;
    if (!ok || (planes.size() != 1) || (planes[0].channels.size() != (std::size_t)layer.getNumComponents())) {
        return false;
    }
    *plane = planes[0];

    return true;
}

// Whether node produces layerID, among the planes it outputs.
bool
producesLayer(const NodePtr& node,
              const std::string& layerID)
{
    EffectInstancePtr effect = node->getEffectInstance();
    EffectInstance::ComponentsNeededMap comps;
    std::list<ImageLayerDesc> passThroughLayers;
    double passThroughTime = 0.;
    int passThroughView = 0;
    std::bitset<4> processChannels;
    EffectInstance::ProcessChannelsPerPlaneMap processChannelsPerPlane;
    int passThroughInput = -1;

    effect->getComponentsNeededAndProduced_public(effect->getRenderHash(), kTime, ViewIdx(0), &comps, &passThroughLayers, &passThroughTime, &passThroughView, &processChannels, &processChannelsPerPlane, &passThroughInput);
    for (const ImageLayerDesc& layer : comps[-1]) {
        if (layer.getLayerID() == layerID) {
            return true;
        }
    }

    return false;
}

} // namespace

// The mask follows the warp, so a mask of the linked pair lands on the curve the warp pulls to:
// the destination, and with Invert the source. Rendering both on one node also proves the cached
// mask does not survive the switch.
TEST_F(NativeSplineWarpTest, LinkedCookieCutterMaskLandsOnTheCurvePulledTo)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr warp = makeSplineWarp(source);
    ASSERT_TRUE(source && warp);
    SplineWarpContextPtr ctx = contextOf(warp);
    ASSERT_TRUE(bool(ctx));
    BezierPtr from = makeRectangle(ctx, 8., 12., 24., 28.);
    BezierPtr to = makeRectangle(ctx, 36., 12., 52., 28.);
    ASSERT_TRUE(from && to);
    ASSERT_TRUE(bool(ctx->linkWithAlignment(from, to, 0., false)));
    ctx->setCookieCutter(from, true);

    const RectI window(0, 0, kParitySourceWidth, kParitySourceHeight);
    const auto maskAt = [&window](const RenderedPlane& mask, int x, int y) {
        return mask.pixels[(std::size_t)(y - window.y1) * window.width() + (x - window.x1)];
    };
    RenderedPlane straight, inverted;
    ASSERT_TRUE(renderLayer(warp, window, outputMaskLayer(), &straight));
    ASSERT_TRUE(setKnobValues(warp, kSplineWarpParamInvert, { 1. }));
    ASSERT_TRUE(renderLayer(warp, window, outputMaskLayer(), &inverted));

    const double tolerance = 0.05;
    EXPECT_NEAR(1., maskAt(straight, 44, 20), tolerance);
    EXPECT_NEAR(0., maskAt(straight, 16, 20), tolerance);
    EXPECT_NEAR(1., maskAt(inverted, 16, 20), tolerance);
    EXPECT_NEAR(0., maskAt(inverted, 44, 20), tolerance);
}

// Output Mask writes the cutter's mask and the cutter never cuts the Source: with no link the warp
// moves nothing, so both are exact.
TEST_F(NativeSplineWarpTest, CookieCutterWritesTheMaskAndLeavesTheSource)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr warp = makeSplineWarp(source);
    ASSERT_TRUE(source && warp);
    SplineWarpContextPtr ctx = contextOf(warp);
    ASSERT_TRUE(bool(ctx));
    BezierPtr cutter = makeRectangle(ctx, 16., 12., 48., 36.);
    ASSERT_TRUE(bool(cutter));
    EXPECT_FALSE(producesLayer(warp, kSplineWarpOutputMaskLayer)) << "no cookie cutter";

    ctx->setCookieCutter(cutter, true);
    EXPECT_TRUE(producesLayer(warp, kSplineWarpOutputMaskLayer));
    EXPECT_FALSE(isIdentityOnSource(warp));

    const RectI window(0, 0, kParitySourceWidth, kParitySourceHeight);
    RenderedPlane original, warped, mask;
    ASSERT_TRUE(renderRGBA(source, window, &original));
    ASSERT_TRUE(renderRGBA(warp, window, &warped));
    ASSERT_TRUE(renderLayer(warp, window, outputMaskLayer(), &mask));
    ASSERT_EQ(1u, mask.channels.size());
    std::size_t wrong = 0;
    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            const float m = mask.pixels[(std::size_t)(y - window.y1) * window.width() + (x - window.x1)];
            if ((x >= 18) && (x < 46) && (y >= 14) && (y < 34)) {
                EXPECT_EQ(1.f, m) << x << ", " << y << " is well inside the cutter";
            } else if ((x < 14) || (x >= 50) || (y < 10) || (y >= 38)) {
                EXPECT_EQ(0.f, m) << x << ", " << y << " is well outside the cutter";
            }
            const float* src = pixelAt(original, x, y);
            const float* got = pixelAt(warped, x, y);
            for (int c = 0; c < 4; ++c) {
                if (got[c] != src[c]) {
                    ++wrong;
                }
            }
        }
    }
    EXPECT_EQ(0u, wrong) << "the output is not the Source";

    // A deactivated cutter is ignored but keeps its role.
    cutter->setGloballyActivated(false, false);
    EXPECT_TRUE(ctx->isCookieCutter(cutter));
    EXPECT_FALSE(producesLayer(warp, kSplineWarpOutputMaskLayer));
    EXPECT_TRUE(isIdentityOnSource(warp));
    cutter->setGloballyActivated(true, false);
    EXPECT_TRUE(producesLayer(warp, kSplineWarpOutputMaskLayer));

    // Output Mask set to none writes nothing.
    KnobChannelSelectPtr outputMask = std::dynamic_pointer_cast<KnobChannelSelect>(warp->getKnobByName(kSplineWarpParamOutputMask));
    ASSERT_TRUE(bool(outputMask));
    outputMask->setNone();
    EXPECT_FALSE(producesLayer(warp, kSplineWarpOutputMaskLayer));
}

// Output Mask names a layer of the output: the Source doesn't need it, the knob lists it, and
// rendering it leaves no message on the node.
TEST_F(NativeSplineWarpTest, OutputMaskCreatesItsLayerOnAnRgbSource)
{
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    setParitySourceComponents(source, "rgb");
    NodePtr warp = makeSplineWarp(source);
    ASSERT_TRUE(bool(warp));
    SplineWarpContextPtr ctx = contextOf(warp);
    ASSERT_TRUE(bool(ctx));
    BezierPtr cutter = makeRectangle(ctx, 16., 12., 48., 36.);
    ASSERT_TRUE(bool(cutter));
    ctx->setCookieCutter(cutter, true);

    KnobChannelSelectPtr outputMask = std::dynamic_pointer_cast<KnobChannelSelect>(warp->getKnobByName(kSplineWarpParamOutputMask));
    ASSERT_TRUE(bool(outputMask));
    outputMask->set(std::string(kSplineWarpOutputMaskLayer) + "." + kSplineWarpOutputMaskChannel);
    EXPECT_TRUE(warp->isTargetLayerKnob(outputMask));

    std::list<ImageLayerDesc> listed;
    warp->listLayersForKnob(outputMask, kTime, ViewIdx(0), &listed);
    ImageLayerDesc resolved;
    int channel = -1;
    EXPECT_TRUE(outputMask->resolve(listed, &resolved, &channel)) << "the knob lists the layer it writes";
    EXPECT_EQ(std::string(kSplineWarpOutputMaskLayer), resolved.getLayerID());

    EXPECT_FALSE(producesLayer(source, kSplineWarpOutputMaskLayer));
    EXPECT_TRUE(producesLayer(warp, kSplineWarpOutputMaskLayer));
    std::string message;
    EXPECT_TRUE(warp->checkSelectedChannelsPresent(kTime, ViewIdx(0), &message)) << message;

    const RectI window(0, 0, kParitySourceWidth, kParitySourceHeight);
    RenderedPlane mask;
    ASSERT_TRUE(renderLayer(warp, window, outputMaskLayer(), &mask));
    ASSERT_EQ(1u, mask.channels.size());
    EXPECT_EQ(1.f, mask.pixels[(std::size_t)(24 - window.y1) * window.width() + (32 - window.x1)]) << "inside the cutter";
    EXPECT_EQ(0.f, mask.pixels[(std::size_t)(4 - window.y1) * window.width() + (4 - window.x1)]) << "outside the cutter";
    EXPECT_FALSE(warp->hasPersistentMessage());
}

// Crop To Format samples the Source only inside its format and keeps the output there; off, the
// warp pulls in the Source's pixels outside the format.
TEST_F(NativeSplineWarpTest, CropToFormat)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr warp = makeSplineWarp(source);
    ASSERT_TRUE(source && warp);
    setParitySourceOrigin(source, -8, -8);
    setBlackOutside(source, true);
    ASSERT_TRUE(setKnobValues(warp, kResamplerParamFilterType, { (double)Resampler::eFilterImpulse }));
    ASSERT_TRUE(bool(linkQuad(contextOf(warp), 20., 15., 16., 3., 0.)));
    expectRectNear(RectD(-8., -8., 56., 40.), rodOf(source), "source");

    expectRectNear(RectD(0., 0., 56., 40.), rodOf(warp), "cropped");
    const RectI window(-8, -8, 56, 40);
    RenderedPlane original, cropped, uncropped;
    ASSERT_TRUE(renderRGBA(source, window, &original));
    ASSERT_TRUE(renderRGBA(warp, window, &cropped));
    for (int y = 4; y < 36; y += 5) {
        // Pixel 1 samples the Source's pixel -2, outside the format.
        const float* src = pixelAt(original, -2, y);
        ASSERT_GT(std::abs(src[0]) + std::abs(src[3]), 1e-3) << "the Source is black at -2, " << y;
        const float* got = pixelAt(cropped, 1, y);
        for (int c = 0; c < 4; ++c) {
            EXPECT_EQ(0.f, got[c]) << "1, " << y << " channel " << c;
        }
    }

    ASSERT_TRUE(setKnobValues(warp, kSplineWarpParamCropToFormat, { 0. }));
    expectRectNear(RectD(-8., -8., 56., 40.), rodOf(warp), "not cropped");
    ASSERT_TRUE(renderRGBA(warp, window, &uncropped));
    for (int y = 4; y < 36; y += 5) {
        const float* src = pixelAt(original, -2, y);
        const float* got = pixelAt(uncropped, 1, y);
        for (int c = 0; c < 4; ++c) {
            EXPECT_NEAR(src[c], got[c], kWarpTolerance) << "1, " << y << " channel " << c;
        }
    }
}
