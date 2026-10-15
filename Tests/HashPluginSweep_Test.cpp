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

#include "Global/Macros.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Knob.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Plugin.h"
#include "Engine/Project.h"
#include "Engine/ProjectColorManagement.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

// The plugin list is only known once AppManager::load() has run, which wmain.cpp does after
// InitGoogleTest() has already expanded every parameterised suite, so the sweep cannot be
// instantiated per plugin ID. Plugins are dealt round-robin into fixed shards instead, each its
// own ctest case, which keeps every case well inside the per-case timeout and lets one crashing
// plugin take down only its shard.
const int kShardCount = 16;

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

std::string
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

Allowlist
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

std::vector<PluginVersion>
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

bool
ofxBundleExpected()
{
    const char* path = std::getenv("OFX_PLUGIN_PATH");

    return path && *path;
}

std::string
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

std::string
hex(U64 h)
{
    std::ostringstream ss;
    ss << std::hex << h;

    return ss.str();
}

typedef std::map<const KnobI*, std::pair<std::string, U64>> KnobHashSnapshot;

KnobHashSnapshot
snapshotKnobHashes(const NodePtr& node)
{
    KnobHashSnapshot snap;
    const KnobsVec knobs = node->getKnobs();

    for (KnobsVec::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
        if (*it && (*it)->isRenderHashed()) {
            snap[it->get()] = std::make_pair((*it)->getName(), (*it)->getContentHash());
        }
    }

    return snap;
}

std::vector<std::string>
knobsThatMoved(const KnobHashSnapshot& before,
               const KnobHashSnapshot& after)
{
    std::vector<std::string> ret;

    for (KnobHashSnapshot::const_iterator it = before.begin(); it != before.end(); ++it) {
        KnobHashSnapshot::const_iterator found = after.find(it->first);
        if ((found == after.end()) || (found->second.second != it->second.second)) {
            ret.push_back(it->second.first);
        }
    }
    for (KnobHashSnapshot::const_iterator it = after.begin(); it != after.end(); ++it) {
        if (before.find(it->first) == before.end()) {
            ret.push_back(it->second.first);
        }
    }

    return ret;
}

std::string
joined(const std::vector<std::string>& names)
{
    std::string ret;

    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i) {
            ret += ", ";
        }
        ret += names[i];
    }

    return ret;
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

bool
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

bool
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

bool
isOCIOColorSpaceKnob(const std::string& name)
{
    return (name == "ocioInputSpace") || (name == "ocioOutputSpace");
}

// openfx-io's GenericOCIO resets a name the config does not know to its default colourspace,
// and folds any name back to the role its colourspace plays (scene_linear, ...). A sticking
// edit therefore has to name a colourspace other than the one the current value resolves to.
bool
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
 * @brief Builds a reversible edit that changes the content of one dimension of the knob.
 * @returns An empty string, or why this dimension cannot be perturbed (a skip, not a failure).
 * Sets *unsupported when the knob type is one the sweep does not know how to edit.
 **/
std::string
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

struct SweepReport {
    std::vector<std::string> failures;
    std::vector<std::string> notes;
    int knobsSwept;
    int dimensionsPerturbed;
    int dimensionsSkipped;

    SweepReport()
        : knobsSwept(0)
        , dimensionsPerturbed(0)
        , dimensionsSkipped(0)
    {
    }
};

void
perturbDimension(const NodePtr& node,
                 const KnobIPtr& knob,
                 int dimension,
                 const std::string& where,
                 SweepReport* report)
{
    const std::string label = where + " " + knobLabel(knob, dimension);

    // Expressions, links and curves have their own content-hash coverage; editing the static
    // value under them would not be the value the node renders with.
    std::string skip;
    if (!knob->getExpression(dimension).empty()) {
        skip = "has an expression";
    } else if (knob->isSlave(dimension)) {
        skip = "is slaved";
    } else if (knob->isAnimated(dimension)) {
        skip = "is animated";
    }
    KnobEdit edit;
    bool unsupported = false;
    if (skip.empty()) {
        skip = makeEdit(knob, dimension, &edit, &unsupported);
    }
    if (unsupported) {
        report->failures.push_back(label + ": the sweep cannot perturb this knob type");

        return;
    }
    if (!skip.empty()) {
        ++report->dimensionsSkipped;
        report->notes.push_back(label + ": skipped, " + skip);

        return;
    }
    ++report->dimensionsPerturbed;

    node->doComputeHashOnMainThread();
    const U64 before = node->getHashValue();
    const U64 knobBefore = knob->getContentHash();
    const KnobHashSnapshot snapshot = snapshotKnobHashes(node);

    if (!edit.apply()) {
        report->failures.push_back(label + ": the knob refused the edit " + edit.what);

        return;
    }
    const bool valueMoved = edit.valueMoved();
    const U64 knobAfter = knob->getContentHash();
    U64 changed = node->getHashValue();
    const bool recomputedOnEdit = changed != before;
    if (!recomputedOnEdit) {
        // Changes that skip the significant-evaluate path (persistent knobs that do not evaluate
        // on change, for one) leave the stored hash stale until something recomputes it. That
        // is a separate defect from the knob's content missing the hash, so recompute here and
        // report it as a note.
        node->doComputeHashOnMainThread();
        changed = node->getHashValue();
    }
    if (changed == before) {
        std::string why;
        if (!valueMoved) {
            why = "the value did not stick (refused, clamped or reset by the plugin)";
        } else if (knobAfter == knobBefore) {
            why = "HASH GAP: the value changed but the knob's content hash did not";
        } else {
            why = "HASH GAP: the knob's content hash changed but the node hash did not";
        }
        report->failures.push_back(label + ": edit " + edit.what + " left the node hash at " + hex(before) + ", " + why);
    } else if (!recomputedOnEdit) {
        report->notes.push_back(label + ": stale until an explicit recompute after edit " + edit.what);
    }

    edit.revert();
    U64 restored = node->getHashValue();
    const bool recomputedOnRevert = restored == before;
    if (!recomputedOnRevert) {
        node->doComputeHashOnMainThread();
        restored = node->getHashValue();
    }
    if (restored == before) {
        if (!recomputedOnRevert && (changed != before)) {
            report->notes.push_back(label + ": stale until an explicit recompute after reverting " + edit.what);
        }

        return;
    }

    const std::vector<std::string> moved = knobsThatMoved(snapshot, snapshotKnobHashes(node));
    std::ostringstream msg;
    msg << label << ": reverting " << edit.what << " left the node hash at " << hex(restored) << " instead of " << hex(before);
    if (moved.empty()) {
        report->failures.push_back(msg.str() + ", HASH GAP: every render-hashed knob is back to its original content");
    } else if (std::find(moved.begin(), moved.end(), knob->getName()) != moved.end()) {
        report->failures.push_back(msg.str() + ", the knob's own content did not come back (also moved: " + joined(moved) + ")");
    } else {
        // The plugin rewrote other knobs while reacting to the edit; the hash follows them,
        // which is the content hash working, not failing.
        report->notes.push_back(msg.str() + ", side effects on " + joined(moved));
    }
} // perturbDimension

void
sweepKnob(const NodePtr& node,
          const KnobIPtr& knob,
          const std::string& where,
          SweepReport* report)
{
    ++report->knobsSwept;
    for (int d = 0; d < knob->getDimension(); ++d) {
        perturbDimension(node, knob, d, where, report);
    }
}

void
printNotes(const SweepReport& report)
{
    for (std::size_t i = 0; i < report.notes.size(); ++i) {
        std::cout << "[hash sweep] " << report.notes[i] << std::endl;
    }
}

std::string
failureSummary(const std::vector<std::string>& failures)
{
    std::ostringstream ss;

    ss << failures.size() << " hash sweep failure(s). Fix the hash, or list a knob that does not "
       << "affect rendering in " << kAllowlistPath << " with its reason:\n";
    for (std::size_t i = 0; i < failures.size(); ++i) {
        ss << "  " << failures[i] << "\n";
    }

    return ss.str();
}

} // namespace

class HashPluginSweepTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    NodePtr createPluginNode(const PluginVersion& plugin)
    {
        CreateNodeArgs args(plugin.id, getApp()->getProject());

        args.setProperty<int>(kCreateNodeArgsPropPluginVersion, plugin.major, 0);
        args.setProperty<int>(kCreateNodeArgsPropPluginVersion, plugin.minor, 1);
        // Deprecated plugins still render in the projects that use them.
        args.setProperty<bool>(kCreateNodeArgsPropAllowNonUserCreatablePlugins, true);
        args.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, false);
        args.setProperty<bool>(kCreateNodeArgsPropSilent, true);
        args.setProperty<bool>(kCreateNodeArgsPropTrustPluginID, true);

        return getApp()->createNode(args);
    }

    /**
     * @brief Sweeps every render-hashed knob of one plugin version, or only `onlyKnob` when it is
     * not empty. Allowlisted knobs are skipped when `allowlist` is not null.
     * @returns False when the node could not be created.
     **/
    bool sweepPlugin(const PluginVersion& plugin,
                     const Allowlist* allowlist,
                     const std::string& onlyKnob,
                     bool* knobFound,
                     SweepReport* report)
    {
        const std::string where = plugin.describe();
        NodePtr node = createPluginNode(plugin);

        if (!node) {
            report->failures.push_back(where + ": the node could not be created");

            return false;
        }
        // Plugins may add or remove knobs while reacting to an edit, so iterate over a copy.
        const KnobsVec knobs = node->getKnobs();
        for (KnobsVec::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
            const KnobIPtr& knob = *it;
            if (!knob) {
                continue;
            }
            if (!onlyKnob.empty()) {
                if (knob->getName() != onlyKnob) {
                    continue;
                }
                if (knobFound) {
                    *knobFound = true;
                }
            }
            if (!knob->isRenderHashed()) {
                continue;
            }
            if (allowlist && allowlist->exempts(plugin.id, knob->getName())) {
                continue;
            }
            sweepKnob(node, knob, where, report);
        }
        node->destroyNode(true, false);

        return true;
    }
};

class HashPluginSweepShardTest
    : public HashPluginSweepTest,
      public ::testing::WithParamInterface<int> {
};

TEST_P(HashPluginSweepShardTest, EveryRenderHashedKnobChangesTheHashAndRevertRestoresIt)
{
    const Allowlist allowlist = loadAllowlist();
    ASSERT_TRUE(allowlist.errors.empty()) << joined(allowlist.errors);

    const std::vector<PluginVersion> plugins = sweptPlugins();
    ASSERT_FALSE(plugins.empty());

    SweepReport report;
    int pluginsSwept = 0;
    for (std::size_t i = GetParam(); i < plugins.size(); i += kShardCount) {
        const PluginVersion& plugin = plugins[i];
        if (allowlist.exemptsPlugin(plugin.id)) {
            continue;
        }
        const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        const std::size_t failuresBefore = report.failures.size();
        const int knobsBefore = report.knobsSwept;
        sweepPlugin(plugin, &allowlist, std::string(), NULL, &report);
        ++pluginsSwept;
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        std::cout << "[hash sweep] " << plugin.describe() << ": " << (report.knobsSwept - knobsBefore) << " knobs, "
                  << (report.failures.size() - failuresBefore) << " failures, " << ms << " ms" << std::endl;
    }

    printNotes(report);
    std::cout << "[hash sweep] shard " << GetParam() << "/" << kShardCount << ": " << pluginsSwept << " plugins, "
              << report.knobsSwept << " knobs, " << report.dimensionsPerturbed << " dimensions perturbed, "
              << report.dimensionsSkipped << " skipped" << std::endl;

    EXPECT_TRUE(report.failures.empty()) << failureSummary(report.failures);
}

INSTANTIATE_TEST_CASE_P(Shards, HashPluginSweepShardTest, ::testing::Range(0, kShardCount));

TEST_F(HashPluginSweepTest, AllowlistIsWellFormedAndEveryEntryIsStillNeeded)
{
    const Allowlist allowlist = loadAllowlist();
    EXPECT_TRUE(allowlist.errors.empty()) << joined(allowlist.errors);

    const std::vector<PluginVersion> plugins = sweptPlugins();
    const bool strict = ofxBundleExpected();
    if (strict) {
        bool anyOfx = false;
        for (std::size_t i = 0; i < plugins.size(); ++i) {
            anyOfx = anyOfx || plugins[i].isOfx;
        }
        EXPECT_TRUE(anyOfx) << "OFX_PLUGIN_PATH is set but no OpenFX plugin was loaded, so the sweep covers native nodes only";
    }

    std::set<std::pair<std::string, std::string>> seen;
    for (std::size_t e = 0; e < allowlist.entries.size(); ++e) {
        const AllowlistEntry& entry = allowlist.entries[e];
        std::ostringstream where;
        where << kAllowlistPath << ":" << entry.line << ": " << entry.pluginID << " " << entry.knobName;

        EXPECT_TRUE(seen.insert(std::make_pair(entry.pluginID, entry.knobName)).second) << where.str() << " is listed twice";

        std::vector<PluginVersion> versions;
        for (std::size_t i = 0; i < plugins.size(); ++i) {
            if (plugins[i].id == entry.pluginID) {
                versions.push_back(plugins[i]);
            }
        }
        if (versions.empty()) {
            if (strict) {
                ADD_FAILURE() << where.str() << " names a plugin that is no longer loaded; remove the entry";
            } else {
                std::cout << "[hash sweep] " << where.str() << ": plugin not loaded, OFX_PLUGIN_PATH unset, not checked" << std::endl;
            }
            continue;
        }

        if (entry.knobName == kWholePluginWildcard) {
            SweepReport report;
            bool allCreated = true;
            for (std::size_t v = 0; v < versions.size(); ++v) {
                allCreated = sweepPlugin(versions[v], NULL, std::string(), NULL, &report) && allCreated;
            }
            EXPECT_FALSE(allCreated && report.failures.empty()) << where.str() << " now sweeps cleanly; remove the entry";
            continue;
        }

        bool knobFound = false;
        SweepReport report;
        for (std::size_t v = 0; v < versions.size(); ++v) {
            sweepPlugin(versions[v], NULL, entry.knobName, &knobFound, &report);
        }
        if (!knobFound) {
            ADD_FAILURE() << where.str() << " matches no knob of the plugin; remove the entry";
        } else if (report.knobsSwept == 0) {
            ADD_FAILURE() << where.str() << " is no longer render-hashed; remove the entry";
        } else if (report.failures.empty()) {
            ADD_FAILURE() << where.str() << " now changes and restores the hash; remove the entry";
        }
    }
}
