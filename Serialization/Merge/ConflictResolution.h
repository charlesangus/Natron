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

#ifndef Serialization_Merge_ConflictResolution_h
#define Serialization_Merge_ConflictResolution_h

#include "Global/Macros.h"

#include <string>

#include "Serialization/KnobSerialization.h"
#include "Serialization/Merge/MergeConflict.h"
#include "Serialization/ProjectSerialization.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

enum class ResolutionSide {
    eMine,
    eTheirs,
    eBase,
};

const MergeConflictValue& sideState(const MergeConflict& conflict, ResolutionSide side);

/**
 * @brief Gives knob, the state of the conflicted knob, value's conflicted dimension, extra state
 * or curve key, and value's other keys at their own times. Returns false with error set when the
 * knob has no such dimension or curve.
 **/
bool applyConflictKnobValue(const MergeConflict& conflict,
                            const MergeConflictValue& value,
                            KnobSerialization* knob,
                            std::string* error);

/**
 * @brief Edits snapshot, the project the conflict was raised on (merged IDs), into the graph the
 * side's recorded state stands for, leaving everything else as it is:
 * - value and curve key: the knob of the node, of its roto item or track, or of the project;
 * - input and inserted chain: the recorded connections, whose sources must be beside their node;
 * - group: the node moves into the side's group with its ID and takes the side's inputs whose
 *   sources are there; the nodes reading from it in the group it left are disconnected;
 * - adopted node: the knobs it left at their default take the side's state;
 * - roto shape, roto item or track: replaced by the side's, removed when the side has none, put
 *   at the end of the node's top layer or track list when the node lost it; the spline warp
 *   for a roto shape conflict without an item;
 * - delete-modify, whose side deleted: that side deletes the node (disconnecting what read from
 *   it), or the user knob from the node's pages; the other side leaves the graph as it is; the
 *   base gives the node its base knobs, or the user knob its base state.
 * Returns false with error set, snapshot possibly part edited, when the side has no state to put
 * back or it no longer fits the snapshot.
 **/
bool applyConflictSide(const MergeConflict& conflict,
                       ResolutionSide side,
                       ProjectSerialization* snapshot,
                       std::string* error);

/**
 * @brief applyConflictSide() for the state of conflict's handler option named option.
 **/
bool applyConflictOption(const MergeConflict& conflict,
                         const std::string& option,
                         ProjectSerialization* snapshot,
                         std::string* error);

const MergeConflictOption* conflictOption(const MergeConflict& conflict, const std::string& name);

} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_Merge_ConflictResolution_h
