## Pending

### 2026-10-11T00:40:00-04:00 — info
- refs: none
- **Two new related features from the user (2026-10-11), filed as five milestones below:** live multi-user sessions, where one Natron hosts a project and others join and see each other's edits live, and node-graph diff with three-way template merge (a supervisor publishes template v1, an artist modifies it, the supervisor publishes v2, and the artist merges). Both rest on one shared foundation: **M95 - Graph Edit Operations**, which adds a persistent node ID plus plain-data graph-edit operations that can be recorded, serialized, sent and applied. The order is M16 - Project Format Redesign → M95 - Graph Edit Operations → (M96 - Graph Diff And Merge → M97 - Merge Resolution UI) in parallel with (M98 - Live Session Core → M99 - Live Session Presence And Undo). Suggested board rows, in this order after M16 - Project Format Redesign:
  ```
  | M95 | Graph Edit Operations | todo | M16 | [M95-graph-edit-operations.md](PLAN/MILESTONES/M95-graph-edit-operations.md) |
  | M96 | Graph Diff And Merge | todo | M95 | [M96-graph-diff-and-merge.md](PLAN/MILESTONES/M96-graph-diff-and-merge.md) |
  | M97 | Merge Resolution UI | todo | M96 | [M97-merge-resolution-ui.md](PLAN/MILESTONES/M97-merge-resolution-ui.md) |
  | M98 | Live Session Core | todo | M95 | [M98-live-session-core.md](PLAN/MILESTONES/M98-live-session-core.md) |
  | M99 | Live Session Presence And Undo | todo | M98 | [M99-live-session-presence-and-undo.md](PLAN/MILESTONES/M99-live-session-presence-and-undo.md) |
  ```
  Suggested `# Context and constraints` bullet: "**M95–M99 (live sessions + graph merge)** added 2026-10-11 via the inbox (user-requested). M95 - Graph Edit Operations is the shared foundation (persistent node ID, plain-data edit operations). M96 - Graph Diff And Merge and M97 - Merge Resolution UI deliver three-way template merge. M98 - Live Session Core and M99 - Live Session Presence And Undo deliver live multi-user editing. All of it runs after M16 - Project Format Redesign. See `DECISIONS/2026-10-11-live-sessions-and-graph-merge.md`."
  Suggested project-wide decision `DECISIONS/2026-10-11-live-sessions-and-graph-merge.md` (+ INDEX line). The user decided on 2026-10-11:
  - **Live sessions are host-authoritative.** The host orders every edit and rebroadcasts it. On the same knob, the last writer wins. Peers stay connected (LAN/VPN) and there is no offline editing. CRDTs and node locking were rejected.
  - **Live-session v1 scope:** graph, knob and position sync, plus presence (each user's selection and open panels, in their colour), per-user undo, and follow-a-user's-viewer. **No session password in v1**, so the host dialog warns that anyone who can reach the port can join.
  - **Each peer renders locally with its own cache.** Only the script is shared, and media paths are assumed to resolve on every machine (shared storage).
  - **Merge is available in the app and on the CLI.** The CLI (`natron-merge`) auto-applies clean changes, records conflicts in the file, and doubles as a git merge driver and textconv. The in-app merge view highlights conflicted nodes and resolves per knob.
  - **A template is "just a bunch of nodes in a script" that lives in a file on disk.** Merge therefore works on the set of nodes that came from template T, not on a whole project or a Group.
  - **Node identity:** persist a node ID, since live sessions need one, but treat it as one matching signal among several. The user's example: an artist deletes a Grade and makes a new one in the same place, which "in a sense" is the same node. The merge's matching policy is decided by a scored spike, M96.P1, and **the user makes the call**.
  - **Node-specific merge resolution:** nodes and knob kinds can carry their own merge handlers, for example per-keyframe curve merge, per-item roto merge and per-track tracker merge. Prior art: the user's `charlesangus/nuke_diff`, whose lessons are recorded in M96's notes.

### 2026-10-11T00:41:00-04:00 — change-request
- refs: M95 (new milestone)
- New milestone **M95 - Graph Edit Operations**, file `PLAN/MILESTONES/M95-graph-edit-operations.md`, board row `| M95 | Graph Edit Operations | todo | M16 | [M95-graph-edit-operations.md](PLAN/MILESTONES/M95-graph-edit-operations.md) |`. Full file content:

````markdown
# M95 - Graph Edit Operations

Shared foundation for M96 - Graph Diff And Merge and M98 - Live Session Core. Every node gets a
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
  batch is a sequence. The same encoding is used on the wire (M98 - Live Session Core) and in
  diff output (M96 - Graph Diff And Merge).
- **Apply on snapshots:** `applyOps(ProjectSerialization&, ops)` is a pure function in
  `Serialization/`. **Apply on the live graph** is in `Engine/GraphOpApply`, which runs under a
  `GraphEditRecorder::Suppress` guard so applied operations are never re-recorded. Each
  operation returns applied, target-missing or invalid, and never crashes. Undo wrapping is the
  caller's job (M97 - Merge Resolution UI wants it; M98 - Live Session Core must not have it).
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

## Phase 95.1: Persistent node identity

- [ ] M95.P1.T1 — Persist a node ID
  - files: `Engine/Node.h`, `Engine/NodeName.cpp`, `Serialization/NodeSerialization.h`, `Serialization/Yaml/NodeYaml.cpp`, new `Tests/NodeId_Test.cpp`
  - approach: implement the ID rules in **Design**: mint at creation, keep on load, mint fresh on paste, duplicate and import, and remint a duplicate on load with a warning. Extend `Node::toSerialization` and `fromSerialization` (M93's conversions) to carry it. YAML emits `id:` first in each node map.
  - verify: compiled at P1.T3. gtest: a new node has a 32-hex ID and two nodes differ; save → load keeps IDs; copy/paste and duplicate get fresh IDs; deleting a node and undoing the delete keeps its ID; a hand-edited bundle with a duplicate ID loads with one reminted ID and one warning.
  - size: M

- [ ] M95.P1.T2 — Find nodes by ID, in Engine and Python
  - files: `Engine/NodeGroup.h`, `Engine/NodeGroup.cpp`, `Engine/Project.h`, `Engine/PyNode.cpp`, `Engine/PyAppInstance.cpp`
  - approach: a per-project `id → NodeWPtr` index, maintained on create, destroy, activate and deactivate, and when a node moves into or out of a group. Add `Project::findNodeById(id)`, Python `Effect.getNodeId()` and `app.getNodeById(id)`, with the PySide/Shiboken typesystem updated if the bindings are generated.
  - verify: compiled at P1.T3. A gtest covers lookup after create, after a move into a group, and after delete (empty result) followed by undo. A Python test round-trips `app.getNodeById(n.getNodeId())`.
  - size: M

- [ ] M95.P1.T3 — Phase 95.1 build and test checkpoint
  - files: none
  - approach: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug`, then `tools/ci/local/test.sh ctest debug`.
  - verify: green. The build is required.
  - size: S

## Phase 95.2: Operation model

- [ ] M95.P2.T1 — Operation data types and YAML encoding
  - files: new `Serialization/Ops/GraphOp.h`, new `Serialization/Ops/GraphOp.cpp`, new `Serialization/Yaml/GraphOpYaml.cpp`, `Serialization/CMakeLists.txt`, new `Tests/Serialization/GraphOpYaml_Test.cpp`
  - approach: implement the variant in **Design**. Encoding reuses the M16 node, knob, roto and tracker encoders. Unknown `op:` values decode to an error result, not an exception that aborts the whole batch.
  - verify: compiled at P2.T5. Every operation kind round-trips encode → decode with field equality; a batch of mixed operations round-trips; an unknown op gives an error entry and the rest still decode.
  - size: M

- [ ] M95.P2.T2 — Apply operations to a plain snapshot
  - files: new `Serialization/Ops/ApplyOps.h`, new `Serialization/Ops/ApplyOps.cpp`, new `Tests/Serialization/ApplyOps_Test.cpp`
  - approach: `applyOps(ProjectSerialization&, const std::vector<GraphOp>&) → std::vector<OpResult>`. Find nodes by ID across nested `children`. Deleting a group removes its subtree. `SetInput` to a missing source gives target-missing.
  - verify: compiled at P2.T5. Each operation kind is applied to a built snapshot and checked; a missing target returns the right result and leaves the snapshot unchanged.
  - size: M

- [ ] M95.P2.T3 — Two-way snapshot diff
  - files: new `Serialization/Ops/SnapshotDiff.h`, new `Serialization/Ops/SnapshotDiff.cpp`, new `Tests/Serialization/SnapshotDiff_Test.cpp`
  - approach: implement `diffSnapshots` from **Design**: deletes, then creates parent-first, then knob, rename, UI and user-knob changes, then every `SetInput` last. Compare knobs by `KnobSerialization` equality, so default-valued knobs that are absent on both sides are equal.
  - verify: compiled at P2.T5. For 10 hand-built pairs (add, delete, rename, rewire, animate, nested group add and delete, roto change), `applyOps(a, diffSnapshots(a, b)) == b`, and diffing identical snapshots gives no operations.
  - size: M

- [ ] M95.P2.T4 — Apply operations to the live graph
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

- [ ] M95.P2.T5 — Phase 95.2 build and test checkpoint
  - files: none
  - approach: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug`, then `tools/ci/local/test.sh ctest debug`.
  - verify: green. The build is required.
  - size: S

## Phase 95.3: Recording edits

- [ ] M95.P3.T1 — Recorder core and knob-edit recording
  - files: new `Engine/GraphEditRecorder.h`, new `Engine/GraphEditRecorder.cpp`, `Engine/Knob.cpp`, `Engine/CMakeLists.txt`, new `Tests/GraphEditRecorder_Test.cpp`
  - approach: a per-project recorder with subscribe and unsubscribe, the `Suppress` RAII guard, and a coalescing buffer flushed at the end of the event-loop turn (or explicitly in tests). Hook value, keyframe, expression and default-reset changes in `evaluateValueChange` and the curve and expression setters. Each emits one `SetKnob` with the knob's whole serialization. Skip anything during project load or under `Suppress`.
  - verify: compiled at P3.T6. gtest: setting a value, a keyframe, an expression and a reset each emit one coalesced `SetKnob`; 100 sets in one turn emit one; edits during load or under `Suppress` emit nothing.
  - size: M

- [ ] M95.P3.T2 — Record node creation and deletion
  - files: `Engine/GraphEditRecorder.cpp`, `Engine/AppInstance.cpp`, `Engine/NodeMain.cpp`, `Engine/Node.cpp` (`activate`/`deactivate`)
  - approach: create emits `CreateNode` with the node's full serialization, captured after its knobs are initialised. `destroyNode` and `deactivate` emit `DeleteNode`, and `activate` emits `CreateNode`, so undo of a delete re-creates the node with the same ID. Compound GUI actions such as group-from-selection and inline-group come out as their primitive operations.
  - verify: compiled at P3.T6. gtests in `Tests/GraphEditRecorder_Test.cpp` cover create, delete, delete followed by reactivate (same ID), and group-from-selection.
  - size: M

- [ ] M95.P3.T3 — Record input, rename and node UI edits
  - files: `Engine/GraphEditRecorder.cpp`, `Engine/NodeInputs.cpp`, `Engine/NodeName.cpp`, `Engine/Node.cpp` (`setPosition`, size and colour setters)
  - approach: `connectInput`, `replaceInput` and `disconnectInput` emit `SetInput`, keyed by input label. `setScriptName` emits `Rename`, and position, size and colour emit `SetNodeUi`, coalesced per turn so a node drag sends one operation per node.
  - verify: compiled at P3.T6. gtests in `Tests/GraphEditRecorder_Test.cpp` cover each kind, including a 50-step drag that emits one `SetNodeUi`.
  - size: M

- [ ] M95.P3.T4 — Record roto, tracker, user-knob and project-level edits
  - files: `Engine/GraphEditRecorder.cpp`, `Engine/RotoContext.cpp`, `Engine/TrackerContext.cpp`, `Engine/Knob.cpp` (user-knob add and remove, page changes), `Engine/Project.cpp`
  - approach: any change to a roto item, stroke or shape marks the node dirty, and the turn flush emits one `SetRotoData`. Tracker works the same way with `SetTrackerData`. User-knob and page edits emit `SetUserKnobs`, and project formats or layer-registry changes emit `SetProjectData`.
  - verify: compiled at P3.T6. gtests: drawing a stroke emits one `SetRotoData`, adding a track one `SetTrackerData`, adding a user knob one `SetUserKnobs`.
  - size: M

- [ ] M95.P3.T5 — Record-and-replay invariant test
  - files: new `Tests/GraphEditReplay_Test.cpp`
  - approach: build S0 (a generator, Grade, Merge, RotoPaint, Tracker and a group), subscribe, then run a scripted session of about 30 edits of every kind, including delete with undo-style reactivation, rename, rewire, animation, a group-from-selection and a roto stroke. Snapshot S1. Replay the recorded operations on a fresh load of S0 and compare the encoded `project.yaml` text. Also check `applyOps(S0, diffSnapshots(S0, S1)) == S1`.
  - verify: compiled at P3.T6, where it passes. A deliberately dropped hook, tried locally and reverted, makes it fail.
  - size: M

- [ ] M95.P3.T6 — Phase 95.3 build, full ctest and smoke
  - files: none
  - approach: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug`, then `tools/ci/local/test.sh ctest debug` and `smoke debug`.
  - verify: everything green. The build is required.
  - size: S

**Verification gate:** full ctest and smoke green, and CI (`format`, `lint-ci`, `build-and-test`)
green. `GraphEditReplay_Test` passes byte-identically, and diff/apply round-trips hold on every
pair. No `Serialization/**` file includes Engine or Gui.

## Decisions

- 2026-10-11 — **User decision:** persist a node ID (live sessions need one), but treat it as one matching signal among several for merge; M96 - Graph Diff And Merge's spike sets the matching policy.
- 2026-10-11 — Operations reference nodes by ID, carry whole-knob serializations, and roto and tracker edits are whole-data operations in v1. This trades bandwidth for simplicity; finer roto operations are a later optimisation if live sessions need them.
````

### 2026-10-11T00:42:00-04:00 — change-request
- refs: M96 (new milestone)
- New milestone **M96 - Graph Diff And Merge**, file `PLAN/MILESTONES/M96-graph-diff-and-merge.md`, board row `| M96 | Graph Diff And Merge | todo | M95 | [M96-graph-diff-and-merge.md](PLAN/MILESTONES/M96-graph-diff-and-merge.md) |`. Full file content:

````markdown
# M96 - Graph Diff And Merge

Three-way merge of node graphs, as an engine and a CLI, with no GUI (that is M97 - Merge
Resolution UI). The workflow: a supervisor publishes a template (a file of nodes), an artist
brings it into a shot script and modifies it, and the supervisor publishes a new version. The
artist merges the upstream changes with their own. Merging runs on plain `ProjectSerialization`
snapshots in `Serialization/Merge/`, so the CLI loads no plugins and starts fast. Depends on
M95 - Graph Edit Operations.

**Phase 96.1 is a spike that stops for the user.** How nodes are matched between versions is
the user's call, made from the spike's scores. Later phases implement the chosen policy behind
a `NodeMatcher` interface that the spike defines.

Prior art, the user's `charlesangus/nuke_diff` (read 2026-10-11). It does a three-way merge of
Nuke Group templates and matches nodes **by name only**, so a rename looks like a delete plus
an add. Its lessons, which this design avoids:
- conflicts are stored in node labels and recovered by scanning label prefixes, which is
  fragile and overwrites the artist's labels;
- "resolve as theirs" swaps in the whole node and drops the artist's non-conflicting
  overrides;
- curve and roto knobs conflict as one opaque string;
- a move on both sides is a full node conflict;
- inline-insert detection follows input 0 only;
- nested groups are not recursed;
- templates work only inside a Group.

## Design (applies to every task)

- **Template provenance.** `Project::importTemplate(path, parentGroup)` loads a template
  bundle and mints fresh node IDs, so the same template can be imported twice. It records a
  template entry: `{key, path, content hash, imported-at}`. A copy of the imported snapshot is
  stored as the bundle member `templates/<key>/base.yaml`, so the merge base survives the
  published file being overwritten. Each imported node carries `origin: {template: <key>,
  node: <template node id>}`. The spike may amend these rules.
- **Merge scope:** the nodes whose `origin` names the template, plus artist-added nodes per
  the scope rule the spike settles (for example, nodes wired strictly between scoped nodes).
  Everything else in the script is untouched.
- **Matching:** a `NodeMatcher` gives base↔mine and base↔theirs correspondences. Policy is
  set at P1.T4.
- **Three-way core.** Node existence follows a table (added, deleted, delete-vs-modify, both
  added). For matched nodes, merge per knob and per dimension, and per input label. Delete
  versus modify is a conflict that keeps the node by default. UI properties (position,
  colour, size) **never conflict**: theirs applies unless mine changed it, in which case mine
  wins.
- **Built-in merge resolution.** A `MergeHandler` registry, keyed by knob kind and by plugin ID:
  - **knob kinds:**
    - curves merge per keyframe and conflict only when both sides changed the same key time differently;
    - strings and text merge line-based (diff3);
    - expressions and links conflict as a whole.
  - **node handlers:**
    - Roto merges per item (by item name, through layers);
    - Tracker merges per track;
    - Group and PyPlug recurse into their children.

    OFX nodes use the generic handlers. Native nodes may register their own.
- **Both sides insert on the same base edge** (any input slot): a structural conflict whose
  default chain order is theirs then mine, recorded as a conflict.
- **Conflicts are data, never labels.** A conflict is `{node id, knob?, dim?, kind, base, mine,
  theirs, handler options}`. The merged snapshot takes **mine** at every conflict, so the
  artist's work is the default. Conflicts are saved as the bundle member
  `merge/conflicts.yaml`, so a conflicted file opens normally and is resolved later in
  M97 - Merge Resolution UI.
- **CLI:** a new `natron-merge` executable that links only `NatronSerialization`:
  - `diff a b` prints a human-readable diff, and `--yaml` prints the operations from M95;
  - `merge base mine theirs -o out` exits 0 when clean and 1 when there are conflicts;
  - `git-driver %O %A %B` follows git's merge-driver contract;
  - `textconv f` prints `project.yaml` for `git diff`.

## Phase 96.1: Identity and provenance spike (stops for the user)

- [ ] M96.P1.T1 — Scenario corpus of template merges
  - files: new `Tests/fixtures/merge-scenarios/build_scenarios.py`, new `Tests/fixtures/merge-scenarios/README.md`, new `Tests/fixtures/merge-scenarios/expected/*.yaml`, generated `Tests/fixtures/merge-scenarios/*/{base,mine,theirs}.ntpz`
  - approach: a NatronRenderer Python script builds at least 12 base/mine/theirs triples:
    - a rename;
    - **an artist deletes a Grade and makes a new one in the same place** (the user's example);
    - delete and recreate as a different type;
    - the artist duplicates a template node;
    - the supervisor replaces a node;
    - both sides insert on the same edge, on input 0 and on a side input;
    - the template is imported twice;
    - template nodes pasted in from another shot;
    - the supervisor wraps nodes into a group;
    - a reconnect;
    - keyframe edits on both sides;
    - roto edits on both sides.

    Each has a hand-written `expected/<name>.yaml` giving the correct correspondences and the expected conflicts.
  - verify: the script builds every triple without error, and `unzip -l` lists `mimetype` first in each. The build is required (NatronRenderer must exist).
  - size: M

- [ ] M96.P1.T2 — Matcher strategies and a scoring harness
  - files: new `Serialization/Merge/NodeMatcher.h`, new `Serialization/Merge/NodeMatcher.cpp`, new `tools/merge/score_matchers.cpp` (or a gtest that prints the table), `Serialization/CMakeLists.txt`
  - approach: define the `NodeMatcher` interface, then implement three strategies:
    - `IdOnly`, which matches on `origin` and ID;
    - `IdPlusAdoption`, which also adopts an unmatched new node when it has the same plugin and the same matched upstream and downstream neighbours on the same input labels as an unmatched removed node. This covers the delete-and-recreate case;
    - `NameBased`, which mirrors nuke_diff.

    The harness scores each strategy against `expected/*.yaml` and prints correct, wrong and missing correspondences per scenario.
  - verify: the harness runs over the whole corpus and prints a table with one row per scenario and one column per strategy.
  - size: L

- [ ] M96.P1.T3 — Provenance and scope prototype, plus spike report
  - files: new `docs/design/graph-merge-identity.md`, the prototype code alongside `Serialization/Merge/NodeMatcher.cpp`
  - approach: prototype the merge-scope rule (provenance-tagged nodes plus artist nodes wired strictly between them, and alternatives) and score it on the corpus. Write the report: the score table from P1.T2, the scope results, the failure cases for each strategy, and a recommendation. Cover the explicit question of whether a deleted-and-recreated node should count as the same node, and when that goes wrong.
  - verify: the report exists, covers every scenario, and makes one recommendation.
  - size: M

- [ ] M96.P1.T4 — User decision on matching policy and scope
  - files: this file's `## Decisions`
  - approach: **stop for the user.** Present the report's table and recommendation, and ask the user to choose the matching policy, the scope rule and any provenance changes. Record the answer as a decision, then re-check P2's briefs against it (PLAN-FORMAT §5a) before starting P2.
  - verify: a user decision is recorded.
  - size: S

## Phase 96.2: Provenance and three-way core

- [ ] M96.P2.T1 — Template provenance in the data model, bundle and Engine
  - files: `Serialization/NodeSerialization.h`, `Serialization/ProjectSerialization.h`, `Serialization/Yaml/NodeYaml.cpp`, `Serialization/Bundle/ProjectBundle.cpp`, `Engine/Project.h/.cpp` (`importTemplate`)
  - approach: the provenance rules from **Design**, as amended at P1.T4. Add the template entry list and the `templates/<key>/base.yaml` members, and the node `origin` field. `importTemplate` mints IDs, sets `origin`, stores the base, and returns the created nodes.
  - verify: compiled at P2.T5. gtest: importing the same template twice gives distinct IDs that share the same `origin.node`; save → load keeps origins and the base member; the base is byte-identical to the imported file's `project.yaml` node subset.
  - size: M

- [ ] M96.P2.T2 — Production matcher and scope
  - files: `Serialization/Merge/NodeMatcher.h/.cpp`, new `Serialization/Merge/MergeScope.h/.cpp`, new `Tests/Serialization/NodeMatcher_Test.cpp`
  - approach: keep only the chosen strategy and scope rule, and delete the losing prototypes and the scoring tool unless the user asked to keep them. Matching is deterministic.
  - verify: compiled at P2.T5. Every corpus scenario's correspondences equal `expected/*.yaml`.
  - size: M

- [ ] M96.P2.T3 — Three-way node, knob and input merge with the handler registry
  - files: new `Serialization/Merge/ThreeWayMerge.h/.cpp`, new `Serialization/Merge/MergeHandler.h/.cpp`, new `Serialization/Merge/MergeConflict.h`, new `Tests/Serialization/ThreeWayMerge_Test.cpp`
  - approach: `merge(base, mine, theirs, scope) → {merged snapshot, conflicts}` for matched nodes: per knob, per dimension, per input label. Default knob values merge as values: a knob absent from the YAML is "default". The generic scalar handler conflicts only on mine ≠ theirs ≠ base. The registry dispatches by plugin ID, then by knob kind. A conflict takes mine.
  - verify: compiled at P2.T5. A table of scalar cases (one side changed, both changed the same way, both changed differently, reset to default on one side) gives the expected merged value and conflict list.
  - size: L

- [ ] M96.P2.T4 — Structural merge
  - files: new `Serialization/Merge/StructuralMerge.cpp`, `Serialization/Merge/ThreeWayMerge.cpp`, new `Tests/Serialization/StructuralMerge_Test.cpp`
  - approach: the node-existence table (added, deleted, delete-vs-modify kept with a conflict, both added). Both inserting on the same base edge on any input slot gives the theirs-then-mine chain plus a structural conflict. Groups recurse. UI properties use the never-conflict rule. Renames follow the matcher, so a rename on one side applies cleanly.
  - verify: compiled at P2.T5. Each structural corpus scenario gives the expected merged graph and conflicts.
  - size: L

- [ ] M96.P2.T5 — Phase 96.2 build and test checkpoint
  - files: none
  - approach: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug`, then `tools/ci/local/test.sh ctest debug`.
  - verify: green. The build is required.
  - size: S

## Phase 96.3: Built-in handlers and conflict persistence

- [ ] M96.P3.T1 — Curve and text merge handlers
  - files: new `Serialization/Merge/CurveMerge.cpp`, new `Serialization/Merge/TextMerge.cpp`, new `Tests/Serialization/CurveTextMerge_Test.cpp`
  - approach: curves are merged as maps keyed by key time. A key changed or added on one side applies. Keys at the same time changed differently conflict, per key. An animated-versus-static change conflicts as a whole. Text uses a line-based diff3 in-house (no dependency), and overlapping hunks conflict.
  - verify: compiled at P3.T5. Keyframes added on both sides at different times merge clean; the same key changed differently gives one per-key conflict; non-overlapping text edits merge, and overlapping ones conflict.
  - size: M

- [ ] M96.P3.T2 — Roto per-item merge handler
  - files: new `Serialization/Merge/RotoMerge.cpp`, new `Tests/Serialization/RotoMerge_Test.cpp`
  - approach: match items by name within their layer path. Items added or deleted on one side apply. For items changed on both sides, item knobs merge per knob through the generic and curve handlers. Shape control points changed on both sides conflict for the whole shape. Layer order follows theirs, plus mine's additions.
  - verify: compiled at P3.T5. The corpus roto scenario merges as expected; both sides adding different shapes keeps both; both editing one shape's points gives one conflict.
  - size: M

- [ ] M96.P3.T3 — Tracker per-track merge handler
  - files: new `Serialization/Merge/TrackerMerge.cpp`, new `Tests/Serialization/TrackerMerge_Test.cpp`
  - approach: match tracks by script name. Tracks added or deleted on one side apply. A track changed on both sides merges its knob curves per key through the curve handler.
  - verify: compiled at P3.T5. Different tracks added on each side are both kept, and the same track keyed at different frames merges clean.
  - size: M

- [ ] M96.P3.T4 — Persist conflicts in the bundle
  - files: new `Serialization/Yaml/MergeConflictYaml.cpp`, `Serialization/Bundle/ProjectBundle.cpp`, `Serialization/ProjectSerialization.h`, `Engine/Project.cpp`, new `Tests/Serialization/MergeConflictYaml_Test.cpp`
  - approach: write `merge/conflicts.yaml` when conflicts exist and drop the member when none remain. Base, mine and theirs values use the knob and curve encoders. Loading a project with conflicts adds the load warning "N unresolved merge conflicts". Engine keeps the list on `Project` for M97 - Merge Resolution UI.
  - verify: compiled at P3.T5. Conflicts round-trip; the member is absent when the list is empty; loading gives exactly one warning.
  - size: M

- [ ] M96.P3.T5 — Phase 96.3 build and test checkpoint
  - files: none
  - approach: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug`, then `tools/ci/local/test.sh ctest debug`.
  - verify: green, and the whole corpus matches `expected/`. The build is required.
  - size: S

## Phase 96.4: `natron-merge` CLI and git integration

- [ ] M96.P4.T1 — `natron-merge diff` and `merge`
  - files: new `MergeTool/CMakeLists.txt`, new `MergeTool/main.cpp`, new `MergeTool/DiffPrinter.cpp`, `CMakeLists.txt` (`add_subdirectory(MergeTool)`), the install rules
  - approach: subcommands as in **Design**. The human-readable diff groups changes per node (`~ Grade1 (renamed from Grade3)`, `  white: 1 → 1.2`, `+ Blur2`, `- Crop1`, `  input Source: Read1 → Blur2`). `merge` takes `--scope template:<key>|all`. Exit codes: 0 clean, 1 conflicts, 2 error. Link only `NatronSerialization`.
  - verify: compiled at P4.T3. A ctest runs `natron-merge` on three corpus triples, checks the exit codes, and checks that its `diff` output contains the expected lines.
  - size: M

- [ ] M96.P4.T2 — git merge driver and textconv
  - files: `MergeTool/main.cpp`, new `docs/graph-merge.md`, new `Tests/merge/git_driver_test.sh`, `Tests/CMakeLists.txt`
  - approach: `git-driver %O %A %B` merges into `%A` with scope `all` and exits nonzero on conflict. `textconv` prints `project.yaml`, with node maps in sorted order. The doc gives the `.gitattributes` and `git config` setup and describes the template workflow.
  - verify: compiled at P4.T3. The shell test makes a temp git repo with the driver configured, commits a base, branches, edits both sides with corpus files, and runs `git merge`: a clean scenario merges automatically and a conflicted one exits nonzero with `merge/conflicts.yaml` present.
  - size: M

- [ ] M96.P4.T3 — Final build, full ctest and smoke
  - files: none
  - approach: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug`, then `tools/ci/local/test.sh ctest debug` and `smoke debug`.
  - verify: everything green. The build is required.
  - size: S

**Verification gate:** the user's matching decision is recorded (P1.T4). Every corpus scenario
merges to its expected graph and conflicts, both through the API and through `natron-merge`.
The git-driver test passes. Full ctest, smoke and CI are green.
`natron-merge` links no Engine or Gui library (`ldd`/`readelf` check).

## Decisions

- 2026-10-11 — **User decision:** a template is "just a bunch of nodes in a script" that lives in a file on disk; merge works on the node set that came from template T.
- 2026-10-11 — **User decision:** the merge is available in the app and on the CLI, and the CLI doubles as a git merge driver.
- 2026-10-11 — **User decision:** the matching policy waits for the P1 spike; the user chooses at P1.T4.
- 2026-10-11 — Conflicts are stored as data in `merge/conflicts.yaml`, never in labels (a nuke_diff lesson), and the merged result defaults to mine at every conflict.
````

### 2026-10-11T00:43:00-04:00 — change-request
- refs: M97 (new milestone)
- New milestone **M97 - Merge Resolution UI**, file `PLAN/MILESTONES/M97-merge-resolution-ui.md`, board row `| M97 | Merge Resolution UI | todo | M96 | [M97-merge-resolution-ui.md](PLAN/MILESTONES/M97-merge-resolution-ui.md) |`. Full file content:

````markdown
# M97 - Merge Resolution UI

The in-app side of M96 - Graph Diff And Merge: importing templates, **Update from Template**,
a visual diff overlay on the node graph, and a merge view that resolves conflicts per knob.
Depends on M96 - Graph Diff And Merge. Freshness-check the briefs (PLAN-FORMAT §5a) against
M96's P1.T4 decision before starting.

## Design (applies to every task)

- **Import Template…** (File menu) calls `Project::importTemplate` from M96 - Graph Diff And
  Merge. **Update from Template…** is in the File menu and in the node-graph context menu on any
  template-tagged node. It finds the template's stored path, asking for the file if the path
  is gone, and merges with base = the stored snapshot, mine = the live scope, and theirs = the
  current file. The result is applied to the live graph as **one undo step** (M95's
  `diffSnapshots` → `GraphOpApply`). The stored base is then updated to theirs, and the merge
  view opens if there are conflicts.
- **Merge view:** a dock panel listing conflicts grouped by node. Selecting one frames the node
  and opens its panel. Each conflict offers Mine, Theirs and Base, plus any options its handler
  offers (for example "keep both keys"), and there are resolve-all-mine and resolve-all-theirs
  buttons. Each resolution is an undoable knob edit. Conflicted nodes carry a **badge glyph** in
  the graph, and their labels are never touched. Opening a file that holds
  `merge/conflicts.yaml` opens the view.
- **Diff overlay:** "Compare with File…" and "Compare with Template Base" tint the graph.
  Added nodes are green, changed ones amber, and removed ones are drawn as translucent ghost
  nodes at their old positions. Selecting a changed node shows a knob diff (base → current) in
  a side panel. The overlay is view-only, and Escape clears it.

## Phase 97.1: Engine and undo glue

- [ ] M97.P1.T1 — Project-level update-from-template and compare
  - files: new `Engine/ProjectMerge.h`, new `Engine/ProjectMerge.cpp`, `Engine/Project.h`, `Engine/CMakeLists.txt`, new `Tests/ProjectMerge_Test.cpp`
  - approach: `ProjectMerge::updateFromTemplate(project, templateKey, newPath)` snapshots the live scope, runs M96's merge, computes operations against the live state, applies them through a caller-supplied applier (so Gui can wrap it in undo), updates the stored base, and returns the conflicts. `ProjectMerge::compare(project, otherSnapshot)` returns a per-node change summary for the overlay.
  - verify: compiled at P1.T3. Headless gtests on two corpus scenarios: a clean update changes only the template nodes and keeps the artist's overrides; a conflicted one returns its conflicts and leaves mine in the graph.
  - size: M

- [ ] M97.P1.T2 — Undoable apply of an operation batch
  - files: `Gui/NodeGraphUndoRedo.h`, `Gui/NodeGraphUndoRedo.cpp`
  - approach: `ApplyGraphOpsCommand` holds forward operations and inverse operations (`diffSnapshots(after, before)` over the touched scope). redo and undo apply them through `GraphOpApply` and refresh the NodeGuis (create and remove).
  - verify: compiled at P1.T3. Exercised by the P4.T1 GUI test: undo after Update from Template restores the pre-merge graph exactly.
  - size: M

- [ ] M97.P1.T3 — Phase 97.1 build and test checkpoint
  - files: none
  - approach: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug`, then `tools/ci/local/test.sh ctest debug`.
  - verify: green. The build is required.
  - size: S

## Phase 97.2: Merge view and actions

- [ ] M97.P2.T1 — Merge conflict panel
  - files: new `Gui/MergeConflictPanel.h`, new `Gui/MergeConflictPanel.cpp`, `Gui/CMakeLists.txt`, the Gui pane registration (`Gui/Gui20.cpp` or wherever panes are registered)
  - approach: a dock widget backed by `Project`'s conflict list. Conflicts are grouped by node, with a count in the title. Selecting a row frames the node and opens its settings panel. It refreshes when the list changes.
  - verify: compiled at P2.T5. The P4.T1 GUI test opens a conflicted file and sees the panel with the right count.
  - size: M

- [ ] M97.P2.T2 — Per-conflict resolution actions
  - files: `Gui/MergeConflictPanel.cpp`, new `Gui/MergeConflictResolve.cpp`, `Engine/ProjectMerge.cpp`
  - approach: Mine, Theirs and Base buttons, and handler options from the conflict's `handler options`. A resolution sets the knob, curve, input or structure through undoable commands and removes the conflict. Resolve-all applies to every conflict in the list or in the selected node. Undoing a resolution restores the conflict.
  - verify: compiled at P2.T5. A GUI test resolves one conflict each way, then undoes, then resolves all.
  - size: M

- [ ] M97.P2.T3 — Conflict badge on nodes
  - files: `Gui/NodeGui.h`, `Gui/NodeGui.cpp`
  - approach: a small warning glyph at the node's top-right corner while the node has unresolved conflicts, in the same style as the existing node-graph glyphs. It is not a label edit, and it updates when the conflict list changes.
  - verify: compiled at P2.T5. A GUI test sees the badge appear on a conflicted node and go away when it is resolved.
  - size: S

- [ ] M97.P2.T4 — Import, update and compare menu actions
  - files: `Gui/Gui20.cpp` (menus), `Gui/NodeGraph35.cpp` or the file that builds the node-graph context menu, `Gui/GuiAppInstance.cpp`
  - approach: File > Import Template…, File > Update from Template… (a list of the project's templates), File > Compare with File…, and a node-graph context menu with "Update from Template" and "Compare with Template Base" on template-tagged nodes. Update applies through `ApplyGraphOpsCommand` and opens the panel when there are conflicts.
  - verify: compiled at P2.T5, and exercised by the P4.T1 GUI test.
  - size: M

- [ ] M97.P2.T5 — Phase 97.2 build and test checkpoint
  - files: none
  - approach: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug`, then `tools/ci/local/test.sh ctest debug`.
  - verify: green. The build is required.
  - size: S

## Phase 97.3: Diff overlay

- [ ] M97.P3.T1 — Node-graph diff overlay
  - files: new `Gui/NodeGraphDiffOverlay.h`, new `Gui/NodeGraphDiffOverlay.cpp`, `Gui/NodeGraph.h`, `Gui/NodeGraph.cpp`
  - approach: given `ProjectMerge::compare`'s summary, tint added, changed and removed nodes. Draw ghost items for removed nodes, with their wires to surviving neighbours drawn dashed. The overlay is view-only: ghosts are not selectable and not serialized. Escape or "Clear Comparison" removes it.
  - verify: compiled at P3.T3. A GUI test compares against a corpus base and checks the tint counts and ghost count.
  - size: M

- [ ] M97.P3.T2 — Knob diff side panel
  - files: new `Gui/NodeDiffPanel.h`, new `Gui/NodeDiffPanel.cpp`
  - approach: with the overlay active, selecting a changed node lists each changed knob as `name: base → current`. Curves show key-count and key-time changes, and inputs show their source change.
  - verify: compiled at P3.T3. A GUI test selects a changed node and reads the expected rows.
  - size: M

- [ ] M97.P3.T3 — Phase 97.3 build and test checkpoint
  - files: none
  - approach: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug`, then `tools/ci/local/test.sh ctest debug`.
  - verify: green. The build is required.
  - size: S

## Phase 97.4: Close-out

- [ ] M97.P4.T1 — GUI test of the template workflow
  - files: new `Tests/gui/m97_template_merge.py`
  - approach: on the `Tests/gui/guitest.py` harness:
    1. import a template;
    2. edit it as the artist;
    3. replace the template file with v2;
    4. run Update from Template on a clean scenario and check the graph;
    5. undo, and check the pre-merge graph;
    6. run a conflicted scenario and resolve through the panel;
    7. save, reopen, and check that the conflicts persisted when left unresolved.
  - verify: passes under `Tests/gui/run-gui-test.sh`. The build is required.
  - size: M

- [ ] M97.P4.T2 — AppImage UAT
  - files: `build/appimages/M97-<sha>.AppImage`, `build/appimages/M97-uat.md`
  - approach: build with `tools/ci/local/package.sh` and launch-check it. The checklist: the supervisor-and-artist workflow end to end on a realistic template (Read → Grade → ColorCorrect → Roto-masked Merge → Write), the compare overlay, resolving in the panel, and `natron-merge` on the same files from a shell.
  - verify: the user signs off. **Stop for the user.**
  - size: M

**Verification gate:** full ctest, smoke, `m97_template_merge.py` and CI green. Update from
Template is a single undo step. Conflicts survive save and reopen. The user signs off the UAT.
````

### 2026-10-11T00:44:00-04:00 — change-request
- refs: M98 (new milestone)
- New milestone **M98 - Live Session Core**, file `PLAN/MILESTONES/M98-live-session-core.md`, board row `| M98 | Live Session Core | todo | M95 | [M98-live-session-core.md](PLAN/MILESTONES/M98-live-session-core.md) |`. Full file content:

````markdown
# M98 - Live Session Core

Live multi-user editing: one Natron session **hosts** a project, and others **join** and see
every graph, knob and position edit live, like a shared Google Doc. Each peer renders locally
with its own cache. Only the script is shared. Depends on M95 - Graph Edit Operations
(recording, encoding and applying edits). Presence, per-user undo and viewer following are
M99 - Live Session Presence And Undo.

Scouting facts (2026-10-11): `Qt::Network` is already linked into Engine, Gui and Renderer
(`CMakeLists.txt:69-71`, `QT_COMPONENTS`). Qt WebSockets is not linked, and nothing uses
`QTcpSocket` yet. The existing IPC is `ProcessHandler` (`QLocalServer`, `Engine/ProcessHandler.h:83-253`).

## Design (applies to every task)

- **Host-authoritative (user decision).** The host's GUI process runs a `QTcpServer`. Frames
  are a u32 big-endian length followed by one YAML document. Message types:
  - `hello{protocol, name, natron version}` and `welcome{userId, colour, seq, snapshot}`;
  - `ops{clientSeq, ops}` and `broadcast{seq, originUser, clientSeq, ops}`;
  - `reject{clientSeq, reason}` and `resyncNode{nodeId, NodeSerialization|deleted}`;
  - `checksum{seq, hash}`, `saveRequest` and `bye`.

  No new dependency. A protocol-version mismatch refuses the join with a clear message.
- **Join:** the host sends its full in-memory bundle (`.ntpz` bytes, including `gui.yaml`) and
  its current seq. The client loads it as an unsaved, session-bound project, titled
  `Live: <host project> @ <host>`.
- **Edits.** Recorded local operations go to the host, tagged with a client seq. The host
  assigns a global seq, applies the operations if they came from a client, and broadcasts them
  to everyone, the originator included as its ack. The client applies its own edits
  optimistically. When a broadcast arrives from another user and touches a key (node+knob,
  node+input, node existence, node UI) that the client still has pending, the client skips it:
  the host will apply the client's operation afterwards, so last-writer-wins converges. When an
  operation's target no longer exists on the host, the host sends `reject` plus `resyncNode` to
  the originator.
- **Divergence check:** every 5 s the host broadcasts the hash of the canonical `project.yaml`
  at a seq. A client compares at the same seq and requests a full resync on a mismatch, logging it.
- **Saving.** Only the host writes the file, and autosave runs only on the host. Ctrl+S on a
  client sends `saveRequest`. Save As on a client writes a private copy and leaves the session.
- **Disconnect.** A client that loses the host keeps an unsaved local copy, is told, and can
  save it. When the host closes the session, every client gets `bye`.
- Remote operations are applied under `Suppress` and are **never pushed onto local undo
  stacks**.
- **No password in v1 (user decision).** The host dialog picks a bind address and port and
  warns that anyone who can reach the port can join.

## Phase 98.1: Protocol, host and client (headless)

- [ ] M98.P1.T1 — Session protocol messages and framing
  - files: new `Serialization/Session/SessionProtocol.h`, new `Serialization/Session/SessionProtocol.cpp`, `Serialization/CMakeLists.txt`, new `Tests/Serialization/SessionProtocol_Test.cpp`
  - approach: message structs and their YAML encoding (operations through M95's `GraphOpYaml`). The framing codec is incremental: it takes bytes and yields complete messages, and rejects frames over a 512 MiB cap.
  - verify: compiled at P1.T5. Every message type round-trips; split and coalesced byte streams decode identically; an oversize or garbage frame gives an error and no crash.
  - size: M

- [ ] M98.P1.T2 — Session host
  - files: new `Engine/LiveSession/SessionHost.h`, new `Engine/LiveSession/SessionHost.cpp`, `Engine/CMakeLists.txt`
  - approach: a `QTcpServer` that handles the join handshake (snapshot from the live project through the bundle writer, in memory), sequencing, apply-then-broadcast (applying through `GraphOpApply` under `Suppress`), rejects with `resyncNode`, checksum broadcast, and disconnect cleanup. It also subscribes to the host's own recorder, so the host user's edits are sequenced the same way.
  - verify: compiled at P1.T5, and exercised by P1.T4.
  - size: L

- [ ] M98.P1.T3 — Session client
  - files: new `Engine/LiveSession/SessionClient.h`, new `Engine/LiveSession/SessionClient.cpp`
  - approach: connect and hello; load the welcome snapshot; send local recorded operations with a client seq while keeping a pending-key set; apply broadcasts using the skip rule in **Design**; clear pending entries on ack; handle reject and `resyncNode`; compare checksums and request a full resync on a mismatch.
  - verify: compiled at P1.T5, and exercised by P1.T4.
  - size: L

- [ ] M98.P1.T4 — Headless convergence test
  - files: new `Tests/LiveSessionConvergence_Test.cpp`
  - approach: on loopback, start one host and three clients, each with its own `AppInstance` in the test process. If one process cannot hold several apps, run the clients as `NatronRenderer` subprocesses driven by a test script instead. Each peer makes 200 random edits concurrently from a fixed seed: knob sets, creates, deletes, rewires, renames, and same-knob races between peers. Then quiesce and require every peer's canonical `project.yaml` to be byte-identical to the host's. A dedicated case covers delete-versus-edit, where the reject and resync converge.
  - verify: compiled at P1.T5, where it passes 20 seeds in a row.
  - size: L

- [ ] M98.P1.T5 — Phase 98.1 build and test checkpoint
  - files: none
  - approach: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug`, then `tools/ci/local/test.sh ctest debug`.
  - verify: green. The build is required.
  - size: S

## Phase 98.2: App integration

- [ ] M98.P2.T1 — Session-bound project lifecycle
  - files: `Engine/Project.h`, `Engine/Project.cpp`, `Engine/AppInstance.h`, `Engine/AppInstance.cpp`
  - approach: load a project from in-memory bundle bytes. Add a session-bound flag that changes the title and makes save route to `saveRequest` on clients and skip autosave. On disconnect, leave a plain unsaved project with the message from **Design**. Save As on a client leaves the session.
  - verify: compiled at P2.T5. gtests: loading from bytes equals loading from a file; a client save sends `saveRequest` and the host writes the file; after a disconnect, the client project is unsaved and editable.
  - size: M

- [ ] M98.P2.T2 — Host and Join dialogs and the status indicator
  - files: new `Gui/LiveSessionDialogs.h`, new `Gui/LiveSessionDialogs.cpp`, `Gui/Gui20.cpp` (File menu: Host Live Session…, Join Live Session…, Leave Session), `Gui/CMakeLists.txt`
  - approach: the host dialog takes a bind address, port and display name, and shows the security warning. The join dialog takes host, port and display name, with recent hosts remembered in settings. A status-bar indicator shows the session state and the number of peers connected.
  - verify: compiled at P2.T5, and exercised by P2.T3.
  - size: M

- [ ] M98.P2.T3 — Two-process GUI test of interactive edits
  - files: new `Tests/gui/m98_live_session.py`, `Tests/gui/run-gui-test.sh` (if it needs a two-process mode)
  - approach: start a host and a client Natron on loopback. On the client, drag a knob slider, set a keyframe, move nodes, create and connect a node, draw a roto stroke, and add a user knob. On the host, rename and delete a node. After each step, both sides' canonical `project.yaml` must match. Any GUI edit path that misses the recorder is a bug to fix in M95's hooks, filed as a new task in this phase.
  - verify: passes under the GUI harness. The build is required.
  - size: M

- [ ] M98.P2.T4 — Phase 98.2 build, full ctest and smoke
  - files: none
  - approach: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug`, then `tools/ci/local/test.sh ctest debug` and `smoke debug`.
  - verify: everything green. The build is required.
  - size: S

## Phase 98.3: Close-out

- [ ] M98.P3.T1 — AppImage UAT across two machines
  - files: `build/appimages/M98-<sha>.AppImage`, `build/appimages/M98-uat.md`
  - approach: build with `tools/ci/local/package.sh` and launch-check it. The checklist:
    - host a bench comp on one machine and join from a second (two processes on one host if no second machine);
    - both edit the same comp at once: knobs, nodes, roto, rewiring;
    - the same knob dragged on both sides converges;
    - each side renders on its own;
    - a client save writes the host's file;
    - pulling the network cable on the client leaves a savable local copy.
  - verify: the user signs off. **Stop for the user.**
  - size: M

**Verification gate:** `LiveSessionConvergence_Test` passes 20 seeds; `m98_live_session.py`,
full ctest, smoke and CI are green; the user signs off the UAT.

## Decisions

- 2026-10-11 — **User decision:** host-authoritative with last-writer-wins, peers connected, no offline editing (CRDTs and node locks rejected).
- 2026-10-11 — **User decision:** no session password in v1; the host dialog warns instead.
- 2026-10-11 — Transport: plain TCP with length-prefixed YAML frames over the already-linked `Qt::Network`. This avoids adding Qt WebSockets, and reuses M95's operation encoding.
- 2026-10-11 — Only the host writes the shared file; clients' Save sends a request to the host.
````

### 2026-10-11T00:45:00-04:00 — change-request
- refs: M99 (new milestone)
- New milestone **M99 - Live Session Presence And Undo**, file `PLAN/MILESTONES/M99-live-session-presence-and-undo.md`, board row `| M99 | Live Session Presence And Undo | todo | M98 | [M99-live-session-presence-and-undo.md](PLAN/MILESTONES/M99-live-session-presence-and-undo.md) |`. Full file content:

````markdown
# M99 - Live Session Presence And Undo

Finishes the user's v1 scope for live sessions on top of M98 - Live Session Core: **presence**
(who is connected, and each user's selection and open panels in their colour), **per-user undo**
(Ctrl+Z undoes only your own edits), and **follow a user's viewer**. Depends on M98 - Live
Session Core.

## Design (applies to every task)

- **Presence messages** are separate from operations and are never recorded or saved.
  `presence{userId, selection: [nodeId], openPanels: [nodeId], viewer?: {...}}` is throttled to
  10 Hz and relayed by the host. Each user has a stable colour assigned at welcome.
- **Per-user undo.** Each peer's local undo stack holds only its own commands, because remote
  operations are never pushed to it (M98 - Live Session Core). Undoing a local command changes
  the graph, and the recorder sends the change as new operations, so other users see the undo
  as an ordinary edit. When a remote operation deletes a node that local commands refer to,
  those commands are made obsolete (`QUndoCommand::setObsolete`) so undo skips them. A local
  undo of a knob edit restores the user's own old value even if someone else changed the knob
  since. That is accepted under last-writer-wins, and the status bar notes it.
- **Follow viewer:** a follower mirrors the leader's viewer frame, active viewer input node,
  zoom and pan, and channels. Following stops on any local viewer interaction. The leader's
  state is sent in presence.

## Phase 99.1: Presence

- [ ] M99.P1.T1 — Presence messages and relay
  - files: `Serialization/Session/SessionProtocol.h/.cpp`, `Engine/LiveSession/SessionHost.cpp`, `Engine/LiveSession/SessionClient.cpp`, `Tests/Serialization/SessionProtocol_Test.cpp`
  - approach: add the `presence` message, the 10 Hz client throttle, host relay, and a per-user presence table on each client that emits a Qt signal when it changes. A user's entry is cleared on disconnect.
  - verify: compiled at P1.T4. Protocol round-trip; in a loopback test, client A's selection reaches client B within 200 ms and is cleared when A disconnects.
  - size: M

- [ ] M99.P1.T2 — Remote selection and open panels in the node graph
  - files: `Gui/NodeGui.h`, `Gui/NodeGui.cpp`, `Gui/NodeGraph.cpp`, `Gui/DockablePanel.cpp`
  - approach: a node selected by a remote user gets a coloured outline in that user's colour, plus a small name tag; several users stack their outlines. A node whose panel a remote user has open shows a coloured dot in its settings-panel header. Local selection and open panels feed the presence sender.
  - verify: compiled at P1.T4. The P3.T3 GUI test checks the outline and dot on B after A selects a node and opens its panel.
  - size: M

- [ ] M99.P1.T3 — Connected-users panel
  - files: new `Gui/LiveSessionUsersPanel.h`, new `Gui/LiveSessionUsersPanel.cpp`, `Gui/CMakeLists.txt`, the Gui pane registration
  - approach: a dock listing users with their colour swatch and name, a "you" marker, the host marker, and a Follow toggle used by P3. Double-clicking a user frames their selection.
  - verify: compiled at P1.T4, and exercised by the P3.T3 GUI test.
  - size: M

- [ ] M99.P1.T4 — Phase 99.1 build and test checkpoint
  - files: none
  - approach: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug`, then `tools/ci/local/test.sh ctest debug`.
  - verify: green. The build is required.
  - size: S

## Phase 99.2: Per-user undo

- [ ] M99.P2.T1 — Make local undo safe under remote edits
  - files: `Gui/NodeGraphUndoRedo.h`, `Gui/NodeGraphUndoRedo.cpp`, `Gui/KnobUndoCommand.h`, `Gui/KnobUndoCommand.cpp`, `Engine/LiveSession/SessionClient.cpp`
  - approach: commands record the node IDs they touch. When a remote `DeleteNode` arrives, every local command touching that ID is marked obsolete, and the stack drops it. Commands keep their nodes by ID, not by `NodeGuiPtr`, wherever the stale pointer would otherwise crash. Remote applies never call `pushUndoCommand`, which is asserted in debug builds. An undo whose target changed remotely restores the local old value and posts the status-bar note.
  - verify: compiled at P2.T2, and exercised by P2.T2.
  - size: L

- [ ] M99.P2.T2 — Per-user undo GUI test
  - files: new `Tests/gui/m99_live_undo.py`
  - approach: two processes, A and B:
    - A edits knob k1 and B edits k2; A undoes, and only k1 reverts on both sides;
    - A creates a node and B moves it; A undoes the create, and the node is gone on both sides;
    - B deletes a node A edited; A undoes, and the obsolete command is skipped without a crash;
    - redo is checked for each case.
  - verify: passes under the GUI harness. The build is required.
  - size: M

## Phase 99.3: Follow a user's viewer

- [ ] M99.P3.T1 — Viewer state in presence
  - files: `Serialization/Session/SessionProtocol.h/.cpp`, `Gui/ViewerTab.cpp`, `Gui/ViewerGL.cpp`
  - approach: viewer state is the current frame, the active viewer node ID with its input index, zoom and pan, and the displayed channels. It is captured on change and sent in presence, throttled. A follower applies it without recording it and without moving its own timeline cursor when it is not following.
  - verify: compiled at P3.T3, and exercised by P3.T3.
  - size: M

- [ ] M99.P3.T2 — Follow toggle and auto-stop
  - files: `Gui/LiveSessionUsersPanel.cpp`, `Gui/ViewerTab.cpp`
  - approach: Follow on a user mirrors their viewer state, and only one leader can be followed at a time. Local scrubbing, zoom, pan or a viewer-input change stops following, with a status-bar note. The leader's name is shown in the viewer corner while following.
  - verify: compiled at P3.T3, and exercised by P3.T3.
  - size: M

- [ ] M99.P3.T3 — Presence and follow GUI test, full ctest and smoke
  - files: new `Tests/gui/m99_presence_follow.py`
  - approach: two processes. A selects a node and opens its panel, and B sees the outline and dot. B follows A, A scrubs to frame 20 and zooms, and B matches. B scrubs, and following stops. Then build and run `tools/ci/local/test.sh ctest debug` and `smoke debug`.
  - verify: everything green. The build is required.
  - size: M

## Phase 99.4: Close-out

- [ ] M99.P4.T1 — AppImage UAT
  - files: `build/appimages/M99-<sha>.AppImage`, `build/appimages/M99-uat.md`
  - approach: build with `tools/ci/local/package.sh` and launch-check it. The checklist covers two or three users on one comp: presence colours, the users panel, per-user undo across the cases in P2.T2, and following a viewer during playback.
  - verify: the user signs off. **Stop for the user.**
  - size: M

**Verification gate:** `m99_live_undo.py` and `m99_presence_follow.py` pass, full ctest, smoke
and CI are green, and the user signs off the UAT.

## Decisions

- 2026-10-11 — **User decision:** v1 includes presence, per-user undo, and follow-a-user's-viewer.
- 2026-10-11 — Per-user undo replays the user's own inverse as new operations (last-writer-wins). Commands invalidated by remote deletes are dropped, not transformed.
````

### 2026-10-11T22:51:00-04:00 — change-request
- refs: M100 (new milestone)
- New milestone **M100 - Content Node Hashing**, user-requested on 2026-10-11. Suggested board row, after M93 - Serialization Boundary: `| M100 | Content Node Hashing | todo | M93 | [M100-content-node-hashing.md](PLAN/MILESTONES/M100-content-node-hashing.md) |`.
- **Why:** today `Node::computeHashInternal` (`Engine/Node.cpp:762`) does not hash content. It hashes a `knobsAge` counter, the input hashes, the script name and the project creation time. The user rejected this ("not a reasonable way to do it"). A node's hash must be a content (Merkle) hash: the plugin type, render-affecting knob values, input hashes by index, and the identity of every file read upstream. Any change to a file, the topology or a knob must then change the hash, and identical state must give an identical hash.
- **Suggested project-wide decision** `DECISIONS/2026-10-11-content-node-hashing.md` (+ INDEX line): "Node hashes become content hashes (plugin identity, render-hashed knob values incl. full curves/expressions, inputs by index, file stamps, project digest), replacing knobsAge. Cache entries are evicted LRU, not purged on hash change. User decision 2026-10-11."
- **Collision to fix separately:** the pending live-session entries above claim M95–M99. M95 and M96 already exist on the board (M95 - Knob Cleanup And Keyer Redesign, M96 - Weighted Curve Tangents). The user has not yet chosen new IDs for them. Do not apply them under the colliding IDs; raise it as an open question.
- **Design input:** an Opus investigation on 2026-10-11 read the code but did not build or reproduce anything. Its findings are in the file's Notes. Full file content:

````markdown
# M100 - Content Node Hashing

Replace the `knobsAge` counter hash with a content hash. Identical state gives an identical
hash, so undo and A/B toggling reuse the cache. Any change to a knob, the topology or an
upstream file changes the hash. The change lands in steps: shadow hash plus audit, then file
identity, then switching the cache keys, then deleting the age machinery. Depends on
M93 - Serialization Boundary, because Phases 100.4 and 100.5 touch `Node.cpp` load code and
`NodeSerialization`. M16 - Project Format Redesign deletes the serialized `KnobsAge` field,
as it already plans to.

## Notes (investigation 2026-10-11, code reading only; re-check line numbers)

**Current scheme:**
- `computeHashInternal` hashes the following (`Node.cpp:762-865`), via vector + boost CRC-64 (`Hash64.cpp:42`):
  - the node's age;
  - the IO container's age;
  - inputs as `hash + i`;
  - the script name;
  - the project creation time.
- When the hash changes, the old entries are purged (`:860`), except while a project is loading or closing (`:929`).
- `knobsAge` and `CreationDate` are saved in the project and restored on load (`Node.cpp:1679`, `ProjectPrivate.cpp:125`).
- The viewer and disk cache tables of contents persist across sessions (`Project.cpp:638`, `AppManager.cpp:501`).
- `ImageKey::operator==` ignores the holder (`ImageKey.cpp:82-98`). Cache lookup is by hash plus equality (`Cache.h:1702`), so entries are effectively shared already.

**Bugs in the current scheme.** Found by reading the code; none is reproduced yet, and the Phase 100.6 regressions must cover them:
1. **Stale images after reopen or autosave recovery.** An unsaved edit takes the age from 10 to 11, and its renders persist in the viewer and disk caches. On reopen the age is back at 10. A *different* edit reaches 11 again, which gives the same hash, so the stale pixels are served.
2. **Two copies of one project collide.** Save As keeps the creation date. Once the two copies diverge, equal ages give equal hashes for different content, in two windows or in later sessions.
3. **A file changed on disk stays stale.** No mtime reaches the hash; only Reload fixes it.
4. **Off-thread edits don't move the hash.** Knob edits made off the main thread, or during inputChanged, never bump the age (the `isMT` guard, `EffectInstance.cpp:3430`).
5. **Undo and A/B toggling discard valid cache.** Every return to an earlier state gets a new age.

Minor: inputs are combined with `+i`; `U32 newAge` truncates (`Node.cpp:1941`); paste and clone deliberately force different hashes (`Gui/NodeGraphPrivate10.cpp:249`).

## Design (applies to every task)

- **Node hash** = XXH3 over four parts. Keep `U64`, and never let the result be 0, because 0 means "not computed".
  1. **Identity:** plugin ID, plugin major and minor version, and a binary stamp (the `.ofx` path, mtime and size; for native nodes, the Natron version and git hash).
  2. **Render-hashed knobs**, sorted by script name, each as name plus content.
     - Content per view and dimension: the static value; the *whole* curve (keys, interpolation, tangents, extrapolation; weights after M96 - Weighted Curve Tangents); the expression string; parametric, table and string data.
     - Doubles are canonicalized: −0 becomes +0, and all NaNs become one value.
     - Each knob memoizes its hash behind a dirty bit.
     - Linked or slaved knobs hash the value they read through the link, which replaces the `knobsAgeChanged` slaving (`Node.cpp:5378`).
  3. **Inputs:** each index mixed with the input's hash, or a fixed "unconnected" marker. The viewer hashes its active inputs with their index. `getInput()` already applies group redirection.
  4. **Project digest:** formats, fps, frame range, OCIO config identity and working space, layer-registry content, and the render-affecting preferences.
- **Which knobs are render-hashed:** value knobs that are not hash-exempt and are persistent or evaluate on change. Erring towards including a knob is the safe direction, since the cost is only a cache miss.
  - Explicitly exempt: label, sublabel, preview, hide-inputs, info text and user colour.
  - Do NOT exempt a knob just because it doesn't evaluate on change: secret persistent OFX state params are often flagged that way.
- **What leaves the hash:** the script name and the creation time. One exception: a node with an expression also folds in its fully qualified name, because `thisNode` and `thisGroup` are resolved by name.
- **Expressions** fold in the content hashes of their dependency knobs (`getExpressionDependencies`, with a visited set). Tokens such as `random`, `time.`, `open(` and `os.` mark the knob volatile.
- **Volatile:** a per-session salt, reused within a session but never served from the disk cache in a later one. It applies to volatile expressions and to plugins on a "volatile" list (Text, which depends on fontconfig, etc.).
- **Roto and RotoPaint:** `RotoItem::appendContentHash`, memoized per item and dirtied wherever `incrementNodesAge()` is called today. It covers the item's knobs, control and feather points with their curves, stroke points, and the order of items. Each stroke's internal nodes fold in their own item's hash.
- **Read/Write containers:** the container's knobs are hashed into the embedded node. This replaces M58's age fold.
- **Files:**
  - A `FileStampRegistry` holds (path, statx mtime_ns, size, inode) with a short TTL.
  - Single files: `KnobFile`, the OCIO config, LUTs, fonts and DeepRead layers. A Write's output file is not stamped.
  - Sequences: a sequence stamp, the hash of every frame's stamp, taken from the existing directory scan. Rescan on inotify (`QFileSystemWatcher`), on app focus-in (NFS gets no inotify), and on Reload.
  - Per-render guard: stat each Read's current frame when render args are set up, and restart the render if the stamp moved. Keep this guard off the serial render path (M90 - Render Path Serial Overhead, M82 profiler).
- **Animation:** hash the whole curve. Time stays in `ImageKey` for frame-varying nodes, as today. Hashing the value at each frame is a later optimization that can ride the per-request hash of M71 - RFV Render Context.
- **Eviction:** no purge on hash change; LRU reclaims old entries. Explicit purges stay on node delete, Reload, Clear Cache and a cache-version change. Raise `ActionsCache`'s instance limit so toggling between two states keeps both.
- **Threading:** off-thread setters set an atomic dirty flag and post `mustComputeHashOnMainThread`. `getHashValue()` and the render-args snapshot recompute a dirty hash under the node lock. The main-thread assert goes away.
- **Persistence:** stop reading `KnobsAge` on load and write 0 until M16 - Project Format Redesign removes it. M93's golden harness already strips it. Bump `NATRON_CACHE_VERSION` when the keys switch over.
- **GPU vs CPU:** not part of the key. Their outputs differ by about a rounding step and already mix in the cache today.
- **Related milestones:**
  - M92 - Linked Nodes gets simpler if this lands first, since link groups hash their shared storage.
  - M19 - USD/Hydra Foundation's `stackHash` should reuse the knob hash.

## Phase 100.1: Hasher and knob content hash
- [ ] M100.P1.T1 — Vendor XXH3 and add a streaming ContentHasher
  - files: libs/xxhash/xxhash.h, libs/CMakeLists.txt, Engine/ContentHasher.h, Engine/ContentHasher.cpp, Tests/ContentHasher_Test.cpp
  - approach: header-only XXH3-64 behind ContentHasher (appendU64, appendDouble with canonicalization, length-prefixed appendString and appendBytes; finalize never returns 0). First check whether the container's OIIO `hash.h` exposes XXH3; if it does, use it and skip vendoring.
  - verify: `ctest -R ContentHasher` covers known vectors, -0/+0 and NaN canonicalization, and ("ab","c") ≠ ("a","bc")
  - size: M
- [ ] M100.P1.T2 — Add memoized KnobI::appendContentHash for every knob type
  - files: Engine/Knob.h, Engine/Knob.cpp, Engine/KnobImpl.h, Engine/KnobTypes.cpp, Engine/Curve.cpp
  - approach: for each view and dimension, hash the static value, the full curve and the expression string. Memoize behind a dirty bit that is cleared in onInternalValueChanged and on curve or expression changes. Linked dimensions hash through their master.
  - verify: Tests/KnobContentHash_Test.cpp: every kind of mutation changes the hash, reverting restores it, and a slaved knob hashes equal to its master
  - size: L
- [ ] M100.P1.T3 — Classify render-hashed knobs and exempt UI-only ones
  - files: Engine/Knob.h, Engine/Knob.cpp, Engine/Node.cpp, Engine/OfxParamInstance.cpp
  - approach: isRenderHashed() = value type && !hashExempt && (persistent || evaluateOnChange). Call setHashExempt on label, sublabel, preview, hide-inputs, info text and user colour.
  - verify: a test shows that editing the label or info text leaves the hash unchanged, and editing a secret persistent param changes it
  - size: M

## Phase 100.2: Node content hash in shadow
- [ ] M100.P2.T1 — Compute the Node content hash beside the age
  - files: Engine/Node.cpp, Engine/Node.h, Engine/NodePrivate.h, Engine/Project.cpp, Engine/Project.h
  - approach: identity + sorted hashed knobs + inputs by index (mixed, with an unconnected marker) + the project digest, stored as contentHash. getHashValue is unchanged.
  - verify: Tests/NodeContentHash_Test.cpp: two freshly built identical graphs hash equal; topology changes and input swaps produce different hashes
  - size: M
- [ ] M100.P2.T2 — Hash roto/paint items and IO containers
  - files: Engine/RotoItem.cpp, Engine/RotoDrawableItem.cpp, Engine/Bezier.cpp, Engine/RotoStrokeItem.cpp, Engine/Node.cpp
  - approach: a per-item memoized appendContentHash, dirtied wherever incrementNodesAge is called. Stroke internal nodes fold in their item's hash, and the container's knobs fold into the embedded IO node.
  - verify: moving one control point changes the hash and undo restores it; a micro-benchmark measures hash cost on a 5k-stroke comp
  - size: L
- [ ] M100.P2.T3 — Fold expression dependencies into knob hashes
  - files: Engine/Knob.cpp, Engine/Knob.h, Engine/Node.cpp
  - approach: fold in each dependency knob's content hash (visited set to stop cycles), the owner's fully qualified name when the node has expressions, and a session salt when a volatile token appears.
  - verify: a test shows that changing a referenced knob on a node that is not upstream changes the hash, and that an expression cycle terminates
  - size: M
- [ ] M100.P2.T4 — Add a NATRON_HASH_AUDIT shadow cross-check
  - files: Engine/Node.cpp, Engine/Node.h, Tests/CMakeLists.txt
  - approach: log once per knob whenever the age and content hashes disagree, in either direction. Run the full ctest with the audit on and record what it finds in `## Decisions`.
  - verify: an audit ctest run in which every discrepancy is explained or fixed
  - size: M

## Phase 100.3: File identity
- [ ] M100.P3.T1 — Add FileStampRegistry and single-file stamps
  - files: Engine/FileStampRegistry.h, Engine/FileStampRegistry.cpp, Engine/KnobFile.cpp, Engine/Project.cpp, Tests/FileStamp_Test.cpp
  - approach: statx stamps with a TTL memo. The hashes of KnobFile and the OCIO config include the stamp, which is re-validated at render start.
  - verify: touching the file changes the hash; an unchanged file keeps a stable hash
  - size: M
- [ ] M100.P3.T2 — Add sequence stamps with inotify and focus-in rescan
  - files: Engine/FileStampRegistry.cpp, the native Read node source (M75 - Native Read), the DeepRead source, Engine/PrecompNode.cpp, Gui/GuiApplicationManager.cpp
  - approach: hash all frame stamps from the existing directory scan. Watch the directory with QFileSystemWatcher, rescan on app focus-in, and force a rescan on Reload.
  - verify: rewriting one frame of a sequence changes the Read's hash; confirm the viewer re-renders in a GUI test or UAT
  - size: L
- [ ] M100.P3.T3 — Add a per-render current-frame stat guard
  - files: Engine/ParallelRenderArgs.cpp, Engine/FileStampRegistry.cpp, Engine/ViewerInstance.cpp
  - approach: stat each Read's frame file when render args are set up. On a mismatch, update the registry, recompute the hash, abort and restart the render.
  - verify: a test overwrites a frame between two renders and gets the new pixels; the M82 profiler shows no serial regression
  - size: M

## Phase 100.4: Switch keys and drop the purge
- [ ] M100.P4.T1 — Make getHashValue return the content hash and bump the cache version
  - files: Engine/Node.cpp, Global/GlobalDefines.h, Engine/Hash64.cpp, Tests/BaseTest.cpp
  - approach: remove the script name (except on nodes with expressions), the creation time and the age from the key. Set NATRON_CACHE_VERSION to 10.
  - verify: the existing hash tests pass; two identical nodes share one cache entry (a counting effect renders once)
  - size: M
- [ ] M100.P4.T2 — Replace purge-on-hash-change with LRU eviction
  - files: Engine/Node.cpp, Engine/EffectInstancePrivate.cpp, Tests/DeepRenderPipeline_Test.cpp, Tests/DeepImageCache_Test.cpp
  - approach: remove the purge in computeHashInternal and keep the delete, Reload and Clear Cache purges. Raise the ActionsCache instance limit. Rewrite the tests that assert a purge as tests that assert reuse.
  - verify: going value 1 → 2 → 1 causes zero re-renders on the third step
  - size: M
- [ ] M100.P4.T3 — Make the hash recompute lazy and thread-safe
  - files: Engine/Node.cpp, Engine/Node.h, Engine/EffectInstance.cpp, Engine/Knob.cpp
  - approach: an atomic dirty flag that any thread can set. getHashValue and the render snapshot recompute under the node lock. Remove the main-thread assert; edits during inputChanged mark the node dirty.
  - verify: a test sets a knob from a worker thread and the next render sees the new hash; run TSan if it is available
  - size: L

## Phase 100.5: Remove knobsAge
- [ ] M100.P5.T1 — Turn core age bumps into dirty marks and delete the age hacks
  - files: Engine/Node.cpp, Engine/NodeInputs.cpp, Engine/EffectInstance.cpp, Engine/Project.cpp, Engine/Node.h
  - approach: incrementKnobsAge becomes markHashDirty. Delete the knobsAgeChanged slaving, the group-input force, the group-disable loop, the format loop and the layer-user bump.
  - verify: full ctest, plus disabled-Group and GroupInput rewire tests
  - size: L
- [ ] M100.P5.T2 — Migrate the remaining age-bump callers
  - files: Engine/RotoDrawableItem.cpp, Engine/RotoStrokeItem.cpp, AddLayers and DeepAddLayers sources, Gui/NodeGraphPrivate10.cpp, Gui/MultiInstancePanel.cpp
  - approach: apply the same replacement, and delete the bump that forces paste and clone to hash differently.
  - verify: `grep incrementKnobsAge` finds nothing; ctest passes
  - size: M
- [ ] M100.P5.T3 — Stop reading KnobsAge on load
  - files: Engine/Node.cpp, Engine/NodeSerialization.cpp (or M93's plain struct)
  - approach: ignore the field on load and write 0; M16 - Project Format Redesign deletes it.
  - verify: the M93 golden harness is unchanged; load → save → load gives an identical hash
  - size: S

## Phase 100.6: Invariant tests and cost
- [ ] M100.P6.T1 — Add a hash-invariant and regression suite
  - files: Tests/HashInvariants_Test.cpp, Tests/CMakeLists.txt
  - approach: the same state gives the same hash (fresh build, save/load, a second app instance); every edit changes it; undo restores it; topology changes and file touches change it; label and position changes don't. Add regressions for Notes bugs 1, 2 and 4.
  - verify: `ctest -R HashInvariants`
  - size: M
- [ ] M100.P6.T2 — Add a plugin sweep: every render-hashed knob of every bundled plugin changes the hash
  - files: Tests/HashPluginSweep_Test.cpp, Tests/CMakeLists.txt, Tests/hash_exempt_allowlist.txt
  - approach: create each plugin from appPTR's list. For each isRenderHashed knob, perturb it by type, assert the hash changed, revert, and assert it is restored. The allowlist explains every exemption.
  - verify: ctest passes with the OFX bundle
  - size: L
- [ ] M100.P6.T3 — Benchmark hash cost
  - files: tools/bench/hash_bench.py
  - approach: time the hash recompute on the bench comps and on a heavy roto comp. Target: keystroke to rehash under 1 ms on HD comps.
  - verify: record the results in `## Decisions`
  - size: M

**Verification gate:** full ctest green in both scheduler modes; the HashInvariants and HashPluginSweep suites pass; the audit run leaves no discrepancy unexplained; the regressions for bugs 1, 2 and 4 pass; value toggling and undo hit the cache; touching a frame file refreshes the viewer in the AppImage; `incrementKnobsAge`/`knobsAge` appear nowhere outside serialization.
````
