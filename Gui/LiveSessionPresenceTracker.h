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

#ifndef NATRON_GUI_LIVESESSIONPRESENCETRACKER_H
#define NATRON_GUI_LIVESESSIONPRESENCETRACKER_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/GlobalDefines.h"
#include "Global/Macros.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QStringList>
#include <QtCore/QTimer>

#include "Engine/EngineFwd.h"
#include "Gui/GuiFwd.h"
#include "Gui/LiveSessionUserMark.h"

class QLabel;

NATRON_NAMESPACE_ENTER

class SessionPresence;

/**
 * @brief Ties the live session presence table to this window: sends this user's selection, open
 * panels and viewer, marks the nodes and panels other users are on, keeps the undo history clear
 * of entries remote edits invalidate, and makes a viewer follow another user's.
 *
 * Following shows the followed user's viewer node in this window, with their frame, zoom, pan,
 * input A and display channels. Those are applied here only: they are not recorded as graph edits
 * or undo entries, so they never reach another user.
 *
 * Following stops on a mouse press, wheel or key press in any viewer, on a timeline move this user
 * made, and on an edit made here of the followed viewer's inputs or channels that leaves it
 * showing something else than the followed user's.
 *
 * While in a session, the viewer whose state is sent holds it in its natronLiveSessionZoom,
 * natronLiveSessionCenterX, natronLiveSessionCenterY and natronLiveSessionViewer (the viewer
 * node's script name) properties, for scripts. A followed user's name shows in the viewer's
 * liveSessionFollowLabel.
 **/
class LiveSessionPresenceTracker
    : public QObject {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    explicit LiveSessionPresenceTracker(Gui* gui);
    ~LiveSessionPresenceTracker() override;

    /**
     * @brief Sends this user's selection and open panels once control returns to the event loop.
     **/
    void scheduleLocalUpdate();

    /**
     * @brief Mirrors userId's viewer from now on, in place of any user followed before.
     **/
    void follow(quint32 userId);

    void stopFollowing(const QString& note = QString());

    std::optional<quint32> getLeader() const;

    /**
     * @brief Centres the node graph on the nodes userId has selected.
     **/
    void frameSelectionOf(quint32 userId);

Q_SIGNALS:

    void followChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* e) override;

private Q_SLOTS:

    void onSessionChanged();
    void onUsersChanged();
    void onPresenceChanged(quint32 userId);
    void onViewerChanged(quint32 userId);
    void onRemoteEdit(const QStringList& deleted, const QStringList& edited);
    void onReplayedOverRemoteEdit(const QString& text, bool undone);
    void onTimelineFrameChanged(SequenceTime time, int reason);
    void sendLocal();
    void sendLocalViewer();

private:
    SessionPresence* presence() const;
    ViewerTab* localViewer() const;
    void refreshMarks();
    void applySelectionMarks(const std::string& nodeId);
    void applyPanelMarks(const std::string& nodeId);
    void applyLeaderViewer();
    void showLeaderLabel(ViewerTab* tab, const QString& name);
    void watchFollowedViewer(ViewerTab* tab);
    void unwatchFollowedViewer();
    void onFollowedViewerKnobChanged(bool mirrored);
    void showNote(const QString& note);

    Gui* _gui;
    QTimer _localTimer;
    QTimer _viewerTimer;
    std::map<std::string, LiveSessionUserMarks> _selectionMarks;
    std::map<std::string, LiveSessionUserMarks> _panelMarks;
    std::optional<quint32> _leader;
    QPointer<QLabel> _leaderLabel;
    QPointer<ViewerTab> _followedTab;
    std::vector<QMetaObject::Connection> _followedTabKnobs;
    bool _applyingLeader = false;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_LIVESESSIONPRESENCETRACKER_H
