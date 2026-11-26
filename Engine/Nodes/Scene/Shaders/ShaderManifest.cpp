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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "ShaderManifest.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>

#include <QDebug>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>

NATRON_NAMESPACE_ENTER

NATRON_NAMESPACE_ANONYMOUS_ENTER

const char* const kRendererCycles = "cycles";
const char* const kRendererMoonRay = "moonray";

// Attributes no group lists go on this page, or on kControlsPage when the node has no groups.
const char* const kOtherPage = "Other";
const char* const kControlsPage = "Controls";

const std::map<std::string, PortTypeClass>&
cyclesTypeTable()
{
    static const std::map<std::string, PortTypeClass> table = {
        { "float", PortTypeClass::Float },
        { "int", PortTypeClass::Int },
        { "boolean", PortTypeClass::Int },
        { "enum", PortTypeClass::Int },
        { "vector", PortTypeClass::Vector3 },
        { "point", PortTypeClass::Point },
        { "normal", PortTypeClass::Normal },
        { "color", PortTypeClass::Color },
        { "string", PortTypeClass::String },
        { "closure", PortTypeClass::Closure },
        { "array_float", PortTypeClass::Float },
        { "array_color", PortTypeClass::Color },
        { "array_vector", PortTypeClass::Vector3 },
        { "array_int", PortTypeClass::Int },
        { "transform", PortTypeClass::Custom },
    };

    return table;
}

// An object attribute or output's type is its interface, when it has one.
const std::map<std::string, PortTypeClass>&
moonRayTypeTable()
{
    static const std::map<std::string, PortTypeClass> table = {
        { "Float", PortTypeClass::Float },
        { "Int", PortTypeClass::Int },
        { "Bool", PortTypeClass::Int },
        { "Rgb", PortTypeClass::Color },
        { "Vec3f", PortTypeClass::Vector3 },
        { "Vec3d", PortTypeClass::Vector3 },
        { "String", PortTypeClass::String },
        { "Vec2f", PortTypeClass::Custom },
        { "Mat4d", PortTypeClass::Custom },
        { "FloatVector", PortTypeClass::Custom },
        { "IntVector", PortTypeClass::Custom },
        { "RgbVector", PortTypeClass::Custom },
        { "Map", PortTypeClass::Texture },
        { "Material", PortTypeClass::Closure },
        { "DwaBase", PortTypeClass::Closure },
        { "DwaBaseLayerable", PortTypeClass::Closure },
        { "DwaBaseHairLayerable", PortTypeClass::Closure },
        { "Displacement", PortTypeClass::Displacement },
    };

    return table;
}

PortTypeClass
typeClassOf(const std::map<std::string, PortTypeClass>& table,
            const std::string& type)
{
    const std::map<std::string, PortTypeClass>::const_iterator it = table.find(type);

    return (it == table.end()) ? PortTypeClass::Custom : it->second;
}

const std::map<std::string, std::string>&
acronyms()
{
    static const std::map<std::string, std::string> words = {
        { "ao", "AO" },
        { "aov", "AOV" },
        { "aovs", "AOVs" },
        { "brdf", "BRDF" },
        { "bsdf", "BSDF" },
        { "bssrdf", "BSSRDF" },
        { "cpu", "CPU" },
        { "ggx", "GGX" },
        { "gpu", "GPU" },
        { "hdr", "HDR" },
        { "hdri", "HDRI" },
        { "hsl", "HSL" },
        { "hsv", "HSV" },
        { "id", "ID" },
        { "ids", "IDs" },
        { "ies", "IES" },
        { "ior", "IOR" },
        { "lod", "LOD" },
        { "ndf", "NDF" },
        { "osl", "OSL" },
        { "rgb", "RGB" },
        { "rgba", "RGBA" },
        { "srgb", "sRGB" },
        { "sss", "SSS" },
        { "udim", "UDIM" },
        { "uv", "UV" },
        { "uvw", "UVW" },
        { "xyz", "XYZ" },
    };

    return words;
}

std::string
toStd(const QString& text)
{
    return text.toStdString();
}

std::string
toStd(const QJsonValue& value)
{
    return value.toString().toStdString();
}

std::string
formatNumber(double value)
{
    std::ostringstream out;

    out << value;

    return out.str();
}

// The values of a number, a bool or a (nested) array of them, flattened.
std::vector<double>
numbers(const QJsonValue& value)
{
    std::vector<double> ret;

    if (value.isArray()) {
        for (const QJsonValue& item : value.toArray()) {
            const std::vector<double> inner = numbers(item);
            ret.insert(ret.end(), inner.begin(), inner.end());
        }
    } else if (value.isBool()) {
        ret.push_back(value.toBool() ? 1. : 0.);
    } else if (value.isDouble()) {
        ret.push_back(value.toDouble());
    }

    return ret;
}

// One row per line: each element of the array, its values space separated.
std::string
tableText(const QJsonValue& value)
{
    std::string ret;

    for (const QJsonValue& row : value.toArray()) {
        std::string line;
        for (double number : numbers(row)) {
            if (!line.empty()) {
                line += ' ';
            }
            line += formatNumber(number);
        }
        if (!ret.empty()) {
            ret += '\n';
        }
        ret += line;
    }

    return ret;
}

// A default as a tooltip shows it: an enum's name, a string, on or off, or its numbers.
std::string
defaultDescription(const QJsonValue& value)
{
    if (value.isString()) {
        return toStd(value);
    }
    if (value.isBool()) {
        return value.toBool() ? "on" : "off";
    }
    std::string ret;
    for (double number : numbers(value)) {
        if (!ret.empty()) {
            ret += ' ';
        }
        ret += formatNumber(number);
    }

    return ret;
}

// An array default as curves: its elements, each a number or one per dimension, spaced evenly
// over [0, 1]. An empty array leaves the identity curves.
std::vector<std::vector<NetworkCurvePoint>>
sampledCurves(const QJsonValue& value,
              int dimensions)
{
    const QJsonArray samples = value.toArray();
    std::vector<std::vector<NetworkCurvePoint>> ret;

    if (samples.isEmpty()) {
        return ret;
    }
    ret.resize(dimensions);
    for (int i = 0; i < samples.size(); ++i) {
        std::vector<double> values = numbers(samples[i]);
        values.resize(dimensions, values.empty() ? 0. : values.back());
        for (int d = 0; d < dimensions; ++d) {
            NetworkCurvePoint point;
            point.x = (samples.size() == 1) ? 0. : (double)i / (samples.size() - 1);
            point.y = values[d];
            ret[d].push_back(point);
        }
    }

    return ret;
}

// A ramp's interpolation code as a control point's. The two exponential codes (2 and 3) have no
// curve interpolation; the nearest, linear, stands in.
KeyframeTypeEnum
rampInterpolation(int code)
{
    switch (code) {
    case 0:
        return eKeyframeTypeConstant;
    case 4:
        return eKeyframeTypeSmooth;
    case 5:
        return eKeyframeTypeCatmullRom;
    case 6:
        return eKeyframeTypeCubic;
    default:
        return eKeyframeTypeLinear;
    }
}

void
setDimensions(NetworkAttribute* attribute,
              NetworkKnobKind knob,
              int dimensions,
              const QJsonValue& value,
              std::vector<std::string> names = std::vector<std::string>())
{
    attribute->knob = knob;
    attribute->dimensions = dimensions;
    attribute->defaults = numbers(value);
    attribute->defaults.resize(dimensions, 0.);
    attribute->dimensionNames = names;
}

const std::vector<std::string>&
xyz()
{
    static const std::vector<std::string> names = { "x", "y", "z" };

    return names;
}

const std::vector<std::string>&
rgb()
{
    static const std::vector<std::string> names = { "r", "g", "b" };

    return names;
}

bool
mapCyclesAttribute(const QJsonObject& json,
                   NetworkAttribute* attribute,
                   std::string* error)
{
    const std::string type = toStd(json.value(QLatin1String("type")));
    const QJsonValue value = json.value(QLatin1String("default"));

    if (type == "float") {
        setDimensions(attribute, NetworkKnobKind::Double, 1, value);
    } else if (type == "int") {
        setDimensions(attribute, NetworkKnobKind::Int, 1, value);
    } else if (type == "boolean") {
        setDimensions(attribute, NetworkKnobKind::Bool, 1, value);
    } else if (type == "enum") {
        attribute->knob = NetworkKnobKind::Choice;
        const std::string defaultName = toStd(value);
        for (const QJsonValue& item : json.value(QLatin1String("enum")).toArray()) {
            const QJsonObject option = item.toObject();
            NetworkChoice choice;
            choice.id = toStd(option.value(QLatin1String("name")));
            choice.label = shaderLabelFromScriptName(choice.id);
            choice.value = formatNumber(option.value(QLatin1String("value")).toDouble());
            if (choice.id == defaultName) {
                attribute->defaultChoice = (int)attribute->choices.size();
            }
            attribute->choices.push_back(choice);
        }
    } else if ((type == "vector") || (type == "point") || (type == "normal")) {
        setDimensions(attribute, NetworkKnobKind::Double, 3, value, xyz());
    } else if (type == "color") {
        setDimensions(attribute, NetworkKnobKind::Color, 3, value, rgb());
    } else if (type == "string") {
        attribute->knob = NetworkKnobKind::String;
        attribute->defaultText = toStd(value);
    } else if (type == "closure") {
        attribute->knob = NetworkKnobKind::None;
    } else if (type == "array_float") {
        attribute->knob = NetworkKnobKind::Parametric;
        attribute->dimensions = 1;
        attribute->curves = sampledCurves(value, 1);
    } else if (type == "array_color") {
        attribute->knob = NetworkKnobKind::Parametric;
        attribute->dimensions = 3;
        attribute->dimensionNames = rgb();
        attribute->curves = sampledCurves(value, 3);
    } else if (type == "array_vector") {
        attribute->knob = NetworkKnobKind::Parametric;
        attribute->dimensions = 3;
        attribute->dimensionNames = xyz();
        attribute->curves = sampledCurves(value, 3);
    } else if (type == "array_int") {
        attribute->knob = NetworkKnobKind::Table;
        attribute->defaultText = tableText(value);
    } else if (type == "transform") {
        setDimensions(attribute, NetworkKnobKind::Double, 12, value);
    } else {
        *error = "attribute " + attribute->name + " has the unknown type " + type;

        return false;
    }
    attribute->port.type = type;
    attribute->port.typeClass = typeClassOf(cyclesTypeTable(), type);
    attribute->hint = toStd(json.value(QLatin1String("ui_name"))) + "\n\nType: " + type;
    const std::string defaultText = defaultDescription(value);
    if (!defaultText.empty()) {
        attribute->hint += "\nDefault: " + defaultText;
    }

    return true;
}

bool
mapMoonRayAttribute(const QJsonObject& json,
                    NetworkAttribute* attribute,
                    std::string* error)
{
    const std::string type = toStd(json.value(QLatin1String("type")));
    const QJsonValue value = json.value(QLatin1String("default"));
    const QJsonArray choices = json.value(QLatin1String("enum")).toArray();
    const QJsonObject structure = json.value(QLatin1String("structure")).toObject();
    const std::string structureType = toStd(structure.value(QLatin1String("type")));

    if ((structureType == "ramp_color") || (structureType == "ramp_float")) {
        // The ramp's arrays are filled in once all of them are read; see rampCurves().
        attribute->knob = NetworkKnobKind::Parametric;
        attribute->ramp = toStd(structure.value(QLatin1String("name")));
        if (attribute->ramp.empty()) {
            *error = "attribute " + attribute->name + " belongs to a ramp with no name";

            return false;
        }
        if (structureType == "ramp_color") {
            attribute->dimensions = 3;
            attribute->dimensionNames = rgb();
        } else {
            attribute->dimensions = 1;
        }
    } else if (type == "Float") {
        setDimensions(attribute, NetworkKnobKind::Double, 1, value);
    } else if ((type == "Int") && !choices.isEmpty()) {
        attribute->knob = NetworkKnobKind::Choice;
        const double defaultValue = value.toDouble();
        for (const QJsonValue& item : choices) {
            const QJsonObject option = item.toObject();
            NetworkChoice choice;
            choice.id = toStd(option.value(QLatin1String("name")));
            choice.label = shaderLabelFromScriptName(choice.id);
            const double optionValue = option.value(QLatin1String("value")).toDouble();
            choice.value = formatNumber(optionValue);
            if (optionValue == defaultValue) {
                attribute->defaultChoice = (int)attribute->choices.size();
            }
            attribute->choices.push_back(choice);
        }
    } else if (type == "Int") {
        setDimensions(attribute, NetworkKnobKind::Int, 1, value);
    } else if (type == "Bool") {
        setDimensions(attribute, NetworkKnobKind::Bool, 1, value);
    } else if (type == "Rgb") {
        setDimensions(attribute, NetworkKnobKind::Color, 3, value, rgb());
    } else if (type == "Vec2f") {
        setDimensions(attribute, NetworkKnobKind::Double, 2, value, { "x", "y" });
    } else if ((type == "Vec3f") || (type == "Vec3d")) {
        setDimensions(attribute, NetworkKnobKind::Double, 3, value, xyz());
    } else if (type == "Mat4d") {
        setDimensions(attribute, NetworkKnobKind::Double, 16, value);
    } else if (type == "String") {
        attribute->knob = json.contains(QLatin1String("asset")) ? NetworkKnobKind::File : NetworkKnobKind::String;
        attribute->defaultText = toStd(value);
    } else if ((type == "SceneObject*") || (type == "SceneObjectVector")) {
        attribute->knob = NetworkKnobKind::None;
    } else if ((type == "FloatVector") || (type == "IntVector") || (type == "RgbVector")) {
        attribute->knob = NetworkKnobKind::Table;
        attribute->defaultText = tableText(value);
    } else {
        *error = "attribute " + attribute->name + " has the unknown type " + type;

        return false;
    }
    if (json.contains(QLatin1String("min"))) {
        attribute->hasDisplayMinimum = true;
        attribute->displayMinimum = json.value(QLatin1String("min")).toDouble();
    }
    if (json.contains(QLatin1String("max"))) {
        attribute->hasDisplayMaximum = true;
        attribute->displayMaximum = json.value(QLatin1String("max")).toDouble();
    }
    const std::string interface = toStd(json.value(QLatin1String("interface")));
    attribute->port.type = interface.empty() ? type : interface;
    attribute->port.typeClass = typeClassOf(moonRayTypeTable(), attribute->port.type);
    attribute->hint = toStd(json.value(QLatin1String("comment")));
    attribute->group = toStd(json.value(QLatin1String("subgroup")));

    return true;
}

// Gives every attribute of each ramp the ramp's knob: its label, and curves with one control point
// per position, from the ramp's positions, values and interpolations arrays.
void
rampCurves(const QJsonArray& attributes,
           NetworkNodeLayout* layout)
{
    struct Ramp {
        std::vector<double> positions;
        std::vector<std::vector<double>> values;
        std::vector<double> interpolations;
        std::string hint;
    };
    std::map<std::string, Ramp> ramps;

    for (const QJsonValue& item : attributes) {
        const QJsonObject json = item.toObject();
        const QJsonObject structure = json.value(QLatin1String("structure")).toObject();
        const std::string name = toStd(structure.value(QLatin1String("name")));
        if (name.empty()) {
            continue;
        }
        Ramp& ramp = ramps[name];
        const std::string path = toStd(structure.value(QLatin1String("path")));
        const QJsonValue value = json.value(QLatin1String("default"));
        if (path == "positions") {
            ramp.positions = numbers(value);
        } else if (path == "values") {
            for (const QJsonValue& row : value.toArray()) {
                ramp.values.push_back(numbers(row));
            }
            ramp.hint = toStd(json.value(QLatin1String("comment")));
        } else if (path == "interpolation_types") {
            ramp.interpolations = numbers(value);
        }
    }
    for (NetworkAttribute& attribute : layout->attributes) {
        const std::map<std::string, Ramp>::const_iterator it = ramps.find(attribute.ramp);
        if (attribute.ramp.empty() || (it == ramps.end())) {
            continue;
        }
        const Ramp& ramp = it->second;
        attribute.label = shaderLabelFromScriptName(attribute.ramp);
        if (!ramp.hint.empty()) {
            attribute.hint = ramp.hint;
        }
        attribute.curves.assign(attribute.dimensions, std::vector<NetworkCurvePoint>());
        for (std::size_t i = 0; i < ramp.positions.size(); ++i) {
            const std::vector<double> none;
            const std::vector<double>& values = (i < ramp.values.size()) ? ramp.values[i] : none;
            for (int d = 0; d < attribute.dimensions; ++d) {
                NetworkCurvePoint point;
                point.x = ramp.positions[i];
                point.y = (d < (int)values.size()) ? values[d] : 0.;
                point.interpolation = rampInterpolation((i < ramp.interpolations.size()) ? (int)ramp.interpolations[i] : 1);
                attribute.curves[d].push_back(point);
            }
        }
    }
}

// What registering a node's plug-in reads: its plug-in ID, label and grouping.
void
setIdentity(const std::string& renderer,
            const QJsonObject& node,
            NetworkNodeLayout* layout)
{
    layout->pluginId = toStd(node.value(QLatin1String("id")));
    layout->label = shaderLabelFromScriptName(toStd(node.value(QLatin1String("name"))));
    layout->grouping = (renderer == kRendererCycles) ? kShaderGroupCycles : kShaderGroupMoonRay;
}

bool
buildLayout(const std::string& renderer,
            const QJsonObject& node,
            const QJsonObject& nodeLayout,
            NetworkNodeLayout* layout,
            std::string* error)
{
    const bool cycles = (renderer == kRendererCycles);
    const std::string name = toStd(node.value(QLatin1String("name")));

    setIdentity(renderer, node, layout);
    layout->description = std::string(cycles ? "Cycles" : "MoonRay") + " " + toStd(node.value(QLatin1String("kind"))) + " " + name + ".";
    for (const QJsonValue& target : node.value(QLatin1String("targets")).toArray()) {
        layout->targets.push_back(toStd(target));
    }

    // The page of each attribute a layout group lists, and the pages in their order.
    std::map<std::string, std::string> listedPage;
    std::vector<std::string> groupOrder;
    if (cycles) {
        for (const QJsonValue& item : nodeLayout.value(QLatin1String("groups")).toArray()) {
            const QJsonObject group = item.toObject();
            const std::string page = toStd(group.value(QLatin1String("name")));
            groupOrder.push_back(page);
            for (const QJsonValue& attribute : group.value(QLatin1String("attributes")).toArray()) {
                listedPage[toStd(attribute)] = page;
            }
        }
    } else {
        for (const QJsonValue& group : node.value(QLatin1String("groups")).toArray()) {
            groupOrder.push_back(toStd(group));
        }
    }
    const std::string fallbackPage = groupOrder.empty() ? kControlsPage : kOtherPage;

    std::set<std::string> knobPages;
    for (const QJsonValue& item : node.value(QLatin1String("attributes")).toArray()) {
        const QJsonObject json = item.toObject();
        NetworkAttribute attribute;
        attribute.name = toStd(json.value(QLatin1String("name")));
        attribute.label = shaderLabelFromScriptName(attribute.name);
        attribute.linkable = json.value(QLatin1String("linkable")).toBool();
        attribute.portOnly = json.value(QLatin1String("port_only")).toBool();
        if (cycles) {
            const std::map<std::string, std::string>::const_iterator it = listedPage.find(attribute.name);
            attribute.page = (it == listedPage.end()) ? fallbackPage : it->second;
        } else {
            attribute.page = toStd(json.value(QLatin1String("group")));
            if (attribute.page.empty()) {
                attribute.page = fallbackPage;
            } else if (std::find(groupOrder.begin(), groupOrder.end(), attribute.page) == groupOrder.end()) {
                groupOrder.push_back(attribute.page);
            }
        }
        const bool mapped = cycles ? mapCyclesAttribute(json, &attribute, error) : mapMoonRayAttribute(json, &attribute, error);
        if (!mapped) {
            *error = layout->pluginId + ": " + *error;

            return false;
        }
        attribute.port.attribute = attribute.name;
        attribute.port.page = attribute.page;
        attribute.port.array = json.value(QLatin1String("array")).toBool();
        attribute.port.varying = attribute.linkable && !attribute.portOnly;
        if (attribute.knob != NetworkKnobKind::None) {
            knobPages.insert(attribute.page);
        }
        layout->attributes.push_back(attribute);
    }
    if (!cycles) {
        rampCurves(node.value(QLatin1String("attributes")).toArray(), layout);
    }
    groupOrder.push_back(fallbackPage);
    for (const std::string& page : groupOrder) {
        if (knobPages.count(page) && (std::find(layout->pages.begin(), layout->pages.end(), page) == layout->pages.end())) {
            layout->pages.push_back(page);
        }
    }

    for (const QJsonValue& item : node.value(QLatin1String("outputs")).toArray()) {
        const QJsonObject json = item.toObject();
        OutputPortDescriptor output;
        output.name = toStd(json.value(QLatin1String("name")));
        const std::string interface = toStd(json.value(QLatin1String("interface")));
        output.type = interface.empty() ? toStd(json.value(QLatin1String("type"))) : interface;
        output.typeClass = typeClassOf(cycles ? cyclesTypeTable() : moonRayTypeTable(), output.type);
        output.array = json.value(QLatin1String("array")).toBool();
        layout->outputs.push_back(output);
    }

    const QJsonArray keyPorts = nodeLayout.value(QLatin1String("key_ports")).toArray();
    if (keyPorts.isEmpty()) {
        for (const NetworkAttribute& attribute : layout->attributes) {
            if (attribute.linkable) {
                layout->defaultPorts.push_back(attribute.name);
            }
        }
    } else {
        for (const QJsonValue& port : keyPorts) {
            layout->defaultPorts.push_back(toStd(port));
        }
        for (const NetworkAttribute& attribute : layout->attributes) {
            if (attribute.portOnly) {
                layout->defaultPorts.push_back(attribute.name);
            }
        }
    }

    return true;
}

bool
readResource(const QString& path,
             QByteArray* data)
{
    QFile file(path);

    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    *data = file.readAll();

    return true;
}

NATRON_NAMESPACE_ANONYMOUS_EXIT

std::string
shaderLabelFromScriptName(const std::string& scriptName)
{
    std::vector<std::string> words;
    std::string word;
    const std::size_t n = scriptName.size();

    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char c = (unsigned char)scriptName[i];
        if ((c == '_') || (c == '.') || (c == ' ') || (c == '-')) {
            if (!word.empty()) {
                words.push_back(word);
                word.clear();
            }
            continue;
        }
        if (!word.empty()) {
            const unsigned char prev = (unsigned char)word.back();
            const bool next = (i + 1 < n) && std::islower((unsigned char)scriptName[i + 1]);
            const bool lowerToUpper = std::islower(prev) && std::isupper(c);
            // The last capital of a run starts the next word: "IORLevel" is "IOR Level".
            const bool runEnds = std::isupper(prev) && std::isupper(c) && next;
            const bool digitChange = (std::isdigit(prev) != 0) != (std::isdigit(c) != 0);
            if (lowerToUpper || runEnds || digitChange) {
                words.push_back(word);
                word.clear();
            }
        }
        word += (char)c;
    }
    if (!word.empty()) {
        words.push_back(word);
    }

    std::string ret;
    for (const std::string& w : words) {
        std::string lower = w;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        const std::map<std::string, std::string>::const_iterator acronym = acronyms().find(lower);
        std::string out = (acronym == acronyms().end()) ? w : acronym->second;
        if (acronym == acronyms().end()) {
            out[0] = (char)std::toupper((unsigned char)out[0]);
        }
        if (!ret.empty()) {
            ret += ' ';
        }
        ret += out;
    }

    return ret;
}

NATRON_NAMESPACE_ANONYMOUS_ENTER

// A manifest is read into its renderer, its nodes and the nodes' layout entries keyed by ID.
struct ManifestDocument {
    std::string renderer;
    QJsonArray nodes;
    QJsonObject nodeLayouts;
};

bool
readManifest(const QByteArray& manifest,
             const QByteArray& layout,
             ManifestDocument* document,
             std::string* error)
{
    QJsonParseError parseError;
    const QJsonDocument manifestDocument = QJsonDocument::fromJson(manifest, &parseError);

    if (!manifestDocument.isObject()) {
        *error = "the manifest is not a JSON object: " + toStd(parseError.errorString());

        return false;
    }
    const QJsonDocument layoutDocument = QJsonDocument::fromJson(layout, &parseError);
    if (!layoutDocument.isObject()) {
        *error = "the layout is not a JSON object: " + toStd(parseError.errorString());

        return false;
    }
    const QJsonObject root = manifestDocument.object();
    document->renderer = toStd(root.value(QLatin1String("renderer")));
    if ((document->renderer != kRendererCycles) && (document->renderer != kRendererMoonRay)) {
        *error = "the manifest's renderer \"" + document->renderer + "\" is not one this build reads";

        return false;
    }
    document->nodes = root.value(QLatin1String("nodes")).toArray();
    document->nodeLayouts = layoutDocument.object().value(QLatin1String("nodes")).toObject();

    return true;
}

QJsonObject
nodeLayoutOf(const ManifestDocument& document,
             const QJsonObject& node)
{
    return document.nodeLayouts.value(node.value(QLatin1String("id")).toString()).toObject();
}

NATRON_NAMESPACE_ANONYMOUS_EXIT

bool
parseShaderManifest(const QByteArray& manifest,
                    const QByteArray& layout,
                    std::vector<NetworkNodeLayout>* layouts,
                    std::string* error)
{
    ManifestDocument document;

    if (!readManifest(manifest, layout, &document, error)) {
        return false;
    }
    for (const QJsonValue& item : document.nodes) {
        const QJsonObject node = item.toObject();
        NetworkNodeLayout built;
        if (!buildLayout(document.renderer, node, nodeLayoutOf(document, node), &built, error)) {
            return false;
        }
        layouts->push_back(built);
    }

    return true;
}

void
registerShaderPortTypeTables()
{
    registerPortTypeTable(kRendererCycles, cyclesTypeTable());
    registerPortTypeTable(kRendererMoonRay, moonRayTypeTable());
}

std::size_t
registerBuiltinShaderPlugins()
{
    static std::size_t registered = 0;
    static bool done = false;

    if (done) {
        return registered;
    }
    done = true;
    registerShaderPortTypeTables();
    static const char* const renderers[] = { kRendererCycles, kRendererMoonRay };
    for (const char* renderer : renderers) {
        const QString base = QString::fromUtf8(":/Shaders/") + QString::fromUtf8(renderer);
        QByteArray manifest;
        QByteArray layout;
        if (!readResource(base + QString::fromUtf8(".json"), &manifest) || !readResource(base + QString::fromUtf8("_layout.json"), &layout)) {
            qWarning() << "The shader manifest of" << renderer << "is missing from the build.";
            continue;
        }
        ManifestDocument document;
        std::string error;
        if (!readManifest(manifest, layout, &document, &error)) {
            qWarning() << "The shader manifest of" << renderer << "cannot be read:" << QString::fromUtf8(error.c_str());
            continue;
        }
        // Registering needs a node's identity only; its attributes are mapped when the first node
        // of it is built, so that startup reads the manifests and maps nothing.
        for (const QJsonValue& item : document.nodes) {
            const QJsonObject node = item.toObject();
            const QJsonObject nodeLayout = nodeLayoutOf(document, node);
            const std::string rendererName = document.renderer;
            NetworkNodeLayout identity;
            setIdentity(rendererName, node, &identity);
            NetworkNode::addLayoutBuilder(identity.pluginId, [rendererName, node, nodeLayout](NetworkNodeLayout* built, std::string* buildError) {
                return buildLayout(rendererName, node, nodeLayout, built, buildError);
            });
            if (NetworkNode::registerPlugin(identity)) {
                ++registered;
            }
        }
    }

    return registered;
}

NATRON_NAMESPACE_EXIT
