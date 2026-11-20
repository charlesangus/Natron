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

#ifndef Engine_LiveSession_SessionShared_h
#define Engine_LiveSession_SessionShared_h

#include "Global/Macros.h"

#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Serialization/Ops/GraphOp.h"

NATRON_NAMESPACE_ENTER
namespace LiveSession {

/**
 * @brief What an operation writes, as keys two operations share when they write the same thing:
 * a node's existence, one knob, one input, a node's name, its node graph appearance, its user
 * knobs, its roto shapes or tracks, or one kind of project data.
 **/
std::vector<std::string> graphOpKeys(const Serialization::GraphOp& op);

/**
 * @brief The key graphOpKeys() gives a create or delete of nodeId.
 **/
std::string nodeExistenceKey(const std::string& nodeId);

/**
 * @brief The node an operation edits (the created one for a createNode), or none for a project
 * edit.
 **/
std::optional<std::string> graphOpTargetNode(const Serialization::GraphOp& op);

/**
 * @brief Every node an operation names: its target, a connection's source, a creation's parent.
 **/
std::vector<std::string> graphOpNodeIds(const Serialization::GraphOp& op);

/**
 * @brief project.yaml as the bundle writer writes it, with the timeline position left out: each
 * peer moves in time on its own.
 **/
std::string canonicalProjectYaml(const ProjectPtr& project);

/**
 * @brief A hex SHA-256 of canonicalProjectYaml() and the blob members it names.
 **/
std::string projectChecksum(const ProjectPtr& project);

/**
 * @brief The project as a bundle (.ntpz) written in memory.
 **/
std::vector<std::byte> projectBundleBytes(const ProjectPtr& project);

/**
 * @brief The ID a createNode of node names as its parent: its main instance, or the group node
 * holding it, or empty at the top level.
 **/
std::string parentIdOf(const NodePtr& node);

/**
 * @brief The collection a createNode with parentId makes its node in, or null when the parent is
 * not found.
 **/
NodeCollectionPtr collectionForParent(const ProjectPtr& project, const std::string& parentId);

/**
 * @brief wanted when no node of collection but except holds it and taken does not hold it,
 * else wanted's trailing digits replaced by the lowest number that makes it free.
 **/
std::string uniqueScriptName(const NodeCollectionPtr& collection,
                             const std::string& wanted,
                             const Node* except,
                             const std::set<std::string>& taken);

/**
 * @brief The operations that make node again on a peer that lacks it: its createNode, then
 * the connections of the node and of the nodes inside it, then those of the nodes it feeds.
 **/
std::vector<Serialization::GraphOp> nodeStateOps(const NodePtr& node);

} // namespace LiveSession
NATRON_NAMESPACE_EXIT

#endif // Engine_LiveSession_SessionShared_h
