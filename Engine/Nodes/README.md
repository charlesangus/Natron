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

`NativeEffectBase` is rooted at `OutputEffectInstance`, not `EffectInstance`
directly. `OutputEffectInstance` is not "a writer" -- `OfxEffectInstance`,
`NodeGroup`, `NoOpBase`, `DiskCacheNode` and `ViewerInstance` all derive from
it too -- it is "an effect that can own a `RenderEngine`", the thing every
real writer entry point (the GUI Render menu, the CLI's `-w`, Python's
`app.render()`) requires via `dynamic_cast<OutputEffectInstance*>` before it
will touch a node. Owning one is cheap for a node that is never rendered as a
root: `RenderEngine`'s constructor only connects a signal, and its scheduler
thread is created lazily on the first actual render-sequence request. Whether
a given native node *is* a writer is a plugin-description flag, exactly as an
OFX plugin's writer-ness follows from its context -- see below.

Writing an `EffectInstance` subclass directly means overriding a half-dozen
one-line accessors (`getPluginID()`, `getPluginLabel()`, `getPluginGrouping()`,
`getMajorVersion()`, `getMinorVersion()`, `getNInputs()`, `getInputDataKind()`,
`isWriter()`, `isOutput()`, ...) for every node. `NativeEffectBase` asks for
that metadata once, as a single `NativePluginDescription` returned from
`getNativePluginDescription()`, and implements those `EffectInstance` and
`OutputEffectInstance` virtuals from it. It adds no capability virtuals of its
own -- see "Single hierarchy" above.

What it supplies so a subclass doesn't have to:

- `getPluginID()`, `getPluginLabel()`, `getPluginDescription()`,
  `getPluginGrouping()`, `getMajorVersion()`, `getMinorVersion()`,
  `isWriter()`, `isOutput()`, `getNInputs()`, `getInputLabel()`,
  `isInputOptional()`, `getOutputDataKind()`, `getInputDataKind()` -- all
  derived from `getNativePluginDescription()`. `isWriter()` and `isOutput()`
  both follow the description's `isWriter` flag, default `false`: a node that
  is meant to be a render root -- a writer -- sets it, and both accessors
  report true; every other native node reports false for both, same as
  before this class carried a `RenderEngine` at all.
- `addAcceptedComponents()` -- defaults to RGB, RGBA and Alpha.
- `addSupportedBitDepth()` -- defaults to byte, short and float.
- `renderThreadSafety()` -- defaults to `eRenderSafetyFullySafeFrame`.
- `createKnob<KNOB_TYPE>(label, dimension)` -- wraps the
  `AppManager::createKnob(this, label, dimension)` idiom every
  `initializeKnobs()` otherwise repeats.

What a subclass still must provide, exactly as it would on top of
`OutputEffectInstance` directly: `getNativePluginDescription()` (the only pure
virtual `NativeEffectBase` adds), `initializeKnobs()`, and its render
behavior (`render()`, or `isIdentity()` if the node is a pass-through). A
writer sets `NativePluginDescription::isWriter = true`; nothing else about
writing a native node changes.

The plugin-identity accessors -- `getPluginID()`, `getPluginLabel()`,
`getPluginDescription()`, `getPluginGrouping()`, `getMajorVersion()`,
`getMinorVersion()`, `isWriter()`, `isOutput()` -- are `OVERRIDE FINAL` on
purpose: `NativePluginDescription` is meant to be the single place a node's
identity is written, and a subclass changes them by changing what it returns
from `getNativePluginDescription()`, not by overriding the accessor. The
remaining defaults are plain overrides that a node may replace when the
default doesn't fit it: `getNInputs()`, `getInputLabel()`, `isInputOptional()`,
`getOutputDataKind()`, `getInputDataKind()`, `resolveOutputDataKind()`,
`inputParticipatesInDataKindPropagation()`, `addAcceptedComponents()`,
`addSupportedBitDepth()` and `renderThreadSafety()`.

## Per-frame metadata

`NativeEffectBase::getOutputMetadata(time, view)` returns the `ImageMetadata`
(`Engine/Nodes/Metadata/ImageMetadata.h`) of the image the node outputs at that
frame and view. It is not virtual: it caches what the protected virtual
`deriveOutputMetadata(time, view)` returns. The default derivation passes
through, unchanged, the metadata of the first connected non-mask input, so
every existing node is transparent to metadata with no edit. A node that adds,
edits or drops keys overrides `deriveOutputMetadata()`, usually starting from
`getUpstreamMetadata(time, view)` or `getInputMetadata(inputNb, time, view)`.

- What an input gives depends on its kind. A native input answers with its own
  `getOutputMetadata()`, so the lookup recurses up a native chain. An OpenFX
  input, or a Read/Write container, answers with its output clip's metadata
  (the embedded decoder's or encoder's for a container), converted by
  `OfxMetadataBridge`. Any other kind of effect gives an empty map.
  `getInputEffectMetadata()` is the one place that lookup is extended.
- Every node's result is cached per `(time, view)`, overrides included, and
  dropped on every knob change of the node or anything upstream of it, every
  connection change (both arrive as a node hash change) and every
  clip-preferences refresh. A derivation must therefore depend only on its
  knobs, its upstream metadata and project settings.
- The cache lock is a leaf. Derivation runs downstream to upstream and holds no
  lock while it asks upstream nodes; invalidation runs upstream to downstream
  and takes only that leaf lock. A derivation must not hold a lock of its own
  while asking upstream nodes. The reasoning is in
  `docs/decisions/2026-09-10-metadata-cache-locking.md`.
- An OpenFX input clip fed by a native node carries exactly that node's
  metadata, converted. The host adds none of the keys it derives for an
  unconnected clip: every standard key describes the source, so a value made up
  from the clip or the timeline would misdescribe it.

Framework policies: `Merge` takes the metadata of its A input (then A2.., then
B if no A is connected), never B's; `NativeGenerator` outputs only
`ofx/framerate` and `ofx/pixelaspect` (it has no source, and `ofx/frame` is the
frame number within the source, so it is omitted); transforms and filters
keep the default.

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
   "Data kinds" below for what to put in the kind fields. Set `isWriter = true`
   if this node is meant to be rendered as a root -- a writer -- through the
   GUI Render menu, the CLI's `-w`, or Python's `app.render()`; leave it at
   its default `false` otherwise, which is what every node needed before this
   flag existed.

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
- `eDataKindPolymorphic` -- on an input, "I accept any kind"; on an output,
  "I have no fixed kind of my own." For pass-through utility nodes (`Dot`, the
  other `NoOps`, group boundaries, `TypedPassthrough`), not for nodes that
  actually produce or transform data. Only nodes that override the kind
  virtuals are polymorphic: an OFX plugin that happens to pass its input
  through, like openfx-misc's `Switch`, is loaded as an `OfxEffectInstance`
  and is typed image/image like every other plugin.

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

**Resolution policy belongs to the node.** A polymorphic node's *effective*
output kind is `Node::getEffectiveOutputDataKind()` (`Engine/Node.cpp`), which
asks the node's own policy. A `NativeEffectBase` subclass supplies one by
overriding `NativeEffectBase::resolveOutputDataKind()` -- that is how a native
switch would follow the input it has selected rather than all of them. Every
other node, including every built-in pass-through, uses the engine default,
`Node::resolveStructuralOutputDataKind()`. OFX plugins are untouched by any of
this: they never override the kind virtuals, so they are image/image.

A node that supplies its own policy must also say which of its inputs that
policy follows, by overriding
`NativeEffectBase::inputParticipatesInDataKindPropagation()`. That is what
stops a kind required of its output from reaching back through a branch it
never reads: a switch following its selected input must return false for the
others, or those branches are typed, rendered and connection-checked as the
kind the switch outputs. The default is true, matching the engine default
below, which follows every polymorphic-declared input at once -- so a node
without a policy needs nothing here.

The engine default resolves **bidirectionally**, from the current topology
alone:

- Upstream, through the node's **polymorphic-declared inputs only**. An input
  declaring a concrete kind says what may connect to it (a mask declared
  `eDataKindImage` stays an image input whatever the node passes through) and
  never decides the output kind.
- Downstream, through what the node's consumers require of it. Connecting a
  `Dot`'s output into a deep input resolves the `Dot` to deep, exactly as
  connecting a deep source into its input does. A consumer that itself accepts
  anything requires nothing of its own, but what *it* must in turn deliver
  still reaches back through it, which is what types a whole chain of
  pass-throughs feeding one concrete consumer -- as far as the first consumer
  that says the input in question does not feed its output.

Nothing in that depends on the order the edges were made, and nothing is
serialized, so a project resolves the same way after a reload as it did while
being built.

A node the graph constrains to no kind at all resolves to
`eDataKindPolymorphic` -- "unconstrained", not a silent fallback to image. A
node it constrains to two different concrete kinds at once is **ambiguous**,
which is a different thing: a node with a primary input and a polymorphic
auxiliary, or one selecting between an image branch and a deep branch, is a
legitimate graph, so the inputs that make it ambiguous are not rejected. What
is rejected is handing that node's output to an input declaring a concrete
kind, which is the point at which a single definite kind is actually required.
`getEffectiveOutputDataKind()` reports ambiguity through an optional
`bool* isAmbiguous` out-parameter and returns `eDataKindPolymorphic` when it is
set, so a caller that only wants something to draw -- `Gui/Edge.cpp`,
`Gui/NodeGui.cpp` -- reads "no single kind" and has no error case to handle.

The result is cached per node and invalidated on connection change
(`Node::invalidateEffectiveOutputDataKindCache()`, driven from
`Node::onInputChanged()`). Because resolution is bidirectional, a node's kind
can change when a connection anywhere on either side of it changes, so the
invalidation walks both directions, and everything derived from the kinds it
reaches is brought back up to date with it: each node's data-kind diagnostic,
and the `Node::dataKindChanged()` signal `NodeGui` repaints its edges from.
A resolution that had to be truncated because it came back to a node already
being resolved -- which a policy reading a neighbour can cause -- is not cached
at all: that answer depends on which node the query started from, and
memoizing it would make every later read depend on query order.

Enforcement then splits by regime. At **connection time** the connection is
refused outright: the user is making the edge, so there is no reason to create
an invalid one. When an edge that was legal becomes illegal through something
other than making that edge, the edge is **kept** and the node that now holds
an input it cannot handle is put into an error state -- the graph is the user's
and is never silently rewired. That error state is recomputed whenever the
node's inputs change (`Node::refreshDataKindConflictMessage()`), so fixing the
graph clears it.

Every check compares a **resolved** kind on the upstream/source side against
the **declared** kind of the consuming input
(`EffectInstance::getInputDataKind(inputNumber)`, a fixed, per-plugin,
per-input value) -- never resolved against resolved. Do not assume any single
one of these three points is the whole story:

1. **Connection time** -- `Node::canConnectInput()`
   (`Engine/NodeInputs.cpp`) is the single choke point used by every GUI drag,
   undo/redo, auto-connect, and the Python API. It calls
   `Node::checkDataKindCompatibility()`, which resolves the upstream node's
   effective output kind and compares it against this node's declared kind
   for that input, returning `Node::eCanConnectInput_incompatibleDataKind`
   on a mismatch and naming the conflicting (upstream) node. An ambiguous
   upstream node is refused here too. This check is skipped while a project is
   loading (see point 2) because mid-restore the answer depends on how much of
   the tree has been reconnected so far.
2. **Project load** -- `Project::reportDataKindConflicts()`
   (`Engine/Project.cpp`) runs once, after the whole node tree and all its
   connections have been restored, and re-checks every restored edge (it is
   `Node::refreshDataKindConflictMessage()` over every node, the same call an
   input change makes for the nodes it affects). Since
   kinds are never serialized, this is the only place a stale or contradictory
   combination (e.g. a project saved before an upstream node's declared kind
   changed, or one wired up through `Node::connectInput()` by a Python script
   or PyPlug, which bypasses `canConnectInput()`) gets caught. Every edge is
   kept exactly as saved and every node holding an input it cannot handle gets
   an `eMessageTypeError` persistent message naming those inputs -- the same
   answer as a project naming an OpenColorIO colorspace its config does not
   define (`Project::reportUnresolvedOCIOColorSpaces()`). The load itself still
   succeeds: a node showing an error is a loaded project, not a failed one.
3. **Chain simulation for polymorphic nodes** -- connecting a new upstream
   source into a node whose own output is polymorphic (e.g. `Dot`,
   `TypedPassthrough`) does more than re-run point 1 for that one edge.
   `checkDataKindCompatibility()` first computes what the node would resolve to
   with the new connection in place, then walks that forward through the node's
   existing downstream consumers (`Node::findDataKindConflictDownstream()`,
   recursing through any further polymorphic nodes) and compares it against
   each concrete consumer's declared input kind. That forward walk is what
   rejects `deep source -> Dot` when `Dot -> image sink` was already connected
   first: the new connection would make the already-connected sink
   incompatible, so it is rejected at the connection that introduces the
   contradiction, naming the sink as the conflicting node. The walk stops at an
   input a node's policy says it does not follow. The engine still cannot
   predict what an overridden `resolveOutputDataKind()` would answer across the
   edge being simulated, so for that one not-yet-existing edge the simulation
   assumes the structural default.

`Tests/DataKind_Test.cpp`, `Tests/DataKindProjectLoad_Test.cpp`, and
`Tests/TypedPassthrough_Test.cpp` exercise all three points end to end,
including through `TypedPassthrough` itself, and are worth reading alongside
this section.

## Conversions between kinds are explicit

No node accepts an edge of a different kind than it declares, and there is no
adapter-insertion mechanism: a conversion between kinds requires an explicit
node (e.g. a `DeepToImage`), so the graph always shows where an
information-destroying conversion happens. A new node should never rely on, or
attempt to register, an implicit conversion of its own; if a node needs data in
a different kind than what feeds it, it takes that kind as its declared input
and requires an explicit converter node upstream.

The Viewer is the intended single exception, once there is deep data to
convert: flattening deep to image for display is the one conversion that has
nowhere else to go, since it is not part of the graph's result. That is a
design intent, not current behavior -- `ViewerInstance` declares image inputs
like every other node here and no flattening exists to run.

## Known limitation: groups are an image barrier

`NodeGroup` declares no kinds of its own, so it takes `EffectInstance`'s
image/image default: a deep or scene chain inside a group is reported as image
at the group node's external output, and connecting that output to a deep or
scene input is rejected. Resolving a group's effective kind from its internal
`GroupOutput` (and its inputs from the corresponding `GroupInput` nodes) is a
design decision that has not been made yet.

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

## Flat image nodes (`NativeImageEffect`)

The flat 2D core nodes -- the colour, merge, generator, spatial and keying
families that replaced the openfx-misc and openfx-io plugins -- sit on
`Engine/Nodes/Image/NativeImageEffect` (`NativeGenerator` for nodes with no
image input). It is a convenience layer on `NativeEffectBase`, not a second
hierarchy. It is float-only, tile-capable, multi-resolution, supports render
scale, and is not multiplanar: the host's layer knob decides which planes are
rendered and which channels of each are processed, and `render()` is called
once per plane. Spatial parameters scale by `mappedScale`. Per-instance state
is knobs only; kernels and lookup tables are built per render call.

### Point ops: the kernel contract

A point operator returns true from `isPointOp()` and builds a `PixelKernel`
(`Engine/Nodes/Image/PixelKernel.h`) in `makeKernel()`. The base's `render()`
then does the whole per-pixel pipeline in one pass over each row, so the host
neither copies unprocessed channels nor multiplies back
(`rendersUnprocessedChannels()` is true). The order is fixed:

1. divide every channel but the divisor itself by the "(Un)premult by" channel,
   where `Image::unPremultDivisorIsUsable()` says it is usable;
2. run the kernel;
3. multiply the processed channels back (`Image::premultiplyValue()`);
4. blend each processed channel with the *undivided* source by mask x `mix`,
   with the `ofxsMaskMixPix()` arithmetic;
5. pass the unprocessed channels through from the source.

A pixel outside the source reads as zero, outside the divisor as a divisor of
one (neither divided nor multiplied), and outside the mask as a mask of zero.
The divide and multiply share their per-pixel helpers with
`Image::unPremultiplyByChannel()`/`premultiplyByChannel()`, so the two paths
compute identical values. A node that overrides `isIdentityOp()` only answers
for the operator itself; the base adds the mask and mix rules.

A `PixelKernel` is fusion-ready: it is immutable, built from knob values at a
`KernelContext` (time, view, mapped scale, processed channels), and
`processRow()` is pure on its `RowIO` -- no knob access, allocation or I/O. Rows
may run in any order, on any thread, or be chained with other kernels over a
strip. No fusing pass exists yet; keep to those rules so one can be added
without touching the nodes.

### Host knobs and script names

The host owns which layers and channels are processed, so a node declares
none of that. It gets, with these script names, which a node must not rename
or redeclare:

- the layer knob at row 0 (`channels` is the channel-set knob), with defaults
  from the traits (`NativeImageTraits::processesAllLayers`, `defaultChannels`);
- `enableMask_<label>` and `maskChannel_<label>` for each input declared with
  `NativeInputDescription::isMask`;
- `hostUnPremultBy`, when the traits ask for it (`hostUnPremult`);
- `maskInvert` and `mix`, declared by the node through `addMaskMixKnobs()` and
  moved by the host to the end of the page, after the mask selectors.

Beyond those, a node's own knobs follow the OFX plugin's order, names, types,
dimensions and defaults. The OFX-internal `NatronOfxParamProcess*`,
`unPremultBy*` and `premultChanged` are not declared. Labels drop the `OFX`
suffix and grouping is a `/`-separated path matching the OFX menus.

### Scheduler declaration: zero unplanned pulls

Everything `render()` fetches must be declared beforehand, so a task-graph
render shows 0 unplanned pulls and the legacy and task-graph schedulers give
bit-identical output at any pool size. `NativeImageEffect` already declares the
source plane, the divisor layer (through the "(Un)premult by" selector) and the
mask layer (through the mask selector), all at the render window. A node that
reads more -- a wider window, another input, another time -- declares it through
`getRegionsOfInterest()`, `getComponentsNeededAndProduced()` and
`getFramesNeeded()`. `isIdentity()` matches the OFX conditions so identities
become identity tasks. `TransformNode`, and `Reformat` with `preserveBB`,
implement `getCanTransform()`/`getTransform()` and consume a concatenated input
transform. No node here touches deep data or GL.

### Threading

The host does not split a render window across threads for these nodes, and
they declare `eRenderSafetyFullySafe`, as an OFX plugin using the multithread
suite does. Each node parallelises its own render, within the thread budget
`AppManager::getNCPUsAvailableForEffect()` grants it, on
`parallelForCancellable` (`NativeEffectBase.h`), never on
`parallelForOnGlobalPool` directly:

- **Point ops** split their own row bands (`NativeImageEffect::makeRowBands()`:
  at least 16384 pixels per band, at most 4 bands per thread). The kernel must
  therefore be safe to call concurrently.
- **Spatial filters** (`Blur`, `EdgeDetect`, `ErodeDilate`) parallelise over
  whole rows or whole columns, one line per task, never over the host's tile
  split. Their result is then independent of thread count and band layout.
- Other nodes with their own `render()` (`Merge`, `Dissolve`, `TransformNode`,
  `Crop`, `Reformat`, `Position`, `Keyer`, `ChromaKeyer`, the generators) split
  row bands the same way as point ops.

Read an image's bounds with `Image::getBounds()` on the calling thread, before
the bands start, and hand the rectangle to the band. `getBounds()` takes the
image's read lock; a band thread that asks for it queues behind a writer
waiting on an image the render already holds, and never returns.

A band thread carries none of the render's TLS, so `EffectInstance::aborted()`
asked there always answers "not aborted". Build one `RenderCancellation` on the
calling thread, pass it to `parallelForCancellable`, and poll its `check()`
every few rows inside the band. Only the calling thread asks the effect; it
publishes the answer in an atomic flag the band threads read, and while it waits
for the helpers it wakes every 10 ms to ask again. Once the flag is set no new
band starts and running bands return at their next poll, so an abort is honoured
within a few rows instead of after the whole window.

### IDs and versions

A native node registers under the ID of the OFX plugin it replaces, with
`majorVersion` one above the highest OFX major registered for that ID and minor
0, so every unversioned or older request -- Python `app.createNode(id)`, PyPlug
calls, Roto's and Tracker's internal nodes -- resolves to the native node. The
OFX plugin stays loadable by its exact major until its family is retired, which
is how a parity test renders both side by side. The ID and major live in
`PLUGINID_NATRON_<NAME>` and `PLUGIN_MAJOR_NATRON_<NAME>` next to the class; each
node exposes `static EffectInstance* BuildEffect(NodePtr)`, and icons are in
`Gui/Resources/Images/NativeNodes/<pluginID>.png`.

Siblings are one class under several IDs: the Merge presets (`Plus`, `Matte`,
`Multiply`, `In`, `Out`, `Screen`, `Max`, `Min`, `Difference`) are `MergeNode`
with a default `operation`; `Solid` is a `Constant` preset; `TransformMasked`
is `TransformNode` with a mask input; `Add`, `Multiply` and `Gamma` are one
`ColorMath` class; `Erode` and `Dilate` are `ErodeDilate` and a thin subclass.
The compat versions of retired plugins are gone, and nothing migrates a project
saved with the OFX versions.

### Writing a parity test

Parity tests live in `Tests/Native/` and are picked up by a glob, so
`Tests/CMakeLists.txt` stays untouched. The helpers are in `Tests/NativeParity.h`.

1. `makeParityPair(app, id, ofxMajor, nativeMajor, maskInputLabel)` creates the
   node at both exact majors, fed by the deterministic `ParitySourceTestEffect`
   (64x48, with negatives, super-whites and exact-0, exact-1 and ramp alpha
   bands). `pair.ofx` is null once the OFX plugin is retired.
2. `setKnobOnBoth()` applies the same values to both nodes, and
   `connectParityInput()` wires further inputs.
3. `compareParity(pair, caseName, roi, mipmapLevel, tolerance, record)` renders
   both through `renderRoI` and compares every channel of every plane. Run every
   case at mipmap 0 and 1.
4. Mark about three representative cases per node `record`. With
   `NATRON_PARITY_RECORD_DIR` set, the OFX output of those is written as
   `Tests/fixtures/native-parity/<id>/<case>.f32` (`NPAR` header, width, height,
   components, float32 little-endian; `.mip1` suffix for mipmap 1), at windows
   near 64x48 and within the family cap of 1.5 MB. Decide which cases to record
   *before* the record step: an unrecorded case is deleted with the plugin.
5. After retirement the same call replays the references. With neither a live
   OFX plugin nor a reference, the test fails; there is no skip.

A node's tolerance is a named constant at the top of its test, built from one of
the classes below, and a node that cannot meet its class carries a measured bound
with the reason beside it.

### Parity tolerances

`ParityTolerance` accepts a value when
`|native - reference| <= absolute + relative * max(1, |reference|)`. A pair of
NaNs matches, as do equal infinities.

| Class | `ParityTolerance` | Bound | Used for |
|---|---|---|---|
| Exact | `exact()` | 0 | generators, Position, Clamp, Invert, Erode/Dilate, Merge Porter-Duff operators, Crop without softness |
| Arithmetic | `arithmetic()` | 1e-6 relative | Add, Multiply, Dissolve |
| Transcendental | `transcendental()` | 1e-5 relative | `pow`, trig, LUTs, HSL: Grade, Gamma, ColorCorrect, Saturation, Keyer, ChromaKeyer, ColorLookup, Merge blend modes |
| Resampling | `resampling()` | 1e-5 absolute | Transform, Reformat, Crop softness |
| IIR | `iir()` | 1e-4 absolute | Blur, EdgeDetect, Transform motion blur |

Every node of the flat families, with the constant its test uses:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| Grade | transcendental | `kGradeTolerance` | gamma |
| ColorCorrect | transcendental | `kColorCorrectTolerance` | tone curves evaluated lock-free |
| Saturation | transcendental | `kSaturationTolerance` | the ACES AP1 option is exempt: native uses the real AP1 coefficients, where the OFX plugin falls through to CCIR 601 |
| Clamp | exact | `kClampTolerance` | |
| Invert | exact | `kInvertTolerance` | |
| Add | arithmetic | `kAdd` (`NativeColorMath_Test`) | |
| Multiply | arithmetic | `kMultiply` | |
| Gamma | transcendental | `kGamma` | |
| Merge and its nine presets | exact / transcendental | `toleranceFor()` per operator | Porter-Duff operators (ATop, ConjointOver, Copy, DisjointOver, In, Mask, Matte, Out, Over, Stencil, Under, XOR) exact; blend modes transcendental, and mask and mix too. `op-divide` measured 3.05e-5 on large values. The presets share the class and test that each carries its operation |
| Dissolve | arithmetic | `kDissolveTolerance` | |
| Constant | exact | `kConstantTolerance` | |
| Solid | exact | `kConstantTolerance` | a Constant preset, tested in `NativeConstant_Test` |
| CheckerBoard | exact | `kCheckerBoardTolerance` | |
| Blur | IIR | `kBlurTolerance` | box filters share the class |
| Transform | resampling / IIR | `kResamplingTolerance`, `kMotionBlurTolerance` | motion blur averages up to hundreds of samples |
| TransformMasked | resampling | `kResamplingTolerance` | the `mask-mix05` case |
| Crop | exact / resampling | `kExactTolerance`, `kSoftTolerance` | softness 0 copies pixels; a soft edge is a double smoothstep rounded to float |
| Reformat | resampling | `kReformatTolerance` | |
| Position | exact | `kPositionTolerance` | |
| Erode, Dilate | exact | `kTolerance` | rectangular min and max are exact in float |
| EdgeDetect | IIR | `kEdgeDetectTolerance` | see the exceptions below |
| Keyer | transcendental | `kKeyerTolerance` | |
| ChromaKeyer | transcendental / measured | `kChromaKeyerTolerance`, `kChromaKeyerUnpremultipliedTolerance` | the unpremultiplied output uses 1e-4 relative |
| ColorLookup | transcendental | `kColorLookupTolerance` | |

Measured exceptions and what they are not:

- **ChromaKeyer, unpremultiplied output: 1e-4 relative**
  (`ParityTolerance::make(0., 1e-4)`). The OFX plugin is built with `-Ofast`, so
  its key differs from the native one by a few ulps, and the unpremultiplied
  output divides the suppressed colour by a key alpha that can be near zero,
  magnifying them. 5e-5 fails; the other `show` outputs stay at the
  transcendental class.
- **EdgeDetect, RGBA processing.** In this host the OFX EdgeDetect always feeds
  alpha into the rms, max and tensor modes: `Node.cpp` keeps the plugin's own
  RGBA switches on and applies the channel set afterwards. The native node
  combines only the processed channels, as upstream Natron's plugin does. Parity
  pairs therefore set the channel set to process RGBA on both sides; with fewer
  channels the two legitimately differ.
- **`-Ofast`.** The OFX plugins are compiled with `-Ofast`, whose ulp
  differences flip tied pixels under Sobel plus non-maxima suppression, because
  erosion leaves flat plateaus that suppression compares exactly. The recorded
  EdgeDetect case uses the Gaussian gradients, which are bit-exact. Do not read a
  Sobel-plus-suppression mismatch as a native bug without checking for ties.
- **Deliberate fixes** are divergences, excluded from parity by case: Saturation's
  ACES AP1 coefficients above. Keyer and ChromaKeyer with an RGB source treat
  alpha as 1 for Normal and add nothing for "Add to Inside Mask", where OFX
  reads out of bounds, and neither mirrors OFX's output-premult preference.
