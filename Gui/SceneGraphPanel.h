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

#ifndef Gui_SceneGraphPanel_h
#define Gui_SceneGraphPanel_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>

#include <QWidget>
#include <QtCore/QModelIndex>
#include <QtCore/QStringList>

#include "Engine/EngineFwd.h"
#include "Gui/GuiFwd.h"
#include "Gui/PanelWidget.h"

NATRON_NAMESPACE_ENTER

class SceneGraphModel;

/**
 * @brief The prims of a scene as a tree, beside the 3D viewer. It shows the scene the last active
 * viewer shows, or the scene of a pinned node. Selecting rows selects the prims in the viewer and
 * the viewer's selection selects the rows. A name filter lists the matching prims instead of the
 * tree. Below the tree, the current prim's attributes at the viewer's frame, with the node that
 * authored each one. Rows drag their prim paths onto prim path and pattern knobs.
 *
 * Fold and Unfold (buttons and the tree's context menu) fold the selected prims to their bounding
 * boxes in the followed viewer (SceneViewport::setFolded).
 *
 * Scripts find its widgets by object name: sceneGraphTree, sceneGraphResults, sceneGraphFilter,
 * sceneGraphPin, sceneGraphFold, sceneGraphUnfold, sceneGraphNode, sceneGraphStatus and
 * sceneGraphAttributes. It mirrors its state into the natronSceneGraphPinned,
 * natronSceneGraphNode, natronSceneGraphHasScene, natronSceneGraphFilterBusy and
 * natronSceneGraphFolded properties.
 **/
class SceneGraphPanel
    : public QWidget,
      public PanelWidget {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    explicit SceneGraphPanel(Gui* gui);

    ~SceneGraphPanel() override;

    SceneGraphModel* getModel() const;

    // Shows node's scene whichever viewer is active; null follows the active viewer again.
    void pinNode(const NodePtr& node);

    NodePtr getPinnedNode() const;

    // The node whose scene is shown, pinned or viewed; null for none.
    NodePtr getShownNode() const;

    // Selects the rows of paths, reading the tree down to them, without telling the viewer.
    void showSelection(const QStringList& paths);

    // The selected prims, the current one last.
    QStringList getSelection() const;

public Q_SLOTS:

    void onActiveViewerChanged();

private Q_SLOTS:

    void onViewerSceneChanged();
    void onSceneSynced();
    void onViewportSelectionChanged(const QStringList& paths);
    void onTreeSelectionChanged();
    void onResultsSelectionChanged();
    void onResultActivated(const QModelIndex& index);
    void onFilterEdited();
    void onFilterStep();
    void onPinToggled(bool pinned);
    void onNodeChosen(int index);
    void onPinnedPoll();
    void onPinnedSynced(bool ok, const QString& error);
    void onModelReset();
    void onExpanded(const QModelIndex& index);
    void onCollapsed(const QModelIndex& index);
    void onCurrentChanged(const QModelIndex& current);
    void refreshAttributes();

protected:
    bool eventFilter(QObject* watched, QEvent* e) override;
    void enterEvent(QEnterEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void keyReleaseEvent(QKeyEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;

private:
    struct Implementation;
    std::unique_ptr<Implementation> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_SceneGraphPanel_h
