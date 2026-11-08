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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "ValueKnobEditSession.h"

#include <cmath>
#include <map>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QUndoCommand>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/AppManager.h"
#include "Engine/Curve.h"
#include "Engine/GlobalModeKind.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Settings.h"
#include "Engine/ViewIdx.h"

#include "Gui/Gui.h"
#include "Gui/KnobGui.h"
#include "Gui/KnobUndoCommand.h"

NATRON_NAMESPACE_ENTER

ValueKnobEditSession::ValueKnobEditSession()
    : _slots()
    , _active(false)
    , _moved(false)
    , _gui()
{
}

ValueKnobEditSession::~ValueKnobEditSession()
{
    abandon();
}

bool
ValueKnobEditSession::begin(const std::vector<KnobGuiPtr>& knobGuis,
                            const QString& /*label*/)
{
    abandon();
    _slots.clear();

    bool anyWritable = false;

    for (std::size_t k = 0; k < knobGuis.size(); ++k) {
        const KnobGuiPtr& knobGui = knobGuis[k];
        KnobIPtr knob = knobGui ? knobGui->getKnob() : KnobIPtr();
        const bool isInt = bool(std::dynamic_pointer_cast<KnobInt>(knob));
        const bool isDouble = bool(std::dynamic_pointer_cast<KnobDouble>(knob));

        if (!isInt && !isDouble) {
            _slots.clear();

            return false;
        }
        for (int d = 0; d < knob->getDimension(); ++d) {
            Slot slot;

            slot.gui = knobGui;
            slot.knob = knob;
            slot.dimension = d;
            slot.isInt = isInt;
            slot.writable = isGlobalModeDimensionWritable(knob, d);
            slot.start = isInt ? (double)std::dynamic_pointer_cast<KnobInt>(knob)->getValue(d) : std::dynamic_pointer_cast<KnobDouble>(knob)->getValue(d);
            slot.current = slot.start;
            anyWritable = anyWritable || slot.writable;
            _slots.push_back(slot);
        }
    }
    if (!anyWritable) {
        _slots.clear();

        return false;
    }
    _active = true;
    _moved = false;

    // Held on its own: when a knob's GUI goes first, abandon() must still reach the Gui to turn
    // draft render back off.
    _gui = knobGuis.front()->getGui();
    if (_gui) {
        _gui->setDraftRenderEnabled(true);
    }

    return true;
}

bool
ValueKnobEditSession::isSlotWritable(int slot) const
{
    return (slot >= 0) && (slot < (int)_slots.size()) && _slots[slot].writable;
}

std::vector<double>
ValueKnobEditSession::getStartValues() const
{
    std::vector<double> values;

    values.reserve(_slots.size());
    for (std::size_t i = 0; i < _slots.size(); ++i) {
        values.push_back(_slots[i].start);
    }

    return values;
}

std::vector<double>
ValueKnobEditSession::getCurrentValues() const
{
    std::vector<double> values;

    values.reserve(_slots.size());
    for (std::size_t i = 0; i < _slots.size(); ++i) {
        values.push_back(_slots[i].current);
    }

    return values;
}

// Writes the listed slots' current values, one knob at a time inside its own beginChanges, so
// a multi-dimension knob renders once per move.
void
ValueKnobEditSession::write(const std::vector<int>& slots)
{
    std::map<KnobI*, std::vector<int>> byKnob;
    std::vector<KnobI*> order;

    for (std::size_t i = 0; i < slots.size(); ++i) {
        KnobIPtr knob = _slots[slots[i]].knob.lock();

        if (!knob) {
            continue;
        }
        if (byKnob.find(knob.get()) == byKnob.end()) {
            order.push_back(knob.get());
        }
        byKnob[knob.get()].push_back(slots[i]);
    }
    for (std::size_t k = 0; k < order.size(); ++k) {
        const std::vector<int>& knobSlots = byKnob[order[k]];
        KnobIPtr knob = _slots[knobSlots.front()].knob.lock();

        knob->beginChanges();
        for (std::size_t j = 0; j < knobSlots.size(); ++j) {
            Slot& slot = _slots[knobSlots[j]];
            KeyFrame key;
            int ret;

            if (slot.isInt) {
                ret = KnobGui::setKnobValue<int>(knob, slot.dimension, (int)std::lround(slot.current), &key, true, eValueChangedReasonUserEdited);
            } else {
                ret = KnobGui::setKnobValue<double>(knob, slot.dimension, slot.current, &key, true, eValueChangedReasonUserEdited);
            }
            // Only the first write can add the key: every later one at the same frame modifies it.
            if (!slot.written && (ret == KnobHelper::eValueChangedReturnCodeKeyframeAdded)) {
                slot.keyAdded = true;
                slot.keyTime = key.getTime();
            }
            slot.written = true;
        }
        knob->endChanges();
    }
}

void
ValueKnobEditSession::move(const std::vector<double>& values)
{
    if (!_active) {
        return;
    }

    std::vector<int> changed;

    for (std::size_t i = 0; (i < _slots.size()) && (i < values.size()); ++i) {
        Slot& slot = _slots[i];
        const double v = slot.isInt ? (double)std::lround(values[i]) : values[i];

        if (!slot.writable || (v == slot.current)) {
            continue;
        }
        slot.current = v;
        changed.push_back((int)i);
    }
    if (changed.empty()) {
        return;
    }
    _moved = true;
    if (appPTR->getCurrentSettings()->getRenderOnEditingFinishedOnly()) {
        return;
    }
    write(changed);
}

// Puts every written slot back to its start and takes away any key the gesture added, the way
// undoing the gesture's own commands would.
void
ValueKnobEditSession::restore()
{
    for (std::size_t i = 0; i < _slots.size(); ++i) {
        Slot& slot = _slots[i];
        KnobIPtr knob = slot.knob.lock();

        if (!slot.written || !knob) {
            continue;
        }
        if (slot.isInt) {
            KnobGui::setKnobValue<int>(knob, slot.dimension, (int)std::lround(slot.start), NULL, true, eValueChangedReasonNatronGuiEdited);
        } else {
            KnobGui::setKnobValue<double>(knob, slot.dimension, slot.start, NULL, true, eValueChangedReasonNatronGuiEdited);
        }
        if (slot.keyAdded) {
            KnobGui::removeKnobKeyframe(knob, slot.keyTime, slot.dimension, ViewIdx(0));
        }
        slot.written = false;
        slot.keyAdded = false;
    }
}

void
ValueKnobEditSession::finish()
{
    _active = false;

    Gui* gui = _gui.data();

    _gui.clear();
    if (gui) {
        gui->setDraftRenderEnabled(false);
    }
}

void
ValueKnobEditSession::end(const QString& label)
{
    if (!_active) {
        return;
    }
    // Draft goes off before the final push so that the committed value renders at full quality.
    finish();
    restore();

    KnobGuiPtr owner;
    QUndoCommand* step = new QUndoCommand();
    int children = 0;

    for (std::size_t i = 0; i < _slots.size(); ++i) {
        const Slot& slot = _slots[i];
        KnobGuiPtr knobGui = slot.gui.lock();

        if (!slot.writable || (slot.current == slot.start) || !knobGui) {
            continue;
        }
        if (slot.isInt) {
            new KnobUndoCommand<int>(knobGui, (int)std::lround(slot.start), (int)std::lround(slot.current), slot.dimension, true, step);
        } else {
            new KnobUndoCommand<double>(knobGui, slot.start, slot.current, slot.dimension, true, step);
        }
        if (!owner) {
            owner = knobGui;
        }
        ++children;
    }
    if (!owner || (children == 0)) {
        delete step;

        return;
    }

    KnobIPtr knob = owner->getKnob();
    QString text = label;

    if (knob) {
        text = QString::fromUtf8(knob->getLabel().c_str()) + QString::fromUtf8(": ") + label;
    }
    step->setText(text);
    // Pushing runs the step's redo, which writes the final values from the start state
    // restore() left, so the step's own undo knows exactly which keys it added.
    owner->pushUndoCommand(step);
}

void
ValueKnobEditSession::abandon()
{
    if (!_active) {
        return;
    }
    finish();
    restore();
}

NATRON_NAMESPACE_EXIT
