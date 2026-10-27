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
#include <functional>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativeParity.h"
#include "NativePluginIdentity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/EdgeExtend.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Merge/Merge.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;
const int kWidth = 64;
const int kHeight = 48;

// The opaque box of the matte and its colour; transparent black around it.
const RectI kBox(20, 14, 44, 34);
const double kColour[3] = { 0.8, 0.4, 0.2 };

bool
isNative(const NodePtr& node)
{
    return node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
}

RectI
pixelRoD(const NodePtr& node)
{
    EffectInstancePtr effect = node->getEffectInstance();
    RectD rod;
    const StatusEnum stat = effect->getRegionOfDefinition(effect->getRenderHash(), kTime, RenderScale::identity, ViewIdx(0), &rod);
    EXPECT_NE(eStatusFailed, stat);

    return rod.toPixelEnclosing(0, 1.);
}

bool
renderRGBA(const NodePtr& node,
           const RectI& window,
           RenderedPlane* plane)
{
    std::vector<RenderedPlane> planes;
    std::string error;
    const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &planes, &error);
    EXPECT_TRUE(ok) << error;
    if (!ok || (planes.size() != 1) || (planes[0].channels.size() != 4)) {
        return false;
    }
    *plane = planes[0];

    return true;
}

// The float of channel c of pixel (x, y) of an RGBA plane.
float
at(const RenderedPlane& plane,
   int x,
   int y,
   int c)
{
    const RectI& w = plane.window;

    return plane.pixels[((std::size_t)(y - w.y1) * w.width() + (std::size_t)(x - w.x1)) * 4 + c];
}

} // namespace

class NativeEdgeExtendTest
    : public BaseTest {
protected:
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kWidth, kHeight, "nativeEdgeExtend64x48", 1.));
    }

    // A Constant of colour rgba, over the project format or over box.
    NodePtr makeConstant(const std::vector<double>& rgba,
                         const RectI* box)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), PLUGIN_MAJOR_NATRON_CONSTANT);
        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        if (box) {
            EXPECT_TRUE(setKnobValue(node, kNativeGeneratorParamExtent, std::string(kNativeGeneratorExtentSize)));
            EXPECT_TRUE(setKnobValues(node, kNativeGeneratorParamBottomLeft, { (double)box->x1, (double)box->y1 }));
            EXPECT_TRUE(setKnobValues(node, kNativeGeneratorParamSize, { (double)box->width(), (double)box->height() }));
        } else {
            EXPECT_TRUE(setKnobValue(node, kNativeGeneratorParamExtent, std::string(kNativeGeneratorExtentProject)));
        }
        EXPECT_TRUE(setKnobValues(node, kConstantParamColor, rgba));

        return node;
    }

    // The matte: an opaque box of kColour over transparent black covering the format.
    NodePtr makeMatte()
    {
        NodePtr b = makeConstant({ 0., 0., 0., 0. }, NULL);
        NodePtr a = makeConstant({ kColour[0], kColour[1], kColour[2], 1. }, &kBox);
        NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_MERGE), PLUGIN_MAJOR_NATRON_MERGE);
        EXPECT_TRUE(bool(merge));
        if (!merge || !a || !b) {
            return NodePtr();
        }
        EXPECT_TRUE(setKnobValue(merge, kMergeParamOperation, std::string("over")));
        connectNodes(b, merge, 0, true);
        connectNodes(a, merge, 1, true);

        return merge;
    }

    NodePtr makeEdgeExtend(const NodePtr& source)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_EDGEEXTEND), PLUGIN_MAJOR_NATRON_EDGEEXTEND);
        EXPECT_TRUE(isNative(node));
        if (node && source) {
            connectNodes(source, node, 0, true);
        }

        return node;
    }

    // The source's and the node's renders over the node's region of definition, which must be
    // the source's.
    void renderPair(const NodePtr& source,
                    const NodePtr& node,
                    RenderedPlane* in,
                    RenderedPlane* out)
    {
        const RectI rod = pixelRoD(node);
        ASSERT_EQ(pixelRoD(source), rod);
        ASSERT_TRUE(renderRGBA(source, rod, in));
        ASSERT_TRUE(renderRGBA(node, rod, out));
        ASSERT_EQ(in->pixels.size(), out->pixels.size());
    }
};

TEST_F(NativeEdgeExtendTest, RegisteredIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_EDGEEXTEND, PLUGIN_MAJOR_NATRON_EDGEEXTEND, "EdgeExtend");
}

TEST_F(NativeEdgeExtendTest, InputsAndKnobs)
{
    resetProject();
    NodePtr node = makeEdgeExtend(NodePtr());
    ASSERT_TRUE(isNative(node));
    EXPECT_EQ(std::string("Source"), node->getInputLabel(0));
    EXPECT_EQ(std::string("Mask"), node->getInputLabel(1));
    EXPECT_TRUE(node->getEffectInstance()->isInputMask(1));
    EXPECT_TRUE(bool(node->getKnobByName("enableMask_Mask")));

    KnobBoolPtr sourcePremultiplied = std::dynamic_pointer_cast<KnobBool>(node->getKnobByName(kEdgeExtendParamSourcePremultiplied));
    ASSERT_TRUE(bool(sourcePremultiplied));
    EXPECT_TRUE(sourcePremultiplied->getValue());
    KnobDoublePtr threshold = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kEdgeExtendParamThreshold));
    ASSERT_TRUE(bool(threshold));
    EXPECT_EQ(0.5, threshold->getValue());
    EXPECT_EQ(0., threshold->getMinimum());
    EXPECT_EQ(1., threshold->getMaximum());
    KnobDoublePtr size = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kEdgeExtendParamSize));
    ASSERT_TRUE(bool(size));
    EXPECT_EQ(10., size->getValue());
    KnobIntPtr slices = std::dynamic_pointer_cast<KnobInt>(node->getKnobByName(kEdgeExtendParamSlices));
    ASSERT_TRUE(bool(slices));
    EXPECT_EQ(5, slices->getValue());
    KnobBoolPtr premultiplyOutput = std::dynamic_pointer_cast<KnobBool>(node->getKnobByName(kEdgeExtendParamPremultiplyOutput));
    ASSERT_TRUE(bool(premultiplyOutput));
    EXPECT_FALSE(premultiplyOutput->getValue());

    // The input's premultiplication comes first, above the size.
    const std::vector<KnobIPtr> knobs = node->getEffectInstance()->getKnobs_mt_safe();
    std::vector<std::string> order;
    for (const KnobIPtr& knob : knobs) {
        const std::string name = knob->getName();
        if ((name == kEdgeExtendParamSourcePremultiplied) || (name == kEdgeExtendParamThreshold) || (name == kEdgeExtendParamSize) || (name == kEdgeExtendParamSlices) || (name == kEdgeExtendParamPremultiplyOutput)) {
            order.push_back(name);
        }
    }
    const std::vector<std::string> expected = { kEdgeExtendParamSourcePremultiplied, kEdgeExtendParamThreshold, kEdgeExtendParamSize, kEdgeExtendParamSlices, kEdgeExtendParamPremultiplyOutput };
    EXPECT_EQ(expected, order);
}

// The slices' blurs add up to one of the size: their variances sum to that of size / 2.4, each
// slice's standard deviation i times the first's.
TEST_F(NativeEdgeExtendTest, SlicesAddUpToTheSize)
{
    for (int n : { 1, 2, 5, 12 }) {
        const std::vector<double> sigmas = EdgeExtend::sliceSigmas(24., n);
        ASSERT_EQ((std::size_t)n, sigmas.size());
        double variance = 0.;
        for (int i = 0; i < n; ++i) {
            EXPECT_NEAR(sigmas[0] * (i + 1), sigmas[i], 1e-12) << n << " slices, slice " << i;
            variance += sigmas[i] * sigmas[i];
        }
        EXPECT_NEAR(10., std::sqrt(variance), 1e-9) << n << " slices";
    }
}

// The opaque pixels keep their colour and every alpha is the source's; the transparent pixels
// near the box take the box's colour, and those beyond the reach of the slices stay black.
TEST_F(NativeEdgeExtendTest, ExtendsTheEdgeColourOutwards)
{
    resetProject();
    NodePtr source = makeMatte();
    ASSERT_TRUE(bool(source));
    NodePtr node = makeEdgeExtend(source);
    ASSERT_TRUE(bool(node));
    RenderedPlane in, out;
    ASSERT_NO_FATAL_FAILURE(renderPair(source, node, &in, &out));

    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            ASSERT_EQ(at(in, x, y, 3), at(out, x, y, 3)) << "alpha at (" << x << "," << y << ")";
            if (kBox.contains(x, y)) {
                for (int c = 0; c < 3; ++c) {
                    ASSERT_EQ(at(in, x, y, c), at(out, x, y, c)) << "opaque pixel (" << x << "," << y << ") channel " << c;
                }
            }
        }
    }
    // One and three pixels outside each side of the box.
    for (int d : { 1, 3 }) {
        const int px[4][2] = {
            { kBox.x1 - d, (kBox.y1 + kBox.y2) / 2 },
            { kBox.x2 - 1 + d, (kBox.y1 + kBox.y2) / 2 },
            { (kBox.x1 + kBox.x2) / 2, kBox.y1 - d },
            { (kBox.x1 + kBox.x2) / 2, kBox.y2 - 1 + d },
        };
        for (const auto& p : px) {
            ASSERT_EQ(0.f, at(in, p[0], p[1], 3));
            for (int c = 0; c < 3; ++c) {
                EXPECT_NEAR(kColour[c], at(out, p[0], p[1], c), 1e-4) << "(" << p[0] << "," << p[1] << ") channel " << c;
            }
        }
    }
}

// Nothing spreads past the reach of the kernels: one slice of size 2.4 is a Gaussian of standard
// deviation 1 and radius 3, so the colour reaches three pixels out and the fourth stays black.
TEST_F(NativeEdgeExtendTest, StopsAtTheKernelsReach)
{
    resetProject();
    NodePtr source = makeMatte();
    ASSERT_TRUE(bool(source));
    NodePtr node = makeEdgeExtend(source);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kEdgeExtendParamSize, { 2.4 }));
    ASSERT_TRUE(setKnobValues(node, kEdgeExtendParamSlices, { 1. }));
    RenderedPlane in, out;
    ASSERT_NO_FATAL_FAILURE(renderPair(source, node, &in, &out));

    const int y = (kBox.y1 + kBox.y2) / 2;
    for (int c = 0; c < 3; ++c) {
        EXPECT_NEAR(kColour[c], at(out, kBox.x1 - 3, y, c), 1e-4) << "channel " << c;
        EXPECT_EQ(0.f, at(out, kBox.x1 - 4, y, c)) << "channel " << c;
    }
    EXPECT_EQ(0.f, at(out, kBox.x1 - 3, y, 3));
}

// The colour of the contaminated ring, kRingColour, at alpha kRingAlpha around the box, which has
// alpha kBoxAlpha over the ring, 0.72: with the default threshold of 0.5, the box is solid and the
// ring is not.
const RectI kRing(10, 6, 54, 42);
const double kRingColour[3] = { 0.1, 0.9, 0.1 };
const double kRingAlpha = 0.3;
const double kBoxAlpha = 0.6;

class NativeEdgeExtendSoftTest
    : public NativeEdgeExtendTest {
protected:
    // A soft matte: the box of kColour at kBoxAlpha over the ring of kRingColour at kRingAlpha,
    // over transparent black, with the colours premultiplied or straight. Over black the ring
    // keeps its values, and the straight box's colour is whatever over makes of it: the tests
    // read the box's values from the render.
    NodePtr makeSoftMatte(bool premultiplied)
    {
        const double box = premultiplied ? kBoxAlpha : 1.;
        const double ring = premultiplied ? kRingAlpha : 1.;
        NodePtr black = makeConstant({ 0., 0., 0., 0. }, NULL);
        NodePtr ringNode = makeConstant({ kRingColour[0] * ring, kRingColour[1] * ring, kRingColour[2] * ring, kRingAlpha }, &kRing);
        NodePtr boxNode = makeConstant({ kColour[0] * box, kColour[1] * box, kColour[2] * box, kBoxAlpha }, &kBox);

        return makeMerge(makeMerge(black, ringNode, "over"), boxNode, "over");
    }

    NodePtr makeMerge(const NodePtr& b,
                      const NodePtr& a,
                      const char* operation)
    {
        NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_MERGE), PLUGIN_MAJOR_NATRON_MERGE);
        EXPECT_TRUE(bool(merge));
        if (!merge || !a || !b) {
            return NodePtr();
        }
        EXPECT_TRUE(setKnobValue(merge, kMergeParamOperation, std::string(operation)));
        connectNodes(b, merge, 0, true);
        connectNodes(a, merge, 1, true);

        return merge;
    }
};

// A premultiplied input with Source Premultiplied and Premultiply Output comes back premultiplied:
// the solid box within a rounding, its alpha exactly, and the ring's colour replaced by the box's,
// times the ring's alpha. Without Premultiply Output the colour is the straight one.
TEST_F(NativeEdgeExtendSoftTest, PremultipliedRoundTrip)
{
    resetProject();
    NodePtr source = makeSoftMatte(true);
    ASSERT_TRUE(bool(source));
    NodePtr premultiplied = makeEdgeExtend(source);
    NodePtr straight = makeEdgeExtend(source);
    ASSERT_TRUE(bool(premultiplied) && bool(straight));
    ASSERT_TRUE(setKnobValues(premultiplied, kEdgeExtendParamPremultiplyOutput, { 1. }));
    RenderedPlane in, out, unpremultiplied;
    ASSERT_NO_FATAL_FAILURE(renderPair(source, premultiplied, &in, &out));
    ASSERT_NO_FATAL_FAILURE(renderPair(source, straight, &in, &unpremultiplied));
    const int y = (kBox.y1 + kBox.y2) / 2;
    const float boxAlpha = at(in, kBox.x1, y, 3);
    ASSERT_NEAR(kBoxAlpha + kRingAlpha * (1. - kBoxAlpha), boxAlpha, 1e-6);
    float boxColour[3];
    for (int c = 0; c < 3; ++c) {
        boxColour[c] = at(in, kBox.x1, y, c) / boxAlpha;
    }

    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            ASSERT_EQ(at(in, x, y, 3), at(out, x, y, 3)) << "alpha at (" << x << "," << y << ")";
            ASSERT_EQ(at(in, x, y, 3), at(unpremultiplied, x, y, 3)) << "alpha at (" << x << "," << y << ")";
            if (kBox.contains(x, y)) {
                for (int c = 0; c < 3; ++c) {
                    ASSERT_NEAR(at(in, x, y, c), at(out, x, y, c), 1e-6) << "solid pixel (" << x << "," << y << ") channel " << c;
                    ASSERT_NEAR(boxColour[c], at(unpremultiplied, x, y, c), 1e-6) << "solid pixel (" << x << "," << y << ") channel " << c;
                }
            }
        }
    }
    for (int d : { 1, 3 }) {
        const int x = kBox.x1 - d;
        ASSERT_NEAR(kRingAlpha, at(in, x, y, 3), 1e-6);
        ASSERT_NEAR(kRingColour[1] * kRingAlpha, at(in, x, y, 1), 1e-6) << "the source is premultiplied";
        for (int c = 0; c < 3; ++c) {
            EXPECT_NEAR(boxColour[c] * kRingAlpha, at(out, x, y, c), 1e-4) << "ring pixel " << d << " out, channel " << c;
            EXPECT_NEAR(boxColour[c], at(unpremultiplied, x, y, c), 1e-4) << "ring pixel " << d << " out, channel " << c;
        }
    }
}

// A straight input without Source Premultiplied or Premultiply Output gives back its solid pixels
// bit for bit, and fills the rest, the straight colour of its transparent pixels included, with
// the extended colour.
TEST_F(NativeEdgeExtendSoftTest, StraightRoundTrip)
{
    resetProject();
    NodePtr source = makeSoftMatte(false);
    ASSERT_TRUE(bool(source));
    NodePtr node = makeEdgeExtend(source);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kEdgeExtendParamSourcePremultiplied, { 0. }));
    RenderedPlane in, out;
    ASSERT_NO_FATAL_FAILURE(renderPair(source, node, &in, &out));
    const int y = (kBox.y1 + kBox.y2) / 2;
    ASSERT_NEAR(kRingColour[1], at(in, kBox.x1 - 2, y, 1), 1e-6) << "the source is straight";

    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            ASSERT_EQ(at(in, x, y, 3), at(out, x, y, 3)) << "alpha at (" << x << "," << y << ")";
            if (kBox.contains(x, y)) {
                for (int c = 0; c < 3; ++c) {
                    ASSERT_EQ(at(in, x, y, c), at(out, x, y, c)) << "solid pixel (" << x << "," << y << ") channel " << c;
                }
            }
        }
    }
    for (int c = 0; c < 3; ++c) {
        EXPECT_NEAR(at(in, kBox.x1, y, c), at(out, kBox.x1 - 2, y, c), 1e-4) << "ring pixel, channel " << c;
    }
}

// The threshold decides which pixels are solid: below the ring's alpha the ring keeps its own
// colour, and above the box's nothing is solid, so there is no colour to extend.
TEST_F(NativeEdgeExtendSoftTest, ThresholdSetsTheSolidPixels)
{
    resetProject();
    NodePtr source = makeSoftMatte(true);
    ASSERT_TRUE(bool(source));
    NodePtr low = makeEdgeExtend(source);
    NodePtr high = makeEdgeExtend(source);
    ASSERT_TRUE(bool(low) && bool(high));
    ASSERT_TRUE(setKnobValues(low, kEdgeExtendParamThreshold, { 0.25 }));
    ASSERT_TRUE(setKnobValues(high, kEdgeExtendParamThreshold, { 0.8 }));
    RenderedPlane in, lowOut, highOut;
    ASSERT_NO_FATAL_FAILURE(renderPair(source, low, &in, &lowOut));
    ASSERT_NO_FATAL_FAILURE(renderPair(source, high, &in, &highOut));

    const int y = (kBox.y1 + kBox.y2) / 2;
    for (int c = 0; c < 3; ++c) {
        // Two pixels into the ring, its colour unpremultiplied; and the box's.
        EXPECT_NEAR(kRingColour[c], at(lowOut, kBox.x1 - 2, y, c), 1e-6) << "channel " << c;
        EXPECT_NEAR(at(in, kBox.x1, y, c) / at(in, kBox.x1, y, 3), at(lowOut, kBox.x1, y, c), 1e-6) << "channel " << c;
    }
    for (std::size_t i = 0; i < highOut.pixels.size(); ++i) {
        if ((i % 4) == 3) {
            ASSERT_EQ(in.pixels[i], highOut.pixels[i]) << "float " << i;
        } else {
            ASSERT_EQ(0.f, highOut.pixels[i]) << "float " << i;
        }
    }
}

// Size 0 blurs nothing, and the node returns the source exactly, neither unpremultiplied nor
// replaced below the threshold.
TEST_F(NativeEdgeExtendTest, SizeZeroIsIdentity)
{
    resetProject();
    NodePtr source = makeMatte();
    ASSERT_TRUE(bool(source));
    NodePtr node = makeEdgeExtend(source);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kEdgeExtendParamSize, { 0. }));
    RenderedPlane in, out;
    ASSERT_NO_FATAL_FAILURE(renderPair(source, node, &in, &out));
    for (std::size_t i = 0; i < in.pixels.size(); ++i) {
        ASSERT_EQ(in.pixels[i], out.pixels[i]) << "float " << i;
    }
}

// A tile of the render equals the same pixels of the full render: the halo covers every slice's
// blur, and the nearest-pixel boundary only applies at the region of definition's edges.
TEST_F(NativeEdgeExtendTest, TileMatchesFullFrame)
{
    resetProject();
    NodePtr source = makeMatte();
    ASSERT_TRUE(bool(source));
    NodePtr node = makeEdgeExtend(source);
    ASSERT_TRUE(bool(node));
    for (int slices : { 1, 5 }) {
        ASSERT_TRUE(setKnobValues(node, kEdgeExtendParamSlices, { (double)slices }));
        const RectI rod = pixelRoD(node);
        const RectI tile(6, 4, 30, 22);
        RenderedPlane whole, part;
        ASSERT_TRUE(renderRGBA(node, rod, &whole));
        ASSERT_TRUE(renderRGBA(node, tile, &part));
        for (int y = tile.y1; y < tile.y2; ++y) {
            for (int x = tile.x1; x < tile.x2; ++x) {
                for (int c = 0; c < 4; ++c) {
                    ASSERT_NEAR(at(whole, x, y, c), at(part, x, y, c), 1e-6 * std::max(1.f, std::fabs(at(whole, x, y, c)))) << slices << " slices at (" << x << "," << y << ")";
                }
            }
        }
    }
}

// Legacy and task-graph renders agree bit for bit, with the default and the opposite
// premultiplications and another threshold, and with a mask and mix.
TEST_F(NativeEdgeExtendTest, SameBothWays)
{
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(source));
    setParitySourceOrigin(source, 0, 0);
    NodePtr node = makeEdgeExtend(source);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kEdgeExtendParamSize, { 6. }));

    const std::function<void(const std::string&)> expectSame = [&](const std::string& caseName) {
        std::vector<int> poolSizes;
        poolSizes.push_back(1);
        poolSizes.push_back(4);
        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            EffectInstancePtr effect = node->getEffectInstance();
            RectD rod;
            ASSERT_NE(eStatusFailed, effect->getRegionOfDefinition(effect->getRenderHash(), kTime, RenderScale::fromMipmapLevel(mipmapLevel), ViewIdx(0), &rod));
            const RectI window = rod.toPixelEnclosing(mipmapLevel, 1.);
            const RenderMismatch m = renderBothWaysDirect(node, kTime, ViewIdx(0), mipmapLevel, window, poolSizes);
            EXPECT_FALSE(m.any) << caseName << ", mipmap " << mipmapLevel << ": " << describe(m);
        }
    };
    expectSame("default");
    ASSERT_TRUE(setKnobValues(node, kEdgeExtendParamSourcePremultiplied, { 0. }));
    ASSERT_TRUE(setKnobValues(node, kEdgeExtendParamPremultiplyOutput, { 1. }));
    ASSERT_TRUE(setKnobValues(node, kEdgeExtendParamThreshold, { 0.2 }));
    ASSERT_TRUE(setKnobValues(node, kEdgeExtendParamSlices, { 2. }));
    expectSame("premultiplied-2-slices");

    NodePtr mask = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(mask));
    setParitySourceOrigin(mask, 8, 4);
    connectNodes(mask, node, 1, true);
    ASSERT_TRUE(setKnobValues(node, "enableMask_Mask", { 1. }));
    ASSERT_TRUE(setKnobValues(node, kOfxMixParamName, { 0.5 }));
    expectSame("premultiplied-2-slices-mask-mix");
}
