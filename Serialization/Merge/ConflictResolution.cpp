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

#include "Serialization/Merge/ConflictResolution.h"

#include <algorithm>
#include <iterator>
#include <list>
#include <map>
#include <optional>

#include "Serialization/Merge/MergeHandler.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

namespace {

    bool
    failWith(std::string* error,
             const std::string& message)
    {
        if (error) {
            *error = message;
        }

        return false;
    }

    std::string
    quoted(const std::string& name)
    {
        return "\"" + name + "\"";
    }

    KnobDimensionSerialization
    defaultDimensionLike(const KnobDimensionSerialization& live)
    {
        KnobDimensionSerialization dimension;
        dimension.defaultValue = live.defaultValue;
        dimension.value = live.defaultValue;

        return dimension;
    }

    // Times are compared as the curve merge stores them: copied, never computed.
    void
    setKeyAt(CurveSerialization* curve,
             double time,
             const std::optional<KeyFrameSerialization>& key)
    {
        std::vector<KeyFrameSerialization>& keys = curve->keyframes;
        keys.erase(std::remove_if(keys.begin(), keys.end(), [time](const KeyFrameSerialization& k) {
                       return k.time == time;
                   }),
                   keys.end());
        if (key) {
            KeyFrameSerialization placed = *key;
            placed.time = time;
            keys.insert(std::upper_bound(keys.begin(), keys.end(), placed, [](const KeyFrameSerialization& a, const KeyFrameSerialization& b) {
                            return a.time < b.time;
                        }),
                        placed);
        }
    }

    // What a side stands for in a delete-modify conflict, whose side is the one that deleted.
    enum class Role {
        eDeleter,
        eKeeper,
        eBase,
    };

    Role
    roleOf(const MergeConflict& conflict,
           ResolutionSide side)
    {
        if (side == ResolutionSide::eBase) {
            return Role::eBase;
        }
        const bool mine = (side == ResolutionSide::eMine);

        return (mine == (conflict.side == MergeSide::eMine)) ? Role::eDeleter : Role::eKeeper;
    }

    typedef std::list<NodeSerializationPtr> NodeList;

    struct FoundNode {
        NodeSerializationPtr node;
        // The list holding it: its group's children, or the top level.
        NodeList* siblings = nullptr;
        std::string parentId;
    };

    bool
    findNode(NodeList* nodes,
             const std::string& parentId,
             const std::string& id,
             FoundNode* found)
    {
        for (const NodeSerializationPtr& node : *nodes) {
            if (!node) {
                continue;
            }
            if (node->id == id) {
                found->node = node;
                found->siblings = nodes;
                found->parentId = parentId;

                return true;
            }
            if (findNode(&node->children, node->id, id, found)) {
                return true;
            }
        }

        return false;
    }

    NodeSerializationPtr
    siblingWithId(const NodeList& siblings,
                  const std::string& id)
    {
        for (const NodeSerializationPtr& node : siblings) {
            if (node && (node->id == id)) {
                return node;
            }
        }

        return NodeSerializationPtr();
    }

    void
    disconnectConsumers(NodeList* siblings,
                        const std::string& scriptName)
    {
        for (const NodeSerializationPtr& node : *siblings) {
            if (!node) {
                continue;
            }
            for (std::map<std::string, std::string>::iterator input = node->inputs.begin(); input != node->inputs.end();) {
                if (input->second == scriptName) {
                    input = node->inputs.erase(input);
                } else {
                    ++input;
                }
            }
        }
    }

    // Deletes the nodes of ids that nothing outside them reads from, and disconnects what read
    // from them. A node an artist also wired elsewhere stays, and so do the nodes it reads from.
    void
    removeUnreadNodes(ProjectSerialization* snapshot,
                      const std::vector<std::string>& ids)
    {
        std::vector<FoundNode> doomed;
        for (const std::string& id : ids) {
            FoundNode found;
            if (findNode(&snapshot->nodes.nodes, std::string(), id, &found)) {
                doomed.push_back(found);
            }
        }
        const auto isDoomed = [&doomed](const NodeSerializationPtr& node) {
            return std::any_of(doomed.begin(), doomed.end(), [&node](const FoundNode& entry) {
                return entry.node == node;
            });
        };
        bool changed = true;
        while (changed) {
            changed = false;
            for (std::vector<FoundNode>::iterator it = doomed.begin(); it != doomed.end();) {
                bool readOutside = false;
                for (const NodeSerializationPtr& reader : *it->siblings) {
                    if (!reader || (reader == it->node) || isDoomed(reader)) {
                        continue;
                    }
                    for (const std::pair<const std::string, std::string>& input : reader->inputs) {
                        if (input.second == it->node->scriptName) {
                            readOutside = true;
                            break;
                        }
                    }
                    if (readOutside) {
                        break;
                    }
                }
                if (readOutside) {
                    it = doomed.erase(it);
                    changed = true;
                } else {
                    ++it;
                }
            }
        }
        for (const FoundNode& entry : doomed) {
            entry.siblings->remove(entry.node);
        }
        for (const FoundNode& entry : doomed) {
            disconnectConsumers(entry.siblings, entry.node->scriptName);
        }
    }

    bool
    connect(ProjectSerialization* snapshot,
            const MergeConflictConnection& connection,
            std::string* error)
    {
        FoundNode found;
        if (!findNode(&snapshot->nodes.nodes, std::string(), connection.nodeId, &found)) {
            return failWith(error, "a node of this conflict is no longer in the project");
        }
        if (connection.sourceId.empty()) {
            found.node->inputs.erase(connection.input);

            return true;
        }
        const NodeSerializationPtr source = siblingWithId(*found.siblings, connection.sourceId);
        if (!source) {
            return failWith(error, "the node to connect to " + quoted(found.node->scriptName) + " is no longer beside it");
        }
        found.node->inputs[connection.input] = source->scriptName;

        return true;
    }

    void
    putKnob(std::list<KnobSerialization>* knobs,
            const std::string& name,
            const KnobSerialization* value)
    {
        std::list<KnobSerialization>::iterator found = std::find_if(knobs->begin(), knobs->end(), [&name](const KnobSerialization& knob) {
            return knob.name == name;
        });
        if (value) {
            if (found == knobs->end()) {
                knobs->push_back(*value);
            } else {
                *found = *value;
            }
        } else if (found != knobs->end()) {
            knobs->erase(found);
        }
    }

    const KnobSerialization*
    knobNamed(const std::list<KnobSerialization>& knobs,
              const std::string& name)
    {
        for (const KnobSerialization& knob : knobs) {
            if (knob.name == name) {
                return &knob;
            }
        }

        return nullptr;
    }

    bool
    removeUserKnob(std::list<GroupKnobChildSerialization>* children,
                   const std::string& name)
    {
        for (std::list<GroupKnobChildSerialization>::iterator child = children->begin(); child != children->end(); ++child) {
            if (!child->isGroup && (child->knob.name == name)) {
                children->erase(child);

                return true;
            }
            if (child->isGroup && removeUserKnob(&child->group.children, name)) {
                return true;
            }
        }

        return false;
    }

    // Some knobs are saved even at their default (a channel set always is); a knob list without
    // them would reset them.
    void
    keepDefaultKnobs(const std::list<KnobSerialization>& live,
                     std::list<KnobSerialization>* knobs)
    {
        for (const KnobSerialization& knob : live) {
            if (isDefaultKnob(knob) && !knobNamed(*knobs, knob.name)) {
                knobs->push_back(knob);
            }
        }
    }

    const RotoItemSerialization&
    rotoItemOf(const RotoLayerChildSerialization& child)
    {
        switch (child.type) {
        case eRotoLayerChildTypeStroke:
            return child.stroke;
        case eRotoLayerChildTypeLayer:
            return child.layer;
        case eRotoLayerChildTypeBezier:
            break;
        }

        return child.bezier;
    }

    std::list<KnobSerialization>*
    rotoItemKnobs(RotoLayerChildSerialization* child)
    {
        switch (child->type) {
        case eRotoLayerChildTypeStroke:
            return &child->stroke.knobs;
        case eRotoLayerChildTypeBezier:
            return &child->bezier.knobs;
        case eRotoLayerChildTypeLayer:
            break;
        }

        return nullptr;
    }

    typedef std::list<RotoLayerChildSerialization> RotoChildren;

    bool
    findRotoItem(RotoChildren* children,
                 const std::string& name,
                 RotoChildren** owner,
                 RotoChildren::iterator* item)
    {
        for (RotoChildren::iterator child = children->begin(); child != children->end(); ++child) {
            if (rotoItemOf(*child).name == name) {
                *owner = children;
                *item = child;

                return true;
            }
            if ((child->type == eRotoLayerChildTypeLayer) && findRotoItem(&child->layer.children, name, owner, item)) {
                return true;
            }
        }

        return false;
    }

    std::list<TrackSerialization>::iterator
    findTrack(TrackerContextSerialization* tracker,
              const std::string& name)
    {
        return std::find_if(tracker->tracks.begin(), tracker->tracks.end(), [&name](const TrackSerialization& track) {
            return track.scriptName == name;
        });
    }

    RotoLayerSerialization*
    findRotoLayer(RotoLayerSerialization* layer,
                  const std::string& name)
    {
        if (layer->name == name) {
            return layer;
        }
        for (RotoLayerChildSerialization& child : layer->children) {
            if (child.type == eRotoLayerChildTypeLayer) {
                if (RotoLayerSerialization* found = findRotoLayer(&child.layer, name)) {
                    return found;
                }
            }
        }

        return nullptr;
    }

    // Where a restored item goes in its list: after the nearest item that came before it on the
    // side it is restored from, or first when none of those is left.
    template <typename Item, typename NameOf>
    typename std::list<Item>::iterator
    insertionPoint(std::list<Item>* items,
                   const std::vector<std::string>& before,
                   NameOf nameOf)
    {
        for (std::vector<std::string>::const_reverse_iterator name = before.rbegin(); name != before.rend(); ++name) {
            typename std::list<Item>::iterator found = std::find_if(items->begin(), items->end(), [&](const Item& item) {
                return nameOf(item) == *name;
            });
            if (found != items->end()) {
                return std::next(found);
            }
        }

        return items->begin();
    }

    bool
    putItem(NodeSerialization* node,
            const std::string& item,
            const MergeConflictValue& value,
            std::string* error)
    {
        if (node->hasRotoContext) {
            const RotoLayerChildSerialization* wanted = (value.roto && !value.roto->baseLayer.children.empty()) ? &value.roto->baseLayer.children.front() : nullptr;
            RotoChildren* owner = nullptr;
            RotoChildren::iterator found;
            if (findRotoItem(&node->rotoContext.baseLayer.children, item, &owner, &found)) {
                if (wanted) {
                    *found = *wanted;
                } else {
                    owner->erase(found);
                }
            } else if (wanted) {
                // A layer the merge removed leaves the item in the base layer.
                RotoLayerSerialization* layer = findRotoLayer(&node->rotoContext.baseLayer, rotoItemOf(*wanted).parentLayerName);
                RotoChildren* children = layer ? &layer->children : &node->rotoContext.baseLayer.children;
                children->insert(insertionPoint(children, value.itemsBefore, [](const RotoLayerChildSerialization& child) {
                                     return rotoItemOf(child).name;
                                 }),
                                 *wanted);
            }

            return true;
        }
        if (node->hasTrackerContext) {
            const TrackSerialization* wanted = (value.tracker && !value.tracker->tracks.empty()) ? &value.tracker->tracks.front() : nullptr;
            std::list<TrackSerialization>::iterator found = findTrack(&node->trackerContext, item);
            if (found != node->trackerContext.tracks.end()) {
                if (wanted) {
                    *found = *wanted;
                } else {
                    node->trackerContext.tracks.erase(found);
                }
            } else if (wanted) {
                std::list<TrackSerialization>* tracks = &node->trackerContext.tracks;
                tracks->insert(insertionPoint(tracks, value.itemsBefore, [](const TrackSerialization& track) {
                                   return track.scriptName;
                               }),
                               *wanted);
            }

            return true;
        }

        return failWith(error, quoted(node->scriptName) + " has no shapes or tracks");
    }

    std::list<KnobSerialization>*
    itemKnobs(NodeSerialization* node,
              const std::string& item)
    {
        if (node->hasRotoContext) {
            RotoChildren* owner = nullptr;
            RotoChildren::iterator found;

            return findRotoItem(&node->rotoContext.baseLayer.children, item, &owner, &found) ? rotoItemKnobs(&*found) : nullptr;
        }
        if (node->hasTrackerContext) {
            std::list<TrackSerialization>::iterator found = findTrack(&node->trackerContext, item);

            return (found == node->trackerContext.tracks.end()) ? nullptr : &found->knobs;
        }

        return nullptr;
    }

    bool
    moveToGroup(ProjectSerialization* snapshot,
                const std::string& nodeId,
                const std::string& groupId,
                std::string* error)
    {
        FoundNode found;
        if (!findNode(&snapshot->nodes.nodes, std::string(), nodeId, &found)) {
            return failWith(error, "the node of this conflict is no longer in the project");
        }
        if (found.parentId == groupId) {
            return true;
        }
        NodeList* target = &snapshot->nodes.nodes;
        if (!groupId.empty()) {
            FoundNode group;
            if (!findNode(&snapshot->nodes.nodes, std::string(), groupId, &group)) {
                return failWith(error, "the group to put " + quoted(found.node->scriptName) + " back in is no longer in the project");
            }
            FoundNode inside;
            if (findNode(&found.node->children, nodeId, groupId, &inside)) {
                return failWith(error, quoted(found.node->scriptName) + " cannot go inside itself");
            }
            target = &group.node->children;
        }
        for (const NodeSerializationPtr& sibling : *target) {
            if (sibling && (sibling->scriptName == found.node->scriptName)) {
                return failWith(error, "the group to put " + quoted(found.node->scriptName) + " back in already has a node of that name");
            }
        }
        found.siblings->remove(found.node);
        disconnectConsumers(found.siblings, found.node->scriptName);
        found.node->inputs.clear();
        target->push_back(found.node);

        return true;
    }

    // A recorded node's own state, other than knobs and inputs, onto the live node. The live
    // node's identity, name and plug-in stay, as the rest of the graph knows it by them.
    void
    restoreNodeState(const NodeSerialization& recorded,
                     NodeSerialization* live)
    {
        live->label = recorded.label;
        live->layout = recorded.layout;
        live->userPages = recorded.userPages;
        live->pagesOrder = recorded.pagesOrder;
        live->hasRotoContext = recorded.hasRotoContext;
        live->rotoContext = recorded.rotoContext;
        live->hasTrackerContext = recorded.hasTrackerContext;
        live->trackerContext = recorded.trackerContext;
    }

    bool
    applyState(const MergeConflict& conflict,
               const MergeConflictValue& value,
               std::optional<Role> role,
               ProjectSerialization* snapshot,
               std::string* error)
    {
        FoundNode found;
        const bool hasNode = !conflict.nodeId.empty() && findNode(&snapshot->nodes.nodes, std::string(), conflict.nodeId, &found);
        switch (conflict.kind) {
        case MergeConflictKind::eInput:
        case MergeConflictKind::eStructuralInsert:
        case MergeConflictKind::eSpliceChoice:
            if (value.connections.empty()) {
                return failWith(error, "this side's connection cannot be put back");
            }
            for (const MergeConflictConnection& connection : value.connections) {
                if (!connect(snapshot, connection, error)) {
                    return false;
                }
            }
            if (conflict.kind == MergeConflictKind::eStructuralInsert) {
                removeUnreadNodes(snapshot, value.removedNodes);
            }

            return true;
        case MergeConflictKind::eReparent:
            if (!value.parent) {
                return failWith(error, "this side's group is not in the project");
            }
            if (!moveToGroup(snapshot, conflict.nodeId, *value.parent, error)) {
                return false;
            }
            for (const MergeConflictConnection& connection : value.connections) {
                std::string ignored;
                // A source that did not follow the node into its group stays unconnected.
                connect(snapshot, connection, &ignored);
            }

            return true;
        case MergeConflictKind::eAdopted:
            if (!hasNode) {
                return failWith(error, "the node of this conflict is no longer in the project");
            }
            if (!value.knobs) {
                return failWith(error, "this side has no such node");
            }
            for (const std::string& name : conflict.defaultedKnobs) {
                putKnob(&found.node->knobs, name, knobNamed(*value.knobs, name));
            }

            return true;
        case MergeConflictKind::eRotoShape:
            if (!hasNode || !found.node->hasRotoContext) {
                return failWith(error, "the node of this conflict no longer has shapes");
            }
            if (!conflict.item.empty()) {
                return putItem(found.node.get(), conflict.item, value, error);
            }
            found.node->rotoContext.hasSplineWarp = value.roto && value.roto->hasSplineWarp;
            found.node->rotoContext.splineWarp = found.node->rotoContext.hasSplineWarp ? value.roto->splineWarp : SplineWarpSerialization();

            return true;
        case MergeConflictKind::eDeleteModify:
            if (conflict.nodeId.empty() && conflict.baseNodeId.empty() && !conflict.knob.empty()) {
                if (role && (*role == Role::eDeleter)) {
                    putKnob(&snapshot->projectKnobs, conflict.knob, nullptr);
                } else if (value.knob) {
                    putKnob(&snapshot->projectKnobs, conflict.knob, &*value.knob);
                }

                return true;
            }
            if (!hasNode) {
                return failWith(error, "the node of this conflict is no longer in the project");
            }
            if (!conflict.item.empty()) {
                return putItem(found.node.get(), conflict.item, value, error);
            }
            if (!conflict.knob.empty()) {
                if (role && (*role == Role::eDeleter)) {
                    putKnob(&found.node->knobs, conflict.knob, nullptr);
                    for (GroupKnobSerialization& page : found.node->userPages) {
                        removeUserKnob(&page.children, conflict.knob);
                    }
                } else if (value.knob) {
                    putKnob(&found.node->knobs, conflict.knob, &*value.knob);
                }

                return true;
            }
            if (!role || (*role == Role::eKeeper)) {
                return true;
            }
            if (*role == Role::eDeleter) {
                found.siblings->remove(found.node);
                disconnectConsumers(found.siblings, found.node->scriptName);

                return true;
            }
            if (!value.knobs) {
                return failWith(error, "the base state of this node was not recorded");
            }
            {
                std::list<KnobSerialization> knobs = *value.knobs;
                keepDefaultKnobs(found.node->knobs, &knobs);
                found.node->knobs = std::move(knobs);
            }
            if (!value.node) {
                return true;
            }
            restoreNodeState(*value.node, found.node.get());
            if (value.parent && !moveToGroup(snapshot, conflict.nodeId, *value.parent, error)) {
                return false;
            }
            found.node->inputs.clear();
            for (const MergeConflictConnection& connection : value.connections) {
                std::string ignored;
                // A base source the merge left out of the node's group stays unconnected.
                connect(snapshot, connection, &ignored);
            }

            return true;
        case MergeConflictKind::eValue:
        case MergeConflictKind::eCurveKey:
            break;
        }

        if ((conflict.kind == MergeConflictKind::eCurveKey) && !conflict.time) {
            return failWith(error, "this key conflict has no time");
        }
        std::list<KnobSerialization>* knobs = nullptr;
        if (conflict.nodeId.empty() && conflict.baseNodeId.empty()) {
            knobs = &snapshot->projectKnobs;
        } else if (!hasNode) {
            return failWith(error, "the node of this conflict is no longer in the project");
        } else if (conflict.item.empty()) {
            knobs = &found.node->knobs;
        } else {
            knobs = itemKnobs(found.node.get(), conflict.item);
            if (!knobs) {
                return failWith(error, quoted(found.node->scriptName) + " no longer has " + quoted(conflict.item));
            }
        }
        const KnobSerialization* live = knobNamed(*knobs, conflict.knob);
        const KnobSerialization* like = live ? live : value.knob ? &*value.knob
            : conflict.mine.knob                                 ? &*conflict.mine.knob
            : conflict.theirs.knob                               ? &*conflict.theirs.knob
            : conflict.base.knob                                 ? &*conflict.base.knob
                                                                 : nullptr;
        if (!like) {
            return failWith(error, "no state of " + quoted(conflict.knob) + " was recorded");
        }
        KnobSerialization state = *like;
        if (!applyConflictKnobValue(conflict, value, &state, error)) {
            return false;
        }
        putKnob(knobs, conflict.knob, isDefaultKnob(state) ? nullptr : &state);

        return true;
    }

    // Whether value, the state of choice, has anything to put back for conflict.
    bool
    hasState(const MergeConflict& conflict,
             const MergeConflictChoice& choice,
             const MergeConflictValue& value)
    {
        switch (conflict.kind) {
        case MergeConflictKind::eInput:
        case MergeConflictKind::eStructuralInsert:
        case MergeConflictKind::eSpliceChoice:
            return !value.connections.empty();
        case MergeConflictKind::eReparent:
            return value.parent.has_value();
        case MergeConflictKind::eAdopted:
            return value.knobs.has_value();
        case MergeConflictKind::eDeleteModify:
            if (choice.option.empty() && conflict.item.empty() && conflict.knob.empty() && (roleOf(conflict, choice.side) == Role::eBase)) {
                return value.knobs.has_value();
            }

            return true;
        case MergeConflictKind::eCurveKey:
            if (!conflict.time) {
                return false;
            }
            // One key replaces the other at a shared time, so the choice could not keep both.
            return std::none_of(value.otherKeys.begin(), value.otherKeys.end(), [&conflict](const KeyFrameSerialization& key) {
                return key.time == *conflict.time;
            });
        case MergeConflictKind::eValue:
        case MergeConflictKind::eRotoShape:
            break;
        }

        return true;
    }

    // The knob the conflicted knob becomes under value, from a recorded state of it.
    std::optional<KnobSerialization>
    knobResult(const MergeConflict& conflict,
               const MergeConflictValue& value)
    {
        const std::optional<KnobSerialization>& like = conflict.mine.knob ? conflict.mine.knob : conflict.theirs.knob ? conflict.theirs.knob
                                                                                                                      : conflict.base.knob;
        if (!like) {
            return std::nullopt;
        }
        KnobSerialization state = *like;
        std::string ignored;
        if (!applyConflictKnobValue(conflict, value, &state, &ignored)) {
            return std::nullopt;
        }

        return state;
    }

    bool
    sameKnobs(const std::list<KnobSerialization>& a,
              const std::list<KnobSerialization>& b,
              const std::vector<std::string>& names)
    {
        return std::all_of(names.begin(), names.end(), [&a, &b](const std::string& name) {
            const KnobSerialization* left = knobNamed(a, name);
            const KnobSerialization* right = knobNamed(b, name);

            return (!left && !right) || (left && right && (*left == *right));
        });
    }

    // Whether choosing a and choosing b give the same graph, as far as the recorded state tells.
    bool
    sameResult(const MergeConflict& conflict,
               const MergeConflictChoice& a,
               const MergeConflictChoice& b)
    {
        const MergeConflictValue& left = choiceState(conflict, a);
        const MergeConflictValue& right = choiceState(conflict, b);
        switch (conflict.kind) {
        case MergeConflictKind::eValue:
        case MergeConflictKind::eCurveKey: {
            if (!conflict.mine.knob && !conflict.theirs.knob && !conflict.base.knob) {
                return (left.knob == right.knob) && (left.key == right.key) && (left.otherKeys == right.otherKeys);
            }
            const std::optional<KnobSerialization> leftKnob = knobResult(conflict, left);
            const std::optional<KnobSerialization> rightKnob = knobResult(conflict, right);

            return leftKnob && rightKnob && (*leftKnob == *rightKnob);
        }
        case MergeConflictKind::eInput:
        case MergeConflictKind::eSpliceChoice:
            return left.connections == right.connections;
        case MergeConflictKind::eStructuralInsert:
            return (left.connections == right.connections) && (left.removedNodes == right.removedNodes);
        case MergeConflictKind::eReparent:
            return (left.parent == right.parent) && (left.connections == right.connections);
        case MergeConflictKind::eAdopted:
            return left.knobs && right.knobs && sameKnobs(*left.knobs, *right.knobs, conflict.defaultedKnobs);
        case MergeConflictKind::eRotoShape:
            return left == right;
        case MergeConflictKind::eDeleteModify:
            break;
        }

        if (!conflict.item.empty() || !a.option.empty() || !b.option.empty()) {
            return left == right;
        }
        const Role leftRole = roleOf(conflict, a.side);
        const Role rightRole = roleOf(conflict, b.side);
        if (leftRole == rightRole) {
            return true;
        }
        if ((leftRole == Role::eDeleter) || (rightRole == Role::eDeleter) || conflict.knob.empty()) {
            // Whether a node's base state is what the kept node already holds depends on the live graph.
            return false;
        }
        const MergeConflictValue& kept = (leftRole == Role::eKeeper) ? left : right;
        const MergeConflictValue& base = (leftRole == Role::eBase) ? left : right;

        // The kept knob is as the merge left it, and a base without the knob leaves it so too.
        return !base.knob || (base.knob == kept.knob);
    }

} // anon namespace

const MergeConflictValue&
sideState(const MergeConflict& conflict,
          ResolutionSide side)
{
    switch (side) {
    case ResolutionSide::eMine:
        return conflict.mine;
    case ResolutionSide::eTheirs:
        return conflict.theirs;
    case ResolutionSide::eBase:
        break;
    }

    return conflict.base;
}

bool
applyConflictKnobValue(const MergeConflict& conflict,
                       const MergeConflictValue& value,
                       KnobSerialization* state,
                       std::string* error)
{
    const int dimension = conflict.dimension;
    // One saved dimension stands for all of the knob's, as the merge compared them.
    if ((dimension >= 0) && (state->dimensions.size() == 1) && (state->dimension > 1)) {
        state->dimensions.resize(state->dimension, state->dimensions.front());
    }
    if (dimension == kMergeConflictAllDimensions) {
        // A folded live knob takes the side's one dimension as it is, so it stays folded.
        if (state->dimensions.empty()) {
            return failWith(error, quoted(conflict.knob) + " has no dimension");
        }
        for (KnobDimensionSerialization& target : state->dimensions) {
            if (conflict.kind == MergeConflictKind::eValue) {
                if (value.knob && !value.knob->dimensions.empty()) {
                    KnobDimensionSerialization recorded = value.knob->dimensions.front();
                    if (!recorded.defaultValue) {
                        recorded.defaultValue = target.defaultValue;
                    }
                    target = std::move(recorded);
                } else {
                    target = defaultDimensionLike(target);
                }
                continue;
            }
            if (!conflict.time) {
                return failWith(error, "this key conflict has no time");
            }
            if (!target.curve) {
                target.curve = CurveSerialization();
            }
            setKeyAt(&*target.curve, *conflict.time, value.key);
            for (const KeyFrameSerialization& key : value.otherKeys) {
                setKeyAt(&*target.curve, key.time, key);
            }
            if (target.curve->keyframes.empty()) {
                target.curve.reset();
            }
        }

        return true;
    }
    if (conflict.kind == MergeConflictKind::eValue) {
        if (dimension < 0) {
            state->parametricCurves = value.knob ? value.knob->parametricCurves : std::nullopt;
            state->stringAnimation = value.knob ? value.knob->stringAnimation : std::nullopt;
            state->choiceOptionID = value.knob ? value.knob->choiceOptionID : std::nullopt;
        } else {
            if (dimension >= int(state->dimensions.size())) {
                return failWith(error, quoted(conflict.knob) + " has no dimension " + std::to_string(dimension));
            }
            if (value.knob && !value.knob->dimensions.empty() && ((dimension < int(value.knob->dimensions.size())) || (value.knob->dimensions.size() == 1))) {
                // A plug-in knob's default is not saved with the side it came from, so the live one stays.
                KnobDimensionSerialization recorded = value.knob->dimensions[std::min(dimension, int(value.knob->dimensions.size()) - 1)];
                if (!recorded.defaultValue) {
                    recorded.defaultValue = state->dimensions[dimension].defaultValue;
                }
                state->dimensions[dimension] = std::move(recorded);
            } else {
                state->dimensions[dimension] = defaultDimensionLike(state->dimensions[dimension]);
            }
        }

        return true;
    }

    if (!conflict.time) {
        return failWith(error, "this key conflict has no time");
    }
    CurveSerialization* curve = nullptr;
    if (state->parametricCurves) {
        if ((dimension < 0) || (dimension >= int(state->parametricCurves->size()))) {
            return failWith(error, quoted(conflict.knob) + " has no curve " + std::to_string(dimension));
        }
        std::list<CurveSerialization>::iterator found = state->parametricCurves->begin();
        std::advance(found, dimension);
        curve = &*found;
    } else {
        if ((dimension < 0) || (dimension >= int(state->dimensions.size()))) {
            return failWith(error, quoted(conflict.knob) + " has no dimension " + std::to_string(dimension));
        }
        KnobDimensionSerialization& target = state->dimensions[dimension];
        if (!target.curve) {
            target.curve = CurveSerialization();
        }
        curve = &*target.curve;
    }
    setKeyAt(curve, *conflict.time, value.key);
    for (const KeyFrameSerialization& key : value.otherKeys) {
        setKeyAt(curve, key.time, key);
    }
    if (!state->parametricCurves) {
        KnobDimensionSerialization& target = state->dimensions[dimension];
        if (target.curve->keyframes.empty()) {
            target.curve.reset();
        }
    }

    return true;
}

bool
applyConflictSide(const MergeConflict& conflict,
                  ResolutionSide side,
                  ProjectSerialization* snapshot,
                  std::string* error)
{
    if ((conflict.kind == MergeConflictKind::eSpliceChoice) && (side == ResolutionSide::eMine)) {
        return true;
    }
    return applyState(conflict, sideState(conflict, side), roleOf(conflict, side), snapshot, error);
}

const MergeConflictOption*
conflictOption(const MergeConflict& conflict,
               const std::string& name)
{
    for (const MergeConflictOption& option : conflict.options) {
        if (option.name == name) {
            return &option;
        }
    }

    return nullptr;
}

bool
applyConflictOption(const MergeConflict& conflict,
                    const std::string& option,
                    ProjectSerialization* snapshot,
                    std::string* error)
{
    const MergeConflictOption* found = conflictOption(conflict, option);
    if (!found) {
        return failWith(error, "this conflict has no choice " + quoted(option));
    }

    return applyState(conflict, found->state, std::nullopt, snapshot, error);
}

const MergeConflictValue&
choiceState(const MergeConflict& conflict,
            const MergeConflictChoice& choice)
{
    if (!choice.option.empty()) {
        const MergeConflictOption* found = conflictOption(conflict, choice.option);
        if (found) {
            return found->state;
        }
    }

    return sideState(conflict, choice.side);
}

std::vector<MergeConflictChoice>
availableChoices(const MergeConflict& conflict,
                 const ChoiceFilter& applicable)
{
    const bool splice = (conflict.kind == MergeConflictKind::eSpliceChoice);
    std::vector<MergeConflictChoice> candidates;
    candidates.push_back(MergeConflictChoice { ResolutionSide::eMine, std::string(), splice ? "Keep My Wiring" : "Mine" });
    candidates.push_back(MergeConflictChoice { ResolutionSide::eTheirs, std::string(), splice ? "Splice In" : "Theirs" });
    if (!splice) {
        candidates.push_back(MergeConflictChoice { ResolutionSide::eBase, std::string(), "Base" });
    }
    for (const MergeConflictOption& option : conflict.options) {
        candidates.push_back(MergeConflictChoice { ResolutionSide::eMine, option.name, option.label });
    }

    std::vector<MergeConflictChoice> offered;
    for (const MergeConflictChoice& choice : candidates) {
        // Keeping my wiring leaves the graph as it is, so there is always something to choose.
        const bool keepsGraph = splice && choice.option.empty() && (choice.side == ResolutionSide::eMine);
        if (!keepsGraph && !hasState(conflict, choice, choiceState(conflict, choice))) {
            continue;
        }
        if (applicable && !applicable(choice)) {
            continue;
        }
        const bool repeats = std::any_of(offered.begin(), offered.end(), [&conflict, &choice](const MergeConflictChoice& earlier) {
            return sameResult(conflict, earlier, choice);
        });
        if (!repeats) {
            offered.push_back(choice);
        }
    }

    return offered;
}

} // namespace Serialization
NATRON_NAMESPACE_EXIT
