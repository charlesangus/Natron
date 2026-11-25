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

#ifndef NATRON_GUI_LIVESESSIONUSERSPANEL_H
#define NATRON_GUI_LIVESESSIONUSERSPANEL_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <QWidget>

#include "Gui/GuiFwd.h"
#include "Gui/PanelWidget.h"

class QLabel;
class QTreeWidget;
class QTreeWidgetItem;

NATRON_NAMESPACE_ENTER

class LiveSessionPresenceTracker;

/**
 * @brief The users of the live session: their colour, name, whether they are this user or the
 * host, and a Follow toggle that mirrors their viewer. Double-clicking a user centres the node
 * graph on their selection.
 *
 * Scripts find the list by the object name liveSessionUsers; the user ID of a row is its first
 * column's Qt::UserRole data.
 **/
class LiveSessionUsersPanel
    : public QWidget,
      public PanelWidget {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    LiveSessionUsersPanel(Gui* gui, LiveSessionPresenceTracker* tracker);
    ~LiveSessionUsersPanel() override;

public Q_SLOTS:

    void refresh();

private Q_SLOTS:

    void onItemChanged(QTreeWidgetItem* item, int column);
    void onItemDoubleClicked(QTreeWidgetItem* item, int column);

protected:
    void enterEvent(QEnterEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void keyReleaseEvent(QKeyEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;

private:
    LiveSessionPresenceTracker* _tracker;
    QTreeWidget* _list;
    QLabel* _empty;
    bool _refreshing = false;
    // The item whose change is handled must outlive its setData, so the list is rebuilt after it.
    bool _inItemChanged = false;
    bool _refreshQueued = false;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_LIVESESSIONUSERSPANEL_H
