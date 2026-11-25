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
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "Serialization/GraphOpTestHelpers.h"
#include "Serialization/Ops/GraphOp.h"
#include "Serialization/Session/SessionProtocol.h"
#include "Serialization/Yaml/GraphOpYaml.h"
#include "Serialization/Yaml/KnobYaml.h"

NATRON_NAMESPACE_USING
using namespace Serialization;
using namespace Serialization::Session;
using GraphOpTest::nid;

namespace {

KnobSerialization
knobFromYaml(const std::string& text)
{
    std::list<KnobSerialization> knobs = decodeKnobsYaml(text);
    EXPECT_EQ(knobs.size(), 1u);

    return knobs.front();
}

std::vector<GraphOp>
sampleOps()
{
    NodeSerializationPtr node = std::make_shared<NodeSerialization>();
    node->id = nid(7);
    node->scriptName = "Blur1";
    node->label = "Blur1";
    node->pluginID = "net.sf.cimg.CImgBlur";
    node->pluginMajorVersion = 1;
    node->pluginMinorVersion = 0;
    node->knobs.push_back(knobFromYaml("size: 3.5"));

    std::vector<GraphOp> ops;
    ops.push_back(GraphOp(CreateNodeOp { std::string(), node }));
    ops.push_back(GraphOp(SetInputOp { nid(7), "Source", nid(3) }));
    ops.push_back(GraphOp(SetKnobOp { nid(7), "size", knobFromYaml("size: 4") }));
    ops.push_back(GraphOp(RenameOp { nid(7), "Soft", std::string("Soft blur") }));
    ops.push_back(GraphOp(DeleteNodeOp { nid(3) }));

    return ops;
}

std::vector<std::byte>
sampleSnapshot()
{
    std::vector<std::byte> bytes;
    for (int i = 0; i < 1000; ++i) {
        bytes.push_back(std::byte((i * 37) & 0xff));
    }

    return bytes;
}

PresenceMessage
samplePresence()
{
    PresenceMessage presence;
    presence.userId = 4;
    presence.name = "Bea";
    presence.colour = NodeUiColor { 0.3f, 0.65f, 0.95f };
    presence.host = false;
    presence.selection = { nid(7), nid(3) };
    presence.openPanels = { nid(7) };
    PresenceViewerState viewer;
    viewer.frame = 20;
    viewer.viewerNodeId = nid(9);
    viewer.inputIndex = 1;
    viewer.zoomFactor = 2.5;
    viewer.zoomAspectRatio = 1.;
    viewer.centerX = 960.5;
    viewer.centerY = -540.25;
    viewer.channels = "Luminance";
    presence.viewer = viewer;

    return presence;
}

std::vector<SessionMessage>
everyMessage()
{
    std::vector<SessionMessage> messages;
    messages.push_back(HelloMessage { kProtocolVersion, "Ana \"the compositor\"", "2.6.0" });
    messages.push_back(WelcomeMessage { 3, NodeUiColor { 0.25f, 0.5f, 1.f }, 42, "comp.ntpz", sampleSnapshot() });
    messages.push_back(WelcomeMessage { 1, NodeUiColor {}, 0, std::string(), std::vector<std::byte>() });
    messages.push_back(OpsMessage { 9, sampleOps() });
    messages.push_back(OpsMessage { 10, std::vector<GraphOp>() });
    messages.push_back(BroadcastMessage { 43, 3, 9, sampleOps() });
    messages.push_back(BroadcastMessage { 44, 0, 0, std::vector<GraphOp>() });
    messages.push_back(RejectMessage { 9, "the node Blur1 does not exist" });
    messages.push_back(ResyncNodeMessage { nid(7), false, sampleOps() });
    messages.push_back(ResyncNodeMessage { nid(8), true, std::vector<GraphOp>() });
    messages.push_back(ChecksumMessage { 44, "9f86d081884c7d659a2feaa0c55ad015" });
    messages.push_back(SaveRequestMessage {});
    messages.push_back(ResyncRequestMessage {});
    messages.push_back(ByeMessage { "The host closed the session." });
    messages.push_back(samplePresence());
    messages.push_back(PresenceMessage { 0, "host", NodeUiColor { 1.f, 0.5f, 0.f }, true, {}, {}, std::nullopt });
    messages.push_back(PresenceLeftMessage { 4 });

    return messages;
}

std::vector<std::byte>
concatenatedFrames(const std::vector<SessionMessage>& messages)
{
    std::vector<std::byte> stream;
    for (const SessionMessage& message : messages) {
        const std::vector<std::byte> frame = frameSessionMessage(message);
        stream.insert(stream.end(), frame.begin(), frame.end());
    }

    return stream;
}

std::vector<std::string>
drain(SessionFrameDecoder* decoder)
{
    std::vector<std::string> texts;
    SessionMessage message;
    while (decoder->next(&message) == SessionFrameDecoder::Status::eMessage) {
        texts.push_back(encodeSessionMessage(message));
    }

    return texts;
}

std::vector<std::byte>
frameOfText(const std::string& text)
{
    const std::uint32_t size = static_cast<std::uint32_t>(text.size());
    std::vector<std::byte> frame = { std::byte(size >> 24), std::byte((size >> 16) & 0xff), std::byte((size >> 8) & 0xff), std::byte(size & 0xff) };
    for (char c : text) {
        frame.push_back(std::byte(static_cast<unsigned char>(c)));
    }

    return frame;
}

} // namespace

TEST(SessionProtocol, EveryMessageRoundTrips)
{
    for (const SessionMessage& message : everyMessage()) {
        const std::string text = encodeSessionMessage(message);
        EXPECT_NE(text.find(std::string(sessionMessageName(message))), std::string::npos) << text;
        const SessionMessage decoded = decodeSessionMessage(text);
        EXPECT_EQ(message.index(), decoded.index()) << text;
        EXPECT_EQ(text, encodeSessionMessage(decoded));
    }
}

TEST(SessionProtocol, FieldsSurviveTheRoundTrip)
{
    const WelcomeMessage welcome = std::get<WelcomeMessage>(decodeSessionMessage(encodeSessionMessage(everyMessage()[1])));
    EXPECT_EQ(welcome.userId, 3u);
    EXPECT_EQ(welcome.colour, (NodeUiColor { 0.25f, 0.5f, 1.f }));
    EXPECT_EQ(welcome.seq, 42u);
    EXPECT_EQ(welcome.projectName, "comp.ntpz");
    EXPECT_EQ(welcome.snapshot, sampleSnapshot());

    const BroadcastMessage broadcast = std::get<BroadcastMessage>(decodeSessionMessage(encodeSessionMessage(everyMessage()[5])));
    EXPECT_EQ(broadcast.seq, 43u);
    EXPECT_EQ(broadcast.originUser, 3u);
    EXPECT_EQ(broadcast.clientSeq, 9u);
    ASSERT_EQ(broadcast.ops.size(), sampleOps().size());
    EXPECT_EQ(encodeGraphOpsYaml(broadcast.ops), encodeGraphOpsYaml(sampleOps()));

    const HelloMessage hello = std::get<HelloMessage>(decodeSessionMessage(encodeSessionMessage(everyMessage()[0])));
    EXPECT_EQ(hello.protocol, kProtocolVersion);
    EXPECT_EQ(hello.name, "Ana \"the compositor\"");
    EXPECT_EQ(hello.natronVersion, "2.6.0");

    const ResyncNodeMessage deleted = std::get<ResyncNodeMessage>(decodeSessionMessage(encodeSessionMessage(everyMessage()[9])));
    EXPECT_EQ(deleted.nodeId, nid(8));
    EXPECT_TRUE(deleted.deleted);
    EXPECT_TRUE(deleted.ops.empty());
}

TEST(SessionProtocol, PresenceFieldsSurviveTheRoundTrip)
{
    const PresenceMessage presence = std::get<PresenceMessage>(decodeSessionMessage(encodeSessionMessage(samplePresence())));
    EXPECT_EQ(presence, samplePresence());
    ASSERT_TRUE(presence.viewer.has_value());
    EXPECT_EQ(presence.viewer->frame, 20);
    EXPECT_EQ(presence.viewer->centerY, -540.25);

    const PresenceMessage bare { 0, "host", NodeUiColor { 1.f, 0.5f, 0.f }, true, {}, {}, std::nullopt };
    const PresenceMessage decodedBare = std::get<PresenceMessage>(decodeSessionMessage(encodeSessionMessage(bare)));
    EXPECT_EQ(decodedBare, bare);
    EXPECT_FALSE(decodedBare.viewer.has_value());

    const PresenceLeftMessage left = std::get<PresenceLeftMessage>(decodeSessionMessage(encodeSessionMessage(PresenceLeftMessage { 4 })));
    EXPECT_EQ(left.userId, 4u);
}

TEST(SessionProtocol, APresenceWithoutItsSelectionIsAnError)
{
    std::string text = encodeSessionMessage(samplePresence());
    const std::string::size_type at = text.find("selection");
    ASSERT_NE(at, std::string::npos);
    text.replace(at, 9, "elsewhere");
    EXPECT_THROW(decodeSessionMessage(text), SessionProtocolError);
}

TEST(SessionProtocol, SplitAndCoalescedStreamsDecodeIdentically)
{
    const std::vector<SessionMessage> messages = everyMessage();
    std::vector<std::string> expected;
    for (const SessionMessage& message : messages) {
        expected.push_back(encodeSessionMessage(message));
    }
    const std::vector<std::byte> stream = concatenatedFrames(messages);

    SessionFrameDecoder whole;
    whole.feed(stream);
    EXPECT_EQ(drain(&whole), expected);
    EXPECT_EQ(whole.bufferedBytes(), 0u);

    SessionFrameDecoder byteByByte;
    std::vector<std::string> texts;
    for (std::byte b : stream) {
        byteByByte.feed(std::span<const std::byte>(&b, 1));
        std::vector<std::string> some = drain(&byteByByte);
        texts.insert(texts.end(), some.begin(), some.end());
    }
    EXPECT_EQ(texts, expected);

    std::mt19937 random(1234);
    for (int trial = 0; trial < 20; ++trial) {
        SessionFrameDecoder chunked;
        texts.clear();
        std::size_t offset = 0;
        while (offset < stream.size()) {
            const std::size_t chunk = std::min<std::size_t>(stream.size() - offset, 1 + random() % 700);
            chunked.feed(std::span<const std::byte>(stream.data() + offset, chunk));
            offset += chunk;
            std::vector<std::string> some = drain(&chunked);
            texts.insert(texts.end(), some.begin(), some.end());
        }
        EXPECT_EQ(texts, expected) << "trial " << trial;
    }
}

TEST(SessionProtocol, IncompleteFrameWaitsForMore)
{
    const std::vector<std::byte> frame = frameSessionMessage(ByeMessage { "done" });
    SessionFrameDecoder decoder;
    decoder.feed(std::span<const std::byte>(frame.data(), frame.size() - 1));
    SessionMessage message;
    EXPECT_EQ(decoder.next(&message), SessionFrameDecoder::Status::eNeedMore);
    decoder.feed(std::span<const std::byte>(frame.data() + frame.size() - 1, 1));
    ASSERT_EQ(decoder.next(&message), SessionFrameDecoder::Status::eMessage);
    EXPECT_EQ(std::get<ByeMessage>(message).reason, "done");
    EXPECT_EQ(decoder.next(&message), SessionFrameDecoder::Status::eNeedMore);
}

TEST(SessionProtocol, OversizeFrameIsAnErrorBeforeItsBodyArrives)
{
    const std::uint32_t size = static_cast<std::uint32_t>(kMaxFrameBytes + 1);
    const std::vector<std::byte> header = { std::byte(size >> 24), std::byte((size >> 16) & 0xff), std::byte((size >> 8) & 0xff), std::byte(size & 0xff) };
    SessionFrameDecoder decoder;
    decoder.feed(header);
    SessionMessage message;
    EXPECT_EQ(decoder.next(&message), SessionFrameDecoder::Status::eError);
    EXPECT_FALSE(decoder.error().empty());

    const std::vector<std::byte> good = frameSessionMessage(SaveRequestMessage {});
    decoder.feed(good);
    EXPECT_EQ(decoder.next(&message), SessionFrameDecoder::Status::eError);
}

TEST(SessionProtocol, ALoweredLimitRefusesALargerFrameAndRaisingItAcceptsIt)
{
    const std::vector<std::byte> bye = frameSessionMessage(ByeMessage { std::string(64, 'x') });
    SessionMessage message;

    SessionFrameDecoder refusing;
    refusing.setMaxFrameBytes(16);
    refusing.feed(std::span<const std::byte>(bye.data(), 4));
    EXPECT_EQ(refusing.next(&message), SessionFrameDecoder::Status::eError);

    SessionFrameDecoder raised;
    raised.setMaxFrameBytes(16);
    raised.setMaxFrameBytes(kMaxFrameBytes);
    raised.feed(bye);
    ASSERT_EQ(raised.next(&message), SessionFrameDecoder::Status::eMessage);
    EXPECT_EQ(std::get<ByeMessage>(message).reason, std::string(64, 'x'));
}

TEST(SessionProtocol, GarbageFramesAreErrors)
{
    const std::vector<std::string> bodies = {
        "not: [valid",
        "just a scalar",
        "- a\n- list\n",
        "type: \"nonsense\"\n",
        "type: \"broadcast\"\nseq: 1\n",
        "type: \"broadcast\"\nseq: -4\noriginUser: 0\nclientSeq: 0\nops: []\n",
        "type: \"ops\"\nclientSeq: 1\nops:\n  - op: \"flyAway\"\n",
        "type: \"welcome\"\nuserId: 1\ncolour: [0, 0, 0]\nseq: 0\nprojectName: \"\"\nsnapshot: {a: 1}\n",
        std::string("\xff\xfe\x00\x01garbage", 11),
    };
    for (const std::string& body : bodies) {
        SessionFrameDecoder decoder;
        decoder.feed(frameOfText(body));
        SessionMessage message;
        EXPECT_EQ(decoder.next(&message), SessionFrameDecoder::Status::eError) << body;
        EXPECT_FALSE(decoder.error().empty()) << body;
        EXPECT_THROW(decodeSessionMessage(body), SessionProtocolError) << body;
    }

    SessionFrameDecoder empty;
    empty.feed(frameOfText(std::string()));
    SessionMessage message;
    EXPECT_EQ(empty.next(&message), SessionFrameDecoder::Status::eError);
}

TEST(SessionProtocol, RandomBytesNeverCrash)
{
    std::mt19937 random(99);
    for (int trial = 0; trial < 200; ++trial) {
        std::vector<std::byte> bytes(1 + random() % 300);
        for (std::byte& b : bytes) {
            b = std::byte(random() & 0xff);
        }
        // A small length keeps most trials past the header and into the decoder.
        if (trial % 2 == 0 && bytes.size() > 4) {
            const std::uint32_t size = static_cast<std::uint32_t>(bytes.size() - 4);
            bytes[0] = std::byte(0);
            bytes[1] = std::byte(0);
            bytes[2] = std::byte((size >> 8) & 0xff);
            bytes[3] = std::byte(size & 0xff);
        }
        SessionFrameDecoder decoder;
        decoder.feed(bytes);
        SessionMessage message;
        SessionFrameDecoder::Status status;
        int guard = 0;
        do {
            status = decoder.next(&message);
        } while (status == SessionFrameDecoder::Status::eMessage && ++guard < 1000);
        EXPECT_NE(status, SessionFrameDecoder::Status::eMessage);
    }
}
