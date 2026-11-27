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

#ifndef Gui_SceneTopBar_h
#define Gui_SceneTopBar_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <QPointer>
#include <QWidget>

class QHBoxLayout;
class QToolButton;

NATRON_NAMESPACE_ENTER

class Button;
class Label;
class SceneViewport;
class SpinBox;

/**
 * @brief The 3D view's top bar: the renderer, look-through, view and clipping menus, the
 * progressive render controls, and the tool options of the scene node the viewer targets.
 **/
class SceneTopBar
    : public QWidget {
public:
    explicit SceneTopBar(QWidget* parent);

    // The view whose menus the buttons open; set once it exists.
    void setViewport(SceneViewport* viewport);

    /**
     * @brief Whether the menus act, and whether the renderer refines progressively. The
     * progressive controls hide under a renderer that does not; the bar's height stays, so the 3D
     * view never resizes, which would rescale its fitted framing.
     **/
    void setActive(bool shown, bool progressive);

    SpinBox* getMaxSamplesBox() const;
    Button* getRefinementPausedButton() const;
    Button* getRenderEverythingButton() const;
    Label* getRefineSamplesLabel() const;

    // Adds the viewer controls of a node to the tool options; the area takes no room while empty.
    void addToolOptions(QWidget* container);

    // Whether container is in the tool options.
    bool hasToolOptions(QWidget* container) const;

    void removeToolOptions(QWidget* container);

private:
    void refreshToolOptions();
    // Shows on the View button whether the view is in perspective or orthographic.
    void refreshView();

    QPointer<SceneViewport> _viewport;
    QToolButton* _menus[4];
    Label* _maxSamplesLabel;
    SpinBox* _maxSamples;
    Button* _refinementPaused;
    Button* _renderEverything;
    Label* _refineSamples;
    QWidget* _toolOptions;
    QHBoxLayout* _toolOptionsLayout;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_SceneTopBar_h
