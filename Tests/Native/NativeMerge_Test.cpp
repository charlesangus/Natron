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
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/LayerRegistry.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Channel/Shuffle.h"
#include "Engine/Nodes/Merge/Dissolve.h"
#include "Engine/Nodes/Merge/Merge.h"
#include "Engine/Nodes/Merge/MergeOps.h"
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

    static void setChannelKnob(const NodePtr& node,
                               const std::string& name,
                               const std::string& channel)
    {
        KnobChannelSelectPtr knob = node->getEffectInstance()->getKnobByNameAndType<KnobChannelSelect>(name);
        ASSERT_TRUE(bool(knob)) << name;
        knob->set(channel);
    }

    static std::string persistentMessage(const NodePtr& node)
    {
        QString message;
        int type = 0;
        node->getPersistentMessage(&message, &type, false);

        return message.toStdString();
    }

    // One side of a merge as the node reads it: in rgba or in the three-channel spec layer, with
    // its alpha from its alpha source.
    struct Side {
        const char* source;
        bool inSpec;
    };

    // A over B, each read and given an alpha as its Side says, both sources carrying spec. B in
    // spec is Layers listing spec alone, rendered in spec; B in rgba is the default Layers,
    // rendered in rgba. The universal channels are rgba.R, so that a universal alpha differs from
    // a layer's own, and the per-channel alpha layers are spec, standing in for an alphaRGB layer.
    void expectAlphaSources(const Side& aSide,
                            const Side& bSide)
    {
        const std::string what = std::string("A ") + aSide.source + (aSide.inSpec ? " in spec" : " in rgba") + " over B " + bSide.source + (bSide.inSpec ? " in spec" : " in rgba");
        NodePtr merge = createMerge();
        NodePtr b = createSource(0, 0);
        NodePtr a = createSource(10, 6);
        ASSERT_TRUE(merge && b && a) << what;
        setParitySourceExtraLayer(a, true);
        setParitySourceExtraLayer(b, true);
        connectNodes(b, merge, kNativeMergeInputB, true);
        connectNodes(a, merge, kNativeMergeInputA, true);
        const std::string rgbaR = std::string(kNatronColorViewRGBA) + ".R";
        setChannelKnob(merge, kNativeMergeParamAAlpha, rgbaR);
        setChannelKnob(merge, kNativeMergeParamBAlpha, rgbaR);
        setLayerKnob(merge, kNativeMergeParamAAlphaLayer, kParitySourceExtraLayerID);
        setLayerKnob(merge, kNativeMergeParamBAlphaLayer, kParitySourceExtraLayerID);
        setLayerKnob(merge, kNativeMergeParamALayer, aSide.inSpec ? kParitySourceExtraLayerID : kNatronColorViewRGBA);
        if (bSide.inSpec) {
            setLayers(merge, { kParitySourceExtraLayerID });
        }
        ASSERT_TRUE(setKnobValue(merge, kNativeMergeParamAAlphaSource, aSide.source)) << what;
        ASSERT_TRUE(setKnobValue(merge, kNativeMergeParamBAlphaSource, bSide.source)) << what;
        const RectI window = paritySourceWindow(b, kTime, 0);
        ASSERT_FALSE(window.isNull()) << what;

        std::vector<float> aRGBA, aSpec, bRGBA, bSpec, got;
        ASSERT_TRUE(renderRGBA(a, window, &aRGBA)) << what;
        ASSERT_TRUE(renderLayer(a, window, paritySourceExtraLayer(), &aSpec)) << what;
        ASSERT_TRUE(renderRGBA(b, window, &bRGBA)) << what;
        ASSERT_TRUE(renderLayer(b, window, paritySourceExtraLayer(), &bSpec)) << what;
        ASSERT_TRUE(renderLayer(merge, window, bSide.inSpec ? paritySourceExtraLayer() : ImageLayerDesc::getRGBAComponents(), &got)) << what;

        // The merged layer's channels are read by index: a three-channel layer read into rgba has
        // its fourth channel reading zero, and rgba read into spec drops its fourth.
        const int nComps = bSide.inSpec ? 3 : 4;
        const auto read = [nComps](const Side& side, const std::vector<float>& rgba, const std::vector<float>& spec, std::size_t p, float* pixel, float* alpha) {
            for (int c = 0; c < nComps; ++c) {
                const float specValue = (c < 3) ? spec[p * 3 + c] : 0.f;
                pixel[c] = side.inSpec ? specValue : rgba[p * 4 + c];
                if (std::string(side.source) == kNativeMergeAlphaPerChannel) {
                    alpha[c] = specValue;
                } else if ((std::string(side.source) == kNativeMergeAlphaPerLayer) && !side.inSpec) {
                    alpha[c] = rgba[p * 4 + 3];
                } else {
                    alpha[c] = rgba[p * 4];
                }
            }
        };
        std::vector<float> want(bSide.inSpec ? bSpec.size() : bRGBA.size());
        for (std::size_t p = 0; (p + 1) * nComps <= want.size(); ++p) {
            float A[4], aAlpha[4], B[4], bAlpha[4];
            read(aSide, aRGBA, aSpec, p, A, aAlpha);
            read(bSide, bRGBA, bSpec, p, B, bAlpha);
            const MergeOps::AlphaSpan aSpan = { aAlpha, nComps, 1 };
            const MergeOps::AlphaSpan bSpan = { bAlpha, nComps, 1 };
            MergeOps::mergeRow(MergeOps::eOver, nComps, A, aSpan, B, bSpan, 1, &want[p * nComps]);
        }
        expectClose(want, got, kTolerance, what);
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

    // The planes node needs from inputNb at kTime, -1 for the planes it outputs.
    static std::list<ImageLayerDesc> neededPlanes(const NodePtr& node,
                                                  int inputNb)
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

        return comps[inputNb];
    }

    static bool hasPlane(const std::list<ImageLayerDesc>& planes,
                         const std::string& layerID)
    {
        for (const ImageLayerDesc& plane : planes) {
            if (plane.getLayerID() == layerID) {
                return true;
            }
        }

        return false;
    }

    // Sets Layers to layerIDs, one row each; no layer is an All row.
    static void setLayers(const NodePtr& node,
                          const std::vector<std::string>& layerIDs)
    {
        KnobChannelSetPtr knob = node->getEffectInstance()->getKnobByNameAndType<KnobChannelSet>(kNativeMergeParamLayers);
        ASSERT_TRUE(bool(knob));
        std::vector<ChannelSetRow> rows(std::max<std::size_t>(1, layerIDs.size()));
        if (layerIDs.empty()) {
            rows[0].mode = ChannelSetRow::eModeAll;
        }
        for (std::size_t i = 0; i < layerIDs.size(); ++i) {
            rows[i].mode = ChannelSetRow::eModeLayer;
            rows[i].layerOrPattern = layerIDs[i];
        }
        knob->setRows(rows);
    }

    // B's colour layers copied into the project layers diffuse and normals, and its spec into
    // albedo, by two Shuffles after a parity source at (x, y) carrying spec: B then carries rgba,
    // spec, diffuse, albedo and normals, each with its own values but for diffuse and normals.
    NodePtr createLayeredSource(int x,
                                int y)
    {
        NodePtr source = createSource(x, y);
        NodePtr toDiffuse = createNode(QString::fromUtf8(PLUGINID_NATRON_SHUFFLE));
        NodePtr toNormals = createNode(QString::fromUtf8(PLUGINID_NATRON_SHUFFLE));
        EXPECT_TRUE(source && toDiffuse && toNormals);
        if (!source || !toDiffuse || !toNormals) {
            return NodePtr();
        }
        setParitySourceExtraLayer(source, true);
        connectNodes(source, toDiffuse, Shuffle::eInputMain, true);
        connectNodes(toDiffuse, toNormals, Shuffle::eInputMain, true);
        setLayerKnob(toDiffuse, kShuffleParamIn1, kNatronColorViewRGBA);
        setLayerKnob(toDiffuse, kShuffleParamOut1, "diffuse");
        setLayerKnob(toDiffuse, kShuffleParamIn2, kParitySourceExtraLayerID);
        setLayerKnob(toDiffuse, kShuffleParamOut2, "albedo");
        setLayerKnob(toNormals, kShuffleParamIn1, kNatronColorViewRGBA);
        setLayerKnob(toNormals, kShuffleParamOut1, "normals");

        return toNormals;
    }

    // Adds diffuse, albedo and normals, three channels each, to the project.
    void addProjectLayers()
    {
        for (const char* id : { "diffuse", "albedo", "normals" }) {
            std::string error;
            EXPECT_EQ(LayerRegistry::eAddResultAdded, getApp()->getProject()->addLayer(ImageLayerDesc(id, id, "", { "R", "G", "B" }), LayerRegistryEntry::eOriginUser, &error)) << id << ": " << error;
        }
    }

    static ImageLayerDesc projectLayer(const std::string& id)
    {
        return ImageLayerDesc(id, id, "", { "R", "G", "B" });
    }

    // The first three channels of each rgba pixel.
    static std::vector<float> rgbOf(const std::vector<float>& rgba)
    {
        std::vector<float> rgb;

        for (std::size_t p = 0; (p + 1) * 4 <= rgba.size(); ++p) {
            rgb.insert(rgb.end(), rgba.begin() + p * 4, rgba.begin() + p * 4 + 3);
        }

        return rgb;
    }

    // op merging A over B in a three-channel layer, each channel's alphas broadcast from the rgba
    // alphas (packed four floats per pixel); an empty aLayer is a layer A lacks, read as zero.
    static std::vector<float> expectedLayerMerge(MergeOps::Operator op,
                                                 const std::vector<float>& aLayer,
                                                 const std::vector<float>& aRGBA,
                                                 const std::vector<float>& bLayer,
                                                 const std::vector<float>& bRGBA)
    {
        std::vector<float> want(bLayer.size());

        for (std::size_t p = 0; (p + 1) * 3 <= want.size(); ++p) {
            const float a = aRGBA[p * 4 + 3];
            const float b = bRGBA[p * 4 + 3];
            const float zero[3] = { 0.f, 0.f, 0.f };
            const float* A = aLayer.empty() ? zero : &aLayer[p * 3];
            const MergeOps::AlphaSpan aSpan = { &a, 0, 0 };
            const MergeOps::AlphaSpan bSpan = { &b, 0, 0 };
            MergeOps::mergeRow(op, 3, A, aSpan, &bLayer[p * 3], bSpan, 1, &want[p * 3]);
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
        ASSERT_TRUE(setKnobValue(merge, kNativeMergeParamSetBBox, c.mode)) << c.mode;
        const RectD rod = regionOfDefinition(merge);
        EXPECT_EQ(c.want.x1, rod.x1) << c.mode;
        EXPECT_EQ(c.want.y1, rod.y1) << c.mode;
        EXPECT_EQ(c.want.x2, rod.x2) << c.mode;
        EXPECT_EQ(c.want.y2, rod.y2) << c.mode;
    }

    // At a mix of 0 the output is B, whatever the mode.
    ASSERT_TRUE(setKnobValue(merge, kNativeMergeParamSetBBox, kNativeMergeBBoxA));
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

// Each alpha source on each side: a universal channel, a layer's own alpha with the universal
// channel for a layer without one, and a per-channel alpha layer.
TEST_F(NativeMergeNodeTest, AlphaSourcesPerSide)
{
    const Side universalRGBA = { kNativeMergeAlphaUniversal, false };
    const Side universalSpec = { kNativeMergeAlphaUniversal, true };
    const Side perLayerRGBA = { kNativeMergeAlphaPerLayer, false };
    const Side perLayerSpec = { kNativeMergeAlphaPerLayer, true };
    const Side perChannelSpec = { kNativeMergeAlphaPerChannel, true };

    expectAlphaSources(universalSpec, universalRGBA);
    expectAlphaSources(perLayerRGBA, universalRGBA);
    expectAlphaSources(perLayerSpec, universalRGBA);
    expectAlphaSources(universalRGBA, perLayerRGBA);
    expectAlphaSources(perLayerRGBA, perLayerSpec);
    expectAlphaSources(perChannelSpec, universalRGBA);
    expectAlphaSources(universalRGBA, perChannelSpec);
    expectAlphaSources(perChannelSpec, perChannelSpec);
}

// Only the chosen source's knobs show: the universal channel unless the side reads a per-channel
// alpha layer, and the alpha layer only then.
TEST_F(NativeMergeNodeTest, AlphaSourceShowsItsKnobs)
{
    NodePtr merge = createMerge();
    ASSERT_TRUE(bool(merge));
    EffectInstancePtr effect = merge->getEffectInstance();
    KnobChannelSelectPtr aAlpha = effect->getKnobByNameAndType<KnobChannelSelect>(kNativeMergeParamAAlpha);
    KnobLayerSelectPtr aAlphaLayer = effect->getKnobByNameAndType<KnobLayerSelect>(kNativeMergeParamAAlphaLayer);
    KnobChannelSelectPtr bAlpha = effect->getKnobByNameAndType<KnobChannelSelect>(kNativeMergeParamBAlpha);
    KnobLayerSelectPtr bAlphaLayer = effect->getKnobByNameAndType<KnobLayerSelect>(kNativeMergeParamBAlphaLayer);
    ASSERT_TRUE(aAlpha && aAlphaLayer && bAlpha && bAlphaLayer);

    EXPECT_FALSE(aAlpha->getIsSecret());
    EXPECT_TRUE(aAlphaLayer->getIsSecret());
    ASSERT_TRUE(setKnobValue(merge, kNativeMergeParamAAlphaSource, kNativeMergeAlphaPerChannel));
    EXPECT_TRUE(aAlpha->getIsSecret());
    EXPECT_FALSE(aAlphaLayer->getIsSecret());
    EXPECT_FALSE(bAlpha->getIsSecret());
    EXPECT_TRUE(bAlphaLayer->getIsSecret());
    ASSERT_TRUE(setKnobValue(merge, kNativeMergeParamAAlphaSource, kNativeMergeAlphaPerLayer));
    EXPECT_FALSE(aAlpha->getIsSecret());
    EXPECT_TRUE(aAlphaLayer->getIsSecret());
}

// A per-channel alpha layer with another channel count than the merged layer fails the render
// with an error naming both.
TEST_F(NativeMergeNodeTest, PerChannelAlphaLayerMismatchIsARenderError)
{
    NodePtr merge = createMerge();
    NodePtr b = createSource(0, 0);
    NodePtr a = createSource(10, 6);
    ASSERT_TRUE(merge && b && a);
    setParitySourceExtraLayer(a, true);
    connectNodes(b, merge, kNativeMergeInputB, true);
    connectNodes(a, merge, kNativeMergeInputA, true);
    ASSERT_TRUE(setKnobValue(merge, kNativeMergeParamAAlphaSource, kNativeMergeAlphaPerChannel));
    setLayerKnob(merge, kNativeMergeParamAAlphaLayer, kParitySourceExtraLayerID);
    const RectI window = paritySourceWindow(b, kTime, 0);
    ASSERT_FALSE(window.isNull());

    std::vector<RenderedPlane> planes;
    std::string error;
    EXPECT_FALSE(renderNodePlanesDirect(merge, kTime, ViewIdx(0), 0, window, std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &planes, &error));
    const QString message = QString::fromUtf8(persistentMessage(merge).c_str());
    EXPECT_TRUE(message.contains(QString::fromUtf8(kParitySourceExtraLayerID))) << message.toStdString();
    EXPECT_TRUE(message.contains(QString::fromUtf8(kNatronColorViewRGBA), Qt::CaseInsensitive)) << message.toStdString();
}

// hue, saturation, color and luminosity blend whole colours, so a per-channel alpha layer on
// either side is a render error naming the operator, withdrawn under a separable one.
TEST_F(NativeMergeNodeTest, NonSeparableOperatorRefusesAPerChannelAlpha)
{
    NodePtr merge = createMerge();
    NodePtr b = createSource(0, 0);
    NodePtr a = createSource(10, 6);
    ASSERT_TRUE(merge && b && a);
    setParitySourceExtraLayer(a, true);
    setParitySourceExtraLayer(b, true);
    connectNodes(b, merge, kNativeMergeInputB, true);
    connectNodes(a, merge, kNativeMergeInputA, true);
    // spec merged alone, A read in its namesake, so that both sides read the alpha layer's
    // channel count.
    setLayers(merge, { kParitySourceExtraLayerID });
    setLayerKnob(merge, kNativeMergeParamAAlphaLayer, kParitySourceExtraLayerID);
    setLayerKnob(merge, kNativeMergeParamBAlphaLayer, kParitySourceExtraLayerID);
    const RectI window = paritySourceWindow(b, kTime, 0);
    ASSERT_FALSE(window.isNull());

    const char* const sides[] = { kNativeMergeParamAAlphaSource, kNativeMergeParamBAlphaSource };
    const char* const operators[] = { "hue", "saturation", "color", "luminosity" };
    for (const char* side : sides) {
        ASSERT_TRUE(setKnobValue(merge, kNativeMergeParamAAlphaSource, kNativeMergeAlphaUniversal));
        ASSERT_TRUE(setKnobValue(merge, kNativeMergeParamBAlphaSource, kNativeMergeAlphaUniversal));
        ASSERT_TRUE(setKnobValue(merge, side, kNativeMergeAlphaPerChannel)) << side;
        for (const char* name : operators) {
            const std::string what = std::string(name) + " with " + side + " per channel";
            ASSERT_TRUE(setKnobValue(merge, kNativeMergeParamOperation, name)) << what;
            std::vector<RenderedPlane> planes;
            std::string error;
            EXPECT_FALSE(renderNodePlanesDirect(merge, kTime, ViewIdx(0), 0, window, std::list<ImageLayerDesc>(1, paritySourceExtraLayer()), &planes, &error)) << what;
            EXPECT_NE(std::string::npos, persistentMessage(merge).find(name)) << what << ": " << persistentMessage(merge);
        }

        ASSERT_TRUE(setKnobValue(merge, kNativeMergeParamOperation, "over"));
        std::vector<float> got;
        EXPECT_TRUE(renderLayer(merge, window, paritySourceExtraLayer(), &got)) << side;
        EXPECT_TRUE(persistentMessage(merge).empty()) << side << ": " << persistentMessage(merge);
    }

    // A non-separable operator takes a per-layer alpha.
    ASSERT_TRUE(setKnobValue(merge, kNativeMergeParamAAlphaSource, kNativeMergeAlphaPerLayer));
    ASSERT_TRUE(setKnobValue(merge, kNativeMergeParamBAlphaSource, kNativeMergeAlphaPerLayer));
    ASSERT_TRUE(setKnobValue(merge, kNativeMergeParamOperation, "hue"));
    std::vector<float> got;
    EXPECT_TRUE(renderLayer(merge, window, paritySourceExtraLayer(), &got));
    EXPECT_TRUE(persistentMessage(merge).empty()) << persistentMessage(merge);
}

// Merge has no host layer knob: Layers, defaulting to rgba, names the merged layers, and A Layer
// defaults to Matching. The knobs of other layer models are gone.
TEST_F(NativeMergeNodeTest, LayersReplaceTheHostLayerKnob)
{
    NodePtr merge = createMerge();
    ASSERT_TRUE(bool(merge));
    EffectInstancePtr effect = merge->getEffectInstance();
    EXPECT_FALSE(bool(merge->getLayerKnob()));
    KnobChannelSetPtr layers = effect->getKnobByNameAndType<KnobChannelSet>(kNativeMergeParamLayers);
    ASSERT_TRUE(bool(layers));
    const std::vector<ChannelSetRow> rows = layers->getRows();
    ASSERT_EQ(1u, rows.size());
    EXPECT_EQ(ChannelSetRow::eModeLayer, rows[0].mode);
    EXPECT_EQ(std::string(kNatronColorViewRGBA), rows[0].layerOrPattern);
    KnobLayerSelectPtr aLayer = effect->getKnobByNameAndType<KnobLayerSelect>(kNativeMergeParamALayer);
    ASSERT_TRUE(bool(aLayer));
    EXPECT_EQ(std::string(kNatronLayerSelectAll), aLayer->getLayer());
    EXPECT_EQ(std::string("Matching"), aLayer->getSummary());
    for (const char* gone : { "bLayer", "alsoMerge", "output", "bbox" }) {
        EXPECT_FALSE(bool(merge->getKnobByName(gone))) << gone;
    }
    EXPECT_TRUE(bool(merge->getKnobByName(kNativeMergeParamSetBBox)));
}

// (a) A over B in rgba, the default: the result is the fold in rgba, and a layer of B Layers
// leaves out is not asked of A and passes through bit-exact.
TEST_F(NativeMergeNodeTest, AOverBInRGBAPassesTheOtherLayersThrough)
{
    NodePtr merge = createMerge();
    NodePtr b = createSource(0, 0);
    NodePtr a = createSource(10, 6);
    ASSERT_TRUE(merge && b && a);
    setParitySourceExtraLayer(a, true);
    setParitySourceExtraLayer(b, true);
    connectNodes(b, merge, kNativeMergeInputB, true);
    connectNodes(a, merge, kNativeMergeInputA, true);
    EXPECT_FALSE(hasPlane(neededPlanes(merge, -1), kParitySourceExtraLayerID));
    EXPECT_FALSE(hasPlane(neededPlanes(merge, kNativeMergeInputA), kParitySourceExtraLayerID));
    const RectI window = paritySourceWindow(b, kTime, 0);
    ASSERT_FALSE(window.isNull());

    std::vector<float> aRGBA, bRGBA, bSpec, gotRGBA, gotSpec;
    ASSERT_TRUE(renderRGBA(a, window, &aRGBA));
    ASSERT_TRUE(renderRGBA(b, window, &bRGBA));
    ASSERT_TRUE(renderLayer(b, window, paritySourceExtraLayer(), &bSpec));
    ASSERT_TRUE(renderRGBA(merge, window, &gotRGBA));
    ASSERT_TRUE(renderLayer(merge, window, paritySourceExtraLayer(), &gotSpec));
    expectClose(expectedFold(MergeOps::eOver, bRGBA, { &aRGBA }), gotRGBA, kTolerance, "A over B in rgba");
    expectClose(bSpec, gotSpec, 0.f, "spec passes through from B");
}

// (b) mask with Layers All multiplies every layer of B by A's rgba.A, spec too, which A lacks:
// there A reads zero but keeps its alpha.
TEST_F(NativeMergeNodeTest, MaskWithAllLayersMultipliesEveryLayerOfB)
{
    NodePtr merge = createMerge();
    NodePtr b = createSource(0, 0);
    NodePtr a = createSource(10, 6);
    ASSERT_TRUE(merge && b && a);
    setParitySourceExtraLayer(b, true);
    connectNodes(b, merge, kNativeMergeInputB, true);
    connectNodes(a, merge, kNativeMergeInputA, true);
    setLayers(merge, {});
    ASSERT_TRUE(setKnobValue(merge, kNativeMergeParamOperation, "mask"));
    EXPECT_TRUE(hasPlane(neededPlanes(merge, -1), kParitySourceExtraLayerID));
    const RectI window = paritySourceWindow(b, kTime, 0);
    ASSERT_FALSE(window.isNull());

    std::vector<float> aRGBA, bRGBA, bSpec, gotRGBA, gotSpec;
    ASSERT_TRUE(renderRGBA(a, window, &aRGBA));
    ASSERT_TRUE(renderRGBA(b, window, &bRGBA));
    ASSERT_TRUE(renderLayer(b, window, paritySourceExtraLayer(), &bSpec));
    ASSERT_TRUE(renderRGBA(merge, window, &gotRGBA));
    ASSERT_TRUE(renderLayer(merge, window, paritySourceExtraLayer(), &gotSpec));
    expectClose(expectedFold(MergeOps::eMask, bRGBA, { &aRGBA }), gotRGBA, kTolerance, "rgba masked");
    expectClose(expectedLayerMerge(MergeOps::eMask, std::vector<float>(), aRGBA, bSpec, bRGBA), gotSpec, kTolerance, "spec masked");
    EXPECT_NE(bSpec, gotSpec) << "spec is unchanged, which proves nothing";
}

// (c) stencil with Layers spec, diffuse and albedo changes exactly those layers of B; normals and
// rgba, which Layers leaves out, pass through bit-exact.
TEST_F(NativeMergeNodeTest, StencilChangesExactlyTheListedLayers)
{
    addProjectLayers();
    NodePtr merge = createMerge();
    NodePtr b = createLayeredSource(0, 0);
    NodePtr a = createSource(10, 6);
    ASSERT_TRUE(merge && b && a);
    connectNodes(b, merge, kNativeMergeInputB, true);
    connectNodes(a, merge, kNativeMergeInputA, true);
    setLayers(merge, { kParitySourceExtraLayerID, "diffuse", "albedo" });
    ASSERT_TRUE(setKnobValue(merge, kNativeMergeParamOperation, "stencil"));
    const std::list<ImageLayerDesc> produced = neededPlanes(merge, -1);
    EXPECT_TRUE(hasPlane(produced, kParitySourceExtraLayerID));
    EXPECT_TRUE(hasPlane(produced, "diffuse"));
    EXPECT_TRUE(hasPlane(produced, "albedo"));
    EXPECT_FALSE(hasPlane(produced, "normals"));
    const RectI window = paritySourceWindow(b, kTime, 0);
    ASSERT_FALSE(window.isNull());

    std::vector<float> aRGBA, bRGBA, gotRGBA;
    ASSERT_TRUE(renderRGBA(a, window, &aRGBA));
    ASSERT_TRUE(renderRGBA(b, window, &bRGBA));
    for (const char* id : { kParitySourceExtraLayerID, "diffuse", "albedo" }) {
        const ImageLayerDesc layer = (std::string(id) == kParitySourceExtraLayerID) ? paritySourceExtraLayer() : projectLayer(id);
        std::vector<float> bLayer, got;
        ASSERT_TRUE(renderLayer(b, window, layer, &bLayer)) << id;
        ASSERT_TRUE(renderLayer(merge, window, layer, &got)) << id;
        expectClose(expectedLayerMerge(MergeOps::eStencil, std::vector<float>(), aRGBA, bLayer, bRGBA), got, kTolerance, std::string(id) + " stencilled");
        EXPECT_NE(bLayer, got) << id << " is unchanged";
    }
    std::vector<float> bNormals, gotNormals;
    ASSERT_TRUE(renderLayer(b, window, projectLayer("normals"), &bNormals));
    ASSERT_TRUE(renderLayer(merge, window, projectLayer("normals"), &gotNormals));
    expectClose(bNormals, gotNormals, 0.f, "normals pass through from B");
    ASSERT_TRUE(renderRGBA(merge, window, &gotRGBA));
    expectClose(bRGBA, gotRGBA, 0.f, "rgba passes through from B");
}

// (d) A Layer rgba merges into B's diffuse, Layers listing diffuse alone: A's R, G and B over
// diffuse, and B's rgba passes through.
TEST_F(NativeMergeNodeTest, ALayerRGBAMergesIntoDiffuse)
{
    addProjectLayers();
    NodePtr merge = createMerge();
    NodePtr b = createLayeredSource(0, 0);
    NodePtr a = createSource(10, 6);
    ASSERT_TRUE(merge && b && a);
    connectNodes(b, merge, kNativeMergeInputB, true);
    connectNodes(a, merge, kNativeMergeInputA, true);
    setLayers(merge, { "diffuse" });
    setLayerKnob(merge, kNativeMergeParamALayer, kNatronColorViewRGBA);
    const RectI window = paritySourceWindow(b, kTime, 0);
    ASSERT_FALSE(window.isNull());

    std::vector<float> aRGBA, bRGBA, bDiffuse, gotDiffuse, gotRGBA;
    ASSERT_TRUE(renderRGBA(a, window, &aRGBA));
    ASSERT_TRUE(renderRGBA(b, window, &bRGBA));
    ASSERT_TRUE(renderLayer(b, window, projectLayer("diffuse"), &bDiffuse));
    ASSERT_TRUE(renderLayer(merge, window, projectLayer("diffuse"), &gotDiffuse));
    ASSERT_TRUE(renderRGBA(merge, window, &gotRGBA));
    expectClose(expectedLayerMerge(MergeOps::eOver, rgbOf(aRGBA), aRGBA, bDiffuse, bRGBA), gotDiffuse, kTolerance, "A rgba over B diffuse");
    expectClose(bRGBA, gotRGBA, 0.f, "rgba passes through from B");
}

// (e) Per-layer alphas: each merged layer takes its own alpha channel on each side, and a layer
// without one, spec, the universal channel, rgba.R, so that the two differ.
TEST_F(NativeMergeNodeTest, PerLayerAlphaTakesEachLayersOwnAlpha)
{
    NodePtr merge = createMerge();
    NodePtr b = createSource(0, 0);
    NodePtr a = createSource(10, 6);
    ASSERT_TRUE(merge && b && a);
    setParitySourceExtraLayer(a, true);
    setParitySourceExtraLayer(b, true);
    connectNodes(b, merge, kNativeMergeInputB, true);
    connectNodes(a, merge, kNativeMergeInputA, true);
    setLayers(merge, { kNatronColorViewRGBA, kParitySourceExtraLayerID });
    const std::string rgbaR = std::string(kNatronColorViewRGBA) + ".R";
    setChannelKnob(merge, kNativeMergeParamAAlpha, rgbaR);
    setChannelKnob(merge, kNativeMergeParamBAlpha, rgbaR);
    ASSERT_TRUE(setKnobValue(merge, kNativeMergeParamAAlphaSource, kNativeMergeAlphaPerLayer));
    ASSERT_TRUE(setKnobValue(merge, kNativeMergeParamBAlphaSource, kNativeMergeAlphaPerLayer));
    const RectI window = paritySourceWindow(b, kTime, 0);
    ASSERT_FALSE(window.isNull());

    std::vector<float> aRGBA, aSpec, bRGBA, bSpec, gotRGBA, gotSpec;
    ASSERT_TRUE(renderRGBA(a, window, &aRGBA));
    ASSERT_TRUE(renderLayer(a, window, paritySourceExtraLayer(), &aSpec));
    ASSERT_TRUE(renderRGBA(b, window, &bRGBA));
    ASSERT_TRUE(renderLayer(b, window, paritySourceExtraLayer(), &bSpec));
    ASSERT_TRUE(renderRGBA(merge, window, &gotRGBA));
    ASSERT_TRUE(renderLayer(merge, window, paritySourceExtraLayer(), &gotSpec));
    expectClose(expectedFold(MergeOps::eOver, bRGBA, { &aRGBA }), gotRGBA, kTolerance, "rgba with rgba.A");

    std::vector<float> want(bSpec.size());
    for (std::size_t p = 0; (p + 1) * 3 <= want.size(); ++p) {
        const float aAlpha = aRGBA[p * 4];
        const float bAlpha = bRGBA[p * 4];
        const MergeOps::AlphaSpan aSpan = { &aAlpha, 0, 0 };
        const MergeOps::AlphaSpan bSpan = { &bAlpha, 0, 0 };
        MergeOps::mergeRow(MergeOps::eOver, 3, &aSpec[p * 3], aSpan, &bSpec[p * 3], bSpan, 1, &want[p * 3]);
    }
    expectClose(want, gotSpec, kTolerance, "spec with rgba.R");
}

// (f) Per-channel alphas: diffuse merged with each channel's alpha from the same channel of each
// side's albedo layer.
TEST_F(NativeMergeNodeTest, PerChannelAlphaTakesEachChannelsAlpha)
{
    addProjectLayers();
    NodePtr merge = createMerge();
    NodePtr b = createLayeredSource(0, 0);
    NodePtr a = createLayeredSource(10, 6);
    ASSERT_TRUE(merge && b && a);
    connectNodes(b, merge, kNativeMergeInputB, true);
    connectNodes(a, merge, kNativeMergeInputA, true);
    setLayers(merge, { "diffuse" });
    ASSERT_TRUE(setKnobValue(merge, kNativeMergeParamAAlphaSource, kNativeMergeAlphaPerChannel));
    ASSERT_TRUE(setKnobValue(merge, kNativeMergeParamBAlphaSource, kNativeMergeAlphaPerChannel));
    setLayerKnob(merge, kNativeMergeParamAAlphaLayer, "albedo");
    setLayerKnob(merge, kNativeMergeParamBAlphaLayer, "albedo");
    const RectI window = paritySourceWindow(b, kTime, 0);
    ASSERT_FALSE(window.isNull());

    std::vector<float> aDiffuse, aAlbedo, bDiffuse, bAlbedo, got;
    ASSERT_TRUE(renderLayer(a, window, projectLayer("diffuse"), &aDiffuse));
    ASSERT_TRUE(renderLayer(a, window, projectLayer("albedo"), &aAlbedo));
    ASSERT_TRUE(renderLayer(b, window, projectLayer("diffuse"), &bDiffuse));
    ASSERT_TRUE(renderLayer(b, window, projectLayer("albedo"), &bAlbedo));
    ASSERT_TRUE(renderLayer(merge, window, projectLayer("diffuse"), &got));
    std::vector<float> want(bDiffuse.size());
    for (std::size_t p = 0; (p + 1) * 3 <= want.size(); ++p) {
        const MergeOps::AlphaSpan aSpan = { &aAlbedo[p * 3], 3, 1 };
        const MergeOps::AlphaSpan bSpan = { &bAlbedo[p * 3], 3, 1 };
        MergeOps::mergeRow(MergeOps::eOver, 3, &aDiffuse[p * 3], aSpan, &bDiffuse[p * 3], bSpan, 1, &want[p * 3]);
    }
    expectClose(want, got, kTolerance, "diffuse with albedo alphas");
}

// A per-channel B alpha makes its alpha layer an output: A's alpha layer merged over B's, each
// channel its own alpha.
TEST_F(NativeMergeNodeTest, PerChannelAlphaWritesTheAlphaLayer)
{
    NodePtr merge = createMerge();
    NodePtr b = createSource(0, 0);
    NodePtr a = createSource(10, 6);
    ASSERT_TRUE(merge && b && a);
    setParitySourceComponents(a, "rgb");
    setParitySourceComponents(b, "rgb");
    setParitySourceExtraLayer(a, true);
    setParitySourceExtraLayer(b, true);
    connectNodes(b, merge, kNativeMergeInputB, true);
    connectNodes(a, merge, kNativeMergeInputA, true);
    ASSERT_TRUE(setKnobValue(merge, kNativeMergeParamAAlphaSource, kNativeMergeAlphaPerChannel));
    ASSERT_TRUE(setKnobValue(merge, kNativeMergeParamBAlphaSource, kNativeMergeAlphaPerChannel));
    setLayerKnob(merge, kNativeMergeParamAAlphaLayer, kParitySourceExtraLayerID);
    EXPECT_FALSE(hasPlane(neededPlanes(merge, -1), kParitySourceExtraLayerID)) << "B's alpha layer is unset";

    setLayerKnob(merge, kNativeMergeParamBAlphaLayer, kParitySourceExtraLayerID);
    EXPECT_TRUE(hasPlane(neededPlanes(merge, -1), kParitySourceExtraLayerID));
    const RectI window = paritySourceWindow(b, kTime, 0);
    ASSERT_FALSE(window.isNull());

    std::vector<float> aSpec, bSpec, got;
    ASSERT_TRUE(renderLayer(a, window, paritySourceExtraLayer(), &aSpec));
    ASSERT_TRUE(renderLayer(b, window, paritySourceExtraLayer(), &bSpec));
    ASSERT_TRUE(renderLayer(merge, window, paritySourceExtraLayer(), &got));
    std::vector<float> want(bSpec.size());
    for (std::size_t p = 0; (p + 1) * 3 <= want.size(); ++p) {
        const MergeOps::AlphaSpan aSpan = { &aSpec[p * 3], 3, 1 };
        const MergeOps::AlphaSpan bSpan = { &bSpec[p * 3], 3, 1 };
        MergeOps::mergeRow(MergeOps::eOver, 3, &aSpec[p * 3], aSpan, &bSpec[p * 3], bSpan, 1, &want[p * 3]);
    }
    expectClose(want, got, kTolerance, "spec alpha over spec alpha");
}

// B's per-channel alpha layer, which Layers leaves out, is written the alpha the fold carries,
// whatever A's alpha source: a universal or per-layer A alpha a, broadcast, merges with each of
// B's alphas by the operator, so over writes a + b * (1 - a). A Layer is rgba, so A is read in
// rgba there and whether A carries the alpha layer changes nothing. The universal channel is
// rgba.R, so that it differs from rgba's own alpha.
TEST_F(NativeMergeNodeTest, PerChannelBAlphaLayerTakesTheCarriedAlpha)
{
    struct Config {
        const char* aSource;
        bool aHasSpec;
    };
    const Config configs[] = {
        { kNativeMergeAlphaUniversal, true },
        { kNativeMergeAlphaUniversal, false },
        { kNativeMergeAlphaPerLayer, true },
        { kNativeMergeAlphaPerLayer, false },
    };
    const MergeOps::Operator operators[] = { MergeOps::eOver, MergeOps::ePlus, MergeOps::eMultiply, MergeOps::eXor };

    for (const Config& config : configs) {
        NodePtr merge = createMerge();
        NodePtr b = createSource(0, 0);
        NodePtr a = createSource(10, 6);
        ASSERT_TRUE(merge && b && a);
        setParitySourceComponents(b, "rgb");
        setParitySourceExtraLayer(b, true);
        setParitySourceExtraLayer(a, config.aHasSpec);
        connectNodes(b, merge, kNativeMergeInputB, true);
        connectNodes(a, merge, kNativeMergeInputA, true);
        setChannelKnob(merge, kNativeMergeParamAAlpha, std::string(kNatronColorViewRGBA) + ".R");
        setLayerKnob(merge, kNativeMergeParamALayer, kNatronColorViewRGBA);
        ASSERT_TRUE(setKnobValue(merge, kNativeMergeParamAAlphaSource, config.aSource));
        ASSERT_TRUE(setKnobValue(merge, kNativeMergeParamBAlphaSource, kNativeMergeAlphaPerChannel));
        setLayerKnob(merge, kNativeMergeParamBAlphaLayer, kParitySourceExtraLayerID);
        EXPECT_TRUE(hasPlane(neededPlanes(merge, -1), kParitySourceExtraLayerID));
        const RectI window = paritySourceWindow(b, kTime, 0);
        ASSERT_FALSE(window.isNull());

        std::vector<float> aRGBA, bSpec;
        ASSERT_TRUE(renderRGBA(a, window, &aRGBA));
        ASSERT_TRUE(renderLayer(b, window, paritySourceExtraLayer(), &bSpec));
        const bool universal = (std::string(config.aSource) == kNativeMergeAlphaUniversal);
        for (MergeOps::Operator op : operators) {
            const std::string what = std::string(config.aSource) + (config.aHasSpec ? " A with spec, " : " A without spec, ") + MergeOps::operatorName(op);
            ASSERT_TRUE(setKnobValue(merge, kNativeMergeParamOperation, MergeOps::operatorName(op))) << what;
            std::vector<float> got;
            ASSERT_TRUE(renderLayer(merge, window, paritySourceExtraLayer(), &got)) << what;

            std::vector<float> want(bSpec.size());
            for (std::size_t p = 0; (p + 1) * 3 <= want.size(); ++p) {
                const float a1 = aRGBA[p * 4 + (universal ? 0 : 3)];
                const float aAlpha[3] = { a1, a1, a1 };
                const MergeOps::AlphaSpan aSpan = { aAlpha, 3, 1 };
                const MergeOps::AlphaSpan bSpan = { &bSpec[p * 3], 3, 1 };
                MergeOps::mergeRow(op, 3, aAlpha, aSpan, &bSpec[p * 3], bSpan, 1, &want[p * 3]);
            }
            expectClose(want, got, kTolerance, what);
        }
    }
}

// An A input lacking A's layer has zero pixels there but keeps its alpha: under over it still
// attenuates the result so far by (1 - a), as an A input carrying the layer does. Only a
// disconnected input reads zero alpha too.
TEST_F(NativeMergeNodeTest, AnAInputLackingTheALayerKeepsItsAlpha)
{
    NodePtr merge = createMerge();
    NodePtr b = createSource(0, 0);
    NodePtr a = createSource(10, 6);
    NodePtr a2 = createSource(1, 2);
    ASSERT_TRUE(merge && b && a && a2);
    setParitySourceExtraLayer(a, true);
    connectNodes(b, merge, kNativeMergeInputB, true);
    connectNodes(a, merge, NativeMerge::aInput(0), true);
    connectNodes(a2, merge, NativeMerge::aInput(1), true);
    setLayerKnob(merge, kNativeMergeParamALayer, kParitySourceExtraLayerID);
    const RectI window = paritySourceWindow(b, kTime, 0);
    ASSERT_FALSE(window.isNull());

    std::vector<float> bRGBA, aRGBA, aSpec, a2RGBA;
    ASSERT_TRUE(renderRGBA(b, window, &bRGBA));
    ASSERT_TRUE(renderRGBA(a, window, &aRGBA));
    ASSERT_TRUE(renderLayer(a, window, paritySourceExtraLayer(), &aSpec));
    ASSERT_TRUE(renderRGBA(a2, window, &a2RGBA));
    bool a2HasAlpha = false;
    for (std::size_t p = 0; (p + 1) * 4 <= a2RGBA.size(); ++p) {
        a2HasAlpha = a2HasAlpha || (a2RGBA[p * 4 + 3] != 0.f);
    }
    ASSERT_TRUE(a2HasAlpha) << "A2's alpha is zero over the window, which proves nothing";

    for (const char* name : { "over", "xor", "plus" }) {
        MergeOps::Operator op = MergeOps::eOver;
        ASSERT_TRUE(MergeOps::operatorFromName(name, &op)) << name;
        const MergeOps::Operator alphaOp = MergeOps::isSeparable(op) ? op : MergeOps::eOver;
        ASSERT_TRUE(setKnobValue(merge, kNativeMergeParamOperation, name)) << name;
        std::vector<float> got;
        ASSERT_TRUE(renderRGBA(merge, window, &got)) << name;

        std::vector<float> want(bRGBA.size());
        for (std::size_t p = 0; (p + 1) * 4 <= want.size(); ++p) {
            float result[4];
            float resultAlpha[4];
            for (int c = 0; c < 4; ++c) {
                result[c] = bRGBA[p * 4 + c];
                resultAlpha[c] = bRGBA[p * 4 + 3];
            }
            // A in spec, its fourth channel reading zero, then A2, which lacks spec, as zero.
            const float aPixel[4] = { aSpec[p * 3], aSpec[p * 3 + 1], aSpec[p * 3 + 2], 0.f };
            const float zero[4] = { 0.f, 0.f, 0.f, 0.f };
            const float* pixels[2] = { aPixel, zero };
            const float alphas[2] = { aRGBA[p * 4 + 3], a2RGBA[p * 4 + 3] };
            for (int k = 0; k < 2; ++k) {
                const float aAlpha[4] = { alphas[k], alphas[k], alphas[k], alphas[k] };
                const MergeOps::AlphaSpan aSpan = { aAlpha, 4, 1 };
                const MergeOps::AlphaSpan rSpan = { resultAlpha, 4, 1 };
                float merged[4];
                float mergedAlpha[4];
                MergeOps::mergeRow(op, 4, pixels[k], aSpan, result, rSpan, 1, merged);
                MergeOps::mergeRow(alphaOp, 4, aAlpha, aSpan, resultAlpha, rSpan, 1, mergedAlpha);
                std::copy(merged, merged + 4, result);
                std::copy(mergedAlpha, mergedAlpha + 4, resultAlpha);
            }
            std::copy(result, result + 4, &want[p * 4]);
        }
        expectClose(want, got, kTolerance, name);
    }
}

// Another staged composite node declares no produced planes: its planes are only its layer
// knob's, whatever layers its inputs carry. Dissolve's knob selects every layer by default, so it
// is narrowed to rgba, and it is between its inputs, so it is no pass-through.
TEST_F(NativeMergeNodeTest, OtherCompositeNodesProduceNoExtraPlanes)
{
    NodePtr dissolve = createNode(QString::fromUtf8(PLUGINID_NATRON_DISSOLVE), PLUGIN_MAJOR_NATRON_DISSOLVE);
    NodePtr first = createSource(0, 0);
    NodePtr second = createSource(10, 6);
    ASSERT_TRUE(dissolve && first && second);
    setParitySourceExtraLayer(first, true);
    setParitySourceExtraLayer(second, true);
    connectNodes(first, dissolve, dissolveSourceInput(0), true);
    connectNodes(second, dissolve, dissolveSourceInput(1), true);
    KnobDoublePtr which = dissolve->getEffectInstance()->getKnobByNameAndType<KnobDouble>(kDissolveParamWhich);
    ASSERT_TRUE(bool(which));
    which->setValue(0.5);
    KnobChannelSetPtr channels = std::dynamic_pointer_cast<KnobChannelSet>(dissolve->getLayerKnob());
    ASSERT_TRUE(bool(channels));
    std::vector<ChannelSetRow> rows(1);
    rows[0].mode = ChannelSetRow::eModeLayer;
    rows[0].layerOrPattern = kNatronColorViewRGBA;
    channels->setRows(rows);

    const std::list<ImageLayerDesc> output = neededPlanes(dissolve, -1);
    EXPECT_FALSE(output.empty());
    EXPECT_FALSE(hasPlane(output, kParitySourceExtraLayerID));
    EXPECT_FALSE(hasPlane(neededPlanes(dissolve, dissolveSourceInput(0)), kParitySourceExtraLayerID));
    EXPECT_FALSE(hasPlane(neededPlanes(dissolve, dissolveSourceInput(1)), kParitySourceExtraLayerID));
}
