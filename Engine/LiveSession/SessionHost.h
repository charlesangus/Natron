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

#ifndef Engine_LiveSession_SessionHost_h
#define Engine_LiveSession_SessionHost_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstdint>
#include <memory>

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER

class SessionPresence;

/**
 * @brief The host of a live session: shares the project of one app with every client that joins
 * on a TCP port.
 *
 * Every edit gets the next number in the session: the clients' operations, applied here
 * without being recorded, and the host user's own, taken from the project's recorder. Each is
 * then broadcast to every client, its originator included as the acknowledgement. An operation
 * the project refuses is reported to its originator with a reject, followed by the host's state
 * of the node it named. A client joining, or asking for it, gets the whole project. Every
 * checksum interval the clients get the hash of the project, to find out whether they diverged.
 *
 * Main thread only. The host stops when destroyed. A client that has not said hello yet may send
 * only a small frame, and one that stops reading is cut off.
 **/
class SessionHost
    : public QObject {
    Q_OBJECT

public:
    explicit SessionHost(const AppInstancePtr& app, QObject* parent = nullptr);
    ~SessionHost() override;

    /**
     * @brief Listens on the IP address bindAddress, every interface when it is empty, and port (0
     * picks a free one). Returns false with a message for the user in error when it cannot.
     **/
    bool start(const QString& bindAddress, quint16 port, const QString& displayName, QString* error);

    /**
     * @brief Tells every client the session ends, then closes every connection.
     **/
    void stop();

    /**
     * @brief Deletes this host from the event loop once no call of it is running: a slot of one of
     * its signals may call it, unlike delete. Call stop() first to tell the clients.
     **/
    void scheduleDeletion();

    bool isRunning() const;
    quint16 serverPort() const;

    /**
     * @brief The clients that have joined.
     **/
    int peerCount() const;

    /**
     * @brief The number of the last edit broadcast.
     **/
    std::uint64_t seq() const;

    /**
     * @brief Milliseconds between checksums, 5000 by default; 0 sends none unless asked.
     **/
    void setChecksumInterval(int milliseconds);

    /**
     * @brief Sends the clients the project's checksum now.
     **/
    void broadcastChecksum();

    /**
     * @brief Delivers the host user's recorded edits now.
     **/
    void flushLocalEdits();

    /**
     * @brief True when nothing is waiting to be read from or written to any client.
     **/
    bool isIdle() const;

    /**
     * @brief How many client edits were rejected, at least in part.
     **/
    int rejectCount() const;

    /**
     * @brief Who is in the session, the host user first, as user 0.
     **/
    SessionPresence* presence() const;

Q_SIGNALS:

    void peerCountChanged(int count);

    /**
     * @brief A line for the user about the session: a peer joined or left, a save failed.
     **/
    void sessionMessage(const QString& message);

    /**
     * @brief A client asked for the project to be saved, which is done by now if the project has a
     * file and unsaved changes.
     **/
    void saveRequested(quint32 userId);

    /**
     * @brief A client's edit, applied here, deleted the nodes with the IDs in deleted and changed
     * those in edited.
     **/
    void remoteNodesEdited(const QStringList& deleted, const QStringList& edited);

private:
    struct Implementation;
    std::unique_ptr<Implementation> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_LiveSession_SessionHost_h
