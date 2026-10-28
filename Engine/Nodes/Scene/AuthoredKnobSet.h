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

#ifndef Engine_Nodes_Scene_AuthoredKnobSet_h
#define Engine_Nodes_Scene_AuthoredKnobSet_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <functional>
#include <mutex>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Global/Enums.h"

// Free of USD, like the node headers that include it.

NATRON_NAMESPACE_ENTER

/**
 * @brief Value knobs that write an opinion only once the user touched them, as Solaris' per-parameter
 * enable toggles do: a scene node that edits prims authors the knobs the user set and leaves every
 * other attribute to its input scene, rather than stamping all its defaults over it.
 *
 * Each value knob is paired with an "authored" KnobBool companion, off by default, shown as an
 * unlabelled toggle in front of it on the same row and serialized like any knob. The node forwards
 * its knobChanged() here: an edit of a value knob by the user, the GUI or a script turns its
 * companion on; restoring the value knob to its default, when no dimension then differs from
 * its default, turns it off. The node's own setValueFromPlugin() edits, time changes and link
 * refreshes leave it alone. Setting an expression on a value knob, which does not go through
 * knobChanged(), turns its companion on too; clearing the expression leaves the companion as is.
 * While a project or a saved node restores the knobs (Node::isRestoringKnobs()) the companions
 * keep their saved state.
 *
 * Authoring is the node's: forEachAuthored() visits the value knobs whose companion is on, and an
 * animated one authors time samples (SceneNodeAuthoring::authorValue() with isAnimated()).
 **/
class AuthoredKnobSet {
public:
    AuthoredKnobSet() = default;

    AuthoredKnobSet(const AuthoredKnobSet&) = delete;
    AuthoredKnobSet& operator=(const AuthoredKnobSet&) = delete;

    /**
     * @brief Adds knob, created, named and set up by the caller but not yet on a page, to page
     * after a companion named <knob's script name>Authored, and to the set. Returns the companion.
     **/
    KnobBoolPtr add(KnobHolder* holder,
                    const KnobPagePtr& page,
                    const KnobIPtr& knob);

    /**
     * @brief For the node's knobChanged(): updates k's companion when k is one of the set's value
     * knobs, and returns whether it is.
     **/
    bool onKnobChanged(KnobI* k,
                       ValueChangedReasonEnum reason);

    // Calls fn on each value knob whose companion is on, in the order they were added.
    void forEachAuthored(const std::function<void(const KnobIPtr&)>& fn) const;

    // The companion of knob, or null when knob is not in the set.
    KnobBoolPtr getCompanion(const KnobI* knob) const;

    std::size_t size() const;

    /**
     * @brief Deletes every value knob and companion of the set from holder, without their GUI
     * (KnobHolder::deleteKnob()), and empties the set. Main thread only.
     **/
    void deleteAll(KnobHolder* holder);

private:
    struct Pair {
        KnobIWPtr value;
        KnobBoolWPtr authored;
    };

    mutable std::mutex _lock;
    std::vector<Pair> _pairs;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_AuthoredKnobSet_h
