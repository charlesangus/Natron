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

#include "RenderSettingsKnobs.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <mutex>
#include <set>
#include <sstream>

#include <QDebug>
#include <QString>

#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Scene/CyclesAovs.h"
#include "Engine/Nodes/Scene/CyclesSettings.h"
#include "Engine/Nodes/Scene/MoonRayAovs.h"
#include "Engine/Nodes/Scene/MoonRaySettings.h"
#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/RenderSettingsPrim.h"
#include "Engine/Nodes/Scene/UtilitySettings.h"

#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec2i.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/imaging/hd/pluginRenderDelegateUniqueHandle.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/rendererPlugin.h>
#include <pxr/imaging/hd/rendererPluginHandle.h>
#include <pxr/imaging/hd/rendererPluginRegistry.h>

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
enum KindEnum {
    eKindNone,
    eKindBool,
    eKindInt,
    eKindUInt,
    eKindFloat,
    eKindDouble,
    eKindString,
    eKindToken,
    eKindVec2i,
    eKindVec2f,
    eKindVec2d,
    eKindVec3f,
    eKindVec3d,
    eKindVec4f,
    eKindVec4d,
    eKindFloatArray,
    eKindDoubleArray,
    eKindIntArray,
};

KindEnum
kindOf(const VtValue& value)
{
    if (value.IsHolding<bool>()) {
        return eKindBool;
    } else if (value.IsHolding<int>()) {
        return eKindInt;
    } else if (value.IsHolding<unsigned int>()) {
        return eKindUInt;
    } else if (value.IsHolding<float>()) {
        return eKindFloat;
    } else if (value.IsHolding<double>()) {
        return eKindDouble;
    } else if (value.IsHolding<std::string>()) {
        return eKindString;
    } else if (value.IsHolding<TfToken>()) {
        return eKindToken;
    } else if (value.IsHolding<GfVec2i>()) {
        return eKindVec2i;
    } else if (value.IsHolding<GfVec2f>()) {
        return eKindVec2f;
    } else if (value.IsHolding<GfVec2d>()) {
        return eKindVec2d;
    } else if (value.IsHolding<GfVec3f>()) {
        return eKindVec3f;
    } else if (value.IsHolding<GfVec3d>()) {
        return eKindVec3d;
    } else if (value.IsHolding<GfVec4f>()) {
        return eKindVec4f;
    } else if (value.IsHolding<GfVec4d>()) {
        return eKindVec4d;
    } else if (value.IsHolding<VtFloatArray>()) {
        return eKindFloatArray;
    } else if (value.IsHolding<VtDoubleArray>()) {
        return eKindDoubleArray;
    } else if (value.IsHolding<VtIntArray>()) {
        return eKindIntArray;
    }

    return eKindNone;
}

// The components of a vector or array value, empty for any other.
std::vector<double>
components(const VtValue& value)
{
    std::vector<double> out;

    switch (kindOf(value)) {
    case eKindVec2i: {
        const GfVec2i& v = value.UncheckedGet<GfVec2i>();
        out.assign(v.data(), v.data() + 2);
        break;
    }
    case eKindVec2f: {
        const GfVec2f& v = value.UncheckedGet<GfVec2f>();
        out.assign(v.data(), v.data() + 2);
        break;
    }
    case eKindVec2d: {
        const GfVec2d& v = value.UncheckedGet<GfVec2d>();
        out.assign(v.data(), v.data() + 2);
        break;
    }
    case eKindVec3f: {
        const GfVec3f& v = value.UncheckedGet<GfVec3f>();
        out.assign(v.data(), v.data() + 3);
        break;
    }
    case eKindVec3d: {
        const GfVec3d& v = value.UncheckedGet<GfVec3d>();
        out.assign(v.data(), v.data() + 3);
        break;
    }
    case eKindVec4f: {
        const GfVec4f& v = value.UncheckedGet<GfVec4f>();
        out.assign(v.data(), v.data() + 4);
        break;
    }
    case eKindVec4d: {
        const GfVec4d& v = value.UncheckedGet<GfVec4d>();
        out.assign(v.data(), v.data() + 4);
        break;
    }
    case eKindFloatArray: {
        const VtFloatArray& a = value.UncheckedGet<VtFloatArray>();
        out.assign(a.cbegin(), a.cend());
        break;
    }
    case eKindDoubleArray: {
        const VtDoubleArray& a = value.UncheckedGet<VtDoubleArray>();
        out.assign(a.cbegin(), a.cend());
        break;
    }
    case eKindIntArray: {
        const VtIntArray& a = value.UncheckedGet<VtIntArray>();
        out.assign(a.cbegin(), a.cend());
        break;
    }
    default:
        break;
    }

    return out;
}

// A value of prototype's type made of c, which has as many components as the type, any count for an array.
VtValue
fromComponents(const VtValue& prototype,
               const std::vector<double>& c)
{
    switch (kindOf(prototype)) {
    case eKindVec2i:
        return (c.size() == 2) ? VtValue(GfVec2i((int)c[0], (int)c[1])) : VtValue();
    case eKindVec2f:
        return (c.size() == 2) ? VtValue(GfVec2f((float)c[0], (float)c[1])) : VtValue();
    case eKindVec2d:
        return (c.size() == 2) ? VtValue(GfVec2d(c[0], c[1])) : VtValue();
    case eKindVec3f:
        return (c.size() == 3) ? VtValue(GfVec3f((float)c[0], (float)c[1], (float)c[2])) : VtValue();
    case eKindVec3d:
        return (c.size() == 3) ? VtValue(GfVec3d(c[0], c[1], c[2])) : VtValue();
    case eKindVec4f:
        return (c.size() == 4) ? VtValue(GfVec4f((float)c[0], (float)c[1], (float)c[2], (float)c[3])) : VtValue();
    case eKindVec4d:
        return (c.size() == 4) ? VtValue(GfVec4d(c[0], c[1], c[2], c[3])) : VtValue();
    case eKindFloatArray: {
        VtFloatArray a(c.size());
        for (std::size_t i = 0; i < c.size(); ++i) {
            a[i] = (float)c[i];
        }

        return VtValue(a);
    }
    case eKindDoubleArray:
        return VtValue(VtDoubleArray(c.begin(), c.end()));
    case eKindIntArray: {
        VtIntArray a(c.size());
        for (std::size_t i = 0; i < c.size(); ++i) {
            a[i] = (int)c[i];
        }

        return VtValue(a);
    }
    default:
        return VtValue();
    }
}

std::string
numberText(double v,
           bool single)
{
    char text[64];

    std::snprintf(text, sizeof(text), single ? "%.9g" : "%.17g", v);

    return text;
}

bool
isSingle(KindEnum kind)
{
    return (kind == eKindFloat) || (kind == eKindVec2f) || (kind == eKindVec3f) || (kind == eKindVec4f) || (kind == eKindFloatArray);
}

// Whitespace-separated numbers; false if text holds anything else.
bool
parseNumbers(const std::string& text,
             std::vector<double>* out)
{
    std::istringstream in(text);
    std::string word;

    out->clear();
    while (in >> word) {
        char* end = nullptr;
        const double v = std::strtod(word.c_str(), &end);
        if (!end || (*end != '\0')) {
            return false;
        }
        out->push_back(v);
    }

    return true;
}

// The value of text as prototype's type; empty if text is not one.
VtValue
parseValue(const std::string& text,
           const VtValue& prototype)
{
    const KindEnum kind = kindOf(prototype);

    if (kind == eKindString) {
        return VtValue(text);
    } else if (kind == eKindToken) {
        return VtValue(TfToken(text));
    }
    std::vector<double> numbers;
    if (!parseNumbers(text, &numbers)) {
        return VtValue();
    }
    switch (kind) {
    case eKindBool:
        return (numbers.size() == 1) ? VtValue(numbers[0] != 0.) : VtValue();
    case eKindInt:
        return (numbers.size() == 1) ? VtValue((int)numbers[0]) : VtValue();
    case eKindUInt:
        return ((numbers.size() == 1) && (numbers[0] >= 0.)) ? VtValue((unsigned int)numbers[0]) : VtValue();
    case eKindFloat:
        return (numbers.size() == 1) ? VtValue((float)numbers[0]) : VtValue();
    case eKindDouble:
        return (numbers.size() == 1) ? VtValue(numbers[0]) : VtValue();
    default:
        return fromComponents(prototype, numbers);
    }
}

// The double a float setting is shown as: the shortest decimal that reads back as the same float, so
// 0.01f is 0.01 and not 0.00999999977648.
double
floatToDouble(float v)
{
    char text[32];

    for (int digits = 1; digits <= 9; ++digits) {
        std::snprintf(text, sizeof(text), "%.*g", digits, (double)v);
        if (std::strtof(text, nullptr) == v) {
            break;
        }
    }

    return std::strtod(text, nullptr);
}

int
clampToInt(double v)
{
    if (v > (double)INT_MAX) {
        return INT_MAX;
    } else if (v < (double)INT_MIN) {
        return INT_MIN;
    }

    return (int)v;
}

std::string
escape(const std::string& text)
{
    std::string out;

    for (char c : text) {
        if (c == '\\') {
            out += "\\\\";
        } else if (c == '\t') {
            out += "\\t";
        } else if (c == '\n') {
            out += "\\n";
        } else {
            out += c;
        }
    }

    return out;
}

std::string
capitalized(std::string word)
{
    if (!word.empty()) {
        word[0] = (char)std::toupper((unsigned char)word[0]);
    }

    return word;
}

// What a delegate declares: its settings, and the namespaces of its render settings prim attributes.
struct Declared {
    RenderSettingsKnobs::DescriptorList list;
    std::vector<std::string> namespaces;
};

std::mutex gDescriptorsMutex;
std::map<std::string, Declared> gDescriptors;
std::map<std::string, RenderSettingsKnobs::DescriptorList> gTestDescriptors;
struct ClassRule {
    const char* group;
    std::vector<std::string> prefixes;
};

// In priority order: the first rule with a matching token wins.
const std::vector<ClassRule>&
classRules()
{
    static const std::vector<ClassRule> rules = {
        { "denoise", { "denois", "oidn" } },
        { "pathGuiding", { "guiding" } },
        { "volumes", { "volume", "raymarch" } },
        { "bounces", { "bounce", "depth" } },
        { "clamping", { "clamp" } },
        { "lights", { "light", "shadow" } },
        { "sampling", { "sample", "sampling", "seed", "adaptive", "noise" } },
        { "filtering", { "filter" } },
        { "motion", { "motion", "shutter", "blur" } },
        { "performance", { "thread", "memory", "cache", "cull", "cores", "fps", "execution", "tile", "bucket" } },
        { "logging", { "log", "debug", "verbose" } },
    };

    return rules;
}

// The order the classified groups take on the page.
const char* const kClassifiedOrder[] = {
    "sampling", "denoise", "bounces", "clamping", "lights", "volumes", "pathGuiding", "filtering", "motion", "performance", "logging"
};

const char*
classifiedTooltip(const std::string& group)
{
    if (group == "sampling") {
        return "How many samples are taken and when sampling stops.";
    }
    if (group == "denoise") {
        return "Removing the noise left in the render.";
    }
    if (group == "bounces") {
        return "How far light paths are followed.";
    }
    if (group == "clamping") {
        return "Limits on how bright a single sample can be.";
    }
    if (group == "lights") {
        return "How lights and shadows are sampled.";
    }
    if (group == "volumes") {
        return "How volumes are marched and stored.";
    }
    if (group == "pathGuiding") {
        return "Learning where light comes from to sample it better.";
    }
    if (group == "filtering") {
        return "How samples are filtered into pixels.";
    }
    if (group == "motion") {
        return "Motion blur and shutter.";
    }
    if (group == "performance") {
        return "Trading speed and memory.";
    }

    return "Logging and debugging output.";
}

std::vector<std::string>
keyTokens(const std::string& key)
{
    std::string segment = key;
    const std::size_t cut = segment.find_last_of(":./");

    if (cut != std::string::npos) {
        segment = segment.substr(cut + 1);
    }
    std::vector<std::string> tokens;
    std::string word;
    for (std::size_t i = 0; i < segment.size(); ++i) {
        const unsigned char c = (unsigned char)segment[i];
        if (!std::isalnum(c)) {
            if (!word.empty()) {
                tokens.push_back(word);
                word.clear();
            }
            continue;
        }
        const bool upper = std::isupper(c) != 0;
        if (upper && !word.empty()) {
            const unsigned char prev = (unsigned char)segment[i - 1];
            const bool nextLower = (i + 1 < segment.size()) && std::islower((unsigned char)segment[i + 1]);
            if (std::islower(prev) || std::isdigit(prev) || (std::isupper(prev) && nextLower)) {
                tokens.push_back(word);
                word.clear();
            }
        }
        word.push_back((char)std::tolower(c));
    }
    if (!word.empty()) {
        tokens.push_back(word);
    }

    return tokens;
}

std::map<std::string, RenderSettingsKnobs::Curated> gTestCurated;

Declared
declaredBy(const std::string& rendererId)
{
    Declared declared;
    RenderSettingsKnobs::DescriptorList& list = declared.list;
    RenderDelegates::Delegate delegate;

    if (!RenderDelegates::find(rendererId, &delegate) || !delegate.available) {
        return declared;
    }
    TfErrorMark mark;
    // Reached at node creation and from a render thread's hash, with nothing above to catch what a
    // delegate throws. The curated knobs do not depend on what it declares.
    try {
        HdRendererPluginHandle plugin = HdRendererPluginRegistry::GetInstance().GetOrCreateRendererPlugin(TfToken(rendererId));
        if (plugin) {
            HdRenderSettingsMap settings;
            if (MoonRayAovs::isMoonRay(rendererId)) {
                for (const std::pair<TfToken, VtValue>& setting : MoonRayAovs::renderSettings(1)) {
                    settings[setting.first] = setting.second;
                }
            }
            const std::function<void()> make = [&]() {
                HdPluginRenderDelegateUniqueHandle made = plugin->CreateDelegate(settings);
                if (!made) {
                    return;
                }
                for (const HdRenderSettingDescriptor& d : made->GetRenderSettingDescriptors()) {
                    RenderSettingsKnobs::Descriptor descriptor;
                    descriptor.key = d.key.GetString();
                    descriptor.name = d.name;
                    descriptor.defaultValue = d.defaultValue;
                    list.push_back(descriptor);
                }
                for (const TfToken& ns : made->GetRenderSettingsNamespaces()) {
                    declared.namespaces.push_back(ns.GetString());
                }
            };
            if (CyclesAovs::isCycles(rendererId)) {
                CyclesAovs::withThreadBudget(1, make);
            } else {
                make();
            }
        }
    } catch (const std::exception& e) {
        qWarning("The renderer %s threw while declaring its settings: %s", rendererId.c_str(), e.what());
        declared = Declared();
    } catch (...) {
        qWarning("The renderer %s threw while declaring its settings.", rendererId.c_str());
        declared = Declared();
    }
    mark.Clear();

    return declared;
}

const Declared&
cachedDeclared(const std::string& rendererId)
{
    std::map<std::string, Declared>::const_iterator found = gDescriptors.find(rendererId);

    if (found == gDescriptors.end()) {
        found = gDescriptors.insert(std::make_pair(rendererId, declaredBy(rendererId))).first;
    }

    return found->second;
}
} // anon namespace

RenderSettingsKnobs::DescriptorList
RenderSettingsKnobs::descriptors(const std::string& rendererId)
{
    std::lock_guard<std::mutex> l(gDescriptorsMutex);
    std::map<std::string, DescriptorList>::const_iterator test = gTestDescriptors.find(rendererId);

    if (test != gTestDescriptors.end()) {
        return test->second;
    }

    return cachedDeclared(rendererId).list;
}

const RenderSettingsKnobs::Curated*
RenderSettingsKnobs::curatedFor(const std::string& rendererId)
{
    {
        std::lock_guard<std::mutex> l(gDescriptorsMutex);
        std::map<std::string, Curated>::const_iterator test = gTestCurated.find(rendererId);
        if (test != gTestCurated.end()) {
            return &test->second;
        }
    }
    if (CyclesAovs::isCycles(rendererId)) {
        return &CyclesSettings::curated();
    } else if (MoonRayAovs::isMoonRay(rendererId)) {
        return &MoonRaySettings::curated();
    } else if (rendererId == kRenderDelegateUtilityId) {
        return &UtilitySettings::curated();
    }

    return nullptr;
}

std::vector<std::string>
RenderSettingsKnobs::declaredNamespaces(const std::string& rendererId)
{
    std::lock_guard<std::mutex> l(gDescriptorsMutex);

    if (gTestDescriptors.count(rendererId)) {
        return std::vector<std::string>();
    }

    return cachedDeclared(rendererId).namespaces;
}

void
RenderSettingsKnobs::setCuratedForTests(const std::string& rendererId,
                                        const Curated* curated)
{
    std::lock_guard<std::mutex> l(gDescriptorsMutex);

    if (curated) {
        gTestCurated[rendererId] = *curated;
    } else {
        gTestCurated.erase(rendererId);
    }
}

std::string
RenderSettingsKnobs::classifyKey(const std::string& key)
{
    const std::vector<std::string> tokens = keyTokens(key);

    for (const ClassRule& rule : classRules()) {
        for (const std::string& token : tokens) {
            for (const std::string& prefix : rule.prefixes) {
                if (token.compare(0, prefix.size(), prefix) == 0) {
                    return rule.group;
                }
            }
        }
    }

    return std::string();
}

bool
RenderSettingsKnobs::validateGroups(const Curated& curated,
                                    std::string* errorMessage)
{
    const auto fail = [&](const std::string& message) {
        if (errorMessage) {
            *errorMessage = message;
        }

        return false;
    };
    std::set<std::string> taken;

    taken.insert("Settings");
    taken.insert("OtherSettings");
    for (const CuratedSetting& c : curated.settings) {
        taken.insert(capitalized(c.name));
    }
    std::map<std::string, std::string> parents;
    for (const GroupSpec& g : curated.groups) {
        if (g.name.empty()) {
            return fail("a group has no name");
        }
        const std::string scriptName = capitalized(g.name);
        if (taken.count(scriptName)) {
            return fail("group " + g.name + " has the script name of a setting or of a reserved group");
        }
        taken.insert(scriptName);
        parents[g.name] = g.parent;
    }
    for (const GroupSpec& g : curated.groups) {
        if (g.parent.empty()) {
            continue;
        }
        std::map<std::string, std::string>::const_iterator parent = parents.find(g.parent);
        if (parent == parents.end()) {
            return fail("group " + g.name + " is inside " + g.parent + ", which is not a group");
        }
        if (!parent->second.empty()) {
            return fail("group " + g.name + " is nested deeper than two levels");
        }
    }
    for (const CuratedSetting& c : curated.settings) {
        if (!c.group.empty() && !parents.count(c.group)) {
            return fail("setting " + c.name + " is in " + c.group + ", which is not a group");
        }
    }

    return true;
}

void
RenderSettingsKnobs::setDescriptorsForTests(const std::string& rendererId,
                                            const DescriptorList& list)
{
    std::lock_guard<std::mutex> l(gDescriptorsMutex);

    if (list.empty()) {
        gTestDescriptors.erase(rendererId);
    } else {
        gTestDescriptors[rendererId] = list;
    }
}

std::vector<std::string>
RenderSettingsKnobs::rendererIds()
{
    std::vector<std::string> ids;

    for (const RenderDelegates::Delegate& delegate : RenderDelegates::list()) {
        if (delegate.available) {
            ids.push_back(delegate.id);
        }
    }
    std::lock_guard<std::mutex> l(gDescriptorsMutex);
    for (const std::pair<const std::string, DescriptorList>& test : gTestDescriptors) {
        if (std::find(ids.begin(), ids.end(), test.first) == ids.end()) {
            ids.push_back(test.first);
        }
    }

    return ids;
}

std::string
RenderSettingsKnobs::dump()
{
    std::string text;

    for (const std::string& id : rendererIds()) {
        const DescriptorList list = descriptors(id);
        if (list.empty()) {
            text += id + "\t(declares no settings, or its delegate could not be made)\n";
        }
        for (const Descriptor& d : list) {
            text += id + "\t" + d.key + "\t" + d.defaultValue.GetTypeName() + "\t" + (isSupported(d.defaultValue) ? escape(valueText(d.defaultValue)) : std::string("(no knob for this type)")) + "\t" + d.name + "\n";
        }
    }

    return text;
}

std::string
RenderSettingsKnobs::scriptPrefix(const std::string& rendererId)
{
    std::string name = rendererId;

    if ((name.size() > 2) && (name.compare(0, 2, "Hd") == 0) && std::isupper((unsigned char)name[2])) {
        name = name.substr(2);
    }
    static const char* const suffixes[] = { "RendererPlugin", "Plugin" };
    for (const char* suffix : suffixes) {
        const std::string s(suffix);
        if ((name.size() > s.size()) && (name.compare(name.size() - s.size(), s.size(), s) == 0)) {
            name = name.substr(0, name.size() - s.size());
            break;
        }
    }
    std::string out;
    for (char c : name) {
        if (std::isalnum((unsigned char)c)) {
            out += c;
        }
    }
    if (out.empty() || std::isdigit((unsigned char)out[0])) {
        out = "renderer" + out;
    }
    // "MoonRay" and "Moonray" both give "moonray".
    std::string lowered;
    for (char c : out) {
        lowered += (char)std::tolower((unsigned char)c);
    }
    if ((lowered == "cycles") || (lowered == "moonray") || (lowered == "storm") || (lowered == "embree")) {
        return lowered;
    }
    out[0] = (char)std::tolower((unsigned char)out[0]);

    return out;
}

std::string
RenderSettingsKnobs::camelCaseFromKey(const std::string& key,
                                      bool wholeKey)
{
    std::string source = key;

    if (!wholeKey) {
        const std::size_t colon = source.rfind(':');
        if (colon != std::string::npos) {
            source = source.substr(colon + 1);
        }
    }
    std::string out;
    bool upperNext = false;
    for (char c : source) {
        if (!std::isalnum((unsigned char)c)) {
            upperNext = !out.empty();
            continue;
        }
        if (out.empty()) {
            out += (char)std::tolower((unsigned char)c);
        } else if (upperNext) {
            out += (char)std::toupper((unsigned char)c);
        } else {
            out += c;
        }
        upperNext = false;
    }

    return out;
}

std::string
RenderSettingsKnobs::labelFromScriptName(const std::string& scriptName,
                                         const std::string& prefix)
{
    std::string name = scriptName;

    if (!prefix.empty() && (name.size() > prefix.size()) && (name.compare(0, prefix.size(), prefix) == 0)) {
        name = name.substr(prefix.size());
    }
    std::vector<std::string> words;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        const bool upper = std::isupper((unsigned char)c);
        const bool digit = std::isdigit((unsigned char)c);
        const char prev = (i > 0) ? name[i - 1] : '\0';
        const bool startsWord = words.empty() || (upper && !std::isupper((unsigned char)prev)) || (digit && !std::isdigit((unsigned char)prev)) || (!digit && std::isdigit((unsigned char)prev));
        if (startsWord) {
            words.push_back(std::string());
        }
        words.back() += c;
    }
    static const char* const acronyms[] = { "AA", "AO", "BSDF", "BSSRDF", "FPS", "GI", "GPU", "ID", "MIS", "OIDN", "RDL" };
    std::string label;
    for (const std::string& w : words) {
        std::string word = capitalized(w);
        std::string upperWord;
        for (char c : word) {
            upperWord += (char)std::toupper((unsigned char)c);
        }
        for (const char* acronym : acronyms) {
            if (upperWord == acronym) {
                word = upperWord;
            }
        }
        label += (label.empty() ? "" : " ") + word;
    }

    return label;
}

bool
RenderSettingsKnobs::isSupported(const VtValue& value)
{
    return kindOf(value) != eKindNone;
}

std::string
RenderSettingsKnobs::valueText(const VtValue& value)
{
    const KindEnum kind = kindOf(value);

    switch (kind) {
    case eKindBool:
        return value.UncheckedGet<bool>() ? "1" : "0";
    case eKindInt:
        return std::to_string(value.UncheckedGet<int>());
    case eKindUInt:
        return std::to_string(value.UncheckedGet<unsigned int>());
    case eKindFloat:
        return numberText(value.UncheckedGet<float>(), true);
    case eKindDouble:
        return numberText(value.UncheckedGet<double>(), false);
    case eKindString:
        return value.UncheckedGet<std::string>();
    case eKindToken:
        return value.UncheckedGet<TfToken>().GetString();
    case eKindNone:
        return std::string();
    default: {
        std::string text;
        for (double c : components(value)) {
            text += (text.empty() ? "" : " ") + numberText(c, isSingle(kind));
        }

        return text;
    }
    }
}

struct RenderSettingsKnobs::Impl {
    struct Setting {
        std::string renderer;
        std::string key;
        // The knob's default, of the type the delegate takes.
        VtValue knobDefault;
        // What the delegate does without the setting: sent only when the knob's value differs.
        VtValue delegateDefault;
        std::vector<Choice> choices;
        std::string follows;
        std::vector<EnabledBy> enabledBy;
        KnobIWPtr knob;
    };

    struct Renderer {
        std::string id;
        KnobPageWPtr page;
        std::vector<std::string> declared;
        std::vector<std::string> excluded;
    };

    KnobHolder* holder;
    std::vector<Setting> settings;
    std::map<const KnobI*, std::size_t> byKnob;
    std::vector<Renderer> renderers;
    bool applying = false;

    const Setting* find(const std::string& renderer,
                        const std::string& key) const
    {
        for (const Setting& s : settings) {
            if ((s.renderer == renderer) && (s.key == key)) {
                return &s;
            }
        }

        return nullptr;
    }

    VtValue resolvedChoice(const Setting& s,
                           int index) const
    {
        if ((index < 0) || (index >= (int)s.choices.size())) {
            return s.delegateDefault;
        }

        return s.choices[index].value.IsEmpty() ? s.delegateDefault : s.choices[index].value;
    }

    // The knob's value as the type the delegate takes; empty if the knob is gone or its text does not parse.
    VtValue knobValue(const Setting& s) const
    {
        KnobIPtr k = s.knob.lock();

        if (!k) {
            return VtValue();
        }
        if (!s.choices.empty()) {
            KnobChoice* choice = dynamic_cast<KnobChoice*>(k.get());

            return choice ? resolvedChoice(s, choice->getValue()) : VtValue();
        }
        const KindEnum kind = kindOf(s.knobDefault);
        switch (kind) {
        case eKindBool: {
            KnobBool* b = dynamic_cast<KnobBool*>(k.get());

            return b ? VtValue(b->getValue()) : VtValue();
        }
        case eKindInt:
        case eKindUInt: {
            KnobInt* i = dynamic_cast<KnobInt*>(k.get());
            if (!i) {
                return VtValue();
            }

            return (kind == eKindInt) ? VtValue(i->getValue()) : VtValue((unsigned int)std::max(0, i->getValue()));
        }
        case eKindFloat:
        case eKindDouble: {
            KnobDouble* d = dynamic_cast<KnobDouble*>(k.get());
            if (!d) {
                return VtValue();
            }

            return (kind == eKindFloat) ? VtValue((float)d->getValue()) : VtValue(d->getValue());
        }
        case eKindString:
        case eKindToken:
        case eKindFloatArray:
        case eKindDoubleArray:
        case eKindIntArray: {
            KnobString* str = dynamic_cast<KnobString*>(k.get());

            return str ? parseValue(str->getValue(), s.knobDefault) : VtValue();
        }
        case eKindVec2i: {
            KnobInt* i = dynamic_cast<KnobInt*>(k.get());

            return i ? VtValue(GfVec2i(i->getValue(0), i->getValue(1))) : VtValue();
        }
        case eKindVec2f:
        case eKindVec2d:
        case eKindVec3f:
        case eKindVec3d:
        case eKindVec4f:
        case eKindVec4d: {
            Knob<double>* d = dynamic_cast<Knob<double>*>(k.get());
            if (!d) {
                return VtValue();
            }
            std::vector<double> c;
            for (int dim = 0; dim < d->getDimension(); ++dim) {
                c.push_back(d->getValue(dim));
            }

            return fromComponents(s.knobDefault, c);
        }
        case eKindNone:
            break;
        }

        return VtValue();
    } // knobValue

    // Sets the knob to value, of the type the delegate takes; false if it cannot hold it.
    bool setKnobValue(const Setting& s,
                      const VtValue& value)
    {
        KnobIPtr k = s.knob.lock();

        if (!k || value.IsEmpty() || (value == knobValue(s))) {
            return (bool)k && !value.IsEmpty();
        }
        if (!s.choices.empty()) {
            KnobChoice* choice = dynamic_cast<KnobChoice*>(k.get());
            for (std::size_t i = 0; choice && i < s.choices.size(); ++i) {
                if (resolvedChoice(s, (int)i) == value) {
                    (void)choice->setValue((int)i);

                    return true;
                }
            }

            return false;
        }
        const KindEnum kind = kindOf(s.knobDefault);
        if (kindOf(value) != kind) {
            return false;
        }
        switch (kind) {
        case eKindBool: {
            KnobBool* b = dynamic_cast<KnobBool*>(k.get());
            if (b) {
                (void)b->setValue(value.UncheckedGet<bool>());
            }

            return b != nullptr;
        }
        case eKindInt:
        case eKindUInt: {
            KnobInt* i = dynamic_cast<KnobInt*>(k.get());
            if (i) {
                (void)i->setValue((kind == eKindInt) ? value.UncheckedGet<int>() : clampToInt(value.UncheckedGet<unsigned int>()));
            }

            return i != nullptr;
        }
        case eKindFloat:
        case eKindDouble: {
            KnobDouble* d = dynamic_cast<KnobDouble*>(k.get());
            if (d) {
                (void)d->setValue((kind == eKindFloat) ? floatToDouble(value.UncheckedGet<float>()) : value.UncheckedGet<double>());
            }

            return d != nullptr;
        }
        case eKindString:
        case eKindToken:
        case eKindFloatArray:
        case eKindDoubleArray:
        case eKindIntArray: {
            KnobString* str = dynamic_cast<KnobString*>(k.get());
            if (str) {
                (void)str->setValue(valueText(value));
            }

            return str != nullptr;
        }
        case eKindVec2i: {
            KnobInt* i = dynamic_cast<KnobInt*>(k.get());
            if (!i) {
                return false;
            }
            const GfVec2i& v = value.UncheckedGet<GfVec2i>();
            (void)i->setValue(v[0], ViewSpec::all(), 0);
            (void)i->setValue(v[1], ViewSpec::all(), 1);

            return true;
        }
        case eKindNone:
            return false;
        default: {
            Knob<double>* d = dynamic_cast<Knob<double>*>(k.get());
            const std::vector<double> c = components(value);
            if (!d || ((int)c.size() != d->getDimension())) {
                return false;
            }
            for (int dim = 0; dim < d->getDimension(); ++dim) {
                (void)d->setValue(c[dim], ViewSpec::all(), dim);
            }

            return true;
        }
        }
    } // setKnobValue

    // Whether the knob of the setting named key, a bool or a number, satisfies the condition; a
    // setting that is missing or neither leaves its dependents enabled.
    bool conditionHolds(const std::string& renderer,
                        const EnabledBy& condition) const
    {
        const Setting* controller = find(renderer, condition.key);
        if (!controller) {
            return true;
        }
        const VtValue v = knobValue(*controller);
        bool on = true;
        if (v.IsHolding<bool>()) {
            on = v.UncheckedGet<bool>();
        } else if (v.IsHolding<int>()) {
            on = v.UncheckedGet<int>() > 0;
        } else if (v.IsHolding<unsigned int>()) {
            on = v.UncheckedGet<unsigned int>() > 0;
        } else if (v.IsHolding<float>()) {
            on = v.UncheckedGet<float>() > 0.f;
        } else if (v.IsHolding<double>()) {
            on = v.UncheckedGet<double>() > 0.;
        } else {
            return true;
        }

        return on != condition.inverted;
    }

    // Sets the knobs of the settings that follow the one at index to its value.
    void syncFollowers(std::size_t index)
    {
        const Setting& leader = settings[index];

        for (const Setting& s : settings) {
            if ((s.renderer == leader.renderer) && (s.follows == leader.key)) {
                applying = true;
                (void)setKnobValue(s, knobValue(leader));
                applying = false;
            }
        }
    }

    KnobIPtr makeKnob(const std::string& label,
                      const VtValue& prototype,
                      const std::vector<Choice>& choices)
    {
        if (!choices.empty()) {
            KnobChoicePtr k = AppManager::createKnob<KnobChoice>(holder, label);
            std::vector<ChoiceOption> options;
            int defaultIndex = 0;
            for (std::size_t i = 0; i < choices.size(); ++i) {
                options.push_back(ChoiceOption(choices[i].id, choices[i].label, ""));
                if (choices[i].value.IsEmpty() || (choices[i].value == prototype)) {
                    defaultIndex = (int)i;
                }
            }
            (void)k->populateChoices(options);
            k->setDefaultValue(defaultIndex);

            return k;
        }
        switch (kindOf(prototype)) {
        case eKindBool: {
            KnobBoolPtr k = AppManager::createKnob<KnobBool>(holder, label);
            k->setDefaultValue(prototype.UncheckedGet<bool>());

            return k;
        }
        case eKindInt:
        case eKindUInt: {
            KnobIntPtr k = AppManager::createKnob<KnobInt>(holder, label);
            if (prototype.IsHolding<int>()) {
                k->setDefaultValue(prototype.UncheckedGet<int>());
            } else {
                k->setMinimum(0);
                k->setDefaultValue(clampToInt(prototype.UncheckedGet<unsigned int>()));
            }

            return k;
        }
        case eKindFloat:
        case eKindDouble: {
            KnobDoublePtr k = AppManager::createKnob<KnobDouble>(holder, label);
            k->setDefaultValue(prototype.IsHolding<float>() ? floatToDouble(prototype.UncheckedGet<float>()) : prototype.UncheckedGet<double>());

            return k;
        }
        case eKindString:
        case eKindToken:
        case eKindFloatArray:
        case eKindDoubleArray:
        case eKindIntArray: {
            KnobStringPtr k = AppManager::createKnob<KnobString>(holder, label);
            k->setDefaultValue(valueText(prototype));

            return k;
        }
        case eKindVec2i: {
            KnobIntPtr k = AppManager::createKnob<KnobInt>(holder, label, 2);
            const GfVec2i& v = prototype.UncheckedGet<GfVec2i>();
            k->setDefaultValue(v[0], 0);
            k->setDefaultValue(v[1], 1);

            return k;
        }
        case eKindVec2f:
        case eKindVec2d: {
            KnobDoublePtr k = AppManager::createKnob<KnobDouble>(holder, label, 2);
            const std::vector<double> c = components(prototype);
            k->setDefaultValue(c[0], 0);
            k->setDefaultValue(c[1], 1);

            return k;
        }
        case eKindVec3f:
        case eKindVec3d:
        case eKindVec4f:
        case eKindVec4d: {
            const std::vector<double> c = components(prototype);
            KnobColorPtr k = AppManager::createKnob<KnobColor>(holder, label, (int)c.size());
            for (std::size_t dim = 0; dim < c.size(); ++dim) {
                k->setDefaultValue(c[dim], (int)dim);
            }

            return k;
        }
        case eKindNone:
            break;
        }

        return KnobIPtr();
    } // makeKnob
};

RenderSettingsKnobs::RenderSettingsKnobs(KnobHolder* holder)
    : _imp(new Impl)
{
    _imp->holder = holder;
}

RenderSettingsKnobs::~RenderSettingsKnobs()
{
}

void
RenderSettingsKnobs::addRenderer(const std::string& rendererId,
                                 const DescriptorList& declared,
                                 const Curated* curated)
{
    Impl::Renderer renderer;
    const std::string prefix = scriptPrefix(rendererId);
    std::set<std::string> names;
    std::set<std::string> handled;
    std::map<std::string, std::string> disabled;

    renderer.id = rendererId;
    const std::vector<std::string> namespaces = RenderSettingsPrim::namespaces(rendererId);
    RenderDelegates::Delegate delegate;
    std::string pageLabel = RenderDelegates::find(rendererId, &delegate) ? delegate.displayName : labelFromScriptName(prefix, std::string());
    if (curated && !curated->pageLabel.empty()) {
        pageLabel = curated->pageLabel;
    }
    KnobPagePtr page = AppManager::createKnob<KnobPage>(_imp->holder, pageLabel);
    page->setName(prefix);
    renderer.page = page;
    names.insert(prefix);
    names.insert(prefix + "Settings");
    names.insert(prefix + "OtherSettings");
    for (const Descriptor& d : declared) {
        renderer.declared.push_back(d.key);
    }
    if (curated) {
        renderer.excluded = curated->excludedKeys;
        handled.insert(curated->excludedKeys.begin(), curated->excludedKeys.end());
        for (const std::pair<std::string, std::string>& d : curated->disabledKeys) {
            disabled.insert(d);
        }
    }

    const auto addSetting = [&](const KnobGroupPtr& group,
                                const std::string& key,
                                std::string scriptName,
                                const std::string& tooltip,
                                const VtValue& knobDefault,
                                const VtValue& delegateDefault,
                                const std::vector<Choice>& choices,
                                const std::string& follows,
                                const std::string& disabledReason,
                                const CuratedSetting* range) {
        if (names.count(scriptName)) {
            scriptName = prefix + capitalized(camelCaseFromKey(key, true));
        }
        for (int n = 2; names.count(scriptName); ++n) {
            scriptName = prefix + capitalized(camelCaseFromKey(key, true)) + std::to_string(n);
        }
        names.insert(scriptName);
        KnobIPtr k = _imp->makeKnob(labelFromScriptName(scriptName, prefix), knobDefault, choices);
        if (!k) {
            return;
        }
        k->setName(scriptName);
        std::string hint = tooltip + "\n\nAuthored as " + RenderSettingsPrim::attributeName(rendererId, key) + " on the render settings prim, and sent to the renderer as " + key + ".";
        if (!disabledReason.empty()) {
            hint += "\n\n" + disabledReason;
            k->setDefaultAllDimensionsEnabled(false);
            k->setAllDimensionsEnabled(false);
        }
        k->setHintToolTip(hint);
        k->setAnimationEnabled(false);
        // Hashed with the namespace it is authored in, so that only a render with this renderer
        // re-keys on it (Node::getHashNamespaces()).
        const std::string attribute = RenderSettingsPrim::attributeName(rendererId, key);
        std::string hashNamespace = namespaces.empty() ? prefix : namespaces.front();
        for (const std::string& ns : namespaces) {
            if (attribute.compare(0, ns.size() + 1, ns + ":") == 0) {
                hashNamespace = ns;
                break;
            }
        }
        k->setHashNamespace(hashNamespace);
        if (range && range->hasRange) {
            if (KnobInt* i = dynamic_cast<KnobInt*>(k.get())) {
                i->setMinimum(clampToInt(range->minimum));
                i->setDisplayMinimum(clampToInt(range->minimum));
                i->setDisplayMaximum(clampToInt(range->displayMaximum));
                if (range->hasMaximum) {
                    i->setMaximum(clampToInt(range->maximum));
                }
            } else if (KnobDouble* d = dynamic_cast<KnobDouble*>(k.get())) {
                d->setMinimum(range->minimum);
                d->setDisplayMinimum(range->minimum);
                d->setDisplayMaximum(range->displayMaximum);
                if (range->hasMaximum) {
                    d->setMaximum(range->maximum);
                }
            }
        }
        if (range && range->sameRowAsNext) {
            k->setAddNewLine(false);
        }
        group->addKnob(k);
        Impl::Setting setting;
        setting.renderer = rendererId;
        setting.key = key;
        setting.knobDefault = knobDefault;
        setting.delegateDefault = delegateDefault;
        setting.choices = choices;
        setting.follows = follows;
        if (range && disabledReason.empty()) {
            setting.enabledBy = range->enabledBy;
        }
        setting.knob = k;
        _imp->byKnob[k.get()] = _imp->settings.size();
        _imp->settings.push_back(setting);
    };

    if (curated) {
        const std::string defaultGroupName = prefix + "Settings";
        std::map<std::string, KnobGroupPtr> made;
        KnobGroupPtr defaultGroup;
        std::string groupsError;
        const bool useGroups = validateGroups(*curated, &groupsError);
        std::map<std::string, const GroupSpec*> specs;
        if (useGroups) {
            for (const GroupSpec& g : curated->groups) {
                specs[g.name] = &g;
                names.insert(prefix + capitalized(g.name));
            }
        } else if (!groupsError.empty()) {
            std::fprintf(stderr, "Render settings of %s: %s; its groups are ignored\n", rendererId.c_str(), groupsError.c_str());
        }

        const std::function<KnobGroupPtr(const std::string&)> groupFor = [&](const std::string& name) -> KnobGroupPtr {
            if (name.empty() || !specs.count(name)) {
                if (!defaultGroup) {
                    defaultGroup = AppManager::createKnob<KnobGroup>(_imp->holder, labelFromScriptName(defaultGroupName, std::string()));
                    defaultGroup->setName(defaultGroupName);
                    defaultGroup->setHintToolTip(curated->tooltip);
                    defaultGroup->setDefaultValue(true);
                    page->addKnob(defaultGroup);
                }

                return defaultGroup;
            }
            std::map<std::string, KnobGroupPtr>::const_iterator found = made.find(name);
            if (found != made.end()) {
                return found->second;
            }
            const GroupSpec& spec = *specs[name];
            const std::string scriptName = prefix + capitalized(spec.name);
            KnobGroupPtr group = AppManager::createKnob<KnobGroup>(_imp->holder, labelFromScriptName(scriptName, prefix));
            group->setName(scriptName);
            group->setHintToolTip(spec.tooltip);
            group->setDefaultValue(spec.open);
            if (spec.parent.empty()) {
                page->addKnob(group);
            } else {
                groupFor(spec.parent)->addKnob(group);
            }
            made[name] = group;

            return group;
        };

        for (const CuratedSetting& c : curated->settings) {
            VtValue delegateDefault = c.defaultValue;
            for (const Descriptor& d : declared) {
                if ((d.key == c.key) && (kindOf(d.defaultValue) == kindOf(c.defaultValue))) {
                    delegateDefault = d.defaultValue;
                }
            }
            VtValue knobDefault = c.overrideDefault ? c.defaultValue : delegateDefault;
            // A follower starts where its leader, listed before it, does.
            const Impl::Setting* leader = c.follows.empty() ? nullptr : _imp->find(rendererId, c.follows);
            if (leader && (kindOf(leader->knobDefault) == kindOf(knobDefault))) {
                knobDefault = leader->knobDefault;
            }
            addSetting(groupFor(c.group), c.key, prefix + capitalized(c.name), c.tooltip, knobDefault, delegateDefault,
                       c.choices, c.follows, c.disabledReason, &c);
            handled.insert(c.key);
        }
    }

    std::vector<const Descriptor*> leftover;
    for (const Descriptor& d : declared) {
        if (!handled.count(d.key) && isSupported(d.defaultValue)) {
            leftover.push_back(&d);
        }
    }

    // A renderer with a table has laid itself out; the others are sorted by what their keys say.
    const bool allOpen = leftover.size() <= 20;
    std::map<std::string, std::vector<const Descriptor*>> classified;
    if (!curated) {
        std::set<std::string> natural;
        for (const Descriptor* d : leftover) {
            natural.insert(prefix + capitalized(camelCaseFromKey(d->key, false)));
        }
        std::vector<const Descriptor*> unclaimed;
        for (const Descriptor* d : leftover) {
            const std::string group = classifyKey(d->key);
            if (!group.empty() && !natural.count(prefix + capitalized(group)) && !names.count(prefix + capitalized(group))) {
                classified[group].push_back(d);
            } else {
                unclaimed.push_back(d);
            }
        }
        leftover = unclaimed;
    }
    bool first = true;
    for (const char* groupName : kClassifiedOrder) {
        std::map<std::string, std::vector<const Descriptor*>>::const_iterator members = classified.find(groupName);
        if (members == classified.end()) {
            continue;
        }
        const std::string scriptName = prefix + capitalized(groupName);
        names.insert(scriptName);
        KnobGroupPtr group = AppManager::createKnob<KnobGroup>(_imp->holder, labelFromScriptName(scriptName, prefix));
        group->setName(scriptName);
        group->setHintToolTip(std::string(classifiedTooltip(groupName)));
        group->setDefaultValue(allOpen || first);
        page->addKnob(group);
        first = false;
        for (const Descriptor* d : members->second) {
            std::map<std::string, std::string>::const_iterator off = disabled.find(d->key);
            addSetting(group, d->key, prefix + capitalized(camelCaseFromKey(d->key, false)), d->name, d->defaultValue, d->defaultValue,
                       std::vector<Choice>(), std::string(), (off == disabled.end()) ? std::string() : off->second, nullptr);
            handled.insert(d->key);
        }
    }

    KnobGroupPtr other;
    for (const Descriptor* dp : leftover) {
        const Descriptor& d = *dp;
        if (!other) {
            const std::string groupName = prefix + "OtherSettings";
            other = AppManager::createKnob<KnobGroup>(_imp->holder, labelFromScriptName(groupName, prefix));
            other->setName(groupName);
            other->setHintToolTip(std::string("Every other setting the renderer declares, as it names them."));
            other->setDefaultValue(false);
            page->addKnob(other);
        }
        std::map<std::string, std::string>::const_iterator off = disabled.find(d.key);
        addSetting(other, d.key, prefix + capitalized(camelCaseFromKey(d.key, false)), d.name, d.defaultValue, d.defaultValue,
                   std::vector<Choice>(), std::string(), (off == disabled.end()) ? std::string() : off->second, nullptr);
        handled.insert(d.key);
    }
    // The settings panel shows no tab for a page without knobs.
    if (page->getChildren().empty()) {
        KnobStringPtr note = AppManager::createKnob<KnobString>(_imp->holder, std::string());
        note->setName(prefix + "NoSettings");
        note->setDefaultValue(pageLabel + " has no settings.");
        note->setAsLabel();
        note->setAnimationEnabled(false);
        note->setEvaluateOnChange(false);
        note->setIsPersistent(false);
        page->addKnob(note);
    }
    _imp->renderers.push_back(renderer);
    refreshEnabled();
} // RenderSettingsKnobs::addRenderer

KnobPagePtr
RenderSettingsKnobs::page(const std::string& rendererId) const
{
    for (const Impl::Renderer& r : _imp->renderers) {
        if (r.id == rendererId) {
            return r.page.lock();
        }
    }

    return KnobPagePtr();
}

bool
RenderSettingsKnobs::knobChanged(KnobI* knob)
{
    std::map<const KnobI*, std::size_t>::const_iterator found = _imp->byKnob.find(knob);
    if (found == _imp->byKnob.end()) {
        return false;
    }
    if (!_imp->applying) {
        _imp->syncFollowers(found->second);
    }
    refreshEnabled();

    return true;
}

void
RenderSettingsKnobs::refreshEnabled()
{
    for (const Impl::Setting& s : _imp->settings) {
        if (s.enabledBy.empty()) {
            continue;
        }
        KnobIPtr k = s.knob.lock();
        if (!k) {
            continue;
        }
        bool enabled = true;
        for (const EnabledBy& condition : s.enabledBy) {
            if (!_imp->conditionHolds(s.renderer, condition)) {
                enabled = false;
                break;
            }
        }
        k->setAllDimensionsEnabled(enabled);
    }
}

RenderSettingsKnobs::Values
RenderSettingsKnobs::values(const std::string& rendererId) const
{
    Values out;

    for (const Impl::Setting& s : _imp->settings) {
        if (s.renderer != rendererId) {
            continue;
        }
        const VtValue v = _imp->knobValue(s);
        if (!v.IsEmpty() && (v != s.delegateDefault)) {
            out[s.key] = v;
        }
    }

    return out;
}

KnobIPtr
RenderSettingsKnobs::knob(const std::string& rendererId,
                          const std::string& key) const
{
    const Impl::Setting* s = _imp->find(rendererId, key);

    return s ? s->knob.lock() : KnobIPtr();
}

std::vector<std::pair<std::string, std::string>>
RenderSettingsKnobs::audit(const std::string& rendererId) const
{
    std::vector<std::pair<std::string, std::string>> out;

    for (const Impl::Renderer& r : _imp->renderers) {
        if (r.id != rendererId) {
            continue;
        }
        for (const std::string& key : r.declared) {
            std::string reach;
            if (std::find(r.excluded.begin(), r.excluded.end(), key) != r.excluded.end()) {
                reach = "(set by Natron)";
            } else if (KnobIPtr k = knob(rendererId, key)) {
                reach = k->getName();
            }
            out.push_back(std::make_pair(key, reach));
        }
    }

    return out;
}

bool
RenderSettingsKnobs::author(const UsdPrim& prim,
                            std::string* errorMessage) const
{
    for (const Impl::Renderer& r : _imp->renderers) {
        for (const std::pair<const std::string, VtValue>& setting : values(r.id)) {
            const std::string name = RenderSettingsPrim::attributeName(r.id, setting.first);
            const SdfValueTypeName type = SdfSchema::GetInstance().FindType(setting.second);
            UsdAttribute attribute;
            if (type && SdfPath::IsValidNamespacedIdentifier(name)) {
                attribute = prim.CreateAttribute(TfToken(name), type, true);
            }
            if (!attribute || !attribute.Set(setting.second)) {
                *errorMessage = "USD could not author the setting " + setting.first + " of " + r.id + " as " + name + " on " + prim.GetPath().GetString() + ".";

                return false;
            }
        }
    }

    return true;
}

NATRON_NAMESPACE_EXIT
