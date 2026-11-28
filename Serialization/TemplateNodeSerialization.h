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

#ifndef Serialization_TemplateNodeSerialization_h
#define Serialization_TemplateNodeSerialization_h

#include "Global/Macros.h"

#include <list>
#include <memory>
#include <string>
#include <vector>

#include "Serialization/NodeSerialization.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

// The script names of the template node's knobs.
inline constexpr const char* kTemplateParamKey = "templateKey";
inline constexpr const char* kTemplateParamName = "templateName";
inline constexpr const char* kTemplateParamId = "templateId";
inline constexpr const char* kTemplateParamVersion = "version";
inline constexpr const char* kTemplateParamPublishDir = "publishDir";
inline constexpr const char* kTemplateParamNewerVersion = "newerVersion";

/**
 * @brief What a template node says about its template: the entry it names in the project, the
 * template's identity and version, and where its versions are published.
 **/
struct TemplateNodeState {
    std::string key;
    std::string templateId;
    std::string name;
    int version = 0;
    std::string publishDir;
};

/**
 * @brief The state saved in node's knobs; a knob that is not saved holds its default.
 **/
TemplateNodeState readTemplateNode(const NodeSerialization& node);

/**
 * @brief Sets node's template knobs to state, replacing the saved ones.
 **/
void writeTemplateNode(const TemplateNodeState& state,
                       NodeSerialization* node);

/**
 * @brief The first template node among nodes, not looking inside groups, or null.
 **/
const NodeSerialization* findTemplateNode(const std::list<NodeSerializationPtr>& nodes);

/**
 * @brief Every template node among nodes and in the groups they hold, in file order.
 **/
std::vector<const NodeSerialization*> findTemplateNodes(const std::list<NodeSerializationPtr>& nodes);

/**
 * @brief Removes every template node from nodes and from the groups they hold.
 **/
void eraseTemplateNodes(std::list<NodeSerializationPtr>* nodes);

} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_TemplateNodeSerialization_h
