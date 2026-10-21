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

#ifndef NATRON_TESTS_HASHSWEEPEDITS_H
#define NATRON_TESTS_HASHSWEEPEDITS_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Knob.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Plugin.h"
#include "Engine/Project.h"
#include "Engine/ProjectColorManagement.h"
#include "Engine/ViewIdx.h"

#include "TestOfxPluginPath.h"

NATRON_NAMESPACE_ENTER

// The plugin enumeration, knob perturbation and exemption allowlist shared by the sweeps that
// edit every render-hashed knob of every bundled plugin.
namespace HashSweep {

const char* const kAllowlistPath = NATRON_TESTS_FIXTURES_DIR "/../hash_exempt_allowlist.txt";
const char* const kWholePluginWildcard = "*";

// Plugins wmain.cpp registers for other suites; they are not shipped.
const char* const kTestPluginIDPrefix = "test.natron.";

// A path in a directory that does not exist: readers and file-driven knobs give up on it at once
// instead of scanning or decoding anything.
const char* const kSweepFilePathA = "/nonexistent-natron-hash-sweep/a/sweep.exr";
const char* const kSweepFilePathB = "/nonexistent-natron-hash-sweep/b/sweep.exr";

struct PluginVersion {
    std::string id;
    int major;
    int minor;
    bool isOfx;

    std::string describe() const
    {
        std::ostringstream ss;
        ss << id << " v" << major << "." << minor;

        return ss.str();
    }
};

struct AllowlistEntry {
    std::string pluginID;
    std::string knobName;
    std::string reason;
    int line;
};

struct Allowlist {
    std::vector<AllowlistEntry> entries;
    std::vector<std::string> errors;

    bool exemptsPlugin(const std::string& pluginID) const
    {
        return exempts(pluginID, kWholePluginWildcard);
    }

    bool exempts(const std::string& pluginID,
                 const std::string& knobName) const
    {
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if ((entries[i].pluginID == pluginID) && (entries[i].knobName == knobName)) {
                return true;
            }
        }

        return false;
    }
};

inline std::string
trimmed(const std::string& s)
{
    const char* const ws = " \t\r\n";
    const std::size_t first = s.find_first_not_of(ws);
    if (first == std::string::npos) {
        return std::string();
    }
    const std::size_t last = s.find_last_not_of(ws);

    return s.substr(first, last - first + 1);
}

inline Allowlist
loadAllowlist()
{
    Allowlist list;
    std::ifstream in(kAllowlistPath);

    if (!in) {
        list.errors.push_back(std::string("cannot open ") + kAllowlistPath);

        return list;
    }
    std::string raw;
    int lineNumber = 0;
    while (std::getline(in, raw)) {
        ++lineNumber;
        const std::string line = trimmed(raw);
        if (line.empty() || (line[0] == '#')) {
            continue;
        }
        std::ostringstream where;
        where << kAllowlistPath << ":" << lineNumber << ": ";
        const std::size_t hash = line.find('#');
        const std::string reason = hash == std::string::npos ? std::string() : trimmed(line.substr(hash + 1));
        if (reason.empty()) {
            list.errors.push_back(where.str() + "an exemption needs a reason after '#'");
            continue;
        }
        std::istringstream fields(line.substr(0, hash));
        AllowlistEntry entry;
        std::string extra;
        if (!(fields >> entry.pluginID >> entry.knobName) || (fields >> extra)) {
            list.errors.push_back(where.str() + "expected 'pluginID knobName  # reason'");
            continue;
        }
        entry.reason = reason;
        entry.line = lineNumber;
        list.entries.push_back(entry);
    }

    return list;
}

inline std::vector<PluginVersion>
sweptPlugins()
{
    std::vector<PluginVersion> ret;
    const PluginsMap& plugins = appPTR->getPluginsList();

    for (PluginsMap::const_iterator it = plugins.begin(); it != plugins.end(); ++it) {
        if (it->first.compare(0, std::string(kTestPluginIDPrefix).size(), kTestPluginIDPrefix) == 0) {
            continue;
        }
        for (PluginVersionsOrdered::const_iterator v = it->second.begin(); v != it->second.end(); ++v) {
            // Internal-only plugins (the paint stroke nodes, the OpenFX Roto) only exist inside
            // the node that owns them and cannot be built on their own.
            if ((*v)->getIsForInternalUseOnly()) {
                continue;
            }
            PluginVersion p;
            p.id = it->first;
            p.major = (*v)->getMajorVersion();
            p.minor = (*v)->getMinorVersion();
            p.isOfx = (*v)->getOfxPlugin() != NULL;
            ret.push_back(p);
        }
    }

    return ret;
}

inline bool
ofxBundleExpected()
{
    return !userOfxPluginPath().isEmpty();
}

inline std::string
knobLabel(const KnobIPtr& knob,
          int dimension)
{
    std::ostringstream ss;
    ss << "knob '" << knob->getName() << "' (" << knob->typeName();
    if (dimension >= 0) {
        ss << ", dim " << dimension;
    }
    ss << ")";

    return ss.str();
}

struct KnobEdit {
    std::function<bool()> apply;
    std::function<void()> revert;
    std::function<bool()> valueMoved;
    std::string what;
};

template <typename T>
std::string
valueString(const T& v)
{
    std::ostringstream ss;
    ss.precision(17);
    ss << v;

    return ss.str();
}

inline bool
pickIntTarget(int value,
              int mini,
              int maxi,
              int* target)
{
    if (value < maxi) {
        *target = value + 1;

        return true;
    }
    if (value > mini) {
        *target = value - 1;

        return true;
    }

    return false;
}

inline bool
pickDoubleTarget(double value,
                 double mini,
                 double maxi,
                 double* target)
{
    const double candidates[] = {
        value + 1., value - 1., mini / 2. + maxi / 2., value / 2. + mini / 2., value / 2. + maxi / 2.
    };

    for (std::size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        const double c = candidates[i];
        if (std::isfinite(c) && (c != value) && (c >= mini) && (c <= maxi)) {
            *target = c;

            return true;
        }
    }

    return false;
}

template <typename T>
void
setStatic(Knob<T>* knob,
          const T& value,
          int dimension)
{
    // Auto-keying off: a keyframe would leave a curve behind that the revert does not remove.
    ignore_result(knob->setValue(value, ViewSpec::all(), dimension, true));
}

template <typename T>
void
makeValueEdit(Knob<T>* knob,
              int dimension,
              const T& original,
              const T& target,
              KnobEdit* edit)
{
    edit->apply = [knob, dimension, target]() {
        setStatic<T>(knob, target, dimension);

        return true;
    };
    edit->revert = [knob, dimension, original]() {
        setStatic<T>(knob, original, dimension);
    };
    edit->valueMoved = [knob, dimension, original]() {
        return knob->getValue(dimension, ViewSpec::current(), false) != original;
    };
    edit->what = valueString(original) + " -> " + valueString(target);
}

inline bool
isOCIOColorSpaceKnob(const std::string& name)
{
    return (name == "ocioInputSpace") || (name == "ocioOutputSpace");
}

// openfx-io's GenericOCIO resets a name the config does not know to its default colourspace,
// and folds any name back to the role its colourspace plays (scene_linear, ...). A sticking
// edit therefore has to name a colourspace other than the one the current value resolves to.
inline bool
pickOtherColorSpace(const std::string& original,
                    std::string* target)
{
    AppInstancePtr app = appPTR->getTopLevelInstance();
    if (!app) {
        return false;
    }
    ProjectColorManagementPtr colorManagement = app->getProject()->getColorManagement();
    if (!colorManagement) {
        return false;
    }
    OCIO_NAMESPACE::ConstConfigRcPtr config = colorManagement->getConfig();
    if (!config) {
        return false;
    }
    const int originalIndex = config->getIndexForColorSpace(original.c_str());
    const std::vector<std::string> names = colorManagement->getColorSpaces();
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (!names[i].empty() && (config->getIndexForColorSpace(names[i].c_str()) != originalIndex)) {
            *target = names[i];

            return true;
        }
    }

    return false;
}

/**
 * @brief Why the static value of this dimension is not what the node renders with, or an empty
 * string when it is. Expressions, links and curves have their own content-hash coverage.
 **/
inline std::string
staticValueSkipReason(const KnobIPtr& knob,
                      int dimension)
{
    if (!knob->getExpression(dimension).empty()) {
        return "has an expression";
    }
    if (knob->isSlave(dimension)) {
        return "is slaved";
    }
    if (knob->isAnimated(dimension)) {
        return "is animated";
    }

    return std::string();
}

/**
 * @brief Builds a reversible edit that changes the content of one dimension of the knob.
 * @returns An empty string, or why this dimension cannot be perturbed (a skip, not a failure).
 * Sets *unsupported when the knob type is one the sweep does not know how to edit.
 **/
inline std::string
makeEdit(const KnobIPtr& knob,
         int dimension,
         KnobEdit* edit,
         bool* unsupported)
{
    *unsupported = false;
    KnobI* k = knob.get();

    if (KnobParametric* parametric = dynamic_cast<KnobParametric*>(k)) {
        int nPoints = 0;
        if (parametric->getNControlPoints(dimension, &nPoints) != eStatusOK) {
            return "cannot read its control points";
        }
        // The derivative-free setter lets the curve recompute smooth tangents both ways, so the
        // revert lands on exactly the original keyframe.
        if (nPoints > 0) {
            double key = 0., value = 0.;
            if (parametric->getNthControlPoint(dimension, 0, &key, &value) != eStatusOK) {
                return "cannot read its first control point";
            }
            edit->apply = [parametric, dimension, key, value]() {
                return parametric->setNthControlPoint(eValueChangedReasonNatronInternalEdited, dimension, 0, key, value + 0.25) == eStatusOK;
            };
            edit->revert = [parametric, dimension, key, value]() {
                ignore_result(parametric->setNthControlPoint(eValueChangedReasonNatronInternalEdited, dimension, 0, key, value));
            };
            edit->what = "first control point value " + valueString(value) + " -> " + valueString(value + 0.25);
        } else {
            const std::pair<double, double> range = parametric->getParametricRange();
            const double key = range.first / 2. + range.second / 2.;
            edit->apply = [parametric, dimension, key]() {
                return parametric->addControlPoint(eValueChangedReasonNatronInternalEdited, dimension, key, 0.5) == eStatusOK;
            };
            edit->revert = [parametric, dimension]() {
                ignore_result(parametric->deleteControlPoint(eValueChangedReasonNatronInternalEdited, dimension, 0));
            };
            edit->what = "control point added at " + valueString(key);
        }
        // A successful setter is the value moving; there is no separate static value to compare.
        edit->valueMoved = []() {
            return true;
        };

        return std::string();
    }

    if (KnobChannelSelect* channel = dynamic_cast<KnobChannelSelect*>(k)) {
        const std::string current = channel->get();
        const std::string targetChannel = current == kNatronColorViewRGBA ".R" ? kNatronColorViewRGBA ".G" : kNatronColorViewRGBA ".R";
        makeValueEdit<std::string>(channel, dimension, channel->getValue(dimension, ViewSpec::current(), false), channel->encode(targetChannel), edit);
        edit->what = (current.empty() ? std::string("None") : current) + " -> " + targetChannel;

        return std::string();
    }

    if (KnobLayerSelect* layer = dynamic_cast<KnobLayerSelect*>(k)) {
        const std::string current = layer->getLayer();
        const std::string targetLayer = current == kNatronColorViewRGBA ? kNatronColorViewAlpha : kNatronColorViewRGBA;
        makeValueEdit<std::string>(layer, dimension, layer->getValue(dimension, ViewSpec::current(), false), layer->encode(targetLayer, layer->getChannels()), edit);
        edit->what = (current.empty() ? std::string("None") : current) + " -> " + targetLayer;

        return std::string();
    }

    if (dynamic_cast<KnobTable*>(k)) {
        // The remaining table knobs (paths, layers, channel sets, shuffle maps) store an encoded
        // string whose valid forms differ per type, so a generic edit would be rejected or normalized.
        return "table knob, encoded content has no generic edit";
    }

    if (KnobChoice* choice = dynamic_cast<KnobChoice*>(k)) {
        const int nEntries = choice->getNumEntries();
        if (nEntries < 2) {
            return "choice with fewer than two entries";
        }
        const int original = choice->getValue(dimension, ViewSpec::current(), false);
        const int target = ((original >= 0) && (original < nEntries)) ? (original + 1) % nEntries : 0;
        makeValueEdit<int>(choice, dimension, original, target, edit);

        return std::string();
    }

    if (KnobBoolBase* b = dynamic_cast<KnobBoolBase*>(k)) {
        const bool original = b->getValue(dimension, ViewSpec::current(), false);
        makeValueEdit<bool>(b, dimension, original, !original, edit);

        return std::string();
    }

    if (KnobIntBase* i = dynamic_cast<KnobIntBase*>(k)) {
        const int original = i->getValue(dimension, ViewSpec::current(), false);
        int target = original;
        if (!pickIntTarget(original, i->getMinimum(dimension), i->getMaximum(dimension), &target)) {
            return "minimum equals maximum";
        }
        makeValueEdit<int>(i, dimension, original, target, edit);

        return std::string();
    }

    if (KnobDoubleBase* d = dynamic_cast<KnobDoubleBase*>(k)) {
        const double original = d->getValue(dimension, ViewSpec::current(), false);
        double target = original;
        if (!pickDoubleTarget(original, d->getMinimum(dimension), d->getMaximum(dimension), &target)) {
            return "no other value inside [minimum, maximum]";
        }
        makeValueEdit<double>(d, dimension, original, target, edit);

        return std::string();
    }

    if (KnobStringBase* s = dynamic_cast<KnobStringBase*>(k)) {
        const std::string original = s->getValue(dimension, ViewSpec::current(), false);
        std::string target;
        if (dynamic_cast<KnobFile*>(k) || dynamic_cast<KnobOutputFile*>(k)) {
            target = original == kSweepFilePathA ? kSweepFilePathB : kSweepFilePathA;
        } else if (isOCIOColorSpaceKnob(knob->getName())) {
            if (!pickOtherColorSpace(original, &target)) {
                return "no other colourspace in the project's OpenColorIO config";
            }
        } else {
            target = original + "_hashsweep";
        }
        makeValueEdit<std::string>(s, dimension, original, target, edit);

        return std::string();
    }

    *unsupported = true;

    return "unsupported knob type";
} // makeEdit

} // namespace HashSweep

NATRON_NAMESPACE_EXIT

#endif // NATRON_TESTS_HASHSWEEPEDITS_H
