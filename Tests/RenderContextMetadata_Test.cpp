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

#include <map>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "ContextTestEffects.h"
#include "CountingTestEffect.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderContext.h"
#include "Engine/RenderStats.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

class RenderContextMetadata
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
        BaseTest::TearDown();
    }

    NodePtr createFormat(bool reads)
    {
        NodePtr node = createNode(QString::fromUtf8(kTestPluginIDContextFormat));
        KnobBool* readsKnob = node ? dynamic_cast<KnobBool*>(node->getKnobByName("reads").get()) : 0;
        if (!readsKnob) {
            return NodePtr();
        }
        readsKnob->setValue(reads);

        return node;
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

    // For each of node's entries under a nonempty context, expects the metadata the request pass
    // derived to carry the format that context gives, and returns how many there were.
    int expectFormatsPerContext(const FrameRequestMap& request,
                                const NodePtr& node)
    {
        int entries = 0;
        for (FrameRequestMap::const_iterator it = request.begin(); it != request.end(); ++it) {
            if ((it->first.node != node) || (it->first.ctxKey == 0)) {
                continue;
            }
            ++entries;
            const ParallelRenderArgsPtr args = request.getContextArgs(it->first);
            if (!args || !args->metadata) {
                ADD_FAILURE() << node->getScriptName_mt_safe() << " has no metadata under a context";
                continue;
            }
            const double v = contextTestValue(args->context);
            EXPECT_EQ(ContextFormatTestEffect::formatWidth(v), args->metadata->getOutputFormat().width())
                << node->getScriptName_mt_safe() << " under v = " << v;
        }

        return entries;
    }
};

// A node whose format reads the test variable derives its metadata under each context it renders
// under, and a node below it passes that format on per context; the context-free ancestor above it
// derives its metadata once.
TEST_F(RenderContextMetadata, MetadataIsDerivedPerContext)
{
    NodePtr ancestor = createFormat(false);
    NodePtr format = createFormat(true);
    NodePtr downstream = createNode(QString::fromUtf8(kTestPluginIDCounting));
    NodePtr fanOut = createNode(QString::fromUtf8(kTestPluginIDContextFanOut));
    ASSERT_TRUE(ancestor && format && downstream && fanOut);
    KnobInt* count = dynamic_cast<KnobInt*>(fanOut->getKnobByName("count").get());
    KnobInt* first = dynamic_cast<KnobInt*>(fanOut->getKnobByName("first").get());
    ASSERT_TRUE(count && first);
    count->setValue(3);
    first->setValue(0);
    ASSERT_TRUE(format->connectInput(ancestor, 0));
    ASSERT_TRUE(downstream->connectInput(format, 0));
    ASSERT_TRUE(fanOut->connectInput(downstream, 0));

    ContextFormatTestEffect* ancestorEffect = dynamic_cast<ContextFormatTestEffect*>(ancestor->getEffectInstance().get());
    ASSERT_TRUE(ancestorEffect != NULL);
    ancestor->getEffectInstance()->dropCachedMetadataDownstream();
    // The render-args setup reads each node's time-invariant answer, which runs the same derivation
    // under the empty context; it is taken here so the count below sees the per-frame one alone.
    ASSERT_TRUE(ancestor->getEffectInstance()->getTimeInvariantMetadata() != NULL);
    const int derivationsBefore = ancestorEffect->derivations();

    FrameRequestMap request;
    ASSERT_NE(eStatusFailed, runRequestPass(fanOut, &request));
    EXPECT_EQ(3, expectFormatsPerContext(request, format));
    EXPECT_EQ(3, expectFormatsPerContext(request, downstream));
    EXPECT_EQ(derivationsBefore + 1, ancestorEffect->derivations());
}

namespace {
RenderContextPtr
contextWithTestValue(double v)
{
    return RenderContext::Builder().set(contextTestVariable(), RenderContextValue(v)).build();
}
} // anon namespace

// Asked under a context it reads, a node keeps its metadata under its hash with that context's key
// folded in and finds it there again; another context derives and keeps its own.
TEST_F(RenderContextMetadata, MetadataDerivedUnderAContextIsCachedUnderItsKey)
{
    NodePtr format = createFormat(true);
    ASSERT_TRUE(bool(format));
    EffectInstancePtr effect = format->getEffectInstance();
    ContextFormatTestEffect* formatEffect = dynamic_cast<ContextFormatTestEffect*>(effect.get());
    ASSERT_TRUE(formatEffect != NULL);
    const RenderContextPtr two = contextWithTestValue(2.);
    const RenderContextPtr three = contextWithTestValue(3.);
    effect->dropCachedMetadataDownstream();
    ASSERT_TRUE(effect->getTimeInvariantMetadata() != NULL);
    const int before = formatEffect->derivations();

    {
        ActionContextScope underTwo(true, two);
        const U64 contextKey = effect->getCurrentRenderContextKey();
        ASSERT_NE(0u, contextKey);
        U64 hash = 0;
        NodeMetadataConstPtr metadata = effect->getMetadata(1., ViewIdx(0), &hash);
        ASSERT_TRUE(metadata != NULL);
        EXPECT_EQ(ContextFormatTestEffect::formatWidth(2.), metadata->getOutputFormat().width());
        EXPECT_EQ(mixRenderContextKey(format->getHashValue(), contextKey), hash);
        EXPECT_NE(format->getHashValue(), hash);
    }
    const int afterFirst = formatEffect->derivations();
    EXPECT_GT(afterFirst, before);

    {
        ActionContextScope underTwo(true, two);
        U64 hash = 0;
        NodeMetadataConstPtr metadata = effect->getMetadata(1., ViewIdx(0), &hash);
        ASSERT_TRUE(metadata != NULL);
        EXPECT_EQ(ContextFormatTestEffect::formatWidth(2.), metadata->getOutputFormat().width());
        EXPECT_NE(0u, hash);
    }
    EXPECT_EQ(afterFirst, formatEffect->derivations()) << "the answer under v = 2 was not reused";

    {
        ActionContextScope underThree(true, three);
        NodeMetadataConstPtr metadata = effect->getMetadata(1., ViewIdx(0));
        ASSERT_TRUE(metadata != NULL);
        EXPECT_EQ(ContextFormatTestEffect::formatWidth(3.), metadata->getOutputFormat().width());
    }
    const int afterThree = formatEffect->derivations();
    EXPECT_GT(afterThree, afterFirst);

    {
        ActionContextScope underTwo(true, two);
        NodeMetadataConstPtr metadata = effect->getMetadata(1., ViewIdx(0));
        ASSERT_TRUE(metadata != NULL);
        EXPECT_EQ(ContextFormatTestEffect::formatWidth(2.), metadata->getOutputFormat().width());
    }
    EXPECT_EQ(afterThree, formatEffect->derivations()) << "the answer under v = 3 replaced the one under v = 2";
}

// An upstream edit made and reverted while a node derives its metadata under a context brings the
// node's hash back, but the derivation overlapped it: nothing is kept under that hash, so the next
// ask derives again, and keeps that answer.
TEST_F(RenderContextMetadata, EditOverlappingADerivationUnderAContextStoresNothing)
{
    NodePtr ancestor = createFormat(false);
    NodePtr format = createFormat(true);
    ASSERT_TRUE(ancestor && format);
    ASSERT_TRUE(format->connectInput(ancestor, 0));
    KnobBool* ancestorReads = dynamic_cast<KnobBool*>(ancestor->getKnobByName("reads").get());
    ASSERT_TRUE(ancestorReads != NULL);
    EffectInstancePtr effect = format->getEffectInstance();
    ContextFormatTestEffect* formatEffect = dynamic_cast<ContextFormatTestEffect*>(effect.get());
    ASSERT_TRUE(formatEffect != NULL);
    const RenderContextPtr two = contextWithTestValue(2.);
    ancestor->getEffectInstance()->dropCachedMetadataDownstream();
    ASSERT_TRUE(ancestor->getEffectInstance()->getTimeInvariantMetadata() != NULL);
    ASSERT_TRUE(effect->getTimeInvariantMetadata() != NULL);
    const U64 hashBefore = format->getHashValue();

    formatEffect->setDerivationHook([ancestorReads]() {
        ancestorReads->setValue(true);
        ancestorReads->setValue(false);
    });
    {
        ActionContextScope underTwo(true, two);
        // The two-argument getMetadata() derives once, whether or not an edit overlapped it.
        NodeMetadataConstPtr metadata = effect->getMetadata(1., ViewIdx(0));
        ASSERT_TRUE(metadata != NULL);
    }
    ASSERT_EQ(hashBefore, format->getHashValue());
    const int afterOverlapped = formatEffect->derivations();

    {
        ActionContextScope underTwo(true, two);
        NodeMetadataConstPtr metadata = effect->getMetadata(1., ViewIdx(0));
        ASSERT_TRUE(metadata != NULL);
        EXPECT_EQ(ContextFormatTestEffect::formatWidth(2.), metadata->getOutputFormat().width());
    }
    const int afterClean = formatEffect->derivations();
    EXPECT_GT(afterClean, afterOverlapped) << "the overlapped derivation was kept";

    {
        ActionContextScope underTwo(true, two);
        NodeMetadataConstPtr metadata = effect->getMetadata(1., ViewIdx(0));
        ASSERT_TRUE(metadata != NULL);
    }
    EXPECT_EQ(afterClean, formatEffect->derivations()) << "the clean derivation was not kept";
}
