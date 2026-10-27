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
#include <atomic>
#include <cmath>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "CountingTestEffect.h"
#include "DeepRenderTestEffect.h"
#include "OfxTestPlugins/natronTest/NatronTestPluginIDs.h"
#include "RenderBothWays.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/DeepImage.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Generator/CheckerBoard.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/RFV/ReverseFlowVariableSet.h"
#include "Engine/Nodes/Transform/Crop.h"
#include "Engine/Nodes/Transform/Transform.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RFV.h"
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
        deepSyntheticSourceImages().clear();
    }

    virtual void TearDown() OVERRIDE
    {
        deepSyntheticSourceImages().clear();
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

    // A project variable of one dimension, declared the way the Python API and the parameter
    // manager declare one: that also makes it app.<name>, which an expression on it needs.
    KnobDoublePtr addProjectDouble(const std::string& name,
                                   double value)
    {
        ProjectPtr project = getApp()->getProject();
        KnobDoublePtr knob = project->createDoubleKnob(name, name, 1);
        std::string error;
        EXPECT_TRUE(RFV::addAsVariable(project.get(), knob, &error)) << error;
        knob->setValue(value);

        return knob;
    }

    // A FrameWindow node over source that renders source at time - offset.
    NodePtr createTimeOffset(const NodePtr& source,
                             int offset)
    {
        NodePtr node = createNode(QString::fromUtf8(kNatronTestFrameWindowPluginID));
        if (!node) {
            ADD_FAILURE() << "no FrameWindow node";

            return NodePtr();
        }
        connectNodes(source, node, 0, true);
        KnobInt* range = dynamic_cast<KnobInt*>(node->getKnobByName("frameRange").get());
        KnobBool* absolute = dynamic_cast<KnobBool*>(node->getKnobByName("absolute").get());
        KnobInt* timeOffset = dynamic_cast<KnobInt*>(node->getKnobByName("timeOffset").get());
        if (!range || !absolute || !timeOffset) {
            ADD_FAILURE() << "FrameWindow knobs missing";

            return NodePtr();
        }
        range->setValue(0, ViewSpec::all(), 0);
        range->setValue(0, ViewSpec::all(), 1);
        absolute->setValue(false);
        timeOffset->setValue(offset);

        return node;
    }

    // A second project view, so view 1 is one the project renders.
    void addRightView()
    {
        std::vector<std::string> extraViews;
        extraViews.push_back("Right");
        getApp()->getProject()->createProjectViews(extraViews);
        ASSERT_EQ(2u, getApp()->getProject()->getProjectViewNames().size());
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

    // Renders root's RGBA at time and view through the given scheduler, bypassing the cache
    // unless told otherwise, and returns its first pixel's red, or -1.
    float renderFirstPixel(const NodePtr& root,
                           RenderSchedulerModeEnum mode,
                           double time = 1.,
                           ViewIdx view = ViewIdx(0),
                           bool byPassCache = true)
    {
        std::vector<RenderedPlane> planes;
        std::string error;
        const RectI roi(0, 0, kCountingTestSize, kCountingTestSize);
        if (!renderNodePlanesInMode(root, time, view, 0 /*mipmapLevel*/, roi, std::list<ImageLayerDesc>(), mode, byPassCache, &planes, &error)) {
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

    // Constant variables reach the reader as plain values, never as bindings.
    {
        FrameRequestMap request;
        ASSERT_NE(eStatusFailed, runRequestPass(merge, &request));
        const std::vector<NodeFrameRequestPtr> entries = entriesOf(request, reader);
        ASSERT_EQ(2u, entries.size());
        std::vector<double> values;
        for (const NodeFrameRequestPtr& entry : entries) {
            const RenderContextValue* value = entry->context ? entry->context->find(RenderContextName::plain("v")) : 0;
            ASSERT_TRUE(value != 0);
            EXPECT_FALSE(std::holds_alternative<RenderContextBinding>(*value));
            values.push_back(doubleIn(entry->context, "v"));
        }
        std::sort(values.begin(), values.end());
        EXPECT_EQ((std::vector<double> { 1., 2. }), values);
    }

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

// Through the cache, an animated Set variable keys each frame apart: going back to a frame gives
// that frame's value, not the last one rendered.
TEST_F(RFVRender, AnimatedVariableThroughTheCacheFollowsTheFrame)
{
    NodePtr reader = createReader("rfv.v or 0");
    KnobDoublePtr v;
    NodePtr set = createSet("v", 0., &v);
    ASSERT_TRUE(reader && set && v);
    v->setValueAtTime(1., 10., ViewSpec::all(), 0);
    v->setValueAtTime(2., 20., ViewSpec::all(), 0);
    connectNodes(reader, set, 0, true);

    for (RenderSchedulerModeEnum mode : { eRenderSchedulerModeTaskGraph, eRenderSchedulerModeLegacy }) {
        EXPECT_EQ(10.f, renderFirstPixel(set, mode, 1., ViewIdx(0), false /*byPassCache*/));
        EXPECT_EQ(20.f, renderFirstPixel(set, mode, 2., ViewIdx(0), false /*byPassCache*/));
        EXPECT_EQ(10.f, renderFirstPixel(set, mode, 1., ViewIdx(0), false /*byPassCache*/));
    }
}

// Through the cache, a Set variable whose expression reads another node's knob follows edits of
// that knob.
TEST_F(RFVRender, SetVariableExpressionThroughTheCacheFollowsTheKnobItReads)
{
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDCounting));
    NodePtr reader = createReader("rfv.v or 0");
    KnobDoublePtr v;
    NodePtr set = createSet("v", 0., &v);
    ASSERT_TRUE(source && reader && set && v);
    KnobDoublePtr sourceValue = std::dynamic_pointer_cast<KnobDouble>(source->getKnobByName("value"));
    ASSERT_TRUE(bool(sourceValue));
    sourceValue->setValue(5.);
    ASSERT_NO_THROW(v->setExpression(0, source->getScriptName() + ".value.getValue(0)", false, true));
    connectNodes(reader, set, 0, true);

    double expected = 5.;
    for (RenderSchedulerModeEnum mode : { eRenderSchedulerModeTaskGraph, eRenderSchedulerModeLegacy }) {
        EXPECT_EQ((float)expected, renderFirstPixel(set, mode, 1., ViewIdx(0), false /*byPassCache*/));
        expected += 2.;
        sourceValue->setValue(expected);
        EXPECT_EQ((float)expected, renderFirstPixel(set, mode, 1., ViewIdx(0), false /*byPassCache*/));
    }
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

// Transform motion blur samples translate.x = rfv.v across the shutter: the Set's animated v is
// read at each sample's time, so the blurred render differs from the unblurred one.
TEST_F(RFVRender, MotionBlurSamplesTheSetVariableAcrossTheShutter)
{
    getApp()->getProject()->setOrAddProjectFormat(Format(0, 0, kFormatWidth, kFormatHeight, "rfvRenderFormat", 1.));
    NodePtr checker = createNode(QString::fromUtf8(PLUGINID_NATRON_CHECKERBOARD), PLUGIN_MAJOR_NATRON_CHECKERBOARD);
    NodePtr transform = createNode(QString::fromUtf8(PLUGINID_NATRON_TRANSFORM), PLUGIN_MAJOR_NATRON_TRANSFORM);
    KnobDoublePtr v;
    NodePtr set = createSet("v", 0., &v);
    ASSERT_TRUE(checker && transform && set && v);
    v->setValueAtTime(1., 10., ViewSpec::all(), 0);
    v->setValueAtTime(2., 110., ViewSpec::all(), 0);
    connectNodes(checker, transform, 0, true);
    connectNodes(transform, set, 0, true);
    KnobIPtr translate = transform->getKnobByName(kTransformNodeParamTranslate);
    KnobDouble* motionBlur = dynamic_cast<KnobDouble*>(transform->getKnobByName(kTransformNodeParamMotionBlur).get());
    KnobDouble* shutter = dynamic_cast<KnobDouble*>(transform->getKnobByName(kTransformNodeParamShutter).get());
    ASSERT_TRUE(translate && motionBlur && shutter);
    ASSERT_NO_THROW(translate->setExpression(0, "rfv.v or 0", false, true));
    shutter->setValue(1.);

    const RectI roi(0, 0, kFormatWidth, kFormatHeight);
    const std::list<ImageLayerDesc> layers(1, ImageLayerDesc::getRGBAComponents());
    std::string error;

    motionBlur->setValue(0.);
    std::vector<RenderedPlane> unblurred;
    ASSERT_TRUE(renderNodePlanesDirect(set, 1., ViewIdx(0), 0, roi, layers, &unblurred, &error)) << error;

    motionBlur->setValue(1.);
    std::vector<RenderedPlane> blurred;
    ASSERT_TRUE(renderNodePlanesDirect(set, 1., ViewIdx(0), 0, roi, layers, &blurred, &error)) << error;
    std::vector<RenderedPlane> blurredTaskGraph;
    ASSERT_TRUE(renderNodePlanesInMode(set, 1., ViewIdx(0), 0, roi, layers, eRenderSchedulerModeTaskGraph, true /*byPassCache*/, &blurredTaskGraph, &error)) << error;

    // The same Transform with translate.x set to v's value at frame 1, without motion blur.
    NodePtr plain = createNode(QString::fromUtf8(PLUGINID_NATRON_TRANSFORM), PLUGIN_MAJOR_NATRON_TRANSFORM);
    ASSERT_TRUE(bool(plain));
    connectNodes(checker, plain, 0, true);
    KnobDouble* plainTranslate = dynamic_cast<KnobDouble*>(plain->getKnobByName(kTransformNodeParamTranslate).get());
    ASSERT_TRUE(plainTranslate != NULL);
    plainTranslate->setValue(10., ViewSpec::all(), 0);
    std::vector<RenderedPlane> fixed;
    ASSERT_TRUE(renderNodePlanesDirect(plain, 1., ViewIdx(0), 0, roi, layers, &fixed, &error)) << error;

    ASSERT_EQ(1u, unblurred.size());
    ASSERT_EQ(1u, blurred.size());
    ASSERT_EQ(1u, blurredTaskGraph.size());
    ASSERT_EQ(1u, fixed.size());
    EXPECT_EQ(fixed[0].pixels, unblurred[0].pixels);
    EXPECT_NE(unblurred[0].pixels, blurred[0].pixels);
    EXPECT_NE(fixed[0].pixels, blurred[0].pixels);
    EXPECT_EQ(blurred[0].pixels, blurredTaskGraph[0].pixels);

    const RenderMismatch m = renderBothWaysDirect(set, 1., ViewIdx(0), 0 /*mipmapLevel*/, roi, std::vector<int> { 1, 4 });
    EXPECT_FALSE(m.any) << describe(m);
}

// reader -> FrameWindow(timeOffset 2) -> Set: a request at frame 3 reaches the reader at frame 1,
// which reads the Set's v at frame 1.
TEST_F(RFVRender, TimeOffsetAboveTheSetReadsTheVariableAtTheShiftedFrame)
{
    NodePtr reader = createReader("rfv.v or 0");
    KnobDoublePtr v;
    NodePtr set = createSet("v", 0., &v);
    ASSERT_TRUE(reader && set && v);
    v->setValueAtTime(1., 10., ViewSpec::all(), 0);
    v->setValueAtTime(3., 30., ViewSpec::all(), 0);
    NodePtr offset = createTimeOffset(reader, 2);
    ASSERT_TRUE(bool(offset));
    connectNodes(offset, set, 0, true);

    EXPECT_EQ(10.f, renderFirstPixel(set, eRenderSchedulerModeTaskGraph, 3.));
    EXPECT_EQ(10.f, renderFirstPixel(set, eRenderSchedulerModeLegacy, 3.));

    const RenderMismatch m = renderBothWaysDirect(set, 3., ViewIdx(0), 0 /*mipmapLevel*/, RectI(0, 0, kCountingTestSize, kCountingTestSize), std::vector<int> { 1, 4 });
    EXPECT_FALSE(m.any) << describe(m);
}

// reader -> Set -> FrameWindow(timeOffset 2): a request at frame 3 passes the Set at frame 1, and
// the reader reads v at frame 1.
TEST_F(RFVRender, TimeOffsetBelowTheSetReadsTheVariableAtTheShiftedFrame)
{
    NodePtr reader = createReader("rfv.v or 0");
    KnobDoublePtr v;
    NodePtr set = createSet("v", 0., &v);
    ASSERT_TRUE(reader && set && v);
    v->setValueAtTime(1., 10., ViewSpec::all(), 0);
    v->setValueAtTime(3., 30., ViewSpec::all(), 0);
    connectNodes(reader, set, 0, true);
    NodePtr offset = createTimeOffset(set, 2);
    ASSERT_TRUE(bool(offset));

    EXPECT_EQ(10.f, renderFirstPixel(offset, eRenderSchedulerModeTaskGraph, 3.));
    EXPECT_EQ(10.f, renderFirstPixel(offset, eRenderSchedulerModeLegacy, 3.));

    const RenderMismatch m = renderBothWaysDirect(offset, 3., ViewIdx(0), 0 /*mipmapLevel*/, RectI(0, 0, kCountingTestSize, kCountingTestSize), std::vector<int> { 1, 4 });
    EXPECT_FALSE(m.any) << describe(m);
}

// reader -> inner Set (x = rfv.y) -> outer Set (y animated): read at frame 1.5, x is y at 1.5.
TEST_F(RFVRender, NestedSetVariableReadsAtAFractionalTime)
{
    NodePtr reader = createReader("rfv.x or 0");
    KnobDoublePtr x;
    KnobDoublePtr y;
    NodePtr inner = createSet("x", 0., &x);
    NodePtr outer = createSet("y", 0., &y);
    ASSERT_TRUE(reader && inner && outer && x && y);
    y->setValueAtTime(1., 10., ViewSpec::all(), 0);
    y->setValueAtTime(2., 20., ViewSpec::all(), 0);
    ASSERT_NO_THROW(x->setExpression(0, "rfv.y or 0", false, true));
    connectNodes(reader, inner, 0, true);
    connectNodes(inner, outer, 0, true);

    const double expected = y->getValueAtTime(1.5);
    ASSERT_GT(expected, 10.);
    ASSERT_LT(expected, 20.);
    EXPECT_FLOAT_EQ((float)expected, renderFirstPixel(outer, eRenderSchedulerModeTaskGraph, 1.5));
    EXPECT_FLOAT_EQ((float)expected, renderFirstPixel(outer, eRenderSchedulerModeLegacy, 1.5));
}

// A right-view render reads a project variable at the right view: the variable's expression is
// evaluated at the view the render started at, not at the left one.
TEST_F(RFVRender, RightViewRenderReadsTheProjectVariableAtThatView)
{
    addRightView();
    addProjectDouble("base", 1.);
    KnobDoublePtr perView = addProjectDouble("perView", 0.);
    // Reading base through rfv also keeps the variable's results by view, not by time alone.
    ASSERT_NO_THROW(perView->setExpression(0, "(rfv.base or 0) + 10 * view", false, true));
    NodePtr reader = createReader("rfv.perView or 0");
    ASSERT_TRUE(bool(reader));

    EXPECT_EQ(11.f, renderFirstPixel(reader, eRenderSchedulerModeTaskGraph, 1., ViewIdx(1)));
    EXPECT_EQ(11.f, renderFirstPixel(reader, eRenderSchedulerModeLegacy, 1., ViewIdx(1)));
    EXPECT_EQ(1.f, renderFirstPixel(reader, eRenderSchedulerModeTaskGraph, 1., ViewIdx(0)));
    EXPECT_EQ(1.f, renderFirstPixel(reader, eRenderSchedulerModeLegacy, 1., ViewIdx(0)));
}

// A project variable whose expression reads an animated project variable gives a render at frame
// 10 the animated value at frame 10, while the timeline is on frame 1.
TEST_F(RFVRender, NestedProjectVariableReadsAtTheRenderedFrame)
{
    KnobDoublePtr animated = addProjectDouble("animated", 0.);
    animated->setValueAtTime(1., 10., ViewSpec::all(), 0);
    animated->setValueAtTime(10., 100., ViewSpec::all(), 0);
    KnobDoublePtr derived = addProjectDouble("derived", 0.);
    ASSERT_NO_THROW(derived->setExpression(0, "rfv.animated or 0", false, true));
    NodePtr reader = createReader("rfv.derived or 0");
    ASSERT_TRUE(bool(reader));
    getApp()->getTimeLine()->seekFrame(1, false, NULL, eTimelineChangeReasonOtherSeek);

    EXPECT_EQ(100.f, renderFirstPixel(reader, eRenderSchedulerModeTaskGraph, 10.));
    EXPECT_EQ(100.f, renderFirstPixel(reader, eRenderSchedulerModeLegacy, 10.));
    EXPECT_EQ(10.f, renderFirstPixel(reader, eRenderSchedulerModeTaskGraph, 1.));
    EXPECT_EQ(10.f, renderFirstPixel(reader, eRenderSchedulerModeLegacy, 1.));
}

// A reader of the project variable derived, whose expression reads rfv.source, under two Sets
// setting source to 1 and 2: the reader reads source through derived, so each Set's context keeps
// source, the reader renders once per Set, and each branch sees its own value.
TEST_F(RFVRender, DiamondThroughADerivedProjectVariable)
{
    addProjectDouble("source", 0.);
    KnobDoublePtr derived = addProjectDouble("derived", 0.);
    ASSERT_NO_THROW(derived->setExpression(0, "rfv.source or 0", false, true));
    NodePtr reader = createReader("rfv.derived or 0");
    NodePtr set1 = createSet("source", 1.);
    NodePtr set2 = createSet("source", 2.);
    NodePtr merge = createNode(QString::fromUtf8(kTestPluginIDCountingMerge));
    ASSERT_TRUE(reader && set1 && set2 && merge);
    connectNodes(reader, set1, 0, true);
    connectNodes(reader, set2, 0, true);
    connectNodes(set1, merge, 0, true);
    connectNodes(set2, merge, 1, true);

    {
        FrameRequestMap request;
        ASSERT_NE(eStatusFailed, runRequestPass(merge, &request));
        const std::vector<NodeFrameRequestPtr> entries = entriesOf(request, reader);
        ASSERT_EQ(2u, entries.size());
        std::vector<double> values;
        for (const NodeFrameRequestPtr& entry : entries) {
            values.push_back(doubleIn(entry->context, "source"));
        }
        std::sort(values.begin(), values.end());
        EXPECT_EQ((std::vector<double> { 1., 2. }), values);
        EXPECT_NE(entries[0]->contextKey, entries[1]->contextKey);
    }

    CountingTestRegistry::reset();
    // The merge holds its value 1 plus the reader under source = 1 and under source = 2.
    EXPECT_EQ(1.f + 1.f + 2.f, renderFirstPixel(merge, eRenderSchedulerModeTaskGraph));
    EXPECT_EQ(2, CountingTestRegistry::renders(reader));
    EXPECT_EQ(1.f + 1.f + 2.f, renderFirstPixel(merge, eRenderSchedulerModeLegacy));

    const RenderMismatch m = renderBothWaysDirect(merge, 1., ViewIdx(0), 0 /*mipmapLevel*/, RectI(0, 0, kCountingTestSize, kCountingTestSize), std::vector<int> { 1, 4 });
    EXPECT_FALSE(m.any) << describe(m);
}

// Threads asking a Set what it overrides and for the context it requests its input under, as a
// render does, while the main thread adds, moves and deletes other variables of the Set: every
// answer holds the variable that stays, with its value.
TEST_F(RFVRender, SetDeclarationEditsDuringRenderReads)
{
    const int kReaders = 4;
    const int kEdits = 200;
    NodePtr set = createSet("stable", 5.);
    ASSERT_TRUE(bool(set));
    const EffectInstancePtr effect = set->getEffectInstance();
    const KnobGroupPtr variables = variablesGroupOf(set);
    ASSERT_TRUE(effect && variables);

    std::atomic<bool> stop(false);
    std::atomic<int> torn(0);
    std::atomic<int> reads(0);
    std::vector<std::thread> readers;
    for (int t = 0; t < kReaders; ++t) {
        readers.emplace_back([&]() {
            while (!stop.load()) {
                if (!effect->getOverriddenContextVariables(0).contains(RenderContextName::plain("stable"))) {
                    ++torn;
                }
                std::vector<RenderContextPtr> contexts;
                if (!effect->getInputContexts(0, 1., ViewIdx(0), RenderContextPtr(), &contexts) || (contexts.size() != 1) || (doubleIn(contexts.front(), "stable") != 5.)) {
                    ++torn;
                }
                ++reads;
            }
        });
    }
    // The edits must overlap reads, so the readers get going first.
    while (reads.load() < kReaders) {
        std::this_thread::yield();
    }
    int refused = 0;
    for (int i = 0; i < kEdits; ++i) {
        const std::string name = "edited" + std::to_string(i % 4);
        KnobDoublePtr knob = effect->createDoubleKnob(name, name, 1);
        if (variables->addKnob(knob)) {
            effect->recreateUserKnobs(false);
            variables->moveOneStepUp(knob.get());
        } else {
            ++refused;
        }
        effect->deleteKnob(knob.get(), true);
        effect->recreateUserKnobs(false);
    }
    stop.store(true);
    for (std::thread& reader : readers) {
        reader.join();
    }
    EXPECT_EQ(0, refused);
    EXPECT_EQ(0, torn.load());
    const RFVDeclarationsConstPtr declared = RFV::getDeclarationSnapshot(effect.get());
    ASSERT_EQ(1u, declared->size());
    EXPECT_EQ("stable", declared->front().name);
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

// Checkerboard -> Grade -> Set(var = 'a') as input B of a node that is an identity of B over every
// tile but the one holding input A. During a paint stroke renderRoI pulls those tiles from the end
// of the identity walk directly, through the Set; they are the checkerboard as the Grade passes it
// under the Set's 'a', not doubled as it would be with the Set's value dropped.
TEST_F(RFVRender, TiledIdentityWalksThroughTheSet)
{
    NodePtr checker, grade;
    buildGradedChecker(&checker, &grade);
    ASSERT_TRUE(checker && grade);
    // The checkerboard covers the whole window by its own extent: the project format is set once per
    // process, so it stays the small one and would leave every identity tile outside the checker's RoD.
    KnobChoicePtr extent = std::dynamic_pointer_cast<KnobChoice>(checker->getKnobByName(kNativeGeneratorParamExtent));
    KnobDoublePtr bottomLeft = std::dynamic_pointer_cast<KnobDouble>(checker->getKnobByName(kNativeGeneratorParamBottomLeft));
    KnobDoublePtr size = std::dynamic_pointer_cast<KnobDouble>(checker->getKnobByName(kNativeGeneratorParamSize));
    ASSERT_TRUE(extent && bottomLeft && size);
    extent->setValueFromID(kNativeGeneratorExtentSize, 0);
    ASSERT_EQ(std::string(kNativeGeneratorExtentSize), extent->getActiveEntry().id);
    for (int i = 0; i < 2; ++i) {
        bottomLeft->setValue(0., ViewSpec::all(), i);
        size->setValue((double)kTiledIdentityTestSize, ViewSpec::all(), i);
    }
    NodePtr setA = createStringSet("var", "a");
    NodePtr inputA = createNode(QString::fromUtf8(kTestPluginIDCounting));
    NodePtr tiled = createNode(QString::fromUtf8(kTestPluginIDTiledIdentity));
    ASSERT_TRUE(setA && inputA && tiled);
    connectNodes(grade, setA, 0, true);
    connectNodes(inputA, tiled, 0, true);
    connectNodes(setA, tiled, 1, true);

    const RectI roi(0, 0, kTiledIdentityTestSize, kTiledIdentityTestSize);
    std::vector<RenderedPlane> checkerPlanes;
    std::string error;
    ASSERT_TRUE(renderNodePlanesDirect(checker, 1., ViewIdx(0), 0, roi, std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &checkerPlanes, &error)) << error;
    ASSERT_EQ(1u, checkerPlanes.size());
    const std::size_t stride = checkerPlanes[0].channels.size();
    ASSERT_GE(stride, 3u);

    const double time = 1.;
    AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);
    ParallelRenderArgsSetter frameRenderArgs(time,
                                             ViewIdx(0),
                                             true /*isRenderUserInteraction*/,
                                             false /*isSequential*/,
                                             abortInfo,
                                             tiled,
                                             0 /*textureIndex*/,
                                             getApp()->getTimeLine().get(),
                                             NodePtr(),
                                             false /*isAnalysis*/,
                                             false /*draftMode*/,
                                             RenderStatsPtr());
    FrameRequestMap request;
    ASSERT_NE(eStatusFailed, EffectInstance::computeRequestPass(time, ViewIdx(0), 0 /*mipmapLevel*/, roi.toCanonical_noClipping(0, 1.), tiled, request));
    frameRenderArgs.updateNodesRequest(request);
    EffectInstancePtr effect = tiled->getEffectInstance();
    // The identity tiles are taken only while a paint stroke is being drawn.
    effect->setDuringPaintStrokeCreationThreadLocal(true);
    EffectInstance::RenderRoIArgs args(time,
                                       RenderScale::identity,
                                       0 /*mipmapLevel*/,
                                       ViewIdx(0),
                                       true /*byPassCache*/,
                                       roi,
                                       RectD(),
                                       std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()),
                                       eImageBitDepthFloat,
                                       false /*calledFromGetImage*/,
                                       0 /*caller*/,
                                       eStorageModeRAM,
                                       time);
    std::map<ImageLayerDesc, ImagePtr> layers;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, effect->renderRoI(args, &layers));
    ASSERT_EQ(1u, layers.size());
    const ImagePtr& image = layers.begin()->second;
    ASSERT_TRUE(image && (image->getBitDepth() == eImageBitDepthFloat));
    ASSERT_EQ((int)stride, (int)image->getComponentsCount());

    Image::ReadAccess access = image->getReadRights();
    bool nonZero = false;
    // The tile holding input A, the first of the 128 x 128 tiles, is rendered; every other one is an identity.
    const int renderedTile = 128;
    for (int y = roi.y1; y < roi.y2; ++y) {
        for (int x = roi.x1; x < roi.x2; ++x) {
            if ((x < renderedTile) && (y < renderedTile)) {
                continue;
            }
            const float* pixel = (const float*)access.pixelAt(x, y);
            ASSERT_TRUE(pixel != 0) << x << ", " << y;
            const std::size_t at = ((std::size_t)(y - roi.y1) * roi.width() + (x - roi.x1)) * stride;
            for (std::size_t c = 0; c < 3; ++c) {
                const float expected = checkerPlanes[0].pixels[at + c];
                nonZero = nonZero || (expected != 0.f);
                if (std::abs(expected - pixel[c]) > 1e-5f) {
                    FAIL() << "pixel " << x << ", " << y << " channel " << c << ": " << pixel[c] << " is not " << expected;
                }
            }
        }
    }
    EXPECT_TRUE(nonZero);
}

namespace {

const int kDeepSlotSize = 4;

// A kDeepSlotSize square of one sample per pixel at depth 1, opaque, with colour value.
DeepImagePtr
makeDeepSlotImage(float value)
{
    DeepImagePtr image = std::make_shared<DeepImage>(RectI(0, 0, kDeepSlotSize, kDeepSlotSize), RenderScale::identity, ViewIdx(0));
    SampleTable& table = image->getSampleTableForWriting();
    const std::size_t pixels = (std::size_t)(kDeepSlotSize * kDeepSlotSize);
    for (std::size_t p = 0; p < pixels; ++p) {
        table.setCount(p, 1);
    }
    table.recomputeOffsets();
    const char* const colour[] = { "R", "G", "B" };
    for (const char* name : colour) {
        float* data = image->getChannelForWriting(name).dataForWriting();
        std::fill(data, data + pixels, value);
    }
    float* alpha = image->getChannelForWriting("A").dataForWriting();
    float* z = image->getChannelForWriting("Z").dataForWriting();
    float* zBack = image->getChannelForWriting("ZBack").dataForWriting();
    std::fill(alpha, alpha + pixels, 1.f);
    std::fill(z, z + pixels, 1.f);
    std::fill(zBack, zBack + pixels, 1.f);
    image->setTidy(true);

    return image;
}

// The red of node's first deep sample at frame 1, rendered under the scheduler's frame args, or -1.
float
renderFirstDeepRed(const NodePtr& node,
                   const TimeLinePtr& timeline)
{
    const double time = 1.;
    AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(true, 0);
    ParallelRenderArgsSetter frameRenderArgs(time,
                                             ViewIdx(0),
                                             true /*isRenderUserInteraction*/,
                                             false /*isSequential*/,
                                             abortInfo,
                                             node,
                                             0 /*textureIndex*/,
                                             timeline.get(),
                                             NodePtr(),
                                             false /*isAnalysis*/,
                                             false /*draftMode*/,
                                             RenderStatsPtr());
    EffectInstance::RenderDeepRoIArgs args(time,
                                           RenderScale::identity,
                                           0 /*mipmapLevel*/,
                                           ViewIdx(0),
                                           false /*byPassCache*/,
                                           RectI(0, 0, kDeepSlotSize, kDeepSlotSize),
                                           RectD(),
                                           0 /*caller*/,
                                           time);
    DeepImagePtr image;
    if (node->getEffectInstance()->renderDeepRoI(args, &image) != EffectInstance::eRenderRoIRetCodeOk) {
        ADD_FAILURE() << "the deep render failed";

        return -1.f;
    }
    const DeepChannelBuffer* red = image ? image->getChannel("R") : 0;
    if (!red || (image->getSampleTable().getTotalSampleCount() == 0)) {
        ADD_FAILURE() << "no deep sample rendered";

        return -1.f;
    }

    return red->data()[0];
}

} // namespace

// A deep source whose slot knob reads rfv.var, under a deep gain, under Set(var = 'a') and
// Set(var = 'b'): each Set's deep render is the source's image for that Set's value, through the
// Set's identity hop to the gain and the gain's pull of the source.
TEST_F(RFVRender, DeepSourceFollowsTheSetBelowIt)
{
    deepSyntheticSourceImages()[1] = makeDeepSlotImage(1.f);
    deepSyntheticSourceImages()[2] = makeDeepSlotImage(2.f);
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDDeepSyntheticSource));
    NodePtr gain = createNode(QString::fromUtf8(kTestPluginIDDeepRenderGain));
    ASSERT_TRUE(source && gain);
    KnobIPtr slot = source->getKnobByName("slot");
    ASSERT_TRUE(bool(slot));
    ASSERT_NO_THROW(slot->setExpression(0, "1 if rfv.var == 'a' else 2", false, true));
    connectNodes(source, gain, 0, true);
    NodePtr setA = createStringSet("var", "a");
    NodePtr setB = createStringSet("var", "b");
    ASSERT_TRUE(setA && setB);
    connectNodes(gain, setA, 0, true);
    connectNodes(gain, setB, 0, true);

    const TimeLinePtr timeline = getApp()->getTimeLine();
    EXPECT_EQ(1.f * kDeepRenderTestGain, renderFirstDeepRed(setA, timeline));
    EXPECT_EQ(2.f * kDeepRenderTestGain, renderFirstDeepRed(setB, timeline));
    // From the deep cache, each Set's answer stays its own.
    EXPECT_EQ(1.f * kDeepRenderTestGain, renderFirstDeepRed(setA, timeline));
    // Without a Set, var is unset.
    EXPECT_EQ(2.f * kDeepRenderTestGain, renderFirstDeepRed(gain, timeline));
}

// Checkerboard -> Crop, reformatting to a rectangle whose width is 32 while rfv.var is 'a' and 16
// otherwise, under Set(var = 'a') and Set(var = 'b'), each followed by a Grade: the format and
// region of definition each Set and the node below it see are the Crop's under that Set's value,
// outside a render and in the request pass.
TEST_F(RFVRender, MetadataAndRoDFollowTheSetBelowThem)
{
    getApp()->getProject()->setOrAddProjectFormat(Format(0, 0, kFormatWidth, kFormatHeight, "rfvMetadataFormat", 1.));
    NodePtr checker = createNode(QString::fromUtf8(PLUGINID_NATRON_CHECKERBOARD), PLUGIN_MAJOR_NATRON_CHECKERBOARD);
    NodePtr crop = createNode(QString::fromUtf8(PLUGINID_NATRON_CROP), PLUGIN_MAJOR_NATRON_CROP);
    ASSERT_TRUE(checker && crop);
    connectNodes(checker, crop, 0, true);
    KnobBoolPtr reformat = std::dynamic_pointer_cast<KnobBool>(crop->getKnobByName(kCropParamReformat));
    KnobDoublePtr bottomLeft = std::dynamic_pointer_cast<KnobDouble>(crop->getKnobByName(kNativeGeneratorParamBottomLeft));
    KnobDoublePtr size = std::dynamic_pointer_cast<KnobDouble>(crop->getKnobByName(kNativeGeneratorParamSize));
    ASSERT_TRUE(reformat && bottomLeft && size);
    reformat->setValue(true);
    bottomLeft->setValue(0., ViewSpec::all(), 0);
    bottomLeft->setValue(0., ViewSpec::all(), 1);
    size->setValue(24., ViewSpec::all(), 1);
    ASSERT_NO_THROW(size->setExpression(0, "32 if rfv.var == 'a' else 16", false, true));

    NodePtr setA = createStringSet("var", "a");
    NodePtr setB = createStringSet("var", "b");
    NodePtr gradeA = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE), PLUGIN_MAJOR_NATRON_GRADE);
    NodePtr gradeB = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE), PLUGIN_MAJOR_NATRON_GRADE);
    ASSERT_TRUE(setA && setB && gradeA && gradeB);
    connectNodes(crop, setA, 0, true);
    connectNodes(crop, setB, 0, true);
    connectNodes(setA, gradeA, 0, true);
    connectNodes(setB, gradeB, 0, true);

    const NodePtr branches[2][2] = { { setA, gradeA }, { setB, gradeB } };
    const int widths[2] = { 32, 16 };
    for (int b = 0; b < 2; ++b) {
        for (const NodePtr& node : branches[b]) {
            NodeMetadataConstPtr metadata = node->getEffectInstance()->getMetadata(1., ViewIdx(0));
            ASSERT_TRUE(bool(metadata));
            EXPECT_EQ(RectI(0, 0, widths[b], 24), metadata->getOutputFormat()) << node->getScriptName_mt_safe();
        }
    }

    // The crop may pad its rectangle, so the two branches are compared by the width they differ by.
    double rodWidths[2][2];
    for (int b = 0; b < 2; ++b) {
        for (int n = 0; n < 2; ++n) {
            EffectInstancePtr effect = branches[b][n]->getEffectInstance();
            RectD rod;
            bool isProjectFormat = false;
            ASSERT_NE(eStatusFailed, effect->getRegionOfDefinition_public(effect->getRenderHash(), 1., RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat));
            rodWidths[b][n] = rod.width();
        }
    }
    for (int n = 0; n < 2; ++n) {
        EXPECT_EQ(16., rodWidths[0][n] - rodWidths[1][n]) << "node " << n;
    }

    // The request pass plans each Grade over the region its Set's value gives.
    double requestWidths[2];
    for (int b = 0; b < 2; ++b) {
        FrameRequestMap request;
        ASSERT_NE(eStatusFailed, runRequestPass(branches[b][1], &request));
        const std::vector<NodeFrameRequestPtr> entries = entriesOf(request, branches[b][1]);
        ASSERT_EQ(1u, entries.size());
        const FrameViewRequest* fv = entries.front()->getFrameViewRequest(1., ViewIdx(0));
        ASSERT_TRUE(fv != 0);
        requestWidths[b] = fv->globalData.rod.width();
    }
    EXPECT_EQ(16., requestWidths[0] - requestWidths[1]);
}
