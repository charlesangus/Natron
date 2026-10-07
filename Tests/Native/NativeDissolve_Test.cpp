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
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "CountingTestEffect.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Merge/Dissolve.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/OfxEffectInstance.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kDissolveID = PLUGINID_NATRON_DISSOLVE;
const int kOfxDissolveMajor = 1;
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
isOfx(const NodePtr& node)
{
    return node && dynamic_cast<OfxEffectInstance*>(node->getEffectInstance().get());
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
        ParityPair pair = makeParityPair(getApp(), kDissolveID, kOfxDissolveMajor, kNativeDissolveMajor, withMask ? std::string("Mask") : std::string());

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

TEST_F(NativeDissolveTest, KnobsMatchTheOfxDissolve)
{
    resetProject();
    ParityPair pair = makePair(1, true);
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(pair.live()) << "the OFX Dissolve must be loadable at major " << kOfxDissolveMajor;

    expectKnobParity(pair.ofx, pair.native);
}

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

TEST_F(NativeDissolveTest, WhichZero)
{
    resetProject();
    ParityPair pair = makePair(3);
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(setKnobOnBoth(pair, kDissolveParamWhich, { 0. }));
    expectParity(pair, "which-0", false);
}

TEST_F(NativeDissolveTest, WhichBetweenZeroAndOne)
{
    resetProject();
    ParityPair pair = makePair(3);
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(setKnobOnBoth(pair, kDissolveParamWhich, { 0.3 }));
    expectParity(pair, "which-0.3", true);
}

TEST_F(NativeDissolveTest, WhichOne)
{
    resetProject();
    ParityPair pair = makePair(3);
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(setKnobOnBoth(pair, kDissolveParamWhich, { 1. }));
    expectParity(pair, "which-1", false);
}

TEST_F(NativeDissolveTest, WhichBetweenOneAndTwo)
{
    resetProject();
    ParityPair pair = makePair(3);
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(setKnobOnBoth(pair, kDissolveParamWhich, { 1.5 }));
    expectParity(pair, "which-1.5", false);
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

TEST_F(NativeDissolveTest, MaskedInvertedBlend)
{
    resetProject();
    ParityPair pair = makePair(2, true);
    ASSERT_TRUE(bool(pair.mask));
    setParitySourceOrigin(pair.mask, 12, 6);
    ASSERT_TRUE(setKnobOnBoth(pair, "enableMask_Mask", { 1. }));
    ASSERT_TRUE(setChannelSelect(pair.native, "maskChannel_Mask", "rgba.A"));
    ASSERT_TRUE(setKnobOnBoth(pair, "maskInvert", { 1. }));
    ASSERT_TRUE(setKnobOnBoth(pair, kDissolveParamWhich, { 0.7 }));
    expectParity(pair, "masked-inverted", false);
}

TEST_F(NativeDissolveTest, MissingUpperInput)
{
    resetProject();
    ParityPair pair = makePair(1);
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(setKnobOnBoth(pair, kDissolveParamWhich, { 0.5 }));
    expectParity(pair, "missing-upper", false);
}

TEST_F(NativeDissolveTest, MissingLowerInput)
{
    resetProject();
    ParityPair pair = makePair(2);
    ASSERT_TRUE(bool(pair.native));
    pair.native->disconnectInput(0);
    if (pair.ofx) {
        pair.ofx->disconnectInput(0);
    }
    ASSERT_TRUE(setKnobOnBoth(pair, kDissolveParamWhich, { 0.5 }));
    expectParity(pair, "missing-lower", false);
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

    if (isPluginMajorRegistered(kDissolveID, kOfxDissolveMajor)) {
        NodePtr ofx = createNode(QString::fromUtf8(kDissolveID), kOfxDissolveMajor);
        ASSERT_TRUE(bool(ofx));
        EXPECT_TRUE(isOfx(ofx));
    }
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
