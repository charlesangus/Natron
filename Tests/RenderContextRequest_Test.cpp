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

#include <list>
#include <map>
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

} // namespace

class RenderContextRequest
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
