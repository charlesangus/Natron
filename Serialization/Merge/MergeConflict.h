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

#ifndef Serialization_Merge_MergeConflict_h
#define Serialization_Merge_MergeConflict_h

#include "Global/Macros.h"

#include <string>
#include <string_view>
#include <vector>

NATRON_NAMESPACE_ENTER
namespace Serialization {

enum class MergeConflictKind {
    // Both sides changed one knob dimension differently; the merge kept mine.
    eValue,
    // Both sides connected one input differently; the merge kept mine.
    eInput,
    // Both sides inserted nodes on one base edge; the merge chained theirs, then mine.
    eStructuralInsert,
    // One side deleted a node the other changed. A node the artist deleted is restored from
    // theirs; a node the supervisor deleted is kept from mine without its origin.
    eDeleteModify,
    // Both sides moved one node into different groups; the merge kept mine. Inputs the two sides
    // also connected differently are listed on it, not raised apart, and keep mine too.
    eReparent,
    // A side's node stands for a base node by structure, not by identity.
    eAdopted,
};

enum class MergeSide {
    eMine,
    eTheirs,
};

inline std::string_view
mergeConflictKindName(MergeConflictKind kind)
{
    switch (kind) {
    case MergeConflictKind::eValue:
        return "value";
    case MergeConflictKind::eInput:
        return "input";
    case MergeConflictKind::eStructuralInsert:
        return "structural-insert";
    case MergeConflictKind::eDeleteModify:
        return "delete-modify";
    case MergeConflictKind::eReparent:
        return "reparent";
    case MergeConflictKind::eAdopted:
        return "adopted";
    }

    return "?";
}

/**
 * @brief One place where the merge chose for the user. Every conflict is already resolved in the
 * merged snapshot, so the user only reviews it.
 **/
struct MergeConflict {
    MergeConflictKind kind = MergeConflictKind::eValue;
    // The base node the conflict is about.
    std::string baseNodeId;
    // That node in the merged snapshot, or empty when the merge left it out.
    std::string nodeId;
    std::string knob;
    // -1 for the whole knob.
    int dimension = -1;
    std::string input;
    // eReparent: the node's inputs the two sides connected differently.
    std::vector<std::string> inputs;
    // eDeleteModify: the side that deleted the node. eAdopted: the side whose node was adopted.
    MergeSide side = MergeSide::eMine;
    // eAdopted: the knobs the adopted node leaves at their default while the base node sets them.
    std::vector<std::string> defaultedKnobs;
};

} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_Merge_MergeConflict_h
