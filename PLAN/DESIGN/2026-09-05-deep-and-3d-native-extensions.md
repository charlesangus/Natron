# Design: native-node extensions for Deep compositing and a USD/Hydra 3D system

2026-09-05. Draft for discussion. Companion research: memory notes
`natron-extension-directions` / `niik-l-fork-3d-deep-cycles`; upstream issue
NatronGitHub/Natron#1006.

## Goals

1. A formalized **native (non-OFX) node framework** that both new subsystems build on.
2. **Deep compositing** as a first-class data type — integrated with caching, RoI,
   and the render scheduler, not a side-channel.
3. A **3D system** with USD interchange, Hydra viewport/rendering (hdStorm as the
   fast unshaded GPU renderer), and a node vocabulary in the spirit of Nuke's new
   3D system.

Non-goals (v1): exposing deep or 3D to OFX plugins; GPU deep processing; a
software scanline renderer; USD Python bindings inside Natron's Python.

## The foundational move: typed graph edges and typed payloads

Natron's graph today moves exactly one thing: flat raster images.
`EffectInstance::renderRoI()` returns `std::map<ImagePlaneDesc, ImagePtr>`
(`Engine/EffectInstance.h:575`), the cache stores fixed-stride tiles keyed by
`ImageKey`, and every connection implicitly means "image".

Both deep and 3D need something else to flow along edges. Rather than two ad-hoc
mechanisms, introduce **one** generalization and make images a special case of it:

- **`DataKindEnum { eDataKindImage, eDataKindDeep, eDataKindScene }`** — declared
  per input and per output by each `EffectInstance`
  (`getOutputDataKind()`, `getInputDataKind(int)`; default: image everywhere, so
  every existing node and every OFX plugin is untouched).
- **Connection typing** — `Node::canConnectInput()` rejects kind mismatches;
  the GUI draws deep edges and scene edges distinctly (Nuke's deep-edge
  affordance). Registered **adapters** allow implicit conversion where it is
  semantically safe and cheap to reason about: deep→image inserts an implicit
  flatten at the Viewer only (never silently mid-graph); scene never implicitly
  converts.
- **Payload transport** — a payload is an immutable, hash-addressed,
  size-accountable value: cacheable, shareable across threads, copy-on-write
  where it has parts. `Image` already behaves this way; `DeepImage` and
  `ScenePayload` (below) are designed to.

The pull-evaluation *actions* generalize per kind rather than being reinvented:

| Action | Image (today) | Deep | Scene |
|---|---|---|---|
| RoD | 2D rect | 2D rect (unchanged) | world bbox (informational) |
| RoI propagation | rects upstream | identical (deep ops are spatially local) | whole-scene pull (RoI is meaningless) |
| FramesNeeded | frames per input | identical | evaluation time + shutter samples |
| Hash / identity | node hash | node hash | node hash + upstream layer-stack hash |
| Cache | `Cache<Image>` | `Cache<DeepImage>` (own budget) | memoized layers + stage cache |

This is the "foundation to build on": one typed-payload substrate, three payload
kinds at launch, and a place to put a fourth (e.g. point clouds, geometry-level
2D splines) without another special case.

### Kind identification and enforcement

Kinds are **plugin-static declarations**, not per-instance or per-edge state:
`getInputDataKind(int)` / `getOutputDataKind()` on `EffectInstance`, defaulting
to image (all OFX plugins and existing built-ins are correctly typed with zero
changes). Edges carry no stored type — an edge's kind is its source's output
kind — so nothing is serialized and nothing can go stale.

Pass-through utility nodes (Dot, Switch, NoOps, Group boundaries) declare
`eDataKindPolymorphic`; their effective kind is resolved structurally from
whatever feeds them (unconstrained when disconnected), cached and invalidated
on connection change. This replaces per-node special cases (cf. the Niik-l
fork's "deep resolves through Dots" hack) with one rule.

Enforcement has three layers:

1. **Connection time** — `Node::canConnectInput()` (`Engine/Node.h:539`) is
   already the single choke point used by every GUI drag path, undo/redo,
   auto-connect, and the Python API. Add
   `eCanConnectInput_incompatibleDataKind`, checked against *resolved* kinds.
   Connecting into a polymorphic chain validates the whole resolved chain, so a
   contradiction (deep source → Dot → image consumer) is rejected at the
   connection that introduces it, naming the conflicting node.
2. **Project load** — connection restore revalidates; an invalid edge is
   dropped with a warning (same policy as a missing plugin), never silently
   miswired.
3. **Render time** — the typed render walk asserts kind agreement. Under 1–2
   this cannot fire from user action; it exists to catch node-author bugs
   loudly.

Adapters are not enforcement escape hatches: the only implicit conversion is
deep→image at the Viewer (which accepts both kinds and flattens). Mid-graph,
information-destroying conversions require an explicit node (`DeepToImage`) so
the graph always shows where they happen.

### Communicating kind in the node graph

Meaning is carried by shape and line style (colorblind- and zoom-safe); color
only reinforces:

- **Node silhouette** per output kind (image = current rect, deep and scene get
  distinct silhouettes) via the already-virtual `NodeGui::paint()` — carved
  within the existing bounding rect, since name frame / icon / preview /
  resize-handle layout assumes it. Hybrid nodes take their output kind's shape.
- **Edge styling by resolved kind** (`Gui/Edge.cpp` already does custom paint +
  dashes): deep and scene edges render distinctly. This is what makes
  polymorphic nodes legible — a Dot stays neutral, its edges show what flows
  through it.
- **Input-arrow glyphs by declared kind** on Natron's dangling unconnected-input
  arrows: communicates what a node *accepts* before connection, and documents
  hybrid nodes (e.g. DeepRecolor) per-port.
- **Color as reinforcement only**: per-group defaults (`_defaultDeepGroupColor`
  already exists in `Engine/Settings.cpp`) — users own body color for
  organization, so it is never the sole signal. `NodeGuiIndicator` badges stay
  reserved for transient states, not kind.

### Native node framework formalization

The mechanism already exists — `EffectInstance` subclass + static
`BuildEffect(NodePtr)` + one line in `AppManager::loadBuiltinNodePlugins()`
(`Engine/AppManager.cpp:1479-1541`). What v1 adds is organization and contract,
not machinery:

- New source layout: `Engine/Nodes/<Domain>/` (`Deep/`, `Scene/`, …), keeping
  `Engine/` core free of node implementations. Existing built-ins stay put until
  a later cleanup.
- A small `NativeEffectBase : EffectInstance` convenience layer: declarative
  plugin metadata (id, grouping, version), typed-IO declaration, knob-building
  helpers — so a new node is one .cpp file, not a tour of EffectInstance's 2000
  lines.
- One deliberately trivial proof node ships with the framework milestone (e.g. a
  native `DeepConstant` or a no-op typed passthrough) so the framework lands
  tested before any feature pressure arrives.

We stay with a **single EffectInstance hierarchy** (capability virtuals
`renderDeep()` / `evaluateScene()` alongside `render()`), not parallel Op
hierarchies à la Nuke's Iop/DeepOp/GeoOp. Natron's knob, hashing, undo,
serialization, and scheduling machinery all hang off EffectInstance; parallel
hierarchies would duplicate all of it for no isolation benefit.

## Part 1 — Deep compositing

### Data model

`DeepImage` mirrors OpenEXR's deep layout so file I/O and memory agree:

```
DeepImage
├─ RectI bounds, RenderScale, ViewIdx
├─ SampleTable (shared_ptr, copy-on-write)
│    counts[pixel]   : uint32
│    offsets[pixel]  : uint64 (prefix sum)
└─ channels : map<string, DeepChannelBuffer>   // "R","G","B","A","Z","ZBack", AOVs…
     each: contiguous float array indexed by offsets
```

Structure-of-channels (per-channel buffers sharing one sample table), not an
interleaved sample array, because:

- color-only ops (DeepGrade, DeepColorCorrect) copy only the channels they touch
  and **share** Z/ZBack/A buffers with their input via COW — the dominant memory
  win in real deep comps;
- it maps 1:1 onto OpenEXR `DeepSlice` and OIIO `DeepData`, and onto Natron's
  existing plane concept (deep AOVs come for free);
- per-channel contiguous floats vectorize.

Invariant: samples within a pixel are **tidy on demand** — nodes record whether
their output is sorted/non-overlapping; `DeepMerge` and flatten tidy lazily.
Semantics follow the OpenEXR "Interpreting Deep Pixels" doc verbatim (point vs
volumetric samples, split/merge/sort, Hillman's volumetric merge math).

### Render path and caching — the part Niik-l skipped

Deep renders go through a real pull pipeline, not a `getDeepImage()` side-channel:

- `EffectInstance::renderDeepRoI(args, DeepImagePtr*)` — same shape as
  `renderRoI`: walks up typed inputs, honors RoI/FramesNeeded, respects abort
  flags and the render scheduler, and consults the cache first.
- **Cache**: a dedicated `Cache<DeepImage>` instantiation (`Engine/Cache.h` is
  already templated) with `DeepImageKey = (nodeHash, time, view, scale)`.
  Entries are variable-size; cost accounting uses actual bytes
  (`getSizeInBytes()` = table + owned channel buffers only, so COW sharing is
  not double-counted). Deep gets its **own memory budget knob** — deep frames
  are large and must not evict the entire 2D image cache.
- **Bounds growth, not tiling, in v1.** The image path caches at requested-RoI
  granularity and grows bounds on demand; deep does the same. True tile-level
  deep caching is deferred: variable sample density makes tile stitching
  error-prone, and deep node RoIs are almost always full-frame in practice.
  The key/param design leaves room for it (bounds live in params, not the key).
- **Threading**: within a node's `renderDeep()`, evaluation is two-pass —
  pass 1 computes per-pixel sample counts (parallel over scanline chunks),
  allocate once, pass 2 fills samples (parallel again). Helpers in the framework
  make this the path of least resistance so node authors don't invent unsafe
  single-pass appends.
- **Viewer**: the Viewer accepts a deep edge and auto-flattens (registered
  deep→image adapter, itself cached as a normal Image, so scrubbing a deep
  stream costs one flatten per frame, once). A pixel-probe shows per-sample
  values (DeepSample-style) — cheap and high-value for debugging.

### v1 node set (Fusion-20-scale MVP, then DeepC-style growth)

Tier 1: `DeepRead`, `DeepWrite` (OIIO DeepData; EXR deep scanline/tiled),
`DeepMerge` (combine + holdout), `DeepToImage` (flatten), `DeepFromImage`
(+Z input), `DeepRecolor`, `DeepCrop`/`DeepReformat`, `DeepExpression`.
Tier 2 (ports of DeepC concepts onto the native API): DeepCGrade-alike,
position/normal mattes, DeepSlice, DeepHoldout variants, DeepDefocus.
Tier 2 is where owning DeepC pays off — each node is small once the transport
exists, and the author holds the copyright for relicensing whatever GPLv3-only
pieces need it.

## Part 2 — 3D system: USD as substrate vs native scene graph

### The two models, honestly

**A. USD as the substrate (Nuke 14, Houdini Solaris).** The scene *is* USD: each
3D node authors edits; consumers compose a stage and hand it to Hydra.

**B. Native scene graph + translation layers (Blender, classic Houdini OBJ,
Gaffer/Cortex).** A purpose-built lightweight scene structure flows through the
graph; USD is an importer/exporter; the viewport is either hand-written or a
Hydra scene delegate/scene index written over the native graph.

What model B buys: exact fit to the engine's value semantics (small immutable
structs, trivially hashable/cacheable — this is why the Niik-l fork went this
way and shipped fast); no 100–200 MB dependency; no USD threading/composition
model to manage; total freedom in schema design.

What model B costs — and why those costs are the wrong ones for this project:

- **You become a schema author forever.** Meshes, then subdivs, curves, points,
  volumes, instancing, primvars, skeletons, lights (by type), cameras
  (every physical attribute), materials… Every one is design + code + I/O +
  viewport support. This is precisely the treadmill Gaffer has run for a decade
  with a full-time team, and the corner the Niik-l fork is already visibly
  painting itself into (bump-only displacement, 4-layer material cap, per-format
  reader quirks).
- **Interchange is a second system.** Every native schema needs a USD (and
  Alembic) translation both ways, forever chasing upstream.
- **The viewport is a third system.** Either hand-rolled GL (Niik-l: ImGui +
  GLSL rasterizer — fine until it isn't) or a Hydra scene index over the native
  graph (Blender's route — real, ongoing engineering that only pays off if the
  native model earns its keep elsewhere).
- Blender and Houdini carry native models because they *predate* USD and their
  cores are destructive/simulation editors over rich in-memory data. Natron has
  **no existing 3D data model and no sim ambitions** — the usual justification
  for model B is absent. A compositor's 3D graph is non-destructive scene
  *description with overrides*, which is literally what USD composition is.

What model A buys: schemas, interchange (ReadUSD is `sublayer this file`; USD's
Alembic file-format plugin reads `.abc` as a layer — Alembic support nearly
free), the viewport (UsdImagingGL/Storm), materials (UsdPreviewSurface,
MaterialX later), and a renderer *ecosystem* — any Hydra render delegate
(Storm today; Karma/Cycles/Arnold delegates as user-side additions) instead of
one hand-integrated renderer. It converts the three biggest cost centers of a
3D system from code-we-write into dependencies-we-pin.

What model A costs, and the mitigations that make the design principled rather
than naive:

1. **Stages are not values.** `UsdStage` is heavyweight, mutable, and expensive
   to compose. → *The stage never flows through the graph.* See ScenePayload
   below: what flows is a layer stack — cheap handles, value-ish, hashable.
2. **Gaffer's shelved Hydra port** (memory, pull-vs-push mismatch) is the
   standard caution. But Gaffer tried to put Hydra *under a non-USD scene
   graph* — exactly model B's translation layer. The lesson is not "avoid
   Hydra"; it is "don't make Hydra render something that isn't USD." Nuke 14,
   Solaris, and usdview all demonstrate model A working at production scale.
3. **Dependency weight** (~100–200 MB, TBB). Acceptable for a DCC in 2026;
   Nuke and Fusion both ship it. Use USD ≥ 24.11 (boost-free, oneTBB),
   monolithic build (`libusd_ms`), Python support **off** in v1 (avoids the
   Shiboken6/PySide6-vs-pxr-bindings coexistence problem entirely), MaterialX
   off initially. Pin the exact USD version against the CY2027 target the fork
   already tracks (see `docs/decisions/2026-08-29-target-vfx-cy2027.md`; same
   pin-exact-tag discipline as the ASWF image decision).
4. **Storm needs a GL 4.5 core context**; `ViewerGL` is GL-2-era. → The 3D
   viewport is a *new* `QOpenGLWidget` with its own core-profile context; it
   never shares the 2D viewer's context. `GPUContextPool` precedent applies.

**Decision: model A — USD is the substrate.** With one qualification that keeps
the door open: node implementations talk to the scene through a thin
`ScenePayload`/`SceneOps` interface (author-layer, compose, query), not raw pxr
types sprayed through the codebase. That interface is small enough to keep USD
out of `Engine/` core headers (pimpl; USD types only in `Engine/Nodes/Scene/`
internals), keeps compile times sane, and means a hypothetical future
alternative backend is "hard but not a rewrite" instead of "impossible." It is
*not* a full abstraction layer over USD semantics — that way lies model B's
translation treadmill with extra steps.

### ScenePayload: what actually flows along a scene edge

```
ScenePayload (immutable value)
├─ layerStack : vector<SdfLayerRefPtr>   // ordered, strongest last
├─ stackHash  : U64                      // combined content hashes
└─ metadata   : up-axis, units, default prim, frame-rate mapping
```

- **Each 3D node owns one anonymous `SdfLayer`** holding exactly its edits
  (a transform override, a reference to a file, a light prim, a pruning
  deactivation…). Output payload = input payload + own layer appended. Layers
  are memoized per node keyed by (knob hash, upstream stack hash): unchanged
  nodes re-emit the same layer handle, so a knob tweak at the end of a 30-node
  scene chain re-authors one layer, not thirty.
- **Composition happens only at consumers** (Viewport3D, HydraRender, WriteUSD,
  bbox queries), through a bounded `StageCache` keyed by stackHash. Consumers
  get a composed read-only `UsdStage`; Hydra population uses the scene-index
  path (Hydra 2.0) rather than the legacy scene delegate.
- **Time policy**: nodes author *animated* knobs as USD time samples across the
  project range into their layer (re-authored on knob/range change); static
  knobs author defaults. Rationale: evaluation-time-only authoring would
  invalidate layers every frame and defeat memoization; range-baking makes
  scrubbing and motion blur (shutter-window sampling) native USD reads, and a
  layer re-bake is milliseconds for parameter-level animation. Heavy animated
  *geometry* comes from files (which are already time-sampled) — we never bake
  vertex data.
- **Threading**: layer authoring is serialized per node (already true under
  Natron's per-node render locks); composed stages are treated as read-only
  after composition; Hydra sync runs on the consumer's thread. USD's TBB
  threading stays internal to USD — no coupling to Natron's QThreadPool.

### v1 node vocabulary (Nuke-14-inspired, deliberately small)

- **I/O**: `ReadScene` (USD sublayer/reference; `.abc` via USD's Alembic plugin;
  `.obj` via the usdObj-style path or a tiny converter), `WriteScene`
  (flattened or layered export).
- **Create**: `Camera3D`, `Light3D` (UsdLux types), `Prim3D` (empty xform /
  simple prims for layout).
- **Edit**: `Transform3D` (xform override on a prim pattern, with viewport
  gizmo), `Merge3D` (layer-stack union — Nuke GeoMerge "merge layers" mode
  first; duplicate/flatten modes later), `Prune3D` (deactivate/visibility by
  pattern), `Material3D` (UsdPreviewSurface authoring + binding).
- **Consume**: `Viewport3D` panel (Storm via UsdImagingGLEngine: unshaded/
  shaded/wireframe, picking→prim selection, camera look-through),
  `HydraRender` node (scene + camera → Image planes: color + depth + prim-ID
  AOVs, delegate selectable — Storm ships; others discoverable via plugInfo),
  making the "fast unshaded GPU renderer" requirement literally Storm's day job.

### Deep ∕ 3D convergence points (v2+, but shape v1 APIs)

- `HydraRender` depth AOV + `DeepFromImage` → cheap deep from 3D renders.
- `DeepToPoints` → points prim in a ScenePayload → inspect deep in Viewport3D.
- A deep-output render delegate (Karma-style A/Z/ZBack) is explicitly out of
  scope but nothing in the payload design precludes it.

## Phasing

1. **M-Typed**: typed edges, payload framework, `Engine/Nodes/` layout,
   `NativeEffectBase`, proof node, docs. No user-facing features. Everything
   after this is parallelizable.
2. **M-Deep-1**: DeepImage + cache + renderDeepRoI + Tier-1 nodes + viewer
   flatten + probe.
3. **M-Scene-1**: USD dependency (pinned, monolithic, no Python), ScenePayload,
   ReadScene, Viewport3D/Storm. *Proves the dependency and the viewport before
   any node vocabulary is built.*
4. **M-Scene-2**: create/edit nodes, Merge3D, HydraRender→image.
5. **M-Deep-2 / M-Bridge**: DeepC-style tier-2 nodes; DeepFromImage+depth-AOV
   and DeepToPoints bridges.

## Risks

- **USD version churn / CY2027 pin** — mitigated by exact-pin discipline and the
  `SceneOps` seam. Revisit pin at each VFX platform year.
- **Deep memory pressure** — separate cache budget; COW channel sharing; watch
  real-world comps before adding tiling complexity.
- **GL 4.5 availability** (old drivers, VMs) — Viewport3D degrades to a clear
  error panel, never blocks 2D compositing; Storm is isolated from the 2D
  render path by construction.
- **Typed-edge serialization** — project files gain payload-kind info on
  connections; loading old projects is trivially compatible (everything is
  image-kind), but new projects with deep/scene edges won't load in upstream
  Natron. Acceptable for a diverging fork; document it.
- **Scope gravity** — the Niik-l fork shows how fast "one more node" compounds.
  The v1 vocabularies above are caps, not floors-to-exceed; growth happens in
  named milestones with this doc amended first.
