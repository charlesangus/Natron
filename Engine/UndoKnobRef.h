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

#ifndef NATRON_ENGINE_UNDOKNOBREF_H
#define NATRON_ENGINE_UNDOKNOBREF_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <string>

#include <QPointer>

#include "Engine/EngineFwd.h"
#include "Engine/Knob.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The knob an undo command addresses, or null once the knob has been destroyed or
 * deleted from its holder. Commands hold knobs rather than knob GUIs because rebuilding a
 * panel replaces every KnobGui while the knobs stay.
 **/
inline KnobIPtr
lockUndoKnob(const KnobIWPtr& knob)
{
    KnobIPtr k = knob.lock();
    KnobHelper* helper = dynamic_cast<KnobHelper*>(k.get());

    if (!k || (helper && !helper->getSignalSlotHandler())) {
        return KnobIPtr();
    }

    return k;
}

/**
 * @brief How an undo command addresses a knob: the knob while it lives, otherwise the live knob
 * of the same name, type and dimension count that its holder now has. Applying or undoing a
 * preset deletes and recreates the node's user knobs, so a knob pointer alone would make every
 * earlier edit of such a knob obsolete. Reattaching the deleted knob objects instead is not an
 * option: deletion drops their expressions, links and Python attribute.
 **/
class UndoKnobRef {
public:
    UndoKnobRef()
        : _knob()
        , _holder()
        , _holderKey(0)
        , _name()
        , _typeName()
        , _dimension(0)
    {
    }

    explicit UndoKnobRef(const KnobIPtr& knob)
        : _knob(knob)
        , _holder()
        , _holderKey(0)
        , _name()
        , _typeName()
        , _dimension(0)
    {
        if (knob) {
            _holder = knob->getHolder();
            _holderKey = knob->getHolder();
            _name = knob->getName();
            _typeName = knob->typeName();
            _dimension = knob->getDimension();
        }
    }

    /**
     * @brief The live knob, or null when neither the knob nor a replacement exists.
     **/
    KnobIPtr lock() const
    {
        KnobIPtr knob = lockUndoKnob(_knob);

        if (knob) {
            return knob;
        }
        KnobHolder* holder = _holder.data();
        if (!holder || _name.empty()) {
            return KnobIPtr();
        }
        knob = lockUndoKnob(holder->getKnobByName(_name));
        if (!knob || (knob->typeName() != _typeName) || (knob->getDimension() != _dimension)) {
            return KnobIPtr();
        }
        _knob = knob;

        return knob;
    }

    /**
     * @brief Orders by holder and name, which stay the same when the knob is recreated. The
     * holder is compared by the address it had, so the order survives the holder's death.
     **/
    bool operator<(const UndoKnobRef& other) const
    {
        if (_holderKey != other._holderKey) {
            return std::less<const KnobHolder*>()(_holderKey, other._holderKey);
        }
        if ((_name != other._name) || !_name.empty()) {
            return _name < other._name;
        }

        // Unnamed knobs never re-resolve, so their pointer is a stable key.
        return _knob.owner_before(other._knob);
    }

private:
    mutable KnobIWPtr _knob;
    QPointer<KnobHolder> _holder;
    const KnobHolder* _holderKey;
    std::string _name;
    std::string _typeName;
    int _dimension;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_UNDOKNOBREF_H
