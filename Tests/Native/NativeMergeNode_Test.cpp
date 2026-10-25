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
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include <ofxNatron.h>

#include "BaseTest.h"
#include "NativeParity.h"
#include "NativePluginIdentity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/LayerRegistry.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Merge/MergeOps.h"
#include "Engine/Nodes/Merge/NativeMerge.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

// Expected pixels computed here in the order the operators use: equal but for the rounding of a
// fused or reordered operation.
const float kTolerance = 1e-6f;

// The first mismatch of got against want, both packed alike, beyond tolerance.
void
expectClose(const std::vector<float>& want,
            const std::vector<float>& got,
            float tolerance,
            const std::string& what)
{
    ASSERT_EQ(want.size(), got.size()) << what;
    std::size_t mismatches = 0;
    std::size_t first = 0;
    for (std::size_t i = 0; i < want.size(); ++i) {
        const bool close = (tolerance == 0.f) ? (want[i] == got[i]) : (std::fabs(want[i] - got[i]) <= tolerance * std::max(1.f, std::fabs(want[i])));
        if (!close) {
            if (mismatches == 0) {
                first = i;
            }
            ++mismatches;
        }
    }
    EXPECT_EQ(0u, mismatches) << what << ": first at float " << first << " of " << want.size() << ", want "
                              << (mismatches ? want[first] : 0.f) << " got " << (mismatches ? got[first] : 0.f);
}

// One pixel of op merging A (alpha a) over B (alpha b), alphas broadcast, as MergeOps computes it.
void
mergePixel(MergeOps::Operator op,
           const float* A,
           float a,
           const float* B,
           float b,
           float* out)
{
    const MergeOps::AlphaSpan aSpan = { &a, 0, 0 };
    const MergeOps::AlphaSpan bSpan = { &b, 0, 0 };

    MergeOps::mergeRow(op, 4, A, aSpan, B, bSpan, 1, out);
}

} // anonymous namespace

class NativeMergeNodeTest
    : public BaseTest {
protected:
    NodePtr createMerge()
    {
        NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_NATIVE_MERGE), PLUGIN_MAJOR_NATRON_NATIVE_MERGE);

        EXPECT_TRUE(bool(merge));

        return merge;
    }

    // An RGBA parity source with its region of definition at (x, y).
    NodePtr createSource(int x,
                         int y)
    {
        NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));

        EXPECT_TRUE(bool(source));
        if (source) {
            setParitySourceComponents(source, "rgba");
            setParitySourceOrigin(source, x, y);
        }

        return source;
    }

    // The pixels of node's layer over window at full scale, as many floats per pixel as the layer
    // has channels, zero outside its region of definition.
    static bool renderLayer(const NodePtr& node,
                            const RectI& window,
                            const ImageLayerDesc& layer,
                            std::vector<float>* pixels)
    {
        std::vector<RenderedPlane> planes;
        std::string error;
        const bool rendered = renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, std::list<ImageLayerDesc>(1, layer), &planes, &error);
        EXPECT_TRUE(rendered) << error;
        if (!rendered || (planes.size() != 1) || (planes[0].channels.size() != (std::size_t)layer.getNumComponents())) {
            return false;
        }
        *pixels = planes[0].pixels;

        return true;
    }

    static bool renderRGBA(const NodePtr& node,
                           const RectI& window,
                           std::vector<float>* pixels)
    {
        return renderLayer(node, window, ImageLayerDesc::getRGBAComponents(), pixels);
    }

    static int identityInput(const NodePtr& node,
                             const RectI& window,
                             bool* identity)
    {
        EffectInstancePtr effect = node->getEffectInstance();
        double inputTime = 0.;
        ViewIdx inputView(0);
        int inputNb = -2;

        *identity = effect->isIdentity_public(false, effect->getRenderHash(), kTime, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, &inputNb);

        return inputNb;
    }

    static RectD regionOfDefinition(const NodePtr& node)
    {
        EffectInstancePtr effect = node->getEffectInstance();
        RectD rod;
        bool isProjectFormat = false;

        EXPECT_NE(eStatusFailed, effect->getRegionOfDefinition_public(effect->getRenderHash(), kTime, RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat));

        return rod;
    }

    static void setLayerKnob(const NodePtr& node,
                             const std::string& name,
                             const std::string& layerID)
    {
        KnobLayerSelectPtr knob = node->getEffectInstance()->getKnobByNameAndType<KnobLayerSelect>(name);
        ASSERT_TRUE(bool(knob)) << name;
        knob->setLayer(layerID);
    }

    // The fold of op over B and the A inputs in order, alphas from rgba.A, as the node computes
    // it: each step merges colours with the alphas of the result so far and of the next A, and
    // the result's alpha with the operator ("over" for a non-separable one).
    static std::vector<float> expectedFold(MergeOps::Operator op,
                                           const std::vector<float>& b,
                                           const std::vector<const std::vector<float>*>& as)
    {
        const MergeOps::Operator alphaOp = MergeOps::isSeparable(op) ? op : MergeOps::eOver;
        std::vector<float> want(b.size());

        for (std::size_t p = 0; (p + 1) * 4 <= b.size(); ++p) {
            float result[4];
            float resultAlpha[4];
            for (int c = 0; c < 4; ++c) {
                result[c] = b[p * 4 + c];
                resultAlpha[c] = b[p * 4 + 3];
            }
            for (const std::vector<float>* a : as) {
                const float* A = &(*a)[p * 4];
                float aAlpha[4] = { A[3], A[3], A[3], A[3] };
                const MergeOps::AlphaSpan aSpan = { aAlpha, 4, 1 };
                const MergeOps::AlphaSpan rSpan = { resultAlpha, 4, 1 };
                float merged[4];
                float mergedAlpha[4];
                MergeOps::mergeRow(op, 4, A, aSpan, result, rSpan, 1, merged);
                MergeOps::mergeRow(alphaOp, 4, aAlpha, aSpan, resultAlpha, rSpan, 1, mergedAlpha);
                std::copy(merged, merged + 4, result);
                std::copy(mergedAlpha, mergedAlpha + 4, resultAlpha);
            }
            std::copy(result, result + 4, &want[p * 4]);
        }

        return want;
    }

    // Whether a, offset, reads zero over part of the window where b does not.
    static bool missesSomewhere(const std::vector<float>& a,
                                const std::vector<float>& b)
    {
        for (std::size_t p = 0; (p + 1) * 4 <= a.size(); ++p) {
            if ((a[p * 4] == 0.f) && (a[p * 4 + 3] == 0.f) && (b[p * 4] != 0.f)) {
                return true;
            }
        }

        return false;
    }
};

TEST_F(NativeMergeNodeTest, RegisteredIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_NATIVE_MERGE, PLUGIN_MAJOR_NATRON_NATIVE_MERGE, "Merge");
}

TEST_F(NativeMergeNodeTest, InputsAreBAMaskThenTheFurtherAs)
{
    NodePtr merge = createMerge();
    ASSERT_TRUE(bool(merge));
    // An inspector node, as the OpenFX Merge is, shows only the next free A input in the node graph.
    EXPECT_TRUE(dynamic_cast<InspectorNode*>(merge.get()) != NULL);

    EffectInstancePtr effect = merge->getEffectInstance();
    ASSERT_EQ(kNativeMergeMaxAInputs + 2, effect->getNInputs());
    EXPECT_EQ("B", effect->getInputLabel(kNativeMergeInputB));
    EXPECT_EQ("A", effect->getInputLabel(kNativeMergeInputA));
    EXPECT_EQ("Mask", effect->getInputLabel(kNativeMergeInputMask));
    EXPECT_EQ("A2", effect->getInputLabel(NativeMerge::aInput(1)));
    EXPECT_EQ("A64", effect->getInputLabel(NativeMerge::aInput(kNativeMergeMaxAInputs - 1)));
}

TEST_F(NativeMergeNodeTest, SubLabelShowsTheOperator)
{
    NodePtr merge = createMerge();
    ASSERT_TRUE(bool(merge));
    KnobStringPtr subLabel = merge->getEffectInstance()->getKnobByNameAndType<KnobString>(kNatronOfxParamStringSublabelName);
    ASSERT_TRUE(bool(subLabel));
    EXPECT_EQ("over", subLabel->getValue());
    ASSERT_TRUE(setKnobValue(merge, kNativeMergeParamOperation, "multiply"));
    EXPECT_EQ("multiply", subLabel->getValue());
}

// A offset from B, so that it reads zero over part of B's window.
TEST_F(NativeMergeNodeTest, OverWithAnOffsetA)
{
    NodePtr merge = createMerge();
    NodePtr b = createSource(0, 0);
    NodePtr a = createSource(10, 6);
    ASSERT_TRUE(merge && b && a);
    connectNodes(b, merge, kNativeMergeInputB, true);
    connectNodes(a, merge, kNativeMergeInputA, true);
    const RectI window = paritySourceWindow(b, kTime, 0);
    ASSERT_FALSE(window.isNull());

    std::vector<float> bPixels, aPixels, got;
    ASSERT_TRUE(renderRGBA(b, window, &bPixels));
    ASSERT_TRUE(renderRGBA(a, window, &aPixels));
    ASSERT_TRUE(renderRGBA(merge, window, &got));
    EXPECT_TRUE(missesSomewhere(aPixels, bPixels));

    // over by hand: A + B(1 - a), on every channel.
    std::vector<float> want(bPixels.size());
    for (std::size_t p = 0; (p + 1) * 4 <= want.size(); ++p) {
        const float alpha = aPixels[p * 4 + 3];
        for (int c = 0; c < 4; ++c) {
            want[p * 4 + c] = aPixels[p * 4 + c] + bPixels[p * 4 + c] * (1.f - alpha);
        }
    }
    expectClose(want, got, kTolerance, "over");
}

// A, A2 and A3 fold in order, each over the result so far with its carried alpha.
TEST_F(NativeMergeNodeTest, ThreeAInputsFoldInOrder)
{
    NodePtr merge = createMerge();
    NodePtr b = createSource(0, 0);
    NodePtr a = createSource(10, 6);
    NodePtr a2 = createSource(1, 2);
    NodePtr a3 = createSource(3, 1);
    ASSERT_TRUE(merge && b && a && a2 && a3);
    connectNodes(b, merge, kNativeMergeInputB, true);
    connectNodes(a, merge, NativeMerge::aInput(0), true);
    connectNodes(a2, merge, NativeMerge::aInput(1), true);
    connectNodes(a3, merge, NativeMerge::aInput(2), true);
    const RectI window = paritySourceWindow(b, kTime, 0);
    ASSERT_FALSE(window.isNull());

    std::vector<float> bPixels, aPixels, a2Pixels, a3Pixels;
    ASSERT_TRUE(renderRGBA(b, window, &bPixels));
    ASSERT_TRUE(renderRGBA(a, window, &aPixels));
    ASSERT_TRUE(renderRGBA(a2, window, &a2Pixels));
    ASSERT_TRUE(renderRGBA(a3, window, &a3Pixels));
    const std::vector<const std::vector<float>*> as = { &aPixels, &a2Pixels, &a3Pixels };

    const char* const operators[] = { "over", "xor" };
    for (const char* name : operators) {
        MergeOps::Operator op = MergeOps::eOver;
        ASSERT_TRUE(MergeOps::operatorFromName(name, &op)) << name;
        ASSERT_TRUE(setKnobValue(merge, kNativeMergeParamOperation, name)) << name;
        std::vector<float> got;
        ASSERT_TRUE(renderRGBA(merge, window, &got)) << name;
        expectClose(expectedFold(op, bPixels, as), got, kTolerance, name);
    }
}

TEST_F(NativeMergeNodeTest, BBoxModes)
{
    NodePtr merge = createMerge();
    NodePtr b = createSource(0, 0);
    NodePtr a = createSource(10, 6);
    NodePtr a2 = createSource(-5, 20);
    ASSERT_TRUE(merge && b && a && a2);
    connectNodes(b, merge, kNativeMergeInputB, true);
    connectNodes(a, merge, NativeMerge::aInput(0), true);
    connectNodes(a2, merge, NativeMerge::aInput(1), true);

    const RectD bRoD = regionOfDefinition(b);
    const RectD aRoD = regionOfDefinition(a);
    const RectD a2RoD = regionOfDefinition(a2);
    ASSERT_FALSE(bRoD.isNull() || aRoD.isNull() || a2RoD.isNull());

    RectD unionRoD = bRoD;
    unionRoD.merge(aRoD);
    unionRoD.merge(a2RoD);
    const RectD intersectionRoD = bRoD.intersect(aRoD).intersect(a2RoD);
    ASSERT_FALSE(intersectionRoD.isNull());
    RectD aUnion = aRoD;
    aUnion.merge(a2RoD);

    const struct {
        const char* mode;
        RectD want;
    } cases[] = {
        { kNativeMergeBBoxUnion, unionRoD },
        { kNativeMergeBBoxIntersection, intersectionRoD },
        { kNativeMergeBBoxA, aUnion },
        { kNativeMergeBBoxB, bRoD },
    };
    for (const auto& c : cases) {
        ASSERT_TRUE(setKnobValue(merge, kNativeMergeParamBBox, c.mode)) << c.mode;
        const RectD rod = regionOfDefinition(merge);
        EXPECT_EQ(c.want.x1, rod.x1) << c.mode;
        EXPECT_EQ(c.want.y1, rod.y1) << c.mode;
        EXPECT_EQ(c.want.x2, rod.x2) << c.mode;
        EXPECT_EQ(c.want.y2, rod.y2) << c.mode;
    }

    // At a mix of 0 the output is B, whatever the mode.
    ASSERT_TRUE(setKnobValue(merge, kNativeMergeParamBBox, kNativeMergeBBoxA));
    ASSERT_TRUE(setKnobValues(merge, kOfxMixParamName, { 0. }));
    const RectD rod = regionOfDefinition(merge);
    EXPECT_EQ(bRoD.x1, rod.x1);
    EXPECT_EQ(bRoD.x2, rod.x2);
    EXPECT_EQ(bRoD.y1, rod.y1);
    EXPECT_EQ(bRoD.y2, rod.y2);
}

// The mask's alpha times mix blends the merge with B, and a mix of 0 is an identity of B.
TEST_F(NativeMergeNodeTest, MaskAndMix)
{
    NodePtr merge = createMerge();
    NodePtr b = createSource(0, 0);
    NodePtr a = createSource(10, 6);
    NodePtr mask = createSource(5, 20);
    ASSERT_TRUE(merge && b && a && mask);
    connectNodes(b, merge, kNativeMergeInputB, true);
    connectNodes(a, merge, kNativeMergeInputA, true);
    connectNodes(mask, merge, kNativeMergeInputMask, true);
    ASSERT_TRUE(setKnobValues(merge, "enableMask_Mask", { 1. }));
    const double kMix = 0.5;
    ASSERT_TRUE(setKnobValues(merge, kOfxMixParamName, { kMix }));
    const RectI window = paritySourceWindow(b, kTime, 0);
    ASSERT_FALSE(window.isNull());

    std::vector<float> bPixels, aPixels, maskPixels, got;
    ASSERT_TRUE(renderRGBA(b, window, &bPixels));
    ASSERT_TRUE(renderRGBA(a, window, &aPixels));
    ASSERT_TRUE(renderRGBA(mask, window, &maskPixels));
    ASSERT_TRUE(renderRGBA(merge, window, &got));

    const std::vector<float> over = expectedFold(MergeOps::eOver, bPixels, { &aPixels });
    std::vector<float> want(bPixels.size());
    bool partial = false;
    for (std::size_t p = 0; (p + 1) * 4 <= want.size(); ++p) {
        const float m = maskPixels[p * 4 + 3] * (float)kMix;
        partial = partial || ((m > 0.f) && (m < (float)kMix));
        for (int c = 0; c < 4; ++c) {
            const float under = bPixels[p * 4 + c];
            const float v = over[p * 4 + c];
            want[p * 4 + c] = (m == 0.f) ? under : v * m + (1.f - m) * under;
        }
    }
    EXPECT_TRUE(partial);
    expectClose(want, got, kTolerance, "masked over at mix 0.5");

    ASSERT_TRUE(setKnobValues(merge, kOfxMixParamName, { 0. }));
    bool identity = false;
    EXPECT_EQ(kNativeMergeInputB, identityInput(merge, window, &identity));
    EXPECT_TRUE(identity);
}

// With no A, an operator that leaves B under a transparent A is an identity of B; one that does
// not merges a transparent A.
TEST_F(NativeMergeNodeTest, IdentityWithNoA)
{
    NodePtr merge = createMerge();
    NodePtr b = createSource(0, 0);
    ASSERT_TRUE(merge && b);
    connectNodes(b, merge, kNativeMergeInputB, true);
    const RectI window = paritySourceWindow(b, kTime, 0);
    ASSERT_FALSE(window.isNull());

    bool identity = false;
    EXPECT_EQ(kNativeMergeInputB, identityInput(merge, window, &identity));
    EXPECT_TRUE(identity);

    // in is Ab: zero for a transparent A.
    ASSERT_TRUE(setKnobValue(merge, kNativeMergeParamOperation, "in"));
    identityInput(merge, window, &identity);
    EXPECT_FALSE(identity);
    std::vector<float> got;
    ASSERT_TRUE(renderRGBA(merge, window, &got));
    expectClose(std::vector<float>(got.size(), 0.f), got, 0.f, "in with no A");

    // Connecting A ends the identity of an over.
    ASSERT_TRUE(setKnobValue(merge, kNativeMergeParamOperation, "over"));
    NodePtr a = createSource(10, 6);
    ASSERT_TRUE(bool(a));
    connectNodes(a, merge, kNativeMergeInputA, true);
    identityInput(merge, window, &identity);
    EXPECT_FALSE(identity);
}

// hue blends colours with the first channel's alpha, and its alpha merges "over".
TEST_F(NativeMergeNodeTest, NonSeparableOperator)
{
    NodePtr merge = createMerge();
    NodePtr b = createSource(0, 0);
    NodePtr a = createSource(10, 6);
    ASSERT_TRUE(merge && b && a);
    connectNodes(b, merge, kNativeMergeInputB, true);
    connectNodes(a, merge, kNativeMergeInputA, true);
    ASSERT_TRUE(setKnobValue(merge, kNativeMergeParamOperation, "hue"));
    const RectI window = paritySourceWindow(b, kTime, 0);
    ASSERT_FALSE(window.isNull());

    std::vector<float> bPixels, aPixels, got;
    ASSERT_TRUE(renderRGBA(b, window, &bPixels));
    ASSERT_TRUE(renderRGBA(a, window, &aPixels));
    ASSERT_TRUE(renderRGBA(merge, window, &got));

    std::vector<float> want(bPixels.size());
    for (std::size_t p = 0; (p + 1) * 4 <= want.size(); ++p) {
        mergePixel(MergeOps::eHue, &aPixels[p * 4], aPixels[p * 4 + 3], &bPixels[p * 4], bPixels[p * 4 + 3], &want[p * 4]);
    }
    expectClose(want, got, kTolerance, "hue");
}

// A read in its three-channel spec layer over B's rgba, with A's alpha from its rgba.A.
TEST_F(NativeMergeNodeTest, ASpecOverBRGBA)
{
    NodePtr merge = createMerge();
    NodePtr b = createSource(0, 0);
    NodePtr a = createSource(10, 6);
    ASSERT_TRUE(merge && b && a);
    setParitySourceExtraLayer(a, true);
    connectNodes(b, merge, kNativeMergeInputB, true);
    connectNodes(a, merge, kNativeMergeInputA, true);
    setLayerKnob(merge, kNativeMergeParamALayer, kParitySourceExtraLayerID);
    const RectI window = paritySourceWindow(b, kTime, 0);
    ASSERT_FALSE(window.isNull());

    std::vector<float> bPixels, aPixels, aSpec, got;
    ASSERT_TRUE(renderRGBA(b, window, &bPixels));
    ASSERT_TRUE(renderRGBA(a, window, &aPixels));
    ASSERT_TRUE(renderLayer(a, window, paritySourceExtraLayer(), &aSpec));
    ASSERT_TRUE(renderRGBA(merge, window, &got));

    // A three-channel layer is read as RGB, its fourth channel reading zero.
    std::vector<float> want(bPixels.size());
    for (std::size_t p = 0; (p + 1) * 4 <= want.size(); ++p) {
        const float A[4] = { aSpec[p * 3], aSpec[p * 3 + 1], aSpec[p * 3 + 2], 0.f };
        mergePixel(MergeOps::eOver, A, aPixels[p * 4 + 3], &bPixels[p * 4], bPixels[p * 4 + 3], &want[p * 4]);
    }
    expectClose(want, got, kTolerance, "A spec over B rgba");
}

// The output layer is a project layer B lacks: it receives the merge, and B's rgba passes through.
TEST_F(NativeMergeNodeTest, OutputToANewLayer)
{
    std::string error;
    const ImageLayerDesc mergeOut("mergeout", "mergeout", "", { "R", "G", "B", "A" });
    ASSERT_EQ(LayerRegistry::eAddResultAdded, getApp()->getProject()->addLayer(mergeOut, LayerRegistryEntry::eOriginUser, &error)) << error;

    NodePtr merge = createMerge();
    NodePtr b = createSource(0, 0);
    NodePtr a = createSource(10, 6);
    ASSERT_TRUE(merge && b && a);
    connectNodes(b, merge, kNativeMergeInputB, true);
    connectNodes(a, merge, kNativeMergeInputA, true);
    setLayerKnob(merge, kNodeParamLayerSelect, "mergeout");
    const RectI window = paritySourceWindow(b, kTime, 0);
    ASSERT_FALSE(window.isNull());

    std::vector<float> bPixels, aPixels, got, rgba;
    ASSERT_TRUE(renderRGBA(b, window, &bPixels));
    ASSERT_TRUE(renderRGBA(a, window, &aPixels));
    ASSERT_TRUE(renderLayer(merge, window, mergeOut, &got));
    expectClose(expectedFold(MergeOps::eOver, bPixels, { &aPixels }), got, kTolerance, "over into mergeout");

    ASSERT_TRUE(renderRGBA(merge, window, &rgba));
    expectClose(bPixels, rgba, 0.f, "rgba passes through from B");
}
