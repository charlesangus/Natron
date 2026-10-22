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

#include "Serialization/Yaml/ClipboardYaml.h"

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include "Serialization/Yaml/YamlPrivate.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

namespace {

    using namespace YamlDetail;

    std::string
    lastNameComponent(const std::string& name)
    {
        const std::size_t dot = name.rfind('.');

        return (dot == std::string::npos) ? name : name.substr(dot + 1);
    }

    NodeGuiSerializationPtr
    findGuiChild(const NodeGuiSerialization& gui,
                 const std::string& scriptName)
    {
        for (const NodeGuiSerializationPtr& child : gui.children) {
            if (child && (lastNameComponent(child->nodeName) == scriptName)) {
                return child;
            }
        }

        return NodeGuiSerializationPtr();
    }

    void
    emitGuiEntries(YAML::Emitter& em,
                   const NodeSerialization& node,
                   const NodeGuiSerialization& gui,
                   const std::string& name)
    {
        emitKey(em, name);
        em << YAML::Value;
        emitNodeGuiValue(em, gui);
        for (const NodeSerializationPtr& child : node.children) {
            if (!child) {
                continue;
            }
            NodeGuiSerializationPtr childGui = findGuiChild(gui, child->scriptName);
            if (childGui) {
                emitGuiEntries(em, *child, *childGui, name + "." + child->scriptName);
            }
        }
    }

    typedef std::map<std::string, NodeGuiSerializationPtr> GuiByName;

    NodeGuiSerializationPtr
    guiForNode(const NodeSerialization& node,
               const std::string& name,
               const GuiByName& guis,
               std::set<std::string>* usedNames,
               bool* found)
    {
        GuiByName::const_iterator it = guis.find(name);
        *found = (it != guis.end());
        NodeGuiSerializationPtr gui = *found ? it->second : std::make_shared<NodeGuiSerialization>();
        if (*found) {
            usedNames->insert(name);
        }
        gui->nodeName = name;
        gui->children.clear();

        // The paste walks a group's nodes and their graph states side by side, so once any of
        // them has a state every one needs an entry.
        std::list<NodeGuiSerializationPtr> children;
        bool anyChildFound = false;
        for (const NodeSerializationPtr& child : node.children) {
            if (!child) {
                continue;
            }
            bool childFound = false;
            children.push_back(guiForNode(*child, name + "." + child->scriptName, guis, usedNames, &childFound));
            anyChildFound = anyChildFound || childFound;
        }
        if (anyChildFound) {
            gui->children = children;
        }

        return gui;
    }

} // namespace

std::string
encodeClipboardYaml(const ClipboardSerialization& clipboard)
{
    InlineBlobSink sink;
    std::set<std::string> usedPaths;
    YAML::Emitter em;
    em << YAML::BeginMap;
    emitDocumentFormat(em, kClipboardYamlFormat, kClipboardYamlFormatVersion);
    emitKey(em, "nodes");
    em << YAML::Value;
    emitNodeMap(em, clipboard.nodes, std::string(), sink, &usedPaths);

    std::vector<std::pair<NodeSerializationPtr, NodeGuiSerializationPtr>> withGui;
    std::list<NodeGuiSerializationPtr>::const_iterator itGui = clipboard.nodesUI.begin();
    for (std::list<NodeSerializationPtr>::const_iterator it = clipboard.nodes.begin(); (it != clipboard.nodes.end()) && (itGui != clipboard.nodesUI.end()); ++it, ++itGui) {
        if (*it && *itGui) {
            withGui.emplace_back(*it, *itGui);
        }
    }
    std::stable_sort(withGui.begin(), withGui.end(), [](const std::pair<NodeSerializationPtr, NodeGuiSerializationPtr>& a, const std::pair<NodeSerializationPtr, NodeGuiSerializationPtr>& b) {
        return a.first->scriptName < b.first->scriptName;
    });
    if (!withGui.empty()) {
        emitKey(em, "gui");
        em << YAML::Value << YAML::BeginMap;
        for (const std::pair<NodeSerializationPtr, NodeGuiSerializationPtr>& entry : withGui) {
            emitGuiEntries(em, *entry.first, *entry.second, entry.first->scriptName);
        }
        em << YAML::EndMap;
    }
    em << YAML::EndMap;

    return documentText(em);
}

ClipboardSerialization
decodeClipboardYaml(std::string_view text,
                    YamlWarnings* warnings)
{
    YAML::Node document = loadDocument(text, "the clipboard");
    if (!document.IsMap()) {
        throw YamlError("this is not a Natron clipboard: it is not a YAML map");
    }
    MapReader reader(document, "clipboard", warnings);
    checkDocumentFormat(reader, kClipboardYamlFormat, kClipboardYamlFormatVersion, "a Natron clipboard");

    ClipboardSerialization clipboard;
    NoBlobSource source;
    if (YAML::Node nodes = reader.get("nodes")) {
        clipboard.nodes = readNodeMap(nodes, source, "clipboard.nodes", warnings);
    }
    GuiByName guis;
    if (YAML::Node gui = reader.get("gui")) {
        MapReader guiReader(gui, "clipboard.gui", warnings);
        for (const std::pair<std::string, YAML::Node>& entry : guiReader.entries()) {
            guis[entry.first] = readNodeGuiValue(entry.first, entry.second, "clipboard.gui." + entry.first, warnings);
        }
    }
    reader.finish();

    std::set<std::string> usedNames;
    for (const NodeSerializationPtr& node : clipboard.nodes) {
        bool found = false;
        clipboard.nodesUI.push_back(guiForNode(*node, node->scriptName, guis, &usedNames, &found));
    }
    for (GuiByName::const_iterator it = guis.begin(); it != guis.end(); ++it) {
        if (usedNames.find(it->first) == usedNames.end()) {
            warn(warnings, "clipboard.gui: \"" + it->first + "\" names no copied node and is ignored");
        }
    }

    return clipboard;
}

} // namespace Serialization
NATRON_NAMESPACE_EXIT
