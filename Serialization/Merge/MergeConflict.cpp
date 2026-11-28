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

#include "Serialization/Merge/MergeConflict.h"

#include <algorithm>

#include "Serialization/Merge/MergeRules.h"
#include "Serialization/Ops/SnapshotDiff.h"
#include "Serialization/ProjectSerialization.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

namespace {

    template <typename T, typename Same>
    bool
    sameOptional(const std::optional<T>& a,
                 const std::optional<T>& b,
                 Same same)
    {
        if (a.has_value() != b.has_value()) {
            return false;
        }

        return !a || same(*a, *b);
    }

    // Recorded nodes compare by editable state, as the graph would show them.
    bool
    sameNode(const NodeSerializationPtr& a,
             const NodeSerializationPtr& b)
    {
        if (!a || !b) {
            return !a && !b;
        }
        if (a->id != b->id) {
            return false;
        }
        ProjectSerialization left;
        left.nodes.nodes.push_back(a);
        ProjectSerialization right;
        right.nodes.nodes.push_back(b);

        return diffSnapshots(editableState(left), editableState(right)).empty();
    }

    // A user knob's label, tooltip, layout and flags are its definition, a plug-in knob's come
    // from the plug-in.
    KnobSerialization
    knobState(const KnobSerialization& knob)
    {
        if (knob.isUserKnob) {
            return knob;
        }
        KnobSerialization state = knob;
        const KnobSerialization blank;
        state.label = blank.label;
        state.tooltip = blank.tooltip;
        state.triggerNewLine = blank.triggerNewLine;
        state.evaluatesOnChange = blank.evaluatesOnChange;
        state.animationEnabled = blank.animationEnabled;
        state.isPersistent = blank.isPersistent;

        return state;
    }

    bool
    sameKnobs(const std::list<KnobSerialization>& a,
              const std::list<KnobSerialization>& b)
    {
        return std::equal(a.begin(), a.end(), b.begin(), b.end(), sameKnobState);
    }

} // namespace

bool
sameKnobState(const KnobSerialization& a,
              const KnobSerialization& b)
{
    return knobState(a) == knobState(b);
}

bool
operator==(const MergeConflictValue& a,
           const MergeConflictValue& b)
{
    return sameOptional(a.knob, b.knob, sameKnobState) && (a.key == b.key) && (a.otherKeys == b.otherKeys) && (a.connections == b.connections) && (a.removedNodes == b.removedNodes) && (a.parent == b.parent) && sameOptional(a.knobs, b.knobs, sameKnobs)
        && sameNode(a.node, b.node) && sameOptional(a.roto, b.roto, MergeDetail::sameRotoContexts) && sameOptional(a.tracker, b.tracker, MergeDetail::sameTrackerContexts)
        && (a.itemsBefore == b.itemsBefore);
}

} // namespace Serialization
NATRON_NAMESPACE_EXIT
