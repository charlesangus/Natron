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

#include "Serialization/TemplateNodeSerialization.h"

#include <algorithm>
#include <variant>

#include "Serialization/ProjectSerialization.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

namespace {

    const KnobSerialization*
    savedKnob(const NodeSerialization& node,
              const char* name)
    {
        for (const KnobSerialization& knob : node.knobs) {
            if ((knob.name == name) && !knob.dimensions.empty() && knob.dimensions[0].value) {
                return &knob;
            }
        }

        return nullptr;
    }

    std::string
    stringOf(const NodeSerialization& node,
             const char* name)
    {
        const KnobSerialization* knob = savedKnob(node, name);
        const std::string* value = knob ? std::get_if<std::string>(&*knob->dimensions[0].value) : nullptr;

        return value ? *value : std::string();
    }

    int
    intOf(const NodeSerialization& node,
          const char* name)
    {
        const KnobSerialization* knob = savedKnob(node, name);
        const int* value = knob ? std::get_if<int>(&*knob->dimensions[0].value) : nullptr;

        return value ? *value : 0;
    }

    // enabled and secret are saved with the value and restored on load, so they must be what
    // the template node's own knob has.
    void
    setKnob(NodeSerialization* node,
            const char* name,
            const char* typeName,
            const KnobValueSerialization& value,
            const KnobValueSerialization& defaultValue,
            bool enabled,
            bool secret)
    {
        KnobSerialization knob;
        knob.name = name;
        knob.typeName = typeName;
        knob.dimension = 1;
        knob.secret = secret;
        knob.dimensions.resize(1);
        knob.dimensions[0].enabled = enabled;
        knob.dimensions[0].value = value;
        knob.dimensions[0].defaultValue = defaultValue;
        for (KnobSerialization& existing : node->knobs) {
            if (existing.name == name) {
                existing = knob;

                return;
            }
        }
        node->knobs.push_back(knob);
    }

} // namespace

TemplateNodeState
readTemplateNode(const NodeSerialization& node)
{
    TemplateNodeState state;
    state.key = stringOf(node, kTemplateParamKey);
    state.templateId = stringOf(node, kTemplateParamId);
    state.name = stringOf(node, kTemplateParamName);
    state.version = intOf(node, kTemplateParamVersion);
    state.importedVersion = intOf(node, kTemplateParamImportedVersion);
    state.publishDir = stringOf(node, kTemplateParamPublishDir);

    return state;
}

void
writeTemplateNode(const TemplateNodeState& state,
                  NodeSerialization* node)
{
    const KnobValueSerialization noText = std::string();
    const KnobValueSerialization zero = 0;
    setKnob(node, kTemplateParamKey, "String", state.key, noText, true, true);
    setKnob(node, kTemplateParamName, "String", state.name, noText, true, false);
    setKnob(node, kTemplateParamId, "String", state.templateId, noText, false, false);
    setKnob(node, kTemplateParamVersion, "Int", state.version, zero, false, false);
    setKnob(node, kTemplateParamImportedVersion, "Int", state.importedVersion, zero, false, false);
    setKnob(node, kTemplateParamPublishDir, "Path", state.publishDir, noText, true, false);
}

const NodeSerialization*
findTemplateNode(const std::list<NodeSerializationPtr>& nodes)
{
    for (const NodeSerializationPtr& node : nodes) {
        if (node && (node->pluginID == kTemplateNodePluginId)) {
            return node.get();
        }
    }

    return nullptr;
}

void
eraseTemplateNodes(std::list<NodeSerializationPtr>* nodes)
{
    nodes->remove_if([](const NodeSerializationPtr& node) {
        return !node || (node->pluginID == kTemplateNodePluginId);
    });
    for (const NodeSerializationPtr& node : *nodes) {
        eraseTemplateNodes(&node->children);
    }
}

} // namespace Serialization
NATRON_NAMESPACE_EXIT
