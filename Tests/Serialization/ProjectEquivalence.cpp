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

#include "ProjectEquivalence.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <list>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "Engine/Curve.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/LayerRegistry.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Plugin.h"
#include "Engine/Project.h"
#include "Engine/TrackMarker.h"
#include "Engine/TrackerContext.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

namespace {

// Where and when the file was saved, not what it holds: these differ between two saves of the
// same project under different names.
const char* const kSaveMetadataKnobs[] = { "projectName", "projectPath", "softwareVersion", "lastAuthor", "lastSaveDate", "creationDate" };

const std::size_t kMaxReportedDifferences = 40;

std::string
exact(double value)
{
    char text[64];
    std::snprintf(text, sizeof(text), "%.17g/0x%016llx", value, (unsigned long long)std::bit_cast<std::uint64_t>(value));

    return std::string(text);
}

std::string
curveText(const Curve& curve)
{
    std::ostringstream text;
    const KeyFrameSet keys = curve.getKeyFrames_mt_safe();
    text << keys.size() << " keys";
    for (KeyFrameSet::const_iterator it = keys.begin(); it != keys.end(); ++it) {
        text << " [t=" << exact(it->getTime()) << " v=" << exact(it->getValue()) << " l=" << exact(it->getLeftDerivative()) << " r=" << exact(it->getRightDerivative()) << " i=" << (int)it->getInterpolation() << ']';
    }

    return text.str();
}

std::string
linkTargetText(const KnobIPtr& master,
               int masterDimension)
{
    std::string holderName = "?";
    KnobHolder* holder = master->getHolder();
    if (EffectInstance* effect = dynamic_cast<EffectInstance*>(holder)) {
        if (NodePtr node = effect->getNode()) {
            holderName = node->getFullyQualifiedName();
        }
    } else if (TrackMarker* track = dynamic_cast<TrackMarker*>(holder)) {
        TrackerContextPtr context = track->getContext();
        NodePtr node = context ? context->getNode() : NodePtr();
        holderName = (node ? node->getFullyQualifiedName() : std::string("?")) + " track " + track->getScriptName_mt_safe();
    }

    return holderName + "." + master->getName() + "." + std::to_string(masterDimension);
}

template <typename T>
std::string
rawValueText(const Knob<T>& knob,
             int dimension);

template <>
std::string
rawValueText<double>(const Knob<double>& knob,
                     int dimension)
{
    return "double " + exact(knob.getRawValue(dimension));
}

template <>
std::string
rawValueText<int>(const Knob<int>& knob,
                  int dimension)
{
    return "int " + std::to_string(knob.getRawValue(dimension));
}

template <>
std::string
rawValueText<bool>(const Knob<bool>& knob,
                   int dimension)
{
    return knob.getRawValue(dimension) ? "bool true" : "bool false";
}

template <>
std::string
rawValueText<std::string>(const Knob<std::string>& knob,
                          int dimension)
{
    return "string \"" + knob.getRawValue(dimension) + "\"";
}

class SnapshotBuilder {
public:
    explicit SnapshotBuilder(ProjectSnapshot* snapshot)
        : _snapshot(snapshot)
    {
    }

    void add(const std::string& key,
             const std::string& value)
    {
        _snapshot->entries[key] = value;
    }

    void addKnob(const std::string& prefix,
                 const KnobIPtr& knob)
    {
        if (!knob->getIsPersistent()) {
            return;
        }
        if (dynamic_cast<KnobPage*>(knob.get()) || dynamic_cast<KnobGroup*>(knob.get()) || dynamic_cast<KnobButton*>(knob.get()) || dynamic_cast<KnobSeparator*>(knob.get())) {
            return;
        }
        const std::string knobKey = prefix + " knob " + knob->getName();
        add(knobKey, knob->typeName() + " x" + std::to_string(knob->getDimension()));

        for (int d = 0; d < knob->getDimension(); ++d) {
            const std::string key = knobKey + "[" + std::to_string(d) + "]";
            if (Knob<double>* isDouble = dynamic_cast<Knob<double>*>(knob.get())) {
                add(key + " value", rawValueText(*isDouble, d));
            } else if (Knob<int>* isInt = dynamic_cast<Knob<int>*>(knob.get())) {
                add(key + " value", rawValueText(*isInt, d));
            } else if (Knob<bool>* isBool = dynamic_cast<Knob<bool>*>(knob.get())) {
                add(key + " value", rawValueText(*isBool, d));
            } else if (Knob<std::string>* isString = dynamic_cast<Knob<std::string>*>(knob.get())) {
                add(key + " value", rawValueText(*isString, d));
            }

            CurvePtr curve = knob->getCurve(ViewSpec(0), d, true);
            if (curve && (curve->getKeyFramesCount() > 0)) {
                add(key + " curve", curveText(*curve));
                if (Knob<std::string>* isString = dynamic_cast<Knob<std::string>*>(knob.get())) {
                    const KeyFrameSet keys = curve->getKeyFrames_mt_safe();
                    std::string strings;
                    for (KeyFrameSet::const_iterator it = keys.begin(); it != keys.end(); ++it) {
                        strings += " [" + exact(it->getTime()) + " \"" + isString->getValueAtTime(it->getTime(), d, ViewSpec(0), false, true) + "\"]";
                    }
                    add(key + " strings", strings);
                }
            }

            const std::string expression = knob->getExpression(d);
            if (!expression.empty()) {
                add(key + " expression", expression + (knob->isExpressionUsingRetVariable(d) ? " (ret)" : ""));
            }

            const std::pair<int, KnobIPtr> master = knob->getMaster(d);
            if (master.second) {
                add(key + " link", linkTargetText(master.second, master.first));
            }

            if (KnobParametric* isParametric = dynamic_cast<KnobParametric*>(knob.get())) {
                CurvePtr parametric = isParametric->getParametricCurve(d);
                if (parametric) {
                    add(key + " parametric", curveText(*parametric));
                }
            }
        }

        if (KnobChoice* isChoice = dynamic_cast<KnobChoice*>(knob.get())) {
            add(knobKey + " option", isChoice->getActiveEntry().id);
        }
    }

    void addNodes(const NodeCollection& collection)
    {
        NodesList nodes;
        collection.getActiveNodes(&nodes);
        for (NodesList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
            if ((*it)->isPartOfProject()) {
                addNode(*it);
            }
        }
    }

    void addNode(const NodePtr& node)
    {
        const std::string prefix = "node " + node->getFullyQualifiedName();
        const Plugin* plugin = node->getPlugin();
        add(prefix, node->getPluginID() + (plugin ? " " + std::to_string(plugin->getMajorVersion()) + "." + std::to_string(plugin->getMinorVersion()) : std::string(" (no plug-in)")));
        add(prefix + " label", node->getLabel());

        double x, y;
        node->getPosition(&x, &y);
        add(prefix + " position", exact(x) + " " + exact(y));
        if (node->hasSize()) {
            double w, h;
            node->getSize(&w, &h);
            add(prefix + " size", exact(w) + " " + exact(h));
        }
        // Colours are stored as floats, so a colour set in double precision only compares after
        // the same rounding.
        if (node->hasUserColor()) {
            double r, g, b;
            node->getUserColor(&r, &g, &b);
            add(prefix + " color", exact((float)r) + " " + exact((float)g) + " " + exact((float)b));
        }
        if (node->hasOverlayColor()) {
            double r, g, b;
            node->getStoredOverlayColor(&r, &g, &b);
            add(prefix + " overlay", exact((float)r) + " " + exact((float)g) + " " + exact((float)b));
        }

        for (int i = 0; i < node->getNInputs(); ++i) {
            NodePtr input = node->getInput(i);
            add(prefix + " input " + std::to_string(i) + " " + node->getInputLabel(i), input ? input->getFullyQualifiedName() : std::string());
        }

        const std::vector<KnobIPtr>& knobs = node->getKnobs();
        for (std::vector<KnobIPtr>::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
            addKnob(prefix, *it);
        }

        if (NodeGroup* group = node->isEffectGroup()) {
            addNodes(*group);
        }
    }

private:
    ProjectSnapshot* _snapshot;
};

} // anonymous namespace

ProjectSnapshot
captureProjectSnapshot(const ProjectPtr& project)
{
    ProjectSnapshot snapshot;
    SnapshotBuilder builder(&snapshot);

    const std::vector<KnobIPtr> knobs = project->getKnobs_mt_safe();
    for (std::vector<KnobIPtr>::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
        if (std::find(std::begin(kSaveMetadataKnobs), std::end(kSaveMetadataKnobs), (*it)->getName()) != std::end(kSaveMetadataKnobs)) {
            continue;
        }
        builder.addKnob("project", *it);
    }

    std::shared_ptr<const std::vector<LayerRegistryEntry>> layers = project->getLayerRegistrySnapshot();
    for (std::vector<LayerRegistryEntry>::const_iterator it = layers->begin(); it != layers->end(); ++it) {
        std::string channels;
        const std::vector<std::string>& names = it->desc.getChannels();
        for (std::size_t c = 0; c < names.size(); ++c) {
            channels += (c ? "," : "") + names[c];
        }
        builder.add("layer " + it->desc.getLayerID(), "label=" + it->desc.getLayerLabel() + " channelsLabel=" + it->desc.getChannelsLabel() + " channels=" + channels + " origin=" + std::to_string((int)it->origin));
    }

    std::list<Format> formats;
    project->getAdditionalFormats(&formats);
    int index = 0;
    for (std::list<Format>::const_iterator it = formats.begin(); it != formats.end(); ++it, ++index) {
        builder.add("format " + std::to_string(index), it->getName() + " [" + std::to_string(it->x1) + " " + std::to_string(it->y1) + " " + std::to_string(it->x2) + " " + std::to_string(it->y2) + "] par=" + exact(it->getPixelAspectRatio()));
    }
    Format defaultFormat;
    project->getProjectDefaultFormat(&defaultFormat);
    builder.add("format default", defaultFormat.getName() + " [" + std::to_string(defaultFormat.x1) + " " + std::to_string(defaultFormat.y1) + " " + std::to_string(defaultFormat.x2) + " " + std::to_string(defaultFormat.y2) + "] par=" + exact(defaultFormat.getPixelAspectRatio()));

    builder.add("timeline current", std::to_string(project->currentFrame()));
    double first = 0., last = 0.;
    project->getFrameRange(&first, &last);
    builder.add("timeline range", exact(first) + " " + exact(last));

    builder.addNodes(*project);

    return snapshot;
}

::testing::AssertionResult
projectsAreEquivalent(const ProjectSnapshot& expected,
                      const ProjectSnapshot& actual)
{
    std::vector<std::string> differences;
    std::set<std::string> keys;
    for (std::map<std::string, std::string>::const_iterator it = expected.entries.begin(); it != expected.entries.end(); ++it) {
        keys.insert(it->first);
    }
    for (std::map<std::string, std::string>::const_iterator it = actual.entries.begin(); it != actual.entries.end(); ++it) {
        keys.insert(it->first);
    }
    for (std::set<std::string>::const_iterator key = keys.begin(); key != keys.end(); ++key) {
        std::map<std::string, std::string>::const_iterator e = expected.entries.find(*key);
        std::map<std::string, std::string>::const_iterator a = actual.entries.find(*key);
        if (a == actual.entries.end()) {
            differences.push_back("missing: " + *key + " = " + e->second);
        } else if (e == expected.entries.end()) {
            differences.push_back("extra: " + *key + " = " + a->second);
        } else if (e->second != a->second) {
            differences.push_back("different: " + *key + "\n    expected " + e->second + "\n    actual   " + a->second);
        }
    }
    if (differences.empty()) {
        return ::testing::AssertionSuccess() << expected.entries.size() << " entries match";
    }

    ::testing::AssertionResult result = ::testing::AssertionFailure();
    result << differences.size() << " differences between the projects:";
    for (std::size_t i = 0; (i < differences.size()) && (i < kMaxReportedDifferences); ++i) {
        result << "\n  " << differences[i];
    }
    if (differences.size() > kMaxReportedDifferences) {
        result << "\n  ...";
    }

    return result;
}

NATRON_NAMESPACE_EXIT
