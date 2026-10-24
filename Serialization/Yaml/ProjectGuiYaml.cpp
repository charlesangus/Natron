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

#include "Serialization/Yaml/ProjectGuiYaml.h"

#include <map>
#include <memory>
#include <vector>

#include "Serialization/Yaml/YamlPrivate.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

namespace {

    using namespace YamlDetail;

    void
    emitBoolKey(YAML::Emitter& em,
                const char* key,
                bool value)
    {
        emitKey(em, key);
        em << YAML::Value;
        emitBool(em, value);
    }

    std::vector<int>
    readIntList(const YAML::Node& node,
                const std::string& context,
                std::size_t expectedSize)
    {
        if (!node.IsSequence() || (node.size() != expectedSize)) {
            fail(node, context, "expected a list of " + std::to_string(expectedSize) + " integers");
        }
        std::vector<int> values;
        for (std::size_t i = 0; i < expectedSize; ++i) {
            values.push_back(readInt(node[i], context));
        }

        return values;
    }

    void
    emitPane(YAML::Emitter& em,
             const PaneLayoutSerialization& pane)
    {
        em << YAML::Flow << YAML::BeginMap;
        emitKey(em, "name");
        em << YAML::Value;
        emitString(em, pane.name);
        emitKey(em, "tabs");
        em << YAML::Value;
        emitStringList(em, pane.tabs);
        emitKey(em, "current");
        em << YAML::Value;
        emitInt(em, pane.currentIndex);
        if (pane.isAnchor) {
            emitBoolKey(em, "anchor", true);
        }
        em << YAML::EndMap;
    }

    std::shared_ptr<PaneLayoutSerialization>
    readPane(const YAML::Node& node,
             const std::string& context,
             YamlWarnings* warnings)
    {
        std::shared_ptr<PaneLayoutSerialization> pane = std::make_shared<PaneLayoutSerialization>();
        MapReader reader(node, context, warnings);
        pane->name = readString(reader.get("name"), context + ".name");
        pane->tabs = readStringList(reader.get("tabs"), context + ".tabs");
        pane->currentIndex = readInt(reader.get("current"), context + ".current");
        if (YAML::Node anchor = reader.get("anchor")) {
            pane->isAnchor = readBool(anchor, context + ".anchor");
        }
        reader.finish();

        return pane;
    }

    void
    emitSplitter(YAML::Emitter& em,
                 const SplitterSerialization& splitter)
    {
        em << YAML::BeginMap;
        emitKey(em, "orientation");
        em << YAML::Value;
        emitInt(em, splitter.orientation);
        emitKey(em, "sizes");
        em << YAML::Value;
        emitString(em, splitter.sizes);
        emitKey(em, "children");
        em << YAML::Value << YAML::BeginSeq;
        for (const SplitterChildSerialization& child : splitter.children) {
            em << YAML::BeginMap;
            if (child.splitter) {
                emitKey(em, "splitter");
                em << YAML::Value;
                emitSplitter(em, *child.splitter);
            } else if (child.pane) {
                emitKey(em, "pane");
                em << YAML::Value;
                emitPane(em, *child.pane);
            }
            em << YAML::EndMap;
        }
        em << YAML::EndSeq;
        em << YAML::EndMap;
    }

    std::shared_ptr<SplitterSerialization>
    readSplitter(const YAML::Node& node,
                 const std::string& context,
                 YamlWarnings* warnings)
    {
        std::shared_ptr<SplitterSerialization> splitter = std::make_shared<SplitterSerialization>();
        MapReader reader(node, context, warnings);
        splitter->orientation = readInt(reader.get("orientation"), context + ".orientation");
        splitter->sizes = readString(reader.get("sizes"), context + ".sizes");
        YAML::Node children = reader.get("children");
        if (!children || !children.IsSequence()) {
            fail(children ? children : node, context + ".children", "expected a list of splitter sides");
        }
        for (std::size_t i = 0; i < children.size(); ++i) {
            const std::string childContext = context + ".children[" + std::to_string(i) + "]";
            MapReader childReader(children[i], childContext, warnings);
            SplitterChildSerialization child;
            if (YAML::Node nested = childReader.get("splitter")) {
                child.splitter = readSplitter(nested, childContext + ".splitter", warnings);
            } else if (YAML::Node pane = childReader.get("pane")) {
                child.pane = readPane(pane, childContext + ".pane", warnings);
            }
            childReader.finish();
            splitter->children.push_back(child);
        }
        reader.finish();

        return splitter;
    }

    void
    emitWindows(YAML::Emitter& em,
                const GuiLayoutSerialization& layout)
    {
        if (layout.windows.empty()) {
            em << YAML::Flow << YAML::BeginSeq << YAML::EndSeq;

            return;
        }
        em << YAML::BeginSeq;
        for (const ApplicationWindowSerialization& window : layout.windows) {
            em << YAML::BeginMap;
            if (window.isMainWindow) {
                emitBoolKey(em, "main", true);
            }
            emitKey(em, "pos");
            em << YAML::Value << YAML::Flow << YAML::BeginSeq;
            emitInt(em, window.positionX);
            emitInt(em, window.positionY);
            em << YAML::EndSeq;
            emitKey(em, "size");
            em << YAML::Value << YAML::Flow << YAML::BeginSeq;
            emitInt(em, window.width);
            emitInt(em, window.height);
            em << YAML::EndSeq;
            if (window.childSplitter) {
                emitKey(em, "splitter");
                em << YAML::Value;
                emitSplitter(em, *window.childSplitter);
            } else if (window.childPane) {
                emitKey(em, "pane");
                em << YAML::Value;
                emitPane(em, *window.childPane);
            } else if (!window.childDockablePanel.empty()) {
                emitKey(em, "panel");
                em << YAML::Value;
                emitString(em, window.childDockablePanel);
            }
            em << YAML::EndMap;
        }
        em << YAML::EndSeq;
    }

    GuiLayoutSerialization
    readWindows(const YAML::Node& node,
                const std::string& context,
                YamlWarnings* warnings)
    {
        GuiLayoutSerialization layout;
        if (!node.IsSequence()) {
            fail(node, context, "expected a list of windows");
        }
        for (std::size_t i = 0; i < node.size(); ++i) {
            const std::string windowContext = context + "[" + std::to_string(i) + "]";
            MapReader reader(node[i], windowContext, warnings);
            ApplicationWindowSerialization window;
            if (YAML::Node main = reader.get("main")) {
                window.isMainWindow = readBool(main, windowContext + ".main");
            }
            std::vector<int> position = readIntList(reader.get("pos"), windowContext + ".pos", 2);
            window.positionX = position[0];
            window.positionY = position[1];
            std::vector<int> size = readIntList(reader.get("size"), windowContext + ".size", 2);
            window.width = size[0];
            window.height = size[1];
            if (YAML::Node splitter = reader.get("splitter")) {
                window.childSplitter = readSplitter(splitter, windowContext + ".splitter", warnings);
            } else if (YAML::Node pane = reader.get("pane")) {
                window.childPane = readPane(pane, windowContext + ".pane", warnings);
            } else if (YAML::Node panel = reader.get("panel")) {
                window.childDockablePanel = readString(panel, windowContext + ".panel");
            }
            reader.finish();
            layout.windows.push_back(window);
        }

        return layout;
    }

    void
    checkFormat(MapReader& reader,
                std::string_view expectedFormat,
                const char* documentName)
    {
        YAML::Node format = reader.get("format");
        if (!format || !format.IsScalar() || (format.Scalar() != expectedFormat)) {
            throw YamlError(std::string(documentName) + " is not a Natron " + "layout" + ": \"format: " + std::string(expectedFormat) + "\" is missing");
        }
        const int version = readInt(reader.get("formatVersion"), std::string(documentName) + ".formatVersion");
        if (version > kLayoutYamlFormatVersion) {
            throw YamlNewerFormatError(std::string(documentName) + " was saved by a newer Natron (format version " + std::to_string(version) + "; this version reads up to " + std::to_string(kLayoutYamlFormatVersion) + ")");
        }
    }

    void
    emitFormatHeader(YAML::Emitter& em,
                     std::string_view format)
    {
        emitKey(em, "format");
        em << YAML::Value << std::string(format);
        emitKey(em, "formatVersion");
        em << YAML::Value;
        emitInt(em, kLayoutYamlFormatVersion);
    }

} // namespace

std::string
encodeGuiLayoutYaml(const GuiLayoutSerialization& layout)
{
    YAML::Emitter em;
    em << YAML::BeginMap;
    emitFormatHeader(em, kLayoutYamlFormat);
    emitKey(em, "windows");
    em << YAML::Value;
    emitWindows(em, layout);
    em << YAML::EndMap;

    return documentText(em);
}

GuiLayoutSerialization
decodeGuiLayoutYaml(std::string_view text,
                    YamlWarnings* warnings)
{
    YAML::Node document = loadDocument(text, "the layout");
    MapReader reader(document, "layout", warnings);
    checkFormat(reader, kLayoutYamlFormat, "the layout");
    GuiLayoutSerialization layout;
    if (YAML::Node windows = reader.get("windows")) {
        layout = readWindows(windows, "layout.windows", warnings);
    }
    reader.finish();

    return layout;
}

} // namespace Serialization
NATRON_NAMESPACE_EXIT
