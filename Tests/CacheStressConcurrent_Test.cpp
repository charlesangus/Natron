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
#include "CacheStressTestEffect.h"
#include "CountingTestEffect.h"
#include "NativeParity.h"
#include "SceneTestEffect.h"

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

// A render thread reads the viewer hash at the moment the A/B switch marks it: the mark comes
// after the active inputs change, so the hash it recomputes, and the one the viewer keeps, name
// the input now shown.
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
            std::atomic<bool> armed(true);
            std::thread worker;
            Node::setHashMarkTraceForTesting([&](const Node* node,
                                                 const char* /*reason*/,
                                                 const KnobI* /*knob*/) {
                if ((node != viewerNode.get()) || !armed.exchange(false)) {
                    return;
                }
                worker = std::thread([&]() {
                    (void)viewerNode->getHashValue();
                    {
                        std::lock_guard<std::mutex> l(m);
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
            if (viewerNode->getHashValue() != hashOfB[next]) {
                ADD_FAILURE() << "cache stress race: NATRON_CACHE_STRESS_SEED=" << seeds[s] << " round " << r
                              << ": the viewer shows input " << next << " under another hash than that input's\nswitch log:\n"
                              << log.str();
                break;
            }
        }
    }
}
