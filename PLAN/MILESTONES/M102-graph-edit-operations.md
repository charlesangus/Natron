# M102 - Graph Edit Operations

Shared foundation for M103 - Graph Diff And Merge and M105 - Live Session Core. Every node gets a
**persistent ID**, and graph edits become **plain-data operations**. An operation can be
recorded from the live graph, serialized, sent, applied to a live graph, applied to a plain
snapshot, or computed as the difference between two snapshots. Depends on M16 - Project Format
Redesign (plain `Serialization/` structs and YAML encoders). No user-facing feature ships here.

Scouting facts (2026-10-11, `main` at `10e0accd5`; re-check line numbers, since M93 and M16 move them):
- There is no node ID. `Node::getHashValue()` (`Engine/Node.h:232`) is a content hash for
  caching. Script names are assigned in `Node::initNodeScriptName` (`Engine/NodeName.cpp:53`).
  A rename (`Node::setScriptName`, `NodeName.cpp:427`) uniquifies the name within its group via
  `NodeGroup::checkNodeName` (`Engine/NodeGroup.h:117`).
- Edits have no single hook. The chokepoints are:
  - knob changes: `KnobHelper::evaluateValueChange[Internal]` (`Engine/Knob.cpp:1870,1827`), whose `reason` argument separates user, plugin and internal edits;
  - create: `AppInstance::createNode[Internal]` (`Engine/AppInstance.cpp:1393,1108`);
  - delete: `Node::destroyNode` (`Engine/NodeMain.cpp:905`). The GUI deletes through `Node::deactivate`/`activate` (`Engine/Node.cpp:3549,3786`) so that undo works;
  - inputs: `Node::connectInput`/`replaceInput`/`disconnectInput` (`Engine/NodeInputs.cpp:765,956,1105,1255`);
  - position: `Node::setPosition` (`Engine/Node.cpp:7719`);
  - rename and label: the `Node::scriptNameChanged`/`labelChanged` signals (`Node.h:1837-1838`).
- Undo stacks are GUI-only (`QUndoStack`, `Gui/NodeGraph.h:288`). The commands hold `NodeGuiPtr` and are not serializable. `Engine/UndoCommand.h` is a thin Engine-side interface.

## Design (applies to every task)

- **Node ID:** 128-bit random, stored as 32 lowercase hex characters (`QUuid::createUuid()`, no
  braces or dashes). It lives on `Node`, in plain `NodeSerialization::id`, and as the YAML
  `id:` key. It is minted at creation and kept on load. Paste, duplicate and import mint fresh
  IDs. A duplicate ID found while loading is reminted, with a load warning (M16's
  load-warnings list). Undo of a delete restores the original ID, because the GUI delete is a
  deactivate.
- **Operations** (`Serialization/Ops/GraphOp.h`): a `std::variant` of
  - `CreateNode{parentId|root, NodeSerialization (children included)}`
  - `DeleteNode{id}`
  - `SetKnob{nodeId|project, knob script name, KnobSerialization of the whole knob, or "default"}`
  - `SetInput{nodeId, input label, sourceId|none}`
  - `Rename{nodeId, scriptName}`
  - `SetNodeUi{nodeId, position?, size?, colour?}`
  - `SetUserKnobs{nodeId, user knob definitions + pages}`
  - `SetRotoData{nodeId, RotoContextSerialization}`
  - `SetTrackerData{nodeId, TrackerContextSerialization}`
  - `SetProjectData{formats?, layer registry?}`

  Operations name nodes **by ID, never by script name**, so a concurrent rename cannot misdirect
  them. Roto and tracker edits are whole-data operations in v1.
- **Encoding:** YAML through the M16 encoders. One operation is a map with an `op:` key, and a
  batch is a sequence. The same encoding is used on the wire (M105 - Live Session Core) and in
  diff output (M103 - Graph Diff And Merge).
- **Apply on snapshots:** `applyOps(ProjectSerialization&, ops)` is a pure function in
  `Serialization/`. **Apply on the live graph** is in `Engine/GraphOpApply`, which runs under a
  `GraphEditRecorder::Suppress` guard so applied operations are never re-recorded. Each
  operation returns applied, target-missing or invalid, and never crashes. Undo wrapping is the
  caller's job (M104 - Merge Resolution UI wants it; M105 - Live Session Core must not have it).
- **Two-way snapshot diff:** `diffSnapshots(a, b)` returns the operations that turn `a` into
  `b`, matching nodes by ID. Nodes are created parent-first, with every `SetInput` in a later
  pass so creation order never matters. Output order is deterministic.
- **Recording:** `Engine/GraphEditRecorder` observes the chokepoints and emits operations for
  every edit that changes serialized state: user, plugin and Python edits alike. It skips
  loading, suppressed applies and time-only refreshes. Knob edits are coalesced per knob per
  event-loop turn into one `SetKnob`. With no subscriber it costs one branch per hook.
- **Invariant**, the milestone's core test: snapshot S0, run an edit session while recording,
  snapshot S1. Replaying the recorded operations on a fresh load of S0 gives a `project.yaml`
  byte-identical to S1. `applyOps(S0, diffSnapshots(S0, S1))` equals S1.

## Phase 102.1: Persistent node identity

- [ ] M102.P1.T1 — Persist a node ID
  - files: `Engine/Node.h`, `Engine/NodeName.cpp`, `Serialization/NodeSerialization.h`, `Serialization/Yaml/NodeYaml.cpp`, new `Tests/NodeId_Test.cpp`
  - approach: implement the ID rules in **Design**: mint at creation, keep on load, mint fresh on paste, duplicate and import, and remint a duplicate on load with a warning. Extend `Node::toSerialization` and `fromSerialization` (M93's conversions) to carry it. YAML emits `id:` first in each node map.
  - verify: compiled at P1.T3. gtest: a new node has a 32-hex ID and two nodes differ; save → load keeps IDs; copy/paste and duplicate get fresh IDs; deleting a node and undoing the delete keeps its ID; a hand-edited bundle with a duplicate ID loads with one reminted ID and one warning.
  - size: M

- [ ] M102.P1.T2 — Find nodes by ID, in Engine and Python
  - files: `Engine/NodeGroup.h`, `Engine/NodeGroup.cpp`, `Engine/Project.h`, `Engine/PyNode.cpp`, `Engine/PyAppInstance.cpp`
  - approach: a per-project `id → NodeWPtr` index, maintained on create, destroy, activate and deactivate, and when a node moves into or out of a group. Add `Project::findNodeById(id)`, Python `Effect.getNodeId()` and `app.getNodeById(id)`, with the PySide/Shiboken typesystem updated if the bindings are generated.
  - verify: compiled at P1.T3. A gtest covers lookup after create, after a move into a group, and after delete (empty result) followed by undo. A Python test round-trips `app.getNodeById(n.getNodeId())`.
  - size: M

- [ ] M102.P1.T3 — Phase 102.1 build and test checkpoint
  - files: none
  - approach: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug`, then `tools/ci/local/test.sh ctest debug`.
  - verify: green. The build is required.
  - size: S

## Phase 102.2: Operation model

- [ ] M102.P2.T1 — Operation data types and YAML encoding
  - files: new `Serialization/Ops/GraphOp.h`, new `Serialization/Ops/GraphOp.cpp`, new `Serialization/Yaml/GraphOpYaml.cpp`, `Serialization/CMakeLists.txt`, new `Tests/Serialization/GraphOpYaml_Test.cpp`
  - approach: implement the variant in **Design**. Encoding reuses the M16 node, knob, roto and tracker encoders. Unknown `op:` values decode to an error result, not an exception that aborts the whole batch.
  - verify: compiled at P2.T5. Every operation kind round-trips encode → decode with field equality; a batch of mixed operations round-trips; an unknown op gives an error entry and the rest still decode.
  - size: M

- [ ] M102.P2.T2 — Apply operations to a plain snapshot
  - files: new `Serialization/Ops/ApplyOps.h`, new `Serialization/Ops/ApplyOps.cpp`, new `Tests/Serialization/ApplyOps_Test.cpp`
  - approach: `applyOps(ProjectSerialization&, const std::vector<GraphOp>&) → std::vector<OpResult>`. Find nodes by ID across nested `children`. Deleting a group removes its subtree. `SetInput` to a missing source gives target-missing.
  - verify: compiled at P2.T5. Each operation kind is applied to a built snapshot and checked; a missing target returns the right result and leaves the snapshot unchanged.
  - size: M

- [ ] M102.P2.T3 — Two-way snapshot diff
  - files: new `Serialization/Ops/SnapshotDiff.h`, new `Serialization/Ops/SnapshotDiff.cpp`, new `Tests/Serialization/SnapshotDiff_Test.cpp`
  - approach: implement `diffSnapshots` from **Design**: deletes, then creates parent-first, then knob, rename, UI and user-knob changes, then every `SetInput` last. Compare knobs by `KnobSerialization` equality, so default-valued knobs that are absent on both sides are equal.
  - verify: compiled at P2.T5. For 10 hand-built pairs (add, delete, rename, rewire, animate, nested group add and delete, roto change), `applyOps(a, diffSnapshots(a, b)) == b`, and diffing identical snapshots gives no operations.
  - size: M

- [ ] M102.P2.T4 — Apply operations to the live graph
  - files: new `Engine/GraphOpApply.h`, new `Engine/GraphOpApply.cpp`, `Engine/CMakeLists.txt`, new `Tests/GraphOpApply_Test.cpp`
  - approach: map each operation onto the existing Engine paths:
    - create through `createNode` with a serialization, keeping its ID;
    - delete through `destroyNode`;
    - knobs through M93's `fromSerialization`;
    - inputs through `connectInput`/`disconnectInput`, looked up by input label;
    - rename through `setScriptName`;
    - UI through `Node::setPosition` and the size and colour setters;
    - roto and tracker through their restore paths.

    Run the batch under `GraphEditRecorder::Suppress` (a stub until P3.T1 if needed). Return per-operation results.
  - verify: compiled at P2.T5. A gtest applies every operation kind to a live project and checks state through `toSerialization`; a missing target returns target-missing and does not crash.
  - size: L

- [ ] M102.P2.T5 — Phase 102.2 build and test checkpoint
  - files: none
  - approach: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug`, then `tools/ci/local/test.sh ctest debug`.
  - verify: green. The build is required.
  - size: S

## Phase 102.3: Recording edits

- [ ] M102.P3.T1 — Recorder core and knob-edit recording
  - files: new `Engine/GraphEditRecorder.h`, new `Engine/GraphEditRecorder.cpp`, `Engine/Knob.cpp`, `Engine/CMakeLists.txt`, new `Tests/GraphEditRecorder_Test.cpp`
  - approach: a per-project recorder with subscribe and unsubscribe, the `Suppress` RAII guard, and a coalescing buffer flushed at the end of the event-loop turn (or explicitly in tests). Hook value, keyframe, expression and default-reset changes in `evaluateValueChange` and the curve and expression setters. Each emits one `SetKnob` with the knob's whole serialization. Skip anything during project load or under `Suppress`.
  - verify: compiled at P3.T6. gtest: setting a value, a keyframe, an expression and a reset each emit one coalesced `SetKnob`; 100 sets in one turn emit one; edits during load or under `Suppress` emit nothing.
  - size: M

- [ ] M102.P3.T2 — Record node creation and deletion
  - files: `Engine/GraphEditRecorder.cpp`, `Engine/AppInstance.cpp`, `Engine/NodeMain.cpp`, `Engine/Node.cpp` (`activate`/`deactivate`)
  - approach: create emits `CreateNode` with the node's full serialization, captured after its knobs are initialised. `destroyNode` and `deactivate` emit `DeleteNode`, and `activate` emits `CreateNode`, so undo of a delete re-creates the node with the same ID. Compound GUI actions such as group-from-selection and inline-group come out as their primitive operations.
  - verify: compiled at P3.T6. gtests in `Tests/GraphEditRecorder_Test.cpp` cover create, delete, delete followed by reactivate (same ID), and group-from-selection.
  - size: M

- [ ] M102.P3.T3 — Record input, rename and node UI edits
  - files: `Engine/GraphEditRecorder.cpp`, `Engine/NodeInputs.cpp`, `Engine/NodeName.cpp`, `Engine/Node.cpp` (`setPosition`, size and colour setters)
  - approach: `connectInput`, `replaceInput` and `disconnectInput` emit `SetInput`, keyed by input label. `setScriptName` emits `Rename`, and position, size and colour emit `SetNodeUi`, coalesced per turn so a node drag sends one operation per node.
  - verify: compiled at P3.T6. gtests in `Tests/GraphEditRecorder_Test.cpp` cover each kind, including a 50-step drag that emits one `SetNodeUi`.
  - size: M

- [ ] M102.P3.T4 — Record roto, tracker, user-knob and project-level edits
  - files: `Engine/GraphEditRecorder.cpp`, `Engine/RotoContext.cpp`, `Engine/TrackerContext.cpp`, `Engine/Knob.cpp` (user-knob add and remove, page changes), `Engine/Project.cpp`
  - approach: any change to a roto item, stroke or shape marks the node dirty, and the turn flush emits one `SetRotoData`. Tracker works the same way with `SetTrackerData`. User-knob and page edits emit `SetUserKnobs`, and project formats or layer-registry changes emit `SetProjectData`.
  - verify: compiled at P3.T6. gtests: drawing a stroke emits one `SetRotoData`, adding a track one `SetTrackerData`, adding a user knob one `SetUserKnobs`.
  - size: M

- [ ] M102.P3.T5 — Record-and-replay invariant test
  - files: new `Tests/GraphEditReplay_Test.cpp`
  - approach: build S0 (a generator, Grade, Merge, RotoPaint, Tracker and a group), subscribe, then run a scripted session of about 30 edits of every kind, including delete with undo-style reactivation, rename, rewire, animation, a group-from-selection and a roto stroke. Snapshot S1. Replay the recorded operations on a fresh load of S0 and compare the encoded `project.yaml` text. Also check `applyOps(S0, diffSnapshots(S0, S1)) == S1`.
  - verify: compiled at P3.T6, where it passes. A deliberately dropped hook, tried locally and reverted, makes it fail.
  - size: M

- [ ] M102.P3.T6 — Phase 102.3 build, full ctest and smoke
  - files: none
  - approach: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug`, then `tools/ci/local/test.sh ctest debug` and `smoke debug`.
  - verify: everything green. The build is required.
  - size: S

**Verification gate:** full ctest and smoke green, and CI (`format`, `lint-ci`, `build-and-test`)
green. `GraphEditReplay_Test` passes byte-identically, and diff/apply round-trips hold on every
pair. No `Serialization/**` file includes Engine or Gui.

## Decisions

- 2026-10-11 — **User decision:** persist a node ID (live sessions need one), but treat it as one matching signal among several for merge; M103 - Graph Diff And Merge's spike sets the matching policy.
- 2026-10-11 — Operations reference nodes by ID, carry whole-knob serializations, and roto and tracker edits are whole-data operations in v1. This trades bandwidth for simplicity; finer roto operations are a later optimisation if live sessions need them.
