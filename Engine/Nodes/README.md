# Native nodes

A native node is an `EffectInstance` subclass built directly into `NatronEngine`
rather than loaded as an OFX plugin bundle. This directory is where those
subclasses live, and this file is the orientation for writing one: the sizing
rule for where a new file goes, the steps to get from an empty `.cpp` to a
registered, testable node, and the rules an author needs to know about data
kinds and the single `EffectInstance` hierarchy. `NativeEffectBase.h`'s
class-level comment carries a full worked example that compiles as written;
read that for the API reference. This file is the map of the directory and
the rules that example doesn't state. The design of the declarative layers
(`NodeSpec`, `ImagePipelineSpec`, the op kinds) and the reasons for its shape
are in `docs/decisions/2026-10-19-native-node-framework.md`.

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
layer *on* `EffectInstance`, not a second hierarchy. `StagedImageEffect` (see
"Flat image nodes") is the same again, a convenience layer *on*
`NativeEffectBase` for flat image nodes: a Point, Passes, Route, Resample or
Composite node is a `StagedImageEffect` with an op plugged in, and a node that
fits none of them subclasses it directly as a Custom node. It adds no
capability virtual of its own and is not a parallel hierarchy.

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
that metadata once, as a single `NodeSpec` (`Engine/Nodes/NodeSpec.h`)
returned from `nodeSpec()`, and implements those `EffectInstance` and
`OutputEffectInstance` virtuals from it. It adds no capability of its own
that is specific to one kind of data -- see "Single hierarchy" above. What it
does add is kind-agnostic: the spec, the `route()` identity hook, `KnobReader`
and `bakeParams()`, and the GPU virtuals (`canRenderOnGpu()`, `renderGpu()`,
`getGpuInputRoI()`, `getGpuInputLayer()`, `getGpuStageSummary()`,
`gpuCostHint()`), which refuse by default. The engine casts to
`NativeEffectBase` to reach them.

What it supplies so a subclass doesn't have to:

- `getPluginID()`, `getPluginLabel()`, `getPluginDescription()`,
  `getPluginGrouping()`, `getMajorVersion()`, `getMinorVersion()`,
  `isWriter()`, `isOutput()`, `getNInputs()`, `getInputLabel()`,
  `isInputOptional()`, `isInputMask()`, `getOutputDataKind()`,
  `getInputDataKind()` -- all derived from `nodeSpec()`. `isWriter()` and
  `isOutput()` both follow the spec's `isWriter` flag, default `false`: a node
  that is meant to be a render root -- a writer -- sets it, and both accessors
  report true; every other native node reports false for both.
- `addAcceptedComponents()` -- defaults to RGB, RGBA and Alpha.
- `addSupportedBitDepth()` -- defaults to byte, short and float.
- `renderThreadSafety()` -- defaults to `eRenderSafetyFullySafeFrame`.
- `isIdentity()` -- answered from `route()`.
- `createKnob<KNOB_TYPE>(label, dimension)` -- wraps the
  `AppManager::createKnob(this, label, dimension)` idiom every
  `initializeKnobs()` otherwise repeats.

What a subclass still must provide, exactly as it would on top of
`OutputEffectInstance` directly: `nodeSpec()` (the only pure virtual
`NativeEffectBase` adds), `initializeKnobs()`, and its render behavior
(`render()`, or `route()` if the node is a pass-through). A writer sets
`NodeSpec::isWriter = true`; nothing else about writing a native node changes.

The plugin-identity accessors -- `getPluginID()`, `getPluginLabel()`,
`getPluginDescription()`, `getPluginGrouping()`, `getMajorVersion()`,
`getMinorVersion()`, `isWriter()`, `isOutput()` -- are `OVERRIDE FINAL` on
purpose: `NodeSpec` is the single place a node's identity is written, and a
subclass changes them by changing the spec, not by overriding the accessor.
The remaining defaults are plain overrides that a node may replace when the
default doesn't fit it: `getNInputs()`, `getInputLabel()`, `isInputOptional()`,
`getOutputDataKind()`, `getInputDataKind()`, `resolveOutputDataKind()`,
`inputParticipatesInDataKindPropagation()`, `addAcceptedComponents()`,
`addSupportedBitDepth()` and `renderThreadSafety()`.

### `NodeSpec`

A `NodeSpec` is an aggregate of `std::string_view`s and spans, so a node
writes it as a `constexpr` with designated initialisers:

```cpp
inline constexpr InputSpec kMyInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

inline constexpr NodeSpec kMySpec = {
    .id = PLUGINID_NATRON_MYNODE,
    .label = "MyNode",
    .description = "What it does.",
    .grouping = PLUGIN_GROUP_COLOR,
    .majorVersion = 1,
    .minorVersion = 0,
    .inputs = kMyInputs,
};
```

Each `InputSpec` has a label, a `DataKindEnum` (default image), a role,
`optional`, and a layer policy. The roles are `Source` (an ordinary input),
`Aux`, `Mask` (the host adds its `enableMask_<label>` and `maskChannel_<label>`
selectors; it must be an image input) and `Variadic`, a run of `count`
numbered inputs labelled `label + (firstNumber + k)`, which must come last.
`InputLayer` says in which layer an input is read: the plane being rendered
(the default), the layer a knob selects (`InputLayer::select(knob)`) or the
layer another input resolves to (`InputLayer::pairWith(i)`). Only a Composite
op's folded inputs follow it; every other op kind reads its source in the plane
being rendered. `time` and
`boundary` are declared but no executor reads them yet, and `rfvReads` holds
names only. `description` and labels are untranslated `string_view`s.

`nodeSpec()` returns `staticNodeSpec<kMySpec>()`, which `static_assert`s that
the spec is valid, so an invalid one fails to compile. A runtime spec
(`MissingPluginNode` builds one from the missing plug-in's record) skips that
check by returning a member `NodeSpec` instead.

`route(time, view)` returns `Route::none()` (the node computes its output),
`Route::input(i, t)` (the node is an identity of input `i` at time `t`) or
`Route::black()` (transparent black with an empty region of definition;
identity with input -1). `isIdentity()` answers from it, so a node that only
forwards an input -- `TypedPassthrough`, `Switch`, the channel nodes at their
no-op settings -- overrides `route()` and nothing else. An image node never
overrides `isIdentity()` itself; see "Flat image nodes".

`KnobReader(time, view)` reads knob values, and a node bakes them once per
evaluation into a plain `Params` struct with `bake(const KnobReader&)`;
`bakeParams(op, time, view)` does so for any type that has it. Render code
works on the baked `Params`, never on the knobs.

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
  input answers with its output clip's metadata, converted by
  `OfxMetadataBridge`. Any other kind of effect gives an empty map.
  `getInputEffectMetadata()` is the one place that lookup is extended.
- A `NoOpBase` input (a Dot, a group Output, the typed test pass-throughs) is
  looked through, along any chain of them, to the node connected to its input
  0: `OfxMetadataBridge::skipPassThroughNodes()` does the walk, for native
  derivation and for an OpenFX input clip alike. A `NoOpBase` is always an
  identity and has no metadata of its own to add. One with nothing connected
  (or no input, like a group Input or a Backdrop) ends the walk and is treated
  as before: empty for a native node, the host-derived keys for an OpenFX clip.
  Nothing extra invalidates: the hash change that reaches a node past a Dot
  already reaches the Dot's downstream.
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
B if no A is connected), never B's; the generators (`GeneratorNode`) output only
`ofx/framerate` and `ofx/pixelaspect` (they have no source, and `ofx/frame` is the
frame number within the source, so it is omitted); transforms and filters
keep the default.

### Reading metadata from Python and expressions

`Effect.metadata(key[, frame, view])` returns one value of the node's output
metadata, `Effect.metadataKeys([frame, view])` the sorted list of its keys.
`frame` defaults to the current frame (the evaluation frame inside an
expression) and `view` to 0. A missing key gives `None`. Types follow the
stored value: int to `int`, double to `float`, string to `str`, a vector to a
`list`; an OpenFX node's one-element vector comes back as a scalar.

Keys carry the prefixes listed under "Metadata" below (`ofx/`, `exr/`,
`exif/`, `tiff/`, `dpx/`, `cin/`, `oiio/`, `quicktime/`). Use
`thisNode.metadata('ofx/frame')` or `app.Read1.metadata('exr/myString')` in an
expression; a frame argument reads another frame, as in
`Read1.metadata('ofx/frame', frame + 1)`.

- An expression that calls `X.metadata()` or `X.metadataKeys()` registers X as
  a dependency of its knob and is re-evaluated whenever X's hash changes, which
  includes an edit anywhere upstream of X. The reading node's own hash follows
  X's.
- When a native node's metadata changes under the same hash, such as a Read
  reloading its file, the readers of its metadata and of the metadata of every
  node downstream of it, native or OpenFX, are refreshed: their nodes' hashes
  move too.
- Renaming X rewrites the references to X in the expression's code and in the
  node paths passed to `getNode()`, leaving metadata keys, other strings and
  comments alone, knob reads in the same expression included; deactivating or
  deleting X marks it invalid, as for an expression reading one of X's knobs.
- A knob that is part of its node's hash cannot read the metadata of a node
  whose hash depends on its own node's, such as a node downstream of it, nor
  can an expression or a link read a knob that leads to such a read: the
  expression or link is refused, and so is a connection that would close such
  a loop. Knobs outside the hash, like the label, can, unless a hashed
  expression reads them.
- An OpenFX node ignores `view`.
- Results are cached per evaluation time, so each frame an expression asks for
  is derived once per hash.

## Metadata

Keys are strings whose prefix names the namespace: `ofx/` (standard keys the
engine and the OpenFX host define), `exr/`, `exif/`, `tiff/`, `dpx/`, `cin/`
(format attributes), `oiio/` (OpenImageIO attributes) and `quicktime/`. Write
routes a key by that prefix, in `OiioWriteSupport::applyMetadata()`:

- `exr/` goes to OpenEXR files only; `exif/` goes to every format; `tiff/`,
  `dpx/` and `cin/` go to TIFF, DPX and Cineon files respectively; `oiio/` goes
  to every format.
- `ofx/timecode` and `ofx/edgecode` are written as `smpte:TimeCode` and
  `smpte:KeyCode`.
- Other `ofx/` keys, `quicktime/` keys, keys that describe the source file's
  layout and a few excluded `oiio/`, `tiff/` and `dpx/` names are never written.
- A key the target format does not take is dropped, with a debug log line.

`Engine/Nodes/Metadata/` holds four nodes, grouped under Metadata: `ModifyMetadata`,
`CopyMetadata`, `RemoveMetadata` and `ViewMetadata`.

All four derive from `NativeEffectBase` and override `isIdentity()` to return
input 0, so the image is passed through without a render and costs nothing.
The three editing nodes override `deriveOutputMetadata()`; `ViewMetadata` keeps
the default pass-through. Key filters are
`MetadataKeyFilter`: comma- or newline-separated globs, `*` matching any
characters including `/`, `?` one, an empty list matching every key.


## I/O nodes

Each I/O node is documented in `Engine/Nodes/IO/docs/<Node>.md`.


## Writing a node, start to finish

`TypedPassthrough` (`Engine/Nodes/TypedPassthrough.h` /
`Engine/Nodes/TypedPassthrough.cpp`) is the framework's own proof node and a
complete, minimal, real example of every step below. It's referenced
throughout; read it alongside this section. A flat image node is shorter
still, because the framework supplies the render, the identity and the stage
knobs from an op: steps 1 to 3 then collapse into the op struct described
under "Flat image nodes", and `Grade` is the example to read.

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
       virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
       {
           return staticNodeSpec<kMySpec>();
       }

       virtual void initializeKnobs() OVERRIDE FINAL;
       // ... plus whichever render-behavior virtual this node needs, see step 3.
   };
   ```

2. **Declare the spec**: define `kMySpec` as a `constexpr NodeSpec`, as shown
   under "`NodeSpec`" above, filling in `id`, `label`, `description`,
   `grouping` (one of the `PLUGIN_GROUP_*` constants in `Global/Macros.h` --
   `PLUGIN_GROUP_DEEP` and `PLUGIN_GROUP_3D` already exist for the
   corresponding domains), `majorVersion`, `minorVersion`, one `InputSpec` per
   input (label, role, whether it's optional, its `DataKindEnum`) and
   `outputKind`. See "Data kinds" below for what to put in the kind fields.
   Set `isWriter = true` if this node is meant to be rendered as a root -- a
   writer -- through the GUI Render menu, the CLI's `-w`, or Python's
   `app.render()`; leave it at its default `false` otherwise.

3. **Knobs and render behavior**: implement `initializeKnobs()` using
   `createKnob<KnobType>(tr("Label"))`. Implement the node's actual behavior:
   override `render()` for a node that produces new pixels, or `route()` for a
   pass-through that just forwards an input unchanged (`TypedPassthrough` does
   the latter -- it has no `render()` override at all, and relies on the same
   identity-render path `Dot` and `NoOpBase` use). A node that produces flat
   images should not override `render()`: it is a `StagedImageEffect`.

4. **Register it**: add a fragment `MyNode.reg.inc` beside the node's header,
   holding the single line

   ```cpp
   NATRON_NATIVE_NODE(MyNode, "MyNode.h")
   ```

   naming the class and its header relative to the fragment. A template
   registered under several IDs gets one fragment per ID, each naming its
   full instantiation and named after the node it registers
   (`Merge/MergePlus.reg.inc` holds
   `NATRON_NATIVE_NODE(MergePreset<eMergePresetPlus>, "Merge.h")`). The
   class is a macro argument, so an instantiation whose arguments contain a
   comma is named through an alias (`Solid` in `Generator/Constant.h`).
   CMake globs the fragments into the generated `NativeRegistryAll.inc`, which
   `AppManager::loadBuiltinNodePlugins()` includes, so neither `AppManager.cpp`
   nor any other shared file changes. Set the node's icon (a resource path,
   e.g. `NATRON_IMAGES_PATH "NativeNodes/MyNode.png"`; leave it out for none)
   and its `deprecated` and `internal` flags in its `NodeSpec`; the ID, label,
   grouping and versions already come from there. Registration instantiates
   the node once (with a null `NodePtr`) purely to read that metadata back off
   it and register a `Plugin*` -- it is not a live node instance.

5. **Test it**: see "Testing" below.

### Adding a native node: checklist

Files a port adds, per node (`<Dir>` is the node's directory under
`Engine/Nodes/`). All are picked up by globs; no shared file is edited.

- [ ] `Engine/Nodes/<Dir>/<Node>.reg.inc`: the registration fragment, see step 4
      above. One per registered ID; a template argument list containing a comma
      goes through an alias. Icon, `deprecated` and `internal` are `NodeSpec`
      fields (`Engine/Nodes/NodeSpec.h`), not part of the fragment. CMake builds
      `NativeRegistryAll.inc` from `Nodes/*.reg.inc`.
- [ ] `Engine/Nodes/<Dir>/<Node>.costseed.inc`: GPU cost model seed rows, built
      from `NATRON_GPU_COST_SEED_CPU_ROW`, `_CPU_MOTION_BLUR_ROW`, `_CPU_SIGMA_ROW`
      and `_GPU_ROW`. CMake aggregates them into `GpuCostSeedsAll.inc`. Regenerate
      with `tools/bench/render-profile-report.py ... --emit-cost-table <source root>`
      rather than writing rows by hand. Delete
      `Engine/GpuCostSeeds/ofx/<plugin-id>.inc` for the OFX plug-in the port
      supersedes, or the plug-in keeps a second seed.
- [ ] `Engine/Nodes/<Dir>/docs/<Node>.md`: the node's doc, with a `Tolerance:` line
      (`Tolerance: none recorded.`, or `Tolerance:` followed by the table of
      class and test constant, as in `Color/docs/Grade.md`).
- [ ] `Tests/Native/Native<Node>_Test.cpp`: the node's test, calling
      `expectNativeIdentity(id, major, label)` from
      `Tests/Native/NativePluginIdentity.h`.
- [ ] `Engine/Gpu/Kernels/<kernel>.cmake`: for each new `.slang` kernel, one
      fragment holding its `slang_add_kernel(...)` lines, named after the
      `.slang` file (see `blur.cmake`).
- [ ] `Gui/Resources/Images/NativeNodes/<icon>.png`: the icon, globbed by
      `Gui/CMakeLists.txt`; the `NodeSpec` icon path names it.
- [ ] `Engine/OfxRetired/<ofx-id>`: one marker per OFX ID the port retires, see
      `Engine/OfxRetired/README.md`. An ID the native node keeps (same plug-in ID)
      gets no marker; the native registration already replaces it.
- [ ] Any test that used a retired ID as a fixture moves to the test-only bundle
      in `Tests/OfxTestPlugins/`.

Two tests catch a mistake here. `NativePluginList_Test` fails when a `.reg.inc`
fragment is not registered (or registers a different class), when a native
plug-in has no fragment, when two fragments share an ID, when a marker names an
ID a native node registers, or names a replacement that is not a registered
native plug-in. `OfxRetired_Test` fails when a marker names an ID no loaded OFX
bundle provides (a typo), or when a retired ID is still registered as an OFX
plug-in. Neither checks the cost seed, doc, icon or kernel fragments.

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

A node declares kinds via `NodeSpec::inputs[i].kind` and
`NodeSpec::outputKind` (or, off `NativeEffectBase`, by
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

## Groups resolve kinds from what is inside them

A `Group` node, a PyPlug instance (which is a group) and a group nested in a group
carry whatever kind their inner graph does, whichever kind that is. `NodeGroup`
declares polymorphic inputs and output, and `Node` resolves them
from the graph instead of from a declaration. Nothing needs to be stated on the
group, and there are no per-`GroupInput`/`GroupOutput` kind knobs.

- **Output.** A group's effective output kind is the effective kind of whatever
  feeds its `GroupOutput`, merged under the same rules as an unfed `Dot`. A deep
  node feeding `GroupOutput` makes the group deep, so a deep-only consumer
  connects to the group's output and renders the same samples as it would
  without the group. The group renders nothing itself: `getInput` redirects
  through it, so the deep render path never sees the group node.
- **Inputs.** A group input has no declared kind. When it is connected outside, it
  carries what is connected there; when it is unconnected, it takes what the
  consumers of its `GroupInput` inside require. A `GroupInput` resolves upstream
  through the group's real input, one graph up.
- **Conflicts.** The downstream walk that collects requirements and finds
  conflicts continues into the matching `GroupInput`'s consumers when it reaches a
  group's input, and on to the group node's outputs when it reaches a
  `GroupOutput`. So a mismatch inside a group (say a deep source into a group
  whose inner consumer takes only image) is refused or reported at the external
  edge that causes it, with a message naming the inner node that cannot take the
  data. An unconnected group input whose inner consumer is deep-only is deep, and
  connecting an image source to it is rejected with
  `eCanConnectInput_incompatibleDataKind`.
- **Invalidation.** Resolved kinds are cached per node, and a change anywhere
  invalidates the connected component in both directions. The walk crosses group
  boundaries too: a change to what feeds `GroupOutput` invalidates the enclosing
  group node and goes on to its outputs, and a change to a group's input reaches
  the `GroupInput` inside. Swapping the node that feeds `GroupOutput` from image
  to deep therefore flips the group's kind and re-reports the conflicts of the
  consumers below it. Nested groups propagate both ways, to any depth.
- **Nested groups** resolve recursively, under the same thread-local cycle guard
  as every other kind walk.
- **PyPlugs.** A PyPlug whose inner chain is deep instantiates with a deep output
  kind and connects to a deep consumer, exactly as the equivalent group does.
- **Kinds are not serialized.** They stay derived. A project reloads with the same
  kinds, and conflicts are checked once the whole graph is restored
  (`Project::reportDataKindConflicts`), so a saved project with a deep chain in a
  group loads with no spurious conflicts and a genuinely conflicting one reports
  on load.

Edge cases:

- A group whose `GroupOutput` is unconnected resolves as an unfed polymorphic node
  does (the usual ambiguity rules): nothing reaches the output, so nothing the
  inputs carry constrains it.
- A group input connected outside but unused inside resolves from the external
  input alone.
- A group input unconnected outside resolves from what its `GroupInput`'s
  consumers inside require.
- Several group inputs of different kinds resolve independently.
- The explicit-conversion rule above still holds: a group does not convert kinds,
  it only reports the kind of what is inside it.

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

## Flat image nodes (`StagedImageEffect`)

The flat 2D core nodes -- the colour, merge, generator, spatial and keying
families that replaced the openfx-misc and openfx-io plugins -- sit on
`Engine/Nodes/Image/StagedImageEffect`. It is a convenience layer on
`NativeEffectBase`, not a second hierarchy: the node's identity is a `NodeSpec`,
and everything the host does around its pixel maths is declared, not
overridden. It is float-only, tile-capable, multi-resolution, supports render
scale, and is not multiplanar: the host's layer knob decides which planes are
rendered and which channels of each are processed, and the render is called
once per plane. Spatial parameters scale by `mappedScale`. Per-instance state
is knobs only; `Params` and lookup tables are built per render call.

A flat image node is three declarations and a few functions:

- a `NodeSpec` (above) for its identity and inputs;
- an `ImagePipelineSpec` with the node's **stages**, its **layer policy** and
  its **op kind**;
- an **op**, a struct that holds its knobs and does the pixel maths.

`ImageNode<OP>` builds the node from them (`OP::kNodeSpec` and `OP::kPipeline`
by default). A node that needs more than the op gives -- extra knobs, region
overrides, `knobChanged()` -- derives from `ImageNode<OP>` and overrides the
virtual it needs, as `Grade` does for Normalize. A generator derives from
`GeneratorNode<OP>`, which adds the extent knobs and the region of definition
and has no input. A node whose maths fits no op kind is a **Custom** node, below.

### A Point op, start to finish

`Grade` (`Engine/Nodes/Color/Grade.h`) is the full example. The smallest op
that compiles against the real signatures is:

```cpp
inline constexpr InputSpec kGainInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

struct GainOp {
    struct Params {
        double gain;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_GAIN,
        .label = "Gain",
        .description = "Multiplies the image by a constant.",
        .grouping = PLUGIN_GROUP_COLOR,
        .inputs = kGainInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = { Stage::Premult, Stage::MaskMix, Stage::PassThroughUnprocessed },
        .layers = LayerPolicy::color(true, true, true, false),
        .op = OpKind::Point,
    };

    void createKnobs(KnobHolder* holder, const KnobPagePtr& page)
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Gain").toStdString());

        k->setName("gain");
        k->setDefaultValue(1.);
        page->addKnob(k);
        gain = k;
    }

    Params bake(const KnobReader& reader) const
    {
        return Params { reader.value(gain) };
    }

    bool isIdentity(const Params& params) const
    {
        return params.gain == 1.;
    }

    void row(const Params& params, const RowIO& io) const
    {
        for (int x = 0; x < io.width; ++x) {
            for (int c = 0; c < io.nComps; ++c) {
                if (io.channels[pixelKernelChannelBit(io.nComps, c)]) {
                    io.dst[x * io.nComps + c] = (float)(io.src[0][x * io.nComps + c] * params.gain);
                }
            }
        }
    }

    GpuOpRef gpu(const Params&) const
    {
        return GpuOpRef::none();
    }

    KnobDoubleWPtr gain;
};

static_assert(PointOp<GainOp>);

class Gain
    : public ImageNode<GainOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node) { return new Gain(node); }
    explicit Gain(NodePtr node) : ImageNode<GainOp>(node) {}
};
```

Registration and testing are steps 4 and 5 above. What the op does and does not
do:

- `bake()` reads each knob once through the `KnobReader` into `Params`, which
  must be copyable (`std::copyable`) so band threads take it by value. It is the
  only place an op touches knobs. `row()` is pure on its `RowIO`: no knob
  access, allocation or I/O, called concurrently for different rows, in any
  order. It writes every channel set in `io.channels`; the stages deal with the
  rest. A `Params` that holds a lot of immutable data (ColorCorrect's and
  ColorLookup's curve tables) holds it through a `std::shared_ptr<const ...>`,
  cached by the curve knobs' hash, rather than copying it per bake.
- `isIdentity()` says whether those `Params` leave the source unchanged,
  whatever the stages do. An op whose maths depends on the render scale takes
  the `PassContext` (the scale and the source's pixel aspect ratio) in all
  three members, `isIdentity(params, context)`, `row(params, context, io)` and
  `gpu(params, context)`; it has one form or the other.
- The op never overrides `render()` or `isIdentity()` of the node: both are
  `FINAL` in `StagedImageEffect`, so a node cannot fork the stage rules.

### Stages

A stage is something the host does around the op, so every node that asks for it
does it the same way, on the CPU and the GPU. `ImagePipelineSpec::stages` lists
them:

- **`Premult`** divides every channel but the divisor itself by the "(Un)premult
  by" channel before the op, where `Image::unPremultDivisorIsUsable()` says it
  is usable, and multiplies the processed channels back after it. The node needs
  a Source input, and a Composite or Resample op cannot have it (no one divisor
  applies to several inputs, and a resampler reads the source as stored); both
  are compile errors.
- **`MaskMix`** blends each processed channel of the op's result with the
  *undivided* source by mask x `mix`, with the `ofxsMaskMixPix()` arithmetic. It
  needs a Mask input.
- **`PassThroughUnprocessed`** writes the channels the op does not process from
  the source. Without it they are left for the host to copy, so a node that
  renders them itself must ask for it to stay bit-exact (Grade, Blur, Position
  do).

The order is fixed: divide, op, multiply back, blend, pass the unprocessed
channels through. A pixel outside the source reads as zero, outside the divisor
as a divisor of one (neither divided nor multiplied), and outside the mask as a
mask of zero. The stage maths is shared by the CPU executor (`runStagesCpu()`,
`Engine/Nodes/Image/StagePlan.h`) and the GPU one (`recordStagesGpu()`), and
the divide and multiply share their per-pixel helpers with
`Image::unPremultiplyByChannel()`/`premultiplyByChannel()`.

Everything the stages decide for one render -- processed channels, the mix, the
mask and its channel, the divisor -- is resolved once on the rendering thread
into a `StagePlan`. A Custom node's `renderOp()` is handed it.

Identity is composed in a fixed order and is never overridden: `route()` other
than `None`; with `MaskMix`, a `mix` of 0; the op's own identity; with
`MaskMix`, a non-inverted mask whose region misses the window. Every stage
identity is the first Source input at the same time (the *blend input*, which a
Composite op can choose).

`LayerPolicy` says which layers and channels the node processes, and so the
layer knob the host gives it: `color(r, g, b, a)` and `all()` get the
input-bound channel set (starting on the colour layer or on "All", with the
given checkbox defaults), `targetSelect()` gets a layer select naming the layer
the node writes (generators), and `none()` gets no knob (Switch).

### Op kinds

`ImagePipelineSpec::op` picks the concept the op satisfies. Each concept starts
with `bake()`/`Params` as for Point.

| Kind | The op provides | The host does |
|---|---|---|
| Point | `isIdentity(params)`, `row(params, io)`, `gpu(params)` | `row()` per band through `runStagesCpu()`; `gpu()` through `recordStagesGpu()` |
| Passes | `isIdentity(params, ctx)`, `passes(params, ctx)` returning a `PassList` | runs the passes over planar scratch buffers; derives the source's region of interest (window + halo) and the region of definition (source + spread) from the list |
| Route | `route(params, ctx)` | the node is an identity everywhere, with region of definition, frames needed and metadata of the routed input; nothing renders. No stages |
| Resample | matrix form or offset form, below | resamples the source; derives the regions; concatenates transforms (matrix form) |
| Composite | `isIdentity(params)`, `inputs(params, ctx)`, `step(params, io)` | folds any number of inputs through `step()` |
| Custom | none: the node implements `isOpIdentity()` and `renderOp()` | `render()` and `isIdentity()` stay the host's, with the stages and the composed identities |

**Passes.** A pass is a line filter along X or Y, a gather over a window, or a
point pass, over scratch slots; `Pass::lineX()`, `lineY()`, `gather()` and
`point()` build them and say their halo and growth. `PassList::gpu` is one
recorder for the whole list, because Blur's GPU kernels fuse their passes. An op
may add `costHint(params, ctx)` for the GPU cost model; without it the list's
sigma gives one. A node narrows the region of definition the list gives by
overriding `clipRegionOfDefinition()` (Blur's "Crop To Format").

**Route.** `route()` returns the `Route` as for any node, here from the op's
`Params` and a `RouteContext` (the time, the view and which inputs are
connected). Any op kind may also have `route()`: the host asks it first and an
answer other than `None` makes the node an identity of that input (Dissolve at
an integer `which`).

**Resample** has two forms, and an op has exactly one.

- *Matrix form*: `isIdentity(params, ctx)`, `settings(params, ctx)` (filter,
  clamp, black outside, invert, blur) and `transform(params, ctx, amount, invert,
  matrix)`, the canonical 3x3 matrix at a blend amount. The host samples through
  it, asks it at every time a motion blur samples, and concatenates through it:
  the node offers its matrix to the host (unless it has `MaskMix`, or
  `canConcatenate()` says no) and always applies the transforms the host
  concatenates into its source. Transform, TransformMasked and Reformat are this
  form. It runs on the GPU only if the op says
  `static constexpr bool kResamplesOnGpu = true`; then the GPU renders it
  whenever its sampling matrices are affine and nothing is concatenated into the
  source. The Resample kind runs its own loop (`runResampleCpu()`), not a row
  function under `runStagesCpu()`, since it fetches through the concatenated
  transform and samples the whole source.
- *Offset form*: `offset(params, ctx)` returns the whole-pixel translation at
  the render scale (output pixel (i, j) shows source pixel (i - x, j - y)). It
  offers no matrix and takes none, and is CPU only. It may add
  `rowMatte(params, ctx)`, a `ResampleRowMatte` that weights each shifted row
  (an empty one weights nothing), and
  `static constexpr bool kClipsSourceToRoI = true`, to read the source as zero
  outside the region of interest it was fetched for, as an OpenFX host shows it.
  Position is the plain form. Crop is the full one: its offset is the
  translation `reformat` needs, its row matte is the black border and the
  softness fade, and it clips the source to its region of interest.

**Composite** is a fold, not a fixed source list. `inputs()` returns a
`CompositeInputs`: a base input, any number of further inputs and a blend
input (the one the stages blend against and every composed identity answers;
the base input by default). Every input is read at the evaluation's time. The result starts
as the base input's row and `step()` combines it with each further input's row
in turn: `io.src[0]` is the result so far, `io.src[1]` the next input's row, and
`step()` writes the processed channels to `io.dst`. Mask and mix apply once, to
the final result. An input is read in the layer its `InputSpec::layer` names and
reads zero wherever it has no pixel. An op reading several inputs at once is not
supported. Dissolve folds the two of its 64 sources that `which` lies between, with `mix` from the `MaskMix` stage.

**Custom** is the way out, and the cost of it is that the node owns its pixels.
A Custom node derives from `StagedImageEffect` directly, returns
`staticNodeSpec<>()` from `nodeSpec()` and `staticImagePipelineSpec<>()`
from `imagePipelineSpec()` (with `.op = OpKind::Custom`), and implements
`isOpIdentity(time, scale, roi, view)` and `renderOp(args, plan)`. It still gets
the layer knob, the stage knobs and the composed identities, and reads the mask,
mix and divisor from the `StagePlan` instead of its own knobs. Merge, Keyer and
ChromaKeyer are Custom nodes; Merge until M101 - Clean-Sheet Merge Node, the
keyers until M95 - Knob Cleanup And Keyer Redesign. A Custom node is on the CPU
unless it opts into the GPU by overriding three virtuals:
`canRenderOpOnGpu(query)` (the answer may depend only on the query and the
node's settings), `renderOpGpu(args)` to record its dispatches over
`args.outputRect`, and `getOpGpuInputLayer()` to say the layer it reads each
input in. The keyers do, with their own kernels.

### The GPU side of an op

A Point op names its GPU work with a `GpuOpRef` returned from `gpu()`:
`GpuOpRef::compiled(kernelId, params)` for a registry kernel and a trivially
copyable parameter block, `GpuOpRef::packed(kernelId, words)` for words the op
lays out itself, `GpuOpRef::runtime(shader, inputs)` for a runtime shader, or
`GpuOpRef::none()` to stay on the CPU. A block over the recorder's 64 KB, or a
component count outside `acceptedComponents`, stays on the CPU too.

The kernel is an `IPixelOp` in the style of `Engine/Gpu/Kernels/pixelwrap.slang`:
`load()` reads the block, `apply(float4, int2)` does the maths on a pixel in
channel-bit order, and an entry calls `runWrapped<Op>`. The wrapper does the
divide, the multiply back, the mask x mix blend and the pass-through of the
stages (`GpuWrap`, `Engine/Gpu/GpuWrap.h`), so the kernel holds the op's maths
and nothing else; there are no standalone kernels. Register the entry with
`slang_add_kernel(name Kernels/x.slang ENTRY name REGISTRY NatronGpu)` in
`Engine/Gpu/CMakeLists.txt`. A Composite op's kernel is an `ICompositeStep` run
through `runCompositeStep<Op>` (`Kernels/compositestep.slang`): it is dispatched
once per further input, folding it into the result so far between two scratch
buffers, and a finishing pass then blends and passes through. A Passes op names
its GPU work in `PassList::gpu`; the placement test is `canRecordPassesGpu()`.

Keep the C++ maths and its Slang twin in step: the same operand types and
evaluation order, the twin in a Slang module beside the kernel (`gradeops.slang`
holds Grade's), and the parameter block mirrored by a struct with
`static_assert(sizeof(...) == ...)`, as `Grade.cpp` does. That is what keeps the
GPU within its parity bound without a second set of maths to review.

### Host knobs and script names

The host owns which layers and channels are processed, so a node declares
none of that. It gets, with these script names, which a node must not rename
or redeclare:

- the layer knob at row 0 (`channels` is the channel-set knob), with defaults
  from the `LayerPolicy`;
- `enableMask_<label>` and `maskChannel_<label>` for each input with the `Mask`
  role;
- `hostUnPremultBy`, with the `Premult` stage;
- `maskInvert` and `mix`, created by the `MaskMix` stage (`addStageKnobs()`,
  which `ImageNode` calls after the op's `createKnobs()`).

The stage knobs sit together below the node's own knobs, under one separator, in
this order: (Un)premult by, the mask selectors and Invert mask, Mix. A node's own
knobs follow the OFX plugin's order, names, types, dimensions and defaults. The
OFX-internal `NatronOfxParamProcess*`, `unPremultBy*` and `premultChanged` are
not declared. Labels drop the `OFX` suffix and grouping is a `/`-separated path
matching the OFX menus. The filters (Blur, EdgeDetect, ErodeDilate) have no
`Premult` stage and filter the data as stored.

### Scheduler declaration: zero unplanned pulls

Everything a render fetches must be declared beforehand, so a task-graph
render shows 0 unplanned pulls and the legacy and task-graph schedulers give
bit-identical output at any pool size. `StagedImageEffect` already declares the
source plane, the divisor layer (through the "(Un)premult by" selector) and the
mask layer (through the mask selector), all at the render window, and a Passes
or Resample op widens the source's region of interest by its halo or filter
reach. A node that reads more -- another input, another time, a wider window --
declares it through `getRegionsOfInterest()`, `getComponentsNeededAndProduced()`
and `getFramesNeeded()` (Dissolve and Merge do). `isIdentity()` is composed from
`route()` and the op, matching the OpenFX conditions so identities become
identity tasks. No node here touches deep data or GL.

### Threading

The host does not split a render window across threads for these nodes, and
they declare `eRenderSafetyFullySafe`, as an OFX plugin using the multithread
suite does. Each node parallelises its own render, within the thread budget
`AppManager::getNCPUsAvailableForEffect()` grants it, on
`parallelForCancellable` (`NativeEffectBase.h`), never on
`parallelForOnGlobalPool` directly:

- **Point and Composite ops** split their own row bands
  (`makeStageRowBands()`: at least 16384 pixels per band, at most 4 bands per
  thread). `row()` and `step()` must therefore be safe to call concurrently.
- **Spatial filters** (`Blur`, `EdgeDetect`, `ErodeDilate`) parallelise over
  whole rows or whole columns, one line per task, never over the host's tile
  split. Their result is then independent of thread count and band layout.
  Each filters the render window plus its halo, clipped to its region of
  definition, whatever the mask; the mask and mix only blend the filtered
  window with the source. A pixel's value then never depends on the mask
  elsewhere, and through two render windows it is identical for the FIR
  Gaussian, the finite-difference EdgeDetect filters and Erode/Dilate. The IIR
  filters (quasi-Gaussian, Gaussian) start their recursion at the buffer's
  edge, so their value at a pixel moves with the window: the halo,
  max(3, ceil(1.5 size)), is about 3.6 sigma, and on the parity source at sizes
  3 to 20 a pixel moved by up to 1.8e-2 (quasi-Gaussian) and 1e-2 (Gaussian)
  between the full frame and a 1x1 window, about 1% of the input's range;
  `NativeSpatialFilterWindow_Test.cpp` holds them to 2e-2. The box family is a
  running sum in double whose rounding to float measured identical.
- Resample ops and the Custom nodes with their own `renderOp()` (`Merge`,
  `Keyer`, `ChromaKeyer`) and the generators split row bands the same way as
  point ops.

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

Siblings are one op under several IDs, each with its own `NodeSpec`: the Merge
presets (`Plus`, `Matte`, `Multiply`, `In`, `Out`, `Screen`, `Max`, `Min`,
`Difference`) are `MergeNode` with a default `operation`; `Solid` is a
`ConstantNode` over `ConstantOp` with its own spec; `TransformMasked` is
`TransformImageNode<true>`, the `TransformOp` node with a mask input; `Add`,
`Multiply` and `Gamma` are `ColorMathNode<KIND>`; `Erode` and `Dilate` are
`ErodeDilateNode<MODE>`. The compat versions of retired plugins are gone, and
nothing migrates a project saved with the OFX versions.

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

Each node's tolerance row (class, the constant its test uses, notes), measured
exceptions and GPU parity notes live in that node's doc,
`Engine/Nodes/<Family>/docs/<Node>.md`.

Measured exceptions and what they are not:

- **Deliberate fixes** are divergences, excluded from parity by case; each is recorded in the node's doc.

### GPU parity

A native node's GPU path is held to the node's own CPU render, not to the OFX
plugin, and states its tolerance in this section: a node row above gets a GPU
bound beside its CPU class, or a measured exception below it, in the same change
that lets the node answer `canRenderOnGpu()`. Vulkan division is 2.5 ulp and
`pow` and the trigonometric functions are looser still, so a GPU bound is
usually wider than the CPU class of the same node.

`Tests/Native/NativeGpuParity.{h,cpp}` is the shared harness. A test names its
bound with `ParityTolerance` and compares through `compareImages()`.

- `createRuntime()` makes a runtime on whatever Vulkan device exists, software
  ones included. With none, the test prints a skip notice and returns, or fails
  under `NATRON_TEST_REQUIRE_GPU=1`.
- `runRenderGpu()` uploads the inputs and an optional mask into views with a
  chosen offset, row padding and bounds (the source may be smaller than the
  window), records `renderGpu()`, and returns the window. It also counts the
  floats written outside it.
- `renderEndToEnd()` renders a small graph once as a scheduler frame with a
  forced GPU context and once under `GpuPlacementOffGuard`. It returns both
  images and the frame's regions, so a test can assert with `ranInRegion()` that
  the node ran on the device.
