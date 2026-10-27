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
#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "ContextTestEffects.h"
#include "CountingTestEffect.h"
#include "RenderBothWays.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/LogEntry.h"
#include "Engine/Node.h"
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

// Makes a knob's value depend on the test variable.
class TestVariableReadSource
    : public ContextReadSource {
public:
    virtual void appendContextReads(RenderContextReadSet* reads) const OVERRIDE FINAL
    {
        reads->add(contextTestVariable());
    }
};

// A context setting the test variable to value.
RenderContextPtr
testContext(double value)
{
    return RenderContext::Builder().set(contextTestVariable(), RenderContextValue(value)).build();
}

// A Python expression counting its evaluations in builtins.<counter> and returning the count.
std::string
countingExpression(const std::string& counter)
{
    return "import builtins\n"
           "builtins."
        + counter + " = getattr(builtins, '" + counter + "', 0) + 1\n"
                                                         "ret = float(builtins."
        + counter + ")\n";
}

// How many error-log entries hold text.
int
errorLogEntriesWith(const QString& text)
{
    std::list<LogEntry> entries;
    appPTR->getErrorLog_mt_safe(&entries);
    int count = 0;
    for (const LogEntry& entry : entries) {
        if (entry.message.contains(text)) {
            ++count;
        }
    }

    return count;
}

} // namespace

class RenderContextRequest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        CountingTestRegistry::reset();
        ContextTestRegistry::reset();
    }

    virtual void TearDown() OVERRIDE
    {
        CountingTestRegistry::reset();
        ContextTestRegistry::reset();
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    NodePtr createWithDouble(const char* pluginID,
                             const char* knob,
                             double value)
    {
        NodePtr node = createNode(QString::fromUtf8(pluginID));
        KnobDouble* valueKnob = node ? dynamic_cast<KnobDouble*>(node->getKnobByName(knob).get()) : 0;
        if (!valueKnob) {
            return NodePtr();
        }
        valueKnob->setValue(value);

        return node;
    }

    NodePtr createFanOut(int count,
                         int first,
                         bool equal)
    {
        NodePtr node = createNode(QString::fromUtf8(kTestPluginIDContextFanOut));
        KnobInt* countKnob = node ? dynamic_cast<KnobInt*>(node->getKnobByName("count").get()) : 0;
        KnobInt* firstKnob = node ? dynamic_cast<KnobInt*>(node->getKnobByName("first").get()) : 0;
        KnobBool* equalKnob = node ? dynamic_cast<KnobBool*>(node->getKnobByName("equal").get()) : 0;
        if (!countKnob || !firstKnob || !equalKnob) {
            return NodePtr();
        }
        countKnob->setValue(count);
        firstKnob->setValue(first);
        equalKnob->setValue(equal);

        return node;
    }

    // Ancestor (a context-free Counting node) -> Reader -> FanOut, the fan-out being the root.
    bool buildFanOutGraph(int count,
                          int first,
                          bool equal,
                          NodePtr* ancestor,
                          NodePtr* reader,
                          NodePtr* fanOut)
    {
        *ancestor = createWithDouble(kTestPluginIDCounting, "value", 1.);
        *reader = createWithDouble(kTestPluginIDContextReader, "value", 1.);
        *fanOut = createFanOut(count, first, equal);

        return *ancestor && *reader && *fanOut && (*reader)->connectInput(*ancestor, 0) && (*fanOut)->connectInput(*reader, 0);
    }

    // The value of knob's dimension 0 at frame 1, read while node's effect renders under context.
    double valueUnder(const NodePtr& node,
                      const KnobIPtr& knob,
                      const RenderContextPtr& context)
    {
        AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);
        ParallelRenderArgsSetter frameRenderArgs(1.,
                                                 ViewIdx(0),
                                                 false /*isRenderUserInteraction*/,
                                                 false /*isSequential*/,
                                                 abortInfo,
                                                 node,
                                                 0 /*textureIndex*/,
                                                 getApp()->getTimeLine().get(),
                                                 NodePtr(),
                                                 false /*isAnalysis*/,
                                                 false /*draftMode*/,
                                                 RenderStatsPtr());
        const EffectInstancePtr effect = node->getEffectInstance();
        const ParallelRenderArgsPtr base = effect->getParallelRenderArgsTLS();
        KnobDouble* asDouble = dynamic_cast<KnobDouble*>(knob.get());
        if (!base || !asDouble) {
            ADD_FAILURE() << "no frame args or not a double knob";

            return -1.;
        }
        ParallelRenderArgsPtr args = std::make_shared<ParallelRenderArgs>(*base);
        args->context = context;
        args->contextKey = context ? context->hash() : 0;
        ContextFrameArgsScope scope(effect, args);

        return asDouble->getValueAtTime(1., 0);
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

    // Renders root's RGBA through the given scheduler and returns its first pixel's red, or -1.
    float renderFirstPixel(const NodePtr& root,
                           RenderSchedulerModeEnum mode)
    {
        std::vector<RenderedPlane> planes;
        std::string error;
        const RectI roi(0, 0, kCountingTestSize, kCountingTestSize);
        if (!renderNodePlanesInMode(root, 1., ViewIdx(0), 0 /*mipmapLevel*/, roi, std::list<ImageLayerDesc>(), mode, true /*byPassCache*/, &planes, &error)) {
            ADD_FAILURE() << error;

            return -1.f;
        }
        if (planes.empty() || planes.front().pixels.empty()) {
            ADD_FAILURE() << "no plane rendered";

            return -1.f;
        }

        return planes.front().pixels.front();
    }
};

// A reading node gets one request entry per context its consumer fans it into, keyed by the
// context's hash; a context-free ancestor gets one entry under the empty context, key 0.
TEST_F(RenderContextRequest, FanOutGivesTheReaderOneEntryPerContext)
{
    NodePtr ancestor, reader, fanOut;
    ASSERT_TRUE(buildFanOutGraph(3, 0, false, &ancestor, &reader, &fanOut));

    FrameRequestMap request;
    ASSERT_NE(eStatusFailed, runRequestPass(fanOut, &request));

    const std::vector<NodeFrameRequestPtr> readerEntries = entriesOf(request, reader);
    ASSERT_EQ(3u, readerEntries.size());
    std::set<double> values;
    std::set<U64> keys;
    for (const NodeFrameRequestPtr& entry : readerEntries) {
        ASSERT_TRUE(bool(entry->context));
        EXPECT_NE(0u, entry->contextKey);
        EXPECT_EQ(entry->context->hash(), entry->contextKey);
        values.insert(contextTestValue(entry->context));
        keys.insert(entry->contextKey);
        EXPECT_TRUE(request.findFrameViewRequest(NodeCtxKey(reader, entry->contextKey), 1., ViewIdx(0)) != NULL);
    }
    EXPECT_EQ((std::set<double> { 0., 1., 2. }), values);
    EXPECT_EQ(3u, keys.size());

    // The thin lookup is the empty context's, which the reader is never requested under.
    EXPECT_TRUE(request.find(reader) == request.end());
    EXPECT_TRUE(request.findFrameViewRequest(reader, 1., ViewIdx(0)) == NULL);

    const std::vector<NodeFrameRequestPtr> ancestorEntries = entriesOf(request, ancestor);
    ASSERT_EQ(1u, ancestorEntries.size());
    EXPECT_EQ(0u, ancestorEntries.front()->contextKey);
    EXPECT_FALSE(bool(ancestorEntries.front()->context));
    EXPECT_TRUE(request.find(ancestor) != request.end());
    EXPECT_EQ(1u, entriesOf(request, fanOut).size());

    // The fan-out's frame depends on each of the reader's entries, the edge naming the key.
    const FrameViewRequest* fanOutFrame = request.findFrameViewRequest(fanOut, 1., ViewIdx(0));
    ASSERT_TRUE(fanOutFrame != NULL);
    std::set<U64> edgeKeys;
    for (const FrameViewRequest::TaskEdge& edge : fanOutFrame->dependencies) {
        EXPECT_EQ(reader, edge.node.lock());
        edgeKeys.insert(edge.contextKey);
    }
    EXPECT_EQ(keys, edgeKeys);

    // Each reader entry depends on the ancestor's single entry.
    for (const NodeFrameRequestPtr& entry : readerEntries) {
        const FrameViewRequest* readerFrame = request.findFrameViewRequest(NodeCtxKey(reader, entry->contextKey), 1., ViewIdx(0));
        ASSERT_TRUE(readerFrame != NULL);
        ASSERT_EQ(1u, readerFrame->dependencies.size());
        EXPECT_EQ(ancestor, readerFrame->dependencies.front().node.lock());
        EXPECT_EQ(0u, readerFrame->dependencies.front().contextKey);
    }
    const FrameViewRequest* ancestorFrame = request.findFrameViewRequest(ancestor, 1., ViewIdx(0));
    ASSERT_TRUE(ancestorFrame != NULL);
    EXPECT_EQ(3, ancestorFrame->consumers);
}

// Equal contexts are one key, so the reader is requested once.
TEST_F(RenderContextRequest, EqualContextsShareTheReaderEntry)
{
    NodePtr ancestor, reader, fanOut;
    ASSERT_TRUE(buildFanOutGraph(3, 1, true, &ancestor, &reader, &fanOut));

    FrameRequestMap request;
    ASSERT_NE(eStatusFailed, runRequestPass(fanOut, &request));
    const std::vector<NodeFrameRequestPtr> readerEntries = entriesOf(request, reader);
    ASSERT_EQ(1u, readerEntries.size());
    EXPECT_EQ(1., contextTestValue(readerEntries.front()->context));
    EXPECT_EQ(1u, entriesOf(request, ancestor).size());
}

// A node requesting its own output under the context it renders under would wait on itself.
TEST_F(RenderContextRequest, SelfTargetingRequestFails)
{
    NodePtr chain = createNode(QString::fromUtf8(kTestPluginIDContextChain));
    ASSERT_TRUE(bool(chain));
    KnobBool* selfTarget = dynamic_cast<KnobBool*>(chain->getKnobByName("selfTarget").get());
    ASSERT_TRUE(selfTarget != NULL);
    selfTarget->setValue(true);
    NodePtr fanOut = createFanOut(1, 2, false);
    ASSERT_TRUE(bool(fanOut));
    ASSERT_TRUE(fanOut->connectInput(chain, 0));

    FrameRequestMap request;
    EXPECT_EQ(eStatusFailed, runRequestPass(fanOut, &request));
    EXPECT_TRUE(chain->hasPersistentMessage());
}

// The key is mixed into a reading node's hash; a context-free node's hash does not change.
TEST_F(RenderContextRequest, ContextKeyIsMixedIntoTheHash)
{
    NodePtr ancestor, reader, fanOut;
    ASSERT_TRUE(buildFanOutGraph(2, 0, false, &ancestor, &reader, &fanOut));

    FrameRequestMap plain;
    ASSERT_NE(eStatusFailed, runRequestPass(reader, &plain));
    FrameRequestMap::const_iterator plainReader = plain.find(reader);
    FrameRequestMap::const_iterator plainAncestor = plain.find(ancestor);
    ASSERT_TRUE(plainReader != plain.end());
    ASSERT_TRUE(plainAncestor != plain.end());
    EXPECT_EQ(reader->getEffectInstance()->getRenderHash(), plainReader->second->nodeHash);

    FrameRequestMap fanned;
    ASSERT_NE(eStatusFailed, runRequestPass(fanOut, &fanned));
    FrameRequestMap::const_iterator fannedAncestor = fanned.find(ancestor);
    ASSERT_TRUE(fannedAncestor != fanned.end());
    EXPECT_EQ(plainAncestor->second->nodeHash, fannedAncestor->second->nodeHash);
    EXPECT_EQ(ancestor->getEffectInstance()->getRenderHash(), fannedAncestor->second->nodeHash);

    std::set<U64> hashes;
    for (const NodeFrameRequestPtr& entry : entriesOf(fanned, reader)) {
        EXPECT_EQ(mixRenderContextKey(plainReader->second->nodeHash, entry->contextKey), entry->nodeHash);
        EXPECT_NE(plainReader->second->nodeHash, entry->nodeHash);
        hashes.insert(entry->nodeHash);
    }
    EXPECT_EQ(2u, hashes.size());
    EXPECT_EQ(plainReader->second->nodeHash, mixRenderContextKey(plainReader->second->nodeHash, 0));
}

// Under two different keys the reader renders twice; under equal keys once. The context-free
// ancestor renders once either way.
TEST_F(RenderContextRequest, ReaderRendersOncePerDistinctKey)
{
    {
        NodePtr ancestor, reader, fanOut;
        ASSERT_TRUE(buildFanOutGraph(2, 0, false, &ancestor, &reader, &fanOut));
        CountingTestRegistry::reset();
        // Each reader holds v + 1 + 1.
        EXPECT_EQ(2.f + 3.f, renderFirstPixel(fanOut, eRenderSchedulerModeTaskGraph));
        EXPECT_EQ(2, CountingTestRegistry::renders(reader));
        EXPECT_EQ(1, CountingTestRegistry::renders(ancestor));
        EXPECT_EQ(1, CountingTestRegistry::renders(fanOut));
    }
    getApp()->getProject()->reset(false, true);
    {
        NodePtr ancestor, reader, fanOut;
        ASSERT_TRUE(buildFanOutGraph(2, 1, true, &ancestor, &reader, &fanOut));
        CountingTestRegistry::reset();
        EXPECT_EQ(3.f + 3.f, renderFirstPixel(fanOut, eRenderSchedulerModeTaskGraph));
        EXPECT_EQ(1, CountingTestRegistry::renders(reader));
        EXPECT_EQ(1, CountingTestRegistry::renders(ancestor));
    }
}

// The legacy scheduler pulls each context serially from the fan-out's render.
TEST_F(RenderContextRequest, LegacyPullsEachContext)
{
    NodePtr ancestor, reader, fanOut;
    ASSERT_TRUE(buildFanOutGraph(3, 0, false, &ancestor, &reader, &fanOut));
    EXPECT_EQ(2.f + 3.f + 4.f, renderFirstPixel(fanOut, eRenderSchedulerModeLegacy));
    EXPECT_EQ(2.f + 3.f + 4.f, renderFirstPixel(fanOut, eRenderSchedulerModeTaskGraph));
}

// Both schedulers, and the task graph on one and several pool threads, render the fan-out alike.
TEST_F(RenderContextRequest, FanOutRendersAlikeAcrossSchedulersAndPoolSizes)
{
    NodePtr ancestor, reader, fanOut;
    ASSERT_TRUE(buildFanOutGraph(3, 0, false, &ancestor, &reader, &fanOut));
    const RenderMismatch m = renderBothWaysDirect(fanOut, 1., ViewIdx(0), 0 /*mipmapLevel*/, RectI(0, 0, kCountingTestSize, kCountingTestSize), std::vector<int> { 1, 4 });
    EXPECT_FALSE(m.any) << describe(m);
}

// A knob reading the test variable keeps an expression result per context: under two contexts on
// one frame its expression runs twice and gives two values, each read again from the cache. A
// knob reading no variable keeps one result for the frame.
TEST_F(RenderContextRequest, ExpressionResultsAreKeptPerContext)
{
    NodePtr reading = createWithDouble(kTestPluginIDContextReader, "value", 1.);
    NodePtr plain = createWithDouble(kTestPluginIDContextReader, "value", 1.);
    ASSERT_TRUE(reading && plain);
    KnobIPtr readingKnob = reading->getKnobByName("value");
    KnobIPtr plainKnob = plain->getKnobByName("value");
    ASSERT_TRUE(readingKnob && plainKnob);
    readingKnob->addContextReadSource(std::make_shared<TestVariableReadSource>());

    ASSERT_NO_THROW(readingKnob->setExpression(0, countingExpression("ctxExprReading"), true, true));
    ASSERT_NO_THROW(plainKnob->setExpression(0, countingExpression("ctxExprPlain"), true, true));
    std::string error, output;
    ASSERT_TRUE(Natron::Python::interpretPythonScript("import builtins\nbuiltins.ctxExprReading = 0\nbuiltins.ctxExprPlain = 0\n", &error, &output)) << error;
    readingKnob->clearExpressionsResults(0);
    plainKnob->clearExpressionsResults(0);

    const RenderContextPtr one = testContext(1.);
    const RenderContextPtr two = testContext(2.);
    const double underOne = valueUnder(reading, readingKnob, one);
    const double underTwo = valueUnder(reading, readingKnob, two);
    EXPECT_NE(underOne, underTwo);
    EXPECT_EQ((std::set<double> { 1., 2. }), (std::set<double> { underOne, underTwo }));
    EXPECT_EQ(underOne, valueUnder(reading, readingKnob, one));
    EXPECT_EQ(underTwo, valueUnder(reading, readingKnob, two));
    ASSERT_TRUE(Natron::Python::interpretPythonScript("import builtins\nassert builtins.ctxExprReading == 2, builtins.ctxExprReading\n", &error, &output)) << error;

    EXPECT_EQ(1., valueUnder(plain, plainKnob, one));
    EXPECT_EQ(1., valueUnder(plain, plainKnob, two));
    ASSERT_TRUE(Natron::Python::interpretPythonScript("import builtins\nassert builtins.ctxExprPlain == 1, builtins.ctxExprPlain\n", &error, &output)) << error;
}

// A fan-out into more contexts than a node's actions cache keeps states for still runs each
// context's region of definition once in the frame: the request pass reserves a state per context.
TEST_F(RenderContextRequest, ActionsCacheKeepsEveryContextOfTheFrame)
{
    const int count = std::max(64, appPTR->getHardwareIdealThreadCount() * 2) + 8;
    NodePtr ancestor, reader, fanOut;
    // From 1, so no context sets the variable to the 0 the empty context reads.
    ASSERT_TRUE(buildFanOutGraph(count, 1, false, &ancestor, &reader, &fanOut));
    ContextTestRegistry::reset();

    // Each reader holds v + 1 + 1.
    EXPECT_EQ(float(count * (count + 1) / 2 + 2 * count), renderFirstPixel(fanOut, eRenderSchedulerModeTaskGraph));
    for (int v = 1; v <= count; ++v) {
        EXPECT_EQ(1, ContextTestRegistry::regionsOfDefinition(reader.get(), v)) << "v = " << v;
    }
}

// Nested fan-outs of 2 x 64 x 64 render the reader under 8192 contexts: the frame renders, one
// warning naming the fan-outs is logged for the render and posted on the outermost one, and it is
// taken back once a render stays at the threshold.
TEST_F(RenderContextRequest, RunawayContextCountWarnsOncePerRender)
{
    NodePtr reader = createWithDouble(kTestPluginIDContextReader, "value", 1.);
    NodePtr inner = createFanOut(64, 0, false);
    NodePtr middle = createFanOut(64, 0, false);
    NodePtr outer = createFanOut(2, 0, false);
    ASSERT_TRUE(reader && inner && middle && outer);
    KnobString* variables = dynamic_cast<KnobString*>(reader->getKnobByName("variables").get());
    KnobString* innerVariable = dynamic_cast<KnobString*>(inner->getKnobByName("variable").get());
    KnobString* middleVariable = dynamic_cast<KnobString*>(middle->getKnobByName("variable").get());
    KnobString* outerVariable = dynamic_cast<KnobString*>(outer->getKnobByName("variable").get());
    ASSERT_TRUE(variables && innerVariable && middleVariable && outerVariable);
    variables->setValue("a,b,c");
    outerVariable->setValue("a");
    middleVariable->setValue("b");
    innerVariable->setValue("c");
    ASSERT_TRUE(inner->connectInput(reader, 0));
    ASSERT_TRUE(middle->connectInput(inner, 0));
    ASSERT_TRUE(outer->connectInput(middle, 0));

    const QString warning = QString::fromUtf8("contexts, more than the");
    appPTR->clearErrorLog_mt_safe();
    // The sum over a < 2, b < 64, c < 64 of the reader's a + b + c + 1.
    EXPECT_EQ(528384.f, renderFirstPixel(outer, eRenderSchedulerModeTaskGraph));
    EXPECT_EQ(1, errorLogEntriesWith(warning));
    EXPECT_TRUE(outer->hasPersistentMessage());
    EXPECT_FALSE(middle->hasPersistentMessage());
    EXPECT_FALSE(inner->hasPersistentMessage());

    // 1 x 64 x 64 is at the threshold.
    KnobInt* outerCount = dynamic_cast<KnobInt*>(outer->getKnobByName("count").get());
    ASSERT_TRUE(outerCount != NULL);
    outerCount->setValue(1);
    appPTR->clearErrorLog_mt_safe();
    EXPECT_EQ(262144.f, renderFirstPixel(outer, eRenderSchedulerModeTaskGraph));
    EXPECT_EQ(0, errorLogEntriesWith(warning));
    EXPECT_FALSE(outer->hasPersistentMessage());
}

// A consumer rendering under a context reads its input's region of definition (here through the
// host's default union) under that context: the request pass and both schedulers see the region
// the input has under each context.
TEST_F(RenderContextRequest, InputRegionIsReadUnderTheConsumersContext)
{
    NodePtr region = createNode(QString::fromUtf8(kTestPluginIDContextRegion));
    NodePtr unionNode = createNode(QString::fromUtf8(kTestPluginIDContextUnion));
    NodePtr fanOut = createFanOut(2, 1, false);
    ASSERT_TRUE(region && unionNode && fanOut);
    ASSERT_TRUE(unionNode->connectInput(region, 0));
    ASSERT_TRUE(fanOut->connectInput(unionNode, 0));

    FrameRequestMap request;
    ASSERT_NE(eStatusFailed, runRequestPass(fanOut, &request));
    const std::vector<NodeFrameRequestPtr> unionEntries = entriesOf(request, unionNode);
    ASSERT_EQ(2u, unionEntries.size());
    for (const NodeFrameRequestPtr& entry : unionEntries) {
        ASSERT_TRUE(bool(entry->context));
        const double v = contextTestValue(entry->context);
        const FrameViewRequest* frameView = entry->findFrameViewRequest(1., ViewIdx(0));
        ASSERT_TRUE(frameView != NULL);
        EXPECT_EQ(ContextRegionTestEffect::regionWidthUnder(v), frameView->globalData.rod.x2) << "v = " << v;
    }

    for (RenderSchedulerModeEnum mode : { eRenderSchedulerModeTaskGraph, eRenderSchedulerModeLegacy }) {
        ContextTestRegistry::reset();
        // Each context's region node holds v.
        EXPECT_EQ(1.f + 2.f, renderFirstPixel(fanOut, mode)) << (int)mode;
        for (double v : { 1., 2. }) {
            for (double right : ContextTestRegistry::regionRights(unionNode.get(), v)) {
                EXPECT_EQ(ContextRegionTestEffect::regionWidthUnder(v), right) << "v = " << v << ", mode " << (int)mode;
            }
        }
    }
}

// A knob whose expression reads another node's context-reading knob keeps a result per context,
// though it has no read source of its own.
TEST_F(RenderContextRequest, ExpressionReadingAContextKnobIsKeptPerContext)
{
    NodePtr source = createWithDouble(kTestPluginIDContextReader, "value", 1.);
    NodePtr reading = createWithDouble(kTestPluginIDContextReader, "value", 1.);
    ASSERT_TRUE(source && reading);
    KnobString* readingVariables = dynamic_cast<KnobString*>(reading->getKnobByName("variables").get());
    ASSERT_TRUE(readingVariables != NULL);
    // The node reads no variable of its own.
    readingVariables->setValue("");
    KnobIPtr sourceKnob = source->getKnobByName("value");
    KnobIPtr readingKnob = reading->getKnobByName("value");
    ASSERT_TRUE(sourceKnob && readingKnob);
    sourceKnob->addContextReadSource(std::make_shared<TestVariableReadSource>());
    EXPECT_FALSE(readingKnob->hasContextReadSources());

    ASSERT_NO_THROW(sourceKnob->setExpression(0, countingExpression("ctxExprSource"), true, true));
    ASSERT_NO_THROW(readingKnob->setExpression(0, source->getScriptName() + ".value.getValue(0)", false, true));
    std::string error, output;
    ASSERT_TRUE(Natron::Python::interpretPythonScript("import builtins\nbuiltins.ctxExprSource = 0\n", &error, &output)) << error;
    sourceKnob->clearExpressionsResults(0);
    readingKnob->clearExpressionsResults(0);

    const RenderContextPtr one = testContext(1.);
    const RenderContextPtr two = testContext(2.);
    const double underOne = valueUnder(reading, readingKnob, one);
    const double underTwo = valueUnder(reading, readingKnob, two);
    EXPECT_NE(underOne, underTwo);
    EXPECT_EQ((std::set<double> { 1., 2. }), (std::set<double> { underOne, underTwo }));
    EXPECT_EQ(underOne, valueUnder(reading, readingKnob, one));
    EXPECT_EQ(underTwo, valueUnder(reading, readingKnob, two));
    ASSERT_TRUE(Natron::Python::interpretPythonScript("import builtins\nassert builtins.ctxExprSource == 2, builtins.ctxExprSource\n", &error, &output)) << error;
}

// Nodes requesting each other under named contexts, here one node under two contexts that request
// each other, would wait on each other: the request pass fails with a persistent error.
TEST_F(RenderContextRequest, ContextRequestCycleFails)
{
    NodePtr chain = createNode(QString::fromUtf8(kTestPluginIDContextChain));
    ASSERT_TRUE(bool(chain));
    KnobBool* cycle = dynamic_cast<KnobBool*>(chain->getKnobByName("cycle").get());
    ASSERT_TRUE(cycle != NULL);
    cycle->setValue(true);
    NodePtr fanOut = createFanOut(1, 0, false);
    ASSERT_TRUE(bool(fanOut));
    ASSERT_TRUE(fanOut->connectInput(chain, 0));

    FrameRequestMap request;
    EXPECT_EQ(eStatusFailed, runRequestPass(fanOut, &request));
    EXPECT_TRUE(chain->hasPersistentMessage());
}

// A node requesting its own output at the previous frame under the context it renders under does
// not wait on itself: the pass plans both frames under the one key and both schedulers render.
TEST_F(RenderContextRequest, SameKeyRequestAtThePreviousFrameIsAllowed)
{
    NodePtr chain = createNode(QString::fromUtf8(kTestPluginIDContextChain));
    ASSERT_TRUE(bool(chain));
    KnobBool* previousFrame = dynamic_cast<KnobBool*>(chain->getKnobByName("previousFrame").get());
    ASSERT_TRUE(previousFrame != NULL);
    previousFrame->setValue(true);
    NodePtr fanOut = createFanOut(1, 2, false);
    ASSERT_TRUE(bool(fanOut));
    ASSERT_TRUE(fanOut->connectInput(chain, 0));

    FrameRequestMap request;
    ASSERT_NE(eStatusFailed, runRequestPass(fanOut, &request));
    EXPECT_FALSE(chain->hasPersistentMessage());
    const std::vector<NodeFrameRequestPtr> chainEntries = entriesOf(request, chain);
    ASSERT_EQ(1u, chainEntries.size());
    EXPECT_NE(0u, chainEntries.front()->contextKey);
    EXPECT_TRUE(chainEntries.front()->findFrameViewRequest(1., ViewIdx(0)) != NULL);
    EXPECT_TRUE(chainEntries.front()->findFrameViewRequest(0., ViewIdx(0)) != NULL);

    EXPECT_EQ(2.f, renderFirstPixel(fanOut, eRenderSchedulerModeTaskGraph));
    EXPECT_EQ(2.f, renderFirstPixel(fanOut, eRenderSchedulerModeLegacy));
}

// An effect requesting its input under a single context of its own through getInputContexts()
// alone pulls it under that context with a plain getImage(), as the pass planned it.
TEST_F(RenderContextRequest, SingleInputContextIsPulledWithoutAScope)
{
    NodePtr reader = createWithDouble(kTestPluginIDContextReader, "value", 1.);
    NodePtr set = createNode(QString::fromUtf8(kTestPluginIDContextSet));
    ASSERT_TRUE(reader && set);
    ASSERT_TRUE(set->connectInput(reader, 0));

    FrameRequestMap request;
    ASSERT_NE(eStatusFailed, runRequestPass(set, &request));
    const std::vector<NodeFrameRequestPtr> readerEntries = entriesOf(request, reader);
    ASSERT_EQ(1u, readerEntries.size());
    EXPECT_EQ(5., contextTestValue(readerEntries.front()->context));

    // The reader holds its value 1 plus v = 5.
    CountingTestRegistry::reset();
    EXPECT_EQ(6.f, renderFirstPixel(set, eRenderSchedulerModeTaskGraph));
    EXPECT_EQ(1, CountingTestRegistry::renders(reader));
    EXPECT_EQ(6.f, renderFirstPixel(set, eRenderSchedulerModeLegacy));
}

// A fan-out nested in another setting the same variable reads nothing of it from upstream: it
// renders under the empty context once, not once per outer context.
TEST_F(RenderContextRequest, NestedFanOutsSettingOneVariableRenderTheInnerOnce)
{
    NodePtr reader = createWithDouble(kTestPluginIDContextReader, "value", 1.);
    NodePtr inner = createFanOut(3, 0, false);
    NodePtr outer = createFanOut(2, 0, false);
    ASSERT_TRUE(reader && inner && outer);
    ASSERT_TRUE(inner->connectInput(reader, 0));
    ASSERT_TRUE(outer->connectInput(inner, 0));

    FrameRequestMap request;
    ASSERT_NE(eStatusFailed, runRequestPass(outer, &request));
    const std::vector<NodeFrameRequestPtr> innerEntries = entriesOf(request, inner);
    ASSERT_EQ(1u, innerEntries.size());
    EXPECT_EQ(0u, innerEntries.front()->contextKey);
    EXPECT_EQ(3u, entriesOf(request, reader).size());

    // Twice the inner's sum over v < 3 of v + 1.
    CountingTestRegistry::reset();
    EXPECT_EQ(12.f, renderFirstPixel(outer, eRenderSchedulerModeTaskGraph));
    EXPECT_EQ(1, CountingTestRegistry::renders(inner));
    EXPECT_EQ(3, CountingTestRegistry::renders(reader));
    EXPECT_EQ(12.f, renderFirstPixel(outer, eRenderSchedulerModeLegacy));
}

// Past the threshold, the warning goes on the fan-out under the empty context whose own contexts
// pass it, not on an earlier one that fans out a variable nothing upstream reads.
TEST_F(RenderContextRequest, ContextCountWarningGoesOnTheFanOutPastTheThreshold)
{
    NodePtr reader = createWithDouble(kTestPluginIDContextReader, "value", 1.);
    NodePtr inner = createFanOut(64, 0, false);
    NodePtr middle = createFanOut(64, 0, false);
    NodePtr outer = createFanOut(2, 0, false);
    NodePtr sibling = createFanOut(2, 0, false);
    ASSERT_TRUE(reader && inner && middle && outer && sibling);
    KnobString* variables = dynamic_cast<KnobString*>(reader->getKnobByName("variables").get());
    KnobString* innerVariable = dynamic_cast<KnobString*>(inner->getKnobByName("variable").get());
    KnobString* middleVariable = dynamic_cast<KnobString*>(middle->getKnobByName("variable").get());
    KnobString* outerVariable = dynamic_cast<KnobString*>(outer->getKnobByName("variable").get());
    KnobString* siblingVariable = dynamic_cast<KnobString*>(sibling->getKnobByName("variable").get());
    ASSERT_TRUE(variables && innerVariable && middleVariable && outerVariable && siblingVariable);
    variables->setValue("a,b,c");
    outerVariable->setValue("a");
    middleVariable->setValue("b");
    innerVariable->setValue("c");
    siblingVariable->setValue("w");
    ASSERT_TRUE(inner->connectInput(reader, 0));
    ASSERT_TRUE(middle->connectInput(inner, 0));
    ASSERT_TRUE(outer->connectInput(middle, 0));
    ASSERT_TRUE(sibling->connectInput(outer, 0));

    FrameRequestMap request;
    ASSERT_NE(eStatusFailed, runRequestPass(sibling, &request));
    EXPECT_EQ(1u, entriesOf(request, outer).size());
    EXPECT_TRUE(outer->hasPersistentMessage());
    EXPECT_FALSE(sibling->hasPersistentMessage());
    EXPECT_FALSE(middle->hasPersistentMessage());
    EXPECT_FALSE(inner->hasPersistentMessage());
}

// A fan-out that warned and is then disabled no longer fans anything out: the next pass takes its
// warning back.
TEST_F(RenderContextRequest, DisablingTheFanOutTakesTheWarningBack)
{
    NodePtr reader = createWithDouble(kTestPluginIDContextReader, "value", 1.);
    NodePtr fanOut = createFanOut(kContextCountWarningThreshold + 1, 0, false);
    ASSERT_TRUE(reader && fanOut);
    ASSERT_TRUE(fanOut->connectInput(reader, 0));

    {
        FrameRequestMap request;
        ASSERT_NE(eStatusFailed, runRequestPass(fanOut, &request));
        EXPECT_TRUE(fanOut->hasPersistentMessage());
    }
    fanOut->setNodeDisabled(true);
    {
        FrameRequestMap request;
        ASSERT_NE(eStatusFailed, runRequestPass(fanOut, &request));
        EXPECT_TRUE(request.contextFanOuts.empty());
        EXPECT_FALSE(fanOut->hasPersistentMessage());
    }
}
