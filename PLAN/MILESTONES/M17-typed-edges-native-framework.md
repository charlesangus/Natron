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
  - approach: after connections are restored on load, revalidate each edge's resolved kinds; drop an invalid edge with a user-visible warning (same policy as a missing plugin), never silently miswire. Old projects are trivially compatible (everything image-kind).
  - verify: unit/integration test — a hand-crafted project with a kind-invalid edge loads with the edge dropped and a warning logged; a normal project loads unchanged.
  - size: M

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

- [ ] M17.P3.T1 — Edge styling by resolved kind
  - files: `Gui/Edge.cpp`, `Gui/Edge.h`
  - approach: `Edge` already does custom paint + dashes — render deep and scene edges distinctly by *resolved* kind (line style first, color as reinforcement only; colorblind- and zoom-safe). This is what makes polymorphic nodes legible: a Dot stays neutral, its edges show what flows through it.
  - verify: manual GUI check with the M17.P2.T2 proof node forced to each kind; screenshot comparison in the GUI test harness if available, else a checklist in the PR.
  - size: M

- [ ] M17.P3.T2 — Node silhouettes and input-arrow glyphs by kind
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

**Verification gate:** full build + entire ctest suite green; new kind-resolution/enforcement unit tests pass; proof node loads and passes its integration test; image-kind rendering of the existing node set is bit-identical (no regression from a foundation-only milestone); doc CI green.
