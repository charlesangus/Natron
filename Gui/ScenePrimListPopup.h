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

#ifndef Gui_ScenePrimListPopup_h
#define Gui_ScenePrimListPopup_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <QtCore/QAbstractListModel>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtWidgets/QFrame>

#include "Gui/GuiFwd.h"

NATRON_NAMESPACE_ENTER

class SceneViewport;

/**
 * @brief The names of one prim's children, read from the stage a batch at a time as the view asks
 * for more, so a prim with any number of children lists without a long read on the main thread.
 * Never waits for the stage: a read that an edit keeps out is tried again shortly. The list starts
 * over when a sync edits the stage, a load edits it, or the viewer starts or stops showing one;
 * a sync that changes nothing leaves it.
 **/
class ScenePrimListModel : public QAbstractListModel {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    enum RoleEnum {
        // The child's full path.
        ePathRole = Qt::UserRole + 1,
        eTypeRole
    };

    ScenePrimListModel(SceneViewport* viewport, const QString& parentPath, QObject* parent = nullptr);

    ~ScenePrimListModel() override;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    bool canFetchMore(const QModelIndex& parent) const override;
    void fetchMore(const QModelIndex& parent) override;

    // Every child is in the list.
    bool isComplete() const;

    // The prim has a payload that is not loaded: its children are not there to list.
    bool isNotLoaded() const;

    // No stage has been shown yet: the list fills when one is.
    bool isWaitingForStage() const;

    QString getParentPath() const;

    QString getChildPath(const QString& name) const;

    // Starts the list over.
    void refresh();

Q_SIGNALS:
    // isComplete(), isNotLoaded() or isWaitingForStage() may have changed.
    void stateChanged();

public Q_SLOTS:

    // Starts over if the stage changed since the list was read, or none was there yet.
    void checkEdits();

private Q_SLOTS:
    void fetchBatch();

private:
    struct Private;
    Private* const _imp;
};

/**
 * @brief The popup under a breadcrumb chevron: a filter field over the children of a prim. With a
 * name to highlight it is the prim's siblings, listed under their parent. A pick selects the prim.
 * A prim whose payload is not loaded shows a "Not loaded" row with a Load button instead of
 * children, and lists them once the load lands.
 *
 * The widget publishes natronPopupParent, natronPopupRows (the first rows past the filter),
 * natronPopupRowCount, natronPopupLoadedCount, natronPopupComplete, natronPopupNotLoaded,
 * natronPopupWaiting, natronPopupCurrent and natronPopupFilter for GUI tests. Its filter field,
 * list and Load button are named "scenePrimListFilter", "scenePrimListView" and "scenePrimListLoad".
 **/
class ScenePrimListPopup : public QFrame {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    ScenePrimListPopup(SceneViewport* viewport, const QString& parentPath, const QString& highlightName, QWidget* parent = nullptr);

    ~ScenePrimListPopup() override;

    // Shows the popup with its top left corner at globalPos.
    void popupAt(const QPoint& globalPos);

public Q_SLOTS:

    // Selects the child of that name and closes. Does nothing when it is not listed.
    void pick(const QString& name);

    void setFilterText(const QString& text);

    // Asks the viewport to load the prim whose children are listed.
    void load();

protected:
    bool eventFilter(QObject* watched, QEvent* e) override;

private Q_SLOTS:
    void onModelChanged();
    void pump();

private:
    void publish();
    void pickIndex(const QModelIndex& proxyIndex);

    struct Private;
    Private* const _imp;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_ScenePrimListPopup_h
