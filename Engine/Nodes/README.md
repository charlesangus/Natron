# Native nodes

A native node is an `EffectInstance` subclass built directly into `NatronEngine`
rather than loaded as an OFX plugin bundle. This directory is where those
subclasses live, and this file is the orientation for writing one: the sizing
rule for where a new file goes, the steps to get from an empty `.cpp` to a
registered, testable node, and the rules an author needs to know about data
kinds and the single `EffectInstance` hierarchy. `NativeEffectBase.h`'s
class-level comment carries a full worked example that compiles as written;
read that for the API reference. This file is the map of the directory and
the rules that example doesn't state.

## What goes here, and what doesn't

New node implementations belong under `Engine/Nodes/<Domain>/`, e.g.
`Engine/Nodes/Deep/`, `Engine/Nodes/Scene/`. A node with no natural domain
(like the framework's own proof node) sits directly under `Engine/Nodes/`.
The point is to keep `Engine/` core -- `EffectInstance`, `Node`, `Project`, the
render scheduler, the cache -- free of node implementations, so the core stays
readable as core.

`Dot`, `Backdrop`, `RotoNode`, `NodeGroup`, `GroupInput`/`GroupOutput`,
`TrackerNode`, and the other pre-existing built-ins registered in
`AppManager::loadBuiltinNodePlugins()` stay where they already live in
`Engine/`. That is deliberate, not an oversight left for cleanup: moving them
is a large, purely-mechanical, high-risk-of-merge-conflict change with no
behavioral payoff, and it is not this framework's job. Don't relocate them as
a drive-by "while I'm in here" change.

## Single hierarchy: no parallel Op classes

Every node, whatever kind of data it produces or consumes, is an
`EffectInstance`. A node that needs to do something a plain image node
doesn't -- render deep data, evaluate a scene -- gets a capability virtual
added to `EffectInstance` (in the shape of `renderDeep()` alongside `render()`,
once deep rendering lands), not a new parallel base class in the shape of
Nuke's `Iop`/`DeepOp`/`GeoOp` split. `EffectInstance` is where the knob
system, hashing, undo/redo, serialization, and the render scheduler all
attach; a second hierarchy would have to either duplicate all of that or
awkwardly share it, for no isolation benefit. If a task seems to call for a
new base class alongside `EffectInstance`, that's a sign to add a virtual to
`EffectInstance` instead, not a sign this rule doesn't apply to your case.

`NativeEffectBase` (below) is not an exception to this: it's a convenience
layer *on* `EffectInstance`, not a second hierarchy.

## `NativeEffectBase`

Writing an `EffectInstance` subclass directly means overriding a half-dozen
one-line accessors (`getPluginID()`, `getPluginLabel()`, `getPluginGrouping()`,
`getMajorVersion()`, `getMinorVersion()`, `getNInputs()`, `getInputDataKind()`,
...) for every node. `NativeEffectBase` asks for that metadata once, as a
single `NativePluginDescription` returned from `getNativePluginDescription()`,
and implements those `EffectInstance` virtuals from it. It adds no capability
virtuals of its own -- see "Single hierarchy" above.

What it supplies so a subclass doesn't have to:

- `getPluginID()`, `getPluginLabel()`, `getPluginDescription()`,
  `getPluginGrouping()`, `getMajorVersion()`, `getMinorVersion()`,
  `getNInputs()`, `getInputLabel()`, `isInputOptional()`,
  `getOutputDataKind()`, `getInputDataKind()` -- all derived from
  `getNativePluginDescription()`.
- `addAcceptedComponents()` -- defaults to RGB, RGBA and Alpha.
- `addSupportedBitDepth()` -- defaults to byte, short and float.
- `renderThreadSafety()` -- defaults to `eRenderSafetyFullySafeFrame`.
- `createKnob<KNOB_TYPE>(label, dimension)` -- wraps the
  `AppManager::createKnob(this, label, dimension)` idiom every
  `initializeKnobs()` otherwise repeats.

What a subclass still must provide, exactly as it would on top of
`EffectInstance` directly: `getNativePluginDescription()` (the only pure
virtual `NativeEffectBase` adds), `initializeKnobs()`, and its render
behavior (`render()`, or `isIdentity()` if the node is a pass-through). Any
of the defaults above can still be overridden per-node if a node genuinely
needs, say, a different accepted-components list -- `NativeEffectBase` only
removes the *obligation* to write them, it doesn't seal them off.

## Writing a node, start to finish

`TypedPassthrough` (`Engine/Nodes/TypedPassthrough.h` /
`Engine/Nodes/TypedPassthrough.cpp`) is the framework's own proof node and a
complete, minimal, real example of every step below. It's referenced
throughout; read it alongside this section.

1. **Header**: declare a class deriving from `NativeEffectBase`, with a
   `static EffectInstance* BuildEffect(NodePtr node)` factory function and a
   constructor that forwards `node` to `NativeEffectBase`. Give the plugin id
   its own `#define PLUGINID_NATRON_...` string macro next to the class, the
   same way `TypedPassthrough.h` defines
   `PLUGINID_NATRON_TYPEDPASSTHROUGH` -- tests and `AppManager.cpp` both need
   that id and should not restate the string literal.

   ```cpp
   class MyNode
       : public NativeEffectBase
   {
   public:
       static EffectInstance* BuildEffect(NodePtr node)
       {
           return new MyNode(node);
       }

       explicit MyNode(NodePtr node)
           : NativeEffectBase(node)
       {
       }

   private:
       virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;
       virtual void initializeKnobs() OVERRIDE FINAL;
       // ... plus whichever render-behavior virtual this node needs, see step 3.
   };
   ```

2. **Declare the plugin metadata**: implement `getNativePluginDescription()`
   in the `.cpp`, filling in `id`, `label`, `description`, `grouping` (one of
   the `PLUGIN_GROUP_*` constants in `Global/Macros.h` -- `PLUGIN_GROUP_DEEP`
   and `PLUGIN_GROUP_3D` already exist for the corresponding domains),
   `majorVersion`, `minorVersion`, one `NativeInputDescription` per input
   (label, whether it's optional, its `DataKindEnum`), and `outputKind`. See
   "Data kinds" below for what to put in the kind fields.

3. **Knobs and render behavior**: implement `initializeKnobs()` using
   `createKnob<KnobType>(tr("Label"))`. Implement the node's actual behavior:
   override `render()` for a node that produces new pixels, or `isIdentity()`
   for a pass-through that just forwards an input unchanged (`TypedPassthrough`
   does the latter -- it has no `render()` override at all, and relies on the
   same identity-render path `Dot` and `NoOpBase` use).

4. **Register it**: add one line to
   `AppManager::loadBuiltinNodePlugins()` in `Engine/AppManager.cpp`:

   ```cpp
   registerBuiltInPlugin<MyNode>(QString::fromUtf8(NATRON_IMAGES_PATH "myNode_icon.png"), false, false);
   ```

   The first argument is the toolbar/node-graph icon path (an empty
   `QString::fromUtf8("")` is fine if there's no icon yet -- that's what
   `TypedPassthrough` does); the two bools are `isDeprecated` and
   `internalUseOnly` respectively. `registerBuiltInPlugin<T>()` instantiates
   the node once (with a null `NodePtr`) purely to read its static metadata
   (id, label, grouping, reader/writer-ness, version, thread safety) back off
   it and register a `Plugin*` -- it is not a live node instance.
   `#include` the new node's header in `AppManager.cpp` alongside the other
   `Engine/Nodes/` includes.

5. **Test it**: see "Testing" below.

## Data kinds

`DataKindEnum` (`Global/Enums.h`) has four values:

- `eDataKindImage` -- a flat raster image. The default for every input and
  output that doesn't say otherwise, so every existing node and every OFX
  plugin is correctly typed with zero changes.
- `eDataKindDeep` -- deep data (per-pixel depth-sample lists). At this stage
  of the framework, this is a **declaration only**: a node can declare an
  input or output as `eDataKindDeep` and have that participate correctly in
  connection typing, but there is no deep payload type, no deep cache, and no
  deep render path yet. Declaring a kind does not mean a node can render that
  kind of data today.
- `eDataKindScene` -- 3D scene data. Same caveat as deep: declaration only,
  no scene payload or evaluation path yet.
- `eDataKindPolymorphic` -- "no fixed kind of my own; take on whatever feeds
  me." For pass-through utility nodes (`Dot`, `Switch`, `NoOps`, group
  boundaries, `TypedPassthrough`), not for nodes that actually produce or
  transform data.

A node declares kinds via `NativePluginDescription::inputs[i].kind` and
`NativePluginDescription::outputKind` (or, off `NativeEffectBase`, by
overriding `EffectInstance::getInputDataKind()` /
`getOutputDataKind()` directly).

**Kinds are static plugin declarations, not per-instance or per-edge state.**
`getInputDataKind()` / `getOutputDataKind()` are declared once per plugin
class, not configured per node instance and not stored per connection. An
edge carries no kind of its own -- its kind is simply its source node's
*effective* output kind -- so nothing about data kinds is ever serialized
into a project, and nothing about it can go stale independently of the graph
itself.

A polymorphic node's *effective* output kind (`Node::getEffectiveOutputDataKind()`,
`Engine/Node.cpp`) is resolved structurally: it walks the node's declared
inputs, resolves each upstream node's effective kind in turn, and takes the
first non-polymorphic kind it finds; a polymorphic node with nothing feeding
it (or only other unresolved polymorphic nodes upstream) resolves to
`eDataKindPolymorphic` itself -- "unconstrained," not a silent fallback to
image. The result is cached per node and invalidated on connection change,
since recomputing it is a graph walk, not a field read.

Enforcement has three separate points. Each one compares a **resolved**
kind on the upstream/source side (`Node::getEffectiveOutputDataKind()`,
walking through any polymorphic nodes as above) against the **declared**
kind of the consuming input (`EffectInstance::getInputDataKind(inputNumber)`,
a fixed, per-plugin, per-input value) -- never resolved against resolved. A
consumer's accepted kind for a given input is fixed by the plugin and needs
no resolving; only the upstream side has to be walked through the graph to
find out what it currently, structurally is. Do not assume any single one of
these three points is the whole story:

1. **Connection time** -- `Node::canConnectInput()`
   (`Engine/NodeInputs.cpp`) is the single choke point used by every GUI drag,
   undo/redo, auto-connect, and the Python API. It calls
   `Node::checkDataKindCompatibility()`, which resolves the upstream node's
   effective output kind and compares it against this node's declared kind
   for that input, returning `Node::eCanConnectInput_incompatibleDataKind`
   on a mismatch and naming the conflicting (upstream) node. This check is
   skipped while a project is loading (see point 2) because mid-restore the
   answer depends on how much of the tree has been reconnected so far.
2. **Project load** -- `ProjectPrivate::revalidateDataKindEdges()`
   (`Engine/ProjectPrivate.cpp`) runs once, after the whole node tree and all
   its connections have been restored, and re-checks every restored edge with
   `checkDataKindCompatibility()`. Since kinds are never serialized, this is
   the only place a stale or contradictory kind combination (e.g. a project
   saved before an upstream node's declared kind changed) gets caught: an
   incompatible edge is dropped (disconnected) and a warning is written to
   the error log -- never silently miswired -- the same policy as a missing
   plugin.
3. **Chain simulation for polymorphic nodes** -- connecting a new upstream
   source into a node whose own output is polymorphic (e.g. `Dot`,
   `TypedPassthrough`) does more than re-run point 1 for that one edge:
   `checkDataKindCompatibility()` also simulates what the polymorphic node's
   resolved output kind would become with the new connection in place
   (`Node::resolveEffectiveOutputDataKindFromInputsWithOverride()`), then
   walks that simulated kind forward through the node's existing downstream
   consumers (`Node::findDataKindConflictDownstream()`, recursing through any
   further polymorphic nodes), comparing it against each concrete consumer's
   declared input kind -- still resolved/simulated upstream value against
   declared consumer input, just propagated forward first. This is what
   rejects `deep source -> Dot` when `Dot -> image sink` was already
   connected first (fine at the time, since `Dot` was still unconstrained):
   the new connection would retroactively make the already-connected sink
   incompatible, so it is rejected at the connection that introduces the
   contradiction, naming the sink as the conflicting node.

`Tests/DataKind_Test.cpp`, `Tests/DataKindProjectLoad_Test.cpp`, and
`Tests/TypedPassthrough_Test.cpp` exercise all three points end to end,
including through `TypedPassthrough` itself, and are worth reading alongside
this section.

## Adapters are Viewer-only

The only implicit data-kind conversion anywhere in the graph is deep-to-image
at the Viewer: the Viewer accepts a deep edge directly and auto-flattens it.
No other node accepts an edge of a different kind than it declares, and there
is no general adapter-insertion mechanism for mid-graph conversions -- an
information-destroying conversion elsewhere in the graph requires an
explicit node (e.g. a `DeepToImage`), so the graph always shows where such a
conversion happens. A new node should never rely on, or attempt to register,
an implicit conversion of its own; if a node needs data in a different kind
than what feeds it, it takes that kind as its declared input and requires an
explicit converter node upstream.

## Testing

A native node is tested the same way as any other node in `Tests/`, using
`BaseTest`'s `createNode()` / `connectNodes()` helpers. `Tests/TypedPassthrough_Test.cpp`
is the pattern to copy: it checks that the plugin is registered and
instantiable by id, that its effective output kind resolves correctly both
fed and unfed, that a kind contradiction introduced through it is rejected
exactly as it would be through `Dot`, and -- since `TypedPassthrough` relies
on the `isIdentity()` route rather than a `render()` override -- that
splicing it into a render chain doesn't change the rendered result, by
rendering the same chain with and without the node and comparing every
output pixel. Add the new test file to `Tests_SOURCES` in `Tests/CMakeLists.txt`.
