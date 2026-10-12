# M104 - Merge Resolution UI

The in-app side of M103 - Graph Diff And Merge: importing templates, **Update from Template**,
a visual diff overlay on the node graph, and a merge view that resolves conflicts per knob.
Depends on M103 - Graph Diff And Merge. Freshness-check the briefs (PLAN-FORMAT §5a) against
M103's P1.T4 decision before starting.

## Design (applies to every task)

- **Import Template…** (File menu) calls `Project::importTemplate` from M103 - Graph Diff And
  Merge. **Update from Template…** is in the File menu and in the node-graph context menu on any
  template-tagged node. It finds the template's stored path, asking for the file if the path
  is gone, and merges with base = the stored snapshot, mine = the live scope, and theirs = the
  current file. The result is applied to the live graph as **one undo step** (M102's
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

## Phase 104.1: Engine and undo glue

- [ ] M104.P1.T1 — Project-level update-from-template and compare
  - files: new `Engine/ProjectMerge.h`, new `Engine/ProjectMerge.cpp`, `Engine/Project.h`, `Engine/CMakeLists.txt`, new `Tests/ProjectMerge_Test.cpp`
  - approach: `ProjectMerge::updateFromTemplate(project, templateKey, newPath)` snapshots the live scope, runs M103's merge, computes operations against the live state, applies them through a caller-supplied applier (so Gui can wrap it in undo), updates the stored base, and returns the conflicts. `ProjectMerge::compare(project, otherSnapshot)` returns a per-node change summary for the overlay.
  - verify: compiled at P1.T3. Headless gtests on two corpus scenarios: a clean update changes only the template nodes and keeps the artist's overrides; a conflicted one returns its conflicts and leaves mine in the graph.
  - size: M

- [ ] M104.P1.T2 — Undoable apply of an operation batch
  - files: `Gui/NodeGraphUndoRedo.h`, `Gui/NodeGraphUndoRedo.cpp`
  - approach: `ApplyGraphOpsCommand` holds forward operations and inverse operations (`diffSnapshots(after, before)` over the touched scope). redo and undo apply them through `GraphOpApply` and refresh the NodeGuis (create and remove).
  - verify: compiled at P1.T3. Exercised by the P4.T1 GUI test: undo after Update from Template restores the pre-merge graph exactly.
  - size: M

- [ ] M104.P1.T3 — Phase 104.1 build and test checkpoint
  - files: none
  - approach: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug`, then `tools/ci/local/test.sh ctest debug`.
  - verify: green. The build is required.
  - size: S

## Phase 104.2: Merge view and actions

- [ ] M104.P2.T1 — Merge conflict panel
  - files: new `Gui/MergeConflictPanel.h`, new `Gui/MergeConflictPanel.cpp`, `Gui/CMakeLists.txt`, the Gui pane registration (`Gui/Gui20.cpp` or wherever panes are registered)
  - approach: a dock widget backed by `Project`'s conflict list. Conflicts are grouped by node, with a count in the title. Selecting a row frames the node and opens its settings panel. It refreshes when the list changes.
  - verify: compiled at P2.T5. The P4.T1 GUI test opens a conflicted file and sees the panel with the right count.
  - size: M

- [ ] M104.P2.T2 — Per-conflict resolution actions
  - files: `Gui/MergeConflictPanel.cpp`, new `Gui/MergeConflictResolve.cpp`, `Engine/ProjectMerge.cpp`
  - approach: Mine, Theirs and Base buttons, and handler options from the conflict's `handler options`. A resolution sets the knob, curve, input or structure through undoable commands and removes the conflict. Resolve-all applies to every conflict in the list or in the selected node. Undoing a resolution restores the conflict.
  - verify: compiled at P2.T5. A GUI test resolves one conflict each way, then undoes, then resolves all.
  - size: M

- [ ] M104.P2.T3 — Conflict badge on nodes
  - files: `Gui/NodeGui.h`, `Gui/NodeGui.cpp`
  - approach: a small warning glyph at the node's top-right corner while the node has unresolved conflicts, in the same style as the existing node-graph glyphs. It is not a label edit, and it updates when the conflict list changes.
  - verify: compiled at P2.T5. A GUI test sees the badge appear on a conflicted node and go away when it is resolved.
  - size: S

- [ ] M104.P2.T4 — Import, update and compare menu actions
  - files: `Gui/Gui20.cpp` (menus), `Gui/NodeGraph35.cpp` or the file that builds the node-graph context menu, `Gui/GuiAppInstance.cpp`
  - approach: File > Import Template…, File > Update from Template… (a list of the project's templates), File > Compare with File…, and a node-graph context menu with "Update from Template" and "Compare with Template Base" on template-tagged nodes. Update applies through `ApplyGraphOpsCommand` and opens the panel when there are conflicts.
  - verify: compiled at P2.T5, and exercised by the P4.T1 GUI test.
  - size: M

- [ ] M104.P2.T5 — Phase 104.2 build and test checkpoint
  - files: none
  - approach: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug`, then `tools/ci/local/test.sh ctest debug`.
  - verify: green. The build is required.
  - size: S

## Phase 104.3: Diff overlay

- [ ] M104.P3.T1 — Node-graph diff overlay
  - files: new `Gui/NodeGraphDiffOverlay.h`, new `Gui/NodeGraphDiffOverlay.cpp`, `Gui/NodeGraph.h`, `Gui/NodeGraph.cpp`
  - approach: given `ProjectMerge::compare`'s summary, tint added, changed and removed nodes. Draw ghost items for removed nodes, with their wires to surviving neighbours drawn dashed. The overlay is view-only: ghosts are not selectable and not serialized. Escape or "Clear Comparison" removes it.
  - verify: compiled at P3.T3. A GUI test compares against a corpus base and checks the tint counts and ghost count.
  - size: M

- [ ] M104.P3.T2 — Knob diff side panel
  - files: new `Gui/NodeDiffPanel.h`, new `Gui/NodeDiffPanel.cpp`
  - approach: with the overlay active, selecting a changed node lists each changed knob as `name: base → current`. Curves show key-count and key-time changes, and inputs show their source change.
  - verify: compiled at P3.T3. A GUI test selects a changed node and reads the expected rows.
  - size: M

- [ ] M104.P3.T3 — Phase 104.3 build and test checkpoint
  - files: none
  - approach: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug`, then `tools/ci/local/test.sh ctest debug`.
  - verify: green. The build is required.
  - size: S

## Phase 104.4: Close-out

- [ ] M104.P4.T1 — GUI test of the template workflow
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

- [ ] M104.P4.T2 — AppImage UAT
  - files: `build/appimages/M104-<sha>.AppImage`, `build/appimages/M104-uat.md`
  - approach: build with `tools/ci/local/package.sh` and launch-check it. The checklist: the supervisor-and-artist workflow end to end on a realistic template (Read → Grade → ColorCorrect → Roto-masked Merge → Write), the compare overlay, resolving in the panel, and `natron-merge` on the same files from a shell.
  - verify: the user signs off. **Stop for the user.**
  - size: M

**Verification gate:** full ctest, smoke, `m97_template_merge.py` and CI green. Update from
Template is a single undo step. Conflicts survive save and reopen. The user signs off the UAT.
