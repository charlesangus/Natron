# M649 - On-Graph Merge Resolution

Full merge resolution on the node graph, from F33 - Graph Diff And Merge's UAT feedback
(2026-11-27). Today the compare overlay is view-only: ghosts (nodes in the other version but
not in the live graph) can't be selected, and nothing in the other version can be brought
across. Conflicts resolve only in the Merge Conflicts dock panel. This milestone:
- makes ghosts selectable, and lets the artist cherry-pick in both directions: restore
  ghosts (spliced back into the chain), revert changed nodes, take single knobs, remove
  added nodes and restore positions;
- resolves conflicts on the graph: clicking a conflict badge opens a popover with the
  available choices, and structural conflicts preview each side as ghosts and dashed wires
  that can be clicked to pick;
- adds a merge toolbar at the bottom of the node graph for both modes;
- sweeps every conflict choice and removes impossible and duplicate ones, starting with
  Keep Both Keys on same-frame keys.

It is a member of F33 - Graph Diff And Merge: it branches off
`feature/f33-graph-diff-and-merge` and merges into it. It needs M647 - Template Compare
Layout's ghost placement (`placedX/placedY`) and moved state.

Code facts (scout of 2026-11-27, on the F33 branch tip 488b376fef):
- Ghosts are one `NodeDiffGhostItem : QGraphicsObject` painting every ghost and wire
  (`Gui/NodeGraphDiffOverlay.h:54`), with `setAcceptedMouseButtons(Qt::NoButton)`
  (`.cpp:77-78`). Data comes from `gui->getGraphComparison()` (`.cpp:242`), a
  `ProjectMerge::ChangeSummary*`.
- `NodeChange` (`Engine/ProjectMerge.h:84-141`) keeps only a description of a removed node
  (id, parent, names, plugin, x/y, `sources`/`readers`). `describe(beforeNodes.at(id),
  eRemoved)` (`ProjectMerge.cpp:797`) drops the serialization, and the other snapshot lives
  only inside `compare`. `KnobChange` already holds `before`/`after` `KnobSerialization`.
- Restore building blocks: `CreateNodeOp`, `SetInputOp` (`Serialization/Ops/GraphOp.h:47,73`),
  `diffSnapshots` (`Serialization/Ops/SnapshotDiff.h:46`), `ProjectMerge::applyAllOrNothing`
  (`ProjectMerge.h:54`), and `ApplyGraphOpsCommand::applyAndPush` for one undo step
  (`Gui/NodeGraphUndoRedo.cpp:2692`). `sideOperations` rebuilding a delete-modify node is
  the closest precedent.
- Rubber-band selection iterates `_nodes` only (`Gui/NodeGraphPrivate.cpp:189-212`), and
  click hit-testing walks up to a `NodeGui` (`isNodeGuiChild`, `Gui/NodeGraph10.cpp:67`).
  Nothing expects a non-NodeGui in `_selection`.
- The conflict badge is a `NodeGuiIndicator` painted into `NodeGuiDecorations`
  (`Gui/NodeGui.cpp:1294,4140`), not clickable. `refreshToolTipAt` (`:4294`) already
  hit-tests badges for tooltips (`Gui/NodeGraph25.cpp:236-247`).
- Conflict kinds: eValue, eInput, eStructuralInsert, eDeleteModify, eReparent, eAdopted,
  eCurveKey, eRotoShape (`Serialization/Merge/MergeConflict.h:40-69`). Choices are Mine,
  Theirs and Base via `canChooseSide`/`sideOperations`, plus `MergeConflict::options`; the
  only option is `kMergeOptionKeepBothKeys`, whose rule (theirs at `time + 1`, or halfway
  to the next key) is `Serialization/Merge/MergeHandler.cpp:147-174`.
- Each side's `connections` (`{nodeId, input, sourceId}`), `removedNodes` and `parent` are
  enough to draw a structural side; eDeleteModify carries the base `node` only.
- Python: `Gui/PyGuiApp.h:192-215` (`importTemplateAsUndoStep`,
  `isMergeConflictBadgeShown`, `clearGraphComparison`), `Engine/PyAppInstance.h:439`.

## Scope

**In:**
- Selectable ghosts: click, shift/ctrl-click and rubber band, with a selection highlight.
- Compare picks, each one undo step, with the comparison recomputed afterwards:
  - **Restore** (ghost): recreate the node at its placement with its knobs, inputs from
    matched or co-restored nodes, and splice downstream back in. Removed Group nodes come
    back whole.
  - **Restore (Keep My Wiring)** and **Restore (Splice Everywhere)** variants.
  - **Revert to Other** (amber node): all its changed knobs, inputs and appearance.
  - **Take** per changed knob, in the diff panel and the popover; **Revert Node** in both.
  - **Remove** (green node): delete it and bridge its main input to downstream.
  - **Restore Position** (moved node).
- On-graph conflict resolution:
  - clicking a conflict badge opens a non-modal popover listing the node's conflicts with
    their available choices;
  - clicking an amber node's change marker opens the same popover with its changed knobs;
  - hovering a structural choice previews that side on the graph; clicking a preview ghost
    picks it.
- **Pending splice choices:** a restored ghost's downstream input that the artist rewired
  becomes an on-graph choice (Keep My Wiring / Splice In) in the conflict list.
- A **merge toolbar** at the bottom of the node graph, in comparison mode or conflict mode,
  hidden when neither is active.
- **Next/Previous Conflict** and **Next/Previous Change** in the node-graph menu with
  shortcuts. They frame the node and open its popover. After a conflicted update the graph
  frames the first conflicted node.
- The Merge Conflicts dock panel stays but no longer opens by default (after an update or
  on load). It offers the same choices as the popover, from the same source.
- The **choice sweep:** an audit of every conflict kind × choice, then removal of
  impossible choices (Keep Both Keys at one frame) and duplicate ones (a choice whose
  result equals an earlier offered choice).
- Python bindings for every pick and for listing and resolving conflicts.
- User docs in `docs/graph-merge.md`.

**Out:**
- Template node, versioning, Publish and Update to Latest (M646 - Template Node And
  Versioning).
- Ghost placement, the template offset and the moved flag (M647 - Template Compare Layout).
- Picking individual children of a removed Group from outside it (none).
- Removing choices that are possible and distinct but leave an odd graph (none; the user
  chose impossible + duplicate only).

**Edge cases:**
- A restored ghost's downstream input still points at the ghost's old upstream, or is
  empty: it is re-routed to the restored node.
- A downstream input the artist rewired since: plain Restore leaves it and adds a pending
  splice choice; Keep My Wiring leaves it with no choice; Splice Everywhere re-routes it.
- Several adjacent ghosts restored together are wired to each other as in the other
  version.
- A restored node's script name collides with a live node: it gets a fresh name, and its
  readers and expressions referencing it within the restored set are remapped.
- A ghost whose input source is neither live-matched nor restored keeps that input empty.
- Comparison and conflicts are **one at a time**: starting a comparison while conflicts are
  unresolved hides their badges and the toolbar's conflict mode until the comparison is
  cleared; an update that raises conflicts clears an active comparison first.
- A mixed selection (ghosts, amber, green) shows each applicable pick in the context menu;
  a pick applies only to the nodes it fits.
- The last conflict resolved from a popover closes it; the toolbar hides when the list
  empties and no comparison is active.
- Conflicts reloaded from a saved project keep their structural previews.
- Pending splice choices persist with the other conflicts on save.

**Assumptions:**
- `merge/conflicts.yaml` may not round-trip every side's `connections`, `removedNodes`,
  `parent` and `node`; P1.T7 checks and adds what's missing.
- M647 - Template Compare Layout's `placedX/placedY` is on `NodeChange` when this starts.

## Phase 649.1: Engine — choices, picks and persistence

- [x] M649.P1.T1 — Audit every conflict choice
  - files: none changed; read `Serialization/Merge/MergeConflict.h`, `Serialization/Merge/MergeHandler.cpp`, `Serialization/Merge/ConflictResolution.cpp`, `Engine/ProjectMerge.cpp`
  - approach: for each conflict kind × each choice (Mine, Theirs, Base, every option), state when it is offered today, what it produces, and whether it is impossible, a duplicate of another choice (and when), or keep. Return the table; the PM records it under this file's `## Decisions` and P1.T2 implements it. Same-frame Keep Both Keys is impossible by user decision.
  - verify: the table has a row for every kind in `MergeConflict.h:40-69` and every choice, each with a keep/drop rule.
  - size: L

- [x] M649.P1.T2 — Offer only possible, distinct choices
  - files: `Serialization/Merge/MergeHandler.cpp`, `Serialization/Merge/MergeConflict.h`, `Serialization/Merge/MergeConflict.cpp`, `Engine/ProjectMerge.h`, `Engine/ProjectMerge.cpp`, `Gui/MergeConflictPanel.cpp`, `Tests/Serialization/ConflictResolution_Test.cpp`
  - approach: add one `availableChoices(conflict)` that every UI and binding uses, implementing P1.T1's table: drop impossible choices, and drop a choice whose result equals an earlier one in Mine, Theirs, Base, options order. Remove Keep Both Keys for keys at the same frame and delete the `time + 1`/halfway rule (`MergeHandler.cpp:147-174`); keep it where the keys are at different frames. `canChooseSide`/`canChooseOption` agree with it. The panel builds its buttons from it.
  - verify: compiled at P1.T8. gtests per table row: a same-frame key conflict offers Mine and Theirs only; Base is hidden when base equals mine; different-frame Keep Both still works.
  - size: M

- [x] M649.P1.T3 — Keep the other version's nodes on the comparison
  - files: `Engine/ProjectMerge.h`, `Engine/ProjectMerge.cpp`, `Tests/ProjectMerge_Test.cpp`
  - approach: `ChangeSummary` holds a `shared_ptr<const ProjectSerialization>` of the other snapshot and the live snapshot it was compared against, and each removed `NodeChange` can look up its `NodeSerialization` (with children for a Group) by node ID. No GUI change.
  - verify: compiled at P1.T8. A gtest compares against a base with a deleted Blur and a deleted Group, and reads both serializations, children included, from the summary.
  - size: M

- [x] M649.P1.T4 — Restore operations with splice
  - files: `Engine/ProjectMerge.h`, `Engine/ProjectMerge.cpp`, `Tests/ProjectMerge_Test.cpp`
  - approach: `ProjectMerge::restoreOperations(summary, nodeIds, SpliceMode{eAsk, eKeepMine, eEverywhere})` returns ops plus a list of pending splice choices `{reader, input, restored}`:
    - `CreateNodeOp` per node at `placedX/placedY`, Groups whole, fresh script names on collision with references remapped inside the restored set;
    - `SetInputOp` for each input whose source is live-matched or co-restored;
    - for each reader in the other version that is live: an input that is empty or still points at the ghost's old upstream is re-routed; a rewired one is re-routed under eEverywhere, left under eKeepMine, and left plus reported under eAsk.
  - verify: compiled at P1.T8. gtests: a deleted middle node restores spliced; two adjacent deleted nodes restore as a chain; a rewired reader is reported under eAsk, left under eKeepMine, re-routed under eEverywhere; a name collision gets a fresh name.
  - size: L

- [x] M649.P1.T5 — Revert, take, remove and position operations
  - files: `Engine/ProjectMerge.h`, `Engine/ProjectMerge.cpp`, `Tests/ProjectMerge_Test.cpp`
  - approach: from the summary: `revertOperations(nodeIds)` (every changed knob, input and appearance back to the other version), `takeKnobOperations(nodeId, knobName)` (`SetKnobOp` from `KnobChange::before`/`after` as the other side), `removeAddedOperations(nodeIds)` (delete, bridging the main input to each downstream reader as a normal delete does), `restorePositionOperations(nodeIds)` (other version's position plus the template offset).
  - verify: compiled at P1.T8. One gtest per function, each applied through `applyAllOrNothing` and re-compared to show the node no longer differs in that respect.
  - size: M

- [x] M649.P1.T6 — Pending splice choices in the conflict list
  - files: `Serialization/Merge/MergeConflict.h`, `Serialization/Merge/MergeConflict.cpp`, `Serialization/Merge/ConflictResolution.cpp`, `Engine/ProjectMerge.cpp`, `Tests/Serialization/ConflictResolution_Test.cpp`
  - approach: a new conflict kind `eSpliceChoice` on the reader node, with choices Keep My Wiring and Splice In (through `availableChoices`), built from P1.T4's report and appended to the project's conflict list, so badge, panel, popover and save handle it like any conflict. Splice In is a `SetInputOp`; Keep My Wiring just removes the entry.
  - verify: compiled at P1.T8. A gtest adds a splice choice, resolves each way, and round-trips it through the conflict serialization.
  - size: M

- [x] M649.P1.T7 — Persist every side's structural state
  - files: the conflict serialization in `Serialization/Merge/MergeConflict.cpp` (and `Serialization/ProjectSerialization*` if `merge/conflicts.yaml` is written there), `Tests/Serialization/ConflictResolution_Test.cpp`
  - approach: check that `merge/conflicts.yaml` writes and reads each side's `connections`, `removedNodes`, `parent`, `knobs` and `node`; add any field that's missing. No change if it all round-trips.
  - verify: compiled at P1.T8. A gtest saves and reloads one conflict of each structural kind and compares the sides field by field.
  - size: M

- [x] M649.P1.T8 — Phase 649.1 build and ctest checkpoint
  - files: none
  - approach: syntax-check the touched TUs, then a remote gate build with ctest.
  - verify: green. The build is required.
  - size: M

## Phase 649.2: Graph UI

- [x] M649.P2.T1 — Selectable ghost items
  - files: `Gui/NodeGraphDiffOverlay.h`, `Gui/NodeGraphDiffOverlay.cpp`, `Gui/NodeGraph.h`, `Gui/NodeGraph10.cpp`, `Gui/NodeGraphPrivate.cpp`
  - approach: replace the single `NodeDiffGhostItem` with one item per ghost plus one wire item. NodeGraph keeps a separate ghost selection set: click, shift/ctrl-click and the rubber band (extend the `_nodes` loop with ghost rects) select ghosts alongside nodes, and clicking empty canvas clears both. Selected ghosts get the normal selection outline. Delete, copy and drag ignore ghosts. Expose the ghost selection for P2.T2 and P2.T6.
  - verify: compiled at P2.T7. Exercised by P3.T1; `m104_diff_overlay.py` still passes (one ghost for the deleted Blur2).
  - size: M

- [x] M649.P2.T2 — Undoable picks and the context menu
  - files: `Gui/NodeGraph35.cpp` (context menu), `Gui/NodeGraphDiffOverlay.cpp`, new `Gui/MergePicks.h`, new `Gui/MergePicks.cpp`, `Gui/CMakeLists.txt`
  - approach: `MergePicks` wraps each P1.T4/P1.T5 builder: apply through `ApplyGraphOpsCommand::applyAndPush` as one undo step named after the pick, append any pending splice choices to the conflict list, then recompute the comparison against the same other snapshot so the overlay refreshes. The context menu offers, for the selection: Restore, Restore (Keep My Wiring), Restore (Splice Everywhere) on ghosts; Revert to Other on amber nodes; Remove on green nodes; Restore Position on moved nodes. Give each QAction an objectName.
  - verify: compiled at P2.T7. Exercised by P3.T1, including undo restoring the comparison.
  - size: M

- [x] M649.P2.T3 — Merge popover from badges and change markers
  - files: new `Gui/MergePopover.h`, new `Gui/MergePopover.cpp`, `Gui/NodeGui.h`, `Gui/NodeGui.cpp`, `Gui/NodeGraph10.cpp`, `Gui/MergeConflictResolve.cpp`, `Gui/CMakeLists.txt`
  - approach: a frameless, non-modal popover anchored to a node. Conflict section: each conflict with a button per `availableChoices` entry, plus All Mine / All Theirs for the node, resolving through `MergeConflictResolve`'s undoable path. Changes section (comparison mode): each changed knob with Take, plus Revert Node, through `MergePicks`. Open it from a click on the conflict badge or on a change marker drawn on amber nodes, hit-tested in `NodeGraph::mousePressEvent` as `refreshToolTipAt` does. It refreshes after each choice, closes on Escape, outside click, or when it empties. The badge tooltip says "Click to resolve".
  - verify: compiled at P2.T7. Exercised by P3.T2.
  - size: M

- [x] M649.P2.T4 — Structural previews on the graph
  - files: new `Gui/MergeConflictPreview.h`, new `Gui/MergeConflictPreview.cpp`, `Gui/MergePopover.cpp`, `Gui/NodeGraph.cpp`, `Gui/CMakeLists.txt`
  - approach: hovering a choice of an eInput, eStructuralInsert, eReparent, eDeleteModify or eSpliceChoice conflict in the popover or panel draws that side: dashed wires from its `connections` in the side's colour, the base node of a delete-modify as a ghost (P2.T1's item), the side's inserted nodes highlighted and the other side's dimmed. While a structural conflict's popover is open its sides stay drawn faintly, and clicking a side's preview ghost or wire end picks that side. Clears when the popover closes.
  - verify: compiled at P2.T7. Exercised by P3.T2's preview checks.
  - size: L

- [x] M649.P2.T5 — Merge toolbar, navigation and panel default
  - files: new `Gui/NodeGraphMergeBar.h`, new `Gui/NodeGraphMergeBar.cpp`, `Gui/NodeGraph.cpp`, `Gui/NodeGraph35.cpp`, `Gui/Gui60.cpp`, `Gui/NodeGui.cpp`, `Gui/MergeConflictPanel.cpp`, `Gui/CMakeLists.txt`
  - approach: a bar along the bottom of the node graph, fitting 1920x1080.
    - Comparison mode: what is compared, added/changed/removed/moved counts, Previous/Next Change, Restore Selected, Revert Selected, Clear Comparison.
    - Conflict mode: count, Previous/Next Conflict, All Mine, All Theirs, Open Panel.
    - Hidden when neither is active. One mode at a time: a comparison hides conflict badges and conflict mode until cleared, and an update that raises conflicts clears the comparison first.
    - Next/Previous actions also go in the node-graph menu with shortcuts; they frame the node and open its popover. After a conflicted update, frame the first conflicted node.
    - The dock panel no longer opens by itself after an update or on load.
  - verify: compiled at P2.T7. Exercised by P3.T1 and P3.T2; `m104_merge_conflicts.py` is updated in P3.T2 for the panel default.
  - size: M

- [x] M649.P2.T6 — Python bindings for picks and conflicts
  - files: `Gui/PyGuiApp.h`, `Gui/PyGuiApp.cpp`, the Gui typesystem XML, `Engine/PyAppInstance.h`, `Engine/PyAppInstance.cpp` (conflict listing)
  - approach: on the GUI app: `restoreFromComparison(effects, splice="ask"|"keepMine"|"everywhere")`, `revertFromComparison(effects)`, `takeKnobFromComparison(effect, knobName)`, `removeAddedFromComparison(effects)`, `restorePositionFromComparison(effects)`, `selectComparisonGhosts(nodeNames)`, `getComparisonGhosts()`, `openMergePopover(effect)`, `isMergePopoverOpen()`, `mergeBarMode()`. On the app: `getMergeConflicts()` returning each conflict's node, kind, label and available choices, and `resolveMergeConflict(index, choice)`. Each pick is one undo step through `MergePicks`. Shiboken returns that can fail early-return in inject-code (no null returns from ownership bindings).
  - verify: compiled at P2.T7. Exercised by P3.T1 and P3.T2.
  - size: M

- [ ] M649.P2.T7 — Phase 649.2 build and GUI checkpoint
  - files: none
  - approach: syntax-check the touched TUs (moc TUs need the build), then a remote gate build with ctest and `--gui` for `m104_diff_overlay.py`, `m104_merge_conflicts.py` and `m97_template_merge.py`.
  - verify: green. The build is required.
  - size: M

## Phase 649.3: Tests, docs and close-out

- [ ] M649.P3.T1 — GUI test of compare picks
  - files: new `Tests/gui/m649_compare_picks.py`
  - approach: on the `Tests/gui/guitest.py` harness, compare against a base with a deleted middle Blur, two adjacent deleted nodes, a deleted Group, an amber Grade, a green added node and a moved node. Restore the middle Blur and check it is spliced; undo and check the ghost is back; rubber-band the adjacent pair and restore them as a chain; restore the Group whole; rewire a reader, Restore, and check a pending splice choice appears and resolves both ways; Take one knob; Revert the Grade; Remove the added node and check the bridge; Restore Position; check the toolbar counts after each. Save Xvfb screenshots of selected ghosts and the comparison toolbar.
  - verify: passes under `Tests/gui/run-gui-test.sh` on the phase build.
  - size: M

- [ ] M649.P3.T2 — GUI test of on-graph conflicts
  - files: new `Tests/gui/m649_graph_conflicts.py`, `Tests/gui/m104_merge_conflicts.py`
  - approach: run a conflicted Update from Template; check the panel did not open, the first conflicted node is framed and the toolbar is in conflict mode. Open a badge's popover; resolve a value conflict with Theirs; check a same-frame key conflict offers only Mine and Theirs; on an input conflict check the preview wires appear on hover, then pick Theirs by clicking the preview; Next Conflict frames the next node; undo restores a conflict; start a comparison and check badges hide, then clear it. Save and reopen and check a structural preview still draws. Update `m104_merge_conflicts.py` to open the panel through Open Panel. Save Xvfb screenshots of the popover, a structural preview and the conflict toolbar.
  - verify: both scripts pass under `Tests/gui/run-gui-test.sh` on the phase build.
  - size: M

- [ ] M649.P3.T3 — User docs for on-graph merge
  - files: `docs/graph-merge.md`
  - approach: add sections on selecting ghosts and each pick, the splice rules and pending splice choices, the popover, structural previews, the merge toolbar and its shortcuts, and the Python calls from P2.T6. Name every action as the UI labels it.
  - verify: every action from P2.T2–P2.T6 is described, and the doc passes the comment/docs policy check.
  - size: M

- [ ] M649.P3.T4 — Gate build, GUI checks and screenshots
  - files: none
  - approach: remote gate build with ctest and `--gui` for `m649_compare_picks.py`, `m649_graph_conflicts.py`, `m104_diff_overlay.py`, `m104_merge_conflicts.py` and `m97_template_merge.py`. Send the user each screenshot from P3.T1 and P3.T2 as its own file before sign-off.
  - verify: green, and the user OKs the screenshots.
  - size: M

**Verification gate:**
- Full ctest is green, including the new ConflictResolution and ProjectMerge gtests.
- The five merge GUI scripts pass remotely.
- A restored ghost is spliced back in as one undo step; a rewired reader becomes a pending
  choice.
- Every conflict is resolvable from its badge popover, and no impossible or duplicate
  choice is offered anywhere.
- The user OKs the screenshots.
- PR CI into the F33 branch is green.

## Decisions

- 2026-11-28 (user): animated knobs are compared and merged at the knob level, not per key — any difference in the curve is a difference of the whole knob; no key reconciliation. One knob conflict (Mine / Theirs / Base) when both sides changed the curve differently; per-key conflicts and Keep Both Keys (any frame) are removed. Supersedes the earlier same-frame/different-frame Keep Both Keys rule.
- 2026-11-27 — **One milestone for cherry-pick and on-graph conflict resolution (user):** the two overlap; the goal is full merge resolution capability on the graph.
- 2026-11-27 — **Splice restored ghosts back in (user).** A reader the artist rewired since is asked about per input, through an on-graph pending choice rather than a dialog, and the Restore menu also offers Keep My Wiring and Splice Everywhere (user: pop-ups are to be avoided).
- 2026-11-27 — **Badge popover plus structural ghosts (user):** clicking a badge opens a popover of choices; structural sides preview as ghosts and dashed wires that can be picked.
- 2026-11-27 — **Keep Both Keys is never offered for same-frame keys (user, inbox 2026-11-27):** two keys can't share a frame; the one-frame/halfway offset rule goes. It stays for keys at different frames.
- 2026-11-27 — **Sweep removes impossible and duplicate choices (user):** an audit lists every kind × choice first; a choice equal in result to an earlier one is hidden.
- 2026-11-27 — **Python bindings for picks and conflicts (user):** pipelines script it, and the GUI tests drive the same calls.
- 2026-11-27 — **Dock panel kept, closed by default (user).** A toolbar at the bottom of the node graph gives counts, Next/Previous and resolve-all, and serves the comparison too (user).
- 2026-11-27 — **Comparison and conflicts are shown one at a time (user).**
- 2026-11-27 — **Remove is offered on added nodes, with a bridge (user):** cherry-pick works in both directions.
- 2026-11-27 — **A removed Group restores whole (user).**
- 2026-11-27 — **Per-knob Take in both the popover and the diff panel (user):** one pattern for everything on the graph.
- 2026-11-27 — Pending splice choices are a conflict kind, so badge, panel, popover and save handle them with no new path (planner). Conflicts reloaded from a file keep structural previews, so P1.T7 makes sure side state persists (planner).
- 2026-11-28 — Phase 1 in f00d619581; m649-gate1 (P1.T8) running. Audit table in the commit's report: a choice is hidden only when impossible or equal to an earlier one; same-frame Keep Both is gone; Base stays on a same-frame key conflict when it means "remove the key" (a distinct outcome — PM reading of P1.T2's verify). PendingSplice carries sourceId. Open: nodes restored from a template version get no template origin/key back — fold into Phase 2 once M646 lands.
- 2026-11-28 — P1.T8: m649-gate1 (full ctest + merge GUI) green except the YAML round trip; fixed b482067c6a (plug-in knob defaults and choice entries persisted); m649-gate2 (ConflictResolution, MergeConflictYaml, ProjectMerge*, NodeMatcher*) green. Phase 2 in progress.
- 2026-11-28 — Phase 2 in 153589397e (m649-gate3 = P2.T7 running). restoreFromComparison takes ghost script names or node IDs (default: selected ghosts), splice "" = ask; getMergeConflicts dicts carry index, nodeId, choiceLabels. Conflict knob equality is state-only.
