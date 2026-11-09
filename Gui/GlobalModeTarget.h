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

#ifndef Gui_GlobalModeTarget_h
#define Gui_GlobalModeTarget_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <vector>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QString>
#include <QtCore/qnamespace.h>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/EngineFwd.h"
#include "Gui/GuiFwd.h"

NATRON_NAMESPACE_ENTER

class ColorTrackballReveal;
struct ColorTrackballHudState;

/**
 * @brief Undoes or redoes on panel's own undo stack, as every global-mode target does between
 * strokes. Does nothing for a null panel.
 **/
void undoOnGlobalModePanel(DockablePanel* panel, bool redo);

/**
 * @brief What the global mode controller (ColorTrackballGlobalMode) drives while engaged: one
 * knob, or a group of knobs edited together, with its own stroke maths, keys, hint block legend
 * and reveal graphic. The controller owns everything shared by every kind of target: the event
 * filter, the gearing, the edge warp, the wheel notches, the right-click exit, the HUD and
 * reveal windows and the highlight frames.
 *
 * A target holds its knobs weakly. Once any of getKnobGuis() is destroyed the controller drops
 * the target without calling anything on it.
 **/
class GlobalModeTarget {
public:
    virtual ~GlobalModeTarget() { }

    /**
     * @brief The knob the target is on: the one stepping moves from and touching records.
     **/
    virtual KnobGui* getKnobGui() const = 0;

    /**
     * @brief Every knob the target may edit; the mode ends when any of them goes away, and any
     * edit to any of them refreshes the readouts.
     **/
    virtual std::vector<KnobGui*> getKnobGuis() const = 0;

    virtual DockablePanel* getPanel() const = 0;

    /**
     * @brief Shows the target as the one being edited (highlighted, opened, scrolled into view)
     * when the controller moves onto it; detach() takes the highlight away again.
     **/
    virtual void attach() = 0;
    virtual void detach() = 0;

    /**
     * @brief Starts a stroke from the knobs' current values. False, with nothing started, when
     * the target cannot be edited right now.
     **/
    virtual bool beginStroke() = 0;

    /**
     * @brief Adds one pointer move of dx, dy screen pixels (y down) at gearFactor and writes the
     * result.
     **/
    virtual void moveStroke(double dx, double dy, double gearFactor) = 0;

    /**
     * @brief Ends the stroke as one undo step.
     **/
    virtual void endStroke() = 0;

    /**
     * @brief Ends the stroke without leaving an undo step.
     **/
    virtual void abandonStroke() = 0;

    virtual void undoBetweenStrokes(bool redo)
    {
        undoOnGlobalModePanel(getPanel(), redo);
    }

    /**
     * @brief Offers the target a key the controller has no use of its own for. True when a
     * readout of the mode needs refreshing as a result.
     **/
    virtual bool handleKey(int key, Qt::KeyboardModifiers modifiers, bool strokeActive) = 0;

    /**
     * @brief Fills the hint block's title and legend; the controller adds the gear and the end
     * of list message.
     **/
    virtual void fillHudState(ColorTrackballHudState* state) const = 0;

    /**
     * @brief Puts the target's own graphic, pills and puck on the reveal window. The window's
     * placement and its hint block are the controller's.
     **/
    virtual void syncReveal(ColorTrackballReveal* reveal, bool strokeActive) = 0;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_GlobalModeTarget_h
