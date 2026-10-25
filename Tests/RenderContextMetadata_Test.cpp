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
