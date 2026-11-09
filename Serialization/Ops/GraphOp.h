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

#ifndef Serialization_Ops_GraphOp_h
#define Serialization_Ops_GraphOp_h

#include "Global/Macros.h"

#include <list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "Serialization/BasicSerialization.h"
#include "Serialization/KnobSerialization.h"
#include "Serialization/NodeSerialization.h"
#include "Serialization/ProjectSerialization.h"
#include "Serialization/RotoSerialization.h"
#include "Serialization/TrackerSerialization.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

/**
 * @brief Creates node, with its ID, its children and their IDs, inside the group parentId (the
 * project's top level when empty). The inputs of the node and of its children are ignored:
 * connections are SetInputOps.
 **/
struct CreateNodeOp {
    std::string parentId;
    NodeSerializationPtr node;
};

/**
 * @brief Deletes a node and, for a group, everything inside it.
 **/
struct DeleteNodeOp {
    std::string nodeId;
};

/**
 * @brief Replaces the whole state of one knob of the node nodeId, or of the project when nodeId is
 * unset: its values, curves, expressions and links. An unset value resets the knob to its default.
 **/
struct SetKnobOp {
    std::optional<std::string> nodeId;
    std::string knobName;
    std::optional<KnobSerialization> value;
};

/**
 * @brief Connects the input inputLabel of nodeId to sourceId, or disconnects it when sourceId is
 * empty. The source must be in the same group as the node.
 **/
struct SetInputOp {
    std::string nodeId;
    std::string inputLabel;
    std::string sourceId;
};

/**
 * @brief Gives a node a new script name and, when set, a new label.
 **/
struct RenameOp {
    std::string nodeId;
    std::string scriptName;
    std::optional<std::string> label;
};

struct NodeUiColor {
    float r = 0.f;
    float g = 0.f;
    float b = 0.f;

    bool operator==(const NodeUiColor&) const = default;
};

/**
 * @brief Changes the node graph appearance of a node. Each field left unset is kept; a field set
 * to an empty optional is cleared. A live node always has a position, so clearing it there
 * changes nothing.
 **/
struct SetNodeUiOp {
    std::string nodeId;
    std::optional<std::optional<std::pair<double, double>>> position;
    std::optional<std::optional<std::pair<double, double>>> size;
    std::optional<std::optional<NodeUiColor>> color;
    std::optional<std::optional<NodeUiColor>> overlayColor;
};

/**
 * @brief Replaces every user page of a node, with the user knobs it holds, and the order of all
 * its pages.
 **/
struct SetUserKnobsOp {
    std::string nodeId;
    std::list<GroupKnobSerialization> userPages;
    std::list<std::string> pagesOrder;
};

/**
 * @brief Replaces the roto shapes of a node, removing them all when roto is unset.
 **/
struct SetRotoDataOp {
    std::string nodeId;
    std::optional<RotoContextSerialization> roto;
};

/**
 * @brief Replaces the tracks of a node, removing them all when tracker is unset.
 **/
struct SetTrackerDataOp {
    std::string nodeId;
    std::optional<TrackerContextSerialization> tracker;
};

/**
 * @brief Replaces the project's additional formats, its layer registry and/or its variables (the
 * user knobs of its variables group, in order, with their definitions and state); an unset field
 * is kept. Either every field is applied or none is.
 **/
struct SetProjectDataOp {
    std::optional<std::list<FormatSerialization>> formats;
    std::optional<std::list<LayerRegistryEntrySerialization>> layers;
    std::optional<std::list<KnobSerialization>> variables;
};

/**
 * @brief One edit of a node graph. Operations name nodes by their persistent ID, never by script
 * name, so a sequence of them stays valid across renames.
 **/
typedef std::variant<CreateNodeOp,
                     DeleteNodeOp,
                     SetKnobOp,
                     SetInputOp,
                     RenameOp,
                     SetNodeUiOp,
                     SetUserKnobsOp,
                     SetRotoDataOp,
                     SetTrackerDataOp,
                     SetProjectDataOp>
    GraphOp;

enum class OpStatus {
    eApplied,
    // A node the operation names does not exist.
    eTargetMissing,
    // The operation cannot be applied as given: an unknown knob or input, a name taken by a
    // sibling, an ID already in use, a value of the wrong type.
    eInvalid,
};

/**
 * @brief The outcome of applying one operation. Applying never throws for a bad operation: it
 * reports it here and leaves the graph as it was.
 **/
struct OpResult {
    OpStatus status = OpStatus::eApplied;
    std::string message;

    bool applied() const
    {
        return status == OpStatus::eApplied;
    }

    static OpResult targetMissing(std::string message)
    {
        return OpResult { OpStatus::eTargetMissing, std::move(message) };
    }

    static OpResult invalid(std::string message)
    {
        return OpResult { OpStatus::eInvalid, std::move(message) };
    }
};

/**
 * @brief Whether id has the form of a persistent node ID: 32 lowercase hexadecimal digits.
 **/
bool isWellFormedNodeId(std::string_view id);

/**
 * @brief The name an operation has in YAML (its `op:` key), e.g. "createNode".
 **/
std::string_view graphOpName(const GraphOp& op);

} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_Ops_GraphOp_h
