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

#include "Serialization/Yaml/ProjectYaml.h"

#include <algorithm>
#include <vector>

#include "Serialization/Yaml/YamlPrivate.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

namespace {

    void
    emitSavedBy(YAML::Emitter& em,
                const ProjectSavedByVersionSerialization& savedBy)
    {
        using namespace YamlDetail;
        em << YAML::Flow << YAML::BeginMap;
        emitKey(em, "version");
        em << YAML::Value << YAML::Flow << YAML::BeginSeq;
        emitInt(em, savedBy.versionMajor);
        emitInt(em, savedBy.versionMinor);
        emitInt(em, savedBy.versionRevision);
        em << YAML::EndSeq;
        emitKey(em, "branch");
        em << YAML::Value;
        emitString(em, savedBy.gitBranch);
        emitKey(em, "commit");
        em << YAML::Value;
        emitString(em, savedBy.gitCommit);
        emitKey(em, "os");
        em << YAML::Value;
        emitString(em, savedBy.operatingSystem);
        emitKey(em, "bits");
        em << YAML::Value;
        emitInt(em, savedBy.bits);
        em << YAML::EndMap;
    }

    ProjectSavedByVersionSerialization
    readSavedBy(const YAML::Node& node,
                YamlWarnings* warnings)
    {
        using namespace YamlDetail;
        const std::string context = "project.savedBy";
        ProjectSavedByVersionSerialization savedBy;
        MapReader reader(node, context, warnings);
        YAML::Node version = reader.get("version");
        if (!version || !version.IsSequence() || (version.size() != 3)) {
            fail(version ? version : node, context + ".version", "expected [major, minor, revision]");
        }
        savedBy.versionMajor = readInt(version[0], context + ".version");
        savedBy.versionMinor = readInt(version[1], context + ".version");
        savedBy.versionRevision = readInt(version[2], context + ".version");
        savedBy.gitBranch = readString(reader.get("branch"), context + ".branch");
        savedBy.gitCommit = readString(reader.get("commit"), context + ".commit");
        savedBy.operatingSystem = readString(reader.get("os"), context + ".os");
        savedBy.bits = readInt(reader.get("bits"), context + ".bits");
        reader.finish();

        return savedBy;
    }

    void
    emitFormats(YAML::Emitter& em,
                const std::list<FormatSerialization>& formats)
    {
        using namespace YamlDetail;
        em << YAML::BeginSeq;
        for (const FormatSerialization& format : formats) {
            em << YAML::Flow << YAML::BeginMap;
            emitKey(em, "name");
            em << YAML::Value;
            emitString(em, format.name);
            emitKey(em, "rect");
            em << YAML::Value << YAML::Flow << YAML::BeginSeq;
            emitInt(em, format.x1);
            emitInt(em, format.y1);
            emitInt(em, format.x2);
            emitInt(em, format.y2);
            em << YAML::EndSeq;
            emitKey(em, "par");
            em << YAML::Value;
            emitDouble(em, format.pixelAspectRatio);
            em << YAML::EndMap;
        }
        em << YAML::EndSeq;
    }

    std::list<FormatSerialization>
    readFormats(const YAML::Node& node,
                YamlWarnings* warnings)
    {
        using namespace YamlDetail;
        std::list<FormatSerialization> formats;
        if (!node.IsSequence()) {
            fail(node, "project.formats", "expected a list of formats");
        }
        for (std::size_t i = 0; i < node.size(); ++i) {
            const std::string context = "project.formats[" + std::to_string(i) + "]";
            MapReader reader(node[i], context, warnings);
            FormatSerialization format;
            format.name = readString(reader.get("name"), context + ".name");
            YAML::Node rect = reader.get("rect");
            if (!rect || !rect.IsSequence() || (rect.size() != 4)) {
                fail(rect ? rect : node[i], context + ".rect", "expected [x1, y1, x2, y2]");
            }
            format.x1 = readInt(rect[0], context + ".rect");
            format.y1 = readInt(rect[1], context + ".rect");
            format.x2 = readInt(rect[2], context + ".rect");
            format.y2 = readInt(rect[3], context + ".rect");
            format.pixelAspectRatio = readDouble(reader.get("par"), context + ".par");
            reader.finish();
            formats.push_back(format);
        }

        return formats;
    }

    void
    emitLayers(YAML::Emitter& em,
               const std::list<LayerRegistryEntrySerialization>& layers)
    {
        using namespace YamlDetail;
        std::vector<const LayerRegistryEntrySerialization*> sorted;
        for (const LayerRegistryEntrySerialization& entry : layers) {
            sorted.push_back(&entry);
        }
        std::stable_sort(sorted.begin(), sorted.end(), [](const LayerRegistryEntrySerialization* a, const LayerRegistryEntrySerialization* b) {
            return a->layer.layerID < b->layer.layerID;
        });
        em << YAML::BeginMap;
        for (std::size_t i = 0; i < sorted.size(); ++i) {
            const LayerRegistryEntrySerialization& entry = *sorted[i];
            if ((i > 0) && (sorted[i - 1]->layer.layerID == entry.layer.layerID)) {
                throw YamlError("the layer registry holds two layers with the ID \"" + entry.layer.layerID + "\"");
            }
            emitKey(em, entry.layer.layerID);
            em << YAML::Value << YAML::Flow << YAML::BeginMap;
            emitKey(em, "label");
            em << YAML::Value;
            emitString(em, entry.layer.layerLabel);
            emitKey(em, "channelsLabel");
            em << YAML::Value;
            emitString(em, entry.layer.channelsLabel);
            emitKey(em, "channels");
            em << YAML::Value;
            emitStringList(em, entry.layer.channels);
            emitKey(em, "origin");
            em << YAML::Value;
            emitInt(em, entry.origin);
            em << YAML::EndMap;
        }
        em << YAML::EndMap;
    }

    std::list<LayerRegistryEntrySerialization>
    readLayers(const YAML::Node& node,
               YamlWarnings* warnings)
    {
        using namespace YamlDetail;
        std::vector<LayerRegistryEntrySerialization> layers;
        MapReader reader(node, "project.layers", warnings);
        for (const std::pair<std::string, YAML::Node>& entry : reader.entries()) {
            const std::string context = "project.layers." + entry.first;
            MapReader layerReader(entry.second, context, warnings);
            LayerRegistryEntrySerialization layer;
            layer.layer.layerID = entry.first;
            layer.layer.layerLabel = readString(layerReader.get("label"), context + ".label");
            layer.layer.channelsLabel = readString(layerReader.get("channelsLabel"), context + ".channelsLabel");
            layer.layer.channels = readStringVector(layerReader.get("channels"), context + ".channels");
            layer.origin = readInt(layerReader.get("origin"), context + ".origin");
            layerReader.finish();
            layers.push_back(layer);
        }
        std::stable_sort(layers.begin(), layers.end(), [](const LayerRegistryEntrySerialization& a, const LayerRegistryEntrySerialization& b) {
            return a.layer.layerID < b.layer.layerID;
        });

        return std::list<LayerRegistryEntrySerialization>(layers.begin(), layers.end());
    }

} // namespace

std::string
encodeProjectYaml(const ProjectSerialization& project,
                  BlobSink& sink)
{
    using namespace YamlDetail;
    YAML::Emitter em;
    std::set<std::string> usedPaths;
    em << YAML::BeginMap;
    emitKey(em, "format");
    em << YAML::Value << std::string(kProjectYamlFormat);
    emitKey(em, "formatVersion");
    em << YAML::Value;
    emitInt(em, kProjectYamlFormatVersion);
    emitKey(em, "savedBy");
    em << YAML::Value;
    emitSavedBy(em, project.savedByVersion);
    if (!project.projectKnobs.empty()) {
        emitKey(em, "knobs");
        em << YAML::Value;
        emitKnobMap(em, project.projectKnobs);
    }
    if (!project.additionalFormats.empty()) {
        emitKey(em, "formats");
        em << YAML::Value;
        emitFormats(em, project.additionalFormats);
    }
    if (!project.layers.empty()) {
        emitKey(em, "layers");
        em << YAML::Value;
        emitLayers(em, project.layers);
    }
    emitKey(em, "timeline");
    em << YAML::Value << YAML::Flow << YAML::BeginMap;
    emitKey(em, "current");
    em << YAML::Value;
    emitInt(em, project.timelineCurrentTime);
    em << YAML::EndMap;
    emitKey(em, "nodes");
    em << YAML::Value;
    emitNodeMap(em, project.nodes.nodes, std::string(), sink, &usedPaths);
    em << YAML::EndMap;

    return documentText(em);
}

ProjectSerialization
decodeProjectYaml(std::string_view text,
                  const BlobSource& source,
                  YamlWarnings* warnings)
{
    using namespace YamlDetail;
    YAML::Node document = loadDocument(text, "project.yaml");
    if (!document.IsMap()) {
        throw YamlError("project.yaml is not a Natron project: it is not a YAML map");
    }
    MapReader reader(document, "project", warnings);
    YAML::Node format = reader.get("format");
    if (!format || !format.IsScalar() || (format.Scalar() != kProjectYamlFormat)) {
        throw YamlError("project.yaml is not a Natron project: \"format: " + std::string(kProjectYamlFormat) + "\" is missing");
    }
    const int formatVersion = readInt(reader.get("formatVersion"), "project.formatVersion");
    if (formatVersion > kProjectYamlFormatVersion) {
        throw YamlError("This project was saved by a newer Natron (project format version " + std::to_string(formatVersion) + "; this version reads up to " + std::to_string(kProjectYamlFormatVersion) + ")");
    }
    if (formatVersion < 1) {
        fail(reader.get("formatVersion"), "project.formatVersion", "invalid format version " + std::to_string(formatVersion));
    }

    ProjectSerialization project;
    if (YAML::Node savedBy = reader.get("savedBy")) {
        project.savedByVersion = readSavedBy(savedBy, warnings);
    }
    if (YAML::Node knobs = reader.get("knobs")) {
        project.projectKnobs = readKnobMap(knobs, "project.knobs", warnings);
    }
    if (YAML::Node formats = reader.get("formats")) {
        project.additionalFormats = readFormats(formats, warnings);
    }
    if (YAML::Node layers = reader.get("layers")) {
        project.layers = readLayers(layers, warnings);
    }
    if (YAML::Node timeline = reader.get("timeline")) {
        MapReader timelineReader(timeline, "project.timeline", warnings);
        if (YAML::Node current = timelineReader.get("current")) {
            project.timelineCurrentTime = readInt(current, "project.timeline.current");
        }
        timelineReader.finish();
    }
    if (YAML::Node nodes = reader.get("nodes")) {
        project.nodes.nodes = readNodeMap(nodes, source, "nodes", warnings);
    }
    reader.finish();

    return project;
}

} // namespace Serialization
NATRON_NAMESPACE_EXIT
