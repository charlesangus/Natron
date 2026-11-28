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

#include "MergeTool/DiffPrinter.h"

#include <map>
#include <optional>
#include <set>
#include <utility>
#include <variant>

#include "Serialization/Yaml/YamlScalars.h"

NATRON_NAMESPACE_ENTER
namespace MergeTool {

using namespace Serialization;

namespace {

    const char* const kArrow = " → ";

    struct IndexedNode {
        NodeSerializationPtr node;
        std::string path;
    };

    class NodeIndex {
    public:
        explicit NodeIndex(const ProjectSerialization& project)
        {
            add(project.nodes.nodes, std::string());
        }

        const IndexedNode* find(const std::string& id) const
        {
            std::map<std::string, IndexedNode>::const_iterator found = _byId.find(id);

            return (found == _byId.end()) ? nullptr : &found->second;
        }

        // Node IDs in a depth-first walk of the tree.
        const std::vector<std::string>& order() const
        {
            return _order;
        }

    private:
        void add(const std::list<NodeSerializationPtr>& nodes,
                 const std::string& prefix)
        {
            for (const NodeSerializationPtr& node : nodes) {
                const std::string path = prefix.empty() ? node->scriptName : prefix + "." + node->scriptName;
                if (!node->id.empty() && _byId.emplace(node->id, IndexedNode { node, path }).second) {
                    _order.push_back(node->id);
                }
                add(node->children, path);
            }
        }

        std::map<std::string, IndexedNode> _byId;
        std::vector<std::string> _order;
    };

    std::string
    quoted(const std::string& text)
    {
        const std::size_t newline = text.find('\n');
        if (newline == std::string::npos) {
            return "\"" + text + "\"";
        }
        std::size_t lines = 1;
        for (char c : text) {
            lines += (c == '\n') ? 1 : 0;
        }

        return "\"" + text.substr(0, newline) + " …\" (" + std::to_string(lines) + " lines)";
    }

    std::string
    valueText(const KnobValueSerialization& value)
    {
        if (const bool* b = std::get_if<bool>(&value)) {
            return *b ? "on" : "off";
        }
        if (const int* i = std::get_if<int>(&value)) {
            return std::to_string(*i);
        }
        if (const double* d = std::get_if<double>(&value)) {
            return formatYamlDouble(*d);
        }

        return quoted(std::get<std::string>(value));
    }

    std::string
    dimensionText(const KnobDimensionSerialization& dimension,
                  bool useDefault)
    {
        if (!useDefault) {
            if (!dimension.expression.empty()) {
                return "expression " + quoted(dimension.expression);
            }
            if (dimension.master) {
                const KnobMasterSerialization& master = *dimension.master;
                std::string text = "link to " + (master.masterNodeName.empty() ? std::string() : master.masterNodeName + ".") + master.masterKnobName;
                if (master.masterDimension >= 0) {
                    text += "[" + std::to_string(master.masterDimension) + "]";
                }

                return text;
            }
            if (dimension.linkGroup) {
                return "linked";
            }
            if (dimension.driver) {
                return "driven";
            }
            if (dimension.curve) {
                const std::size_t keys = dimension.curve->keyframes.size();

                return "animated (" + std::to_string(keys) + ((keys == 1) ? " key)" : " keys)");
            }
        }
        const std::optional<KnobValueSerialization>& value = useDefault ? dimension.defaultValue : dimension.value;

        return value ? valueText(*value) : std::string("default");
    }

    std::string
    knobStateText(const KnobSerialization& knob,
                  bool useDefault)
    {
        if (!useDefault) {
            if (knob.parametricCurves) {
                return "curves (" + std::to_string(knob.parametricCurves->size()) + ")";
            }
            if (knob.stringAnimation && !knob.stringAnimation->empty()) {
                const std::size_t keys = knob.stringAnimation->size();

                return "animated (" + std::to_string(keys) + ((keys == 1) ? " key)" : " keys)");
            }
            if (knob.choiceOptionID && !knob.choiceOptionID->empty()) {
                return quoted(*knob.choiceOptionID);
            }
        }
        std::vector<std::string> dimensions;
        for (const KnobDimensionSerialization& dimension : knob.dimensions) {
            dimensions.push_back(dimensionText(dimension, useDefault));
        }
        if (dimensions.empty()) {
            return "default";
        }
        bool folded = true;
        for (const std::string& text : dimensions) {
            folded = folded && (text == dimensions.front());
        }
        if (folded) {
            return dimensions.front();
        }
        std::string text = "(";
        for (std::size_t i = 0; i < dimensions.size(); ++i) {
            text += (i ? ", " : "") + dimensions[i];
        }

        return text + ")";
    }

    std::string
    pairText(const std::optional<std::pair<double, double>>& pair)
    {
        if (!pair) {
            return "none";
        }

        return "(" + formatYamlDouble(pair->first) + ", " + formatYamlDouble(pair->second) + ")";
    }

    std::string
    colorText(const std::optional<NodeUiColor>& color)
    {
        if (!color) {
            return "none";
        }

        return "(" + formatYamlFloat(color->r) + ", " + formatYamlFloat(color->g) + ", " + formatYamlFloat(color->b) + ")";
    }

    std::optional<std::pair<double, double>>
    positionOf(const NodeSerialization& node)
    {
        return node.layout.hasPosition ? std::make_optional(std::make_pair(node.layout.x, node.layout.y)) : std::nullopt;
    }

    std::optional<std::pair<double, double>>
    sizeOf(const NodeSerialization& node)
    {
        return node.layout.hasSize ? std::make_optional(std::make_pair(node.layout.width, node.layout.height)) : std::nullopt;
    }

    std::optional<NodeUiColor>
    colorOf(const NodeSerialization& node)
    {
        return node.layout.hasColor ? std::make_optional(NodeUiColor { node.layout.colorR, node.layout.colorG, node.layout.colorB }) : std::nullopt;
    }

    std::optional<NodeUiColor>
    overlayColorOf(const NodeSerialization& node)
    {
        return node.layout.hasOverlayColor ? std::make_optional(NodeUiColor { node.layout.overlayR, node.layout.overlayG, node.layout.overlayB }) : std::nullopt;
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

    std::string
    inputText(const std::string& label)
    {
        return label.empty() ? std::string("input") : "input " + label;
    }

    struct Section {
        char mark = '~';
        std::string title;
        std::vector<std::string> lines;
    };

    class Printer {
    public:
        Printer(const ProjectSerialization& before,
                const ProjectSerialization& after)
            : _beforeProject(before)
            , _before(before)
            , _after(after)
        {
        }

        void apply(const GraphOp& op)
        {
            if (const CreateNodeOp* create = std::get_if<CreateNodeOp>(&op)) {
                created(*create->node);
            } else if (const DeleteNodeOp* remove = std::get_if<DeleteNodeOp>(&op)) {
                if (const IndexedNode* node = _before.find(remove->nodeId)) {
                    deleted(*node->node);
                }
            } else if (const RenameOp* rename = std::get_if<RenameOp>(&op)) {
                changed(rename->nodeId);
            } else if (const SetKnobOp* knob = std::get_if<SetKnobOp>(&op)) {
                knobChanged(*knob);
            } else if (const SetInputOp* input = std::get_if<SetInputOp>(&op)) {
                inputChanged(*input);
            } else if (const SetNodeUiOp* ui = std::get_if<SetNodeUiOp>(&op)) {
                uiChanged(*ui);
            } else if (const SetUserKnobsOp* userKnobs = std::get_if<SetUserKnobsOp>(&op)) {
                add(changed(userKnobs->nodeId), "user knobs changed");
            } else if (const SetRotoDataOp* roto = std::get_if<SetRotoDataOp>(&op)) {
                add(changed(roto->nodeId), roto->roto ? "roto items changed" : "roto items removed");
            } else if (const SetTrackerDataOp* tracker = std::get_if<SetTrackerDataOp>(&op)) {
                add(changed(tracker->nodeId), tracker->tracker ? "tracks changed" : "tracks removed");
            } else if (const SetProjectDataOp* data = std::get_if<SetProjectDataOp>(&op)) {
                if (data->formats) {
                    _project.lines.push_back("formats changed");
                }
                if (data->layers) {
                    _project.lines.push_back("layers changed");
                }
                if (data->variables) {
                    _project.lines.push_back("variables changed");
                }
            }
        }

        std::string text() const
        {
            std::vector<std::string> ids = _after.order();
            std::set<std::string> listed(ids.begin(), ids.end());
            for (const std::string& id : _before.order()) {
                if (listed.insert(id).second) {
                    ids.push_back(id);
                }
            }
            std::string result;
            for (const std::string& id : ids) {
                std::map<std::string, Section>::const_iterator section = _sections.find(id);
                if (section != _sections.end()) {
                    write(section->second, &result);
                }
            }
            if (!_project.lines.empty()) {
                write(_project, &result);
            }

            return result;
        }

    private:
        static void write(const Section& section,
                          std::string* out)
        {
            *out += std::string(1, section.mark) + " " + section.title + "\n";
            for (const std::string& line : section.lines) {
                *out += "  " + line + "\n";
            }
        }

        static void add(Section* section,
                        const std::string& line)
        {
            if (section) {
                section->lines.push_back(line);
            }
        }

        void created(const NodeSerialization& node)
        {
            if (!node.id.empty()) {
                const IndexedNode* indexed = _after.find(node.id);
                Section& section = _sections[node.id];
                section.mark = '+';
                section.title = (indexed ? indexed->path : node.scriptName) + " (" + node.pluginID + ")";
            }
            for (const NodeSerializationPtr& child : node.children) {
                created(*child);
            }
        }

        void deleted(const NodeSerialization& node)
        {
            if (!node.id.empty()) {
                const IndexedNode* indexed = _before.find(node.id);
                Section& section = _sections[node.id];
                section.mark = '-';
                section.title = (indexed ? indexed->path : node.scriptName) + " (" + node.pluginID + ")";
            }
            for (const NodeSerializationPtr& child : node.children) {
                deleted(*child);
            }
        }

        // The section of a node both snapshots have, made on first use, or null for a node
        // the operations name that is in neither.
        Section* changed(const std::string& id)
        {
            std::map<std::string, Section>::iterator found = _sections.find(id);
            if (found != _sections.end()) {
                return &found->second;
            }
            const IndexedNode* before = _before.find(id);
            const IndexedNode* after = _after.find(id);
            if (!before || !after) {
                return nullptr;
            }
            Section& section = _sections[id];
            section.title = after->path;
            if (before->node->scriptName != after->node->scriptName) {
                section.title += " (renamed from " + before->node->scriptName + ")";
            }
            if (before->node->label != after->node->label) {
                section.lines.push_back("label: " + quoted(before->node->label) + kArrow + quoted(after->node->label));
            }

            return &section;
        }

        void knobChanged(const SetKnobOp& op)
        {
            const KnobSerialization* before = nullptr;
            Section* section = &_project;
            if (op.nodeId) {
                section = changed(*op.nodeId);
                const IndexedNode* node = _before.find(*op.nodeId);
                before = node ? knobNamed(node->node->knobs, op.knobName) : nullptr;
            } else {
                before = knobNamed(_beforeProject.projectKnobs, op.knobName);
            }
            const KnobSerialization* after = op.value ? &*op.value : nullptr;
            add(section, op.knobName + ": " + knobText(before, after) + kArrow + knobText(after, before));
        }

        void inputChanged(const SetInputOp& op)
        {
            const IndexedNode* after = _after.find(op.nodeId);
            const IndexedNode* source = op.sourceId.empty() ? nullptr : _after.find(op.sourceId);
            const std::string sourceName = source ? source->node->scriptName : std::string("none");
            std::map<std::string, Section>::iterator created = _sections.find(op.nodeId);
            if ((created != _sections.end()) && (created->second.mark == '+')) {
                if (source) {
                    created->second.lines.push_back(inputText(op.inputLabel) + ": " + sourceName);
                }

                return;
            }
            const IndexedNode* before = _before.find(op.nodeId);
            std::string previous = "none";
            if (before) {
                std::map<std::string, std::string>::const_iterator input = before->node->inputs.find(op.inputLabel);
                if (input != before->node->inputs.end()) {
                    previous = input->second;
                }
            }
            std::string line = inputText(op.inputLabel) + ": " + previous + kArrow + sourceName;
            if ((previous == sourceName) && source) {
                line += " (a new node)";
            }
            if (after) {
                add(changed(op.nodeId), line);
            }
        }

        void uiChanged(const SetNodeUiOp& op)
        {
            const IndexedNode* before = _before.find(op.nodeId);
            Section* section = changed(op.nodeId);
            if (!before || !section) {
                return;
            }
            const NodeSerialization& node = *before->node;
            if (op.position) {
                section->lines.push_back("position: " + pairText(positionOf(node)) + kArrow + pairText(*op.position));
            }
            if (op.size) {
                section->lines.push_back("size: " + pairText(sizeOf(node)) + kArrow + pairText(*op.size));
            }
            if (op.color) {
                section->lines.push_back("colour: " + colorText(colorOf(node)) + kArrow + colorText(*op.color));
            }
            if (op.overlayColor) {
                section->lines.push_back("overlay colour: " + colorText(overlayColorOf(node)) + kArrow + colorText(*op.overlayColor));
            }
        }

        const ProjectSerialization& _beforeProject;
        NodeIndex _before;
        NodeIndex _after;
        std::map<std::string, Section> _sections;
        Section _project { '~', "Project", {} };
    };

} // namespace

std::string
knobText(const KnobSerialization* knob,
         const KnobSerialization* other)
{
    if (knob) {
        return knobStateText(*knob, false);
    }
    if (other) {
        return knobStateText(*other, true);
    }

    return "default";
}

std::string
printDiff(const ProjectSerialization& before,
          const ProjectSerialization& after,
          const std::vector<GraphOp>& ops)
{
    Printer printer(before, after);
    for (const GraphOp& op : ops) {
        printer.apply(op);
    }

    return printer.text();
}

} // namespace MergeTool
NATRON_NAMESPACE_EXIT
