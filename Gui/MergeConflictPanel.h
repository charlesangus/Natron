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

#ifndef Gui_MergeConflictPanel_h
#define Gui_MergeConflictPanel_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <memory>
#include <vector>

#include <QWidget>

#include "Engine/EngineFwd.h"
#include "Engine/ProjectMerge.h"
#include "Gui/GuiFwd.h"
#include "Gui/PanelWidget.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Lists the project's unresolved merge conflicts, grouped by node, and resolves them:
 * selecting a row frames its node and opens its settings panel; Mine, Theirs and Base put that
 * side's state back, Accept keeps what the merge chose. Selecting a node's row acts on all of its
 * conflicts. It also keeps the conflict badges of the node graph up to date.
 **/
class MergeConflictPanel
    : public QWidget,
      public PanelWidget {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    explicit MergeConflictPanel(Gui* gui);

    ~MergeConflictPanel() override;

    // The selected conflicts, as indices into the project's list, in list order.
    std::vector<std::size_t> getSelectedConflicts() const;

    void selectConflicts(const std::vector<std::size_t>& indices);

    // Resolves those of the conflicts at indices that can take a side, reporting a failure in a
    // dialog; acceptConflicts() keeps what the merge chose for all of them.
    void resolveConflicts(const std::vector<std::size_t>& indices, ProjectMerge::ConflictSide side);
    void acceptConflicts(const std::vector<std::size_t>& indices);

public Q_SLOTS:

    void refresh();

private Q_SLOTS:

    void onProjectLoaded();
    void onSelectionChanged();
    void onCurrentItemChanged();
    void onMineClicked();
    void onTheirsClicked();
    void onBaseClicked();
    void onAcceptClicked();
    void onAllMineClicked();
    void onAllTheirsClicked();

protected:
    void enterEvent(QEnterEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void keyReleaseEvent(QKeyEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;

private:
    void resolveAll(ProjectMerge::ConflictSide side);

    struct Implementation;
    std::unique_ptr<Implementation> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_MergeConflictPanel_h
