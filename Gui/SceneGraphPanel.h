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

#include <QAbstractItemView>
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
 * viewer shows, or, following the node selection, that of the scene node last selected in the
 * node graph, or the scene of a pinned node. When the rows are the viewer's prims, selecting rows
 * selects the prims in the viewer and the viewer's selection selects the rows. The filter lists
 * the matching prims instead of the tree: by name, or, for text that starts with / or holds { or
 * %, by the path expression a prim pattern knob takes, whose matches are selected as they are
 * found and which Apply To Node writes to the current node's first prim pattern knob. Beside the
 * field, the expression's error or how many unloaded branches were not searched. Below the tree, a ScenePrimInspector on the current
 * prim at the viewer's frame. Rows drag their prim paths onto prim path and pattern knobs.
 * Twirling open a row of an unloaded payload loads that payload alone, and the row menu loads and
 * unloads the selection, on the stage the rows are read from; closing a row never unloads.
 *
 * A node the viewer does not show is read on a worker into a stage of the panel's own, opened with
 * no payload loaded; the rows of the stage left stay up, greyed, until it is read. The stages of
 * the last few nodes shown are kept, so that going back to one reads nothing unless the node was
 * edited after its stage was read. The open rows and the row at the top of the tree carry over from node to node:
 * a row whose prim is missing reopens when it comes back, and an unloaded payload row stays
 * closed until twirled.
 *
 * Scripts find its widgets by object name: sceneGraphTree, sceneGraphChangedHereTree,
 * sceneGraphResults, sceneGraphFilter, sceneGraphApplyToNode, sceneGraphFilterMessage, sceneGraphPin, sceneGraphFollow, sceneGraphNode,
 * sceneGraphChangedHere and sceneGraphStatus, and the inspector's. Rows the shown node's own layers
 * author something on, and their ancestors, are marked (SceneGraphModel::eChangedHereRole); the
 * Changed Here button swaps the tree for one listing only those (SceneChangedHereModel), leaving
 * the tree as it was. It mirrors its state into the natronSceneGraphPinned, natronSceneGraphFollow
 * ("viewer" or "selection"), natronSceneGraphNode, natronSceneGraphHasScene,
 * natronSceneGraphFilterBusy, natronSceneGraphFilterExpression, natronSceneGraphFilterError,
 * natronSceneGraphFilterUnloaded, natronSceneGraphStageEditCount, natronSceneGraphStageNotices,
 * natronSceneGraphChangedHereOnly, natronSceneGraphChangedHereBusy, natronSceneGraphChangedHereCount,
 * natronSceneGraphChangedHereRows, natronSceneGraphComposeCount (evaluations that reached a stage
 * of the panel's own), natronSceneGraphSwitching, natronSceneGraphCachedStages,
 * natronSceneGraphTimersActive and natronSceneGraphTopPath properties.
 **/
class SceneGraphPanel
    : public QWidget,
      public PanelWidget {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    enum FollowEnum {
        eFollowViewer = 0,
        eFollowSelection
    };

    explicit SceneGraphPanel(Gui* gui);

    ~SceneGraphPanel() override;

    SceneGraphModel* getModel() const;

    // Shows node's scene whichever viewer is active; null follows the active viewer again.
    void pinNode(const NodePtr& node);

    NodePtr getPinnedNode() const;

    // What the panel shows when not pinned: the active viewer's scene, or the selected node's.
    void setFollow(FollowEnum follow);

    FollowEnum getFollow() const;

    // The node whose scene is shown, pinned or viewed; null for none.
    NodePtr getShownNode() const;

    // Selects the rows of paths, reading the tree down to them, without telling the viewer.
    void showSelection(const QStringList& paths);

    // The selected prims, the current one last.
    QStringList getSelection() const;

    // Shows, and publishes as natronSceneArmedPick, the knob an armed pick writes to, "node.knob";
    // empty when none is armed.
    void setArmedPick(const QString& label);

    // Selects the rows of paths and scrolls to the last, opening their ancestors one level at a time
    // so that no payload is loaded on the way. The viewer's selection is left as it is.
    void revealPaths(const QStringList& paths);

    // Whether the panel is unpinned and follows the active viewer, which is viewer.
    bool followsViewer(const ViewerTab* viewer) const;

public Q_SLOTS:

    void onActiveViewerChanged();

private Q_SLOTS:

    void onViewerSceneChanged();
    void onSceneSynced();
    void onStageLoadsChanged();
    void onViewportSelectionChanged(const QStringList& paths);
    void onTreeSelectionChanged();
    void onChangedHereSelectionChanged();
    void onResultsSelectionChanged();
    void onResultActivated(const QModelIndex& index);
    void onFilterEdited();
    void onFilterStep();
    void onApplyToNode();
    void onPinToggled(bool pinned);
    void onNodeChosen(int index);
    void onPinnedPoll();
    void onSelectionPoll();
    void onPinnedSynced(bool ok, const QString& error);
    void onModelReset();
    void onExpanded(const QModelIndex& index);
    void onCollapsed(const QModelIndex& index);
    void onCurrentChanged(const QModelIndex& current);
    void refreshInspector();

protected:
    bool eventFilter(QObject* watched, QEvent* e) override;
    void enterEvent(QEnterEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void keyReleaseEvent(QKeyEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;

private:
    void copySelectedPaths();
    void showRowMenu(QAbstractItemView* view, const QPoint& pos);

    // Reads node's scene into a stage of the panel's own; null shows the followed scene.
    void showNodeScene(const NodePtr& node);

    struct Implementation;
    std::unique_ptr<Implementation> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_SceneGraphPanel_h
