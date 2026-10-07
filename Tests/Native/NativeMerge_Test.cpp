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
#include <iostream>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include <ofxNatron.h>

#include "BaseTest.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Generator/CheckerBoard.h"
#include "Engine/Nodes/Merge/Merge.h"
#include "Engine/Nodes/Merge/MergeOperators.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Plugin.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING
NATRON_PYTHON_NAMESPACE_USING

namespace {

const int kOfxMergeMajor = 2;
const int kNativeMergeMajor = PLUGIN_MAJOR_NATRON_MERGE;
const double kTime = 1.;

const MergePresetEnum kPresets[] = {
    eMergePresetMerge, eMergePresetPlus, eMergePresetMatte, eMergePresetMultiply, eMergePresetIn,
    eMergePresetOut, eMergePresetScreen, eMergePresetMax, eMergePresetMin, eMergePresetDifference
};

bool
isNative(const NodePtr& node)
{
    return node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
}

// The Porter-Duff operators only add and multiply the inputs, so they are held to the exact
// class; the blend modes divide, take roots or go through the HSL conversion, so to the class of
// the transcendental nodes.
ParityTolerance
toleranceFor(MergeOperators::Operation op)
{
    switch (op) {
    case MergeOperators::eATop:
    case MergeOperators::eConjointOver:
    case MergeOperators::eCopy:
    case MergeOperators::eDisjointOver:
    case MergeOperators::eIn:
    case MergeOperators::eMask:
    case MergeOperators::eMatte:
    case MergeOperators::eOut:
    case MergeOperators::eOver:
    case MergeOperators::eStencil:
    case MergeOperators::eUnder:
    case MergeOperators::eXOR:
        return ParityTolerance::exact();
    default:
        return ParityTolerance::transcendental();
    }
}

// Wide enough for every source the cases offset, so clipping to the region of definition is
// compared too.
RectI
caseWindow(unsigned mipmapLevel)
{
    return RectD(-4., -4., 84., 64.).toPixelEnclosing(mipmapLevel, 1.);
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

RectD
regionOfDefinition(const NodePtr& node)
{
    RectD rod;
    EffectInstancePtr effect = node ? node->getEffectInstance() : EffectInstancePtr();

    EXPECT_TRUE(bool(effect));
    if (!effect) {
        return rod;
    }
    bool isProjectFormat = false;
    const StatusEnum stat = effect->getRegionOfDefinition_public(effect->getRenderHash(), kTime, RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat);
    EXPECT_NE(eStatusFailed, stat) << node->getPluginID();

    return rod;
}

void
expectSameRect(const RectD& expected,
               const RectD& actual,
               const std::string& what)
{
    EXPECT_EQ(expected.x1, actual.x1) << what;
    EXPECT_EQ(expected.y1, actual.y1) << what;
    EXPECT_EQ(expected.x2, actual.x2) << what;
    EXPECT_EQ(expected.y2, actual.y2) << what;
}

} // namespace

class NativeMergeTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        ProjectPtr project = getApp()->getProject();
        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, 96, 72, "nativeMerge96x72", 1.));
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    // B on input 0, at the origin; with `withA`, A on input A, offset so the two overlap only in
    // part and every pixel of the overlap pairs different values.
    ParityPair makePair(const std::string& id,
                        bool withA = true,
                        bool withMask = false)
    {
        ParityPair pair = makeParityPair(getApp(), id, kOfxMergeMajor, kNativeMergeMajor, withMask ? std::string("Mask") : std::string());

        EXPECT_TRUE(bool(pair.native)) << id;
        EXPECT_FALSE(pair.live()) << "the OFX " << id << " is retired, so parity replays the recorded references";
        EXPECT_TRUE(isNative(pair.native)) << id;
        if (withA && pair.native) {
            NodePtr a = connectParityInput(pair, "A");
            EXPECT_TRUE(bool(a));
            if (a) {
                setParitySourceOrigin(a, 12, 6);
            }
        }

        return pair;
    }

    // Compares the pair at each listed mipmap level over the case window and prints the largest
    // difference, so the tolerance actually needed is on record.
    void expectParity(const ParityPair& pair,
                      const std::string& caseName,
                      const ParityTolerance& tolerance,
                      bool record,
                      unsigned lastMipmapLevel = 1)
    {
        for (unsigned mipmapLevel = 0; mipmapLevel <= lastMipmapLevel; ++mipmapLevel) {
            const ParityResult r = compareParity(pair, caseName, caseWindow(mipmapLevel), mipmapLevel, tolerance, record);
            EXPECT_TRUE(r.ok) << pair.id << " " << caseName << ", mipmap " << mipmapLevel << ": " << describe(r);
            EXPECT_FALSE(r.live);
            EXPECT_GE(r.planesCompared, 1) << caseName;
            std::cout << "[ parity ] " << pair.id << " " << caseName << " mipmap " << mipmapLevel << (r.live ? " live" : " replay")
                      << ": planes " << r.planesCompared << ", max abs diff " << r.maxAbsDiff << std::endl;
        }
    }

    NodePtr createMerge(const NodePtr& a,
                        const NodePtr& b,
                        const NodePtr& mask = NodePtr())
    {
        NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_MERGE));

        EXPECT_TRUE(isNative(merge));
        if (!merge) {
            return merge;
        }
        connectNodes(b, merge, kMergeInputB, true);
        connectNodes(a, merge, kMergeInputA, true);
        EXPECT_TRUE(setKnobValues(merge, kOfxMixParamName, std::vector<double>(1, 0.5)));
        if (mask) {
            connectNodes(mask, merge, kMergeInputMask, true);
            EXPECT_TRUE(setKnobValues(merge, "enableMask_Mask", std::vector<double>(1, 1.)));
        }

        return merge;
    }

    NodePtr createSource(int index)
    {
        NodePtr source = createNode(QString::fromUtf8(PLUGINID_NATRON_CHECKERBOARD));

        EXPECT_TRUE(bool(source));
        if (source) {
            std::vector<double> color(4, 1.);
            color[0] = color[1] = color[2] = 0.1 + 0.001 * index;
            EXPECT_TRUE(setKnobValues(source, "color0", color));
        }

        return source;
    }

    NodePtr createGrade(const NodePtr& input,
                        int index)
    {
        NodePtr grade = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));

        EXPECT_TRUE(bool(grade));
        if (grade) {
            connectNodes(input, grade, 0, true);
            std::vector<double> multiply(4, 1.);
            multiply[0] = 1. + 0.0001 * (index % 7 + 1);
            EXPECT_TRUE(setKnobValues(grade, kGradeParamMultiply, multiply));
        }

        return grade;
    }

    // Renders `node` both ways at pool sizes 1 and 4: bit-exact, and the task graph pulls
    // nothing it did not plan.
    void expectSameBothWays(const NodePtr& node)
    {
        std::vector<int> poolSizes;
        poolSizes.push_back(1);
        poolSizes.push_back(4);
        std::vector<int> unplannedPulls;
        const RenderMismatch m = renderBothWaysDirect(node, kTime, ViewIdx(0), 0, RectI(0, 0, 96, 72), poolSizes, std::function<void()>(), 0.f, &unplannedPulls);
        EXPECT_FALSE(m.any) << describe(m);
        EXPECT_EQ(poolSizes.size(), unplannedPulls.size());
        for (std::size_t i = 0; i < unplannedPulls.size(); ++i) {
            EXPECT_EQ(0, unplannedPulls[i]) << "pool size " << poolSizes[i];
        }
    }
};

TEST_F(NativeMergeTest, EveryPresetCarriesItsOperation)
{
    for (std::size_t p = 0; p < sizeof(kPresets) / sizeof(kPresets[0]); ++p) {
        const std::string id = MergeNode::presetPluginID(kPresets[p]);
        NodePtr native = createNodeAtMajor(getApp(), id, kNativeMergeMajor);
        ASSERT_TRUE(isNative(native)) << id;
        EXPECT_EQ(kNativeMergeMajor, native->getMajorVersion()) << id;

        KnobChoice* operation = dynamic_cast<KnobChoice*>(native->getKnobByName(kMergeParamOperation).get());
        ASSERT_TRUE(operation != NULL) << id;
        EXPECT_EQ((int)MergeNode::presetOperation(kPresets[p]), operation->getValue()) << id;
    }
}

TEST_F(NativeMergeTest, InputsKeepTheOfxOrder)
{
    NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_MERGE));

    ASSERT_TRUE(isNative(merge));
    // An inspector node, as the OpenFX Merge is, shows only the next free A input in the node graph.
    EXPECT_TRUE(dynamic_cast<InspectorNode*>(merge.get()) != NULL);
    EffectInstancePtr effect = merge->getEffectInstance();
    ASSERT_EQ(kMergeMaxAInputs + 2, effect->getNInputs());
    EXPECT_EQ(std::string("B"), effect->getInputLabel(0));
    EXPECT_EQ(std::string("A"), effect->getInputLabel(1));
    EXPECT_EQ(std::string("Mask"), effect->getInputLabel(2));
    EXPECT_EQ(std::string("A2"), effect->getInputLabel(3));
    EXPECT_EQ(std::string("A64"), effect->getInputLabel(kMergeMaxAInputs + 1));
    for (int i = 0; i < effect->getNInputs(); ++i) {
        EXPECT_TRUE(effect->isInputOptional(i)) << i;
        EXPECT_EQ(i == kMergeInputMask, effect->isInputMask(i)) << i;
    }
}

TEST_F(NativeMergeTest, OperationRefreshesTheSubLabelAndAlphaMasking)
{
    NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_MERGE));

    ASSERT_TRUE(isNative(merge));
    KnobString* subLabel = dynamic_cast<KnobString*>(merge->getKnobByName(kNatronOfxParamStringSublabelName).get());
    KnobBool* alphaMasking = dynamic_cast<KnobBool*>(merge->getKnobByName(kMergeParamAlphaMasking).get());
    ASSERT_TRUE(subLabel != NULL);
    ASSERT_TRUE(alphaMasking != NULL);
    EXPECT_EQ(std::string("over"), subLabel->getValue());
    EXPECT_EQ(MergeOperators::isMaskable(MergeOperators::eOver), alphaMasking->isEnabled(0));

    ASSERT_TRUE(setKnobValue(merge, kMergeParamOperation, "multiply"));
    EXPECT_EQ(std::string("multiply"), subLabel->getValue());
    EXPECT_EQ(MergeOperators::isMaskable(MergeOperators::eMultiply), alphaMasking->isEnabled(0));

    ASSERT_TRUE(setKnobValue(merge, kMergeParamOperation, "matte"));
    EXPECT_EQ(std::string("matte"), subLabel->getValue());
    EXPECT_EQ(MergeOperators::isMaskable(MergeOperators::eMatte), alphaMasking->isEnabled(0));
}

TEST_F(NativeMergeTest, OverWithAnOffsetA)
{
    ParityPair pair = makePair(PLUGINID_NATRON_MERGE);
    expectParity(pair, "over-offset-a", ParityTolerance::exact(), true);
}

TEST_F(NativeMergeTest, HueOperator)
{
    ParityPair pair = makePair(PLUGINID_NATRON_MERGE);
    ASSERT_TRUE(bool(pair.native));

    ASSERT_TRUE(setKnobOnBoth(pair, kMergeParamOperation, MergeOperators::operationId(MergeOperators::eHue)));
    ASSERT_TRUE(setKnobOnBoth(pair, kMergeParamAlphaMasking, { 0. }));
    expectParity(pair, "op-hue", toleranceFor(MergeOperators::eHue), true, 0);
}

TEST_F(NativeMergeTest, ThreeAInputs)
{
    ParityPair pair = makePair(PLUGINID_NATRON_MERGE);
    NodePtr a2 = connectParityInput(pair, "A2");
    NodePtr a3 = connectParityInput(pair, "A3");
    ASSERT_TRUE(bool(a2));
    ASSERT_TRUE(bool(a3));
    setParitySourceOrigin(a2, -6, 10);
    setParitySourceOrigin(a3, 20, -2);
    setParitySourceComponents(a3, "alpha");
    expectParity(pair, "three-a-inputs", ParityTolerance::exact(), true);
}

TEST_F(NativeMergeTest, MaskAndMix)
{
    ParityPair pair = makePair(PLUGINID_NATRON_MERGE, true, true);
    ASSERT_TRUE(bool(pair.mask));
    setParitySourceOrigin(pair.mask, 8, 4);
    ASSERT_TRUE(setKnobOnBoth(pair, "enableMask_Mask", { 1. }));
    ASSERT_TRUE(setChannelSelect(pair.native, "maskChannel_Mask", "rgba.A"));
    ASSERT_TRUE(setKnobOnBoth(pair, kOfxMixParamName, { 0.5 }));
    ASSERT_TRUE(setKnobOnBoth(pair, kMergeParamOperation, std::string("screen")));
    expectParity(pair, "mask-mix", ParityTolerance::transcendental(), true);
}

TEST_F(NativeMergeTest, BoundingBoxWithoutTheNamedInputIsTheProjectExtent)
{
    NodePtr a = createSource(0);
    NodePtr b = createSource(1);
    ASSERT_TRUE(bool(a));
    ASSERT_TRUE(bool(b));
    NodePtr merge = createMerge(a, b);
    ASSERT_TRUE(isNative(merge));

    ASSERT_TRUE(setKnobValue(merge, kMergeParamBBox, std::string(kMergeBBoxA)));
    merge->disconnectInput(kMergeInputA);
    expectSameRect(RectD(0., 0., 96., 72.), regionOfDefinition(merge), "bbox a without A");
}

TEST_F(NativeMergeTest, UnversionedRequestsGetTheNativeMerge)
{
    NodePtr unversioned = createNode(QString::fromUtf8(PLUGINID_NATRON_MERGE));
    ASSERT_TRUE(bool(unversioned));
    EXPECT_TRUE(isNative(unversioned));
    EXPECT_EQ(kNativeMergeMajor, unversioned->getMajorVersion());

    NodesList before = getApp()->getProject()->getNodes();
    const std::string script = getApp()->getAppIDString() + ".createNode(\"" + std::string(PLUGINID_NATRON_MERGE) + "\")\n";
    std::string error, output;
    EXPECT_TRUE(interpretPythonScript(script, &error, &output)) << error;
    NodesList after = getApp()->getProject()->getNodes();
    NodePtr created;
    for (NodesList::const_iterator it = after.begin(); it != after.end(); ++it) {
        if (std::find(before.begin(), before.end(), *it) == before.end()) {
            created = *it;
        }
    }
    ASSERT_TRUE(bool(created));
    EXPECT_EQ(std::string(PLUGINID_NATRON_MERGE), created->getPluginID());
    EXPECT_TRUE(isNative(created));
}

// The bench's wide topology: leaves of (CheckerBoard -> Grade) reduced by a balanced tree of
// merges at mix 0.5, here with 16 nodes.
TEST_F(NativeMergeTest, WideGraphRendersTheSameInBothSchedulerModes)
{
    std::vector<NodePtr> level;
    for (int i = 0; i < 5; ++i) {
        NodePtr source = createSource(i);
        ASSERT_TRUE(bool(source));
        NodePtr grade = createGrade(source, i);
        ASSERT_TRUE(bool(grade));
        level.push_back(grade);
    }
    while (level.size() > 1) {
        std::vector<NodePtr> next;
        for (std::size_t j = 0; j + 1 < level.size(); j += 2) {
            NodePtr merge = createMerge(level[j], level[j + 1]);
            ASSERT_TRUE(bool(merge));
            next.push_back(merge);
        }
        if (level.size() % 2) {
            next.push_back(level.back());
        }
        level = next;
    }
    expectSameBothWays(level[0]);
}

// The bench's comp topology: a seeded random DAG of grades, transforms, blurs, fan-outs and
// merges over 20 steps, the first merge masked, every remaining branch merged at the end.
TEST_F(NativeMergeTest, CompGraphRendersTheSameInBothSchedulerModes)
{
    unsigned int state = 1;
    const auto next = [&state](unsigned int range) {
        state = state * 1103515245u + 12345u;

        return (state >> 16) % range;
    };

    std::vector<NodePtr> streams;
    streams.push_back(createSource(0));
    ASSERT_TRUE(bool(streams[0]));
    int nSources = 1;
    bool masked = false;
    for (int made = 0; made < 20; ++made) {
        const unsigned int r = next(100);
        const std::size_t k = next((unsigned int)streams.size());
        NodePtr s = streams[k];
        if (r < 8) {
            streams.push_back(createSource(nSources++));
        } else if (r < 40) {
            streams[k] = createGrade(s, made);
        } else if (r < 52) {
            NodePtr transform = createNode(QString::fromUtf8(PLUGINID_OFX_TRANSFORM));
            ASSERT_TRUE(bool(transform));
            connectNodes(s, transform, 0, true);
            std::vector<double> translate(2, 0.);
            translate[0] = 0.25 + 0.01 * (made % 3);
            EXPECT_TRUE(setKnobValues(transform, "translate", translate));
            streams[k] = transform;
        } else if (r < 62) {
            NodePtr blur = createNode(QString::fromUtf8(PLUGINID_OFX_BLURCIMG));
            ASSERT_TRUE(bool(blur));
            connectNodes(s, blur, 0, true);
            EXPECT_TRUE(setKnobValues(blur, "size", std::vector<double>(2, 3.)));
            streams[k] = blur;
        } else if (r < 72) {
            NodePtr dot = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
            ASSERT_TRUE(bool(dot));
            connectNodes(s, dot, 0, true);
            streams.push_back(dot);
        } else if (streams.size() > 1) {
            const std::size_t j = next((unsigned int)streams.size());
            if (j != k) {
                NodePtr mask = masked ? NodePtr() : createSource(100);
                masked = true;
                NodePtr merge = createMerge(streams[k], streams[j], mask);
                ASSERT_TRUE(bool(merge));
                streams[k] = merge;
                streams.erase(streams.begin() + j);
            } else {
                streams[k] = createGrade(s, made);
            }
        } else {
            streams[k] = createGrade(s, made);
        }
    }
    NodePtr out = streams[0];
    for (std::size_t i = 1; i < streams.size(); ++i) {
        out = createMerge(streams[i], out);
        ASSERT_TRUE(bool(out));
    }
    expectSameBothWays(out);
}
