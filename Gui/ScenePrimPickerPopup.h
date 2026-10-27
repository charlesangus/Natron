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

#ifndef Gui_ScenePrimPickerPopup_h
#define Gui_ScenePrimPickerPopup_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>

#include <QDialog>
#include <QtCore/QStringList>

#include "Engine/EngineFwd.h"
#include "Gui/GuiFwd.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Picks prims from a node's input scene for a prim path or pattern knob: a tree over the
 * scene, single-select for a path and multi-select for a pattern, or the 3D view's selection.
 * Non-modal; primsPicked() fires once when the user confirms.
 **/
class ScenePrimPickerPopup
    : public QDialog {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    /**
     * @param sceneNode the node whose scene is listed, or null for none.
     * @param current the knob's paths, selected in the tree once the scene is read.
     **/
    ScenePrimPickerPopup(Gui* gui,
                         const NodePtr& sceneNode,
                         bool multiple,
                         const QStringList& current,
                         double time,
                         QWidget* parent);

    ~ScenePrimPickerPopup() override;

    // The paths selected in the tree, in the order they were selected.
    QStringList getTreeSelection() const;

Q_SIGNALS:

    void primsPicked(const QStringList& paths);

private Q_SLOTS:

    void onAccepted();
    void onUseViewerSelection();
    void updateButtons();

private:
    void onSynced(bool ok, const QString& error);

    struct Implementation;
    std::unique_ptr<Implementation> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_ScenePrimPickerPopup_h
