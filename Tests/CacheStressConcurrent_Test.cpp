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

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <QCoreApplication>

#include "BaseTest.h"
#include "CacheStress.h"
#include "CountingTestEffect.h"
#include "NativeParity.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

using namespace CacheStress;

const RectI kWindow(0, 0, kCountingTestSize, kCountingTestSize);
const std::vector<std::uint32_t> kSeeds = { 1u, 2u, 3u };
const int kRounds = 4;
const double kTime = 1.;

// Holds every render of one node until released. The Counting effect calls the hook before it
// reads its inputs or its knobs, so whatever the main thread does meanwhile lands mid-render.
class Gate {
public:
    void setNode(const Node* node)
    {
        std::lock_guard<std::mutex> k(_mutex);

        _node = node;
    }

    void arm()
    {
        std::lock_guard<std::mutex> k(_mutex);

        _armed = true;
        _entered = false;
        _released = false;
    }

    void hold(const Node* node)
    {
        std::unique_lock<std::mutex> k(_mutex);

        if ((node != _node) || !_armed) {
            return;
        }
        _entered = true;
        _cond.notify_all();
        _cond.wait_for(k, std::chrono::seconds(30), [this]() { return _released; });
    }

    bool waitEntered()
    {
        std::unique_lock<std::mutex> k(_mutex);

        return _cond.wait_for(k, std::chrono::seconds(10), [this]() { return _entered; });
    }

    void release()
    {
        std::lock_guard<std::mutex> k(_mutex);

        _released = true;
        _armed = false;
        _cond.notify_all();
    }

private:
    std::mutex _mutex;
    std::condition_variable _cond;
    const Node* _node = nullptr;
    bool _armed = false;
    bool _entered = false;
    bool _released = false;
};

struct GateGraph {
    NodePtr src1;
    NodePtr src2;
    NodePtr gate; // CountingMerge: A from src1, B optional
    NodePtr grade;
    NodePtr blur; // the probe
};

struct Edit {
    std::string name;
    std::function<void()> apply;
    std::function<void()> undo;
};

enum RaceKind {
    eRaceTopology, // input edits, queued while the nodes render
    eRaceKnobs, // knob edits, which a background app applies at once
    eRaceRestarted // input edits queued during one render and dequeued during the next
};

bool
buildGateGraph(const AppInstancePtr& app,
               GateGraph* g,
               std::string* error)
{
    app->getProject()->reset(false, true);
    clearEverything(app);
    const NodeCollectionPtr root = app->getProject();
    g->src1 = createConstantIn(app, root, kWindow, { 0.2, 0.3, 0.4, 1. });
    g->src2 = createConstantIn(app, root, kWindow, { 0.5, 0.1, 0.7, 0.5 });
    g->gate = createIn(app, root, kTestPluginIDCountingMerge);
    g->grade = createIn(app, root, PLUGINID_NATRON_GRADE, PLUGIN_MAJOR_NATRON_GRADE);
    g->blur = createIn(app, root, PLUGINID_NATRON_BLUR, PLUGIN_MAJOR_NATRON_BLUR);
    if (!g->src1 || !g->src2 || !g->gate || !g->grade || !g->blur) {
        *error = "could not create the nodes";

        return false;
    }
    if (!g->gate->connectInput(g->src1, 0) || !g->grade->connectInput(g->gate, 0) || !g->blur->connectInput(g->grade, 0) || !setKnobValues(g->blur, kBlurParamSize, { 1., 1. })) {
        *error = "could not set up the chain";

        return false;
    }

    return true;
}

Edit
toggleInput(const NodePtr& node,
            int input,
            const NodePtr& source,
            const std::string& what)
{
    Edit e;

    if (node->getInput(input)) {
        e.name = "disconnect " + what;
        e.apply = [node, input]() { static_cast<void>(node->disconnectInput(input)); };
        e.undo = [node, input, source]() { static_cast<void>(node->connectInput(source, input)); };
    } else {
        e.name = "connect " + what;
        e.apply = [node, input, source]() { static_cast<void>(node->connectInput(source, input)); };
        e.undo = [node, input]() { static_cast<void>(node->disconnectInput(input)); };
    }

    return e;
}

Edit
topologyEdit(std::mt19937& rng,
             const GateGraph& g)
{
    switch (rng() % 3) {
    case 0:
        return toggleInput(g.gate, 1, g.src2, "the gate's B");
    case 1:
        return toggleInput(g.grade, 1, g.src2, "the grade's mask");
    default: {
        const NodePtr current = g.blur->getInput(0);
        const NodePtr other = (current == g.grade) ? g.gate : g.grade;
        const NodePtr blur = g.blur;
        Edit e;
        e.name = std::string("feed the blur from the ") + ((other == g.grade) ? "grade" : "gate");
        e.apply = [blur, other]() { static_cast<void>(blur->replaceInput(other, 0)); };
        e.undo = [blur, current]() { static_cast<void>(blur->replaceInput(current, 0)); };

        return e;
    }
    }
}

Edit
knobEdit(std::mt19937& rng,
         const GateGraph& g)
{
    const bool onGate = (rng() % 2) == 0;
    KnobDoublePtr knob = std::dynamic_pointer_cast<KnobDouble>(onGate ? g.gate->getKnobByName("value") : g.grade->getKnobByName(kGradeParamWhite));
    Edit e;

    if (!knob) {
        e.name = "no knob";
        e.apply = []() { };
        e.undo = []() { };

        return e;
    }
    const double old = knob->getValue(0);
    const double value = old + 0.25 + 0.25 * (double)(rng() % 3);
    std::ostringstream os;
    os << (onGate ? "the gate's value " : "the grade's white ") << old << " -> " << value;
    e.name = os.str();
    e.apply = [knob, value]() { knob->setValue(value, ViewSpec::all(), 0); };
    e.undo = [knob, old]() { knob->setValue(old, ViewSpec::all(), 0); };

    return e;
}

void
dequeueAll(const AppInstancePtr& app)
{
    NodesList nodes;

    app->getProject()->getNodes_recursive(nodes, false);
    for (NodesList::iterator it = nodes.begin(); it != nodes.end(); ++it) {
        (*it)->dequeueActions();
    }
    QCoreApplication::processEvents();
}

bool
reference(const AppInstancePtr& app,
          const GateGraph& g,
          RenderSchedulerModeEnum mode,
          RenderedPlane* out,
          std::string* error)
{
    clearEverything(app);

    return renderProbe(g.blur, kTime, 0, kWindow, mode, true, out, error);
}

std::string
checkCached(const GateGraph& g,
            RenderSchedulerModeEnum mode,
            const RenderedPlane& ref,
            const std::string& what)
{
    RenderedPlane got;
    std::string error;

    if (!renderProbe(g.blur, kTime, 0, kWindow, mode, false, &got, &error)) {
        return what + ": the cached render failed: " + error;
    }
    const std::string diff = compareExact(ref, got);

    return diff.empty() ? std::string() : (what + ": " + diff);
}

// A render of the probe on a worker thread, flagged as rendering the way the viewer and the
// writers flag theirs, so input edits on the main thread are queued.
class GatedRender {
public:
    GatedRender(Gate* gate,
                const GateGraph& g,
                RenderSchedulerModeEnum mode)
        : _gate(gate)
    {
        gate->arm();
        const NodePtr probe = g.blur;
        _thread = std::thread([this, probe, mode]() {
            RenderingFlagSetter flag(probe);
            _ok = renderProbe(probe, kTime, 0, kWindow, mode, false, &_plane, &_error);
        });
    }

    ~GatedRender()
    {
        finish();
    }

    // False when the render never reached the gate.
    bool waitHeld()
    {
        return _gate->waitEntered();
    }

    void finish()
    {
        _gate->release();
        if (_thread.joinable()) {
            _thread.join();
        }
    }

    bool ok() const
    {
        return _ok;
    }

    const RenderedPlane& plane() const
    {
        return _plane;
    }

    const std::string& error() const
    {
        return _error;
    }

private:
    Gate* _gate;
    std::thread _thread;
    bool _ok = false;
    RenderedPlane _plane;
    std::string _error;
};

// One round: references for the states before and after the edit, then the edit made while a
// cached render is held at the gate, then cached renders of the edited state, of the state
// before it, and of the edited state again. Returns "" or what went wrong.
std::string
raceRound(const AppInstancePtr& app,
          Gate* gate,
          const GateGraph& g,
          RenderSchedulerModeEnum mode,
          const Edit& edit,
          RaceKind kind)
{
    RenderedPlane before;
    RenderedPlane after;
    std::string error;

    if (!reference(app, g, mode, &before, &error)) {
        return "the reference before the edit failed: " + error;
    }
    edit.apply();
    const bool afterOk = reference(app, g, mode, &after, &error);
    edit.undo();
    if (!afterOk) {
        return "the reference after the edit failed: " + error;
    }

    clearEverything(app);
    std::string workerMismatch;
    {
        GatedRender first(gate, g, mode);
        if (!first.waitHeld()) {
            return "the render never reached the gate";
        }
        edit.apply();
        first.finish();
        if (kind == eRaceRestarted) {
            // The queue is still pending: a second render starts on the old inputs and the
            // dequeue lands under it.
            clearEverything(app);
            GatedRender second(gate, g, mode);
            if (!second.waitHeld()) {
                return "the restarted render never reached the gate";
            }
            dequeueAll(app);
            second.finish();
            if (!second.ok()) {
                workerMismatch = "the restarted render failed: " + second.error();
            } else {
                const std::string diff = compareExact(before, second.plane());
                if (!diff.empty()) {
                    workerMismatch = "the restarted render differs from the state it started in: " + diff;
                }
            }
        } else {
            dequeueAll(app);
            if (!first.ok()) {
                workerMismatch = "the held render failed: " + first.error();
            } else if (kind == eRaceTopology) {
                // Input edits wait for the render, so it shows the state it started in.
                const std::string diff = compareExact(before, first.plane());
                if (!diff.empty()) {
                    workerMismatch = "the held render saw the queued edit: " + diff;
                }
            }
        }
    }
    if (!workerMismatch.empty()) {
        return workerMismatch;
    }

    std::string mismatch = checkCached(g, mode, after, "after the edit");
    if (!mismatch.empty()) {
        return mismatch;
    }
    edit.undo();
    dequeueAll(app);
    mismatch = checkCached(g, mode, before, "back to the state before the edit");
    if (!mismatch.empty()) {
        return mismatch;
    }
    edit.apply();
    dequeueAll(app);

    return checkCached(g, mode, after, "the edit made again");
} // raceRound

// Every seed, both modes, kRounds edits each; one gtest failure per failing seed and mode.
int
runRaces(const AppInstancePtr& app,
         RaceKind kind)
{
    Gate gate;
    int failures = 0;
    const std::vector<std::uint32_t> seeds = seedsToRun(kSeeds);
    const int rounds = stepBudget(kRounds);

    CountingTestRegistry::setRenderHook([&gate](const Node* node) { gate.hold(node); });
    for (std::size_t s = 0; s < seeds.size(); ++s) {
        for (int m = 0; m < 2; ++m) {
            const RenderSchedulerModeEnum mode = (m == 0) ? eRenderSchedulerModeLegacy : eRenderSchedulerModeTaskGraph;
            const char* modeName = (m == 0) ? "legacy" : "task graph";
            std::printf("[ STRESS   ] seed %u, %s, %d rounds\n", seeds[s], modeName, rounds);
            std::fflush(stdout);
            GateGraph g;
            std::string error;
            if (!buildGateGraph(app, &g, &error)) {
                ADD_FAILURE() << error;

                return failures + 1;
            }
            gate.setNode(g.gate.get());
            std::mt19937 rng(seeds[s]);
            std::ostringstream log;
            for (int r = 0; r < rounds; ++r) {
                const Edit edit = (kind == eRaceKnobs) ? knobEdit(rng, g) : topologyEdit(rng, g);
                log << "  " << r << ": " << edit.name << "\n";
                const std::string mismatch = raceRound(app, &gate, g, mode, edit, kind);
                if (!mismatch.empty()) {
                    ++failures;
                    ADD_FAILURE() << "cache stress race: NATRON_CACHE_STRESS_SEED=" << seeds[s] << " round " << r << " (" << modeName << "): "
                                  << mismatch << "\nedit log:\n"
                                  << log.str();
                    break;
                }
            }
            gate.setNode(nullptr);
        }
    }
    CountingTestRegistry::reset();

    return failures;
} // runRaces

} // namespace

class CacheStressConcurrentTest
    : public BaseTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        CountingTestRegistry::reset();
        getApp()->getProject()->reset(false, true);
        appPTR->clearAllCaches();
        BaseTest::TearDown();
    }
};

// Connects, disconnects and replacements made while a cached render is held queue until it
// ends; after the dequeue, the edited state and the state before it both render as from empty
// caches.
TEST_F(CacheStressConcurrentTest, InputEditsDuringARender)
{
    EXPECT_EQ(0, runRaces(getApp(), eRaceTopology));
}

// A background app applies knob edits at once, so the held render reads the new values while its
// hash was taken from the old ones, and stores the new state's pixels under the old key.
TEST_F(CacheStressConcurrentTest, DISABLED_KnobEditsDuringRender_M202)
{
    EXPECT_EQ(0, runRaces(getApp(), eRaceKnobs));
}

// dequeueActions does not wait for renders that started after the edit was queued, so the inputs
// change under a running render.
TEST_F(CacheStressConcurrentTest, DISABLED_DequeueUnderARestartedRender_P4T1)
{
    EXPECT_EQ(0, runRaces(getApp(), eRaceRestarted));
}
