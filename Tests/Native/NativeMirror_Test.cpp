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

#include <cstring>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "Native/NativeGpuParity.h"
#include "NativeParity.h"
#include "NativePluginIdentity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Transform/Crop.h"
#include "Engine/Nodes/Transform/Mirror.h"
#include "Engine/Nodes/Transform/Transform.h"
#include "Engine/ParallelRenderArgs.h"
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

RectI
pixelRoD(const NodePtr& node,
         unsigned mipmapLevel)
{
    EffectInstancePtr effect = node->getEffectInstance();
    RectD rod;
    const StatusEnum stat = effect->getRegionOfDefinition(effect->getRenderHash(), kTime, RenderScale::fromMipmapLevel(mipmapLevel), ViewIdx(0), &rod);
    EXPECT_NE(eStatusFailed, stat);

    return rod.toPixelEnclosing(mipmapLevel, 1.);
}

bool
renderRGBA(const NodePtr& node,
           unsigned mipmapLevel,
           const RectI& window,
           RenderedPlane* plane)
{
    std::list<ImageLayerDesc> layers;
    layers.push_back(ImageLayerDesc::getRGBAComponents());
    std::vector<RenderedPlane> planes;
    std::string error;
    const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), mipmapLevel, window, layers, &planes, &error);
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
    return &plane.pixels[((std::size_t)(y - plane.window.y1) * plane.window.width() + (x - plane.window.x1)) * 4];
}

bool
sameBits(const float* a,
         const float* b)
{
    return std::memcmp(a, b, 4 * sizeof(float)) == 0;
}

} // namespace

class NativeMirrorTest
    : public BaseTest {
protected:
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeMirror64x48", 1.));
    }

    NodePtr makeSource(int originX = 0,
                       int originY = 0)
    {
        NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
        EXPECT_TRUE(bool(source));
        if (source) {
            setParitySourceOrigin(source, originX, originY);
        }

        return source;
    }

    NodePtr makeMirror(const NodePtr& input,
                       bool flip,
                       bool flop)
    {
        NodePtr mirror = createNode(QString::fromUtf8(PLUGINID_NATRON_MIRROR), PLUGIN_MAJOR_NATRON_MIRROR);
        EXPECT_TRUE(isNative(mirror));
        if (!mirror) {
            return mirror;
        }
        connectNodes(input, mirror, 0, true);
        EXPECT_TRUE(setKnobValues(mirror, kMirrorParamFlip, { flip ? 1. : 0. }));
        EXPECT_TRUE(setKnobValues(mirror, kMirrorParamFlop, { flop ? 1. : 0. }));

        return mirror;
    }

    NodePtr makeTransform(const NodePtr& input,
                          double tx,
                          double ty,
                          double rotate,
                          double sx,
                          double sy)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_TRANSFORM), PLUGIN_MAJOR_NATRON_TRANSFORM);
        EXPECT_TRUE(isNative(node));
        if (!node) {
            return node;
        }
        connectNodes(input, node, 0, true);
        EXPECT_TRUE(setKnobValues(node, kTransformNodeParamCenter, { 32., 24. }));
        EXPECT_TRUE(setKnobValues(node, kTransformNodeParamTranslate, { tx, ty }));
        EXPECT_TRUE(setKnobValues(node, kTransformNodeParamRotate, { rotate }));
        EXPECT_TRUE(setKnobValues(node, kTransformNodeParamScale, { sx, sy }));

        return node;
    }

    // Mirror renders the reversal of `upstream` over its region of definition, bit for bit,
    // which is also the region of the mirror.
    void expectReversal(const NodePtr& upstream,
                        const NodePtr& mirror,
                        bool flip,
                        bool flop,
                        unsigned mipmapLevel,
                        const std::string& what)
    {
        const RectI window = pixelRoD(upstream, mipmapLevel);
        ASSERT_FALSE(window.isNull()) << what;
        EXPECT_EQ(window, pixelRoD(mirror, mipmapLevel)) << what << ": the region of definition is the source's";

        RenderedPlane before;
        RenderedPlane after;
        ASSERT_TRUE(renderRGBA(upstream, mipmapLevel, window, &before)) << what;
        ASSERT_TRUE(renderRGBA(mirror, mipmapLevel, window, &after)) << what;

        std::size_t wrong = 0;
        std::size_t moved = 0;
        for (int y = window.y1; y < window.y2; ++y) {
            for (int x = window.x1; x < window.x2; ++x) {
                const int sx = flop ? (window.x1 + window.x2 - 1 - x) : x;
                const int sy = flip ? (window.y1 + window.y2 - 1 - y) : y;
                if (!sameBits(pixelAt(after, x, y), pixelAt(before, sx, sy))) {
                    if (wrong == 0) {
                        ADD_FAILURE() << what << ", mipmap " << mipmapLevel << ": first mismatch at (" << x << ", " << y << ") expecting source (" << sx << ", " << sy << ")";
                    }
                    ++wrong;
                }
                if (!sameBits(pixelAt(before, sx, sy), pixelAt(before, x, y))) {
                    ++moved;
                }
            }
        }
        EXPECT_EQ(0u, wrong) << what << ", mipmap " << mipmapLevel;
        EXPECT_GT(moved, 0u) << what << ": the reversal changes nothing in this image";
    }

    void expectReversalAtBothScales(const NodePtr& upstream,
                                    const NodePtr& mirror,
                                    bool flip,
                                    bool flop,
                                    const std::string& what)
    {
        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            expectReversal(upstream, mirror, flip, flop, mipmapLevel, what);
        }
    }

    // The two nodes render the same pixels over `window` to within the resampling tolerance.
    void expectSameImage(const NodePtr& a,
                         const NodePtr& b,
                         const RectI& window,
                         const std::string& what)
    {
        RenderedPlane pa;
        RenderedPlane pb;
        ASSERT_TRUE(renderRGBA(a, 0, window, &pa)) << what;
        ASSERT_TRUE(renderRGBA(b, 0, window, &pb)) << what;
        ASSERT_EQ(pa.pixels.size(), pb.pixels.size()) << what;
        const ImageDiff diff = compareImages(pa.pixels, pb.pixels, ParityTolerance::resampling());
        EXPECT_EQ(0u, diff.mismatches) << what << ": first at float " << diff.firstMismatchIndex << ", worst " << diff.worstAbsolute;
    }

    // The node reads the source directly, its own matrix folded into the chain.
    void expectConcatenatesInto(const NodePtr& node,
                                const NodePtr& expectedInput,
                                const std::string& what)
    {
        EffectInstancePtr effect = node->getEffectInstance();
        EXPECT_TRUE(effect->getCanTransform()) << what;
        InputMatrixMap transforms;
        effect->tryConcatenateTransforms(kTime, false, ViewIdx(0), RenderScale::identity, &transforms);
        ASSERT_EQ(1u, transforms.count(0)) << what;
        ASSERT_TRUE(bool(transforms[0].newInputEffect)) << what;
        EXPECT_TRUE(bool(transforms[0].cat)) << what;
        // newInputEffect is the most upstream node of the chain; the image is fetched from its input.
        EXPECT_EQ(expectedInput->getEffectInstance(), transforms[0].newInputEffect->getInput(transforms[0].newInputNbToFetchFrom)) << what;
    }

    NodePtr makeCrop(const NodePtr& input,
                     double x,
                     double y,
                     double w,
                     double h)
    {
        NodePtr crop = createNode(QString::fromUtf8(PLUGINID_NATRON_CROP), PLUGIN_MAJOR_NATRON_CROP);
        EXPECT_TRUE(isNative(crop));
        if (crop) {
            connectNodes(input, crop, 0, true);
            EXPECT_TRUE(setKnobValues(crop, kNativeGeneratorParamBottomLeft, { x, y }));
            EXPECT_TRUE(setKnobValues(crop, kNativeGeneratorParamSize, { w, h }));
            EXPECT_TRUE(setKnobValues(crop, kCropParamSoftness, { 0. }));
        }

        return crop;
    }
};

TEST_F(NativeMirrorTest, Flip)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr mirror = makeMirror(source, true, false);
    ASSERT_TRUE(bool(mirror));
    expectReversalAtBothScales(source, mirror, true, false, "flip");
}

TEST_F(NativeMirrorTest, Flop)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr mirror = makeMirror(source, false, true);
    ASSERT_TRUE(bool(mirror));
    expectReversalAtBothScales(source, mirror, false, true, "flop");
}

TEST_F(NativeMirrorTest, FlipAndFlop)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr mirror = makeMirror(source, true, true);
    ASSERT_TRUE(bool(mirror));
    expectReversalAtBothScales(source, mirror, true, true, "flip and flop");
}

TEST_F(NativeMirrorTest, OddWidthAndHeightRegion)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr crop = makeCrop(source, 5., 3., 31., 17.);
    ASSERT_TRUE(bool(crop));
    const RectI cropRoD = pixelRoD(crop, 0);
    ASSERT_EQ(1, cropRoD.width() % 2) << "the region is not odd-sized";
    ASSERT_EQ(1, cropRoD.height() % 2) << "the region is not odd-sized";

    NodePtr mirror = makeMirror(crop, true, true);
    ASSERT_TRUE(bool(mirror));
    expectReversalAtBothScales(crop, mirror, true, true, "odd region, flip and flop");
}

TEST_F(NativeMirrorTest, OffsetRegions)
{
    resetProject();
    const int origins[][2] = { { 7, -5 }, { -13, 9 }, { 100, 31 } };
    for (const auto& origin : origins) {
        NodePtr source = makeSource(origin[0], origin[1]);
        const std::string what = "origin (" + std::to_string(origin[0]) + ", " + std::to_string(origin[1]) + ")";
        NodePtr mirror = makeMirror(source, true, true);
        ASSERT_TRUE(bool(mirror)) << what;
        expectReversalAtBothScales(source, mirror, true, true, what);
        EXPECT_EQ(origin[0], pixelRoD(mirror, 0).x1) << what;
        EXPECT_EQ(origin[1], pixelRoD(mirror, 0).y1) << what;
    }
}

TEST_F(NativeMirrorTest, OddAndOffsetRegion)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr crop = makeCrop(source, -3., 6., 29., 21.);
    ASSERT_TRUE(bool(crop));
    NodePtr mirror = makeMirror(crop, true, true);
    ASSERT_TRUE(bool(mirror));
    expectReversalAtBothScales(crop, mirror, true, true, "odd region away from the origin");
}

TEST_F(NativeMirrorTest, NeitherKnobIsIdentity)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr mirror = makeMirror(source, false, false);
    ASSERT_TRUE(bool(mirror));
    EffectInstancePtr effect = mirror->getEffectInstance();
    const RectI window = pixelRoD(source, 0);
    double inputTime = 0.;
    ViewIdx inputView(0);
    int inputNb = -1;

    EXPECT_TRUE(effect->isIdentity_public(false, effect->getRenderHash(), kTime, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, &inputNb));
    EXPECT_EQ(0, inputNb);

    ASSERT_TRUE(setKnobValues(mirror, kMirrorParamFlop, { 1. }));
    EXPECT_FALSE(effect->isIdentity_public(false, effect->getRenderHash(), kTime, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, &inputNb));
}

// Mirror followed by a Transform is one resample by the Transform, through the product of both
// matrices. A reflection about the centre composed with a Transform about that centre is the
// same Transform with a negated scale.
TEST_F(NativeMirrorTest, MirrorThenTransformIsOneResample)
{
    resetProject();
    NodePtr source = makeSource();
    const RectI window(-16, -16, 80, 64);

    for (int axis = 0; axis < 2; ++axis) {
        const bool flip = (axis == 0);
        const std::string what = flip ? "flip then transform" : "flop then transform";
        NodePtr mirror = makeMirror(source, flip, !flip);
        NodePtr chained = makeTransform(mirror, 5.3173, 3.2719, 15., 0.75, 0.6);
        ASSERT_TRUE(bool(mirror) && bool(chained)) << what;
        expectConcatenatesInto(chained, source, what);

        NodePtr reference = makeTransform(source, 5.3173, 3.2719, 15., flip ? 0.75 : -0.75, flip ? -0.6 : 0.6);
        ASSERT_TRUE(bool(reference)) << what;
        expectSameImage(chained, reference, window, what);
    }
}

// A Transform followed by a Mirror is one resample by the Mirror, with its impulse filter. Rotation
// stays out of it so the reference, a Transform with the translation and scale reflected, is exact
// to a rotation-free matrix. The Mirror reflects about the centre of the Transform's region, not
// the source's: y -> (y1 + y2) - y, so the reflected Transform about the centre 24 translates by
// (y1 + y2) - 48 - ty.
TEST_F(NativeMirrorTest, TransformThenMirrorIsOneResample)
{
    resetProject();
    NodePtr source = makeSource();
    const RectI window(-16, -16, 80, 64);
    const double ty = 3.2719;
    NodePtr transform = makeTransform(source, 5.3173, ty, 0., 0.75, 0.6);
    NodePtr mirror = makeMirror(transform, true, false);
    ASSERT_TRUE(bool(transform) && bool(mirror));
    expectConcatenatesInto(mirror, source, "transform then flip");

    const RectI transformRoD = pixelRoD(transform, 0);
    ASSERT_FALSE(transformRoD.isNull());
    NodePtr reference = makeTransform(source, 5.3173, (double)(transformRoD.y1 + transformRoD.y2) - 48. - ty, 0., 0.75, -0.6);
    ASSERT_TRUE(bool(reference));
    ASSERT_TRUE(setKnobValue(reference, kResamplerParamFilterType, std::string("impulse")));
    expectSameImage(mirror, reference, window, "transform then flip");
}

TEST_F(NativeMirrorTest, GpuMatchesCpuExactly)
{
    resetProject();
    const struct {
        int originX;
        int originY;
        bool flip;
        bool flop;
    } cases[] = {
        { 0, 0, true, false },
        { 0, 0, false, true },
        { 0, 0, true, true },
        { 7, -5, true, true },
    };
    for (const auto& c : cases) {
        NodePtr source = makeSource(c.originX, c.originY);
        NodePtr mirror = makeMirror(source, c.flip, c.flop);
        ASSERT_TRUE(bool(mirror));
        const std::string what = std::string(c.flip ? "flip " : "") + (c.flop ? "flop " : "") + "origin (" + std::to_string(c.originX) + ", " + std::to_string(c.originY) + ")";

        const RectI window = paritySourceWindow(source, kTime, 0);
        const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ source, mirror }), mirror, window, kTime);
        if (result.skipped) {
            return;
        }
        ASSERT_TRUE(result.ok) << what << ": " << result.error;
        EXPECT_TRUE(result.ranInRegion(mirror)) << what;

        ASSERT_EQ(result.cpu.size(), result.gpu.size()) << what;
        const ImageDiff diff = compareImages(result.cpu, result.gpu, ParityTolerance::exact());
        EXPECT_EQ(0u, diff.mismatches) << what << ": first at float " << diff.firstMismatchIndex << ", worst " << diff.worstAbsolute;
    }
}

TEST_F(NativeMirrorTest, UnversionedRequestsGetTheNativeMirror)
{
    NodePtr unversioned = createNode(QString::fromUtf8(PLUGINID_NATRON_MIRROR));
    ASSERT_TRUE(bool(unversioned));
    EXPECT_TRUE(isNative(unversioned));
    EXPECT_EQ(PLUGIN_MAJOR_NATRON_MIRROR, unversioned->getMajorVersion());
}

TEST_F(NativeMirrorTest, RegisteredIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_MIRROR, PLUGIN_MAJOR_NATRON_MIRROR, "Mirror");
}
