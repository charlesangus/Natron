# Milestone 17: Typed graph edges and native node framework

Foundation milestone ("M-Typed" in the design doc). One typed-payload substrate
with three payload kinds (image, deep, scene), connection-time enforcement, a
formalized native-node layer, and the node-graph affordances that make kinds
legible. **No user-facing compositing features** — deep and scene kinds exist as
declarations only; their payloads/render paths arrive in M18/M19. Everything
after this milestone is parallelizable.

Design authority: `PLAN/DESIGN/2026-09-05-deep-and-3d-native-extensions.md`
(sections "The foundational move", "Kind identification and enforcement",
"Communicating kind in the node graph", "Native node framework formalization").
Task briefs below summarize; the design doc governs on any ambiguity.

## Phase 17.1: Data-kind declaration and resolution

- [x] M17.P1.T1 — Add `DataKindEnum` and per-node kind declaration to EffectInstance
  - files: `Global/Enums.h`, `Engine/EffectInstance.h`, `Engine/EffectInstance.cpp`
  - approach: `DataKindEnum { eDataKindImage, eDataKindDeep, eDataKindScene, eDataKindPolymorphic }`. Virtuals `getOutputDataKind()` and `getInputDataKind(int)` on `EffectInstance`, both defaulting to `eDataKindImage` so every OFX plugin and existing built-in is correctly typed with zero changes. Kinds are plugin-static declarations — no per-instance or per-edge state, nothing serialized.
  - verify: full build; existing ctest suite green (no behavior change anywhere).
  - size: S

- [x] M17.P1.T2 — Structural kind resolution for polymorphic pass-through nodes
  - files: `Engine/Node.h`, `Engine/Node.cpp`, `Engine/NoOpBase.h`/`.cpp` (Dot/Switch), `Engine/GroupInput.cpp` or equivalent group-boundary sources
  - approach: pass-through utilities (Dot, Switch, NoOps, Group boundaries) declare `eDataKindPolymorphic`; a resolver on `Node` walks the graph to compute the effective kind from whatever feeds the chain (unconstrained when disconnected), caching the result and invalidating on connection change. One rule, no per-node special cases.
  - verify: unit test — a Dot fed by an image source resolves to image; disconnected Dot resolves unconstrained; resolution cache invalidates on reconnect.
  - size: M

- [x] M17.P1.T3 — Connection-time enforcement in `canConnectInput`
  - files: `Engine/Node.h` (the `CanConnectInputReturnValue` enum at :541), `Engine/Node.cpp`, `Gui/NodeGraph*.cpp` (user-facing message)
  - approach: add `eCanConnectInput_incompatibleDataKind`, checked against *resolved* kinds inside `canConnectInput()` — the single choke point every GUI drag path, undo/redo, auto-connect, and the Python API already use. Connecting into a polymorphic chain validates the whole resolved chain so a contradiction (deep source → Dot → image consumer) is rejected at the connection that introduces it, naming the conflicting node in the GUI message.
  - verify: unit tests — mismatched direct connection rejected; contradiction through a Dot chain rejected with the right return value; image→image and polymorphic cases still connect.
  - size: M

- [x] M17.P1.T4 — Project-load revalidation of edge kinds
  - files: `Engine/Project.cpp` / `Engine/ProjectPrivate.cpp` (connection-restore path)
  - approach: **Revised 2026-09-06 — see the decision below; the drop-the-edge behaviour first shipped here is superseded by M17.P1.T5.** After connections are restored on load, revalidate each edge's resolved kinds; an edge that does not hold up is **kept**, and the downstream node that cannot handle its input is put into an error state, using the same mechanism as a node whose configuration it cannot satisfy (see `Tests/ProjectOCIO_Test.cpp`'s `ColorSpaceMissingFromTheActiveConfigPutsItsNodeInAnErrorState`). Never silently rewire the user's graph. Old projects are trivially compatible (everything image-kind).
  - verify: unit/integration test — a hand-crafted project with a kind-invalid edge loads with the edge still connected and the node in an error state; a normal project loads unchanged with no error state.
  - size: M

- [x] M17.P1.T5 — Node-authored data-kind resolution
  - files: `Engine/Node.h`, `Engine/Node.cpp`, `Engine/Nodes/NativeEffectBase.h`, `Engine/Nodes/NativeEffectBase.cpp`, `Tests/DataKind_Test.cpp`, `Tests/DataKindTestEffect.h`
  - approach: **Added 2026-09-06 from PR #19 review — see the decision below.** Replace the engine's first-wins guess with a resolution policy the node owns. Engine-level default in `Node`: bidirectional structural resolution — a polymorphic node's kind is the concrete kind it touches on either side, walking polymorphic-*declared* inputs and downstream consumers; order-independent and history-free, so it survives a reload. Only polymorphic-declared inputs may contribute (a connected concrete-declared input, e.g. a mask, must never determine the output). Multiple polymorphic inputs carrying disagreeing concrete kinds resolve to ambiguous, and an ambiguous producer may not connect to a consumer declaring a concrete kind. `NativeEffectBase` gains an override hook so a native node can supply its own policy (a future Switch following its selected input, declaring the knob dependency so the cache invalidates on knob change too); its default delegates to the engine resolution. No `EffectInstance` API change — OFX plugins are unaffected and stay image/image.
  - verify: unit tests — multi-input polymorphic node with disagreeing concrete inputs resolves ambiguous and is rejected by a concrete consumer; agreeing inputs resolve to that kind and connect; a concrete-declared input alone does not determine a polymorphic output; a pass-through resolves the same regardless of which side was connected first, and identically after a save/load round trip; existing single-input pass-through behaviour unchanged.
  - size: L

## Phase 17.2: Native node framework

- [x] M17.P2.T1 — `Engine/Nodes/` layout and `NativeEffectBase` convenience layer
  - files: `Engine/Nodes/NativeEffectBase.h`, `Engine/Nodes/NativeEffectBase.cpp`, `Engine/CMakeLists.txt` (or top-level CMake source lists)
  - approach: `NativeEffectBase : EffectInstance` — declarative plugin metadata (id, grouping, version), typed-IO declaration, knob-building helpers, so a new native node is one .cpp file. New source layout `Engine/Nodes/<Domain>/` keeps `Engine/` core free of node implementations; existing built-ins stay put. Single EffectInstance hierarchy — capability virtuals, no parallel Op hierarchies.
  - verify: builds; a header-doc example in the class comment compiles as written (framework doc task M17.P2.T3 expands it).
  - size: M

- [x] M17.P2.T2 — Proof node registered through the framework
  - files: `Engine/Nodes/TypedPassthrough.cpp` (new), `Engine/AppManager.cpp` (`loadBuiltinNodePlugins`, ~:1517)
  - approach: one deliberately trivial native node built on `NativeEffectBase` — a polymorphic typed no-op passthrough — registered via `loadBuiltinNodePlugins()`. Proves declaration, registration, typed-IO, and knob helpers before any feature pressure arrives.
  - verify: node appears in the node menu; inserting it mid-chain renders identically (integration test alongside the M11 OFX render test harness); kind resolution flows through it per M17.P1.T2's tests.
  - size: S

- [x] M17.P2.T3 — Native node framework documentation
  - files: `Engine/Nodes/README.md`
  - approach: **Re-planned 2026-09-06 — see the decision below.** In-repo only; no docs-branch page. Document the contract: subclass `NativeEffectBase`, declare kinds, register in `loadBuiltinNodePlugins()`; the sizing of what belongs in `Engine/Nodes/<Domain>/` vs `Engine/`; the enforcement rules a node author must know (kinds are static plugin declarations; adapters are Viewer-only).
  - verify: a reader can create the proof node (M17.P2.T2) from the doc alone — check the doc against `Engine/Nodes/TypedPassthrough.{h,cpp}` and the worked example in `NativeEffectBase.h`, and confirm no step is missing.
  - size: S

## Phase 17.3: Kind legibility in the node graph

- [x] M17.P3.T1 — Edge styling by resolved kind
  - files: `Gui/Edge.cpp`, `Gui/Edge.h`
  - approach: `Edge` already does custom paint + dashes — render deep and scene edges distinctly by *resolved* kind (line style first, color as reinforcement only; colorblind- and zoom-safe). This is what makes polymorphic nodes legible: a Dot stays neutral, its edges show what flows through it.
  - verify: manual GUI check with the M17.P2.T2 proof node forced to each kind; screenshot comparison in the GUI test harness if available, else a checklist in the PR.
  - size: M

- [x] M17.P3.T2 — Node silhouettes and input-arrow glyphs by kind
  - files: `Gui/NodeGui.cpp`, `Gui/NodeGui.h`
  - approach: per-output-kind node silhouette carved within the existing bounding rect via the already-virtual `NodeGui::paint()` (name frame / icon / preview / resize-handle layout assumes that rect); input-arrow glyphs by *declared* kind on dangling unconnected-input arrows so a node's accepted kinds read before connection. Hybrid nodes take their output kind's shape. `NodeGuiIndicator` badges stay reserved for transient state.
  - verify: manual GUI check per M17.P3.T1's method; layout untouched for image-kind nodes (the entire existing node set renders pixel-identical).
  - size: M

## Decisions

- 2026-09-05 — M17.P1.T3's downstream conflict walk does not cross group boundaries: `Node::findDataKindConflictDownstream()` walks direct outputs rather than `getOutputsWithGroupRedirection()`, so a contradiction only visible past a `GroupInput`/`GroupOutput` boundary is not caught by the forward check. The backward check already understands group redirection and catches the same contradiction from the other connection order, so no graph can reach an invalid steady state — and M17.P1.T4's project-load revalidation is the backstop. Left as-is to keep the task to one coherent change; revisit if a real graph trips it.
- 2026-09-05 — pre-existing cache-thread shutdown race left unfixed in M17: `DeleterThread::quitThread()` and `CacheCleanerThread::quitThread()` in `Engine/Cache.h` satisfy their `mustQuit` handshake before `QThreadPrivate::finish()` runs and never call `QThread::wait()`, so `~Cache` can `qFatal` with "QThread: Destroyed while thread is still running". Unchanged since 2016, unrelated to typed edges, and only fires under CPU contention (reproduced on `HashChangesOnInputConnected` and `HashChangesOnKnobValueChange` under synthetic load; clean on an idle machine). Out of scope here — recorded so a red CI run on this branch is read as this flake, not a regression. Fix would be a `wait()` after the handshake in both, mirroring `GenericSchedulerThread::~GenericSchedulerThread`.
- 2026-09-05 — single EffectInstance hierarchy, no parallel Op hierarchies: Natron's knob, hashing, undo, serialization, and scheduling machinery all hang off EffectInstance; parallel hierarchies would duplicate all of it for no isolation benefit (design doc, "Native node framework formalization").

- 2026-09-05 — M17.P1.T4 moved data-kind enforcement off the per-connection restore path: `Node::canConnectInput()` now skips the kind check while `Project::isLoadingProject()`, and `ProjectPrivate::revalidateDataKindEdges()` judges the fully-restored tree instead. Kinds resolve structurally, so mid-restore the verdict depends on how much of the tree happens to be wired yet — `NodeCollection::connectNodes()` was rejecting the edge order-dependently with only a `qDebug()`, which is the silent miswire the task exists to prevent. The whole-tree pass is the only sound point of enforcement on load, and the only one that warns.

- 2026-09-05 — test plugins registered from `Tests/wmain.cpp` now get a label-without-suffix: `registerTestBuiltInPlugin()` runs after `AppManager::load()`, so `onAllPluginsLoaded()` has already assigned every other plugin's, and without it these nodes take empty script names and no connection between them survives a save/load round trip. Latent trap for the whole `DataKind_Test.cpp` suite, not just M17.P1.T4.

- 2026-09-05 — `NativeEffectBase` queries its subclass's `NativePluginDescription` on demand rather than caching it: virtual dispatch to a derived override does not work from a base constructor, and every call site (registration, UI display, project load) is off the render path. Keeps the class free of per-instance state, which is what makes data kinds static plugin declarations rather than instance data.

- 2026-09-06 — M17.P2.T2's "renders identically mid-chain" test uses the gtest render path (`RenderRange_Test.cpp`'s SeNoise -> WriteOIIO EXR pattern, comparing every pixel of all four channels), not the M11 harness the brief named: `tools/ci/smoke_test.py` is a separate Python/NatronRenderer harness that `test.sh ctest` does not run, so a test written against it would not gate anything.

- 2026-09-06 — M17.P2.T3's framework doc is in-repo only (`Engine/Nodes/README.md`), not on the `docs` branch its brief named. The brief's `verify` ("doc CI (M14 gate) passes") describes a gate that does not exist: M14 moved documentation to a parked orphan branch and left no doc CI, and per `DECISIONS/2026-09-04-docs-to-orphan-branch.md` that branch holds stale 2.4-era user docs with the publish decision deferred. A developer doc written today would be buried in explicitly-not-current material with nothing gating it. In-repo, it sits beside the code it describes and is reviewed in the same PR. User confirmed 2026-09-06.

- 2026-09-06 — edge kind styling uses pen width as the primary channel, not dashes: `Edge`'s existing dash pattern already means "this input is not live" (mask, inactive viewer input, non-selected input of an identity pass-through), which is a destination-side interaction state orthogonal to data kind and can hold at the same time. Overloading one QPen dash property for both would force a precedence choice whenever both apply. Width also degrades better at zoom-out than a dash period, which collapses into uniform gray sub-pixel. Colour (Okabe-Ito blue/orange) is reinforcement only, applied in the lowest-priority branch so selection, highlight and rendering colours still win. Image and polymorphic edges keep exactly the pen they had.

- 2026-09-06 — no GUI test harness exists in this repo, so M17.P3's tasks cannot be verified automatically: their gate is build + ctest + a human visual pass against a reviewer checklist carried in the PR body. Recorded so the milestone is not read as having automated coverage it does not have.

- 2026-09-06 — the cache-thread shutdown race recorded above is not confined to the two tests originally named: it aborted `BaseTest.RenderFrameRangeProducesDistinctFramePixels` during M17.P3.T2's verification (test body reported `[  PASSED  ]`, then `QThread: Destroyed while thread is still running` at teardown, ctest reading it as "Subprocess aborted"). A clean re-run gave 70/70. Any ctest case can hit it, so a single red CI run on this branch showing an abort *after* a passing test body is this flake, not a regression.

- 2026-09-06 — data-kind resolution policy belongs to the node, not the engine (user decision, PR #19 review). `eDataKindPolymorphic` was carrying two meanings at once — "this input accepts anything" and "my output kind is derived from my inputs" — which left the engine guessing which inputs determine the output. First-wins guessed "input 0"; the reviewer's proposed uniform-kind rule guessed "all of them, and they must agree". Both invent an answer the node never gave, and uniform-kind additionally forbids legitimate graphs: a node with a primary input plus a polymorphic auxiliary, or a Switch selecting between an image and a deep branch. Resolved by giving nodes the policy, with a bidirectional structural default. Rejected "whichever side is connected first sets the other" as stated: kinds are never serialized, so a history-dependent rule resolves differently after a reload and `revalidateDataKindEdges()` could drop an edge that was legal when built — resolving from whatever concrete kind the node structurally touches gives the same UX without the history. The override hook goes on `NativeEffectBase`, not `EffectInstance`, so OFX is untouched; existing built-in pass-throughs (`Dot`, `NoOpBase`, group boundaries) are served by the engine default and are not migrated.

- 2026-09-06 — `NodeGroup` is an image barrier, deferred: it declares no kinds, so a deep or scene chain inside a group reports as image at the group's external output (PR #19 review finding). Resolving a group's kind from its internal `GroupOutput` is a design decision, not a contained fix, and no node can produce deep or scene data yet, so nothing can reach it. Deferred to the milestone that introduces real deep payloads; recorded in `Engine/Nodes/README.md` as a known limitation.

- 2026-09-06 — an edge that becomes invalid after the fact is kept and the downstream node enters an error state; it is never removed (user decision, PR #19 review). Two regimes: at **connection time** an incompatible connection is still rejected outright, since the user is actively making it and no invalid edge need exist; **later invalidation** — a project load, a resolution change propagating from elsewhere, or a future knob-driven policy change — leaves the graph exactly as the user built it and surfaces the problem on the node instead. Silently rewiring someone's comp on load is worse than showing them a node that cannot run, and the codebase already has the precedent (a project naming an OCIO colorspace the active config lacks loads with that node errored, not with the project altered). This reverses M17.P1.T4's original drop-the-edge-with-a-warning behaviour, which had been modelled on the missing-plugin policy.

- 2026-09-07 — M17.P3's visual gate was met with screenshots rather than a human pass. The Xvfb GUI harness built for the release packaging gate scripts a graph and captures the node-graph view, so the styling is reviewable from images. It immediately caught what review and tests had not: the dangling-input kind glyph was anchored at the input edge's node-centre endpoint rather than its free tip, drawing it over the node's own label. `Edge::initLine()` builds a dangling edge from the node centre out to the arrow tip, so the two endpoints are centre-then-tip, not the other way round. Fixed in `35f8afbb1`. GL-dependent appearance remains unverified: this Xvfb produces no usable GLX config.

**Verification gate:** full build + entire ctest suite green; new kind-resolution/enforcement unit tests pass; proof node loads and passes its integration test; image-kind rendering of the existing node set is bit-identical (no regression from a foundation-only milestone); doc CI green.
