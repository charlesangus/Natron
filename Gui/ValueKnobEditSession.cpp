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
#include "Gui/KnobGuiValue.h"
#include "Gui/KnobUndoCommand.h"

NATRON_NAMESPACE_ENTER

namespace {

/**
 * @brief Holds the fold state of every listed knob's panel row while its dimensions are written
 * one at a time, then refreshes each knob once. A refresh between two of the writes would see
 * the dimensions differ and unfold a knob whose dimensions all end up equal again.
 **/
class ValueKnobRefreshBatch {
public:
    explicit ValueKnobRefreshBatch(const std::vector<KnobIWPtr>& knobs)
        : _knobs(knobs)
        , _held()
    {
        for (std::size_t i = 0; i < _knobs.size(); ++i) {
            KnobGuiPtr gui = KnobGui::getLiveGui(_knobs[i].lock());

            if (KnobGuiValue* value = dynamic_cast<KnobGuiValue*>(gui.get())) {
                value->setFoldStateHeld(true);
                _held.push_back(gui);
            }
        }
    }

    ~ValueKnobRefreshBatch()
    {
        for (std::size_t i = 0; i < _held.size(); ++i) {
            if (KnobGuiValue* value = dynamic_cast<KnobGuiValue*>(_held[i].lock().get())) {
                value->setFoldStateHeld(false);
            }
        }
        for (std::size_t i = 0; i < _knobs.size(); ++i) {
            if (KnobIPtr knob = _knobs[i].lock()) {
                KnobGui::refreshLiveGui(knob, -1);
            }
        }
    }

private:
    ValueKnobRefreshBatch(const ValueKnobRefreshBatch&) = delete;
    ValueKnobRefreshBatch& operator=(const ValueKnobRefreshBatch&) = delete;

    std::vector<KnobIWPtr> _knobs;
    std::vector<KnobGuiWPtr> _held;
};

/**
 * @brief A whole stroke's undo step: one KnobUndoCommand child per edited dimension, undone and
 * redone inside one ValueKnobRefreshBatch over every knob they touch.
 **/
class ValueKnobStrokeCommand
    : public QUndoCommand {
public:
    explicit ValueKnobStrokeCommand(const std::vector<KnobIWPtr>& knobs)
        : QUndoCommand()
        , _knobs(knobs)
    {
    }

    virtual void undo() OVERRIDE FINAL
    {
        ValueKnobRefreshBatch batch(_knobs);

        QUndoCommand::undo();
    }

    virtual void redo() OVERRIDE FINAL
    {
        ValueKnobRefreshBatch batch(_knobs);

        QUndoCommand::redo();
    }

private:
    std::vector<KnobIWPtr> _knobs;
};

void
addKnobOnce(const KnobIPtr& knob,
            std::vector<KnobIWPtr>* knobs,
            std::vector<KnobI*>* seen)
{
    for (std::size_t i = 0; i < seen->size(); ++i) {
        if ((*seen)[i] == knob.get()) {
            return;
        }
    }
    seen->push_back(knob.get());
    knobs->push_back(knob);
}

} // namespace

ValueKnobEditSession::ValueKnobEditSession()
    : _slots()
    , _live()
    , _active(false)
    , _gui()
{
}

ValueKnobEditSession::~ValueKnobEditSession()
{
    abandon();
}

int
ValueKnobEditSession::findSlot(const KnobIPtr& knob,
                               int dimension) const
{
    for (std::size_t i = 0; i < _slots.size(); ++i) {
        if ((_slots[i].dimension == dimension) && (_slots[i].knob.lock() == knob)) {
            return (int)i;
        }
    }

    return -1;
}

bool
ValueKnobEditSession::collectSlots(const std::vector<KnobGuiPtr>& knobGuis,
                                   std::vector<int>* live)
{
    const std::size_t heldBefore = _slots.size();
    std::vector<int> collected;
    bool anyWritable = false;

    for (std::size_t k = 0; k < knobGuis.size(); ++k) {
        const KnobGuiPtr& knobGui = knobGuis[k];
        KnobIPtr knob = knobGui ? knobGui->getKnob() : KnobIPtr();
        KnobIntPtr asInt = std::dynamic_pointer_cast<KnobInt>(knob);
        KnobDoublePtr asDouble = std::dynamic_pointer_cast<KnobDouble>(knob);

        if (!asInt && !asDouble) {
            anyWritable = false;
            break;
        }
        for (int d = 0; d < knob->getDimension(); ++d) {
            int index = findSlot(knob, d);

            if (index < 0) {
                Slot slot;

                slot.gui = knobGui;
                slot.knob = knob;
                slot.dimension = d;
                slot.isInt = bool(asInt);
                slot.writable = isGlobalModeDimensionWritable(knob, d);
                slot.start = asInt ? (double)asInt->getValue(d) : asDouble->getValue(d);
                slot.current = slot.start;
                index = (int)_slots.size();
                _slots.push_back(slot);
            }
            collected.push_back(index);
            anyWritable = anyWritable || _slots[index].writable;
        }
    }
    if (!anyWritable) {
        _slots.resize(heldBefore);

        return false;
    }
    *live = collected;

    return true;
}

bool
ValueKnobEditSession::begin(const std::vector<KnobGuiPtr>& knobGuis,
                            const QString& /*label*/)
{
    abandon();
    _slots.clear();
    _live.clear();
    if (knobGuis.empty() || !collectSlots(knobGuis, &_live)) {
        _slots.clear();
        _live.clear();

        return false;
    }
    _active = true;

    // Held on its own: when a knob's GUI goes first, abandon() must still reach the Gui to turn
    // draft render back off.
    _gui = knobGuis.front()->getGui();
    if (_gui) {
        _gui->setDraftRenderEnabled(true);
    }

    return true;
}

bool
ValueKnobEditSession::switchKnobs(const std::vector<KnobGuiPtr>& knobGuis)
{
    if (!_active) {
        return false;
    }

    return collectSlots(knobGuis, &_live);
}

bool
ValueKnobEditSession::isSlotWritable(int slot) const
{
    return (slot >= 0) && (slot < (int)_live.size()) && _slots[_live[slot]].writable;
}

std::vector<double>
ValueKnobEditSession::getStartValues() const
{
    std::vector<double> values;

    values.reserve(_live.size());
    for (std::size_t i = 0; i < _live.size(); ++i) {
        values.push_back(_slots[_live[i]].start);
    }

    return values;
}

std::vector<double>
ValueKnobEditSession::getCurrentValues() const
{
    std::vector<double> values;

    values.reserve(_live.size());
    for (std::size_t i = 0; i < _live.size(); ++i) {
        values.push_back(_slots[_live[i]].current);
    }

    return values;
}

// Writes the listed slots' current values, one knob at a time inside its own beginChanges, so
// a multi-dimension knob renders once per move. Each knob is locked again at its turn: a write
// to an earlier one can run code that deletes it.
void
ValueKnobEditSession::write(const std::vector<int>& slots)
{
    std::vector<KnobIWPtr> knobs;
    std::vector<KnobI*> seen;

    for (std::size_t i = 0; i < slots.size(); ++i) {
        if (KnobIPtr knob = _slots[slots[i]].knob.lock()) {
            addKnobOnce(knob, &knobs, &seen);
        }
    }

    ValueKnobRefreshBatch batch(knobs);

    for (std::size_t k = 0; k < knobs.size(); ++k) {
        KnobIPtr knob = knobs[k].lock();

        if (!knob) {
            continue;
        }
        knob->beginChanges();
        for (std::size_t j = 0; j < slots.size(); ++j) {
            Slot& slot = _slots[slots[j]];

            if (slot.knob.lock() != knob) {
                continue;
            }

            KeyFrame key;
            int ret;

            if (slot.isInt) {
                ret = KnobGui::setKnobValue<int>(knob, slot.dimension, (int)std::lround(slot.current), &key, false, eValueChangedReasonUserEdited);
            } else {
                ret = KnobGui::setKnobValue<double>(knob, slot.dimension, slot.current, &key, false, eValueChangedReasonUserEdited);
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

    for (std::size_t i = 0; (i < _live.size()) && (i < values.size()); ++i) {
        Slot& slot = _slots[_live[i]];
        const double v = slot.isInt ? (double)std::lround(values[i]) : values[i];

        if (!slot.writable || (v == slot.current)) {
            continue;
        }
        slot.current = v;
        changed.push_back(_live[i]);
    }
    if (changed.empty()) {
        return;
    }
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
    std::vector<KnobIWPtr> knobs;
    std::vector<KnobI*> seen;

    for (std::size_t i = 0; i < _slots.size(); ++i) {
        KnobIPtr knob = _slots[i].knob.lock();

        if (_slots[i].written && knob) {
            addKnobOnce(knob, &knobs, &seen);
        }
    }

    ValueKnobRefreshBatch batch(knobs);

    for (std::size_t i = 0; i < _slots.size(); ++i) {
        Slot& slot = _slots[i];
        KnobIPtr knob = slot.knob.lock();

        if (!slot.written || !knob) {
            continue;
        }
        if (slot.isInt) {
            KnobGui::setKnobValue<int>(knob, slot.dimension, (int)std::lround(slot.start), NULL, false, eValueChangedReasonNatronGuiEdited);
        } else {
            KnobGui::setKnobValue<double>(knob, slot.dimension, slot.start, NULL, false, eValueChangedReasonNatronGuiEdited);
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

    std::vector<KnobIWPtr> knobs;
    std::vector<KnobI*> seen;
    KnobGuiPtr owner;

    for (std::size_t i = 0; i < _slots.size(); ++i) {
        const Slot& slot = _slots[i];
        KnobGuiPtr knobGui = slot.gui.lock();
        KnobIPtr knob = slot.knob.lock();

        if (!slot.writable || (slot.current == slot.start) || !knobGui || !knob) {
            continue;
        }
        addKnobOnce(knob, &knobs, &seen);
        if (!owner) {
            owner = knobGui;
        }
    }
    if (!owner) {
        return;
    }

    ValueKnobStrokeCommand* step = new ValueKnobStrokeCommand(knobs);

    for (std::size_t i = 0; i < _slots.size(); ++i) {
        const Slot& slot = _slots[i];
        KnobGuiPtr knobGui = slot.gui.lock();

        if (!slot.writable || (slot.current == slot.start) || !knobGui || !slot.knob.lock()) {
            continue;
        }
        if (slot.isInt) {
            new KnobUndoCommand<int>(knobGui, (int)std::lround(slot.start), (int)std::lround(slot.current), slot.dimension, true, step);
        } else {
            new KnobUndoCommand<double>(knobGui, slot.start, slot.current, slot.dimension, true, step);
        }
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
