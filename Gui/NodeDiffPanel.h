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

#ifndef Gui_NodeDiffPanel_h
#define Gui_NodeDiffPanel_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>

#include <QStringList>
#include <QWidget>

#include "Engine/EngineFwd.h"
#include "Engine/ProjectMerge.h"
#include "Gui/GuiFwd.h"
#include "Gui/PanelWidget.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief While the Gui holds a graph comparison, lists what differs on the nodes selected in the
 * last node graph whose selection changed: each changed knob as base → current, the key counts
 * and key times of curves, and the sources of inputs.
 **/
class NodeDiffPanel
    : public QWidget,
      public PanelWidget {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    explicit NodeDiffPanel(Gui* gui);

    ~NodeDiffPanel() override;

    void scheduleRefresh(NodeGraph* graph);

    QStringList getRows() const;

    static QStringList describe(const ProjectMerge::NodeChange& change);

public Q_SLOTS:
    void refresh();

private Q_SLOTS:
    void onComparisonChanged();
    void onClearClicked();
    void onProjectLoaded();

protected:
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

#endif // Gui_NodeDiffPanel_h
