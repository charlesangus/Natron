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

#ifndef NATRON_ENGINE_UNDOGESTURE_H
#define NATRON_ENGINE_UNDOGESTURE_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>

#include <QtGlobal>

NATRON_NAMESPACE_ENTER

/**
 * @brief One continuous user gesture on a widget or a viewer overlay: a drag from press to
 * release, a held arrow key, a quick run of wheel notches.
 *
 * Commands that fold consecutive edits into one undo step (QUndoCommand::mergeWith()) carry the
 * key of the gesture that pushed them and fold only into a command with the same non-zero key.
 * Everything else, a typed value committed with Return or on focus out, a click, a separate
 * drag, is pushed with key 0 and stays its own undo step, even when it edits the same thing as
 * the step before it. The widget or overlay interact that sees the press and the release owns
 * the gesture, so the boundary is explicit rather than guessed from timing or from what the
 * commands edit. This lives in Engine so that Engine-side commands, such as the roto ones, use
 * the same keys as the Gui widgets.
 **/
class UndoGesture {
public:
    UndoGesture()
        : _key(0)
    {
    }

    /**
     * @brief Starts a new gesture, which also ends the one in progress.
     **/
    void begin()
    {
        _key = newKey();
    }

    void end()
    {
        _key = 0;
    }

    bool isOpen() const
    {
        return _key != 0;
    }

    /**
     * @brief The key the commands pushed during this gesture carry, 0 when none is in progress.
     **/
    quint64 key() const
    {
        return _key;
    }

    /**
     * @brief A key no other gesture of this process has had. One counter serves every widget and
     * interact, so keys from two different gesture owners editing the same thing never collide.
     * Inline functions share one static local across the whole program.
     **/
    static quint64 newKey()
    {
        // 0 means "no gesture", so the first key handed out is 1.
        static std::atomic<quint64> counter(0);

        return ++counter;
    }

    static bool sameGesture(quint64 a,
                            quint64 b)
    {
        return (a != 0) && (a == b);
    }

private:
    quint64 _key;
};

/**
 * @brief The gesture key of a command that merges with commands of its own gesture only. A
 * subclass checks inSameGestureAs() first in its mergeWith().
 **/
class UndoGestureMember {
public:
    UndoGestureMember()
        : _gestureKey(0)
    {
    }

    void setGestureKey(quint64 key)
    {
        _gestureKey = key;
    }

    quint64 getGestureKey() const
    {
        return _gestureKey;
    }

    bool inSameGestureAs(const UndoGestureMember& other) const
    {
        return UndoGesture::sameGesture(_gestureKey, other._gestureKey);
    }

private:
    quint64 _gestureKey;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_UNDOGESTURE_H
