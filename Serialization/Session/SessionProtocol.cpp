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

#include "Serialization/Session/SessionProtocol.h"

#include <cstring>
#include <type_traits>
#include <utility>

#include "Serialization/Yaml/YamlPrivate.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {
namespace Session {

    namespace {

        using namespace YamlDetail;

        template <class T>
        struct MessageName;

        template <>
        struct MessageName<HelloMessage> {
            static constexpr std::string_view value = "hello";
        };
        template <>
        struct MessageName<WelcomeMessage> {
            static constexpr std::string_view value = "welcome";
        };
        template <>
        struct MessageName<OpsMessage> {
            static constexpr std::string_view value = "ops";
        };
        template <>
        struct MessageName<BroadcastMessage> {
            static constexpr std::string_view value = "broadcast";
        };
        template <>
        struct MessageName<RejectMessage> {
            static constexpr std::string_view value = "reject";
        };
        template <>
        struct MessageName<ResyncNodeMessage> {
            static constexpr std::string_view value = "resyncNode";
        };
        template <>
        struct MessageName<ChecksumMessage> {
            static constexpr std::string_view value = "checksum";
        };
        template <>
        struct MessageName<SaveRequestMessage> {
            static constexpr std::string_view value = "saveRequest";
        };
        template <>
        struct MessageName<ResyncRequestMessage> {
            static constexpr std::string_view value = "resyncRequest";
        };
        template <>
        struct MessageName<ByeMessage> {
            static constexpr std::string_view value = "bye";
        };
        template <>
        struct MessageName<PresenceMessage> {
            static constexpr std::string_view value = "presence";
        };
        template <>
        struct MessageName<PresenceLeftMessage> {
            static constexpr std::string_view value = "presenceLeft";
        };

        void
        emitStringField(YAML::Emitter& em,
                        std::string_view key,
                        const std::string& value)
        {
            emitKey(em, key);
            em << YAML::Value;
            emitString(em, value);
        }

        void
        emitIntField(YAML::Emitter& em,
                     std::string_view key,
                     long long value)
        {
            emitKey(em, key);
            em << YAML::Value;
            emitInt(em, value);
        }

        void
        emitDoubleField(YAML::Emitter& em,
                        std::string_view key,
                        double value)
        {
            emitKey(em, key);
            em << YAML::Value;
            emitDouble(em, value);
        }

        void
        emitBoolField(YAML::Emitter& em,
                      std::string_view key,
                      bool value)
        {
            emitKey(em, key);
            em << YAML::Value;
            emitBool(em, value);
        }

        void
        emitStringListField(YAML::Emitter& em,
                            std::string_view key,
                            const std::vector<std::string>& values)
        {
            emitKey(em, key);
            em << YAML::Value;
            emitStringList(em, values);
        }

        void
        emitOpsField(YAML::Emitter& em,
                     const std::vector<GraphOp>& ops)
        {
            emitKey(em, "ops");
            em << YAML::Value;
            emitGraphOpList(em, ops);
        }

        std::uint64_t
        readSeq(MapReader& reader,
                std::string_view key)
        {
            const std::string context = reader.context() + "." + std::string(key);
            const YAML::Node node = reader.get(key);
            if (!node) {
                fail(node, context, "missing");
            }
            const long long value = readInt64(node, context);
            if (value < 0) {
                fail(node, context, "expected a non-negative number");
            }

            return static_cast<std::uint64_t>(value);
        }

        std::string
        readStringField(MapReader& reader,
                        std::string_view key)
        {
            const std::string context = reader.context() + "." + std::string(key);
            const YAML::Node node = reader.get(key);
            if (!node) {
                fail(node, context, "missing");
            }

            return readString(node, context);
        }

        YAML::Node
        requiredField(MapReader& reader,
                      std::string_view key)
        {
            const YAML::Node node = reader.get(key);
            if (!node) {
                fail(node, reader.context() + "." + std::string(key), "missing");
            }

            return node;
        }

        double
        readDoubleField(MapReader& reader,
                        std::string_view key)
        {
            return readDouble(requiredField(reader, key), reader.context() + "." + std::string(key));
        }

        int
        readIntField(MapReader& reader,
                     std::string_view key)
        {
            return readInt(requiredField(reader, key), reader.context() + "." + std::string(key));
        }

        bool
        readBoolField(MapReader& reader,
                      std::string_view key)
        {
            return readBool(requiredField(reader, key), reader.context() + "." + std::string(key));
        }

        std::vector<std::string>
        readStringListField(MapReader& reader,
                            std::string_view key)
        {
            return readStringVector(requiredField(reader, key), reader.context() + "." + std::string(key));
        }

        std::vector<GraphOp>
        readOpsField(MapReader& reader)
        {
            const std::string context = reader.context() + ".ops";
            const YAML::Node node = reader.get("ops");
            if (!node) {
                fail(node, context, "missing");
            }
            std::vector<GraphOp> ops;
            for (DecodedGraphOp& decoded : readGraphOpList(node, context, nullptr)) {
                if (!decoded.op) {
                    throw SessionProtocolError("unreadable operation: " + decoded.error);
                }
                ops.push_back(std::move(*decoded.op));
            }

            return ops;
        }

        void
        emitFields(YAML::Emitter& em,
                   const HelloMessage& message)
        {
            emitIntField(em, "protocol", message.protocol);
            emitStringField(em, "name", message.name);
            emitStringField(em, "natronVersion", message.natronVersion);
        }

        void
        emitFields(YAML::Emitter& em,
                   const WelcomeMessage& message)
        {
            emitIntField(em, "userId", message.userId);
            emitFloatTriple(em, "colour", message.colour.r, message.colour.g, message.colour.b);
            emitIntField(em, "seq", static_cast<long long>(message.seq));
            emitStringField(em, "projectName", message.projectName);
            emitKey(em, "snapshot");
            em << YAML::Value << YAML::Binary(reinterpret_cast<const unsigned char*>(message.snapshot.data()), message.snapshot.size());
        }

        void
        emitFields(YAML::Emitter& em,
                   const OpsMessage& message)
        {
            emitIntField(em, "clientSeq", static_cast<long long>(message.clientSeq));
            emitOpsField(em, message.ops);
        }

        void
        emitFields(YAML::Emitter& em,
                   const BroadcastMessage& message)
        {
            emitIntField(em, "seq", static_cast<long long>(message.seq));
            emitIntField(em, "originUser", message.originUser);
            emitIntField(em, "clientSeq", static_cast<long long>(message.clientSeq));
            emitOpsField(em, message.ops);
        }

        void
        emitFields(YAML::Emitter& em,
                   const RejectMessage& message)
        {
            emitIntField(em, "clientSeq", static_cast<long long>(message.clientSeq));
            emitStringField(em, "reason", message.reason);
        }

        void
        emitFields(YAML::Emitter& em,
                   const ResyncNodeMessage& message)
        {
            emitStringField(em, "nodeId", message.nodeId);
            emitKey(em, "deleted");
            em << YAML::Value;
            emitBool(em, message.deleted);
            emitOpsField(em, message.ops);
        }

        void
        emitFields(YAML::Emitter& em,
                   const ChecksumMessage& message)
        {
            emitIntField(em, "seq", static_cast<long long>(message.seq));
            emitStringField(em, "hash", message.hash);
        }

        void
        emitFields(YAML::Emitter&,
                   const SaveRequestMessage&)
        {
        }

        void
        emitFields(YAML::Emitter&,
                   const ResyncRequestMessage&)
        {
        }

        void
        emitFields(YAML::Emitter& em,
                   const ByeMessage& message)
        {
            emitStringField(em, "reason", message.reason);
        }

        void
        emitFields(YAML::Emitter& em,
                   const PresenceMessage& message)
        {
            emitIntField(em, "userId", message.userId);
            emitStringField(em, "name", message.name);
            emitFloatTriple(em, "colour", message.colour.r, message.colour.g, message.colour.b);
            emitBoolField(em, "host", message.host);
            emitStringListField(em, "selection", message.selection);
            emitStringListField(em, "openPanels", message.openPanels);
            if (!message.viewer) {
                return;
            }
            const PresenceViewerState& viewer = *message.viewer;
            emitKey(em, "viewer");
            em << YAML::Value << YAML::BeginMap;
            emitIntField(em, "frame", viewer.frame);
            emitStringField(em, "viewerNodeId", viewer.viewerNodeId);
            emitIntField(em, "inputIndex", viewer.inputIndex);
            emitDoubleField(em, "zoomFactor", viewer.zoomFactor);
            emitDoubleField(em, "zoomAspectRatio", viewer.zoomAspectRatio);
            emitDoubleField(em, "centerX", viewer.centerX);
            emitDoubleField(em, "centerY", viewer.centerY);
            emitStringField(em, "channels", viewer.channels);
            em << YAML::EndMap;
        }

        void
        emitFields(YAML::Emitter& em,
                   const PresenceLeftMessage& message)
        {
            emitIntField(em, "userId", message.userId);
        }

        void
        readFields(MapReader& reader,
                   HelloMessage* message)
        {
            const YAML::Node protocol = reader.get("protocol");
            if (!protocol) {
                fail(protocol, reader.context() + ".protocol", "missing");
            }
            message->protocol = readInt(protocol, reader.context() + ".protocol");
            message->name = readStringField(reader, "name");
            message->natronVersion = readStringField(reader, "natronVersion");
        }

        void
        readFields(MapReader& reader,
                   WelcomeMessage* message)
        {
            const std::string userIdContext = reader.context() + ".userId";
            const YAML::Node userId = reader.get("userId");
            if (!userId) {
                fail(userId, userIdContext, "missing");
            }
            message->userId = readUInt(userId, userIdContext);
            const std::string colourContext = reader.context() + ".colour";
            const YAML::Node colour = reader.get("colour");
            if (!colour) {
                fail(colour, colourContext, "missing");
            }
            const std::vector<float> rgb = readFloatTriple(colour, colourContext);
            message->colour = NodeUiColor { rgb[0], rgb[1], rgb[2] };
            message->seq = readSeq(reader, "seq");
            message->projectName = readStringField(reader, "projectName");
            const std::string snapshotContext = reader.context() + ".snapshot";
            const YAML::Node snapshot = reader.get("snapshot");
            if (!snapshot || !snapshot.IsScalar()) {
                fail(snapshot, snapshotContext, "expected the project bundle as binary");
            }
            const YAML::Binary binary = snapshot.as<YAML::Binary>();
            const std::byte* data = reinterpret_cast<const std::byte*>(binary.data());
            message->snapshot.assign(data, data + binary.size());
        }

        void
        readFields(MapReader& reader,
                   OpsMessage* message)
        {
            message->clientSeq = readSeq(reader, "clientSeq");
            message->ops = readOpsField(reader);
        }

        void
        readFields(MapReader& reader,
                   BroadcastMessage* message)
        {
            message->seq = readSeq(reader, "seq");
            const std::string originContext = reader.context() + ".originUser";
            const YAML::Node origin = reader.get("originUser");
            if (!origin) {
                fail(origin, originContext, "missing");
            }
            message->originUser = readUInt(origin, originContext);
            message->clientSeq = readSeq(reader, "clientSeq");
            message->ops = readOpsField(reader);
        }

        void
        readFields(MapReader& reader,
                   RejectMessage* message)
        {
            message->clientSeq = readSeq(reader, "clientSeq");
            message->reason = readStringField(reader, "reason");
        }

        void
        readFields(MapReader& reader,
                   ResyncNodeMessage* message)
        {
            message->nodeId = readStringField(reader, "nodeId");
            const std::string deletedContext = reader.context() + ".deleted";
            const YAML::Node deleted = reader.get("deleted");
            if (!deleted) {
                fail(deleted, deletedContext, "missing");
            }
            message->deleted = readBool(deleted, deletedContext);
            message->ops = readOpsField(reader);
        }

        void
        readFields(MapReader& reader,
                   ChecksumMessage* message)
        {
            message->seq = readSeq(reader, "seq");
            message->hash = readStringField(reader, "hash");
        }

        void
        readFields(MapReader&,
                   SaveRequestMessage*)
        {
        }

        void
        readFields(MapReader&,
                   ResyncRequestMessage*)
        {
        }

        void
        readFields(MapReader& reader,
                   ByeMessage* message)
        {
            message->reason = readStringField(reader, "reason");
        }

        void
        readFields(MapReader& reader,
                   PresenceMessage* message)
        {
            message->userId = readUInt(requiredField(reader, "userId"), reader.context() + ".userId");
            message->name = readStringField(reader, "name");
            const std::vector<float> rgb = readFloatTriple(requiredField(reader, "colour"), reader.context() + ".colour");
            message->colour = NodeUiColor { rgb[0], rgb[1], rgb[2] };
            message->host = readBoolField(reader, "host");
            message->selection = readStringListField(reader, "selection");
            message->openPanels = readStringListField(reader, "openPanels");
            const YAML::Node viewer = reader.get("viewer");
            if (!viewer) {
                message->viewer.reset();

                return;
            }
            const std::string viewerContext = reader.context() + ".viewer";
            if (!viewer.IsMap()) {
                fail(viewer, viewerContext, "expected a map");
            }
            MapReader viewerReader(viewer, viewerContext, nullptr);
            PresenceViewerState state;
            state.frame = readIntField(viewerReader, "frame");
            state.viewerNodeId = readStringField(viewerReader, "viewerNodeId");
            state.inputIndex = readIntField(viewerReader, "inputIndex");
            state.zoomFactor = readDoubleField(viewerReader, "zoomFactor");
            state.zoomAspectRatio = readDoubleField(viewerReader, "zoomAspectRatio");
            state.centerX = readDoubleField(viewerReader, "centerX");
            state.centerY = readDoubleField(viewerReader, "centerY");
            state.channels = readStringField(viewerReader, "channels");
            message->viewer = state;
        }

        void
        readFields(MapReader& reader,
                   PresenceLeftMessage* message)
        {
            message->userId = readUInt(requiredField(reader, "userId"), reader.context() + ".userId");
        }

        template <std::size_t I = 0>
        bool
        decodeAlternative(std::string_view type,
                          MapReader& reader,
                          SessionMessage* message)
        {
            if constexpr (I < std::variant_size_v<SessionMessage>) {
                using T = std::variant_alternative_t<I, SessionMessage>;
                if (type == MessageName<T>::value) {
                    T decoded;
                    readFields(reader, &decoded);
                    *message = std::move(decoded);

                    return true;
                }

                return decodeAlternative<I + 1>(type, reader, message);
            } else {
                return false;
            }
        }

        std::uint32_t
        readBigEndian32(const std::byte* bytes)
        {
            return (std::uint32_t(bytes[0]) << 24) | (std::uint32_t(bytes[1]) << 16) | (std::uint32_t(bytes[2]) << 8) | std::uint32_t(bytes[3]);
        }

    } // namespace

    std::string_view
    sessionMessageName(const SessionMessage& message)
    {
        return std::visit([](const auto& alternative) {
            return MessageName<std::decay_t<decltype(alternative)>>::value;
        },
                          message);
    }

    std::string
    encodeSessionMessage(const SessionMessage& message)
    {
        try {
            YAML::Emitter em;
            em << YAML::BeginMap;
            emitStringField(em, "type", std::string(sessionMessageName(message)));
            std::visit([&em](const auto& alternative) { emitFields(em, alternative); }, message);
            em << YAML::EndMap;

            return documentText(em);
        } catch (const YamlError& e) {
            throw SessionProtocolError(std::string("cannot encode a session message: ") + e.what());
        } catch (const YAML::Exception& e) {
            throw SessionProtocolError(std::string("cannot encode a session message: ") + e.what());
        }
    }

    SessionMessage
    decodeSessionMessage(std::string_view text)
    {
        try {
            const YAML::Node root = loadDocument(text, "a session message");
            if (!root || !root.IsMap()) {
                fail(root, "message", "expected a map");
            }
            MapReader reader(root, "message", nullptr);
            const YAML::Node typeNode = reader.get("type");
            if (!typeNode) {
                fail(root, "message.type", "missing");
            }
            const std::string type = readString(typeNode, "message.type");
            SessionMessage message;
            if (!decodeAlternative(type, reader, &message)) {
                fail(typeNode, "message.type", "unknown message \"" + type + "\"");
            }

            return message;
        } catch (const SessionProtocolError&) {
            throw;
        } catch (const YamlError& e) {
            throw SessionProtocolError(e.what());
        } catch (const YAML::Exception& e) {
            throw SessionProtocolError(e.what());
        }
    }

    std::vector<std::byte>
    frameSessionMessage(const SessionMessage& message)
    {
        const std::string text = encodeSessionMessage(message);
        if (text.size() > kMaxFrameBytes) {
            throw SessionProtocolError("a " + std::string(sessionMessageName(message)) + " message of " + std::to_string(text.size()) + " bytes is over the frame limit");
        }
        const std::uint32_t size = static_cast<std::uint32_t>(text.size());
        std::vector<std::byte> frame(4 + text.size());
        frame[0] = std::byte((size >> 24) & 0xff);
        frame[1] = std::byte((size >> 16) & 0xff);
        frame[2] = std::byte((size >> 8) & 0xff);
        frame[3] = std::byte(size & 0xff);
        if (!text.empty()) {
            std::memcpy(frame.data() + 4, text.data(), text.size());
        }

        return frame;
    }

    void
    SessionFrameDecoder::feed(std::span<const std::byte> bytes)
    {
        if (!_error.empty() || bytes.empty()) {
            return;
        }
        // Consumed bytes are dropped once they outweigh the rest, so feeding stays linear.
        if (_offset > 0 && _offset >= _buffer.size() - _offset) {
            _buffer.erase(_buffer.begin(), _buffer.begin() + static_cast<std::ptrdiff_t>(_offset));
            _offset = 0;
        }
        _buffer.insert(_buffer.end(), bytes.begin(), bytes.end());
    }

    void
    SessionFrameDecoder::feed(const char* data,
                              std::size_t size)
    {
        feed(std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), size));
    }

    SessionFrameDecoder::Status
    SessionFrameDecoder::failWith(std::string message)
    {
        _error = std::move(message);
        _buffer.clear();
        _offset = 0;

        return Status::eError;
    }

    SessionFrameDecoder::Status
    SessionFrameDecoder::next(SessionMessage* message)
    {
        if (!_error.empty()) {
            return Status::eError;
        }
        if (bufferedBytes() < 4) {
            return Status::eNeedMore;
        }
        const std::uint32_t size = readBigEndian32(_buffer.data() + _offset);
        if (size == 0) {
            return failWith("empty session frame");
        }
        if (size > _maxFrameBytes) {
            return failWith("a session frame of " + std::to_string(size) + " bytes is over the limit of " + std::to_string(_maxFrameBytes));
        }
        if (bufferedBytes() < 4 + std::size_t(size)) {
            return Status::eNeedMore;
        }
        const std::string_view text(reinterpret_cast<const char*>(_buffer.data() + _offset + 4), size);
        _offset += 4 + std::size_t(size);
        try {
            *message = decodeSessionMessage(text);
        } catch (const SessionProtocolError& e) {
            return failWith(std::string("bad session frame: ") + e.what());
        }
        if (_offset == _buffer.size()) {
            _buffer.clear();
            _offset = 0;
        }

        return Status::eMessage;
    }

} // namespace Session
} // namespace Serialization
NATRON_NAMESPACE_EXIT
