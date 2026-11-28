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

#include <list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Serialization/CurveSerialization.h"
#include "Serialization/KnobSerialization.h"
#include "Serialization/NodeSerialization.h"
#include "Serialization/RotoSerialization.h"
#include "Serialization/TrackerSerialization.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

enum class MergeConflictKind {
    // Both sides changed one knob dimension differently; the merge kept mine.
    eValue,
    // Both sides connected one input differently; the merge kept mine. Also an input of a node
    // only mine has whose source theirs deleted without one clear replacement in the node's group,
    // and any input whose source the merge put in another group than the node's; the merge left
    // it unconnected.
    eInput,
    // Both sides inserted nodes on one base edge; the merge chained theirs, then mine.
    eStructuralInsert,
    // One side deleted a node, or a user knob, roto item or track, the other changed. A node the
    // artist deleted is restored from theirs; a node the supervisor deleted is kept from mine
    // without its origin. A group counts as changed when a node was added or changed inside it,
    // and comes back with what is inside it. A knob, item or track is kept as the side that changed
    // it has it.
    eDeleteModify,
    // Both sides moved one node into different groups; the merge kept mine. Inputs the two sides
    // also connected differently are listed on it, not raised apart, and keep mine too, as are the
    // inputs left unconnected because their source is in another group.
    eReparent,
    // A side's node stands for a base node by structure, not by identity.
    eAdopted,
    // Both sides changed one key of an animation curve differently, or added different keys at
    // one time; the merge kept mine's key, or no key when mine has none there.
    eCurveKey,
    // Both sides changed one roto shape's points differently, or the two sides hold different
    // kinds of item under one name; the merge kept mine's. An empty item stands for the node's
    // spline-warp data.
    eRotoShape,
    // A node restored from a compared version used to feed one of the reader's inputs, which the
    // artist has since wired elsewhere; the restore left it. Mine keeps that wiring, theirs
    // connects the input to the restored node.
    eSpliceChoice,
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
    case MergeConflictKind::eCurveKey:
        return "curve-key";
    case MergeConflictKind::eRotoShape:
        return "roto-shape";
    case MergeConflictKind::eSpliceChoice:
        return "splice-choice";
    }

    return "?";
}

inline std::optional<MergeConflictKind>
mergeConflictKindFromName(std::string_view name)
{
    for (MergeConflictKind kind : { MergeConflictKind::eValue, MergeConflictKind::eInput, MergeConflictKind::eStructuralInsert, MergeConflictKind::eDeleteModify, MergeConflictKind::eReparent, MergeConflictKind::eAdopted, MergeConflictKind::eCurveKey, MergeConflictKind::eRotoShape, MergeConflictKind::eSpliceChoice }) {
        if (mergeConflictKindName(kind) == name) {
            return kind;
        }
    }

    return std::nullopt;
}

/**
 * @brief One input connection of the merged snapshot, by merged node IDs.
 **/
struct MergeConflictConnection {
    std::string nodeId;
    std::string input;
    // Empty for an unconnected input.
    std::string sourceId;

    bool operator==(const MergeConflictConnection&) const = default;
};

/**
 * @brief What one side holds where a conflict is, in the merged snapshot's node IDs: the state
 * that choosing the side puts back. Each kind fills the fields that it names.
 **/
struct MergeConflictValue {
    // eValue, and eDeleteModify of a user knob: the knob, absent when the side leaves it at its
    // default or has no such knob.
    std::optional<KnobSerialization> knob;
    // eCurveKey: the key at the conflict's time, absent when the side has no key there.
    std::optional<KeyFrameSerialization> key;
    // eCurveKey: keys at other times that the choice adds beside key.
    std::vector<KeyFrameSerialization> otherKeys;
    // eInput, eStructuralInsert, eReparent and eSpliceChoice: the connections that give the graph this side's
    // wiring. Empty when it cannot be put back, as when the side's source is not in the merged
    // snapshot.
    std::vector<MergeConflictConnection> connections;
    // eStructuralInsert: the nodes inserted on the link that this side does not have. Choosing
    // the side deletes them, except those that something outside them still reads from.
    std::vector<std::string> removedNodes;
    // eReparent: the group holding the node, empty for the top level; unset when that group is
    // not in the merged snapshot.
    std::optional<std::string> parent;
    // eDeleteModify of a node: the base's knobs. eAdopted: each side's state of the defaulted
    // knobs. A knob left out is at its default; unset when the side has nothing to put back.
    std::optional<std::list<KnobSerialization>> knobs;
    // eDeleteModify of a node, on the base side: the base node by itself, under its merged ID and
    // script name, without its inputs or children. Its knobs are in knobs, and its inputs and group
    // in connections and parent. Null when the side has nothing to put back.
    NodeSerializationPtr node;
    // eRotoShape and eDeleteModify of a roto item: the item alone in the base layer, with its
    // parentLayerName naming the layer it is in; for an empty item name, the spline-warp data.
    // Unset when the side has no such item.
    std::optional<RotoContextSerialization> roto;
    // eDeleteModify of a track: the track alone. Unset when the side has no such track.
    std::optional<TrackerContextSerialization> tracker;
    // eDeleteModify of a roto item or track: the items of its layer, or the tracks, that come
    // before it on this side, nearest last, so that it goes back where it was.
    std::vector<std::string> itemsBefore;
};

// Roto and tracker data compare by state, as the merge compares them.
bool operator==(const MergeConflictValue& a, const MergeConflictValue& b);

/**
 * @brief An extra choice a handler offers for a conflict beside Mine, Theirs and Base, such as
 * keeping both keys of a curve-key conflict. Choosing it puts its state back as choosing a side
 * puts back that side's.
 **/
struct MergeConflictOption {
    // Stable across saves, e.g. "keep-both-keys".
    std::string name;
    std::string label;
    MergeConflictValue state;

    bool operator==(const MergeConflictOption&) const = default;
};

inline constexpr std::string_view kMergeOptionKeepBothKeys = "keep-both-keys";

// The dimension of a conflict on a knob every side saved folded, as one dimension standing for
// all of them: its side's single dimension goes to every dimension.
inline constexpr int kMergeConflictAllDimensions = -2;

/**
 * @brief One place where the merge chose for the user. Every conflict is already resolved in the
 * merged snapshot, so the user only reviews it.
 **/
struct MergeConflict {
    MergeConflictKind kind = MergeConflictKind::eValue;
    // The template import the merge updated.
    std::string templateKey;
    // The base node the conflict is about, or empty for a node only mine has.
    std::string baseNodeId;
    // That node in the merged snapshot, or empty when the merge left it out.
    std::string nodeId;
    std::string knob;
    // -1 for the whole knob, kMergeConflictAllDimensions for every dimension of a folded knob. For
    // a parametric knob's curve-key conflict, the curve's index.
    int dimension = -1;
    // eCurveKey: the key's time.
    std::optional<double> time;
    // The roto item or track the conflict is about, by script name, or empty for the node itself.
    std::string item;
    std::string input;
    // eReparent: the node's inputs the two sides connected differently.
    std::vector<std::string> inputs;
    // eDeleteModify: the side that deleted the node or knob. eAdopted: the side whose node was adopted.
    MergeSide side = MergeSide::eMine;
    // eAdopted: the knobs the adopted node leaves at their default while the base node sets them.
    std::vector<std::string> defaultedKnobs;
    MergeConflictValue base;
    MergeConflictValue mine;
    MergeConflictValue theirs;
    std::vector<MergeConflictOption> options;

    bool operator==(const MergeConflict&) const = default;
};

} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_Merge_MergeConflict_h
