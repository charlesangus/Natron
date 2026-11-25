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

#ifndef Engine_LiveSession_SessionClient_h
#define Engine_LiveSession_SessionClient_h

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
 * @brief A client of a live session: replaces the project of one app with the host's, then keeps
 * the two in step.
 *
 * The app's own edits are applied at once and sent to the host, which numbers them with
 * everyone's and broadcasts them back. Until that acknowledgement arrives an edit is pending,
 * and an edit of another user that writes the same thing is skipped: the host applies the
 * pending one after it, so it wins on every peer. An edit of another user that the pending ones
 * keep from applying here, a connection closing a cycle with a pending one for instance, waits
 * until they are acknowledged or refused. The host's checksum is compared whenever nothing is
 * pending, and a mismatch reloads the whole project from the host.
 *
 * Main thread only. Leaving, or losing the host, keeps the project as it is.
 **/
class SessionClient
    : public QObject {
    Q_OBJECT

public:
    enum class State {
        eDisconnected,
        eConnecting,
        eJoining,
        eJoined,
    };

    explicit SessionClient(const AppInstancePtr& app, QObject* parent = nullptr);
    ~SessionClient() override;

    /**
     * @brief Connects and asks to join; joined() or left() follows.
     **/
    void connectToHost(const QString& host, quint16 port, const QString& displayName);

    /**
     * @brief Tells the host and disconnects; left() gives reason, or a default one when it is
     * empty.
     **/
    void leave(const QString& reason = QString());

    /**
     * @brief Deletes this client from the event loop once it is not handling a message: a slot of
     * one of its signals may call it, unlike delete, while loading the host's project for instance.
     **/
    void scheduleDeletion();

    /**
     * @brief Asks the host to save the shared project.
     **/
    void requestSave();

    State state() const;

    /**
     * @brief The ID the host gave this user, 0 before joining.
     **/
    quint32 userId() const;

    /**
     * @brief The number of the last host edit applied.
     **/
    std::uint64_t seq() const;

    /**
     * @brief Delivers this app's recorded edits now.
     **/
    void flushLocalEdits();

    /**
     * @brief True while some edits sent have not been acknowledged.
     **/
    bool hasPending() const;

    /**
     * @brief True when nothing is waiting to be read or written.
     **/
    bool isIdle() const;

    /**
     * @brief How many host checksums differed from this project's.
     **/
    int checksumMismatchCount() const;

    /**
     * @brief How many of this user's edits the host rejected, at least in part.
     **/
    int rejectCount() const;

    /**
     * @brief How many times the whole project was loaded from the host, the join included.
     **/
    int projectLoadCount() const;

    /**
     * @brief Who is in the session, as the host relays it. This user is part of it once joined.
     **/
    SessionPresence* presence() const;

Q_SIGNALS:

    void joined();

    /**
     * @brief The session ended for this app: it left, the host closed it, or the connection was
     * lost. reason is for the user.
     **/
    void left(const QString& reason);

    /**
     * @brief A line for the user about the session.
     **/
    void sessionMessage(const QString& message);

    /**
     * @brief Another user's edit, applied here, deleted the nodes with the IDs in deleted and
     * changed those in edited.
     **/
    void remoteNodesEdited(const QStringList& deleted, const QStringList& edited);

private:
    struct Implementation;
    std::unique_ptr<Implementation> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_LiveSession_SessionClient_h
