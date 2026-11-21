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
#include <cstring>
#include <functional>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "Native/FilterKnobRenderChecks.h"
#include "NativeParity.h"
#include "NativePluginIdentity.h"
#include "RegionIndependenceSupport.h"
#include "RenderBothWays.h"
#include "SplineWarpTestCurves.h"
#include "WarpChainSampling.h"
#include "WarpChainTestEffects.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/InverseWarpChain.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Transform/IDistort.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarp.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

bool
isNative(const NodePtr& node)
{
    return node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
}

// An empty value sets the selector to None.
bool
setChannelSelect(const NodePtr& node,
                 const std::string& name,
                 const std::string& value)
{
    KnobChannelSelect* select = node ? dynamic_cast<KnobChannelSelect*>(node->getKnobByName(name).get()) : NULL;

    EXPECT_TRUE(select != NULL) << name;
    if (!select) {
        return false;
    }
    if (value.empty()) {
        select->setNone();
    } else {
        select->set(value);
    }

    return true;
}

bool
renderLayer(const NodePtr& node,
            unsigned mipmapLevel,
            const RectI& window,
            const ImageLayerDesc& layer,
            RenderedPlane* plane)
{
    std::list<ImageLayerDesc> layers;
    layers.push_back(layer);
    std::vector<RenderedPlane> planes;
    std::string error;
    const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), mipmapLevel, window, layers, &planes, &error);
    EXPECT_TRUE(ok) << node->getScriptName() << ": " << error;
    if (!ok || (planes.size() != 1) || (planes[0].channels.size() != (std::size_t)layer.getNumComponents())) {
        return false;
    }
    *plane = planes[0];

    return true;
}

bool
renderRGBA(const NodePtr& node,
           unsigned mipmapLevel,
           const RectI& window,
           RenderedPlane* plane)
{
    return renderLayer(node, mipmapLevel, window, ImageLayerDesc::getRGBAComponents(), plane);
}

const float*
pixelAt(const RenderedPlane& plane,
        int x,
        int y)
{
    const std::size_t n = plane.channels.size();

    return &plane.pixels[((std::size_t)(y - plane.window.y1) * plane.window.width() + (x - plane.window.x1)) * n];
}

// Counts the pixels of `actual` over `window` that differ from `expected(x, y)` beyond `tolerance`,
// reporting the first. `expected` returns false for a pixel not to check.
std::size_t
countMismatches(const RenderedPlane& actual,
                const RectI& window,
                const std::function<bool(int, int, float*)>& expected,
                const ParityTolerance& tolerance,
                const std::string& what)
{
    std::size_t wrong = 0;
    float want[4];

    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            if (!expected(x, y, want)) {
                continue;
            }
            const float* got = pixelAt(actual, x, y);
            for (int c = 0; c < 4; ++c) {
                if (!tolerance.accepts(want[c], got[c])) {
                    if (wrong == 0) {
                        ADD_FAILURE() << what << ": first mismatch at (" << x << ", " << y << ") channel " << c << ": " << got[c] << " expected " << want[c];
                    }
                    ++wrong;
                    break;
                }
            }
        }
    }

    return wrong;
}

} // namespace

class NativeIDistortTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerRegionTestSourcesOnce();
        BaseTest::SetUp();
    }

    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeIDistort64x48", 1.));
    }

    NodePtr makeSource()
    {
        NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
        EXPECT_TRUE(bool(source));

        return source;
    }

    NodePtr makeConstantUV(double u,
                           double v)
    {
        NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), PLUGIN_MAJOR_NATRON_CONSTANT);
        EXPECT_TRUE(bool(constant));
        if (constant) {
            EXPECT_TRUE(setKnobValues(constant, kConstantParamColor, { u, v, 0., 1. }));
        }

        return constant;
    }

    NodePtr makeIDistort(const NodePtr& source,
                         const NodePtr& uv,
                         const std::string& filter)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_IDISTORT), PLUGIN_MAJOR_NATRON_IDISTORT);
        EXPECT_TRUE(isNative(node));
        if (!node) {
            return node;
        }
        if (source) {
            connectNodes(source, node, kIDistortInputSource, true);
        }
        if (uv) {
            connectNodes(uv, node, kIDistortInputUV, true);
        }
        EXPECT_TRUE(setKnobValue(node, kResamplerParamFilterType, filter));

        return node;
    }

    // A SplineWarp on input moving a quad by (2.5, -1.5) and scaling it by 1.1, filtered
    // bilinearly, with no format boundary pinning the image.
    NodePtr makeSplineWarp(const NodePtr& input)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_SPLINEWARP), PLUGIN_MAJOR_NATRON_SPLINEWARP);
        EXPECT_TRUE(isNative(node));
        if (!node) {
            return node;
        }
        if (input) {
            connectNodes(input, node, kSplineWarpInputSource, true);
        }
        EXPECT_TRUE(setKnobValues(node, kSplineWarpParamBoundaryBBox, { 0. }));
        EXPECT_TRUE(setKnobValue(node, kResamplerParamFilterType, std::string("bilinear")));
        EXPECT_TRUE(bool(SplineWarpTestCurves::linkQuad(SplineWarpTestCurves::contextOf(node), 22., 16., 16., 2.5, -1.5, 1.1)));

        return node;
    }

    NodePtr makeProvider(const NodePtr& input)
    {
        NodePtr node = createNode(QString::fromUtf8(kTestPluginIDWarpChainProvider));
        EXPECT_TRUE(node && providerOf(node));
        if (node && input) {
            connectNodes(input, node, 0, true);
        }

        return node;
    }

    static WarpChainTestProvider* providerOf(const NodePtr& node)
    {
        return node ? dynamic_cast<WarpChainTestProvider*>(node->getEffectInstance().get()) : NULL;
    }

    void expectIdentityOnSource(const NodePtr& source,
                                const NodePtr& node,
                                const std::string& what)
    {
        EffectInstancePtr effect = node->getEffectInstance();
        const RectI window = paritySourceWindow(source, kTime, 0);
        double inputTime = 0.;
        ViewIdx inputView;
        int inputNb = -1;
        EXPECT_TRUE(effect->isIdentity_public(true, effect->getRenderHash(), kTime, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, &inputNb)) << what;
        EXPECT_EQ(kIDistortInputSource, inputNb) << what;
    }

    // `node` renders the Source moved left by `shift` pixels at mipmapLevel wherever the shifted
    // pixel is inside the Source.
    void expectShift(const NodePtr& source,
                     const NodePtr& node,
                     int shift,
                     unsigned mipmapLevel,
                     const ParityTolerance& tolerance,
                     const std::string& what)
    {
        const RectI window = paritySourceWindow(source, kTime, mipmapLevel);
        ASSERT_FALSE(window.isNull()) << what;
        RenderedPlane before;
        RenderedPlane after;
        ASSERT_TRUE(renderRGBA(source, mipmapLevel, window, &before)) << what;
        ASSERT_TRUE(renderRGBA(node, mipmapLevel, window, &after)) << what;
        const std::size_t wrong = countMismatches(after, window, [&](int x, int y, float* want) {
            if ((x + shift < window.x1) || (x + shift >= window.x2)) {
                return false;
            }
            std::memcpy(want, pixelAt(before, x + shift, y), 4 * sizeof(float));

            return true; }, tolerance, what);
        EXPECT_EQ(0u, wrong) << what << ", mipmap " << mipmapLevel;
    }
};

TEST_F(NativeIDistortTest, RegisteredIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_IDISTORT, PLUGIN_MAJOR_NATRON_IDISTORT, "IDistort");
}

TEST_F(NativeIDistortTest, ZeroUVIsTheSource)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr uv = makeConstantUV(0., 0.);
    ASSERT_TRUE(source && uv);

    NodePtr impulse = makeIDistort(source, uv, "impulse");
    ASSERT_TRUE(bool(impulse));
    expectShift(source, impulse, 0, 0, ParityTolerance::exact(), "zero UV, impulse");

    NodePtr cubic = makeIDistort(source, uv, "cubic");
    ASSERT_TRUE(bool(cubic));
    expectShift(source, cubic, 0, 0, ParityTolerance::resampling(), "zero UV, cubic");
}

TEST_F(NativeIDistortTest, DisconnectedUVIsIdentityOnSource)
{
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    NodePtr node = makeIDistort(source, NodePtr(), "cubic");
    ASSERT_TRUE(bool(node));

    expectIdentityOnSource(source, node, "UV disconnected");
    expectShift(source, node, 0, 0, ParityTolerance::exact(), "UV disconnected");
}

TEST_F(NativeIDistortTest, ConstantUShiftsByTwoPixels)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr uv = makeConstantUV(2., 0.);
    ASSERT_TRUE(source && uv);

    NodePtr impulse = makeIDistort(source, uv, "impulse");
    ASSERT_TRUE(bool(impulse));
    expectShift(source, impulse, 2, 0, ParityTolerance::exact(), "U = +2, impulse");

    NodePtr cubic = makeIDistort(source, uv, "cubic");
    ASSERT_TRUE(bool(cubic));
    expectShift(source, cubic, 2, 0, ParityTolerance::resampling(), "U = +2, cubic");
}

TEST_F(NativeIDistortTest, OneSelectedChannelLeavesTheOtherAtZero)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr uv = makeConstantUV(2., 5.);
    ASSERT_TRUE(source && uv);

    NodePtr node = makeIDistort(source, uv, "impulse");
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setChannelSelect(node, kIDistortParamChannelV, std::string()));
    expectShift(source, node, 2, 0, ParityTolerance::exact(), "V set to None");
}

TEST_F(NativeIDistortTest, UVScaleAndOffset)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr uv = makeConstantUV(1., 0.);
    ASSERT_TRUE(source && uv);

    NodePtr scaled = makeIDistort(source, uv, "impulse");
    ASSERT_TRUE(bool(scaled));
    ASSERT_TRUE(setKnobValues(scaled, kIDistortParamUVScale, { 2., 1. }));
    expectShift(source, scaled, 2, 0, ParityTolerance::exact(), "U = 1, uvScale 2");

    NodePtr offset = makeIDistort(source, uv, "impulse");
    ASSERT_TRUE(bool(offset));
    ASSERT_TRUE(setKnobValues(offset, kIDistortParamUVOffset, { -3., 0. }));
    ASSERT_TRUE(setKnobValues(offset, kIDistortParamUVScale, { 0.5, 1. }));
    expectShift(source, offset, 2, 0, ParityTolerance::exact(), "U = 1, uvOffset -3, uvScale 0.5");

    NodePtr zero = makeIDistort(source, uv, "impulse");
    ASSERT_TRUE(bool(zero));
    ASSERT_TRUE(setKnobValues(zero, kIDistortParamUVScale, { 0., 0. }));
    expectIdentityOnSource(source, zero, "uvScale 0");
    expectShift(source, zero, 0, 0, ParityTolerance::exact(), "uvScale 0");
}

// The displacement is in full-resolution pixels: at mipmap 1 a U of 4 moves the image by 2, and
// the half-resolution render picks the full-resolution render's odd pixels.
TEST_F(NativeIDistortTest, RenderScaleMatchesTheDownscaledFullRender)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr uv = makeConstantUV(4., 2.);
    ASSERT_TRUE(source && uv);

    const char* const filters[] = { "impulse", "cubic" };
    for (const char* filter : filters) {
        const ParityTolerance tolerance = (std::string(filter) == "impulse") ? ParityTolerance::exact() : ParityTolerance::resampling();
        NodePtr node = makeIDistort(source, uv, filter);
        ASSERT_TRUE(bool(node));
        ASSERT_TRUE(setChannelSelect(node, kIDistortParamChannelV, std::string()));
        expectShift(source, node, 2, 1, tolerance, std::string("U = 4 at mipmap 1, ") + filter);

        const RectI full = paritySourceWindow(source, kTime, 0);
        const RectI half = paritySourceWindow(source, kTime, 1);
        RenderedPlane fullPlane;
        RenderedPlane halfPlane;
        ASSERT_TRUE(renderRGBA(node, 0, full, &fullPlane)) << filter;
        ASSERT_TRUE(renderRGBA(node, 1, half, &halfPlane)) << filter;
        const std::size_t wrong = countMismatches(halfPlane, half, [&](int x, int y, float* want) {
            const int fx = 2 * x + 1;
            const int fy = 2 * y + 1;
            // A sample clamped to the Source's edge lands on different pixels at the two scales.
            if ((fx + 4 >= full.x2) || (fy >= full.y2)) {
                return false;
            }
            std::memcpy(want, pixelAt(fullPlane, fx, fy), 4 * sizeof(float));

            return true; }, tolerance, std::string("mipmap 1 against mipmap 0, ") + filter);
        EXPECT_EQ(0u, wrong) << filter;
    }
}

// The spec layer holds a smooth field within [-0.25, 1.25] pixels, so every sample point is
// checked against Resampler::interpolate() there.
TEST_F(NativeIDistortTest, UVFromAnyLayer)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr uv = makeSource();
    ASSERT_TRUE(source && uv);
    setParitySourceExtraLayer(uv, true);

    NodePtr node = makeIDistort(source, uv, "impulse");
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setChannelSelect(node, kIDistortParamChannelU, std::string(kParitySourceExtraLayerID) + ".R"));
    ASSERT_TRUE(setChannelSelect(node, kIDistortParamChannelV, std::string(kParitySourceExtraLayerID) + ".G"));

    std::vector<int> poolSizes;
    poolSizes.push_back(1);
    poolSizes.push_back(4);
    for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
        const RectI window = paritySourceWindow(source, kTime, mipmapLevel);
        const double scale = (mipmapLevel == 0) ? 1. : 0.5;
        RenderedPlane before;
        RenderedPlane field;
        RenderedPlane after;
        ASSERT_TRUE(renderRGBA(source, mipmapLevel, window, &before));
        ASSERT_TRUE(renderLayer(uv, mipmapLevel, window, paritySourceExtraLayer(), &field));
        ASSERT_TRUE(renderRGBA(node, mipmapLevel, window, &after));

        const Resampler::SourceImage image(before.pixels.data(), before.window, 4);
        std::size_t moved = 0;
        const std::size_t wrong = countMismatches(after, window, [&](int x, int y, float* want) {
            const float* uvPix = pixelAt(field, x, y);
            const float sx = (float)((double)x + 0.5 + (double)uvPix[0] * scale);
            const float sy = (float)((double)y + 0.5 + (double)uvPix[1] * scale);
            (void)Resampler::interpolate(Resampler::eFilterImpulse, false, sx, sy, image, EdgeMode::Stretch, want);
            if (std::memcmp(want, pixelAt(before, x, y), 4 * sizeof(float)) != 0) {
                ++moved;
            }

            return true; }, ParityTolerance::exact(), "U/V from the spec layer");
        EXPECT_EQ(0u, wrong) << "mipmap " << mipmapLevel;
        EXPECT_GT(moved, 0u) << "mipmap " << mipmapLevel << ": the field moves nothing";

        std::vector<int> unplannedPulls;
        const RenderMismatch m = renderBothWaysDirect(node, kTime, ViewIdx(0), mipmapLevel, window, poolSizes, std::function<void()>(), 0.f, &unplannedPulls);
        EXPECT_FALSE(m.any) << "mipmap " << mipmapLevel << ": " << describe(m);
        EXPECT_EQ(poolSizes.size(), unplannedPulls.size());
        for (std::size_t p = 0; p < unplannedPulls.size(); ++p) {
            EXPECT_EQ(0, unplannedPulls[p]) << "mipmap " << mipmapLevel << ", pool " << poolSizes[p];
        }
    }
}

TEST_F(NativeIDistortTest, HonoursTheNewFiltersAndTheirParameters)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr uv = makeConstantUV(1.25, -0.5);
    ASSERT_TRUE(source && uv);
    NodePtr node = makeIDistort(source, uv, "cubic");
    ASSERT_TRUE(bool(node));

    const RectI window(0, 0, kParitySourceWidth, kParitySourceHeight);
    EXPECT_TRUE(filterChoiceChangesTheRender(node, window, kTime, Resampler::eFilterCubic, Resampler::eFilterLanczos3));
    EXPECT_TRUE(sincLobesChangeTheRender(node, window, kTime));
    EXPECT_TRUE(bicubicParametersChangeTheRender(node, window, kTime));
}

TEST_F(NativeIDistortTest, FilteredFieldIsIndependentOfTheWindow)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr uv = makeSource();
    ASSERT_TRUE(source && uv);
    setParitySourceExtraLayer(uv, true);

    NodePtr node = makeIDistort(source, uv, "cubic");
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setChannelSelect(node, kIDistortParamChannelU, std::string(kParitySourceExtraLayerID) + ".B"));
    ASSERT_TRUE(setChannelSelect(node, kIDistortParamChannelV, "rgba.R"));
    ASSERT_TRUE(setKnobValues(node, kIDistortParamUVScale, { 8., 4. }));

    std::vector<int> poolSizes;
    poolSizes.push_back(1);
    poolSizes.push_back(4);
    for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
        const RectI window = paritySourceWindow(source, kTime, mipmapLevel);
        std::vector<int> unplannedPulls;
        const RenderMismatch m = renderBothWaysDirect(node, kTime, ViewIdx(0), mipmapLevel, window, poolSizes, std::function<void()>(), 0.f, &unplannedPulls);
        EXPECT_FALSE(m.any) << "mipmap " << mipmapLevel << ": " << describe(m);
        for (std::size_t p = 0; p < unplannedPulls.size(); ++p) {
            EXPECT_EQ(0, unplannedPulls[p]) << "mipmap " << mipmapLevel << ", pool " << poolSizes[p];
        }
    }

    // The Jacobians at a tile's edge read the UV past it, so a tile matches the whole frame.
    const RectI full = paritySourceWindow(source, kTime, 0);
    const RectI tile(full.x1 + 10, full.y1 + 7, full.x1 + 30, full.y1 + 21);
    RenderedPlane whole;
    RenderedPlane part;
    ASSERT_TRUE(renderRGBA(node, 0, full, &whole));
    ASSERT_TRUE(renderRGBA(node, 0, tile, &part));
    const std::size_t wrong = countMismatches(part, tile, [&](int x, int y, float* want) {
        std::memcpy(want, pixelAt(whole, x, y), 4 * sizeof(float));

        return true; }, ParityTolerance::exact(), "tile against whole frame");
    EXPECT_EQ(0u, wrong);
}

TEST_F(NativeIDistortTest, MixBlendsWithTheSource)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr uv = makeConstantUV(2., 0.);
    ASSERT_TRUE(source && uv);

    NodePtr node = makeIDistort(source, uv, "impulse");
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kOfxMixParamName, { 0.5 }));

    const RectI window = paritySourceWindow(source, kTime, 0);
    RenderedPlane before;
    RenderedPlane after;
    ASSERT_TRUE(renderRGBA(source, 0, window, &before));
    ASSERT_TRUE(renderRGBA(node, 0, window, &after));
    const std::size_t wrong = countMismatches(after, window, [&](int x, int y, float* want) {
        if (x + 2 >= window.x2) {
            return false;
        }
        const float* moved = pixelAt(before, x + 2, y);
        const float* here = pixelAt(before, x, y);
        for (int c = 0; c < 4; ++c) {
            want[c] = moved[c] * 0.5f + 0.5f * here[c];
        }

        return true; }, ParityTolerance::arithmetic(), "mix 0.5");
    EXPECT_EQ(0u, wrong);
}

// The SplineWarp concatenates into the IDistort: its field is sampled through once, as each node's
// map applied by hand to the source in a single resample.
TEST_F(NativeIDistortTest, SamplesAnUpstreamSplineWarpOnce)
{
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    setParitySourceOrigin(source, 0, 0);
    NodePtr warp = makeSplineWarp(source);
    NodePtr uv = makeConstantUV(2., 1.);
    ASSERT_TRUE(warp && uv);
    NodePtr node = makeIDistort(warp, uv, "bilinear");
    ASSERT_TRUE(bool(node));
    EffectInstancePtr effect = node->getEffectInstance();

    InputMatrixMap transforms;
    effect->tryConcatenateTransforms(kTime, false, ViewIdx(0), RenderScale::identity, &transforms);
    ASSERT_EQ(1u, transforms.count(kIDistortInputSource));
    EXPECT_TRUE(bool(transforms[kIDistortInputSource].warpChain));
    const std::optional<EdgeMode> edgeMode = effect->getConcatenationEdgeMode(kTime, ViewIdx(0));
    ASSERT_TRUE(edgeMode.has_value());
    const InverseWarpMapConstPtr field = WarpChainSampling::offeredMap(warp, kTime, WarpChainSampling::rodOf(warp, kTime));
    ASSERT_TRUE(bool(field));

    const RectI window(0, 0, kParitySourceWidth, kParitySourceHeight);
    RenderedPlane image, distorted;
    ASSERT_TRUE(renderRGBA(source, 0, window, &image));
    ASSERT_TRUE(renderRGBA(node, 0, window, &distorted));
    const RenderedPlane reference = WarpChainSampling::sampleThrough([&](std::size_t n, double* sx, double* sy, double* J) {
        for (std::size_t i = 0; i < n; ++i) {
            sx[i] += 2.;
            sy[i] += 1.;
        }
        field->mapRow(n, sx, sy, J); }, image, window, Resampler::eFilterBilinear, *edgeMode);
    const RectI inside(8, 8, 56, 40);
    EXPECT_LE(WarpChainSampling::maxDiff(distorted, reference, inside), 1e-4);
    EXPECT_GT(WarpChainSampling::maxDiff(image, reference, inside), 1e-3) << "the warps should move the Source";
}

// The IDistort reads its Source whole; through the SplineWarp that is the field's pullback of the
// SplineWarp's region, asked of the Source directly.
TEST_F(NativeIDistortTest, RequestsTheSplineWarpPullBackOfItsSourceRegion)
{
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    setParitySourceOrigin(source, 0, 0);
    NodePtr warp = makeSplineWarp(source);
    NodePtr uv = makeConstantUV(2., 1.);
    ASSERT_TRUE(warp && uv);
    NodePtr node = makeIDistort(warp, uv, "bilinear");
    ASSERT_TRUE(bool(node));

    const RectD warpRoD = WarpChainSampling::rodOf(warp, kTime);
    const InverseWarpMapConstPtr field = WarpChainSampling::offeredMap(warp, kTime, warpRoD);
    ASSERT_TRUE(bool(field));
    const double support = node->getEffectInstance()->getInverseWarpChainFilterSupport(kIDistortInputSource, kTime, ViewIdx(0));
    const RectD expected = field->pullBack(warpRoD, support);

    const RectD window = WarpChainSampling::canonicalOf(RectI(0, 0, kParitySourceWidth, kParitySourceHeight));
    RectD region;
    ASSERT_TRUE(WarpChainSampling::requestedRegion(node, getApp()->getTimeLine().get(), kTime, window, source, &region));
    EXPECT_EQ(expected, region);
    RectD skipped;
    EXPECT_FALSE(WarpChainSampling::requestedRegion(node, getApp()->getTimeLine().get(), kTime, window, warp, &skipped)) << "the SplineWarp should not render";
}

// Every tile of a render samples through the one map the request pass built.
TEST_F(NativeIDistortTest, BuildsAProviderMapOncePerRender)
{
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    setParitySourceOrigin(source, 224, 232);
    NodePtr provider = makeProvider(source);
    NodePtr uv = makeConstantUV(2., 1.);
    ASSERT_TRUE(provider && uv);
    NodePtr node = makeIDistort(provider, uv, "bilinear");
    ASSERT_TRUE(bool(node));

    RenderedPlane distorted;
    ASSERT_TRUE(renderRGBA(node, 0, paritySourceWindow(source, kTime, 0), &distorted));
    const std::vector<RectD> windows = providerOf(provider)->getMapWindows();
    ASSERT_EQ(1u, windows.size());
    EXPECT_EQ(WarpChainSampling::rodOf(provider, kTime), windows[0]);
}

// A map over an infinite region could be neither built nor sampled: the render fails, saying why,
// before the provider is asked for one.
TEST_F(NativeIDistortTest, InfiniteSourceThroughAWarpFailsTheRender)
{
    resetProject();
    NodePtr source = createNode(QString::fromUtf8(kRegionTestSourceID));
    RegionTestSource* region = source ? dynamic_cast<RegionTestSource*>(source->getEffectInstance().get()) : NULL;
    ASSERT_TRUE(region != NULL);
    region->setFrame(RectI(0, 0, 16, 16), true);
    NodePtr provider = makeProvider(source);
    NodePtr uv = makeConstantUV(2., 1.);
    ASSERT_TRUE(provider && uv);
    NodePtr node = makeIDistort(provider, uv, "bilinear");
    ASSERT_TRUE(bool(node));

    std::vector<RenderedPlane> planes;
    std::string error;
    EXPECT_FALSE(renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, RectI(0, 0, 16, 16), std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &planes, &error));
    QString message;
    int type = 0;
    node->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeError, type);
    EXPECT_EQ(QString::fromUtf8(InverseWarpChain::getInfiniteWindowError().c_str()), message);
    EXPECT_TRUE(providerOf(provider)->getMapWindows().empty());
}
