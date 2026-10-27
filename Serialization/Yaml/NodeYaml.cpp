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

#include <algorithm>
#include <map>

#include "Serialization/Yaml/YamlPrivate.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {
namespace YamlDetail {

    namespace {

        std::vector<NodeSerializationPtr>
        sortedByScriptName(const std::list<NodeSerializationPtr>& nodes,
                           const std::string& parentFullName)
        {
            std::vector<NodeSerializationPtr> sorted;
            sorted.reserve(nodes.size());
            for (const NodeSerializationPtr& node : nodes) {
                if (node) {
                    sorted.push_back(node);
                }
            }
            std::stable_sort(sorted.begin(), sorted.end(), [](const NodeSerializationPtr& a, const NodeSerializationPtr& b) {
                return a->scriptName < b->scriptName;
            });
            for (std::size_t i = 1; i < sorted.size(); ++i) {
                if (sorted[i]->scriptName == sorted[i - 1]->scriptName) {
                    throw YamlError("two nodes are named \"" + sorted[i]->scriptName + "\" in " + (parentFullName.empty() ? std::string("the project") : "the group " + parentFullName));
                }
            }

            return sorted;
        }

        void
        emitNode(YAML::Emitter& em,
                 const NodeSerialization& node,
                 const std::string& fullName,
                 BlobSink& sink,
                 std::set<std::string>* usedPaths,
                 bool withCacheIDs)
        {
            em << YAML::BeginMap;
            emitKey(em, "plugin");
            em << YAML::Value;
            emitString(em, node.pluginID);
            emitKey(em, "version");
            em << YAML::Value << YAML::Flow << YAML::BeginSeq;
            emitInt(em, node.pluginMajorVersion);
            emitInt(em, node.pluginMinorVersion);
            em << YAML::EndSeq;
            if (node.label != node.scriptName) {
                emitKey(em, "label");
                em << YAML::Value;
                emitString(em, node.label);
            }
            if (withCacheIDs && !node.cacheID.empty()) {
                emitKey(em, "cacheID");
                em << YAML::Value;
                emitString(em, node.cacheID);
            }
            if (!node.pythonModule.empty()) {
                emitKey(em, "python");
                em << YAML::Value << YAML::Flow << YAML::BeginMap;
                emitKey(em, "module");
                em << YAML::Value;
                emitString(em, node.pythonModule);
                emitKey(em, "version");
                em << YAML::Value;
                emitInt(em, node.pythonModuleVersion);
                em << YAML::EndMap;
            }
            if (node.layout.hasPosition) {
                emitDoublePair(em, "pos", node.layout.x, node.layout.y);
            }
            if (node.layout.hasSize) {
                emitDoublePair(em, "size", node.layout.width, node.layout.height);
            }
            if (node.layout.hasColor) {
                emitFloatTriple(em, "color", node.layout.colorR, node.layout.colorG, node.layout.colorB);
            }
            if (node.layout.hasOverlayColor) {
                emitFloatTriple(em, "overlay", node.layout.overlayR, node.layout.overlayG, node.layout.overlayB);
            }
            if (!node.knobs.empty()) {
                emitKey(em, "knobs");
                em << YAML::Value;
                emitKnobMap(em, node.knobs);
            }
            if (!node.inputs.empty()) {
                emitKey(em, "inputs");
                em << YAML::Value << YAML::Flow << YAML::BeginMap;
                for (std::map<std::string, std::string>::const_iterator it = node.inputs.begin(); it != node.inputs.end(); ++it) {
                    emitKey(em, it->first);
                    em << YAML::Value;
                    emitString(em, it->second);
                }
                em << YAML::EndMap;
            }
            if (!node.pagesOrder.empty()) {
                emitKey(em, "pages");
                em << YAML::Value;
                emitStringList(em, node.pagesOrder);
            }
            if (!node.userPages.empty()) {
                emitKey(em, "userPages");
                em << YAML::Value;
                emitUserPages(em, node.userPages);
            }
            BlobEncodeContext blobs { &sink, fullName, usedPaths };
            if (node.hasRotoContext) {
                emitKey(em, "roto");
                em << YAML::Value;
                emitRotoContext(em, node.rotoContext, blobs);
            }
            if (node.hasTrackerContext) {
                emitKey(em, "tracker");
                em << YAML::Value;
                emitTrackerContext(em, node.trackerContext, blobs);
            }
            if (!node.children.empty()) {
                emitKey(em, "children");
                em << YAML::Value;
                emitNodeMap(em, node.children, fullName, sink, usedPaths, withCacheIDs);
            }
            em << YAML::EndMap;
        }

        NodeSerializationPtr
        readNode(const std::string& scriptName,
                 const YAML::Node& node,
                 const BlobSource& source,
                 const std::string& context,
                 YamlWarnings* warnings)
        {
            NodeSerializationPtr result = std::make_shared<NodeSerialization>();
            NodeSerialization& data = *result;
            MapReader reader(node, context, warnings);
            data.scriptName = scriptName;
            data.pluginID = readString(reader.get("plugin"), context + ".plugin");
            if (data.pluginID.empty()) {
                fail(node, context, "a node needs a plugin ID");
            }
            YAML::Node version = reader.get("version");
            if (!version || !version.IsSequence() || (version.size() != 2)) {
                fail(version ? version : node, context + ".version", "expected [major, minor]");
            }
            data.pluginMajorVersion = readInt(version[0], context + ".version");
            data.pluginMinorVersion = readInt(version[1], context + ".version");
            if (YAML::Node label = reader.get("label")) {
                data.label = readString(label, context + ".label");
            } else {
                data.label = scriptName;
            }
            if (YAML::Node cacheID = reader.get("cacheID")) {
                data.cacheID = readString(cacheID, context + ".cacheID");
            }
            if (YAML::Node python = reader.get("python")) {
                MapReader pythonReader(python, context + ".python", warnings);
                data.pythonModule = readString(pythonReader.get("module"), context + ".python.module");
                data.pythonModuleVersion = readUInt(pythonReader.get("version"), context + ".python.version");
                pythonReader.finish();
            }
            if (YAML::Node pos = reader.get("pos")) {
                const std::vector<double> values = readDoubleVector(pos, context + ".pos", 2);
                data.layout.hasPosition = true;
                data.layout.x = values[0];
                data.layout.y = values[1];
            }
            if (YAML::Node size = reader.get("size")) {
                const std::vector<double> values = readDoubleVector(size, context + ".size", 2);
                data.layout.hasSize = true;
                data.layout.width = values[0];
                data.layout.height = values[1];
            }
            if (YAML::Node color = reader.get("color")) {
                const std::vector<float> values = readFloatTriple(color, context + ".color");
                data.layout.hasColor = true;
                data.layout.colorR = values[0];
                data.layout.colorG = values[1];
                data.layout.colorB = values[2];
            }
            if (YAML::Node overlay = reader.get("overlay")) {
                const std::vector<float> values = readFloatTriple(overlay, context + ".overlay");
                data.layout.hasOverlayColor = true;
                data.layout.overlayR = values[0];
                data.layout.overlayG = values[1];
                data.layout.overlayB = values[2];
            }
            if (YAML::Node knobs = reader.get("knobs")) {
                data.knobs = readKnobMap(knobs, context + ".knobs", warnings);
            }
            if (YAML::Node inputs = reader.get("inputs")) {
                MapReader inputsReader(inputs, context + ".inputs", warnings);
                for (const std::pair<std::string, YAML::Node>& entry : inputsReader.entries()) {
                    data.inputs[entry.first] = readString(entry.second, context + ".inputs." + entry.first);
                }
            }
            if (YAML::Node pages = reader.get("pages")) {
                data.pagesOrder = readStringList(pages, context + ".pages");
            }
            if (YAML::Node userPages = reader.get("userPages")) {
                data.userPages = readUserPages(userPages, context + ".userPages", warnings);
            }
            if (YAML::Node roto = reader.get("roto")) {
                data.hasRotoContext = true;
                data.rotoContext = readRotoContext(roto, source, context + ".roto", warnings);
            }
            if (YAML::Node tracker = reader.get("tracker")) {
                data.hasTrackerContext = true;
                data.trackerContext = readTrackerContext(tracker, source, context + ".tracker", warnings);
            }
            if (YAML::Node children = reader.get("children")) {
                data.children = readNodeMap(children, source, context + ".children", warnings);
            }
            reader.finish();

            return result;
        }

    } // namespace

    void
    emitNodeMap(YAML::Emitter& em,
                const std::list<NodeSerializationPtr>& nodes,
                const std::string& parentFullName,
                BlobSink& sink,
                std::set<std::string>* usedPaths,
                bool withCacheIDs)
    {
        const std::vector<NodeSerializationPtr> sorted = sortedByScriptName(nodes, parentFullName);
        if (sorted.empty()) {
            em << YAML::Flow << YAML::BeginMap << YAML::EndMap;

            return;
        }
        em << YAML::BeginMap;
        for (const NodeSerializationPtr& node : sorted) {
            emitKey(em, node->scriptName);
            em << YAML::Value;
            const std::string fullName = parentFullName.empty() ? node->scriptName : parentFullName + "." + node->scriptName;
            emitNode(em, *node, fullName, sink, usedPaths, withCacheIDs);
        }
        em << YAML::EndMap;
    }

    std::list<NodeSerializationPtr>
    readNodeMap(const YAML::Node& node,
                const BlobSource& source,
                const std::string& context,
                YamlWarnings* warnings)
    {
        std::vector<NodeSerializationPtr> nodes;
        MapReader reader(node, context, warnings);
        for (const std::pair<std::string, YAML::Node>& entry : reader.entries()) {
            if (entry.first.empty()) {
                fail(entry.second, context, "a node needs a script name");
            }
            nodes.push_back(readNode(entry.first, entry.second, source, context + "." + entry.first, warnings));
        }
        std::stable_sort(nodes.begin(), nodes.end(), [](const NodeSerializationPtr& a, const NodeSerializationPtr& b) {
            return a->scriptName < b->scriptName;
        });

        return std::list<NodeSerializationPtr>(nodes.begin(), nodes.end());
    }

} // namespace YamlDetail
} // namespace Serialization
NATRON_NAMESPACE_EXIT
