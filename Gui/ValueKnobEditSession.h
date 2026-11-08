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

#ifndef Gui_ValueKnobEditSession_h
#define Gui_ValueKnobEditSession_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <vector>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QPointer>
#include <QString>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/EngineFwd.h"
#include "Gui/GuiFwd.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief One gesture's edit of every dimension of one or two numeric knobs (KnobDouble or
 * KnobInt), such as a global-mode stroke on skewX and skewY together. Moves write straight
 * through the knobs so the viewer follows the drag; end() puts the knobs back to where the
 * gesture began and pushes a single undo command whose redo writes the final values, so one
 * undo restores every knob and removes any key the gesture added at the current frame.
 *
 * Dimensions driven by an expression or a link are never written.
 **/
class ValueKnobEditSession {
public:
    ValueKnobEditSession();

    /**
     * @brief Abandons a running gesture.
     **/
    ~ValueKnobEditSession();

    /**
     * @brief Starts a gesture over every dimension of knobGuis, in order: slot i is dimension i
     * of the first knob, then the second knob's dimensions follow. False, with nothing started,
     * when no slot is writable or a knob is not numeric.
     **/
    bool begin(const std::vector<KnobGuiPtr>& knobGuis, const QString& label);

    /**
     * @brief Points the running gesture at knobGuis instead, laid out as begin() lays them out,
     * keeping every edit made so far in the same undo step. A dimension the gesture already
     * holds keeps its start value and carries on from its latest one. False, with the slots
     * unchanged, when no new slot is writable or a knob is not numeric; the gesture runs on.
     **/
    bool switchKnobs(const std::vector<KnobGuiPtr>& knobGuis);

    bool isActive() const
    {
        return _active;
    }

    int getSlotCount() const
    {
        return (int)_live.size();
    }

    bool isSlotWritable(int slot) const;

    /**
     * @brief Every slot's value when the gesture first held it.
     **/
    std::vector<double> getStartValues() const;

    /**
     * @brief The gesture's latest values, or the start values before any move.
     **/
    std::vector<double> getCurrentValues() const;

    /**
     * @brief Writes values (one per slot) to the writable slots, with the begin-time values as
     * the gesture's old values. Held back until end() while the "render on editing finished
     * only" preference is on.
     **/
    void move(const std::vector<double>& values);

    /**
     * @brief Ends the gesture as one undo step labelled label, or none when nothing changed.
     **/
    void end(const QString& label);

    /**
     * @brief Ends the gesture with every knob back at its start and no undo step.
     **/
    void abandon();

private:
    struct Slot {
        KnobGuiWPtr gui;
        KnobIWPtr knob;
        int dimension = 0;
        bool isInt = false;
        bool writable = false;
        double start = 0.;
        double current = 0.;
        bool written = false;
        bool keyAdded = false;
        double keyTime = 0.;
    };

    int findSlot(const KnobIPtr& knob, int dimension) const;
    bool collectSlots(const std::vector<KnobGuiPtr>& knobGuis, std::vector<int>* live);
    void write(const std::vector<int>& slots);
    void restore();
    void finish();

    // Every dimension the gesture has held, across switchKnobs(); end() and abandon() cover them all.
    std::vector<Slot> _slots;
    // The indices into _slots of the knobs the gesture is on now, in slot order.
    std::vector<int> _live;
    bool _active;
    QPointer<Gui> _gui;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_ValueKnobEditSession_h
