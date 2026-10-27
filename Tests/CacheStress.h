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

#ifndef NATRON_TESTS_CACHESTRESS_H
#define NATRON_TESTS_CACHESTRESS_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/RectI.h"

#include "RenderBothWays.h"

NATRON_NAMESPACE_ENTER

/*
 * Seeded cache-invalidation stress.
 *
 * Every step applies one edit to a small graph and renders a probe node through the caches; the
 * result must equal, bit for bit, a render of the same state from empty caches. A reference render
 * empties the caches, so a run is two passes over the same graph: pass A applies the ops and
 * renders each step from empty caches, pass B rebuilds the graph, applies the same ops and renders
 * through the caches without ever clearing them. A reference render that fails fails the seed; one
 * that produces nothing (an empty output) is a valid reference the cached render must match.
 *
 * NATRON_CACHE_STRESS_SEED=<n> replays one seed, NATRON_CACHE_STRESS_STEPS=<n> changes the step
 * budget and NATRON_CACHE_STRESS_SEEDS=<n> adds n seeds to every case (the heavy mode).
 */
namespace CacheStress {

enum OpKind {
    eOpConnect = 0, // connect a candidate source to an empty slot
    eOpDisconnect, // empty an optional slot
    eOpReplace, // connect another candidate source to a connected slot
    eOpSwap, // switchInput0And1
    eOpSetValue, // a value, at the current time if the knob is animated
    eOpAddKey,
    eOpRemoveKey,
    eOpSetExpression, // reads one dimension of another knob
    eOpClearExpression,
    eOpFrameExpression, // depends on frame, on a knob without keyframes
    eOpWholeValueExpression, // reads a 1-D knob with getValue(), a dimension -1 dependency
    eOpLink,
    eOpUnlink,
    eOpToggleDisabled,
    eOpFrameRange,
    eOpTime,
    eOpRevisit, // undoes the last one to three steps
    eOpViewerB, // switches the viewer's B side
    eOpMipmap, // renders at another mipmap level
    eOpRewriteFile, // rewrites a file a node reads, behind its back
    eOpReload, // presses a Read's Reload
    eOpRename, // renames an expression source, often to another node's name plus a digit
    eOpDynamicExpression, // as setExpression, but finds the node by a computed name the parser cannot see
    eOpCount
};

typedef std::uint32_t OpMask;

inline OpMask
opBit(OpKind kind)
{
    return OpMask(1) << unsigned(kind);
}

// The whole-value expression op stays out of the default mask while its hole is open (see
// DISABLED_WholeValueExpression_M72). The viewer B and mipmap ops are only exercised by their
// scripted cases, ViewerABSwitch and IdentityAcrossScales; the file ops only by scripted cases on
// graphs that list files.
OpMask defaultOps();
OpMask allOps();

const char* opName(OpKind kind);

struct KnobTarget {
    int node = -1;
    std::string name;
    int dim = 0;
    std::vector<double> values;
    // Keyframes are only added to animatable targets; expression sources are taken from the
    // others, so a node that reads one through an expression is never frame-varying unseen.
    bool animatable = true;
};

struct InputSlot {
    int node = -1;
    int input = 0;
    std::vector<int> sources;
    bool optional = true;
};

struct StressGraph {
    std::vector<NodePtr> nodes;
    std::vector<KnobTarget> knobs;
    std::vector<InputSlot> inputSlots;
    std::vector<int> swappable; // nodes whose inputs 0 and 1 may be swapped
    std::vector<int> toggles; // nodes that may be disabled
    std::vector<std::pair<int, int>> links; // knob target pairs, linked whole-knob
    std::vector<int> exprTargets; // knob targets that may get an expression
    std::vector<int> wholeValueSources; // 1-D knob targets read with getValue()
    int probe = -1; // the node rendered; ignored when viewer is set
    int viewer = -1; // an InspectorNode whose B side picks the probe
    std::vector<int> viewerInputs; // the viewer inputs eOpViewerB may pick
    RectI window; // at mipmap 0
    int firstTime = 1;
    int lastTime = 6;
    // Writes version n of a file the graph reads; the builder writes version 0.
    std::vector<std::function<bool(int version, std::string* error)>> files;
};

// Creates the graph in app's project; the same builder runs once per pass.
typedef std::function<bool(const AppInstancePtr& app, StressGraph* graph, std::string* error)> GraphBuilder;

enum PrimitiveKind {
    ePrimSetInput = 0, // source -1 disconnects
    ePrimSwap,
    ePrimSetValue,
    ePrimSetValueAtTime,
    ePrimDeleteKey,
    ePrimRemoveAnimation,
    ePrimSetExpression, // empty text clears
    ePrimLink,
    ePrimUnlink,
    ePrimSetDisabled,
    ePrimFrameRange,
    ePrimSeek,
    ePrimViewerB,
    ePrimMipmap,
    // first: the file, input: the version. Until the next Reload, a render may fail or render
    // nothing, and one that succeeds must equal the last render before the rewrite.
    ePrimRewriteFile,
    ePrimReload,
    ePrimRename, // text: the new script name
    ePrimSetting, // text: an application Settings knob, set at dim to value; scripted only
    ePrimAddLayer // text: a project layer added, or grown, to first channels; scripted only
};

struct Primitive {
    PrimitiveKind kind = ePrimSetValue;
    int node = -1;
    int input = 0;
    int source = -1;
    int knob = -1;
    int dim = 0;
    int link = -1;
    double value = 0.;
    double time = 0.;
    int first = 0;
    int last = 0;
    std::string text;
    // Expressions name nodes by script name, resolved when the primitive is applied.
    int exprSourceKnob = -1;
    int exprSourceDim = 0; // -1 reads the whole value
    bool exprDynamic = false; // the source node and getter are found by getattr on computed names
};

struct Step {
    OpKind kind = eOpSetValue;
    std::vector<Primitive> prims;
};

struct Options {
    OpMask ops = 0;
    int steps = 24;
    bool legacy = true;
    bool taskGraph = true;
    // Applied before the seeded steps, in order, each as one step.
    std::vector<Step> scripted;
    // Seeded steps after the scripted ones; 0 with a script runs the script alone.
    bool seeded = true;
    // When set, the cached pass renders through it instead of the two modes and checks the result
    // against the Legacy reference with compare (compareExact when unset).
    std::function<bool(const NodePtr& node, double time, unsigned mipmapLevel, const RectI& window, RenderedPlane* out, std::string* error)> cachedRender;
    std::function<std::string(const RenderedPlane& reference, const RenderedPlane& got)> compare;
    // When set with cachedRender, the reference pass also renders every step through
    // cachedRender from empty caches, and the cached pass must match that render exactly before
    // compare checks it against the Legacy reference.
    bool cachedRenderReference = false;
    std::string cachedRenderName = "custom";
};

struct Result {
    bool ok = true;
    std::uint32_t seed = 0;
    int failedStep = -1;
    std::string report;
};

// One seed, both passes. The report names the seed, the failing step, the mode and the op log.
Result run(const AppInstancePtr& app, const GraphBuilder& builder, std::uint32_t seed, const Options& options);

// Runs each seed (or NATRON_CACHE_STRESS_SEED alone) and adds a gtest failure per failing seed.
// Returns the number of seeds that failed.
int expectClean(const AppInstancePtr& app, const GraphBuilder& builder, const std::vector<std::uint32_t>& seeds, Options options);

// The step budget after NATRON_CACHE_STRESS_STEPS.
int stepBudget(int defaultSteps);

// The seeds a case runs after NATRON_CACHE_STRESS_SEED and NATRON_CACHE_STRESS_SEEDS.
std::vector<std::uint32_t> seedsToRun(const std::vector<std::uint32_t>& defaults);

// Empties every cache, then the actions cache of every node of app's project.
void clearEverything(const AppInstancePtr& app);

// The colour plane of a render in the given mode from a node, with byPassCache as given.
bool renderProbe(const NodePtr& node,
                 double time,
                 unsigned mipmapLevel,
                 const RectI& window,
                 RenderSchedulerModeEnum mode,
                 bool byPassCache,
                 RenderedPlane* out,
                 std::string* error);

// "" when both planes have the same window, layer and channels, and every sample is bit-exact;
// otherwise the first difference.
std::string compareExact(const RenderedPlane& reference, const RenderedPlane& got);

Primitive setValuePrim(int knob, int dim, double value);
Primitive setInputPrim(int node, int input, int source);
Primitive viewerBPrim(int viewerInput);
Primitive mipmapPrim(int level);
Primitive rewriteFilePrim(int file, int version);
Primitive reloadPrim(int node);
// Each pass starts from the setting's value before the run and puts it back after.
Primitive settingPrim(const std::string& knobName, int dim, double value);
// The registry only grows, so this has no inverse; the next pass starts from a reset project.
Primitive addLayerPrim(const std::string& layerID, int channels);
Primitive expressionPrim(int knob, int dim, const std::string& text);
Primitive dimensionExpressionPrim(int knob, int dim, int sourceKnob, int sourceDim);

std::string describe(const StressGraph& graph, const Primitive& prim);

// Graph-building helpers; nodes are created without auto-connection, at the given major version
// (-1 for the newest).
NodePtr createIn(const AppInstancePtr& app, const NodeCollectionPtr& parent, const std::string& pluginID, int major = -1);
NodePtr createConstantIn(const AppInstancePtr& app, const NodeCollectionPtr& parent, const RectI& extent, const std::vector<double>& color);
// A group with neither GroupInput nor GroupOutput yet.
NodePtr createEmptyGroupIn(const AppInstancePtr& app, const NodeCollectionPtr& parent);
// A viewer with no input; it needs kCreateNodeArgsPropAllowNonUserCreatablePlugins.
NodePtr createViewerIn(const AppInstancePtr& app, const NodeCollectionPtr& parent);
// The index of node in graph->nodes, or -1 for a null node.
int addNode(StressGraph* graph, const NodePtr& node);
int addKnob(StressGraph* graph, int node, const std::string& name, int dim, const std::vector<double>& values, bool animatable = true);
void addSlot(StressGraph* graph, int node, int input, const std::vector<int>& sources, bool optional = true);

} // namespace CacheStress

NATRON_NAMESPACE_EXIT

#endif // NATRON_TESTS_CACHESTRESS_H
