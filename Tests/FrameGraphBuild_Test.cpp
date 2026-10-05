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
#include <chrono>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QFuture>
#include <QtCore/QThreadPool>

#include "BaseTest.h"
#include "PassThroughRoDTestEffect.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/RenderStats.h"
#include "Engine/Settings.h"
#include "Engine/TLSHolder.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kFrameBlendPluginID = "net.sf.openfx.FrameBlend";
const int kWindowSize = 64;

typedef std::tuple<std::string, double, int> CallKey;
typedef std::set<CallKey> CallSet;

NodePtr
createNamedNode(const AppInstancePtr& app,
                const char* pluginID,
                const std::string& name)
{
    CreateNodeArgs args(pluginID, app->getProject());

    args.setProperty<std::string>(kCreateNodeArgsPropNodeInitialName, name);

    return app->createNode(args);
}

CallKey
makeKey(const NodePtr& node,
        double time,
        ViewIdx view)
{
    return CallKey(node->getScriptName_mt_safe(), time, view.value());
}

std::string
describeKeys(const CallSet& keys)
{
    std::ostringstream ss;

    for (CallSet::const_iterator it = keys.begin(); it != keys.end(); ++it) {
        ss << " (" << std::get<0>(*it) << ", " << std::get<1>(*it) << ", " << std::get<2>(*it) << ")";
    }

    return ss.str();
}

std::string
describeDifference(const CallSet& edges,
                   const CallSet& calls)
{
    CallSet onlyEdges;
    CallSet onlyCalls;

    for (CallSet::const_iterator it = edges.begin(); it != edges.end(); ++it) {
        if (!calls.count(*it)) {
            onlyEdges.insert(*it);
        }
    }
    for (CallSet::const_iterator it = calls.begin(); it != calls.end(); ++it) {
        if (!edges.count(*it)) {
            onlyCalls.insert(*it);
        }
    }

    return "edges without a call:" + describeKeys(onlyEdges) + "; calls without an edge:" + describeKeys(onlyCalls);
}

void
setColor(const NodePtr& node,
         const char* knobName,
         double r,
         double g,
         double b,
         double a)
{
    KnobColor* color = dynamic_cast<KnobColor*>(node->getKnobByName(knobName).get());

    ASSERT_TRUE(color != NULL) << node->getScriptName() << "." << knobName;
    ASSERT_EQ(4, color->getDimension());
    color->setValues(r, g, b, a, ViewSpec::all(), eValueChangedReasonNatronInternalEdited);
}

void
setGradeMultiply(const NodePtr& grade,
                 double value)
{
    KnobColor* multiply = dynamic_cast<KnobColor*>(grade->getKnobByName("multiply").get());

    ASSERT_TRUE(multiply != NULL) << grade->getScriptName();
    for (int d = 0; d < multiply->getDimension(); ++d) {
        multiply->setValue(value, ViewSpec::all(), d);
    }
}

// Node creation and connection query RoDs, so the pass would otherwise start with the whole chain already cached.
void
clearActionsCaches(const std::vector<NodePtr>& nodes)
{
    for (std::vector<NodePtr>::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        (*it)->getEffectInstance()->clearActionsCache();
    }
}

struct EdgeSummary {
    CallSet targets;
    std::size_t count;

    EdgeSummary()
        : targets()
        , count(0)
    {
    }
};

} // namespace

class FrameGraphBuild
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    /**
     * @brief Runs the request pass on root and then renders root the way the renderers do, with the request data
     * installed and a RenderStats recording every renderRoI call.
     **/
    bool buildAndRender(const NodePtr& root,
                        double time,
                        FrameRequestMap* request,
                        CallSet* calls,
                        std::string* error)
    {
        appPTR->clearAllCaches();

        AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);
        RenderStatsPtr stats = std::make_shared<RenderStats>(false);
        stats->setTrackRenderRoICalls(true);
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
                                                 stats);

        const RectI window(0, 0, kWindowSize, kWindowSize);
        const RectD canonicalWindow(0., 0., kWindowSize, kWindowSize);

        if (EffectInstance::computeRequestPass(time, ViewIdx(0), 0 /*mipmapLevel*/, canonicalWindow, root, *request) == eStatusFailed) {
            *error = "request pass failed";

            return false;
        }
        frameRenderArgs.updateNodesRequest(*request);

        std::list<ImageLayerDesc> components;
        components.push_back(ImageLayerDesc::getRGBAComponents());
        EffectInstance::RenderRoIArgs args(time,
                                           RenderScale::identity,
                                           0 /*mipmapLevel*/,
                                           ViewIdx(0),
                                           false /*byPassCache*/,
                                           window,
                                           RectD(),
                                           components,
                                           eImageBitDepthFloat,
                                           false /*calledFromGetImage*/,
                                           0 /*caller*/,
                                           eStorageModeRAM,
                                           time);
        std::map<ImageLayerDesc, ImagePtr> layers;
        if ((root->getEffectInstance()->renderRoI(args, &layers) != EffectInstance::eRenderRoIRetCodeOk) || layers.empty()) {
            *error = "render failed";

            return false;
        }

        const CallKey rootKey = makeKey(root, time, ViewIdx(0));
        const std::map<CallKey, int> recorded = stats->getRenderRoICalls();
        calls->clear();
        for (std::map<CallKey, int>::const_iterator it = recorded.begin(); it != recorded.end(); ++it) {
            if (it->first != rootKey) {
                calls->insert(it->first);
            }
        }

        return true;
    }

    /**
     * @brief Collects every edge target and checks the structural invariants of the request map: each target exists,
     * consumer counts equal in-degree, and every target is numbered before its consumer.
     **/
    void collectEdges(const FrameRequestMap& request,
                      EdgeSummary* summary)
    {
        std::map<const FrameViewRequest*, int> inDegree;

        for (FrameRequestMap::const_iterator it = request.begin(); it != request.end(); ++it) {
            for (NodeFrameViewRequestData::const_iterator fv = it->second->frames.begin(); fv != it->second->frames.end(); ++fv) {
                const FrameViewRequest& consumer = fv->second;
                EXPECT_GE(consumer.dfsPostOrder, 0) << it->first->getScriptName();
                for (std::vector<FrameViewRequest::TaskEdge>::const_iterator dep = consumer.dependencies.begin(); dep != consumer.dependencies.end(); ++dep) {
                    NodePtr target = dep->node.lock();
                    ASSERT_TRUE(bool(target));
                    summary->targets.insert(makeKey(target, dep->time, dep->view));
                    ++summary->count;
                    EXPECT_EQ(0u, dep->mipmapLevel);

                    const FrameViewRequest* targetRequest = request.findFrameViewRequest(target, dep->time, dep->view);
                    ASSERT_TRUE(targetRequest != NULL) << target->getScriptName() << " at " << dep->time;
                    ++inDegree[targetRequest];
                    EXPECT_LT(targetRequest->dfsPostOrder, consumer.dfsPostOrder) << it->first->getScriptName() << " -> " << target->getScriptName();
                    EXPECT_FALSE(targetRequest->componentsRequested.empty()) << target->getScriptName();
                }
            }
        }

        for (FrameRequestMap::const_iterator it = request.begin(); it != request.end(); ++it) {
            for (NodeFrameViewRequestData::const_iterator fv = it->second->frames.begin(); fv != it->second->frames.end(); ++fv) {
                std::map<const FrameViewRequest*, int>::const_iterator found = inDegree.find(&fv->second);
                const int expected = (found == inDegree.end()) ? 0 : found->second;
                EXPECT_EQ(expected, fv->second.consumers) << it->first->getScriptName() << " at " << fv->first.time;
            }
        }
    }

    void expectEdgesMatchCalls(const NodePtr& root,
                               double time,
                               FrameRequestMap* request,
                               EdgeSummary* summary,
                               CallSet* calls)
    {
        std::string error;

        ASSERT_TRUE(buildAndRender(root, time, request, calls, &error)) << error;
        ASSERT_NO_FATAL_FAILURE(collectEdges(*request, summary));
        EXPECT_TRUE(summary->targets == *calls) << describeDifference(summary->targets, *calls);
    }
};

TEST_F(FrameGraphBuild, ChainOfFiftyWalksEachNodeOnce)
{
    const AppInstancePtr app = getApp();
    std::vector<NodePtr> chain;

    NodePtr constant = createNamedNode(app, PLUGINID_OFX_CONSTANT, "ChainConstant");
    ASSERT_TRUE(bool(constant));
    ASSERT_NO_FATAL_FAILURE(setColor(constant, "color", 0.5, 0.5, 0.5, 1.));
    chain.push_back(constant);
    for (int i = 1; i < 50; ++i) {
        NodePtr grade = createNamedNode(app, PLUGINID_OFX_GRADE, "ChainGrade" + std::to_string(i));
        ASSERT_TRUE(bool(grade));
        ASSERT_TRUE(grade->connectInput(chain.back(), 0));
        ASSERT_NO_FATAL_FAILURE(setGradeMultiply(grade, 1.01));
        chain.push_back(grade);
    }

    FrameRequestMap request;
    EdgeSummary summary;
    CallSet calls;
    ASSERT_NO_FATAL_FAILURE(expectEdgesMatchCalls(chain.back(), 1., &request, &summary, &calls));

    EXPECT_EQ(49u, summary.count);
    EXPECT_EQ(49u, summary.targets.size());
    EXPECT_EQ(chain.size(), request.size());
    for (std::size_t i = 0; i < chain.size(); ++i) {
        FrameRequestMap::const_iterator found = request.find(chain[i]);
        ASSERT_TRUE(found != request.end()) << chain[i]->getScriptName();
        ASSERT_EQ(1u, found->second->frames.size()) << chain[i]->getScriptName();
        const FrameViewRequest& fv = found->second->frames.begin()->second;
        EXPECT_EQ(i + 1 < chain.size() ? 1 : 0, fv.consumers) << chain[i]->getScriptName();
        EXPECT_EQ(i > 0 ? 1u : 0u, fv.dependencies.size()) << chain[i]->getScriptName();
        // A chain has a single depth-first post-order, so each number is fixed by the node's position.
        EXPECT_EQ((int)i, fv.dfsPostOrder) << chain[i]->getScriptName();
    }
    EXPECT_EQ((int)chain.size(), request.nextDfsPostOrder);
}

TEST_F(FrameGraphBuild, DeepChainRequestPassFitsAPoolThreadStack)
{
    const AppInstancePtr app = getApp();
    // A recursive pass, or a cold RoD query on the root recursing through each Grade's default RoD, takes about
    // 2.2 kB of stack per node, over 4 MB for this chain. A 2 MB pool thread checks that neither grows with depth.
    const int nGrades = 2000;
    const int poolStackBytes = 2 * 1024 * 1024;
    std::vector<NodePtr> chain;

    const std::chrono::steady_clock::time_point buildStart = std::chrono::steady_clock::now();
    NodePtr constant = createNamedNode(app, PLUGINID_OFX_CONSTANT, "DeepConstant");
    ASSERT_TRUE(bool(constant));
    ASSERT_NO_FATAL_FAILURE(setColor(constant, "color", 0.5, 0.5, 0.5, 1.));
    chain.push_back(constant);
    for (int i = 1; i <= nGrades; ++i) {
        NodePtr grade = createNamedNode(app, PLUGINID_OFX_GRADE, "DeepGrade" + std::to_string(i));
        ASSERT_TRUE(bool(grade));
        ASSERT_TRUE(grade->connectInput(chain.back(), 0));
        ASSERT_NO_FATAL_FAILURE(setGradeMultiply(grade, 1.01));
        chain.push_back(grade);
    }
    const NodePtr root = chain.back();
    clearActionsCaches(chain);
    const std::chrono::steady_clock::time_point passStart = std::chrono::steady_clock::now();

    FrameRequestMap request;
    StatusEnum stat = eStatusFailed;
    QThreadPool smallStackPool;
    smallStackPool.setStackSize(poolStackBytes);
    QFuture<void> future = QtConcurrent::run(&smallStackPool, [&]() {
        const RectD canonicalWindow(0., 0., kWindowSize, kWindowSize);

        stat = EffectInstance::computeRequestPass(1., ViewIdx(0), 0 /*mipmapLevel*/, canonicalWindow, root, request);
        appPTR->getAppTLS()->cleanupTLSForThread();
    });
    future.waitForFinished();
    smallStackPool.waitForDone();
    const std::chrono::steady_clock::time_point passEnd = std::chrono::steady_clock::now();
    std::cout << "DeepChain " << nGrades << " grades: build+connect "
              << std::chrono::duration_cast<std::chrono::milliseconds>(passStart - buildStart).count()
              << " ms, request pass "
              << std::chrono::duration_cast<std::chrono::milliseconds>(passEnd - passStart).count() << " ms"
              << std::endl;

    ASSERT_EQ(eStatusOK, stat);
    EXPECT_EQ(chain.size(), request.size());

    EdgeSummary summary;
    ASSERT_NO_FATAL_FAILURE(collectEdges(request, &summary));
    EXPECT_EQ((std::size_t)nGrades, summary.count);

    const FrameViewRequest* constantRequest = request.findFrameViewRequest(constant, 1., ViewIdx(0));
    const FrameViewRequest* rootRequest = request.findFrameViewRequest(root, 1., ViewIdx(0));
    ASSERT_TRUE(constantRequest != NULL);
    ASSERT_TRUE(rootRequest != NULL);
    EXPECT_EQ(0, constantRequest->dfsPostOrder);
    EXPECT_EQ(nGrades, rootRequest->dfsPostOrder);
}

TEST_F(FrameGraphBuild, DeepChainOfFourThousandRequestPassOnDefaultStack)
{
    const AppInstancePtr app = getApp();
    const int nPassThroughs = 4000;
    std::vector<NodePtr> chain;

    const std::chrono::steady_clock::time_point buildStart = std::chrono::steady_clock::now();
    NodePtr source = createNamedNode(app, kTestPluginIDPassThroughRoD, "PassThroughSource");
    ASSERT_TRUE(bool(source));
    chain.push_back(source);
    for (int i = 1; i <= nPassThroughs; ++i) {
        NodePtr passThrough = createNamedNode(app, kTestPluginIDPassThroughRoD, "PassThrough" + std::to_string(i));
        ASSERT_TRUE(bool(passThrough));
        ASSERT_TRUE(passThrough->connectInput(chain.back(), 0));
        chain.push_back(passThrough);
    }
    const NodePtr root = chain.back();
    clearActionsCaches(chain);
    const std::chrono::steady_clock::time_point passStart = std::chrono::steady_clock::now();

    FrameRequestMap request;
    StatusEnum stat = eStatusFailed;
    // Its own pool: the global pool's threads have an enlarged stack that would hide a recursion.
    QThreadPool defaultStackPool;
    QFuture<void> future = QtConcurrent::run(&defaultStackPool, [&]() {
        const RectD canonicalWindow(0., 0., kPassThroughRoDTestSize, kPassThroughRoDTestSize);

        stat = EffectInstance::computeRequestPass(1., ViewIdx(0), 0 /*mipmapLevel*/, canonicalWindow, root, request);
        appPTR->getAppTLS()->cleanupTLSForThread();
    });
    future.waitForFinished();
    defaultStackPool.waitForDone();
    const std::chrono::steady_clock::time_point passEnd = std::chrono::steady_clock::now();
    std::cout << "DeepChain " << nPassThroughs << " pass-throughs: build+connect "
              << std::chrono::duration_cast<std::chrono::milliseconds>(passStart - buildStart).count()
              << " ms, request pass "
              << std::chrono::duration_cast<std::chrono::milliseconds>(passEnd - passStart).count() << " ms"
              << std::endl;

    ASSERT_EQ(eStatusOK, stat);
    EXPECT_EQ(chain.size(), request.size());

    EdgeSummary summary;
    ASSERT_NO_FATAL_FAILURE(collectEdges(request, &summary));
    EXPECT_EQ((std::size_t)nPassThroughs, summary.count);

    for (std::size_t i = 0; i < chain.size(); ++i) {
        const FrameViewRequest* fv = request.findFrameViewRequest(chain[i], 1., ViewIdx(0));
        ASSERT_TRUE(fv != NULL) << chain[i]->getScriptName();
        EXPECT_EQ((int)i, fv->dfsPostOrder) << chain[i]->getScriptName();
    }
    EXPECT_EQ((int)chain.size(), request.nextDfsPostOrder);
}

TEST_F(FrameGraphBuild, DiamondLadderCountsBothConsumers)
{
    const AppInstancePtr app = getApp();

    NodePtr previous = createNamedNode(app, PLUGINID_OFX_CONSTANT, "LadderRoot");
    ASSERT_TRUE(bool(previous));
    ASSERT_NO_FATAL_FAILURE(setColor(previous, "color", 0.25, 0.5, 0.75, 1.));
    std::vector<NodePtr> joins;
    for (int i = 0; i < 20; ++i) {
        const std::string suffix = std::to_string(i);
        NodePtr left = createNamedNode(app, PLUGINID_OFX_GRADE, "LadderLeft" + suffix);
        NodePtr right = createNamedNode(app, PLUGINID_OFX_GRADE, "LadderRight" + suffix);
        NodePtr join = createNamedNode(app, PLUGINID_OFX_MERGE, "LadderJoin" + suffix);
        ASSERT_TRUE(bool(left));
        ASSERT_TRUE(bool(right));
        ASSERT_TRUE(bool(join));
        ASSERT_TRUE(left->connectInput(previous, 0));
        ASSERT_TRUE(right->connectInput(previous, 0));
        ASSERT_TRUE(join->connectInput(left, 0));
        ASSERT_TRUE(join->connectInput(right, 1));
        joins.push_back(join);
        previous = join;
    }
    NodePtr bottom = createNamedNode(app, PLUGINID_OFX_GRADE, "LadderBottom");
    ASSERT_TRUE(bool(bottom));
    ASSERT_TRUE(bottom->connectInput(previous, 0));

    FrameRequestMap request;
    EdgeSummary summary;
    CallSet calls;
    ASSERT_NO_FATAL_FAILURE(expectEdgesMatchCalls(bottom, 1., &request, &summary, &calls));

    for (std::size_t i = 0; i + 1 < joins.size(); ++i) {
        const FrameViewRequest* fv = request.findFrameViewRequest(joins[i], 1., ViewIdx(0));
        ASSERT_TRUE(fv != NULL) << joins[i]->getScriptName();
        EXPECT_EQ(2, fv->consumers) << joins[i]->getScriptName();
    }
    const FrameViewRequest* last = request.findFrameViewRequest(joins.back(), 1., ViewIdx(0));
    ASSERT_TRUE(last != NULL);
    EXPECT_EQ(1, last->consumers);
}

TEST_F(FrameGraphBuild, FrameBlendHonoursThePreFetchCap)
{
    const AppInstancePtr app = getApp();
    const double time = 10.;

    NodePtr constant = createNamedNode(app, PLUGINID_OFX_CONSTANT, "BlendConstant");
    NodePtr offset = createNamedNode(app, PLUGINID_OFX_TIMEOFFSET, "BlendOffset");
    NodePtr blend = createNamedNode(app, kFrameBlendPluginID, "BlendFrames");
    ASSERT_TRUE(bool(constant));
    ASSERT_TRUE(bool(offset));
    ASSERT_TRUE(bool(blend));
    ASSERT_NO_FATAL_FAILURE(setColor(constant, "color", 0.5, 0.25, 0.125, 1.));
    ASSERT_TRUE(offset->connectInput(constant, 0));
    ASSERT_TRUE(blend->connectInput(offset, 0));

    KnobInt* offsetKnob = dynamic_cast<KnobInt*>(offset->getKnobByName("timeOffset").get());
    ASSERT_TRUE(offsetKnob != NULL);
    offsetKnob->setValue(3);

    KnobInt* frameRange = dynamic_cast<KnobInt*>(blend->getKnobByName("frameRange").get());
    ASSERT_TRUE(frameRange != NULL);
    frameRange->setValue(-5, ViewSpec::all(), 0);
    frameRange->setValue(0, ViewSpec::all(), 1);
    KnobBool* absolute = dynamic_cast<KnobBool*>(blend->getKnobByName("absolute").get());
    ASSERT_TRUE(absolute != NULL);
    absolute->setValue(false);

    FrameRequestMap request;
    EdgeSummary summary;
    CallSet calls;
    std::string error;
    ASSERT_TRUE(buildAndRender(blend, time, &request, &calls, &error)) << error;
    ASSERT_NO_FATAL_FAILURE(collectEdges(request, &summary));

    // FrameBlend needs frames 5..10 of the offset: the first four are pre-rendered, the rest are fetched by getImage.
    CallSet beyondCap;
    beyondCap.insert(makeKey(offset, 9., ViewIdx(0)));
    beyondCap.insert(makeKey(offset, 10., ViewIdx(0)));

    CallSet expectedCalls = summary.targets;
    for (CallSet::const_iterator it = beyondCap.begin(); it != beyondCap.end(); ++it) {
        EXPECT_FALSE(summary.targets.count(*it)) << std::get<0>(*it) << " at " << std::get<1>(*it);
        expectedCalls.insert(*it);
    }
    EXPECT_TRUE(expectedCalls == calls) << describeDifference(expectedCalls, calls);

    for (double f = 5.; f <= 8.; f += 1.) {
        EXPECT_TRUE(summary.targets.count(makeKey(offset, f, ViewIdx(0)))) << "offset at " << f;
    }
    for (double f = 2.; f <= 7.; f += 1.) {
        EXPECT_TRUE(summary.targets.count(makeKey(constant, f, ViewIdx(0)))) << "constant at " << f;
    }

    const FrameViewRequest* blendRequest = request.findFrameViewRequest(blend, time, ViewIdx(0));
    ASSERT_TRUE(blendRequest != NULL);
    std::size_t edgesOnSource = 0;
    for (std::vector<FrameViewRequest::TaskEdge>::const_iterator it = blendRequest->dependencies.begin(); it != blendRequest->dependencies.end(); ++it) {
        if (it->inputNb == 0) {
            ++edgesOnSource;
        }
    }
    EXPECT_EQ((std::size_t)NATRON_MAX_FRAMES_NEEDED_PRE_FETCHING, edgesOnSource);

    for (double f = 9.; f <= 10.; f += 1.) {
        const FrameViewRequest* nested = request.findFrameViewRequest(offset, f, ViewIdx(0));
        ASSERT_TRUE(nested != NULL) << "offset at " << f;
        EXPECT_EQ(0, nested->consumers) << "offset at " << f;
    }
}

TEST_F(FrameGraphBuild, DisabledNodeIsASingleAliasEdge)
{
    const AppInstancePtr app = getApp();

    NodePtr constant = createNamedNode(app, PLUGINID_OFX_CONSTANT, "DisabledConstant");
    NodePtr upper = createNamedNode(app, PLUGINID_OFX_GRADE, "DisabledUpper");
    NodePtr disabled = createNamedNode(app, PLUGINID_OFX_GRADE, "DisabledMiddle");
    NodePtr lower = createNamedNode(app, PLUGINID_OFX_GRADE, "DisabledLower");
    ASSERT_TRUE(bool(constant));
    ASSERT_TRUE(bool(upper));
    ASSERT_TRUE(bool(disabled));
    ASSERT_TRUE(bool(lower));
    ASSERT_NO_FATAL_FAILURE(setColor(constant, "color", 0.5, 0.5, 0.5, 1.));
    ASSERT_TRUE(upper->connectInput(constant, 0));
    ASSERT_TRUE(disabled->connectInput(upper, 0));
    ASSERT_TRUE(lower->connectInput(disabled, 0));
    ASSERT_NO_FATAL_FAILURE(setGradeMultiply(upper, 0.5));
    ASSERT_NO_FATAL_FAILURE(setGradeMultiply(disabled, 0.5));
    ASSERT_NO_FATAL_FAILURE(setGradeMultiply(lower, 0.5));
    disabled->setNodeDisabled(true);

    FrameRequestMap request;
    EdgeSummary summary;
    CallSet calls;
    ASSERT_NO_FATAL_FAILURE(expectEdgesMatchCalls(lower, 1., &request, &summary, &calls));

    const FrameViewRequest* middle = request.findFrameViewRequest(disabled, 1., ViewIdx(0));
    ASSERT_TRUE(middle != NULL);
    EXPECT_TRUE(middle->globalData.isIdentity);
    ASSERT_EQ(1u, middle->dependencies.size());
    EXPECT_EQ(upper, middle->dependencies.front().node.lock());
    EXPECT_EQ(1., middle->dependencies.front().time);
    EXPECT_EQ(0, middle->dependencies.front().inputNb);
    EXPECT_EQ(1, middle->consumers);

    const FrameViewRequest* upperRequest = request.findFrameViewRequest(upper, 1., ViewIdx(0));
    ASSERT_TRUE(upperRequest != NULL);
    EXPECT_EQ(1, upperRequest->consumers);
    EXPECT_TRUE(std::find(upperRequest->componentsRequested.begin(), upperRequest->componentsRequested.end(), ImageLayerDesc::getRGBAComponents()) != upperRequest->componentsRequested.end());
    EXPECT_EQ(3u, summary.count);
}

TEST_F(FrameGraphBuild, ConcatenatedTransformsSkipTheUpstreamTransform)
{
    const AppInstancePtr app = getApp();

    NodePtr constant = createNamedNode(app, PLUGINID_OFX_CONSTANT, "TransformConstant");
    NodePtr first = createNamedNode(app, PLUGINID_OFX_TRANSFORM, "TransformFirst");
    NodePtr second = createNamedNode(app, PLUGINID_OFX_TRANSFORM, "TransformSecond");
    NodePtr grade = createNamedNode(app, PLUGINID_OFX_GRADE, "TransformGrade");
    ASSERT_TRUE(bool(constant));
    ASSERT_TRUE(bool(first));
    ASSERT_TRUE(bool(second));
    ASSERT_TRUE(bool(grade));
    ASSERT_NO_FATAL_FAILURE(setColor(constant, "color", 0.5, 0.5, 0.5, 1.));
    ASSERT_TRUE(first->connectInput(constant, 0));
    ASSERT_TRUE(second->connectInput(first, 0));
    ASSERT_TRUE(grade->connectInput(second, 0));
    ASSERT_NO_FATAL_FAILURE(setGradeMultiply(grade, 0.5));

    KnobDouble* firstTranslate = dynamic_cast<KnobDouble*>(first->getKnobByName("translate").get());
    KnobDouble* secondTranslate = dynamic_cast<KnobDouble*>(second->getKnobByName("translate").get());
    ASSERT_TRUE(firstTranslate != NULL);
    ASSERT_TRUE(secondTranslate != NULL);
    firstTranslate->setValue(10., ViewSpec::all(), 0);
    firstTranslate->setValue(5., ViewSpec::all(), 1);
    secondTranslate->setValue(-3., ViewSpec::all(), 0);
    secondTranslate->setValue(2., ViewSpec::all(), 1);

    FrameRequestMap request;
    EdgeSummary summary;
    CallSet calls;
    ASSERT_NO_FATAL_FAILURE(expectEdgesMatchCalls(grade, 1., &request, &summary, &calls));

    EXPECT_TRUE(summary.targets.count(makeKey(second, 1., ViewIdx(0))));
    EXPECT_TRUE(summary.targets.count(makeKey(constant, 1., ViewIdx(0))));
    if (appPTR->getCurrentSettings()->isTransformConcatenationEnabled()) {
        EXPECT_FALSE(summary.targets.count(makeKey(first, 1., ViewIdx(0))));
        const FrameViewRequest* secondRequest = request.findFrameViewRequest(second, 1., ViewIdx(0));
        ASSERT_TRUE(secondRequest != NULL);
        ASSERT_EQ(1u, secondRequest->dependencies.size());
        EXPECT_EQ(constant, secondRequest->dependencies.front().node.lock());
    }
}

TEST_F(FrameGraphBuild, MergeMaskInputIsAnEdge)
{
    const AppInstancePtr app = getApp();

    NodePtr background = createNamedNode(app, PLUGINID_OFX_CONSTANT, "MaskBackground");
    NodePtr foreground = createNamedNode(app, PLUGINID_OFX_CONSTANT, "MaskForeground");
    NodePtr matte = createNamedNode(app, PLUGINID_OFX_CONSTANT, "MaskMatte");
    NodePtr merge = createNamedNode(app, PLUGINID_OFX_MERGE, "MaskMerge");
    ASSERT_TRUE(bool(background));
    ASSERT_TRUE(bool(foreground));
    ASSERT_TRUE(bool(matte));
    ASSERT_TRUE(bool(merge));
    ASSERT_NO_FATAL_FAILURE(setColor(background, "color", 0.1, 0.2, 0.3, 1.));
    ASSERT_NO_FATAL_FAILURE(setColor(foreground, "color", 0.6, 0.5, 0.4, 0.5));
    ASSERT_NO_FATAL_FAILURE(setColor(matte, "color", 1., 1., 1., 1.));

    EffectInstancePtr mergeEffect = merge->getEffectInstance();
    int maskInput = -1;
    for (int i = 0; i < merge->getNInputs(); ++i) {
        if (mergeEffect->isInputMask(i)) {
            maskInput = i;
            break;
        }
    }
    ASSERT_GE(maskInput, 0);
    ASSERT_TRUE(merge->connectInput(background, 0));
    ASSERT_TRUE(merge->connectInput(foreground, 1));
    ASSERT_TRUE(merge->connectInput(matte, maskInput));
    KnobBool* maskEnabled = dynamic_cast<KnobBool*>(merge->getKnobByName("enableMask_" + merge->getInputLabel(maskInput)).get());
    ASSERT_TRUE(maskEnabled != NULL);
    maskEnabled->setValue(true);

    FrameRequestMap request;
    EdgeSummary summary;
    CallSet calls;
    ASSERT_NO_FATAL_FAILURE(expectEdgesMatchCalls(merge, 1., &request, &summary, &calls));

    EXPECT_TRUE(summary.targets.count(makeKey(matte, 1., ViewIdx(0))));
    const FrameViewRequest* mergeRequest = request.findFrameViewRequest(merge, 1., ViewIdx(0));
    ASSERT_TRUE(mergeRequest != NULL);
    bool hasMaskEdge = false;
    for (std::vector<FrameViewRequest::TaskEdge>::const_iterator it = mergeRequest->dependencies.begin(); it != mergeRequest->dependencies.end(); ++it) {
        if (it->inputNb == maskInput) {
            hasMaskEdge = true;
            EXPECT_EQ(matte, it->node.lock());
        }
    }
    EXPECT_TRUE(hasMaskEdge);
}

TEST_F(FrameGraphBuild, ExpressionDependencyIsNotAnEdge)
{
    const AppInstancePtr app = getApp();

    NodePtr constant = createNamedNode(app, PLUGINID_OFX_CONSTANT, "ExprConstant");
    NodePtr grade = createNamedNode(app, PLUGINID_OFX_GRADE, "ExprGrade");
    NodePtr otherConstant = createNamedNode(app, PLUGINID_OFX_CONSTANT, "ExprOtherConstant");
    NodePtr otherGrade = createNamedNode(app, PLUGINID_OFX_GRADE, "ExprOtherGrade");
    ASSERT_TRUE(bool(constant));
    ASSERT_TRUE(bool(grade));
    ASSERT_TRUE(bool(otherConstant));
    ASSERT_TRUE(bool(otherGrade));
    ASSERT_NO_FATAL_FAILURE(setColor(constant, "color", 0.5, 0.5, 0.5, 1.));
    ASSERT_TRUE(grade->connectInput(constant, 0));
    ASSERT_TRUE(otherGrade->connectInput(otherConstant, 0));
    ASSERT_NO_FATAL_FAILURE(setGradeMultiply(otherGrade, 0.5));

    KnobIPtr multiply = grade->getKnobByName("multiply");
    ASSERT_TRUE(bool(multiply));
    for (int d = 0; d < multiply->getDimension(); ++d) {
        const std::string expr = otherGrade->getScriptName() + ".multiply.getValue(" + std::to_string(d) + ")";
        ASSERT_NO_THROW(multiply->setExpression(d, expr, false, true));
    }

    FrameRequestMap request;
    EdgeSummary summary;
    CallSet calls;
    ASSERT_NO_FATAL_FAILURE(expectEdgesMatchCalls(grade, 1., &request, &summary, &calls));

    EXPECT_EQ(1u, summary.count);
    EXPECT_TRUE(summary.targets.count(makeKey(constant, 1., ViewIdx(0))));
    EXPECT_TRUE(request.find(otherGrade) == request.end());
    EXPECT_TRUE(request.find(otherConstant) == request.end());
}
