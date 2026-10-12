# M103 - Graph Diff And Merge

Three-way merge of node graphs, as an engine and a CLI, with no GUI (that is M104 - Merge
Resolution UI). The workflow: a supervisor publishes a template (a file of nodes), an artist
brings it into a shot script and modifies it, and the supervisor publishes a new version. The
artist merges the upstream changes with their own. Merging runs on plain `ProjectSerialization`
snapshots in `Serialization/Merge/`, so the CLI loads no plugins and starts fast. Depends on
M102 - Graph Edit Operations.

**Phase 103.1 is a spike that stops for the user.** How nodes are matched between versions is
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
  M104 - Merge Resolution UI.
- **CLI:** a new `natron-merge` executable that links only `NatronSerialization`:
  - `diff a b` prints a human-readable diff, and `--yaml` prints the operations from M102;
  - `merge base mine theirs -o out` exits 0 when clean and 1 when there are conflicts;
  - `git-driver %O %A %B` follows git's merge-driver contract;
  - `textconv f` prints `project.yaml` for `git diff`.

## Phase 103.1: Identity and provenance spike (stops for the user)

- [ ] M103.P1.T1 — Scenario corpus of template merges
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

- [ ] M103.P1.T2 — Matcher strategies and a scoring harness
  - files: new `Serialization/Merge/NodeMatcher.h`, new `Serialization/Merge/NodeMatcher.cpp`, new `tools/merge/score_matchers.cpp` (or a gtest that prints the table), `Serialization/CMakeLists.txt`
  - approach: define the `NodeMatcher` interface, then implement three strategies:
    - `IdOnly`, which matches on `origin` and ID;
    - `IdPlusAdoption`, which also adopts an unmatched new node when it has the same plugin and the same matched upstream and downstream neighbours on the same input labels as an unmatched removed node. This covers the delete-and-recreate case;
    - `NameBased`, which mirrors nuke_diff.

    The harness scores each strategy against `expected/*.yaml` and prints correct, wrong and missing correspondences per scenario.
  - verify: the harness runs over the whole corpus and prints a table with one row per scenario and one column per strategy.
  - size: L

- [ ] M103.P1.T3 — Provenance and scope prototype, plus spike report
  - files: new `docs/design/graph-merge-identity.md`, the prototype code alongside `Serialization/Merge/NodeMatcher.cpp`
  - approach: prototype the merge-scope rule (provenance-tagged nodes plus artist nodes wired strictly between them, and alternatives) and score it on the corpus. Write the report: the score table from P1.T2, the scope results, the failure cases for each strategy, and a recommendation. Cover the explicit question of whether a deleted-and-recreated node should count as the same node, and when that goes wrong.
  - verify: the report exists, covers every scenario, and makes one recommendation.
  - size: M

- [ ] M103.P1.T4 — User decision on matching policy and scope
  - files: this file's `## Decisions`
  - approach: **stop for the user.** Present the report's table and recommendation, and ask the user to choose the matching policy, the scope rule and any provenance changes. Record the answer as a decision, then re-check P2's briefs against it (PLAN-FORMAT §5a) before starting P2.
  - verify: a user decision is recorded.
  - size: S

## Phase 103.2: Provenance and three-way core

- [ ] M103.P2.T1 — Template provenance in the data model, bundle and Engine
  - files: `Serialization/NodeSerialization.h`, `Serialization/ProjectSerialization.h`, `Serialization/Yaml/NodeYaml.cpp`, `Serialization/Bundle/ProjectBundle.cpp`, `Engine/Project.h/.cpp` (`importTemplate`)
  - approach: the provenance rules from **Design**, as amended at P1.T4. Add the template entry list and the `templates/<key>/base.yaml` members, and the node `origin` field. `importTemplate` mints IDs, sets `origin`, stores the base, and returns the created nodes.
  - verify: compiled at P2.T5. gtest: importing the same template twice gives distinct IDs that share the same `origin.node`; save → load keeps origins and the base member; the base is byte-identical to the imported file's `project.yaml` node subset.
  - size: M

- [ ] M103.P2.T2 — Production matcher and scope
  - files: `Serialization/Merge/NodeMatcher.h/.cpp`, new `Serialization/Merge/MergeScope.h/.cpp`, new `Tests/Serialization/NodeMatcher_Test.cpp`
  - approach: keep only the chosen strategy and scope rule, and delete the losing prototypes and the scoring tool unless the user asked to keep them. Matching is deterministic.
  - verify: compiled at P2.T5. Every corpus scenario's correspondences equal `expected/*.yaml`.
  - size: M

- [ ] M103.P2.T3 — Three-way node, knob and input merge with the handler registry
  - files: new `Serialization/Merge/ThreeWayMerge.h/.cpp`, new `Serialization/Merge/MergeHandler.h/.cpp`, new `Serialization/Merge/MergeConflict.h`, new `Tests/Serialization/ThreeWayMerge_Test.cpp`
  - approach: `merge(base, mine, theirs, scope) → {merged snapshot, conflicts}` for matched nodes: per knob, per dimension, per input label. Default knob values merge as values: a knob absent from the YAML is "default". The generic scalar handler conflicts only on mine ≠ theirs ≠ base. The registry dispatches by plugin ID, then by knob kind. A conflict takes mine.
  - verify: compiled at P2.T5. A table of scalar cases (one side changed, both changed the same way, both changed differently, reset to default on one side) gives the expected merged value and conflict list.
  - size: L

- [ ] M103.P2.T4 — Structural merge
  - files: new `Serialization/Merge/StructuralMerge.cpp`, `Serialization/Merge/ThreeWayMerge.cpp`, new `Tests/Serialization/StructuralMerge_Test.cpp`
  - approach: the node-existence table (added, deleted, delete-vs-modify kept with a conflict, both added). Both inserting on the same base edge on any input slot gives the theirs-then-mine chain plus a structural conflict. Groups recurse. UI properties use the never-conflict rule. Renames follow the matcher, so a rename on one side applies cleanly.
  - verify: compiled at P2.T5. Each structural corpus scenario gives the expected merged graph and conflicts.
  - size: L

- [ ] M103.P2.T5 — Phase 103.2 build and test checkpoint
  - files: none
  - approach: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug`, then `tools/ci/local/test.sh ctest debug`.
  - verify: green. The build is required.
  - size: S

## Phase 103.3: Built-in handlers and conflict persistence

- [ ] M103.P3.T1 — Curve and text merge handlers
  - files: new `Serialization/Merge/CurveMerge.cpp`, new `Serialization/Merge/TextMerge.cpp`, new `Tests/Serialization/CurveTextMerge_Test.cpp`
  - approach: curves are merged as maps keyed by key time. A key changed or added on one side applies. Keys at the same time changed differently conflict, per key. An animated-versus-static change conflicts as a whole. Text uses a line-based diff3 in-house (no dependency), and overlapping hunks conflict.
  - verify: compiled at P3.T5. Keyframes added on both sides at different times merge clean; the same key changed differently gives one per-key conflict; non-overlapping text edits merge, and overlapping ones conflict.
  - size: M

- [ ] M103.P3.T2 — Roto per-item merge handler
  - files: new `Serialization/Merge/RotoMerge.cpp`, new `Tests/Serialization/RotoMerge_Test.cpp`
  - approach: match items by name within their layer path. Items added or deleted on one side apply. For items changed on both sides, item knobs merge per knob through the generic and curve handlers. Shape control points changed on both sides conflict for the whole shape. Layer order follows theirs, plus mine's additions.
  - verify: compiled at P3.T5. The corpus roto scenario merges as expected; both sides adding different shapes keeps both; both editing one shape's points gives one conflict.
  - size: M

- [ ] M103.P3.T3 — Tracker per-track merge handler
  - files: new `Serialization/Merge/TrackerMerge.cpp`, new `Tests/Serialization/TrackerMerge_Test.cpp`
  - approach: match tracks by script name. Tracks added or deleted on one side apply. A track changed on both sides merges its knob curves per key through the curve handler.
  - verify: compiled at P3.T5. Different tracks added on each side are both kept, and the same track keyed at different frames merges clean.
  - size: M

- [ ] M103.P3.T4 — Persist conflicts in the bundle
  - files: new `Serialization/Yaml/MergeConflictYaml.cpp`, `Serialization/Bundle/ProjectBundle.cpp`, `Serialization/ProjectSerialization.h`, `Engine/Project.cpp`, new `Tests/Serialization/MergeConflictYaml_Test.cpp`
  - approach: write `merge/conflicts.yaml` when conflicts exist and drop the member when none remain. Base, mine and theirs values use the knob and curve encoders. Loading a project with conflicts adds the load warning "N unresolved merge conflicts". Engine keeps the list on `Project` for M104 - Merge Resolution UI.
  - verify: compiled at P3.T5. Conflicts round-trip; the member is absent when the list is empty; loading gives exactly one warning.
  - size: M

- [ ] M103.P3.T5 — Phase 103.3 build and test checkpoint
  - files: none
  - approach: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug`, then `tools/ci/local/test.sh ctest debug`.
  - verify: green, and the whole corpus matches `expected/`. The build is required.
  - size: S

## Phase 103.4: `natron-merge` CLI and git integration

- [ ] M103.P4.T1 — `natron-merge diff` and `merge`
  - files: new `MergeTool/CMakeLists.txt`, new `MergeTool/main.cpp`, new `MergeTool/DiffPrinter.cpp`, `CMakeLists.txt` (`add_subdirectory(MergeTool)`), the install rules
  - approach: subcommands as in **Design**. The human-readable diff groups changes per node (`~ Grade1 (renamed from Grade3)`, `  white: 1 → 1.2`, `+ Blur2`, `- Crop1`, `  input Source: Read1 → Blur2`). `merge` takes `--scope template:<key>|all`. Exit codes: 0 clean, 1 conflicts, 2 error. Link only `NatronSerialization`.
  - verify: compiled at P4.T3. A ctest runs `natron-merge` on three corpus triples, checks the exit codes, and checks that its `diff` output contains the expected lines.
  - size: M

- [ ] M103.P4.T2 — git merge driver and textconv
  - files: `MergeTool/main.cpp`, new `docs/graph-merge.md`, new `Tests/merge/git_driver_test.sh`, `Tests/CMakeLists.txt`
  - approach: `git-driver %O %A %B` merges into `%A` with scope `all` and exits nonzero on conflict. `textconv` prints `project.yaml`, with node maps in sorted order. The doc gives the `.gitattributes` and `git config` setup and describes the template workflow.
  - verify: compiled at P4.T3. The shell test makes a temp git repo with the driver configured, commits a base, branches, edits both sides with corpus files, and runs `git merge`: a clean scenario merges automatically and a conflicted one exits nonzero with `merge/conflicts.yaml` present.
  - size: M

- [ ] M103.P4.T3 — Final build, full ctest and smoke
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
