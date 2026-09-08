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

#include "BaseTest.h"
#include "DataKindTestEffect.h"

#include "Engine/EffectInstance.h"
#include "Engine/Node.h"

NATRON_NAMESPACE_USING

// A freshly connected Dot has no declared kind of its own (eDataKindPolymorphic):
// its effective output kind must resolve structurally to whatever concrete kind
// feeds it, here the image-kind generator plugged into its single input.
TEST_F(BaseTest, DotFedByImageSourceResolvesToImage)
{
    NodePtr generator = createNode(_generatorPluginID);
    NodePtr dot = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));

    ASSERT_TRUE(generator && dot);

    connectNodes(generator, dot, 0, true);

    EXPECT_EQ(eDataKindImage, dot->getEffectiveOutputDataKind());
}

// With nothing feeding it, a polymorphic pass-through node resolves to
// eDataKindPolymorphic itself: "no constraint yet", not a fallback to image.
TEST_F(BaseTest, DisconnectedDotResolvesUnconstrained)
{
    NodePtr dot = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));

    ASSERT_TRUE(bool(dot));

    EXPECT_EQ(eDataKindPolymorphic, dot->getEffectiveOutputDataKind());
}

// The resolution is cached, so this exercises cache invalidation specifically:
// disconnecting must drop the memoized "image" answer back to unconstrained, and
// reconnecting must produce a fresh, correct answer rather than replaying a stale one.
TEST_F(BaseTest, DotResolutionUpdatesAfterDisconnectAndReconnect)
{
    NodePtr generator = createNode(_generatorPluginID);
    NodePtr dot = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));

    ASSERT_TRUE(generator && dot);

    connectNodes(generator, dot, 0, true);
    EXPECT_EQ(eDataKindImage, dot->getEffectiveOutputDataKind());

    disconnectNodes(generator, dot, true);
    EXPECT_EQ(eDataKindPolymorphic, dot->getEffectiveOutputDataKind());

    connectNodes(generator, dot, 0, true);
    EXPECT_EQ(eDataKindImage, dot->getEffectiveOutputDataKind());
}

// A concrete image-kind sink cannot be fed directly by a concrete deep-kind source:
// canConnectInput must reject at the connection that introduces the contradiction, and
// name the node responsible for the conflicting kind (here, the source itself).
TEST_F(BaseTest, DirectConcreteKindMismatchRejected)
{
    NodePtr deepSource = createNode(QString::fromUtf8(kTestPluginIDDataKindDeepSource));
    NodePtr imageSink = createNode(QString::fromUtf8(kTestPluginIDDataKindImageSink));

    ASSERT_TRUE(deepSource && imageSink);

    NodePtr conflictingNode;
    Node::CanConnectInputReturnValue ret = imageSink->canConnectInput(deepSource, 0, &conflictingNode);

    EXPECT_EQ(Node::eCanConnectInput_incompatibleDataKind, ret);
    EXPECT_EQ(deepSource.get(), conflictingNode.get());
}

// A concrete image source connecting to a concrete image sink is untouched by the new check.
TEST_F(BaseTest, ImageToImageStillConnects)
{
    NodePtr generator = createNode(_generatorPluginID);
    NodePtr imageSink = createNode(QString::fromUtf8(kTestPluginIDDataKindImageSink));

    ASSERT_TRUE(generator && imageSink);

    EXPECT_EQ(Node::eCanConnectInput_ok, imageSink->canConnectInput(generator, 0));
    connectNodes(generator, imageSink, 0, true);
}

// A Dot with nothing feeding it is unconstrained (eDataKindPolymorphic), not image: connecting
// it into a concrete image sink must still be allowed exactly as before this check existed.
TEST_F(BaseTest, StillUnconstrainedPolymorphicConnects)
{
    NodePtr dot = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    NodePtr imageSink = createNode(QString::fromUtf8(kTestPluginIDDataKindImageSink));

    ASSERT_TRUE(dot && imageSink);

    EXPECT_EQ(Node::eCanConnectInput_ok, imageSink->canConnectInput(dot, 0));
    connectNodes(dot, imageSink, 0, true);
}

// A contradiction introduced through a Dot chain, discovered from the Dot's already-resolved
// upstream side: deep source -> Dot (fine, Dot is unconstrained), then Dot -> image sink must
// be rejected because the Dot's effective output kind has already resolved to deep.
TEST_F(BaseTest, DotChainContradictionRejectedFromUpstreamSide)
{
    NodePtr deepSource = createNode(QString::fromUtf8(kTestPluginIDDataKindDeepSource));
    NodePtr dot = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    NodePtr imageSink = createNode(QString::fromUtf8(kTestPluginIDDataKindImageSink));

    ASSERT_TRUE(deepSource && dot && imageSink);

    connectNodes(deepSource, dot, 0, true);
    EXPECT_EQ(eDataKindDeep, dot->getEffectiveOutputDataKind());

    NodePtr conflictingNode;
    Node::CanConnectInputReturnValue ret = imageSink->canConnectInput(dot, 0, &conflictingNode);

    EXPECT_EQ(Node::eCanConnectInput_incompatibleDataKind, ret);
    EXPECT_EQ(dot.get(), conflictingNode.get());
}

// The same contradiction, introduced from the other end: Dot -> image sink first (fine, Dot is
// still unconstrained), then deep source -> Dot must be rejected, because connecting it would
// retroactively make the Dot's already-connected downstream sink incompatible. This is the case
// that requires validating the whole resolved chain, not just the two ends of the new edge.
TEST_F(BaseTest, DotChainContradictionRejectedFromDownstreamSide)
{
    NodePtr deepSource = createNode(QString::fromUtf8(kTestPluginIDDataKindDeepSource));
    NodePtr dot = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    NodePtr imageSink = createNode(QString::fromUtf8(kTestPluginIDDataKindImageSink));

    ASSERT_TRUE(deepSource && dot && imageSink);

    connectNodes(dot, imageSink, 0, true);
    EXPECT_EQ(eDataKindImage, dot->getEffectiveOutputDataKind());

    NodePtr conflictingNode;
    Node::CanConnectInputReturnValue ret = dot->canConnectInput(deepSource, 0, &conflictingNode);

    EXPECT_EQ(Node::eCanConnectInput_incompatibleDataKind, ret);
    EXPECT_EQ(imageSink.get(), conflictingNode.get());

    EXPECT_EQ(eDataKindImage, dot->getEffectiveOutputDataKind());
}

// The same pass-through, resolved from the other direction: nothing feeds the Dot, but the only
// thing it feeds declares deep, so deep is the only kind it can be carrying.
TEST_F(BaseTest, DotResolvesFromItsDownstreamConsumerAlone)
{
    NodePtr dot = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    NodePtr deepSink = createNode(QString::fromUtf8(kTestPluginIDDataKindDeepSink));

    ASSERT_TRUE(dot && deepSink);

    connectNodes(dot, deepSink, 0, true);

    EXPECT_EQ(eDataKindDeep, dot->getEffectiveOutputDataKind());
}

// Upstream-only, for comparison with the case above: the same node, the same answer, reached from
// the opposite side.
TEST_F(BaseTest, DotResolvesFromItsUpstreamSourceAlone)
{
    NodePtr deepSource = createNode(QString::fromUtf8(kTestPluginIDDataKindDeepSource));
    NodePtr dot = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));

    ASSERT_TRUE(deepSource && dot);

    connectNodes(deepSource, dot, 0, true);

    EXPECT_EQ(eDataKindDeep, dot->getEffectiveOutputDataKind());
}

// Kinds are resolved from the topology alone, never from the history of how it was built: the same
// graph wired in either order has to answer the same, or a project would resolve differently after
// a reload, where every edge is restored in serialization order.
TEST_F(BaseTest, ResolutionDoesNotDependOnConnectionOrder)
{
    NodePtr deepSourceA = createNode(QString::fromUtf8(kTestPluginIDDataKindDeepSource));
    NodePtr dotA = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    NodePtr deepSinkA = createNode(QString::fromUtf8(kTestPluginIDDataKindDeepSink));

    ASSERT_TRUE(deepSourceA && dotA && deepSinkA);

    connectNodes(deepSourceA, dotA, 0, true);
    connectNodes(dotA, deepSinkA, 0, true);

    NodePtr deepSourceB = createNode(QString::fromUtf8(kTestPluginIDDataKindDeepSource));
    NodePtr dotB = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    NodePtr deepSinkB = createNode(QString::fromUtf8(kTestPluginIDDataKindDeepSink));

    ASSERT_TRUE(deepSourceB && dotB && deepSinkB);

    connectNodes(dotB, deepSinkB, 0, true);
    connectNodes(deepSourceB, dotB, 0, true);

    EXPECT_EQ(eDataKindDeep, dotA->getEffectiveOutputDataKind());
    EXPECT_EQ(dotA->getEffectiveOutputDataKind(), dotB->getEffectiveOutputDataKind());
}

// Several polymorphic inputs carrying the same concrete kind say one thing, not several: the node
// carries that kind and connects to a consumer declaring it.
TEST_F(BaseTest, MultiInputPolymorphicWithAgreeingInputsResolvesToThatKind)
{
    NodePtr deepSource1 = createNode(QString::fromUtf8(kTestPluginIDDataKindDeepSource));
    NodePtr deepSource2 = createNode(QString::fromUtf8(kTestPluginIDDataKindDeepSource));
    NodePtr poly = createNode(QString::fromUtf8(kTestPluginIDDataKindPolyTwoInputs));
    NodePtr deepSink = createNode(QString::fromUtf8(kTestPluginIDDataKindDeepSink));

    ASSERT_TRUE(deepSource1 && deepSource2 && poly && deepSink);

    connectNodes(deepSource1, poly, 0, true);
    connectNodes(deepSource2, poly, 1, true);

    bool ambiguous = true;
    EXPECT_EQ(eDataKindDeep, poly->getEffectiveOutputDataKind(&ambiguous));
    EXPECT_FALSE(ambiguous);

    EXPECT_EQ(Node::eCanConnectInput_ok, deepSink->canConnectInput(poly, 0));
    connectNodes(poly, deepSink, 0, true);
}

// Inputs of different concrete kinds are legitimate -- a node selecting between an image branch
// and a deep branch has exactly that shape -- so neither input is refused. What the node produces
// is simply not one kind, and getEffectiveOutputDataKind() reports that as ambiguous.
TEST_F(BaseTest, MultiInputPolymorphicWithDisagreeingInputsIsAmbiguousNotRejected)
{
    NodePtr generator = createNode(_generatorPluginID);
    NodePtr deepSource = createNode(QString::fromUtf8(kTestPluginIDDataKindDeepSource));
    NodePtr poly = createNode(QString::fromUtf8(kTestPluginIDDataKindPolyTwoInputs));

    ASSERT_TRUE(generator && deepSource && poly);

    connectNodes(generator, poly, 0, true);

    EXPECT_EQ(Node::eCanConnectInput_ok, poly->canConnectInput(deepSource, 1));
    connectNodes(deepSource, poly, 1, true);

    bool ambiguous = false;
    EXPECT_EQ(eDataKindPolymorphic, poly->getEffectiveOutputDataKind(&ambiguous));
    EXPECT_TRUE(ambiguous);
}

// Where the ambiguity is refused: not at the inputs that created it, but at a consumer that has to
// be handed one definite kind and cannot be.
TEST_F(BaseTest, AmbiguousProducerRejectedByConcreteConsumer)
{
    NodePtr generator = createNode(_generatorPluginID);
    NodePtr deepSource = createNode(QString::fromUtf8(kTestPluginIDDataKindDeepSource));
    NodePtr poly = createNode(QString::fromUtf8(kTestPluginIDDataKindPolyTwoInputs));
    NodePtr imageSink = createNode(QString::fromUtf8(kTestPluginIDDataKindImageSink));

    ASSERT_TRUE(generator && deepSource && poly && imageSink);

    connectNodes(generator, poly, 0, true);
    connectNodes(deepSource, poly, 1, true);

    NodePtr conflictingNode;
    Node::CanConnectInputReturnValue ret = imageSink->canConnectInput(poly, 0, &conflictingNode);

    EXPECT_EQ(Node::eCanConnectInput_incompatibleDataKind, ret);
    EXPECT_EQ(poly.get(), conflictingNode.get());
}

// An input that declares a concrete kind constrains what may connect to it, it does not decide
// what the node produces: with only that input connected the output is still unconstrained.
TEST_F(BaseTest, ConcreteDeclaredInputDoesNotResolvePolymorphicOutput)
{
    NodePtr generator = createNode(_generatorPluginID);
    NodePtr poly = createNode(QString::fromUtf8(kTestPluginIDDataKindPolyAndImageInput));

    ASSERT_TRUE(generator && poly);

    connectNodes(generator, poly, 1, true);

    bool ambiguous = true;
    EXPECT_EQ(eDataKindPolymorphic, poly->getEffectiveOutputDataKind(&ambiguous));
    EXPECT_FALSE(ambiguous);
}

// A native node that overrides NativeEffectBase::resolveOutputDataKind() decides its own kind, and
// the engine uses that answer rather than what the graph structurally says: here scene, although
// the only thing connected to the node is an image source.
TEST_F(BaseTest, NativeNodeResolutionPolicyOverridesStructuralResolution)
{
    NodePtr generator = createNode(_generatorPluginID);
    NodePtr policy = createNode(QString::fromUtf8(kTestPluginIDDataKindScenePolicy));

    ASSERT_TRUE(generator && policy);

    connectNodes(generator, policy, 0, true);

    bool ambiguous = true;
    EXPECT_EQ(eDataKindScene, policy->getEffectiveOutputDataKind(&ambiguous));
    EXPECT_FALSE(ambiguous);

    NodePtr imageSink = createNode(QString::fromUtf8(kTestPluginIDDataKindImageSink));
    ASSERT_TRUE(bool(imageSink));

    NodePtr conflictingNode;
    EXPECT_EQ(Node::eCanConnectInput_incompatibleDataKind, imageSink->canConnectInput(policy, 0, &conflictingNode));
    EXPECT_EQ(policy.get(), conflictingNode.get());
}

// A node whose policy reads a neighbour can close a resolution loop, and truncating that loop
// answers "unconstrained" for whichever node the query happened to start from. Memoizing that
// would make later reads depend on which node was asked first, so the same graph is built twice
// and queried in opposite orders: both nodes must answer the same either way.
TEST_F(BaseTest, ResolutionOfACycleDoesNotDependOnWhichNodeIsQueriedFirst)
{
    NodePtr deepSourceA = createNode(QString::fromUtf8(kTestPluginIDDataKindDeepSource));
    NodePtr mirrorA = createNode(QString::fromUtf8(kTestPluginIDDataKindConsumerMirrorPolicy));
    NodePtr polyA = createNode(QString::fromUtf8(kTestPluginIDDataKindPolyTwoInputs));

    ASSERT_TRUE(deepSourceA && mirrorA && polyA);

    connectNodes(mirrorA, polyA, 0, true);
    connectNodes(deepSourceA, polyA, 1, true);

    NodePtr deepSourceB = createNode(QString::fromUtf8(kTestPluginIDDataKindDeepSource));
    NodePtr mirrorB = createNode(QString::fromUtf8(kTestPluginIDDataKindConsumerMirrorPolicy));
    NodePtr polyB = createNode(QString::fromUtf8(kTestPluginIDDataKindPolyTwoInputs));

    ASSERT_TRUE(deepSourceB && mirrorB && polyB);

    connectNodes(mirrorB, polyB, 0, true);
    connectNodes(deepSourceB, polyB, 1, true);

    bool polyAAmbiguous = true;
    const DataKindEnum polyAKind = polyA->getEffectiveOutputDataKind(&polyAAmbiguous);
    bool mirrorAAmbiguous = true;
    const DataKindEnum mirrorAKind = mirrorA->getEffectiveOutputDataKind(&mirrorAAmbiguous);

    bool mirrorBAmbiguous = true;
    const DataKindEnum mirrorBKind = mirrorB->getEffectiveOutputDataKind(&mirrorBAmbiguous);
    bool polyBAmbiguous = true;
    const DataKindEnum polyBKind = polyB->getEffectiveOutputDataKind(&polyBAmbiguous);

    EXPECT_EQ(mirrorBKind, mirrorAKind);
    EXPECT_EQ(mirrorBAmbiguous, mirrorAAmbiguous);
    EXPECT_EQ(polyBKind, polyAKind);
    EXPECT_EQ(polyBAmbiguous, polyAAmbiguous);

    EXPECT_EQ(eDataKindDeep, polyAKind);
    EXPECT_EQ(eDataKindDeep, mirrorAKind);

    // Re-reading in the opposite order to the one each graph was first queried in must not move
    // either answer: nothing about the first query may have been recorded.
    EXPECT_EQ(mirrorAKind, mirrorA->getEffectiveOutputDataKind());
    EXPECT_EQ(polyBKind, polyB->getEffectiveOutputDataKind());
}

// A node that owns its resolution policy follows the input it selected, so the branches it does
// not follow carry nothing on its account: what it must deliver downstream must not reach back
// through them and type, and then reject connections on, a branch it never reads.
TEST_F(BaseTest, PolicyNodeDoesNotTypeTheBranchesItDoesNotFollow)
{
    NodePtr selectedBranch = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    NodePtr ignoredBranch = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    NodePtr policy = createNode(QString::fromUtf8(kTestPluginIDDataKindSelectFirstInputPolicy));
    NodePtr deepSink = createNode(QString::fromUtf8(kTestPluginIDDataKindDeepSink));

    ASSERT_TRUE(selectedBranch && ignoredBranch && policy && deepSink);

    connectNodes(selectedBranch, policy, 0, true);
    connectNodes(ignoredBranch, policy, 1, true);
    connectNodes(policy, deepSink, 0, true);

    EXPECT_EQ(eDataKindDeep, policy->getEffectiveOutputDataKind());
    EXPECT_EQ(eDataKindDeep, selectedBranch->getEffectiveOutputDataKind());

    bool ambiguous = true;
    EXPECT_EQ(eDataKindPolymorphic, ignoredBranch->getEffectiveOutputDataKind(&ambiguous));
    EXPECT_FALSE(ambiguous);

    NodePtr generator = createNode(_generatorPluginID);
    ASSERT_TRUE(bool(generator));

    EXPECT_EQ(Node::eCanConnectInput_ok, ignoredBranch->canConnectInput(generator, 0));
    connectNodes(generator, ignoredBranch, 0, true);

    EXPECT_EQ(eDataKindImage, ignoredBranch->getEffectiveOutputDataKind());
    EXPECT_EQ(eDataKindDeep, policy->getEffectiveOutputDataKind());
}
