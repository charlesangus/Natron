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

#ifndef Serialization_Session_SessionProtocol_h
#define Serialization_Session_SessionProtocol_h

#include "Global/Macros.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "Serialization/Ops/GraphOp.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {
namespace Session {

    /**
     * @brief The version a peer announces in hello. A host refuses a peer announcing another one.
     **/
    inline constexpr int kProtocolVersion = 1;

    /**
     * @brief The largest frame body accepted. A frame announcing more ends the stream.
     **/
    inline constexpr std::size_t kMaxFrameBytes = std::size_t(512) << 20;

    /**
     * @brief Thrown when a message cannot be encoded or decoded. The stream it came from cannot be
     * trusted any further.
     **/
    class SessionProtocolError
        : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    // Client to host, first message on a connection.
    struct HelloMessage {
        int protocol = kProtocolVersion;
        std::string name;
        std::string natronVersion;
    };

    /**
     * @brief Host to client: the answer to hello, and to resyncRequest. snapshot is the project bundle
     * (.ntpz bytes) at seq; projectPath and projectName are the host's project file.
     **/
    struct WelcomeMessage {
        std::uint32_t userId = 0;
        NodeUiColor colour;
        std::uint64_t seq = 0;
        std::string projectPath;
        std::string projectName;
        std::vector<std::byte> snapshot;
    };

    // Client to host: operations recorded locally, numbered by the client from 1.
    struct OpsMessage {
        std::uint64_t clientSeq = 0;
        std::vector<GraphOp> ops;
    };

    /**
     * @brief Host to every client: the operations the host applied as its edit number seq. They come
     * from originUser (0 for the host's own user); for a client's edit, clientSeq is the number that
     * client gave it, and the message is that client's acknowledgement.
     **/
    struct BroadcastMessage {
        std::uint64_t seq = 0;
        std::uint32_t originUser = 0;
        std::uint64_t clientSeq = 0;
        std::vector<GraphOp> ops;
    };

    // Host to the originator: operations of clientSeq the host could not apply.
    struct RejectMessage {
        std::uint64_t clientSeq = 0;
        std::string reason;
    };

    /**
     * @brief Host to one client: the host's state of one node. When deleted is false, ops recreate
     * it: a createNode followed by the setInput operations of its own connections, those of the
     * nodes inside it, and those of the nodes it feeds.
     **/
    struct ResyncNodeMessage {
        std::string nodeId;
        bool deleted = false;
        std::vector<GraphOp> ops;
    };

    // Host to every client: the hash of the host's canonical project.yaml once edit seq is applied.
    struct ChecksumMessage {
        std::uint64_t seq = 0;
        std::string hash;
    };

    // Client to host: save the shared project file.
    struct SaveRequestMessage {
    };

    // Client to host: send the whole project again, as a welcome.
    struct ResyncRequestMessage {
    };

    // Either way: the session ends for the receiver, for reason.
    struct ByeMessage {
        std::string reason;
    };

    typedef std::variant<HelloMessage,
                         WelcomeMessage,
                         OpsMessage,
                         BroadcastMessage,
                         RejectMessage,
                         ResyncNodeMessage,
                         ChecksumMessage,
                         SaveRequestMessage,
                         ResyncRequestMessage,
                         ByeMessage>
        SessionMessage;

    /**
     * @brief The `type` key of a message in YAML, e.g. "broadcast".
     **/
    std::string_view sessionMessageName(const SessionMessage& message);

    /**
     * @brief One message as one YAML map: `type`, then its fields under their names. Operations are
     * lists as encodeGraphOpsYaml writes them, the snapshot is `!!binary`. Throws
     * SessionProtocolError when an operation cannot be encoded.
     **/
    std::string encodeSessionMessage(const SessionMessage& message);

    /**
     * @brief Reads what encodeSessionMessage writes. Unknown keys are ignored, so that a later
     * version of the same protocol may add some. Throws SessionProtocolError for anything else that
     * does not decode, an unreadable operation included.
     **/
    SessionMessage decodeSessionMessage(std::string_view text);

    /**
     * @brief The message as it goes on the wire: its encoded length as a big-endian u32, then the
     * encoding. Throws SessionProtocolError when it is larger than kMaxFrameBytes.
     **/
    std::vector<std::byte> frameSessionMessage(const SessionMessage& message);

    /**
     * @brief Cuts a byte stream into messages, whatever the way the bytes arrive. Once it fails, it
     * stays failed: the bytes after a bad frame cannot be resynchronised.
     **/
    class SessionFrameDecoder {
    public:
        enum class Status {
            eNeedMore,
            eMessage,
            eError,
        };

        void feed(std::span<const std::byte> bytes);
        void feed(const char* data, std::size_t size);

        /**
         * @brief eMessage with the next complete message in message, eNeedMore when no complete frame
         * is buffered, eError once a frame is empty, over kMaxFrameBytes or does not decode.
         **/
        Status next(SessionMessage* message);

        const std::string& error() const
        {
            return _error;
        }

        std::size_t bufferedBytes() const
        {
            return _buffer.size() - _offset;
        }

    private:
        Status failWith(std::string message);

        std::vector<std::byte> _buffer;
        std::size_t _offset = 0;
        std::string _error;
    };

} // namespace Session
} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_Session_SessionProtocol_h
