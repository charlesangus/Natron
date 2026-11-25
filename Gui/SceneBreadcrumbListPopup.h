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

#ifndef Gui_SceneBreadcrumbListPopup_h
#define Gui_SceneBreadcrumbListPopup_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <QtCore/QAbstractListModel>
#include <QtCore/QList>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtWidgets/QFrame>

#include "Gui/GuiFwd.h"

class QMimeData;

NATRON_NAMESPACE_ENTER

class SceneViewport;

/**
 * @brief The selected paths, one row each, or with a level set, one row per ancestor at that depth
 * with the number of selected paths under it. Built from the list alone, never from the stage, so a
 * selection of any size lists at once. A path shallower than the level is its own group.
 **/
class SceneBreadcrumbListModel : public QAbstractListModel {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    enum RoleEnum {
        // The row's path: the selected prim, or the group's ancestor.
        ePathRole = Qt::UserRole + 1,
        // The selected paths in the row's group, 0 with no level set.
        eCountRole
    };

    explicit SceneBreadcrumbListModel(QObject* parent = nullptr);

    ~SceneBreadcrumbListModel() override;

    void setPaths(const QStringList& paths);

    // 0 lists the paths themselves.
    void setLevel(int level);

    int getLevel() const;

    int getPathCount() const;

    QString getRowPath(int row) const;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;

private:
    void regroup();

    struct Private;
    Private* const _imp;
};

/**
 * @brief The popup under the breadcrumb bar's "+N" badge: every selected prim, each row drawn as its
 * path with one clickable span per name, and a Level spinner that groups the rows by their ancestor
 * at that depth. A click on a name selects that prim alone; a drag of a row carries its path (a group
 * row its ancestor) in the Scene Graph's mime format. It follows the selection while open.
 *
 * The widget is named "sceneBreadcrumbListPopup" and publishes natronListRows (the first rows'
 * texts, e.g. "/geo/group_1 (14)" when grouped), natronListRowCount, natronListPathCount and
 * natronListLevel for GUI tests. Its list and spinner are named "breadcrumbListView" and
 * "breadcrumbListLevel".
 **/
class SceneBreadcrumbListPopup : public QFrame {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    // A press on anchor closes the popup without reaching anchor, so a button that toggles the popup
    // does not open it again.
    SceneBreadcrumbListPopup(SceneViewport* viewport, const QStringList& paths, QWidget* anchor, QWidget* parent = nullptr);

    ~SceneBreadcrumbListPopup() override;

    // Shows the popup below the widget `under`, its right edge on the anchor's, kept on the screen.
    void popupUnder(QWidget* under);

    // Makes the mime data a drag of the row would carry, as a child named "breadcrumbListMime" that
    // replaces the last one.
    Q_INVOKABLE void exposeMimeData(int row);

public Q_SLOTS:

    void setPaths(const QStringList& paths);

    void setLevel(int level);

    // Clicks the name at index `segment` (0 is the first name under the root) of the row, through the
    // same hit test a mouse click uses. Does nothing when the row or name does not exist.
    void clickSegment(int row, int segment);

protected:
    void mousePressEvent(QMouseEvent* e) override;
    bool eventFilter(QObject* watched, QEvent* e) override;

private:
    void publish();
    bool clickAt(const QPoint& viewportPos);
    void startDrag(int row);

    struct Private;
    Private* const _imp;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_SceneBreadcrumbListPopup_h
