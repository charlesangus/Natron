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

- [ ] M17.P1.T2 — Structural kind resolution for polymorphic pass-through nodes
  - files: `Engine/Node.h`, `Engine/Node.cpp`, `Engine/NoOpBase.h`/`.cpp` (Dot/Switch), `Engine/GroupInput.cpp` or equivalent group-boundary sources
  - approach: pass-through utilities (Dot, Switch, NoOps, Group boundaries) declare `eDataKindPolymorphic`; a resolver on `Node` walks the graph to compute the effective kind from whatever feeds the chain (unconstrained when disconnected), caching the result and invalidating on connection change. One rule, no per-node special cases.
  - verify: unit test — a Dot fed by an image source resolves to image; disconnected Dot resolves unconstrained; resolution cache invalidates on reconnect.
  - size: M

- [ ] M17.P1.T3 — Connection-time enforcement in `canConnectInput`
  - files: `Engine/Node.h` (the `CanConnectInputReturnValue` enum at :541), `Engine/Node.cpp`, `Gui/NodeGraph*.cpp` (user-facing message)
  - approach: add `eCanConnectInput_incompatibleDataKind`, checked against *resolved* kinds inside `canConnectInput()` — the single choke point every GUI drag path, undo/redo, auto-connect, and the Python API already use. Connecting into a polymorphic chain validates the whole resolved chain so a contradiction (deep source → Dot → image consumer) is rejected at the connection that introduces it, naming the conflicting node in the GUI message.
  - verify: unit tests — mismatched direct connection rejected; contradiction through a Dot chain rejected with the right return value; image→image and polymorphic cases still connect.
  - size: M

- [ ] M17.P1.T4 — Project-load revalidation of edge kinds
  - files: `Engine/Project.cpp` / `Engine/ProjectPrivate.cpp` (connection-restore path)
  - approach: after connections are restored on load, revalidate each edge's resolved kinds; drop an invalid edge with a user-visible warning (same policy as a missing plugin), never silently miswire. Old projects are trivially compatible (everything image-kind).
  - verify: unit/integration test — a hand-crafted project with a kind-invalid edge loads with the edge dropped and a warning logged; a normal project loads unchanged.
  - size: M

## Phase 17.2: Native node framework

- [ ] M17.P2.T1 — `Engine/Nodes/` layout and `NativeEffectBase` convenience layer
  - files: `Engine/Nodes/NativeEffectBase.h`, `Engine/Nodes/NativeEffectBase.cpp`, `Engine/CMakeLists.txt` (or top-level CMake source lists)
  - approach: `NativeEffectBase : EffectInstance` — declarative plugin metadata (id, grouping, version), typed-IO declaration, knob-building helpers, so a new native node is one .cpp file. New source layout `Engine/Nodes/<Domain>/` keeps `Engine/` core free of node implementations; existing built-ins stay put. Single EffectInstance hierarchy — capability virtuals, no parallel Op hierarchies.
  - verify: builds; a header-doc example in the class comment compiles as written (framework doc task M17.P2.T3 expands it).
  - size: M

- [ ] M17.P2.T2 — Proof node registered through the framework
  - files: `Engine/Nodes/TypedPassthrough.cpp` (new), `Engine/AppManager.cpp` (`loadBuiltinNodePlugins`, ~:1517)
  - approach: one deliberately trivial native node built on `NativeEffectBase` — a polymorphic typed no-op passthrough — registered via `loadBuiltinNodePlugins()`. Proves declaration, registration, typed-IO, and knob helpers before any feature pressure arrives.
  - verify: node appears in the node menu; inserting it mid-chain renders identically (integration test alongside the M11 OFX render test harness); kind resolution flows through it per M17.P1.T2's tests.
  - size: S

- [ ] M17.P2.T3 — Native node framework documentation
  - files: docs branch (per M14 layout) — one new page on writing a native node; `Engine/Nodes/README.md`
  - approach: document the contract: subclass `NativeEffectBase`, declare kinds, register; the sizing of what belongs in `Engine/Nodes/<Domain>/` vs `Engine/`; the enforcement rules a node author must know (kinds are static; adapters are Viewer-only).
  - verify: doc CI (M14 gate) passes; a reader can create the proof node (M17.P2.T2) from the doc alone.
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

- 2026-09-05 — single EffectInstance hierarchy, no parallel Op hierarchies: Natron's knob, hashing, undo, serialization, and scheduling machinery all hang off EffectInstance; parallel hierarchies would duplicate all of it for no isolation benefit (design doc, "Native node framework formalization").

**Verification gate:** full build + entire ctest suite green; new kind-resolution/enforcement unit tests pass; proof node loads and passes its integration test; image-kind rendering of the existing node set is bit-identical (no regression from a foundation-only milestone); doc CI green.
