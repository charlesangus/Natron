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
    EXPECT_EQ(eDataKindPolymorphic, dot->getEffectiveOutputDataKind());

    NodePtr conflictingNode;
    Node::CanConnectInputReturnValue ret = dot->canConnectInput(deepSource, 0, &conflictingNode);

    EXPECT_EQ(Node::eCanConnectInput_incompatibleDataKind, ret);
    EXPECT_EQ(imageSink.get(), conflictingNode.get());

    EXPECT_EQ(eDataKindPolymorphic, dot->getEffectiveOutputDataKind());
}
