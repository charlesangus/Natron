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
#include <limits>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobShuffleMap.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Channel/Shuffle.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

#include <ofxImageEffect.h>

NATRON_NAMESPACE_USING

namespace {

QString
messageOf(const NodePtr& node)
{
    QString message;
    int type = 0;
    node->getPersistentMessage(&message, &type, false);

    return message;
}

bool
contains(const NodesList& nodes,
         const NodePtr& node)
{
    return std::find(nodes.begin(), nodes.end(), node) != nodes.end();
}

} // namespace

class PersistentMessageTest
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

    NodePtr createReader(const std::string& fixture)
    {
        CreateNodeArgs args(_readOIIOPluginID.toStdString(), getApp()->getProject());
        args.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, std::string(NATRON_TESTS_FIXTURES_DIR "/") + fixture);
        NodePtr reader = getApp()->createNode(args);
        EXPECT_TRUE(bool(reader)) << "node creation failed for " << _readOIIOPluginID.toStdString();

        return reader;
    }

    // Read(flat-three-layers.exr) at frame 1, Read(flat-rgba-only.exr) at frame 2 -> Switch ->
    // Shuffle writing diffuse.g into Color.r -> Write, so the Shuffle fails at frame 2 only.
    void createSwitchShuffleGraph()
    {
        NodePtr readerA = createReader("flat-three-layers.exr");
        NodePtr readerB = createReader("flat-rgba-only.exr");
        ASSERT_TRUE(readerA && readerB);

        _switch = createNode(QString::fromUtf8("net.sf.openfx.switchPlugin"));
        ASSERT_TRUE(bool(_switch));
        connectNodes(readerA, _switch, 0, true);
        connectNodes(readerB, _switch, 1, true);
        KnobIntPtr which = std::dynamic_pointer_cast<KnobInt>(_switch->getKnobByName("which"));
        ASSERT_TRUE(bool(which));
        which->setValueAtTime(1, 0, ViewSpec::all(), 0);
        which->setValueAtTime(2, 1, ViewSpec::all(), 0);

        _shuffle = createNode(QString::fromUtf8(PLUGINID_NATRON_SHUFFLE));
        ASSERT_TRUE(bool(_shuffle));
        connectNodes(_switch, _shuffle, Shuffle::eInputMain, true);

        KnobLayerSelectPtr in1 = std::dynamic_pointer_cast<KnobLayerSelect>(_shuffle->getKnobByName(kShuffleParamIn1));
        ASSERT_TRUE(bool(in1));
        in1->setLayer("diffuse");
        KnobShuffleMapPtr mapping = std::dynamic_pointer_cast<KnobShuffleMap>(_shuffle->getKnobByName(kShuffleParamMapping));
        ASSERT_TRUE(bool(mapping));
        mapping->setSource(1, 0, ShuffleSource::makeInput(1, 1));
        // diffuse has no fourth channel, so the default A would fail at frame 1 too.
        mapping->setSource(1, 3, ShuffleSource::makeZero());

        _writer = createNode(_writeOIIOPluginID);
        ASSERT_TRUE(bool(_writer));
        connectNodes(_shuffle, _writer, 0, true);
        KnobChoice* bitDepth = dynamic_cast<KnobChoice*>(_writer->getKnobByName("bitDepth").get());
        ASSERT_TRUE(bitDepth != NULL);
        bitDepth->setValueFromID("32f", 0);
    }

    // Renders frame through the Write node and returns whether it produced its file.
    bool renderFrame(const QTemporaryDir& tmp,
                     int frame)
    {
        const std::string path = (tmp.path() + QString::fromUtf8("/frame%1.exr").arg(frame)).toStdString();
        _writer->setOutputFilesForWriter(path);
        QFile::remove(QString::fromStdString(path));

        OutputEffectInstance* writerEffect = dynamic_cast<OutputEffectInstance*>(_writer->getEffectInstance().get());
        EXPECT_TRUE(writerEffect != NULL);
        if (!writerEffect) {
            return false;
        }
        std::list<AppInstance::RenderWork> works;
        works.push_back(AppInstance::RenderWork(writerEffect, frame, frame, 1, false));
        getApp()->startWritersRendering(false, works);

        const bool rendered = QFile::exists(QString::fromStdString(path));
        QFile::remove(QString::fromStdString(path));

        return rendered;
    }

    NodePtr _switch;
    NodePtr _shuffle;
    NodePtr _writer;
};

TEST_F(PersistentMessageTest, PassingCheckClearsOnlyFromANewerRenderThatWasNotAborted)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_SHUFFLE));
    ASSERT_TRUE(bool(node));

    const U64 posting = AbortableRenderInfo::getLatestRenderSequence() + 10;
    node->setChannelSelectorMessage("diffuse.G is missing", posting);
    ASSERT_TRUE(node->hasPersistentMessage());

    node->clearChannelSelectorMessageFromRender(posting - 1, false);
    EXPECT_TRUE(node->hasPersistentMessage()) << "an older render cleared the error";

    node->clearChannelSelectorMessageFromRender(posting, false);
    EXPECT_TRUE(node->hasPersistentMessage()) << "the posting render cleared its own error";

    node->clearChannelSelectorMessageFromRender(posting + 1, true);
    EXPECT_TRUE(node->hasPersistentMessage()) << "an aborted render cleared the error";

    node->clearChannelSelectorMessageFromRender(posting + 1, false);
    EXPECT_FALSE(node->hasPersistentMessage()) << "a newer render did not clear the error";
}

TEST_F(PersistentMessageTest, FailureFromARenderOlderThanTheLastPassIsNotPosted)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_SHUFFLE));
    ASSERT_TRUE(bool(node));

    const U64 passing = AbortableRenderInfo::getLatestRenderSequence() + 10;
    node->clearChannelSelectorMessageFromRender(passing, false);

    node->setChannelSelectorMessage("stale", passing - 1);
    EXPECT_FALSE(node->hasPersistentMessage());

    node->setChannelSelectorMessage("current", passing + 1);
    EXPECT_EQ(QString::fromUtf8("current"), messageOf(node));
}

TEST_F(PersistentMessageTest, FailureFromAnAbortedOrSupersededRenderIsNotPosted)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_SHUFFLE));
    ASSERT_TRUE(bool(node));

    AbortableRenderInfoPtr aborted = AbortableRenderInfo::create(true, 0);
    AbortableRenderInfoPtr superseded = AbortableRenderInfo::create(true, 0);
    AbortableRenderInfoPtr current = AbortableRenderInfo::create(true, 0);
    ASSERT_FALSE(superseded->isSuperseded());
    aborted->setAborted();
    superseded->setSuperseded();
    EXPECT_TRUE(superseded->isSuperseded());
    EXPECT_FALSE(superseded->isAborted()) << "superseding stopped the render";

    node->setChannelSelectorMessageFromRender("from the aborted render", aborted);
    EXPECT_FALSE(node->hasPersistentMessage()) << messageOf(node).toStdString();

    node->setChannelSelectorMessageFromRender("from the superseded render", superseded);
    EXPECT_FALSE(node->hasPersistentMessage()) << messageOf(node).toStdString();

    node->setChannelSelectorMessageFromRender("from the current render", current);
    EXPECT_EQ(QString::fromUtf8("from the current render"), messageOf(node));

    node->clearChannelSelectorMessageFromRender(current->getRenderSequence(), false);
    EXPECT_TRUE(node->hasPersistentMessage()) << "the posting render cleared its own error";

    AbortableRenderInfoPtr newer = AbortableRenderInfo::create(true, 0);
    node->clearChannelSelectorMessageFromRender(newer->getRenderSequence(), false);
    EXPECT_FALSE(node->hasPersistentMessage()) << "a newer passing render did not clear the error";
}

TEST_F(PersistentMessageTest, RenderOfAFrameTheUserLeftDoesNotPostAfterTheLanding)
{
    createSwitchShuffleGraph();
    if (HasFatalFailure()) {
        return;
    }
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    // A frame-2 render still running when the user lands on frame 1: the frame-1 request
    // supersedes it, and frame 1 comes from the cache while no error is posted yet.
    AbortableRenderInfoPtr leftFrame = AbortableRenderInfo::create(true, 0);
    leftFrame->setSuperseded();
    _shuffle->refreshChannelSelectorMessageAtTime(1);
    ASSERT_FALSE(_shuffle->hasPersistentMessage());

    std::string frame2Error;
    ASSERT_FALSE(_shuffle->checkSelectedChannelsPresent(2, ViewIdx(0), &frame2Error));
    _shuffle->setChannelSelectorMessageFromRender(frame2Error, leftFrame);
    EXPECT_FALSE(_shuffle->hasPersistentMessage()) << "the render the user left posted its error over frame 1";

    EXPECT_FALSE(renderFrame(tmp, 2));
    EXPECT_EQ(QString::fromStdString(frame2Error), messageOf(_shuffle)) << "a newer render of frame 2 did not post";

    EXPECT_TRUE(renderFrame(tmp, 1));
    EXPECT_FALSE(_shuffle->hasPersistentMessage());
}

TEST_F(PersistentMessageTest, SameTextFromTheOtherPosterKeepsTheOwner)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_SHUFFLE));
    ASSERT_TRUE(bool(node));
    const U64 posting = AbortableRenderInfo::getLatestRenderSequence() + 10;

    node->setChannelSelectorMessage("same text", posting);
    node->setPersistentMessage(eMessageTypeError, "same text");
    node->clearChannelSelectorMessageFromRender(posting + 1, false);
    EXPECT_FALSE(node->hasPersistentMessage()) << "a generic repost took the channel error away from its clear";

    node->setPersistentMessage(eMessageTypeError, "generic text");
    node->setChannelSelectorMessage("generic text", posting + 2);
    node->clearChannelSelectorMessageFromRender(posting + 3, false);
    EXPECT_EQ(QString::fromUtf8("generic text"), messageOf(node)) << "a channel repost handed a generic error to the channel clear";
}

TEST_F(PersistentMessageTest, RenderPostedErrorSurvivesOlderAndAbortedPassesAndClearsAtAGoodFrame)
{
    createSwitchShuffleGraph();
    if (HasFatalFailure()) {
        return;
    }
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const U64 beforeFailure = AbortableRenderInfo::getLatestRenderSequence();
    EXPECT_FALSE(renderFrame(tmp, 2));
    ASSERT_TRUE(_shuffle->hasPersistentMessage());
    EXPECT_TRUE(messageOf(_shuffle).contains(QString::fromUtf8("diffuse"))) << messageOf(_shuffle).toStdString();
    EXPECT_GT(AbortableRenderInfo::getLatestRenderSequence(), beforeFailure);

    _shuffle->clearChannelSelectorMessageFromRender(beforeFailure, false);
    EXPECT_TRUE(_shuffle->hasPersistentMessage()) << "a render older than the failing one cleared it";

    _shuffle->clearChannelSelectorMessageFromRender(AbortableRenderInfo::getLatestRenderSequence() + 1, true);
    EXPECT_TRUE(_shuffle->hasPersistentMessage()) << "an aborted render cleared it";

    _shuffle->refreshChannelSelectorMessageAtTime(2);
    EXPECT_TRUE(_shuffle->hasPersistentMessage()) << "the frame that fails cleared it";

    _shuffle->refreshChannelSelectorMessageAtTime(1);
    EXPECT_FALSE(_shuffle->hasPersistentMessage()) << "moving to a frame that renders left it";

    EXPECT_FALSE(renderFrame(tmp, 2));
    ASSERT_TRUE(_shuffle->hasPersistentMessage()) << "a render after the clear did not post again";

    EXPECT_TRUE(renderFrame(tmp, 1));
    EXPECT_FALSE(_shuffle->hasPersistentMessage()) << "a newer passing render left it";
}

TEST_F(PersistentMessageTest, UpstreamCollectorFindsTheErroringShuffleOnlyWhileItErrs)
{
    createSwitchShuffleGraph();
    if (HasFatalFailure()) {
        return;
    }
    // Stands in for what a viewer's A input is connected to.
    NodePtr viewed = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    ASSERT_TRUE(bool(viewed));
    connectNodes(_shuffle, viewed, 0, true);

    NodePtr unrelated = createNode(QString::fromUtf8(PLUGINID_NATRON_SHUFFLE));
    ASSERT_TRUE(bool(unrelated));
    unrelated->setPersistentMessage(eMessageTypeError, "not upstream");
    ASSERT_TRUE(unrelated->hasPersistentMessage());

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    EXPECT_FALSE(renderFrame(tmp, 2));
    ASSERT_TRUE(_shuffle->hasPersistentMessage());

    NodesList roots;
    roots.push_back(viewed);
    NodesList found;
    Node::getNodesWithPersistentMessageUpstream(roots, &found);
    EXPECT_TRUE(contains(found, _shuffle));
    EXPECT_FALSE(contains(found, unrelated));
    EXPECT_EQ(1, (int)std::count(found.begin(), found.end(), _shuffle));

    EXPECT_TRUE(renderFrame(tmp, 1));
    ASSERT_FALSE(_shuffle->hasPersistentMessage());

    found.clear();
    Node::getNodesWithPersistentMessageUpstream(roots, &found);
    EXPECT_FALSE(contains(found, _shuffle));
    EXPECT_FALSE(contains(found, unrelated));
}

TEST_F(PersistentMessageTest, PreviewKeepsAChannelSelectorErrorAndClearsAGenericOne)
{
    NodePtr reader = createReader("flat-three-layers.exr");
    ASSERT_TRUE(bool(reader));
    NodePtr shuffle = createNode(QString::fromUtf8(PLUGINID_NATRON_SHUFFLE));
    ASSERT_TRUE(bool(shuffle));
    connectNodes(reader, shuffle, Shuffle::eInputMain, true);

    const int size = 32;
    std::vector<unsigned int> buffer(size * size, 0);

    // Owned by a render newer than the preview's, so only the preview's own clear could drop it.
    shuffle->setChannelSelectorMessage("owned channel error", std::numeric_limits<U64>::max());
    ASSERT_TRUE(shuffle->hasPersistentMessage());
    int width = size;
    int height = size;
    shuffle->makePreviewImage(1, &width, &height, &buffer[0]);
    EXPECT_EQ(QString::fromUtf8("owned channel error"), messageOf(shuffle));

    shuffle->clearChannelSelectorMessage();
    ASSERT_FALSE(shuffle->hasPersistentMessage());
    shuffle->setPersistentMessage(eMessageTypeError, "generic error");
    width = size;
    height = size;
    shuffle->makePreviewImage(1, &width, &height, &buffer[0]);
    EXPECT_FALSE(shuffle->hasPersistentMessage()) << messageOf(shuffle).toStdString();
}
