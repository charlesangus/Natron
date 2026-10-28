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
#include "HashSweepEdits.h"

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
using namespace NATRON_NAMESPACE::HashSweep;

namespace {

// The plugin list is only known once AppManager::load() has run, which wmain.cpp does after
// InitGoogleTest() has already expanded every parameterised suite, so the sweep cannot be
// instantiated per plugin ID. Plugins are dealt round-robin into fixed shards instead, each its
// own ctest case, which keeps every case well inside the per-case timeout and lets one crashing
// plugin take down only its shard.
const int kShardCount = 16;

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

    std::string skip = staticValueSkipReason(knob, dimension);
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

    node->refreshHashNow();
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
        // Recomputed so that a knob whose content misses the hash is told apart from one whose
        // edit did not mark the node dirty; both fail.
        node->refreshHashNow();
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
        report->failures.push_back(label + ": DIRTY GAP: edit " + edit.what + " did not mark the node hash dirty, it moved only on an explicit recompute");
    }

    edit.revert();
    U64 restored = node->getHashValue();
    const bool recomputedOnRevert = restored == before;
    if (!recomputedOnRevert) {
        node->refreshHashNow();
        restored = node->getHashValue();
    }
    if (restored == before) {
        if (!recomputedOnRevert && (changed != before)) {
            report->failures.push_back(label + ": DIRTY GAP: reverting " + edit.what + " did not mark the node hash dirty, it came back only on an explicit recompute");
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
        // Plugins may add or remove knobs while reacting to an edit, so iterate over a copy and
        // sweep the knob the node holds under each name now: an edit may have replaced it.
        const KnobsVec knobs = node->getKnobs();
        for (KnobsVec::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
            if (!*it) {
                continue;
            }
            const KnobsVec& live = node->getKnobs();
            const bool held = std::find(live.begin(), live.end(), *it) != live.end();
            const KnobIPtr knob = held ? *it : node->getKnobByName((*it)->getName());
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
