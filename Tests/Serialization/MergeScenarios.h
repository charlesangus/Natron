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

#ifndef Tests_Serialization_MergeScenarios_h
#define Tests_Serialization_MergeScenarios_h

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "Serialization/NodeSerialization.h"
#include "Serialization/ProjectSerialization.h"

// The merge-scenario corpus of Tests/fixtures/merge-scenarios, built as plain snapshots: the
// graphs, IDs, origins and the knob edits each scenario's README row describes.
namespace MergeScenarios {

// A well-formed node ID that depends only on label.
inline std::string
hexId(const std::string& label)
{
    std::uint64_t a = 1469598103934665603ULL;
    std::uint64_t b = 1099511628211ULL ^ 0x9e3779b97f4a7c15ULL;
    for (unsigned char c : label) {
        a = (a ^ c) * 1099511628211ULL;
        b = (b ^ c) * 0x100000001b3ULL + 0x7f4a7c15ULL;
    }
    char text[33];
    std::snprintf(text, sizeof(text), "%016llx%016llx", (unsigned long long)a, (unsigned long long)b);

    return std::string(text);
}

struct SimShape {
    std::string name;
    std::vector<std::pair<double, double>> points;
    double opacity = 1.;
};

struct SimNode {
    std::string name;
    std::string plugin;
    std::string id;
    std::string parent;
    std::map<std::string, std::string> inputs;
    std::map<std::string, std::vector<double>> values;
    std::map<std::string, std::string> expressions;
    // Knob to the (frame, value) keys of its first dimension.
    std::map<std::string, std::map<double, double>> keys;
    // A Roto node's shapes, in its base layer.
    std::vector<SimShape> shapes;
    std::string originKey;
    std::string originNode;
};

struct SimGraph {
    std::vector<SimNode> nodes;

    SimNode& get(const std::string& name,
                 const std::string& parent = std::string())
    {
        for (SimNode& node : nodes) {
            if ((node.name == name) && (node.parent == parent)) {
                return node;
            }
        }
        throw std::runtime_error("no node " + name);
    }

    SimNode& add(const std::string& name,
                 const std::string& plugin,
                 const std::string& idLabel,
                 std::map<std::string, std::string> inputs = {},
                 const std::string& parent = std::string())
    {
        SimNode node;
        node.name = name;
        node.plugin = plugin;
        node.id = hexId(idLabel);
        node.parent = parent;
        node.inputs = std::move(inputs);
        nodes.push_back(node);

        return nodes.back();
    }

    void remove(const std::string& name)
    {
        nodes.erase(std::remove_if(nodes.begin(), nodes.end(), [&](const SimNode& node) {
                        return (node.name == name) && node.parent.empty();
                    }),
                    nodes.end());
        for (SimNode& node : nodes) {
            if (!node.parent.empty()) {
                continue;
            }
            for (std::map<std::string, std::string>::iterator it = node.inputs.begin(); it != node.inputs.end();) {
                it = (it->second == name) ? node.inputs.erase(it) : std::next(it);
            }
        }
    }

    void rename(const std::string& from,
                const std::string& to)
    {
        for (SimNode& node : nodes) {
            if (!node.parent.empty()) {
                continue;
            }
            if (node.name == from) {
                node.name = to;
            }
            for (std::pair<const std::string, std::string>& input : node.inputs) {
                if (input.second == from) {
                    input.second = to;
                }
            }
        }
    }

    void set(const std::string& name,
             const std::string& knob,
             std::vector<double> values,
             const std::string& parent = std::string())
    {
        get(name, parent).values[knob] = std::move(values);
    }

    void expression(const std::string& name,
                    const std::string& knob,
                    const std::string& text)
    {
        get(name).expressions[knob] = text;
    }

    void key(const std::string& name,
             const std::string& knob,
             std::map<double, double> keys)
    {
        get(name).keys[knob] = std::move(keys);
    }

    SimShape& shape(const std::string& node,
                    const std::string& name)
    {
        for (SimShape& shape : get(node).shapes) {
            if (shape.name == name) {
                return shape;
            }
        }
        throw std::runtime_error("no shape " + name);
    }

    void addShape(const std::string& node,
                  const std::string& name)
    {
        SimShape shape;
        shape.name = name;
        shape.points = { { 0., 0. }, { 100., 0. }, { 100., 100. }, { 0., 100. } };
        get(node).shapes.push_back(shape);
    }

    static Natron::Serialization::CurveSerialization curveOf(const std::map<double, double>& keys)
    {
        Natron::Serialization::CurveSerialization curve;
        for (const std::pair<const double, double>& entry : keys) {
            Natron::Serialization::KeyFrameSerialization key;
            key.time = entry.first;
            key.value = entry.second;
            key.interpolation = 1;
            curve.keyframes.push_back(key);
        }

        return curve;
    }

    static Natron::Serialization::RotoLayerChildSerialization bezierOf(const SimShape& shape)
    {
        using namespace Natron::Serialization;
        RotoLayerChildSerialization child;
        child.type = eRotoLayerChildTypeBezier;
        child.bezier.name = shape.name;
        child.bezier.label = shape.name;
        child.bezier.activated = true;
        child.bezier.parentLayerName = "Layer1";
        child.bezier.closed = true;
        for (const std::pair<double, double>& point : shape.points) {
            BezierCPSerialization cp;
            cp.x = point.first;
            cp.y = point.second;
            child.bezier.controlPoints.push_back(cp);
            child.bezier.featherPoints.push_back(cp);
        }
        KnobSerialization opacity;
        opacity.name = "opacity";
        opacity.typeName = "Double";
        opacity.dimension = 1;
        opacity.dimensions.resize(1);
        opacity.dimensions[0].defaultValue = KnobValueSerialization(1.);
        opacity.dimensions[0].value = KnobValueSerialization(shape.opacity);
        child.bezier.knobs.push_back(opacity);

        return child;
    }

    Natron::Serialization::ProjectSerialization project() const
    {
        using namespace Natron::Serialization;
        ProjectSerialization project;
        std::map<std::string, NodeSerializationPtr> topLevel;
        std::vector<std::pair<NodeSerializationPtr, std::string>> made;
        for (const SimNode& sim : nodes) {
            NodeSerializationPtr node = std::make_shared<NodeSerialization>();
            node->scriptName = sim.name;
            node->label = sim.name;
            node->pluginID = sim.plugin;
            node->id = sim.id;
            node->inputs = sim.inputs;
            node->origin.templateKey = sim.originKey;
            node->origin.nodeId = sim.originNode;
            std::set<std::string> knobNames;
            for (const std::pair<const std::string, std::vector<double>>& value : sim.values) {
                knobNames.insert(value.first);
            }
            for (const std::pair<const std::string, std::string>& expression : sim.expressions) {
                knobNames.insert(expression.first);
            }
            for (const std::pair<const std::string, std::map<double, double>>& keys : sim.keys) {
                knobNames.insert(keys.first);
            }
            for (const std::string& knobName : knobNames) {
                KnobSerialization knob;
                knob.name = knobName;
                knob.typeName = "Double";
                std::map<std::string, std::vector<double>>::const_iterator values = sim.values.find(knobName);
                const std::size_t count = (values == sim.values.end()) ? 1 : values->second.size();
                knob.dimension = int(count);
                knob.dimensions.resize(count);
                for (std::size_t i = 0; i < count; ++i) {
                    knob.dimensions[i].defaultValue = KnobValueSerialization(1.);
                    knob.dimensions[i].value = KnobValueSerialization((values == sim.values.end()) ? 1. : values->second[i]);
                }
                std::map<std::string, std::string>::const_iterator expression = sim.expressions.find(knobName);
                if (expression != sim.expressions.end()) {
                    knob.dimensions[0].expression = expression->second;
                }
                std::map<std::string, std::map<double, double>>::const_iterator keys = sim.keys.find(knobName);
                if (keys != sim.keys.end()) {
                    knob.dimensions[0].curve = curveOf(keys->second);
                }
                node->knobs.push_back(knob);
            }
            if (sim.plugin == "Roto") {
                node->hasRotoContext = true;
                node->rotoContext.baseLayer.name = "Layer1";
                for (const SimShape& shape : sim.shapes) {
                    node->rotoContext.baseLayer.children.push_back(bezierOf(shape));
                }
            }
            made.emplace_back(node, sim.parent);
            if (sim.parent.empty()) {
                topLevel[sim.name] = node;
            }
        }
        for (const std::pair<NodeSerializationPtr, std::string>& entry : made) {
            if (entry.second.empty()) {
                project.nodes.nodes.push_back(entry.first);
            } else {
                topLevel.at(entry.second)->children.push_back(entry.first);
            }
        }

        return project;
    }
};

typedef std::map<std::string, std::string> Names;

inline std::string
renamed(const Names& renames,
        const std::string& name)
{
    Names::const_iterator found = renames.find(name);

    return (found == renames.end()) ? name : found->second;
}

// The template: TplIn -> Grade1 -> Blur1 -> Merge1.B, Constant1 -> Merge1.A, Merge1 ->
// ColorCorrect1 -> TplOut, plus Matte -> Merge1.Mask with roto. Grade1 sets white and Blur1 sets
// size, so a node recreated at its default differs from the template.
inline SimGraph
templateGraph(const std::string& idPrefix,
              const Names& renames = Names(),
              bool roto = false)
{
    const auto n = [&](const std::string& name) {
        return renamed(renames, name);
    };
    SimGraph graph;
    graph.add(n("TplIn"), "Dot", idPrefix + "TplIn");
    graph.add(n("Grade1"), "Grade", idPrefix + "Grade1", { { "Source", n("TplIn") } }).values["white"] = { 0.9 };
    graph.add(n("Blur1"), "Blur", idPrefix + "Blur1", { { "Source", n("Grade1") } }).values["size"] = { 2., 2. };
    graph.add(n("Constant1"), "Constant", idPrefix + "Constant1");
    graph.add(n("Merge1"), "Merge", idPrefix + "Merge1", { { "B", n("Blur1") }, { "A", n("Constant1") } });
    graph.add(n("ColorCorrect1"), "ColorCorrect", idPrefix + "ColorCorrect1", { { "Source", n("Merge1") } });
    graph.add(n("TplOut"), "Dot", idPrefix + "TplOut", { { "", n("ColorCorrect1") } });
    if (roto) {
        graph.add(n("Matte"), "Roto", idPrefix + "Matte");
        graph.addShape(n("Matte"), "Garbage");
        graph.addShape(n("Matte"), "Core");
        graph.get(n("Merge1")).inputs["Mask"] = n("Matte");
    }

    return graph;
}

// Imports the template into graph as templateKey: fresh IDs, origins naming the template's IDs
// (prefix "b:"), and, with withShot, a Plate feeding TplIn and a ShotOut after TplOut.
inline void
importTemplate(SimGraph* graph,
               const std::string& idPrefix,
               const std::string& templateKey,
               const Names& renames = Names(),
               bool roto = false,
               const std::string& plate = "Plate",
               const std::string& shotOut = "ShotOut",
               bool withShot = true)
{
    const SimGraph imported = templateGraph(idPrefix, renames, roto);
    const SimGraph canonical = templateGraph("b:", Names(), roto);
    for (std::size_t i = 0; i < imported.nodes.size(); ++i) {
        SimNode node = imported.nodes[i];
        node.originKey = templateKey;
        node.originNode = canonical.nodes[i].id;
        graph->nodes.push_back(node);
    }
    if (withShot) {
        graph->add(plate, "CheckerBoard", idPrefix + plate);
        graph->get(renamed(renames, "TplIn")).inputs[""] = plate;
        graph->add(shotOut, "Dot", idPrefix + shotOut, { { "", renamed(renames, "TplOut") } });
    }
}

// Inserts a node on the input label of downstream.
inline void
insertBefore(SimGraph* graph,
             const std::string& name,
             const std::string& plugin,
             const std::string& idLabel,
             const std::string& downstream,
             const std::string& label,
             const std::string& newLabel = "Source")
{
    SimNode& down = graph->get(downstream);
    std::map<std::string, std::string> inputs;
    if (down.inputs.count(label)) {
        inputs[newLabel] = down.inputs[label];
    }
    graph->add(name, plugin, idLabel, inputs);
    graph->get(downstream).inputs[label] = name;
}

// Moves Blur1, and Merge1 with withMerge, into a new group fed by Grade1 (InB) and Constant1 (InA)
// and feeding what fed on them. The moved nodes keep their IDs.
inline void
wrapIntoGroup(SimGraph* graph,
              const std::string& group,
              const std::string& idPrefix,
              bool withMerge,
              const std::string& input = "InB")
{
    std::map<std::string, std::string> groupInputs = { { input, "Grade1" } };
    if (withMerge) {
        groupInputs["InA"] = "Constant1";
    }
    graph->add(group, "Group", idPrefix + group, groupInputs);
    graph->add(input, "Input", idPrefix + group + "." + input, {}, group);
    graph->get("Blur1").parent = group;
    graph->get("Blur1", group).inputs = { { "Source", input } };
    if (withMerge) {
        graph->add("InA", "Input", idPrefix + group + ".InA", {}, group);
        graph->get("Merge1").parent = group;
        graph->get("Merge1", group).inputs = { { "B", "Blur1" }, { "A", "InA" } };
        graph->add("LookOut", "Output", idPrefix + group + ".LookOut", { { "Source", "Merge1" } }, group);
        graph->get("ColorCorrect1").inputs["Source"] = group;
    } else {
        graph->add("LookOut", "Output", idPrefix + group + ".LookOut", { { "Source", "Blur1" } }, group);
        graph->get("Merge1").inputs["B"] = group;
    }
}

struct Scenario {
    SimGraph base;
    SimGraph mine;
    SimGraph theirs;
};

// The template imported once as grade_tpl, with no edit on either side.
inline Scenario
templateScenario(bool roto = false)
{
    Scenario scenario;
    scenario.base = templateGraph("b:", Names(), roto);
    importTemplate(&scenario.mine, "m:", "grade_tpl", Names(), roto);
    scenario.theirs = templateGraph("b:", Names(), roto);

    return scenario;
}

inline std::map<std::string, Scenario>
scenarios()
{
    std::map<std::string, Scenario> all;
    const auto make = [](bool roto = false) {
        return templateScenario(roto);
    };
    {
        Scenario s = make();
        s.mine.rename("Grade1", "KeyGrade");
        s.mine.set("KeyGrade", "multiply", { 1.5 });
        s.theirs.set("Blur1", "size", { 4., 4. });
        all["rename"] = s;
    }
    {
        Scenario s = make();
        s.mine.remove("Grade1");
        s.mine.add("Grade2", "Grade", "m:new", { { "Source", "TplIn" } }).values["multiply"] = { 2. };
        s.mine.get("Blur1").inputs["Source"] = "Grade2";
        s.theirs.set("Grade1", "gamma", { 1.2 });
        all["delete-recreate-grade"] = s;
    }
    {
        Scenario s = make();
        s.mine.remove("Grade1");
        s.mine.add("ArtistCC", "ColorCorrect", "m:new", { { "Source", "TplIn" } });
        s.mine.get("Blur1").inputs["Source"] = "ArtistCC";
        s.theirs.set("Grade1", "multiply", { 1.3 });
        all["delete-recreate-other-type"] = s;
    }
    {
        Scenario s = make();
        insertBefore(&s.mine, "Blur1_copy", "Blur", "m:copy", "ShotOut", "");
        s.mine.set("Blur1_copy", "size", { 2., 2. });
        s.theirs.set("Blur1", "size", { 5., 5. });
        all["duplicate-template-node"] = s;
    }
    {
        Scenario s = make();
        s.mine.set("Blur1", "size", { 3., 3. });
        s.theirs.remove("Blur1");
        s.theirs.add("Blur2", "Blur", "t:new", { { "Source", "Grade1" } }).values["size"] = { 6., 6. };
        s.theirs.get("Merge1").inputs["B"] = "Blur2";
        all["supervisor-replaces-node"] = s;
    }
    {
        Scenario s = make();
        insertBefore(&s.mine, "ArtistBlur", "Blur", "m:new", "Blur1", "Source");
        insertBefore(&s.theirs, "TplTransform", "Transform", "t:new", "Blur1", "Source");
        all["both-insert-input0"] = s;
    }
    {
        Scenario s = make();
        insertBefore(&s.mine, "MatteGrade", "Grade", "m:new", "Merge1", "A");
        insertBefore(&s.theirs, "MatteBlur", "Blur", "t:new", "Merge1", "A");
        all["both-insert-side-input"] = s;
    }
    {
        Scenario s = make();
        importTemplate(&s.mine, "m2:", "grade_tpl_2", { { "TplIn", "TplIn2" }, { "Grade1", "Grade2" }, { "Blur1", "Blur2" }, { "Constant1", "Constant2" }, { "Merge1", "Merge2" }, { "ColorCorrect1", "ColorCorrect2" }, { "TplOut", "TplOut2" } }, false, "Plate2", "ShotOut2");
        s.mine.set("Grade2", "multiply", { 1.4 });
        s.theirs.set("Grade1", "gamma", { 1.2 });
        all["imported-twice"] = s;
    }
    {
        Scenario s = make();
        s.mine = SimGraph();
        s.mine.add("Plate", "CheckerBoard", "m:Plate");
        s.mine.add("Grade1", "Grade", "m:own", { { "Source", "Plate" } });
        importTemplate(&s.mine, "m:", "grade_tpl", { { "Grade1", "Grade2" } }, false, "", "", false);
        s.mine.get("TplIn").inputs[""] = "Grade1";
        s.mine.add("ShotOut", "Dot", "m:ShotOut", { { "", "TplOut" } });
        s.theirs.set("Grade1", "multiply", { 1.3 });
        all["pasted-from-other-shot"] = s;
    }
    {
        Scenario s = make();
        s.mine.set("Blur1", "size", { 3., 3. });
        wrapIntoGroup(&s.theirs, "Look", "t:", true);
        all["wrap-into-group"] = s;
    }
    {
        Scenario s = make();
        s.mine.add("ArtistMatte", "Constant", "m:new");
        s.mine.get("Merge1").inputs["A"] = "ArtistMatte";
        s.theirs.add("Constant2", "Constant", "t:new");
        s.theirs.get("Merge1").inputs["A"] = "Constant2";
        all["reconnect"] = s;
    }
    {
        Scenario s = make();
        s.base.key("Grade1", "multiply", { { 1., 1. }, { 10., 2. } });
        s.mine.key("Grade1", "multiply", { { 1., 1. }, { 10., 2.5 }, { 20., 3. } });
        s.theirs.key("Grade1", "multiply", { { 1., 1.2 }, { 10., 1.8 }, { 30., 0.5 } });
        all["keyframes-both-sides"] = s;
    }
    {
        Scenario s = make(true);
        s.mine.shape("Matte", "Garbage").points[0].first = 5.;
        s.mine.addShape("Matte", "ArtistFix");
        s.theirs.shape("Matte", "Garbage").points[2].first = 120.;
        s.theirs.shape("Matte", "Core").opacity = 0.5;
        s.theirs.addShape("Matte", "Edge");
        all["roto-both-sides"] = s;
    }
    {
        Scenario s = make();
        insertBefore(&s.mine, "ArtistGrade", "Grade", "m:ag", "Merge1", "B");
        insertBefore(&s.mine, "ShotGrade", "Grade", "m:sg", "ShotOut", "");
        s.mine.add("GarbageMatte", "Constant", "m:gm");
        s.mine.get("Merge1").inputs["Mask"] = "GarbageMatte";
        s.theirs.set("ColorCorrect1", "saturation", { 0.5 });
        all["artist-adds-around-template"] = s;
    }
    {
        Scenario s = make();
        s.mine.remove("Grade1");
        s.mine.add("GradeA", "Grade", "m:a", { { "Source", "TplIn" } });
        s.mine.add("GradeB", "Grade", "m:b", { { "Source", "GradeA" } });
        s.mine.get("Blur1").inputs["Source"] = "GradeB";
        s.theirs.set("Grade1", "gamma", { 1.2 });
        all["delete-recreate-as-two"] = s;
    }
    {
        Scenario s = make();
        s.mine.remove("Grade1");
        s.mine.remove("Blur1");
        s.mine.add("Grade2", "Grade", "m:g2", { { "Source", "TplIn" } });
        s.mine.add("Blur2", "Blur", "m:b2", { { "Source", "Grade2" } });
        s.mine.get("Merge1").inputs["B"] = "Blur2";
        s.theirs.set("Grade1", "gamma", { 1.2 });
        s.theirs.set("Blur1", "size", { 5., 5. });
        all["delete-recreate-adjacent-pair"] = s;
    }
    {
        Scenario s = make();
        SimNode& pasted = s.mine.get("Grade1");
        pasted.id = hexId("m:paste");
        pasted.inputs.clear();
        s.mine.get("Blur1").inputs["Source"] = "TplIn";
        s.theirs.set("Grade1", "gamma", { 1.2 });
        all["cut-paste-template-node"] = s;
    }
    {
        Scenario s = make();
        s.mine.remove("Blur1");
        s.mine.add("Blur1_copy", "Blur", "m:copy", { { "Source", "Grade1" } }).values["size"] = { 2., 2. };
        s.mine.get("Merge1").inputs["B"] = "Blur1_copy";
        s.theirs.set("Blur1", "size", { 5., 5. });
        all["duplicate-then-delete-original"] = s;
    }
    {
        Scenario s = make();
        wrapIntoGroup(&s.mine, "ArtistLook", "m:", true);
        s.theirs.set("Blur1", "size", { 5., 5. });
        all["artist-groups-template-nodes"] = s;
    }
    {
        Scenario s = make();
        wrapIntoGroup(&s.mine, "ArtistLook", "m:", false);
        wrapIntoGroup(&s.theirs, "Look", "t:", false, "Src");
        all["both-sides-reparent"] = s;
    }
    {
        Scenario s = make();
        s.base.expression("Blur1", "size", "Grade1.multiply.getValue(0)");
        s.theirs.expression("Blur1", "size", "Grade1.multiply.getValue(0)");
        s.mine.expression("Blur1", "size", "Grade1.multiply.getValue(0)");
        s.mine.rename("Grade1", "KeyGrade");
        s.mine.expression("Blur1", "size", "KeyGrade.multiply.getValue(0)");
        s.theirs.expression("ColorCorrect1", "saturation", "Grade1.multiply.getValue(0)");
        all["rename-plus-expression"] = s;
    }

    return all;
}

// The node at path, script names joined by '.', or null.
inline Natron::Serialization::NodeSerializationPtr
nodeAt(const Natron::Serialization::ProjectSerialization& project,
       const std::string& path)
{
    const std::list<Natron::Serialization::NodeSerializationPtr>* level = &project.nodes.nodes;
    Natron::Serialization::NodeSerializationPtr found;
    std::stringstream parts(path);
    std::string part;
    while (std::getline(parts, part, '.')) {
        found.reset();
        for (const Natron::Serialization::NodeSerializationPtr& node : *level) {
            if (node->scriptName == part) {
                found = node;
                break;
            }
        }
        if (!found) {
            return found;
        }
        level = &found->children;
    }

    return found;
}

inline const Natron::Serialization::KnobSerialization*
knobOf(const Natron::Serialization::NodeSerialization& node,
       const std::string& name)
{
    for (const Natron::Serialization::KnobSerialization& knob : node.knobs) {
        if (knob.name == name) {
            return &knob;
        }
    }

    return nullptr;
}

// The knob's values, empty when the node leaves it at its default.
inline std::vector<double>
valuesOf(const Natron::Serialization::NodeSerialization& node,
         const std::string& name)
{
    std::vector<double> values;
    const Natron::Serialization::KnobSerialization* knob = knobOf(node, name);
    if (knob) {
        for (const Natron::Serialization::KnobDimensionSerialization& dimension : knob->dimensions) {
            if (!dimension.value) {
                values.push_back(1.);
            } else if (const int* integer = std::get_if<int>(&*dimension.value)) {
                // A bundle keeps no knob types, so a whole number reads back as an int.
                values.push_back(*integer);
            } else {
                values.push_back(std::get<double>(*dimension.value));
            }
        }
    }

    return values;
}

inline std::string
expressionOf(const Natron::Serialization::NodeSerialization& node,
             const std::string& name)
{
    const Natron::Serialization::KnobSerialization* knob = knobOf(node, name);

    return (knob && !knob->dimensions.empty()) ? knob->dimensions[0].expression : std::string();
}

struct ExpectedConflict {
    std::string node;
    std::string kind;
    std::string knob;
    int dimension = -1;
    std::string item;
    std::string input;
    // Space-separated in the file.
    std::vector<std::string> defaulted;

    bool operator<(const ExpectedConflict& other) const
    {
        return std::tie(node, kind, knob, dimension, item, input, defaulted) < std::tie(other.node, other.kind, other.knob, other.dimension, other.item, other.input, other.defaulted);
    }

    bool operator==(const ExpectedConflict& other) const
    {
        return std::tie(node, kind, knob, dimension, item, input, defaulted) == std::tie(other.node, other.kind, other.knob, other.dimension, other.item, other.input, other.defaulted);
    }
};

struct ExpectedMerge {
    std::string templateKey;
    // Base path to the other side's path, empty for no counterpart.
    std::map<std::string, std::string> baseMine;
    std::map<std::string, std::string> baseTheirs;
    std::set<std::string> scope;
    std::set<ExpectedConflict> conflicts;
};

inline std::string
trimmed(const std::string& text)
{
    const std::size_t first = text.find_first_not_of(' ');
    if (first == std::string::npos) {
        return std::string();
    }
    const std::size_t last = text.find_last_not_of(' ');

    return text.substr(first, last - first + 1);
}

// Reads the subset of expected/<scenario>.yaml the corpus uses: one line per mapping entry, flow
// lists for scope, and one flow map per conflict.
inline std::vector<ExpectedMerge>
readExpected(const std::string& path)
{
    std::ifstream file(path);
    std::vector<ExpectedMerge> merges;
    std::string line;
    int section = 0;
    while (std::getline(file, line)) {
        if (line.rfind("  - template: ", 0) == 0) {
            merges.emplace_back();
            merges.back().templateKey = trimmed(line.substr(14));
            section = 0;
        } else if (merges.empty()) {
            continue;
        } else if (line == "    base_mine:") {
            section = 1;
        } else if (line == "    base_theirs:") {
            section = 2;
        } else if (line.rfind("    scope: [", 0) == 0) {
            std::stringstream items(line.substr(12, line.rfind(']') - 12));
            std::string item;
            while (std::getline(items, item, ',')) {
                if (!trimmed(item).empty()) {
                    merges.back().scope.insert(trimmed(item));
                }
            }
            section = 0;
        } else if (line.rfind("    conflicts:", 0) == 0) {
            section = 3;
        } else if ((section == 3) && (line.rfind("      - {", 0) == 0)) {
            ExpectedConflict conflict;
            std::stringstream fields(line.substr(9, line.rfind('}') - 9));
            std::string field;
            while (std::getline(fields, field, ',')) {
                const std::size_t colon = field.find(':');
                const std::string key = trimmed(field.substr(0, colon));
                const std::string value = trimmed(field.substr(colon + 1));
                if (key == "node") {
                    conflict.node = value;
                } else if (key == "kind") {
                    conflict.kind = value;
                } else if (key == "knob") {
                    conflict.knob = value;
                } else if (key == "dim") {
                    conflict.dimension = std::stoi(value);
                } else if (key == "item") {
                    conflict.item = value;
                } else if (key == "input") {
                    conflict.input = value;
                } else if (key == "defaulted") {
                    std::stringstream names(value);
                    std::string name;
                    while (names >> name) {
                        conflict.defaulted.push_back(name);
                    }
                }
            }
            merges.back().conflicts.insert(conflict);
        } else if (((section == 1) || (section == 2)) && (line.rfind("      ", 0) == 0)) {
            const std::size_t colon = line.find(": ");
            const std::string key = trimmed(line.substr(0, colon));
            const std::string value = trimmed(line.substr(colon + 2));
            ((section == 1) ? merges.back().baseMine : merges.back().baseTheirs)[key] = (value == "~") ? std::string() : value;
        } else if (line.rfind("    ", 0) != 0) {
            section = 0;
        }
    }

    return merges;
}

} // namespace MergeScenarios

#endif // Tests_Serialization_MergeScenarios_h
