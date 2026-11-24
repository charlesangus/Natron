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

#include "LiveSessionUsersPanel.h"

#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QPixmap>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "Engine/AppInstance.h"
#include "Engine/LiveSession/SessionPresence.h"

#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/LiveSessionPresenceTracker.h"

NATRON_NAMESPACE_ENTER

namespace {

enum ColumnEnum {
    eColumnUser = 0,
    eColumnRole,
    eColumnFollow,
    eColumnCount
};

} // namespace

LiveSessionUsersPanel::LiveSessionUsersPanel(Gui* gui,
                                             LiveSessionPresenceTracker* tracker)
    : QWidget(gui)
    , PanelWidget(this, gui)
    , _tracker(tracker)
    , _list(nullptr)
    , _empty(nullptr)
{
    setObjectName(QString::fromUtf8("liveSessionUsersPanel"));
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(2);

    _empty = new QLabel(tr("Not in a live session"), this);
    _empty->setObjectName(QString::fromUtf8("liveSessionUsersEmpty"));
    layout->addWidget(_empty);

    _list = new QTreeWidget(this);
    _list->setObjectName(QString::fromUtf8("liveSessionUsers"));
    _list->setColumnCount(eColumnCount);
    _list->setHeaderLabels(QStringList() << tr("User") << tr("Role") << tr("Follow"));
    _list->setRootIsDecorated(false);
    _list->setUniformRowHeights(true);
    _list->header()->setStretchLastSection(false);
    _list->header()->setSectionResizeMode(eColumnUser, QHeaderView::Stretch);
    _list->setToolTip(tr("Double-click a user to centre the node graph on their selection. Follow mirrors their viewer "
                         "until this viewer is used."));
    layout->addWidget(_list);

    QObject::connect(_list, &QTreeWidget::itemChanged, this, &LiveSessionUsersPanel::onItemChanged);
    QObject::connect(_list, &QTreeWidget::itemDoubleClicked, this, &LiveSessionUsersPanel::onItemDoubleClicked);
    GuiAppInstancePtr app = gui->getApp();
    if (app) {
        QObject::connect(app.get(), &AppInstance::liveSessionChanged, this, &LiveSessionUsersPanel::refresh);
        QObject::connect(app.get(), &AppInstance::liveSessionUsersChanged, this, &LiveSessionUsersPanel::refresh);
    }
    if (_tracker) {
        QObject::connect(_tracker, &LiveSessionPresenceTracker::followChanged, this, &LiveSessionUsersPanel::refresh);
    }
    refresh();
}

LiveSessionUsersPanel::~LiveSessionUsersPanel()
{
}

void
LiveSessionUsersPanel::refresh()
{
    GuiAppInstancePtr app = getGui() ? getGui()->getApp() : GuiAppInstancePtr();
    SessionPresence* presence = app ? app->getLiveSessionPresence() : nullptr;
    const std::vector<SessionPresence::Entry> users = presence ? presence->users() : std::vector<SessionPresence::Entry>();
    const std::optional<quint32> leader = _tracker ? _tracker->getLeader() : std::nullopt;

    if (_inItemChanged) {
        if (!_refreshQueued) {
            _refreshQueued = true;
            QMetaObject::invokeMethod(this, [this]() {
                _refreshQueued = false;
                refresh(); }, Qt::QueuedConnection);
        }

        return;
    }
    _refreshing = true;
    _list->clear();
    for (const SessionPresence::Entry& user : users) {
        const bool self = presence->hasLocalUser() && (presence->localUserId() == user.userId);
        QTreeWidgetItem* item = new QTreeWidgetItem(_list);
        const QColor colour = QColor::fromRgbF(user.colour.r, user.colour.g, user.colour.b);
        QPixmap swatch(12, 12);
        swatch.fill(colour);
        item->setIcon(eColumnUser, QIcon(swatch));
        item->setText(eColumnUser, QString::fromStdString(user.name));
        item->setData(eColumnUser, Qt::UserRole, QVariant::fromValue<uint>(user.userId));
        QStringList roles;
        if (self) {
            roles << tr("you");
        }
        if (user.host) {
            roles << tr("host");
        }
        item->setText(eColumnRole, roles.join(QString::fromUtf8(", ")));
        if (!self) {
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(eColumnFollow, (leader && (*leader == user.userId)) ? Qt::Checked : Qt::Unchecked);
            item->setToolTip(eColumnFollow, tr("Mirror the viewer of %1").arg(QString::fromStdString(user.name)));
        }
    }
    _refreshing = false;
    _empty->setVisible(users.empty());
    _list->setVisible(!users.empty());
}

void
LiveSessionUsersPanel::onItemChanged(QTreeWidgetItem* item,
                                     int column)
{
    if (_refreshing || !_tracker || (column != eColumnFollow)) {
        return;
    }
    const quint32 userId = item->data(eColumnUser, Qt::UserRole).toUInt();
    const bool checked = (item->checkState(eColumnFollow) == Qt::Checked);
    _inItemChanged = true;
    if (checked) {
        _tracker->follow(userId);
    } else {
        const std::optional<quint32> leader = _tracker->getLeader();
        if (leader && (*leader == userId)) {
            _tracker->stopFollowing(tr("Stopped following"));
        }
    }
    // Unchecks the user followed before, and a user that could not be followed.
    refresh();
    _inItemChanged = false;
}

void
LiveSessionUsersPanel::onItemDoubleClicked(QTreeWidgetItem* item,
                                           int /*column*/)
{
    if (_tracker) {
        _tracker->frameSelectionOf(item->data(eColumnUser, Qt::UserRole).toUInt());
    }
}

void
LiveSessionUsersPanel::enterEvent(QEnterEvent* e)
{
    enterEventBase();
    QWidget::enterEvent(e);
}

void
LiveSessionUsersPanel::leaveEvent(QEvent* e)
{
    leaveEventBase();
    QWidget::leaveEvent(e);
}

void
LiveSessionUsersPanel::keyPressEvent(QKeyEvent* e)
{
    handleUnCaughtKeyPressEvent(e);
    QWidget::keyPressEvent(e);
}

void
LiveSessionUsersPanel::keyReleaseEvent(QKeyEvent* e)
{
    handleUnCaughtKeyUpEvent(e);
    QWidget::keyReleaseEvent(e);
}

void
LiveSessionUsersPanel::mousePressEvent(QMouseEvent* e)
{
    takeClickFocus();
    QWidget::mousePressEvent(e);
}

NATRON_NAMESPACE_EXIT
