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

    void
    emitDoublePair(YAML::Emitter& em,
                   const char* key,
                   double a,
                   double b)
    {
        emitKey(em, key);
        em << YAML::Value << YAML::Flow << YAML::BeginSeq;
        emitDouble(em, a);
        emitDouble(em, b);
        em << YAML::EndSeq;
    }

    void
    emitFloatTriple(YAML::Emitter& em,
                    const char* key,
                    float r,
                    float g,
                    float b)
    {
        emitKey(em, key);
        em << YAML::Value << YAML::Flow << YAML::BeginSeq;
        emitFloat(em, r);
        emitFloat(em, g);
        emitFloat(em, b);
        em << YAML::EndSeq;
    }

    std::vector<float>
    readFloatTriple(const YAML::Node& node,
                    const std::string& context)
    {
        if (!node.IsSequence() || (node.size() != 3)) {
            fail(node, context, "expected [r, g, b]");
        }

        return { readFloat(node[0], context), readFloat(node[1], context), readFloat(node[2], context) };
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

} // namespace

namespace YamlDetail {

    void
    emitNodeGuiValue(YAML::Emitter& em,
                     const NodeGuiSerialization& node)
    {
        em << YAML::Flow << YAML::BeginMap;
        emitDoublePair(em, "pos", node.positionX, node.positionY);
        emitDoublePair(em, "size", node.width, node.height);
        if (node.previewEnabled) {
            emitBoolKey(em, "preview", true);
        }
        if (node.colorWasFound) {
            emitFloatTriple(em, "color", node.colorR, node.colorG, node.colorB);
        }
        if (node.hasUserColor) {
            emitFloatTriple(em, "userColor", node.userColorR, node.userColorG, node.userColorB);
        }
        if (node.hasOverlayColor) {
            emitKey(em, "overlay");
            em << YAML::Value << YAML::Flow << YAML::BeginSeq;
            emitDouble(em, node.overlayColorR);
            emitDouble(em, node.overlayColorG);
            emitDouble(em, node.overlayColorB);
            em << YAML::EndSeq;
        }
        if (node.selected) {
            emitBoolKey(em, "selected", true);
        }
        em << YAML::EndMap;
    }

    NodeGuiSerializationPtr
    readNodeGuiValue(const std::string& name,
                     const YAML::Node& node,
                     const std::string& context,
                     YamlWarnings* warnings)
    {
        NodeGuiSerializationPtr result = std::make_shared<NodeGuiSerialization>();
        NodeGuiSerialization& data = *result;
        data.nodeName = name;
        MapReader reader(node, context, warnings);
        std::vector<double> position = readDoubleVector(reader.get("pos"), context + ".pos", 2);
        data.positionX = position[0];
        data.positionY = position[1];
        if (YAML::Node size = reader.get("size")) {
            std::vector<double> values = readDoubleVector(size, context + ".size", 2);
            data.width = values[0];
            data.height = values[1];
        }
        if (YAML::Node preview = reader.get("preview")) {
            data.previewEnabled = readBool(preview, context + ".preview");
        }
        if (YAML::Node color = reader.get("color")) {
            std::vector<float> values = readFloatTriple(color, context + ".color");
            data.colorR = values[0];
            data.colorG = values[1];
            data.colorB = values[2];
            data.colorWasFound = true;
        }
        if (YAML::Node userColor = reader.get("userColor")) {
            std::vector<float> values = readFloatTriple(userColor, context + ".userColor");
            data.userColorR = values[0];
            data.userColorG = values[1];
            data.userColorB = values[2];
            data.hasUserColor = true;
        }
        if (YAML::Node overlay = reader.get("overlay")) {
            std::vector<double> values = readDoubleVector(overlay, context + ".overlay", 3);
            data.overlayColorR = values[0];
            data.overlayColorG = values[1];
            data.overlayColorB = values[2];
            data.hasOverlayColor = true;
        }
        if (YAML::Node selected = reader.get("selected")) {
            data.selected = readBool(selected, context + ".selected");
        }
        reader.finish();

        return result;
    }

} // namespace YamlDetail

namespace {

    using namespace YamlDetail;

    void
    emitNodeGui(YAML::Emitter& em,
                const NodeGuiSerialization& node)
    {
        emitKey(em, node.nodeName);
        em << YAML::Value;
        emitNodeGuiValue(em, node);
        for (const NodeGuiSerializationPtr& child : node.children) {
            if (child) {
                emitNodeGui(em, *child);
            }
        }
    }

    // A group's nodes are written after it under their fully qualified names; each finds its
    // group again by the name before its last dot.
    std::list<NodeGuiSerialization>
    readNodeGuis(const YAML::Node& node,
                 YamlWarnings* warnings)
    {
        MapReader reader(node, "gui.nodes", warnings);
        std::vector<std::pair<std::string, NodeGuiSerializationPtr>> nodes;
        std::map<std::string, NodeGuiSerializationPtr> byName;
        for (const std::pair<std::string, YAML::Node>& entry : reader.entries()) {
            NodeGuiSerializationPtr data = readNodeGuiValue(entry.first, entry.second, "gui.nodes." + entry.first, warnings);
            nodes.emplace_back(entry.first, data);
            byName[entry.first] = data;
        }
        std::vector<NodeGuiSerializationPtr> topLevel;
        for (const std::pair<std::string, NodeGuiSerializationPtr>& entry : nodes) {
            const std::size_t dot = entry.first.rfind('.');
            NodeGuiSerializationPtr parent;
            if (dot != std::string::npos) {
                std::map<std::string, NodeGuiSerializationPtr>::const_iterator found = byName.find(entry.first.substr(0, dot));
                if (found != byName.end()) {
                    parent = found->second;
                }
            }
            if (parent) {
                parent->children.push_back(entry.second);
            } else {
                topLevel.push_back(entry.second);
            }
        }
        std::list<NodeGuiSerialization> result;
        for (const NodeGuiSerializationPtr& node : topLevel) {
            result.push_back(*node);
        }

        return result;
    }

    void
    emitViewer(YAML::Emitter& em,
               const ViewerDataSerialization& viewer)
    {
        em << YAML::BeginMap;
        emitKey(em, "zoom");
        em << YAML::Value << YAML::Flow << YAML::BeginSeq;
        emitDouble(em, viewer.zoomLeft);
        emitDouble(em, viewer.zoomBottom);
        emitDouble(em, viewer.zoomFactor);
        em << YAML::EndSeq;
        emitBoolKey(em, "zoomedSinceFit", viewer.zoomOrPanSinceLastFit);
        emitKey(em, "roi");
        em << YAML::Value << YAML::Flow << YAML::BeginMap;
        emitBoolKey(em, "enabled", viewer.userRoIEnabled);
        emitKey(em, "rect");
        em << YAML::Value << YAML::Flow << YAML::BeginSeq;
        emitDouble(em, viewer.userRoI.x1);
        emitDouble(em, viewer.userRoI.y1);
        emitDouble(em, viewer.userRoI.x2);
        emitDouble(em, viewer.userRoI.y2);
        em << YAML::EndSeq;
        em << YAML::EndMap;
        emitBoolKey(em, "clipToProject", viewer.isClippedToProject);
        emitBoolKey(em, "fullFrame", viewer.isFullFrameProcessEnabled);
        emitBoolKey(em, "autoContrast", viewer.autoContrastEnabled);
        emitKey(em, "gain");
        em << YAML::Value;
        emitDouble(em, viewer.gain);
        emitKey(em, "gamma");
        em << YAML::Value;
        emitDouble(em, viewer.gamma);
        emitBoolKey(em, "renderScale", viewer.renderScaleActivated);
        emitKey(em, "mipmapLevel");
        em << YAML::Value;
        emitInt(em, viewer.mipmapLevel);
        const std::pair<const char*, const std::string*> strings[] = {
            { "display", &viewer.display }, { "view", &viewer.view }, { "look", &viewer.look }, { "channels", &viewer.channels }, { "layer", &viewer.layerName }, { "alphaLayer", &viewer.alphaLayerName }
        };
        for (const std::pair<const char*, const std::string*>& entry : strings) {
            emitKey(em, entry.first);
            em << YAML::Value;
            emitString(em, *entry.second);
        }
        emitKey(em, "wipeOperator");
        em << YAML::Value;
        emitInt(em, viewer.wipeCompositingOperator);
        emitKey(em, "toolbars");
        em << YAML::Value << YAML::Flow << YAML::BeginMap;
        emitBoolKey(em, "left", viewer.leftToolbarVisible);
        emitBoolKey(em, "right", viewer.rightToolbarVisible);
        emitBoolKey(em, "top", viewer.topToolbarVisible);
        emitBoolKey(em, "player", viewer.playerVisible);
        emitBoolKey(em, "infobar", viewer.infobarVisible);
        emitBoolKey(em, "timeline", viewer.timelineVisible);
        em << YAML::EndMap;
        emitBoolKey(em, "checkerboard", viewer.checkerboardEnabled);
        emitKey(em, "paused");
        em << YAML::Value << YAML::Flow << YAML::BeginSeq;
        emitBool(em, viewer.isInputAPaused);
        emitBool(em, viewer.isInputBPaused);
        em << YAML::EndSeq;
        emitKey(em, "fps");
        em << YAML::Value;
        emitDouble(em, viewer.fps);
        emitBoolKey(em, "fpsLocked", viewer.fpsLocked);
        emitKey(em, "range");
        em << YAML::Value << YAML::Flow << YAML::BeginSeq;
        emitInt(em, viewer.leftBound);
        emitInt(em, viewer.rightBound);
        em << YAML::EndSeq;
        emitKey(em, "inputs");
        em << YAML::Value << YAML::Flow << YAML::BeginSeq;
        emitInt(em, viewer.activeInputA);
        emitInt(em, viewer.activeInputB);
        em << YAML::EndSeq;
        em << YAML::EndMap;
    }

    ViewerDataSerialization
    readViewer(const YAML::Node& node,
               const std::string& context,
               YamlWarnings* warnings)
    {
        ViewerDataSerialization viewer;
        MapReader reader(node, context, warnings);
        if (YAML::Node zoom = reader.get("zoom")) {
            std::vector<double> values = readDoubleVector(zoom, context + ".zoom", 3);
            viewer.zoomLeft = values[0];
            viewer.zoomBottom = values[1];
            viewer.zoomFactor = values[2];
        }
        if (YAML::Node zoomed = reader.get("zoomedSinceFit")) {
            viewer.zoomOrPanSinceLastFit = readBool(zoomed, context + ".zoomedSinceFit");
        }
        if (YAML::Node roi = reader.get("roi")) {
            MapReader roiReader(roi, context + ".roi", warnings);
            viewer.userRoIEnabled = readBool(roiReader.get("enabled"), context + ".roi.enabled");
            std::vector<double> rect = readDoubleVector(roiReader.get("rect"), context + ".roi.rect", 4);
            viewer.userRoI.x1 = rect[0];
            viewer.userRoI.y1 = rect[1];
            viewer.userRoI.x2 = rect[2];
            viewer.userRoI.y2 = rect[3];
            roiReader.finish();
        }
        const std::pair<const char*, bool*> flags[] = {
            { "clipToProject", &viewer.isClippedToProject }, { "fullFrame", &viewer.isFullFrameProcessEnabled }, { "autoContrast", &viewer.autoContrastEnabled }, { "renderScale", &viewer.renderScaleActivated }, { "checkerboard", &viewer.checkerboardEnabled }, { "fpsLocked", &viewer.fpsLocked }
        };
        for (const std::pair<const char*, bool*>& entry : flags) {
            if (YAML::Node value = reader.get(entry.first)) {
                *entry.second = readBool(value, context + "." + entry.first);
            }
        }
        if (YAML::Node gain = reader.get("gain")) {
            viewer.gain = readDouble(gain, context + ".gain");
        }
        if (YAML::Node gamma = reader.get("gamma")) {
            viewer.gamma = readDouble(gamma, context + ".gamma");
        }
        if (YAML::Node mipmapLevel = reader.get("mipmapLevel")) {
            viewer.mipmapLevel = readUInt(mipmapLevel, context + ".mipmapLevel");
        }
        const std::pair<const char*, std::string*> strings[] = {
            { "display", &viewer.display }, { "view", &viewer.view }, { "look", &viewer.look }, { "channels", &viewer.channels }, { "layer", &viewer.layerName }, { "alphaLayer", &viewer.alphaLayerName }
        };
        for (const std::pair<const char*, std::string*>& entry : strings) {
            if (YAML::Node value = reader.get(entry.first)) {
                *entry.second = readString(value, context + "." + entry.first);
            }
        }
        if (YAML::Node wipe = reader.get("wipeOperator")) {
            viewer.wipeCompositingOperator = readInt(wipe, context + ".wipeOperator");
        }
        if (YAML::Node toolbars = reader.get("toolbars")) {
            MapReader toolbarReader(toolbars, context + ".toolbars", warnings);
            const std::pair<const char*, bool*> visible[] = {
                { "left", &viewer.leftToolbarVisible }, { "right", &viewer.rightToolbarVisible }, { "top", &viewer.topToolbarVisible }, { "player", &viewer.playerVisible }, { "infobar", &viewer.infobarVisible }, { "timeline", &viewer.timelineVisible }
            };
            for (const std::pair<const char*, bool*>& entry : visible) {
                if (YAML::Node value = toolbarReader.get(entry.first)) {
                    *entry.second = readBool(value, context + ".toolbars." + entry.first);
                }
            }
            toolbarReader.finish();
        }
        if (YAML::Node paused = reader.get("paused")) {
            if (!paused.IsSequence() || (paused.size() != 2)) {
                fail(paused, context + ".paused", "expected [a, b]");
            }
            viewer.isInputAPaused = readBool(paused[0], context + ".paused");
            viewer.isInputBPaused = readBool(paused[1], context + ".paused");
        }
        if (YAML::Node fps = reader.get("fps")) {
            viewer.fps = readDouble(fps, context + ".fps");
        }
        if (YAML::Node range = reader.get("range")) {
            std::vector<int> values = readIntList(range, context + ".range", 2);
            viewer.leftBound = values[0];
            viewer.rightBound = values[1];
        }
        if (YAML::Node inputs = reader.get("inputs")) {
            std::vector<int> values = readIntList(inputs, context + ".inputs", 2);
            viewer.activeInputA = values[0];
            viewer.activeInputB = values[1];
        }
        reader.finish();

        return viewer;
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
            throw YamlError(std::string(documentName) + " is not a Natron " + (expectedFormat == kGuiYamlFormat ? "GUI state" : "layout") + ": \"format: " + std::string(expectedFormat) + "\" is missing");
        }
        const int version = readInt(reader.get("formatVersion"), std::string(documentName) + ".formatVersion");
        if (version > kGuiYamlFormatVersion) {
            throw YamlNewerFormatError(std::string(documentName) + " was saved by a newer Natron (format version " + std::to_string(version) + "; this version reads up to " + std::to_string(kGuiYamlFormatVersion) + ")");
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
        emitInt(em, kGuiYamlFormatVersion);
    }

} // namespace

std::string
encodeProjectGuiYaml(const ProjectGuiSerialization& gui)
{
    YAML::Emitter em;
    em << YAML::BeginMap;
    emitFormatHeader(em, kGuiYamlFormat);
    if (!gui.nodes.empty()) {
        emitKey(em, "nodes");
        em << YAML::Value << YAML::BeginMap;
        for (const NodeGuiSerialization& node : gui.nodes) {
            emitNodeGui(em, node);
        }
        em << YAML::EndMap;
    }
    if (!gui.viewers.empty()) {
        emitKey(em, "viewers");
        em << YAML::Value << YAML::BeginMap;
        for (std::map<std::string, ViewerDataSerialization>::const_iterator it = gui.viewers.begin(); it != gui.viewers.end(); ++it) {
            emitKey(em, it->first);
            em << YAML::Value;
            emitViewer(em, it->second);
        }
        em << YAML::EndMap;
    }
    if (!gui.histograms.empty()) {
        emitKey(em, "histograms");
        em << YAML::Value;
        emitStringList(em, gui.histograms);
    }
    if (!gui.openedPanels.empty() || !gui.openedPanelsMinimized.empty() || !gui.openedPanelsHideUnmodified.empty()) {
        emitKey(em, "panels");
        em << YAML::Value << YAML::BeginMap;
        emitKey(em, "opened");
        em << YAML::Value;
        emitStringList(em, gui.openedPanels);
        emitKey(em, "minimized");
        em << YAML::Value;
        emitStringList(em, gui.openedPanelsMinimized);
        emitKey(em, "hideUnmodified");
        em << YAML::Value;
        emitStringList(em, gui.openedPanelsHideUnmodified);
        em << YAML::EndMap;
    }
    if (!gui.scriptEditorInput.empty()) {
        emitKey(em, "scriptEditor");
        em << YAML::Value;
        emitString(em, gui.scriptEditorInput);
    }
    if (!gui.pythonPanels.empty()) {
        emitKey(em, "pythonPanels");
        em << YAML::Value << YAML::BeginSeq;
        for (const PythonPanelSerialization& panel : gui.pythonPanels) {
            em << YAML::BeginMap;
            emitKey(em, "name");
            em << YAML::Value;
            emitString(em, panel.name);
            emitKey(em, "function");
            em << YAML::Value;
            emitString(em, panel.pythonFunction);
            if (!panel.knobs.empty()) {
                emitKey(em, "knobs");
                em << YAML::Value;
                emitKnobMap(em, panel.knobs);
            }
            emitKey(em, "userData");
            em << YAML::Value;
            emitString(em, panel.userData);
            em << YAML::EndMap;
        }
        em << YAML::EndSeq;
    }
    emitKey(em, "layout");
    em << YAML::Value;
    emitWindows(em, gui.layout);
    em << YAML::EndMap;

    return documentText(em);
}

ProjectGuiSerialization
decodeProjectGuiYaml(std::string_view text,
                     YamlWarnings* warnings)
{
    YAML::Node document = loadDocument(text, "gui.yaml");
    MapReader reader(document, "gui", warnings);
    checkFormat(reader, kGuiYamlFormat, "gui.yaml");

    ProjectGuiSerialization gui;
    if (YAML::Node nodes = reader.get("nodes")) {
        gui.nodes = readNodeGuis(nodes, warnings);
    }
    if (YAML::Node viewers = reader.get("viewers")) {
        MapReader viewersReader(viewers, "gui.viewers", warnings);
        for (const std::pair<std::string, YAML::Node>& entry : viewersReader.entries()) {
            gui.viewers[entry.first] = readViewer(entry.second, "gui.viewers." + entry.first, warnings);
        }
    }
    if (YAML::Node histograms = reader.get("histograms")) {
        gui.histograms = readStringList(histograms, "gui.histograms");
    }
    if (YAML::Node panels = reader.get("panels")) {
        MapReader panelsReader(panels, "gui.panels", warnings);
        if (YAML::Node opened = panelsReader.get("opened")) {
            gui.openedPanels = readStringList(opened, "gui.panels.opened");
        }
        if (YAML::Node minimized = panelsReader.get("minimized")) {
            gui.openedPanelsMinimized = readStringList(minimized, "gui.panels.minimized");
        }
        if (YAML::Node hideUnmodified = panelsReader.get("hideUnmodified")) {
            gui.openedPanelsHideUnmodified = readStringList(hideUnmodified, "gui.panels.hideUnmodified");
        }
        panelsReader.finish();
    }
    if (YAML::Node scriptEditor = reader.get("scriptEditor")) {
        gui.scriptEditorInput = readString(scriptEditor, "gui.scriptEditor");
    }
    if (YAML::Node pythonPanels = reader.get("pythonPanels")) {
        if (!pythonPanels.IsSequence()) {
            fail(pythonPanels, "gui.pythonPanels", "expected a list of Python panels");
        }
        for (std::size_t i = 0; i < pythonPanels.size(); ++i) {
            const std::string context = "gui.pythonPanels[" + std::to_string(i) + "]";
            MapReader panelReader(pythonPanels[i], context, warnings);
            PythonPanelSerialization panel;
            panel.name = readString(panelReader.get("name"), context + ".name");
            panel.pythonFunction = readString(panelReader.get("function"), context + ".function");
            if (YAML::Node knobs = panelReader.get("knobs")) {
                panel.knobs = readKnobMap(knobs, context + ".knobs", warnings);
            }
            if (YAML::Node userData = panelReader.get("userData")) {
                panel.userData = readString(userData, context + ".userData");
            }
            panelReader.finish();
            gui.pythonPanels.push_back(panel);
        }
    }
    if (YAML::Node layout = reader.get("layout")) {
        gui.layout = readWindows(layout, "gui.layout", warnings);
    }
    reader.finish();

    return gui;
}

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
