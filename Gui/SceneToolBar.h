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

#ifndef Gui_SceneToolBar_h
#define Gui_SceneToolBar_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <vector>

#include <QPointer>
#include <QWidget>

class QButtonGroup;
class QToolButton;

NATRON_NAMESPACE_ENTER

class SceneViewport;

/**
 * @brief The 3D view's left bar: the transform tools, then the geometry pick modes. The hidden
 * transformToolSlot and snapSlot widgets hold the places of tools still to come.
 **/
class SceneToolBar
    : public QWidget {
public:
    explicit SceneToolBar(QWidget* parent);

    // The view whose tool the buttons pick and show; set once it exists.
    void setViewport(SceneViewport* viewport);

    /**
     * @brief Whether the buttons act: the transform tools while the 3D view is shown, the pick
     * modes while it shows geometry. Buttons that cannot act are disabled, never hidden, so the
     * bar keeps its layout.
     **/
    void setActive(bool shown, bool geometry);

    // Shows the pick mode, a GeoComponentEnum, without calling the pick mode callback.
    void setPickMode(int mode);

    // Called with the pick mode the user chose.
    void setPickModeCallback(const std::function<void(int)>& callback);

private:
    void refreshTools();

    QPointer<SceneViewport> _viewport;
    // In the order of the view's transform tool actions.
    std::vector<QToolButton*> _toolButtons;
    QButtonGroup* _pickModes;
    std::function<void(int)> _pickModeCallback;
    bool _shown = false;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_SceneToolBar_h
