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

#include <functional>
#include <iostream>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include <ofxNatron.h>

#include "BaseTest.h"
#include "CountingTestEffect.h"
#include "NativeParity.h"
#include "NativePluginIdentity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Merge/Dissolve.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kDissolveID = PLUGINID_NATRON_DISSOLVE;
const int kNativeDissolveMajor = PLUGIN_MAJOR_NATRON_DISSOLVE;
const double kTime = 1.;

// Each pixel is one lerp or one masked mix of float arithmetic, evaluated in the same order.
const ParityTolerance kDissolveTolerance = ParityTolerance::arithmetic();

bool
isNative(const NodePtr& node)
{
    return node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
}

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
    select->set(value);

    return true;
}

// The three sources' union and a margin, so clipping to the region of definition is compared too.
RectI
caseWindow(unsigned mipmapLevel)
{
    return RectD(-12., -4., 76., 62.).toPixelEnclosing(mipmapLevel, 1.);
}

} // namespace

class NativeDissolveTest
    : public BaseTest {
protected:
    void resetProject()
    {
        getApp()->getProject()->reset(false, true);
    }

    // The pair with `inputs` sources connected to "0".."inputs-1", each at its own origin so that
    // the images and regions of definition differ.
    ParityPair makePair(int inputs,
                        bool withMask = false)
    {
        ParityPair pair = makeParityPair(getApp(), kDissolveID, kNativeDissolveMajor, withMask ? std::string("Mask") : std::string());

        EXPECT_TRUE(bool(pair.native));
        EXPECT_TRUE(isNative(pair.native));
        if (pair.source) {
            setParitySourceOrigin(pair.source, 0, 0);
        }
        const int origins[3][2] = { { 0, 0 }, { 8, 4 }, { -6, 10 } };
        for (int k = 1; k < inputs; ++k) {
            NodePtr source = connectParityInput(pair, std::to_string(k));
            EXPECT_TRUE(bool(source)) << k;
            if (source) {
                setParitySourceOrigin(source, origins[k][0], origins[k][1]);
            }
        }

        return pair;
    }

    void expectParity(const ParityPair& pair,
                      const std::string& caseName,
                      bool record)
    {
        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            const ParityResult r = compareParity(pair, caseName, caseWindow(mipmapLevel), mipmapLevel, kDissolveTolerance, record);
            EXPECT_TRUE(r.ok) << caseName << ", mipmap " << mipmapLevel << ": " << describe(r);
            EXPECT_GE(r.planesCompared, 1) << caseName;
            std::cout << "[ parity ] Dissolve " << caseName << " mipmap " << mipmapLevel << ": planes " << r.planesCompared
                      << ", max abs diff " << r.maxAbsDiff << std::endl;
        }
    }

    NodePtr createCountingLeaf(double value)
    {
        NodePtr node = createNode(QString::fromUtf8(kTestPluginIDCounting));

        if (!node || !setKnobValues(node, "value", { value })) {
            return NodePtr();
        }

        return node;
    }
};

TEST_F(NativeDissolveTest, InputsKeepTheOfxOrder)
{
    resetProject();
    NodePtr node = createNode(QString::fromUtf8(kDissolveID), kNativeDissolveMajor);
    ASSERT_TRUE(isNative(node));
    // An inspector node, as the OpenFX Dissolve is, shows only the next free input in the node graph.
    EXPECT_TRUE(dynamic_cast<InspectorNode*>(node.get()) != NULL);

    EffectInstancePtr effect = node->getEffectInstance();
    ASSERT_EQ(kDissolveSourceCount + 1, effect->getNInputs());
    EXPECT_EQ("0", effect->getInputLabel(0));
    EXPECT_EQ("1", effect->getInputLabel(1));
    EXPECT_EQ("Mask", effect->getInputLabel(kDissolveMaskInput));
    EXPECT_EQ("2", effect->getInputLabel(3));
    EXPECT_EQ("63", effect->getInputLabel(kDissolveSourceCount));
    for (int i = 0; i <= kDissolveSourceCount; ++i) {
        EXPECT_TRUE(effect->isInputOptional(i)) << i;
        EXPECT_EQ(i == kDissolveMaskInput, effect->isInputMask(i)) << i;
    }
}

TEST_F(NativeDissolveTest, WhichBetweenZeroAndOne)
{
    resetProject();
    ParityPair pair = makePair(3);
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(setKnobOnBoth(pair, kDissolveParamWhich, { 0.3 }));
    expectParity(pair, "which-0.3", true);
}

TEST_F(NativeDissolveTest, WhichTwoAndAHalfIsClampedToTheLastInput)
{
    resetProject();
    ParityPair pair = makePair(3);
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(setKnobOnBoth(pair, kDissolveParamWhich, { 2.5 }));
    expectParity(pair, "which-2.5", true);
}

TEST_F(NativeDissolveTest, MaskedBlend)
{
    resetProject();
    ParityPair pair = makePair(2, true);
    ASSERT_TRUE(bool(pair.mask));
    setParitySourceOrigin(pair.mask, 12, 6);
    ASSERT_TRUE(setKnobOnBoth(pair, "enableMask_Mask", { 1. }));
    ASSERT_TRUE(setChannelSelect(pair.native, "maskChannel_Mask", "rgba.A"));
    ASSERT_TRUE(setKnobOnBoth(pair, kDissolveParamWhich, { 0.4 }));
    expectParity(pair, "masked", true);
}

TEST_F(NativeDissolveTest, DisplayRangeFollowsTheHighestConnectedInput)
{
    resetProject();
    NodePtr node = createNode(QString::fromUtf8(kDissolveID), kNativeDissolveMajor);
    ASSERT_TRUE(isNative(node));
    KnobDouble* which = dynamic_cast<KnobDouble*>(node->getKnobByName(kDissolveParamWhich).get());
    ASSERT_TRUE(which != NULL);
    EXPECT_EQ(0., which->getMinimum(0));
    EXPECT_EQ(63., which->getMaximum(0));
    EXPECT_EQ(1., which->getDisplayMaximum(0));

    NodePtr leaf = createCountingLeaf(1.);
    ASSERT_TRUE(bool(leaf));
    connectNodes(leaf, node, Dissolve::sourceInput(5), true);
    EXPECT_EQ(5., which->getDisplayMaximum(0));
}

TEST_F(NativeDissolveTest, UnversionedRequestsGetTheNativeDissolve)
{
    NodePtr unversioned = createNode(QString::fromUtf8(kDissolveID));
    ASSERT_TRUE(bool(unversioned));
    EXPECT_TRUE(isNative(unversioned));
    EXPECT_EQ(kNativeDissolveMajor, unversioned->getMajorVersion());
}

TEST_F(NativeDissolveTest, OnlyTheTwoChosenInputsAreRendered)
{
    resetProject();
    NodePtr dissolve = createNode(QString::fromUtf8(kDissolveID), kNativeDissolveMajor);
    ASSERT_TRUE(isNative(dissolve));
    NodePtr leaves[3];
    const double values[3] = { 10., 20., 30. };
    for (int k = 0; k < 3; ++k) {
        leaves[k] = createCountingLeaf(values[k]);
        ASSERT_TRUE(bool(leaves[k]));
        connectNodes(leaves[k], dissolve, Dissolve::sourceInput(k), true);
    }
    ASSERT_TRUE(setKnobValues(dissolve, kDissolveParamWhich, { 0.3 }));

    CountingTestRegistry::reset();
    std::vector<int> poolSizes;
    poolSizes.push_back(1);
    poolSizes.push_back(4);
    for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
        std::vector<int> unplannedPulls;
        const RectI window = RectD(0., 0., kCountingTestSize, kCountingTestSize).toPixelEnclosing(mipmapLevel, 1.);
        const RenderMismatch m = renderBothWaysDirect(dissolve, kTime, ViewIdx(0), mipmapLevel, window, poolSizes, std::function<void()>(), 0.f, &unplannedPulls);
        EXPECT_FALSE(m.any) << "mipmap " << mipmapLevel << ": " << describe(m);
        for (std::size_t p = 0; p < unplannedPulls.size(); ++p) {
            EXPECT_EQ(0, unplannedPulls[p]) << "mipmap " << mipmapLevel << ", pool " << poolSizes[p];
        }
    }
    EXPECT_GT(CountingTestRegistry::renders(leaves[0]), 0);
    EXPECT_GT(CountingTestRegistry::renders(leaves[1]), 0);
    EXPECT_EQ(0, CountingTestRegistry::renders(leaves[2]));
}

TEST_F(NativeDissolveTest, RegisteredIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_DISSOLVE, PLUGIN_MAJOR_NATRON_DISSOLVE, "Dissolve");
}

namespace {

// A native Constant whose Extent is the entry extentId.
NodePtr
createExtentConstant(const AppInstancePtr& app,
                     const char* extentId)
{
    NodePtr node = createNodeAtMajor(app, PLUGINID_NATRON_CONSTANT, PLUGIN_MAJOR_NATRON_CONSTANT);
    KnobChoice* extent = node ? dynamic_cast<KnobChoice*>(node->getKnobByName(kNativeGeneratorParamExtent).get()) : NULL;

    EXPECT_TRUE(extent != NULL);
    if (!extent) {
        return NodePtr();
    }
    const std::vector<ChoiceOption> entries = extent->getEntries_mt_safe();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].id == extentId) {
            extent->setValue((int)i, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);
        }
    }

    return node;
}

// A native Constant whose output format is width x height at par.
NodePtr
createFormatConstant(const AppInstancePtr& app,
                     double width,
                     double height,
                     double par)
{
    NodePtr node = createExtentConstant(app, kNativeGeneratorExtentFormat);

    if (!node || !setKnobValues(node, kNatronParamFormatSize, { width, height }) || !setKnobValues(node, kNatronParamFormatPar, { par })) {
        return NodePtr();
    }

    return node;
}

// A native Constant of colour rgba over box.
NodePtr
createBoxConstant(const AppInstancePtr& app,
                  const RectD& box,
                  const std::vector<double>& rgba)
{
    NodePtr node = createExtentConstant(app, kNativeGeneratorExtentSize);

    if (!node || !setKnobValues(node, kNativeGeneratorParamBottomLeft, { box.x1, box.y1 }) || !setKnobValues(node, kNativeGeneratorParamSize, { box.width(), box.height() }) || !setKnobValues(node, kConstantParamColor, rgba)) {
        return NodePtr();
    }

    return node;
}

// The RGBA of node over window at time and full scale, row-major from the bottom row.
bool
renderRGBA(const NodePtr& node,
           double time,
           const RectI& window,
           std::vector<float>* pixels)
{
    std::list<ImageLayerDesc> layers(1, ImageLayerDesc::mapNCompsToColorLayer(4));
    std::vector<RenderedPlane> planes;
    std::string error;

    if (!renderNodePlanesDirect(node, time, ViewIdx(0), 0, window, layers, &planes, &error)) {
        ADD_FAILURE() << error;

        return false;
    }
    if ((planes.size() != 1) || (planes[0].channels.size() != 4)) {
        ADD_FAILURE() << "expected one RGBA plane";

        return false;
    }
    *pixels = planes[0].pixels;

    return true;
}

const double kFromColor[4] = { 0.2, 0.4, 0.6, 1. };
const double kToColor[4] = { 1., 0.5, 0., 0.5 };

} // namespace

// Between two sources a Dissolve routes nowhere, so it keeps the derivation from its inputs (the
// first source's format and PAR) rather than the project format a black route answers. On a
// whole number it passes that source through and answers its values.
TEST_F(NativeDissolveTest, AFractionalDissolveKeepsTheDefaultDerivation)
{
    resetProject();
    getApp()->getProject()->setAutoSetProjectFormatEnabled(true);
    getApp()->getProject()->setOrAddProjectFormat(Format(0, 0, 80, 60, "nativeDissolve80x60", 1.5));

    NodePtr first = createFormatConstant(getApp(), 64., 48., 1.);
    NodePtr second = createFormatConstant(getApp(), 100., 30., 2.);
    NodePtr node = createNode(QString::fromUtf8(kDissolveID), kNativeDissolveMajor);
    ASSERT_TRUE(first && second && isNative(node));
    connectNodes(first, node, 0, true);
    connectNodes(second, node, 1, true);

    ASSERT_TRUE(setKnobValues(node, kDissolveParamWhich, { 0.5 }));
    NodeMetadataConstPtr blended = node->getEffectInstance()->getMetadata(kTime, ViewIdx(0));
    ASSERT_TRUE(bool(blended));
    EXPECT_EQ(RectI(0, 0, 64, 48), blended->getOutputFormat());
    EXPECT_DOUBLE_EQ(1., blended->getPixelAspectRatio(-1));
    EXPECT_EQ(4, blended->getNComps(-1));

    ASSERT_TRUE(setKnobValues(node, kDissolveParamWhich, { 1. }));
    NodeMetadataConstPtr passed = node->getEffectInstance()->getMetadata(kTime, ViewIdx(0));
    ASSERT_TRUE(bool(passed));
    EXPECT_EQ(RectI(0, 0, 100, 30), passed->getOutputFormat());
    EXPECT_DOUBLE_EQ(2., passed->getPixelAspectRatio(-1));
}

// The mask and mix blend the dissolve with input 0, the lower of the two inputs: by the mask's
// alpha times the mix, or by its complement times the mix when inverted.
TEST_F(NativeDissolveTest, MaskInvertAndMixBlendWithTheLowerInput)
{
    resetProject();
    const RectD box(0., 0., 16., 16.);
    NodePtr from = createBoxConstant(getApp(), box, { kFromColor[0], kFromColor[1], kFromColor[2], kFromColor[3] });
    NodePtr to = createBoxConstant(getApp(), box, { kToColor[0], kToColor[1], kToColor[2], kToColor[3] });
    NodePtr mask = createBoxConstant(getApp(), box, { 0., 0., 0., 0.25 });
    NodePtr node = createNode(QString::fromUtf8(kDissolveID), kNativeDissolveMajor);
    ASSERT_TRUE(from && to && mask && isNative(node));
    connectNodes(from, node, Dissolve::sourceInput(0), true);
    connectNodes(to, node, Dissolve::sourceInput(1), true);
    connectNodes(mask, node, kDissolveMaskInput, true);
    ASSERT_TRUE(setKnobValues(node, "enableMask_Mask", { 1. }));
    ASSERT_TRUE(setChannelSelect(node, "maskChannel_Mask", "rgba.A"));
    ASSERT_TRUE(setKnobValues(node, kDissolveParamWhich, { 0.4 }));

    struct Case {
        bool invert;
        double mix;
    };
    const Case cases[] = { { false, 1. }, { false, 0.5 }, { true, 0.5 }, { true, 0. } };
    const RectI window(0, 0, 16, 16);
    for (const Case& c : cases) {
        ASSERT_TRUE(setKnobValues(node, kOfxMaskInvertParamName, { c.invert ? 1. : 0. }));
        ASSERT_TRUE(setKnobValues(node, kOfxMixParamName, { c.mix }));
        std::vector<float> pixels;
        ASSERT_TRUE(renderRGBA(node, kTime, window, &pixels));
        ASSERT_EQ((std::size_t)window.width() * window.height() * 4, pixels.size());
        const double amount = (c.invert ? 0.75 : 0.25) * c.mix;
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            const int k = (int)(i % 4);
            const double dissolved = kFromColor[k] + (kToColor[k] - kFromColor[k]) * 0.4;
            const double expected = kFromColor[k] + (dissolved - kFromColor[k]) * amount;
            ASSERT_NEAR(expected, pixels[i], 1e-6) << "invert " << c.invert << ", mix " << c.mix << ", float " << i;
        }
    }
}

// An unconnected input reads black, so the region is that of the connected input alone, and
// nothing at all when neither of the blended inputs is connected.
TEST_F(NativeDissolveTest, AnUnconnectedInputAddsNoRegion)
{
    resetProject();
    const RectD box(11., 7., 48., 30.);
    NodePtr to = createBoxConstant(getApp(), box, { kToColor[0], kToColor[1], kToColor[2], kToColor[3] });
    NodePtr node = createNode(QString::fromUtf8(kDissolveID), kNativeDissolveMajor);
    ASSERT_TRUE(to && isNative(node));
    connectNodes(to, node, Dissolve::sourceInput(1), true);
    EffectInstancePtr effect = node->getEffectInstance();

    ASSERT_TRUE(setKnobValues(node, kDissolveParamWhich, { 0.5 }));
    RectD rod;
    ASSERT_NE(eStatusFailed, effect->getRegionOfDefinition(effect->getRenderHash(), kTime, RenderScale::identity, ViewIdx(0), &rod));
    EXPECT_EQ(box, rod);

    std::vector<float> pixels;
    const RectI window(0, 0, 64, 40);
    ASSERT_TRUE(renderRGBA(node, kTime, window, &pixels));
    ASSERT_EQ((std::size_t)window.width() * window.height() * 4, pixels.size());
    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            const bool inside = (x >= box.x1) && (x < box.x2) && (y >= box.y1) && (y < box.y2);
            for (int k = 0; k < 4; ++k) {
                const float value = pixels[(((std::size_t)(y - window.y1) * window.width()) + (x - window.x1)) * 4 + k];
                ASSERT_NEAR(inside ? kToColor[k] * 0.5 : 0., value, 1e-6) << "(" << x << ", " << y << ") channel " << k;
            }
        }
    }

    ASSERT_TRUE(setKnobValues(node, kDissolveParamWhich, { 0. }));
    rod = RectD(0., 0., 1., 1.);
    ASSERT_NE(eStatusFailed, effect->getRegionOfDefinition(effect->getRenderHash(), kTime, RenderScale::identity, ViewIdx(0), &rod));
    EXPECT_TRUE(rod.isNull()) << rod.x1 << " " << rod.y1 << " " << rod.x2 << " " << rod.y2;
}

// An animated Which blends by its value at the rendered frame.
TEST_F(NativeDissolveTest, AnimatedWhichBlendsByItsValueAtEachFrame)
{
    resetProject();
    const RectD box(0., 0., 8., 8.);
    NodePtr from = createBoxConstant(getApp(), box, { kFromColor[0], kFromColor[1], kFromColor[2], kFromColor[3] });
    NodePtr to = createBoxConstant(getApp(), box, { kToColor[0], kToColor[1], kToColor[2], kToColor[3] });
    NodePtr node = createNode(QString::fromUtf8(kDissolveID), kNativeDissolveMajor);
    ASSERT_TRUE(from && to && isNative(node));
    connectNodes(from, node, Dissolve::sourceInput(0), true);
    connectNodes(to, node, Dissolve::sourceInput(1), true);
    KnobDouble* which = dynamic_cast<KnobDouble*>(node->getKnobByName(kDissolveParamWhich).get());
    ASSERT_TRUE(which != NULL);
    which->setValueAtTime(1., 0., ViewSpec::all(), 0);
    which->setValueAtTime(11., 1., ViewSpec::all(), 0);

    const RectI window(0, 0, 8, 8);
    for (const double time : { 1., 4., 6., 11. }) {
        const double w = which->getValueAtTime(time, 0, ViewSpec::all());
        if ((time > 1.) && (time < 11.)) {
            ASSERT_GT(w, 0.) << time;
            ASSERT_LT(w, 1.) << time;
        }
        std::vector<float> pixels;
        ASSERT_TRUE(renderRGBA(node, time, window, &pixels));
        ASSERT_EQ((std::size_t)window.width() * window.height() * 4, pixels.size());
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            const int k = (int)(i % 4);
            ASSERT_NEAR(kFromColor[k] + (kToColor[k] - kFromColor[k]) * w, pixels[i], 1e-6) << "frame " << time << ", float " << i;
        }
    }
}
