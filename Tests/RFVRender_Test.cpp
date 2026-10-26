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
#include <list>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "CountingTestEffect.h"
#include "RenderBothWays.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Generator/CheckerBoard.h"
#include "Engine/Nodes/RFV/ReverseFlowVariableSet.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderContext.h"
#include "Engine/RenderStats.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const int kFormatWidth = 64;
const int kFormatHeight = 48;

// The request entries of node, one per context key.
std::vector<NodeFrameRequestPtr>
entriesOf(const FrameRequestMap& request,
          const NodePtr& node)
{
    std::vector<NodeFrameRequestPtr> entries;
    for (FrameRequestMap::const_iterator it = request.begin(); it != request.end(); ++it) {
        if (it->first.node == node) {
            entries.push_back(it->second);
        }
    }

    return entries;
}

// The double the context sets the variable to, or -1 when it does not set a double.
double
doubleIn(const RenderContextPtr& context,
         const std::string& name)
{
    const RenderContextValue* value = context ? context->find(RenderContextName::plain(name)) : 0;
    const double* asDouble = value ? std::get_if<double>(value) : 0;

    return asDouble ? *asDouble : -1.;
}

KnobGroupPtr
variablesGroupOf(const NodePtr& node)
{
    return std::dynamic_pointer_cast<KnobGroup>(node->getKnobByName(kReverseFlowVariableSetParamVariables));
}

} // namespace

class RFVRender
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        CountingTestRegistry::reset();
    }

    virtual void TearDown() OVERRIDE
    {
        CountingTestRegistry::reset();
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    // A Set node declaring the Double variable name.
    NodePtr createSet(const std::string& name,
                      double value,
                      KnobDoublePtr* variable = 0)
    {
        NodePtr set = createNode(QString::fromUtf8(PLUGINID_NATRON_REVERSEFLOWVARIABLESET));
        if (!set) {
            ADD_FAILURE() << "no Set node";

            return NodePtr();
        }
        EffectInstancePtr effect = set->getEffectInstance();
        KnobDoublePtr knob = effect->createDoubleKnob(name, name, 1);
        if (!variablesGroupOf(set)->addKnob(knob)) {
            ADD_FAILURE() << "the Set node refused " << name;

            return NodePtr();
        }
        effect->recreateUserKnobs(false);
        knob->setValue(value);
        if (variable) {
            *variable = knob;
        }

        return set;
    }

    // A Set node declaring the String variable name.
    NodePtr createStringSet(const std::string& name,
                            const std::string& value)
    {
        NodePtr set = createNode(QString::fromUtf8(PLUGINID_NATRON_REVERSEFLOWVARIABLESET));
        if (!set) {
            ADD_FAILURE() << "no Set node";

            return NodePtr();
        }
        EffectInstancePtr effect = set->getEffectInstance();
        KnobStringPtr knob = effect->createStringKnob(name, name);
        if (!variablesGroupOf(set)->addKnob(knob)) {
            ADD_FAILURE() << "the Set node refused " << name;

            return NodePtr();
        }
        effect->recreateUserKnobs(false);
        knob->setValue(value);

        return set;
    }

    // A counting node whose value knob has the expression.
    NodePtr createReader(const std::string& expression)
    {
        NodePtr node = createNode(QString::fromUtf8(kTestPluginIDCounting));
        KnobIPtr value = node ? node->getKnobByName("value") : KnobIPtr();
        if (!value) {
            ADD_FAILURE() << "no counting node";

            return NodePtr();
        }
        EXPECT_NO_THROW(value->setExpression(0, expression, false, true)) << expression;

        return node;
    }

    // A project variable of one dimension.
    KnobDoublePtr addProjectDouble(const std::string& name,
                                   double value)
    {
        ProjectPtr project = getApp()->getProject();
        KnobDoublePtr knob = project->createDoubleKnob(name, name, 1);
        EXPECT_TRUE(project->getRFVGroup()->addKnob(knob));
        project->recreateUserKnobs(false);
        knob->setValue(value);

        return knob;
    }

    StatusEnum runRequestPass(const NodePtr& root,
                              FrameRequestMap* request)
    {
        const double time = 1.;
        AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);
        ParallelRenderArgsSetter frameRenderArgs(time,
                                                 ViewIdx(0),
                                                 false /*isRenderUserInteraction*/,
                                                 false /*isSequential*/,
                                                 abortInfo,
                                                 root,
                                                 0 /*textureIndex*/,
                                                 getApp()->getTimeLine().get(),
                                                 NodePtr(),
                                                 false /*isAnalysis*/,
                                                 false /*draftMode*/,
                                                 RenderStatsPtr());
        const RectD canonicalWindow(0., 0., kCountingTestSize, kCountingTestSize);
        const StatusEnum stat = EffectInstance::computeRequestPass(time, ViewIdx(0), 0 /*mipmapLevel*/, canonicalWindow, root, *request);
        if (stat != eStatusFailed) {
            frameRenderArgs.updateNodesRequest(*request);
        }

        return stat;
    }

    // Renders root's RGBA at time through the given scheduler and returns its first pixel's red, or -1.
    float renderFirstPixel(const NodePtr& root,
                           RenderSchedulerModeEnum mode,
                           double time = 1.)
    {
        std::vector<RenderedPlane> planes;
        std::string error;
        const RectI roi(0, 0, kCountingTestSize, kCountingTestSize);
        if (!renderNodePlanesInMode(root, time, ViewIdx(0), 0 /*mipmapLevel*/, roi, std::list<ImageLayerDesc>(), mode, true /*byPassCache*/, &planes, &error)) {
            ADD_FAILURE() << error;

            return -1.f;
        }
        if (planes.empty() || planes.front().pixels.empty()) {
            ADD_FAILURE() << "no plane rendered";

            return -1.f;
        }

        return planes.front().pixels.front();
    }

    // Checkerboard -> Grade, whose multiply is 1 while rfv.var is 'a' and 2 otherwise.
    void buildGradedChecker(NodePtr* checker,
                            NodePtr* grade)
    {
        getApp()->getProject()->setOrAddProjectFormat(Format(0, 0, kFormatWidth, kFormatHeight, "rfvRenderFormat", 1.));
        *checker = createNode(QString::fromUtf8(PLUGINID_NATRON_CHECKERBOARD), PLUGIN_MAJOR_NATRON_CHECKERBOARD);
        *grade = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE), PLUGIN_MAJOR_NATRON_GRADE);
        ASSERT_TRUE(*checker && *grade);
        connectNodes(*checker, *grade, 0, true);
        KnobIPtr multiply = (*grade)->getKnobByName(kGradeParamMultiply);
        ASSERT_TRUE(bool(multiply));
        for (int i = 0; i < multiply->getDimension(); ++i) {
            ASSERT_NO_THROW(multiply->setExpression(i, "1 if rfv.var == 'a' else 2", false, true));
        }
    }

    // The RGBA of node over the format, rendered in one Legacy renderRoI.
    std::vector<RenderedPlane> renderFormat(const NodePtr& node)
    {
        std::vector<RenderedPlane> planes;
        std::string error;
        const RectI roi(0, 0, kFormatWidth, kFormatHeight);
        const std::list<ImageLayerDesc> layers(1, ImageLayerDesc::getRGBAComponents());
        EXPECT_TRUE(renderNodePlanesDirect(node, 1., ViewIdx(0), 0, roi, layers, &planes, &error)) << error;

        return planes;
    }

    // Whether the colour channels of node are those of source scaled by scale.
    void expectScaled(const NodePtr& source,
                      const NodePtr& node,
                      float scale)
    {
        const std::vector<RenderedPlane> in = renderFormat(source);
        const std::vector<RenderedPlane> out = renderFormat(node);
        ASSERT_EQ(1u, in.size());
        ASSERT_EQ(1u, out.size());
        ASSERT_EQ(in[0].channels, out[0].channels);
        ASSERT_EQ(in[0].pixels.size(), out[0].pixels.size());
        const std::size_t stride = in[0].channels.size();
        ASSERT_GE(stride, 3u);
        bool nonZero = false;
        for (std::size_t i = 0; i < in[0].pixels.size(); ++i) {
            if ((i % stride) >= 3) {
                continue;
            }
            nonZero = nonZero || (in[0].pixels[i] != 0.f);
            if (std::abs(in[0].pixels[i] * scale - out[0].pixels[i]) > 1e-5f) {
                ADD_FAILURE() << "value " << i << ": " << out[0].pixels[i] << " is not " << scale << " x " << in[0].pixels[i];

                return;
            }
        }
        EXPECT_TRUE(nonZero);
    }
};

// The request pass carries each Set's overlay to the nodes above it: a reader under Set1 is
// requested with a = Set1's value, under Set2 with Set2's, and a node reading no variable once.
TEST_F(RFVRender, RequestPassCarriesEachSetsValues)
{
    NodePtr ancestor = createNode(QString::fromUtf8(kTestPluginIDCounting));
    NodePtr reader = createReader("rfv.a or 0");
    NodePtr set1 = createSet("a", 1.);
    NodePtr set2 = createSet("a", 2.);
    NodePtr merge = createNode(QString::fromUtf8(kTestPluginIDCountingMerge));
    ASSERT_TRUE(ancestor && reader && set1 && set2 && merge);
    connectNodes(ancestor, reader, 0, true);
    connectNodes(reader, set1, 0, true);
    connectNodes(reader, set2, 0, true);
    connectNodes(set1, merge, 0, true);
    connectNodes(set2, merge, 1, true);

    {
        FrameRequestMap request;
        ASSERT_NE(eStatusFailed, runRequestPass(set1, &request));
        const std::vector<NodeFrameRequestPtr> entries = entriesOf(request, reader);
        ASSERT_EQ(1u, entries.size());
        EXPECT_EQ(1., doubleIn(entries.front()->context, "a"));
        EXPECT_NE(0u, entries.front()->contextKey);
    }
    {
        FrameRequestMap request;
        ASSERT_NE(eStatusFailed, runRequestPass(set2, &request));
        const std::vector<NodeFrameRequestPtr> entries = entriesOf(request, reader);
        ASSERT_EQ(1u, entries.size());
        EXPECT_EQ(2., doubleIn(entries.front()->context, "a"));
    }
    {
        FrameRequestMap request;
        ASSERT_NE(eStatusFailed, runRequestPass(merge, &request));
        const std::vector<NodeFrameRequestPtr> entries = entriesOf(request, reader);
        ASSERT_EQ(2u, entries.size());
        std::vector<double> values;
        for (const NodeFrameRequestPtr& entry : entries) {
            values.push_back(doubleIn(entry->context, "a"));
        }
        std::sort(values.begin(), values.end());
        EXPECT_EQ((std::vector<double> { 1., 2. }), values);
        EXPECT_NE(entries[0]->contextKey, entries[1]->contextKey);

        // The Sets read nothing from below, as they override what the reader reads.
        for (const NodePtr& set : { set1, set2 }) {
            const std::vector<NodeFrameRequestPtr> setEntries = entriesOf(request, set);
            ASSERT_EQ(1u, setEntries.size());
            EXPECT_EQ(0u, setEntries.front()->contextKey);
        }
        const std::vector<NodeFrameRequestPtr> ancestorEntries = entriesOf(request, ancestor);
        ASSERT_EQ(1u, ancestorEntries.size());
        EXPECT_EQ(0u, ancestorEntries.front()->contextKey);
    }
}

// Checkerboard -> Grade whose gain is 1 if rfv.var == 'a' else 2, under two Sets: the branch
// setting 'a' passes the checkerboard through, the one setting 'b' doubles it, in both schedulers.
TEST_F(RFVRender, GradeGainFollowsTheSetBelowIt)
{
    NodePtr checker, grade;
    buildGradedChecker(&checker, &grade);
    ASSERT_TRUE(checker && grade);
    NodePtr setA = createStringSet("var", "a");
    NodePtr setB = createStringSet("var", "b");
    ASSERT_TRUE(setA && setB);
    connectNodes(grade, setA, 0, true);
    connectNodes(grade, setB, 0, true);

    expectScaled(checker, setA, 1.f);
    expectScaled(checker, setB, 2.f);

    const RectI roi(0, 0, kFormatWidth, kFormatHeight);
    for (const NodePtr& set : { setA, setB }) {
        const RenderMismatch m = renderBothWaysDirect(set, 1., ViewIdx(0), 0 /*mipmapLevel*/, roi, std::vector<int> { 1, 4 });
        EXPECT_FALSE(m.any) << describe(m);
    }
}

// The project's value is the base a Set overrides: the Grade alone reads the project's, under the
// Set it reads the Set's, and an edit of the project's reaches the Grade alone.
TEST_F(RFVRender, SetOverridesTheProjectValue)
{
    ProjectPtr project = getApp()->getProject();
    KnobStringPtr projectVar = project->createStringKnob("var", "var");
    ASSERT_TRUE(project->getRFVGroup()->addKnob(projectVar));
    project->recreateUserKnobs(false);
    projectVar->setValue("b");

    NodePtr checker, grade;
    buildGradedChecker(&checker, &grade);
    ASSERT_TRUE(checker && grade);
    NodePtr setA = createStringSet("var", "a");
    ASSERT_TRUE(bool(setA));
    connectNodes(grade, setA, 0, true);

    expectScaled(checker, grade, 2.f);
    expectScaled(checker, setA, 1.f);

    projectVar->setValue("a");
    expectScaled(checker, grade, 1.f);
}

// With no Set in the graph, a render reads the project's value.
TEST_F(RFVRender, GraphWithoutASetSeesTheProjectValue)
{
    addProjectDouble("p", 4.);
    NodePtr reader = createReader("rfv.p or 0");
    ASSERT_TRUE(bool(reader));
    EXPECT_EQ(4.f, renderFirstPixel(reader, eRenderSchedulerModeTaskGraph));
    EXPECT_EQ(4.f, renderFirstPixel(reader, eRenderSchedulerModeLegacy));
}

// A -> Set1 and A -> Set2 -> Merge: A renders once per Set, its context-free ancestor once.
TEST_F(RFVRender, DiamondRendersTheReaderOncePerSet)
{
    NodePtr ancestor = createNode(QString::fromUtf8(kTestPluginIDCounting));
    NodePtr reader = createReader("rfv.v or 0");
    NodePtr set1 = createSet("v", 1.);
    NodePtr set2 = createSet("v", 2.);
    NodePtr merge = createNode(QString::fromUtf8(kTestPluginIDCountingMerge));
    ASSERT_TRUE(ancestor && reader && set1 && set2 && merge);
    connectNodes(ancestor, reader, 0, true);
    connectNodes(reader, set1, 0, true);
    connectNodes(reader, set2, 0, true);
    connectNodes(set1, merge, 0, true);
    connectNodes(set2, merge, 1, true);

    CountingTestRegistry::reset();
    // The merge holds its value 1 plus the reader under v = 1 (1 + 1) and under v = 2 (2 + 1).
    EXPECT_EQ(1.f + 2.f + 3.f, renderFirstPixel(merge, eRenderSchedulerModeTaskGraph));
    EXPECT_EQ(2, CountingTestRegistry::renders(reader));
    EXPECT_EQ(1, CountingTestRegistry::renders(ancestor));
    EXPECT_EQ(1, CountingTestRegistry::renders(merge));

    EXPECT_EQ(1.f + 2.f + 3.f, renderFirstPixel(merge, eRenderSchedulerModeLegacy));

    const RenderMismatch m = renderBothWaysDirect(merge, 1., ViewIdx(0), 0 /*mipmapLevel*/, RectI(0, 0, kCountingTestSize, kCountingTestSize), std::vector<int> { 1, 4 });
    EXPECT_FALSE(m.any) << describe(m);
}

// An animated Set variable gives the reader its value at each frame.
TEST_F(RFVRender, AnimatedVariableFollowsTheFrame)
{
    NodePtr reader = createReader("rfv.v or 0");
    KnobDoublePtr v;
    NodePtr set = createSet("v", 0., &v);
    ASSERT_TRUE(reader && set && v);
    v->setValueAtTime(1., 10., ViewSpec::all(), 0);
    v->setValueAtTime(2., 20., ViewSpec::all(), 0);
    connectNodes(reader, set, 0, true);

    EXPECT_EQ(10.f, renderFirstPixel(set, eRenderSchedulerModeTaskGraph, 1.));
    EXPECT_EQ(20.f, renderFirstPixel(set, eRenderSchedulerModeTaskGraph, 2.));
    EXPECT_EQ(10.f, renderFirstPixel(set, eRenderSchedulerModeLegacy, 1.));
    EXPECT_EQ(20.f, renderFirstPixel(set, eRenderSchedulerModeLegacy, 2.));
}

// A Set variable's expression reads what the nodes below the Set set.
TEST_F(RFVRender, SetVariableExpressionReadsTheDownstreamValue)
{
    NodePtr reader = createReader("rfv.x or 0");
    KnobDoublePtr x;
    NodePtr inner = createSet("x", 0., &x);
    NodePtr outer = createSet("y", 3.);
    ASSERT_TRUE(reader && inner && outer && x);
    ASSERT_NO_THROW(x->setExpression(0, "(rfv.y or 0) * 2", false, true));
    connectNodes(reader, inner, 0, true);
    connectNodes(inner, outer, 0, true);

    EXPECT_EQ(6.f, renderFirstPixel(outer, eRenderSchedulerModeTaskGraph));
    EXPECT_EQ(6.f, renderFirstPixel(outer, eRenderSchedulerModeLegacy));
    // Without the outer Set, y is unset.
    EXPECT_EQ(0.f, renderFirstPixel(inner, eRenderSchedulerModeTaskGraph));

    const RenderMismatch m = renderBothWaysDirect(outer, 1., ViewIdx(0), 0 /*mipmapLevel*/, RectI(0, 0, kCountingTestSize, kCountingTestSize), std::vector<int> { 1, 4 });
    EXPECT_FALSE(m.any) << describe(m);
}

// A variable no Set and not the project sets reads as None in a render.
TEST_F(RFVRender, UnsetVariableIsNone)
{
    NodePtr reader = createReader("-1 if rfv.unset is None else 1");
    NodePtr set = createSet("v", 1.);
    ASSERT_TRUE(reader && set);
    connectNodes(reader, set, 0, true);

    EXPECT_EQ(-1.f, renderFirstPixel(set, eRenderSchedulerModeTaskGraph));
    EXPECT_EQ(-1.f, renderFirstPixel(set, eRenderSchedulerModeLegacy));
}
