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

#ifndef Engine_LiveSession_SessionPresence_h
#define Engine_LiveSession_SessionPresence_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <QtCore/QElapsedTimer>
#include <QtCore/QObject>
#include <QtCore/QTimer>

#include "Serialization/Session/SessionProtocol.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Who is in a live session and what each user is doing: their selection, the nodes whose
 * panels they have open and their viewer. One table per peer, this user's own entry included.
 *
 * This user's entry goes to the sender at most once per throttle interval, and only when it
 * differs from the one sent last. None of it is part of the project.
 *
 * Main thread only.
 **/
class SessionPresence
    : public QObject {
    Q_OBJECT

public:
    typedef Serialization::Session::PresenceMessage Entry;
    typedef Serialization::Session::PresenceViewerState ViewerState;
    typedef std::function<void(const Entry&)> Sender;

    static constexpr int kDefaultThrottleMs = 100;

    explicit SessionPresence(QObject* parent = nullptr);
    ~SessionPresence() override;

    void setSender(const Sender& sender);

    void setThrottleInterval(int milliseconds);

    /**
     * @brief Makes this user part of the table under userId, keeping the selection, panels and
     * viewer set before, and sends the entry.
     **/
    void setLocalUser(quint32 userId, const std::string& name, const Serialization::NodeUiColor& colour, bool host);

    bool hasLocalUser() const;
    quint32 localUserId() const;

    void setLocalSelection(const std::vector<std::string>& nodeIds);
    void setLocalOpenPanels(const std::vector<std::string>& nodeIds);
    void setLocalViewer(const std::optional<ViewerState>& viewer);

    /**
     * @brief Sends this user's entry now if it differs from the one sent last.
     **/
    void flush();

    /**
     * @brief Stores another user's entry, replacing the one it had. This user's own ID is ignored.
     **/
    void updateUser(const Entry& entry);

    void removeUser(quint32 userId);

    /**
     * @brief Forgets every user, this one included, and stops sending.
     **/
    void clear();

    /**
     * @brief Every user, ordered by ID, this one included once it is part of the table.
     **/
    std::vector<Entry> users() const;

    /**
     * @brief The user's entry, or null.
     **/
    const Entry* user(quint32 userId) const;

    /**
     * @brief The users other than this one that have nodeId selected, or its panel open.
     **/
    std::vector<Entry> remoteUsersSelecting(const std::string& nodeId) const;
    std::vector<Entry> remoteUsersWithPanelOpen(const std::string& nodeId) const;

Q_SIGNALS:

    /**
     * @brief Emitted when a user enters or leaves the table, or takes another name.
     **/
    void usersChanged();

    /**
     * @brief Emitted when the entry of userId differs, or is gone.
     **/
    void userChanged(quint32 userId);

    /**
     * @brief Emitted when the viewer of userId differs, or is gone.
     **/
    void viewerChanged(quint32 userId);

private:
    void localChanged();
    void sendNow();

    Sender _sender;
    std::map<quint32, Entry> _users;
    Entry _local;
    bool _hasLocalUser = false;
    bool _dirty = false;
    int _throttleMs = kDefaultThrottleMs;
    QElapsedTimer _lastSent;
    QTimer _timer;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_LiveSession_SessionPresence_h
