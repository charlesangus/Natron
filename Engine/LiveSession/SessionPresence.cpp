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

#include "SessionPresence.h"

#include <algorithm>

NATRON_NAMESPACE_ENTER

namespace {

bool
contains(const std::vector<std::string>& ids,
         const std::string& id)
{
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

} // namespace

SessionPresence::SessionPresence(QObject* parent)
    : QObject(parent)
{
    _timer.setSingleShot(true);
    // A coarse timer may fire up to 5% early, which would send more often than the interval allows.
    _timer.setTimerType(Qt::PreciseTimer);
    QObject::connect(&_timer, &QTimer::timeout, this, [this]() { sendNow(); });
}

SessionPresence::~SessionPresence()
{
}

void
SessionPresence::setSender(const Sender& sender)
{
    _sender = sender;
}

void
SessionPresence::setThrottleInterval(int milliseconds)
{
    _throttleMs = std::max(0, milliseconds);
}

void
SessionPresence::setLocalUser(quint32 userId,
                              const std::string& name,
                              const Serialization::NodeUiColor& colour,
                              bool host)
{
    if (_hasLocalUser && (_local.userId != userId)) {
        _users.erase(_local.userId);
    }
    _local.userId = userId;
    _local.name = name;
    _local.colour = colour;
    _local.host = host;
    _hasLocalUser = true;
    _users[userId] = _local;
    Q_EMIT usersChanged();
    Q_EMIT userChanged(userId);
    _dirty = true;
    _timer.stop();
    sendNow();
}

bool
SessionPresence::hasLocalUser() const
{
    return _hasLocalUser;
}

quint32
SessionPresence::localUserId() const
{
    return _local.userId;
}

void
SessionPresence::setLocalSelection(const std::vector<std::string>& nodeIds)
{
    if (_local.selection == nodeIds) {
        return;
    }
    _local.selection = nodeIds;
    localChanged();
}

void
SessionPresence::setLocalOpenPanels(const std::vector<std::string>& nodeIds)
{
    if (_local.openPanels == nodeIds) {
        return;
    }
    _local.openPanels = nodeIds;
    localChanged();
}

void
SessionPresence::setLocalViewer(const std::optional<ViewerState>& viewer)
{
    if (_local.viewer == viewer) {
        return;
    }
    _local.viewer = viewer;
    localChanged();
    if (_hasLocalUser) {
        Q_EMIT viewerChanged(_local.userId);
    }
}

void
SessionPresence::localChanged()
{
    if (!_hasLocalUser) {
        return;
    }
    _users[_local.userId] = _local;
    Q_EMIT userChanged(_local.userId);
    _dirty = true;
    if (_timer.isActive()) {
        return;
    }
    const qint64 sinceLast = _lastSent.isValid() ? _lastSent.elapsed() : qint64(_throttleMs);
    if (sinceLast >= _throttleMs) {
        sendNow();
    } else {
        _timer.start(int(_throttleMs - sinceLast));
    }
}

void
SessionPresence::sendNow()
{
    if (!_dirty || !_hasLocalUser) {
        return;
    }
    _dirty = false;
    _lastSent.start();
    if (_sender) {
        _sender(_local);
    }
}

void
SessionPresence::flush()
{
    _timer.stop();
    sendNow();
}

void
SessionPresence::updateUser(const Entry& entry)
{
    if (_hasLocalUser && (entry.userId == _local.userId)) {
        return;
    }
    std::map<quint32, Entry>::iterator found = _users.find(entry.userId);
    const bool added = (found == _users.end());
    if (!added && (found->second == entry)) {
        return;
    }
    const bool viewerMoved = added ? entry.viewer.has_value() : (found->second.viewer != entry.viewer);
    const bool renamed = !added && (found->second.name != entry.name);
    _users[entry.userId] = entry;
    if (added || renamed) {
        Q_EMIT usersChanged();
    }
    Q_EMIT userChanged(entry.userId);
    if (viewerMoved) {
        Q_EMIT viewerChanged(entry.userId);
    }
}

void
SessionPresence::removeUser(quint32 userId)
{
    if (_hasLocalUser && (userId == _local.userId)) {
        return;
    }
    std::map<quint32, Entry>::iterator found = _users.find(userId);
    if (found == _users.end()) {
        return;
    }
    const bool hadViewer = found->second.viewer.has_value();
    _users.erase(found);
    Q_EMIT usersChanged();
    Q_EMIT userChanged(userId);
    if (hadViewer) {
        Q_EMIT viewerChanged(userId);
    }
}

void
SessionPresence::clear()
{
    _timer.stop();
    _dirty = false;
    _hasLocalUser = false;
    _lastSent.invalidate();
    std::map<quint32, Entry> gone;
    gone.swap(_users);
    if (gone.empty()) {
        return;
    }
    Q_EMIT usersChanged();
    for (const std::pair<const quint32, Entry>& user : gone) {
        Q_EMIT userChanged(user.first);
        if (user.second.viewer) {
            Q_EMIT viewerChanged(user.first);
        }
    }
}

std::vector<SessionPresence::Entry>
SessionPresence::users() const
{
    std::vector<Entry> all;
    all.reserve(_users.size());
    for (const std::pair<const quint32, Entry>& user : _users) {
        all.push_back(user.second);
    }

    return all;
}

const SessionPresence::Entry*
SessionPresence::user(quint32 userId) const
{
    std::map<quint32, Entry>::const_iterator found = _users.find(userId);

    return (found == _users.end()) ? nullptr : &found->second;
}

std::vector<SessionPresence::Entry>
SessionPresence::remoteUsersSelecting(const std::string& nodeId) const
{
    std::vector<Entry> found;
    for (const std::pair<const quint32, Entry>& user : _users) {
        if ((!_hasLocalUser || (user.first != _local.userId)) && contains(user.second.selection, nodeId)) {
            found.push_back(user.second);
        }
    }

    return found;
}

std::vector<SessionPresence::Entry>
SessionPresence::remoteUsersWithPanelOpen(const std::string& nodeId) const
{
    std::vector<Entry> found;
    for (const std::pair<const quint32, Entry>& user : _users) {
        if ((!_hasLocalUser || (user.first != _local.userId)) && contains(user.second.openPanels, nodeId)) {
            found.push_back(user.second);
        }
    }

    return found;
}

NATRON_NAMESPACE_EXIT
