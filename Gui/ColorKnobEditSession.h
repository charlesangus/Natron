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

#ifndef NATRON_GUI_COLORKNOBEDITSESSION_H
#define NATRON_GUI_COLORKNOBEDITSESSION_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QString>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/ColorWheelMath.h"
#include "Engine/EngineFwd.h"

#include "Gui/GuiFwd.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Turns one colour gesture (a wheel or slider drag in the adjustment panel, a swatch
 * drag, a global-mode stroke) into knob changes: begin records the knob's values and turns
 * draft render on, move sets the gesture's current value on every dimension through the undo
 * stack, and end turns draft render off and makes sure the final value has been committed.
 *
 * Every push carries the values recorded at begin as its old values, so the pushes of one
 * gesture compress into a single step that undoes back to where the gesture started. When the
 * "render on editing finished only" preference is set, moves only record the value and end
 * pushes it once.
 **/
class ColorKnobEditSession {
public:
    explicit ColorKnobEditSession(const KnobGuiPtr& knobGui);
    ~ColorKnobEditSession();

    ColorKnobEditSession(const ColorKnobEditSession&) = delete;
    ColorKnobEditSession& operator=(const ColorKnobEditSession&) = delete;

    /**
     * @brief The knob's current value at the current time. A 1-dim knob reads as a grey
     * (r = g = b), and only a 4-dim knob has alpha.
     **/
    static KnobColorValue readKnobValue(const KnobColorPtr& knob);

    /**
     * @brief The luma-chroma basis of the project's working space, or of ACEScg when there is
     * no colour management to read it from.
     **/
    static LumaBasis basisFor(const ProjectColorManagementPtr& colorManagement);

    bool isActive() const;

    /**
     * @brief The label of the gesture's latest step, for naming its undo step.
     **/
    const QString& getLabel() const;

    void begin(const QString& label);
    void move(const KnobColorValue& value, const QString& label);
    void end(const KnobColorValue& value, const QString& label);

    /**
     * @brief Ends a running gesture with the last value it moved to, for when its source goes
     * away before it reports its end.
     **/
    void abandon();

private:
    KnobColorPtr getKnob() const;
    std::list<double> toKnobValues(const KnobColorValue& value) const;
    void push(const std::list<double>& values);

    /**
     * @brief The undo text for the gesture's latest step: "<knob label>: <axis>".
     **/
    QString undoText() const;

    KnobGuiWPtr _knobGui;
    int _nDims;
    std::list<double> _start;
    std::list<double> _current;
    std::list<double> _committed;
    bool _active;
    bool _moved;
    QString _label;
    quint64 _mergeKey;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_COLORKNOBEDITSESSION_H
