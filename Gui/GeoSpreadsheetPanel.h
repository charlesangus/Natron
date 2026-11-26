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

#ifndef Gui_GeoSpreadsheetPanel_h
#define Gui_GeoSpreadsheetPanel_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>

#include <QWidget>

#include "Engine/EngineFwd.h"
#include "Gui/GuiFwd.h"
#include "Gui/NodeSourceBar.h"
#include "Gui/PanelWidget.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief A node's native geometry as a spreadsheet of its points, vertices, primitives and detail
 * at the viewer's frame. Its source bar (NodeSourceBar) picks the node: by default the one the
 * active viewer's input A shows, or, following the node selection, the node putting out native
 * geometry last selected in the node graph, or a pinned node. The rows are the copy the 3D view
 * serves when it shows that node and the copy is current, so rows and picks share element
 * numbers, else the node evaluated on the panel's worker. A read under way when the node changes is cancelled; one under
 * way when the frame changes or the node is edited finishes, and the panel then reads again. While
 * a read has not finished the last rows stay up, greyed. Nothing is read while the panel is hidden.
 *
 * Scripts find its widgets by object name: geometrySpreadsheetPanel,
 * geometrySpreadsheetPin, geometrySpreadsheetFollow and geometrySpreadsheetNode (the source bar),
 * geometrySpreadsheetStatus and geometrySpreadsheet (see GeoSpreadsheetWidget). It mirrors its
 * state into the natronGeometrySpreadsheetNode (the script name of the node whose rows are shown),
 * natronGeometrySpreadsheetPinned, natronGeometrySpreadsheetFollow ("viewer" or "selection"),
 * natronGeometrySpreadsheetBusy, natronGeometrySpreadsheetStale and
 * natronGeometrySpreadsheetReadsFinished properties.
 **/
class GeoSpreadsheetPanel
    : public QWidget,
      public PanelWidget {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    explicit GeoSpreadsheetPanel(Gui* gui);

    ~GeoSpreadsheetPanel() override;

    // Cancels the read under way, unpins, and forgets the viewer and node shown, until the next refresh.
    void releaseNodes();

    // Lists node's geometry whichever viewer is active; null follows again.
    void pinNode(const NodePtr& node);

    NodePtr getPinnedNode() const;

    // What the panel lists when not pinned: the active viewer's geometry, or the selected node's.
    void setFollow(NodeSourceBar::FollowEnum follow);

    NodeSourceBar::FollowEnum getFollow() const;

public Q_SLOTS:

    void onActiveViewerChanged();

protected:
    void enterEvent(QEnterEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void keyReleaseEvent(QKeyEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void showEvent(QShowEvent* e) override;

private:
    struct Implementation;
    std::unique_ptr<Implementation> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_GeoSpreadsheetPanel_h
