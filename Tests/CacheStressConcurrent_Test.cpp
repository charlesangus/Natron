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

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <list>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "CacheStress.h"
#include "CacheStressTestEffect.h"
#include "CountingTestEffect.h"
#include "NativeParity.h"
#include "SceneTestEffect.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CacheDiskTier.h"
#include "Engine/CacheEntry.h"
#include "Engine/CacheTier.h"
#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/ImageKey.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Merge/Merge.h"
#include "Engine/Project.h"
#include "Engine/Settings.h"
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
    std::shared_ptr<KnobDoubleBase> knob = std::dynamic_pointer_cast<KnobDoubleBase>(onGate ? g.gate->getKnobByName("value") : g.grade->getKnobByName(kGradeParamWhite));
    Edit e;

    if (!knob) {
        ADD_FAILURE() << "the knob edit target does not exist or is not a double knob";
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
            // The queue is still pending: a second render starts on the old inputs, and a
            // dequeue attempted under it must wait for it to end.
            clearEverything(app);
            GatedRender second(gate, g, mode);
            if (!second.waitHeld()) {
                return "the restarted render never reached the gate";
            }
            dequeueAll(app);
            second.finish();
            dequeueAll(app);
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

void
setSourceValue(const NodePtr& source,
               int value)
{
    KnobInt* knob = dynamic_cast<KnobInt*>(source->getKnobByName("value").get());

    ASSERT_TRUE(knob != NULL);
    knob->setValue(value);
}

// The value the source's scene composes, evaluated on this thread.
std::string
evaluateSource(const NodePtr& source,
               int* value)
{
    ScenePayload payload;
    std::string error;

    if (source->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(kTime, ViewIdx(0)), &payload, &error) != EffectInstance::eRenderRoIRetCodeOk) {
        return "the evaluation failed: " + error;
    }
    if (!sceneTestReadSourceValue(payload, value, &error)) {
        return "could not read the scene: " + error;
    }

    return std::string();
}

// One round: a worker evaluates the source with its value set to `asked`, and is held inside the
// authoring while the main thread sets `edited`. Then the edit is undone and redone, each
// evaluation checked against the knob, which is what a source authored from empty caches holds.
std::string
sceneRaceRound(const NodePtr& source,
               int asked,
               int edited)
{
    std::mutex mutex;
    std::condition_variable cond;
    bool entered = false;
    bool released = false;

    setSourceValue(source, asked);
    sceneTestSourceAuthorHook() = [&]() {
        std::unique_lock<std::mutex> k(mutex);
        entered = true;
        cond.notify_all();
        cond.wait_for(k, std::chrono::seconds(30), [&]() { return released; });
    };
    int workerValue = 0;
    std::string workerError;
    std::thread worker([&]() {
        workerError = evaluateSource(source, &workerValue);
    });
    bool held = false;
    {
        std::unique_lock<std::mutex> k(mutex);
        held = cond.wait_for(k, std::chrono::seconds(10), [&]() { return entered; });
    }
    if (held) {
        setSourceValue(source, edited);
    }
    {
        std::lock_guard<std::mutex> k(mutex);
        released = true;
        cond.notify_all();
    }
    worker.join();
    sceneTestSourceAuthorHook() = std::function<void()>();
    if (!held) {
        return "the evaluation never reached the authoring";
    }
    if (!workerError.empty()) {
        return "the held evaluation: " + workerError;
    }

    const int checks[] = { asked, edited };
    for (int value : checks) {
        setSourceValue(source, value);
        int got = 0;
        const std::string error = evaluateSource(source, &got);
        if (!error.empty()) {
            return error;
        }
        if (got != value) {
            std::ostringstream os;
            os << "the value is " << value << " but the scene composes " << got;

            return os.str();
        }
    }

    return std::string();
} // sceneRaceRound

// One round on source -> filter, filter reading wide or narrow: references of both, then from
// cold caches connect wide, revert to narrow and connect wide again, each followed by a cached
// render. The filter stores its region of definition from onInputChanged(), under the hash the
// node has then. Returns "" or what went wrong.
std::string
inputChangedRound(const AppInstancePtr& app,
                  const NodePtr& filter,
                  const NodePtr& wide,
                  const NodePtr& narrow)
{
    std::string error;
    RenderedPlane refWide;
    RenderedPlane refNarrow;

    if (!filter->replaceInput(narrow, 0)) {
        return "could not connect the narrow source";
    }
    clearEverything(app);
    if (!renderProbe(filter, kTime, 0, kWindow, eRenderSchedulerModeLegacy, true, &refNarrow, &error)) {
        return "the narrow reference failed: " + error;
    }
    if (!filter->replaceInput(wide, 0)) {
        return "could not connect the wide source";
    }
    clearEverything(app);
    if (!renderProbe(filter, kTime, 0, kWindow, eRenderSchedulerModeLegacy, true, &refWide, &error)) {
        return "the wide reference failed: " + error;
    }
    if (compareExact(refWide, refNarrow).empty()) {
        return "the two sources render alike";
    }
    if (!filter->replaceInput(narrow, 0)) {
        return "could not revert to the narrow source";
    }
    clearEverything(app);

    const struct {
        NodePtr source;
        const RenderedPlane* ref;
        const char* what;
    } steps[] = {
        { wide, &refWide, "connect wide" },
        { narrow, &refNarrow, "revert to narrow" },
        { wide, &refWide, "connect wide again" },
    };
    for (const auto& step : steps) {
        if (!filter->replaceInput(step.source, 0)) {
            return std::string(step.what) + ": the edit failed";
        }
        RenderedPlane got;
        if (!renderProbe(filter, kTime, 0, kWindow, eRenderSchedulerModeLegacy, false, &got, &error)) {
            return std::string(step.what) + ": the cached render failed: " + error;
        }
        const std::string diff = compareExact(*step.ref, got);
        if (!diff.empty()) {
            return std::string(step.what) + ": " + diff;
        }
    }

    return std::string();
}

struct TwoInputGroup {
    NodePtr a;
    NodePtr b;
    NodePtr group;
    NodePtr in1;
    NodePtr in2;
    NodePtr grade; // the probe, after the group
};

// a and b feed a group whose Merge puts its second input over its first.
bool
buildTwoInputGroup(const AppInstancePtr& app,
                   const std::vector<double>& colorA,
                   const std::vector<double>& colorB,
                   TwoInputGroup* g,
                   std::string* error)
{
    app->getProject()->reset(false, true);
    clearEverything(app);
    const NodeCollectionPtr root = app->getProject();
    g->a = createConstantIn(app, root, kWindow, colorA);
    g->b = createConstantIn(app, root, kWindow, colorB);
    g->group = createEmptyGroupIn(app, root);
    g->grade = createIn(app, root, PLUGINID_NATRON_GRADE, PLUGIN_MAJOR_NATRON_GRADE);
    const NodeCollectionPtr inside = g->group ? std::dynamic_pointer_cast<NodeCollection>(g->group->getEffectInstance()) : NodeCollectionPtr();
    if (!g->a || !g->b || !inside || !g->grade) {
        *error = "could not create the nodes";

        return false;
    }
    g->in1 = createIn(app, inside, PLUGINID_NATRON_INPUT);
    g->in2 = createIn(app, inside, PLUGINID_NATRON_INPUT);
    const NodePtr merge = createIn(app, inside, PLUGINID_NATRON_NATIVE_MERGE, PLUGIN_MAJOR_NATRON_NATIVE_MERGE);
    const NodePtr out = createIn(app, inside, PLUGINID_NATRON_OUTPUT);
    if (!g->in1 || !g->in2 || !merge || !out) {
        *error = "could not create the group's nodes";

        return false;
    }
    if (!merge->connectInput(g->in1, kNativeMergeInputB) || !merge->connectInput(g->in2, NativeMerge::aInput(0)) || !out->connectInput(merge, 0)
        || !g->group->connectInput(g->a, 0) || !g->group->connectInput(g->b, 1) || !g->grade->connectInput(g->group, 0)) {
        *error = "could not connect the group";

        return false;
    }

    return true;
}

// One round: references with and without the group's second input, then from cold caches the
// input is removed, a cached render runs, the dequeue a render's end asks for runs, and the
// input is put back; every cached render must match its reference.
std::string
groupInputRound(const AppInstancePtr& app,
                const TwoInputGroup& g)
{
    std::string error;
    RenderedPlane refWith;
    RenderedPlane refWithout;
    NodeGroup* group = dynamic_cast<NodeGroup*>(g.group->getEffectInstance().get());

    if (!group) {
        return "the group is not a NodeGroup";
    }
    const auto remove = [&]() {
        g.in2->deactivate();
    };
    const auto restore = [&]() -> bool {
        g.in2->activate();

        return g.group->connectInput(g.b, 1);
    };
    clearEverything(app);
    if (!renderProbe(g.grade, kTime, 0, kWindow, eRenderSchedulerModeLegacy, true, &refWith, &error)) {
        return "the reference with both inputs failed: " + error;
    }
    remove();
    clearEverything(app);
    const bool withoutOk = renderProbe(g.grade, kTime, 0, kWindow, eRenderSchedulerModeLegacy, true, &refWithout, &error);
    if (!restore()) {
        return "could not restore the second input";
    }
    if (!withoutOk) {
        return "the reference without the second input failed: " + error;
    }
    if (compareExact(refWith, refWithout).empty()) {
        return "the second input changes nothing";
    }

    clearEverything(app);
    dequeueAll(app);
    for (int m = 0; m < 2; ++m) {
        const RenderSchedulerModeEnum mode = (m == 0) ? eRenderSchedulerModeLegacy : eRenderSchedulerModeTaskGraph;
        const std::string modeName = (m == 0) ? "legacy" : "task graph";
        RenderedPlane got;
        if (!renderProbe(g.grade, kTime, 0, kWindow, mode, false, &got, &error)) {
            return modeName + ", both inputs: the cached render failed: " + error;
        }
        std::string diff = compareExact(refWith, got);
        if (!diff.empty()) {
            return modeName + ", both inputs: " + diff;
        }
        remove();
        for (int pass = 0; pass < 2; ++pass) {
            if (group->getNInputs() != 1) {
                return modeName + ": the removed input came back after the dequeue";
            }
            if (!renderProbe(g.grade, kTime, 0, kWindow, mode, false, &got, &error)) {
                return modeName + ", input removed: the cached render failed: " + error;
            }
            diff = compareExact(refWithout, got);
            if (!diff.empty()) {
                return modeName + ", input removed" + (pass ? " and dequeued: " : ": ") + diff;
            }
            dequeueAll(app);
        }
        if (!restore()) {
            return "could not restore the second input";
        }
        dequeueAll(app);
        if (group->getNInputs() != 2) {
            return modeName + ": the restored input is missing after the dequeue";
        }
    }

    return std::string();
} // groupInputRound
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
// Disabled until renders key their stores by a hash snapshot taken when they start.
TEST_F(CacheStressConcurrentTest, DISABLED_KnobEditsDuringRender)
{
    EXPECT_EQ(0, runRaces(getApp(), eRaceKnobs));
}

// A render that starts after the edit was queued but before the queue is applied keeps the
// inputs it started with; the queue applies once it ends.
TEST_F(CacheStressConcurrentTest, DequeueUnderARestartedRender)
{
    EXPECT_EQ(0, runRaces(getApp(), eRaceRestarted));
}

// A knob edited while a scene node authors its layer: the layer holds the edit, so it must not be
// memoized under the key taken before it, or undoing the edit would serve it.
TEST_F(CacheStressConcurrentTest, SceneKnobEditDuringAuthoring)
{
    const std::vector<std::uint32_t> seeds = seedsToRun(kSeeds);
    const int rounds = stepBudget(kRounds);

    for (std::size_t s = 0; s < seeds.size(); ++s) {
        getApp()->getProject()->reset(false, true);
        clearEverything(getApp());
        const NodePtr source = createIn(getApp(), getApp()->getProject(), kTestPluginIDSceneSource);
        ASSERT_TRUE(bool(source));
        std::mt19937 rng(seeds[s]);
        std::ostringstream log;
        // The memo holds the layer of the last value evaluated; asking for another one authors.
        int last = 0;
        for (int r = 0; r < rounds; ++r) {
            int asked = 1 + (int)(rng() % 4);
            if (asked == last) {
                asked = last + 1;
            }
            const int edited = asked + 1 + (int)(rng() % 3);
            last = edited;
            log << "  " << r << ": ask " << asked << ", edit to " << edited << " while authoring\n";
            const std::string mismatch = sceneRaceRound(source, asked, edited);
            if (!mismatch.empty()) {
                ADD_FAILURE() << "cache stress race: NATRON_CACHE_STRESS_SEED=" << seeds[s] << " round " << r << ": " << mismatch << "\nedit log:\n"
                              << log.str();
                break;
            }
        }
    }
}

// What onInputChanged() stores in a node's own actions cache is keyed under the node's new hash:
// stored under the hash from before the edit, reverting the edit would serve it.
TEST_F(CacheStressConcurrentTest, OnInputChangedUnderTheNewHash)
{
    const std::vector<std::uint32_t> seeds = seedsToRun(kSeeds);
    const int rounds = stepBudget(kRounds);

    for (std::size_t s = 0; s < seeds.size(); ++s) {
        getApp()->getProject()->reset(false, true);
        clearEverything(getApp());
        const NodeCollectionPtr root = getApp()->getProject();
        std::mt19937 rng(seeds[s]);
        const NodePtr wide = createConstantIn(getApp(), root, kWindow, { 0.2, 0.3, 0.4, 1. });
        const NodePtr filter = createIn(getApp(), root, kTestPluginIDInputChanged);
        ASSERT_TRUE(bool(wide) && bool(filter));
        for (int r = 0; r < rounds; ++r) {
            // Strictly inside the window, so the two regions always differ.
            const int x1 = 1 + (int)(rng() % (kWindow.width() / 2));
            const int y1 = 1 + (int)(rng() % (kWindow.height() / 2));
            const RectI extent(x1, y1, x1 + 1 + (int)(rng() % (kWindow.x2 - x1 - 1)), y1 + 1 + (int)(rng() % (kWindow.y2 - y1 - 1)));
            const NodePtr narrow = createConstantIn(getApp(), root, extent, { 0.2, 0.3, 0.4, 1. });
            ASSERT_TRUE(bool(narrow));
            const std::string mismatch = inputChangedRound(getApp(), filter, wide, narrow);
            if (!mismatch.empty()) {
                ADD_FAILURE() << "cache stress: NATRON_CACHE_STRESS_SEED=" << seeds[s] << " round " << r << ", narrow source (" << extent.x1 << ", " << extent.y1 << ", "
                              << extent.x2 << ", " << extent.y2 << "): " << mismatch;
                break;
            }
        }
    }
}

// A render thread reads the viewer hash at the moment the A/B switch marks it. The mark comes
// from refreshActiveInputs(), after the active inputs change and before the rest of the viewer's
// onInputChanged() runs, so the hash read there, and the one the viewer keeps, name the input now
// shown.
TEST_F(CacheStressConcurrentTest, ViewerABSwitchDuringRender)
{
    const std::vector<std::uint32_t> seeds = seedsToRun(kSeeds);
    const int rounds = stepBudget(kRounds);
    const std::vector<std::vector<double>> colors = { { 0.9, 0.1, 0.1, 1. }, { 0.1, 0.8, 0.2, 1. }, { 0.2, 0.3, 0.7, 1. } };

    for (std::size_t s = 0; s < seeds.size(); ++s) {
        getApp()->getProject()->reset(false, true);
        clearEverything(getApp());
        const NodeCollectionPtr root = getApp()->getProject();
        const NodePtr viewerNode = createViewerIn(getApp(), root);
        ASSERT_TRUE(bool(viewerNode));
        InspectorNode* viewer = dynamic_cast<InspectorNode*>(viewerNode.get());
        ASSERT_TRUE(viewer != NULL);
        for (std::size_t i = 0; i < colors.size(); ++i) {
            const NodePtr source = createConstantIn(getApp(), root, kWindow, colors[i]);
            ASSERT_TRUE(bool(source));
            ASSERT_TRUE(viewerNode->connectInput(source, (int)i));
        }
        viewer->setInputA(0);
        // The hash of each B side, set the way that marks after the change.
        std::vector<U64> hashOfB(colors.size());
        for (std::size_t i = 0; i < colors.size(); ++i) {
            viewer->setInputB((int)i);
            hashOfB[i] = viewerNode->getHashValue();
        }
        int b = (int)colors.size() - 1;

        std::mt19937 rng(seeds[s]);
        std::ostringstream log;
        for (int r = 0; r < rounds; ++r) {
            const int next = (b + 1 + (int)(rng() % (colors.size() - 1))) % (int)colors.size();
            log << "  " << r << ": B " << b << " -> " << next << "\n";
            // Nothing else is pending, so the read below flushes no other mark.
            (void)viewerNode->getHashValue();

            std::mutex m;
            std::condition_variable cv;
            bool read = false;
            U64 readHash = 0;
            std::atomic<bool> armed(true);
            std::thread worker;
            Node::setHashMarkTraceForTesting([&](const Node* node,
                                                 const char* /*reason*/,
                                                 const KnobI* /*knob*/) {
                if ((node != viewerNode.get()) || !armed.exchange(false)) {
                    return;
                }
                worker = std::thread([&]() {
                    const U64 h = viewerNode->getHashValue();
                    {
                        std::lock_guard<std::mutex> l(m);
                        readHash = h;
                        read = true;
                    }
                    cv.notify_all();
                });
                // Bounded: a read that has to report a mark of its own waits for this one.
                std::unique_lock<std::mutex> l(m);
                cv.wait_for(l, std::chrono::seconds(2), [&]() { return read; });
            });
            viewer->setActiveInputAndRefresh(next, false);
            Node::setHashMarkTraceForTesting(Node::HashMarkTrace());
            if (worker.joinable()) {
                worker.join();
            }
            b = next;

            int activeA = -1, activeB = -1;
            viewer->getActiveInputs(activeA, activeB);
            ASSERT_EQ(next, activeB);
            if (armed) {
                ADD_FAILURE() << "round " << r << ": the A/B switch did not mark the viewer";
                break;
            }
            bool wasRead;
            {
                std::lock_guard<std::mutex> l(m);
                wasRead = read;
            }
            if (wasRead && (readHash != hashOfB[next])) {
                ADD_FAILURE() << "cache stress race: NATRON_CACHE_STRESS_SEED=" << seeds[s] << " round " << r
                              << ": a hash read at the mark names another input than the one now shown\nswitch log:\n"
                              << log.str();
                break;
            }
            if (viewerNode->getHashValue() != hashOfB[next]) {
                ADD_FAILURE() << "cache stress race: NATRON_CACHE_STRESS_SEED=" << seeds[s] << " round " << r
                              << ": the viewer shows input " << next << " under another hash than that input's\nswitch log:\n"
                              << log.str();
                break;
            }
        }
    }
}

// Removing a GroupInput updates the group's input lists at once; the dequeue that follows a
// render must keep it removed, or the group would read through an input it no longer has.
TEST_F(CacheStressConcurrentTest, GroupInputRemovedThenDequeued)
{
    const std::vector<std::uint32_t> seeds = seedsToRun(kSeeds);
    const int rounds = stepBudget(kRounds);

    for (std::size_t s = 0; s < seeds.size(); ++s) {
        std::mt19937 rng(seeds[s]);
        for (int r = 0; r < rounds; ++r) {
            const auto channel = [&rng]() { return 0.1 + 0.1 * (double)(rng() % 8); };
            const std::vector<double> colorA = { channel(), channel(), channel(), 1. };
            // Brighter than colorA times its alpha in every channel, so the over always shows.
            const auto bright = [&rng]() { return 0.75 + 0.05 * (double)(rng() % 4); };
            const std::vector<double> colorB = { bright(), bright(), bright(), 0.5 + 0.1 * (double)(rng() % 5) };
            TwoInputGroup g;
            std::string error;
            ASSERT_TRUE(buildTwoInputGroup(getApp(), colorA, colorB, &g, &error)) << error;
            const std::string mismatch = groupInputRound(getApp(), g);
            if (!mismatch.empty()) {
                ADD_FAILURE() << "cache stress: NATRON_CACHE_STRESS_SEED=" << seeds[s] << " round " << r << ": " << mismatch;
                break;
            }
        }
    }
}

namespace {

const char* const kTieredChildEnv = "NATRON_TEST_CACHE_STRESS_TIERED";

// Over the cost of writing a small file on any disk, so that what the gate renders earns a file.
const std::chrono::milliseconds kSlowRender(50);

bool
isTieredChild()
{
    const char* const value = std::getenv(kTieredChildEnv);

    return value && value[0];
}

// The node cache takes its tier when the process loads, and every test process but this one has
// switched it off so as not to write into the developer's home directory.
void
expectInTieredChild(const char* test)
{
    QTemporaryDir home;
    ASSERT_TRUE(home.isValid());

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QString::fromUtf8("NATRON_CACHE_DISK_TIER"), QString::fromUtf8("on"));
    env.insert(QString::fromUtf8("HOME"), home.path());
    env.insert(QString::fromUtf8(kTieredChildEnv), QString::fromUtf8("1"));

    QProcess child;
    child.setProcessChannelMode(QProcess::MergedChannels);
    child.setProcessEnvironment(env);
    child.start(QCoreApplication::applicationFilePath(), { QString::fromUtf8("--gtest_filter=CacheStressTieredChild.") + QString::fromUtf8(test) });
    ASSERT_TRUE(child.waitForStarted(30000));
    ASSERT_TRUE(child.waitForFinished(240000));
    const QByteArray output = child.readAll();
    EXPECT_EQ(QProcess::NormalExit, child.exitStatus()) << output.constData();
    EXPECT_EQ(0, child.exitCode()) << output.constData();
    EXPECT_TRUE(output.contains("[  PASSED  ] 1 test")) << output.constData();
}

// Waits out the publisher's backoff, and nothing longer: it returns as soon as a file appears.
bool
publishWhatIsInRam(CacheDiskTier& tier,
                   std::uint64_t publishedBefore)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);

    while (std::chrono::steady_clock::now() < deadline) {
        tier.maintainNow(false);
        if (tier.waitIdle(30000) && (tier.publishedFiles() > publishedBefore)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return false;
}

// An entry's cost excludes the time spent rendering its inputs, so the Blur and Grade entries stay
// too cheap to earn a file however slow the gate is. Only the gate's own entry can reach the disk,
// and it needs one: the gate otherwise renders to a private image.
bool
buildCachedGateGraph(const AppInstancePtr& app,
                     GateGraph* g,
                     std::string* error)
{
    if (!buildGateGraph(app, g, error)) {
        return false;
    }
    if (!setKnobValues(g->gate, "cacheOutput", { 1. })) {
        *error = "could not make the gate cache its output";

        return false;
    }

    return true;
}

std::string
demotionUnderAWaitingRender(const AppInstancePtr& app,
                            Gate* gate,
                            const GateGraph& g,
                            RenderSchedulerModeEnum mode,
                            CacheDiskTier& tier)
{
    RenderedPlane ref;
    std::string error;

    if (!reference(app, g, mode, &ref, &error)) {
        return "the reference failed: " + error;
    }
    clearEverything(app);

    // Other states of the gate leave unheld entries, which are what the pressure below can demote.
    const std::shared_ptr<KnobDoubleBase> value = std::dynamic_pointer_cast<KnobDoubleBase>(g.gate->getKnobByName("value"));
    if (!value) {
        return "the gate has no value knob";
    }
    const std::uint64_t publishedBefore = tier.publishedFiles();
    const double original = value->getValue(0);
    for (int i = 1; i <= 3; ++i) {
        value->setValue(original + 0.25 * i, ViewSpec::all(), 0);
        RenderedPlane other;
        if (!renderProbe(g.blur, kTime, 0, kWindow, mode, false, &other, &error)) {
            return "rendering another state failed: " + error;
        }
    }
    value->setValue(original, ViewSpec::all(), 0);

    const double ramPercent = appPTR->getCurrentSettings()->getRamMaximumPercent();
    std::string mismatch;
    {
        GatedRender first(gate, g, mode);
        if (!first.waitHeld()) {
            return "the render never reached the gate";
        }
        GatedRender waiting(gate, g, mode);
        // Gives the second render time to reach the pending entries. If it is slower the round
        // still checks the same results, only with the wait starting after the demotion.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        appPTR->setApplicationsCachesMaximumMemoryPercent(0.);
        // The queue takes one entry at a time against a budget this small.
        for (int i = 0; i < 4; ++i) {
            appPTR->clearExceedingEntriesFromNodeCache();
            if (!tier.waitIdle(30000)) {
                mismatch = "the demotions never finished";
            }
        }
        appPTR->setApplicationsCachesMaximumMemoryPercent(ramPercent);

        first.finish();
        waiting.finish();
        if (!first.ok() || !waiting.ok()) {
            mismatch = "a render failed: " + first.error() + waiting.error();
        } else {
            std::string diff = compareExact(ref, first.plane());
            if (diff.empty()) {
                diff = compareExact(ref, waiting.plane());
            }
            if (!diff.empty()) {
                mismatch = "a render under demotion differs: " + diff;
            }
        }
    }
    if (!mismatch.empty()) {
        return mismatch;
    }
    if (tier.publishedFiles() <= publishedBefore) {
        return "no entry reached the disk";
    }

    tier.forgetProbes();

    return checkCached(g, mode, ref, "after the demotion");
} // demotionUnderAWaitingRender

std::uint64_t
pixelsOf(const RectI& r)
{
    return r.isNull() ? 0 : (std::uint64_t)r.width() * (std::uint64_t)r.height();
}

// What the node cache and the disk tier hold of the gate's output after each step, printed with a
// failure so that it names where an entry came from. Only peeks and probes: no lookup counts as a
// use, and the probe results it caches are forgotten again.
class GateEntryTrace {
public:
    GateEntryTrace(const GateGraph& g,
                   CacheDiskTier& tier)
        : _g(g)
        , _tier(tier)
        , _originNs(CacheEviction::nowNs())
    {
    }

    void note(const std::string& step)
    {
        std::ostringstream os;

        os << "  [" << msSince(CacheEviction::nowNs()) << " ms] " << step << ": gate renders " << CountingTestRegistry::renders(_g.gate)
           << ", files published " << _tier.publishedFiles() << ", write failures " << _tier.writeFailures() << '\n';
        describeEntries(os);
        _text += os.str();
    }

    std::string dump() const
    {
        return "\nwhat the caches held of the gate's output (times since the trace began, entries at mip 0):\n" + _text;
    }

private:
    double msSince(std::uint64_t ns) const
    {
        return ((double)ns - (double)_originNs) / 1e6;
    }

    void describeEntries(std::ostringstream& os)
    {
        const EffectInstancePtr effect = _g.gate->getEffectInstance();
        const U64 nodeHash = effect->getHash();
        std::set<U64> seen;
        bool any = false;

        for (const bool frameVarying : { false, true }) {
            const ImageKey key = effect->outputCacheKey(nodeHash, frameVarying, kTime, ViewIdx(0), false, 0);
            key.forEachServingKey(0, 0, ImageLayerDesc::getRGBAComponents(), eImageBitDepthFloat, [&](const ImageKey& candidate) {
                if (!seen.insert(candidate.getHash()).second) {
                    return false;
                }
                std::list<ImagePtr> images;
                appPTR->peekImage(candidate, &images);
                for (const ImagePtr& image : images) {
                    any = true;
                    describeImage(os, candidate, image, images.size());
                }
                std::vector<CacheDiskFileInfo> files;
                if (_tier.probe(Image::diskIdentityOfKey(candidate), NULL, &files)) {
                    for (const CacheDiskFileInfo& info : files) {
                        any = true;
                        describeFile(os, candidate, info);
                    }
                }

                return false;
            });
        }
        // A negative result of these probes would otherwise hide a file the next render looks for.
        _tier.forgetProbes();
        if (!any) {
            os << "    nothing of the gate (node hash " << std::hex << nodeHash << std::dec << ")\n";
        }
    }

    void describeImage(std::ostringstream& os,
                       const ImageKey& key,
                       const ImagePtr& image,
                       std::size_t peeked) const
    {
        const RectI bounds = image->getBounds();
        std::list<RectI> rest;
        image->getRestToRender(bounds, rest);
        std::uint64_t missing = 0;
        for (const RectI& r : rest) {
            missing += pixelsOf(r);
        }
        const CacheEntryHeader* const header = image->evictionHeader();

        // An unallocated entry with no holder but this list is the stub peek makes from a file header.
        os << "    " << (image->isAllocated() ? "RAM entry" : (image.use_count() == 1 ? "disk stub" : "unallocated entry")) << " key "
           << std::hex << key.getHash() << std::dec << ", bounds (" << bounds.x1 << ',' << bounds.y1 << ")-(" << bounds.x2 << ',' << bounds.y2
           << "), " << (pixelsOf(bounds) - std::min(missing, pixelsOf(bounds))) << " of " << pixelsOf(bounds) << " pixels rendered, "
           << "shared_ptr holders other than this trace " << (image.use_count() - 1) << " (of " << peeked << " peeked)";
        if (header) {
            const unsigned residency = header->residency.load();
            os << ", residency" << ((residency & CacheEntryHeader::eResidencyHost) ? " host" : "") << ((residency & CacheEntryHeader::eResidencyDisk) ? " disk" : "")
               << ", reuse count " << header->reuseCount.load() << ", last use at " << msSince(header->lastUse.load()) << " ms, recompute cost "
               << header->recomputeCostNs.load() / 1e6 << " ms";
        }
        os << '\n';
    }

    void describeFile(std::ostringstream& os,
                      const ImageKey& key,
                      const CacheDiskFileInfo& info) const
    {
        struct stat st;
        double writtenMs = 0.;
        const bool stated = ::stat(info.path.c_str(), &st) == 0;
        if (stated) {
            writtenMs = msSince((std::uint64_t)st.st_mtim.tv_sec * 1000000000ull + (std::uint64_t)st.st_mtim.tv_nsec);
        }

        os << "    file " << info.path << " key " << std::hex << key.getHash() << std::dec << ", " << info.fileBytes << " bytes, bounds ("
           << info.bounds.x1 << ',' << info.bounds.y1 << ")-(" << info.bounds.x2 << ',' << info.bounds.y2 << "), header claims " << info.renderedPixels
           << " of " << info.totalPixels << " pixels" << (info.allRendered ? " (all rendered)" : "") << ", stats reuse count " << info.stats.reuseCount;
        // Rewriting the stats of a file that is already there moves its mtime too.
        if (stated) {
            os << ", last modified at " << writtenMs << " ms";
        }
        os << '\n';
    }

    const GateGraph& _g;
    CacheDiskTier& _tier;
    const std::uint64_t _originNs;
    std::string _text;
};

std::string
promotionOfAPartialEntry(const AppInstancePtr& app,
                         const GateGraph& g,
                         RenderSchedulerModeEnum mode,
                         CacheDiskTier& tier)
{
    const RectI half(0, 0, kCountingTestSize / 2, kCountingTestSize);
    RenderedPlane halfReference;
    RenderedPlane wholeReference;
    std::string error;
    GateEntryTrace trace(g, tier);

    clearEverything(app);
    trace.note("cleared before the references");
    // The task graph never bypasses the cache, so in that mode these leave entries behind until the
    // clear below.
    if (!renderProbe(g.blur, kTime, 0, half, mode, true, &halfReference, &error) || !renderProbe(g.blur, kTime, 0, kWindow, mode, true, &wholeReference, &error)) {
        return "the references failed: " + error + trace.dump();
    }
    trace.note("references rendered");

    clearEverything(app);
    trace.note("cleared after the references, expecting nothing");
    RenderedPlane plane;
    const std::uint64_t publishedBefore = tier.publishedFiles();
    if (!renderProbe(g.blur, kTime, 0, half, mode, false, &plane, &error)) {
        return "the partial render failed: " + error + trace.dump();
    }
    trace.note("half rendered, expecting one more gate render and a half covered RAM entry");
    if (!publishWhatIsInRam(tier, publishedBefore)) {
        return "the partial entry never reached the disk" + trace.dump();
    }
    trace.note("published, expecting a file whose header claims half the pixels");
    appPTR->clearAllCachesInRam();
    tier.forgetProbes();
    trace.note("RAM cleared, expecting the file only");

    const int rendersBefore = CountingTestRegistry::renders(g.gate);
    if (!renderProbe(g.blur, kTime, 0, half, mode, false, &plane, &error)) {
        return "the promoted render failed: " + error + trace.dump();
    }
    trace.note("half rendered again, expecting no gate render and the promoted half covered entry");
    std::string diff = compareExact(halfReference, plane);
    if (!diff.empty()) {
        return "the promoted region differs: " + diff + trace.dump();
    }
    if (CountingTestRegistry::renders(g.gate) != rendersBefore) {
        return "the region on disk was rendered again" + trace.dump();
    }

    if (!renderProbe(g.blur, kTime, 0, kWindow, mode, false, &plane, &error)) {
        return "the render of the rest failed: " + error + trace.dump();
    }
    trace.note("whole rendered, expecting one more gate render");
    diff = compareExact(wholeReference, plane);
    if (!diff.empty()) {
        return "the rest differs from a render from empty caches: " + diff + trace.dump();
    }
    if (CountingTestRegistry::renders(g.gate) == rendersBefore) {
        return "the rest was not rendered" + trace.dump();
    }

    return std::string();
} // promotionOfAPartialEntry

class CacheStressTieredChild
    : public CacheStressConcurrentTest {
};

} // anon namespace

TEST_F(CacheStressConcurrentTest, DemotionWhileARenderWaitsOnAPendingRegion)
{
    expectInTieredChild("DemotionWhileARenderWaits");
}

TEST_F(CacheStressConcurrentTest, PromotionOfAPartiallyRenderedEntry)
{
    expectInTieredChild("PromotionOfAPartialEntry");
}

// While a render is held mid-entry and a second one waits on the regions it marked pending, RAM
// pressure pushes everything unheld to the disk. Both renders must still finish with the pixels of
// a render from empty caches.
TEST_F(CacheStressTieredChild, DemotionWhileARenderWaits)
{
    if (!isTieredChild()) {
        std::printf("Only runs in the process the parent test starts.\n");

        return;
    }
    const std::shared_ptr<CacheDiskTier> tier = appPTR->getNodeCacheDiskTier();
    ASSERT_TRUE(tier != nullptr);

    Gate gate;
    const AppInstancePtr app = getApp();
    const int rounds = stepBudget(3);
    std::atomic<const Node*> slowNode(nullptr);
    CountingTestRegistry::setRenderHook([&gate, &slowNode](const Node* node) {
        if (node == slowNode.load()) {
            std::this_thread::sleep_for(kSlowRender);
        }
        gate.hold(node);
    });
    for (int m = 0; m < 2; ++m) {
        const RenderSchedulerModeEnum mode = (m == 0) ? eRenderSchedulerModeLegacy : eRenderSchedulerModeTaskGraph;
        GateGraph g;
        std::string error;
        ASSERT_TRUE(buildCachedGateGraph(app, &g, &error)) << error;
        gate.setNode(g.gate.get());
        slowNode.store(g.gate.get());
        for (int r = 0; r < rounds; ++r) {
            const std::string mismatch = demotionUnderAWaitingRender(app, &gate, g, mode, *tier);
            EXPECT_TRUE(mismatch.empty()) << "mode " << m << " round " << r << ": " << mismatch;
        }
        slowNode.store(nullptr);
        gate.setNode(nullptr);
    }
    EXPECT_EQ(0U, tier->writeFailures());
}

// The file of a half rendered entry comes back into RAM with what it holds: that half renders
// nothing, the other half renders only then, and the two together match a whole render.
TEST_F(CacheStressTieredChild, PromotionOfAPartialEntry)
{
    if (!isTieredChild()) {
        std::printf("Only runs in the process the parent test starts.\n");

        return;
    }
    const std::shared_ptr<CacheDiskTier> tier = appPTR->getNodeCacheDiskTier();
    ASSERT_TRUE(tier != nullptr);

    const AppInstancePtr app = getApp();
    std::atomic<const Node*> slowNode(nullptr);
    CountingTestRegistry::setRenderHook([&slowNode](const Node* node) {
        if (node == slowNode.load()) {
            std::this_thread::sleep_for(kSlowRender);
        }
    });
    for (int m = 0; m < 2; ++m) {
        const RenderSchedulerModeEnum mode = (m == 0) ? eRenderSchedulerModeLegacy : eRenderSchedulerModeTaskGraph;
        GateGraph g;
        std::string error;
        ASSERT_TRUE(buildCachedGateGraph(app, &g, &error)) << error;
        slowNode.store(g.gate.get());
        const std::string mismatch = promotionOfAPartialEntry(app, g, mode, *tier);
        EXPECT_TRUE(mismatch.empty()) << "mode " << m << ": " << mismatch;
        slowNode.store(nullptr);
    }
    EXPECT_EQ(0U, tier->writeFailures());
}
