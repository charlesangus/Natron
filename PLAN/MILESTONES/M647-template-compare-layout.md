# M647 - Template Compare Layout

The layout half of F33 - Graph Diff And Merge's UAT round 1 feedback (2026-11-27).
Compare with Template Base draws ghosts at their absolute base-file positions. Once the artist
has moved the template, the ghosts float away from the nodes around them. Every moved node
is tinted as changed, even when the whole template was simply moved. This milestone:
- removes the template's overall offset before comparing;
- adds a separate, quieter "moved" state for nodes moved within the template;
- places ghosts, and nodes an update adds, relative to their matched neighbours.

It is a member of F33 - Graph Diff And Merge: it branches off
`feature/f33-graph-diff-and-merge` and merges into it, and runs beside M646 - Template Node
And Versioning.

Code facts (diagnosis of 2026-11-27, on the F33 branch tip e073795254):
- Ghost rectangles come straight from `change.x/y` (`Gui/NodeGraphDiffOverlay.cpp:349-354`),
  which `describe` fills from the base node's `layout.x/y` (`Engine/ProjectMerge.cpp:103-105`).
- Import copies `layout` unchanged (`Engine/ProjectTemplateImport.cpp:221-256`).
- `diffSnapshots` emits `SetNodeUiOp` for position, size or colour
  (`Serialization/Ops/SnapshotDiff.cpp:315-330`). `compare` maps it to `appearance = true`
  (`ProjectMerge.cpp:725-728`); the overlay tints it (`NodeGraphDiffOverlay.cpp:305`) and the
  panel prints "Appearance changed" (`Gui/NodeDiffPanel.cpp:413`).
- The merge itself already follows the never-conflict rule for layout
  (`Serialization/Merge/ThreeWayMerge.cpp:483-497`). Only the compare views, and the
  placement of nodes an update adds, are wrong.

## Scope

**In:**
- The **template offset**: per group, the median of (mine − base) position over the matched
  nodes in the compared scope.
- A **moved** flag on `ChangeSummary` nodes, for nodes whose position still differs from base
  by more than 10 scene units once the offset is removed.
- `appearance` counts only colour and size.
- A **placement** for every removed node: base position plus the offset of its nearest matched
  neighbour by graph distance in base, or the group's offset when it has none.
- Nodes added by an update are placed the same way, from their neighbours in theirs.
- In the overlay, ghosts use the placement, and moved nodes get an outline with no tint.
  The diff panel shows "Moved".

**Out:**
- The template node, its hull and versioned updates (M646 - Template Node And Versioning).
- Changing how the merge resolves layout (none; never-conflict stays).

**Edge cases:**
- A compare with no matched nodes in a group uses a zero offset there.
- Nodes inside groups use their own group's offset and neighbours, never the parent's.
- A removed node whose neighbours were also removed takes the offset of its nearest matched
  node through the chain, or failing that the group offset.
- A move within the 10-unit threshold, after removing the offset, is not flagged. This
  absorbs rounding from snapping.
- Compare with File… (not a template) uses the same offset rule over the whole matched graph.

## Phase 647.1: Offset-aware compare and placement

- [x] M647.P1.T1 — Template offset, moved flag and ghost placement in compare
  - files: `Engine/ProjectMerge.h`, `Engine/ProjectMerge.cpp`, `Tests/ProjectMerge_Test.cpp`
  - approach: in `ProjectMerge::compare`:
    - compute the per-group median offset over matched nodes;
    - set `moved` when the offset-corrected position differs by more than 10 units, and keep `appearance` for colour and size only;
    - for removed nodes, fill a new `placedX/placedY` from the nearest matched neighbour's offset (BFS over base wiring within the group), falling back to the group offset.

    Put the offset and neighbour logic in one helper, so P1.T2 can reuse it.
  - verify: compiled at P1.T4. gtests:
    - moving every template node by (500, 300) gives no moved or appearance flags;
    - moving one node within the template sets only `moved` on it;
    - a removed node's placement sits at its neighbour's offset;
    - nodes inside a group use the group's own offset.
  - size: M

- [x] M647.P1.T2 — Place nodes added by an update
  - files: `Engine/ProjectMerge.cpp`, `Tests/ProjectMerge_Test.cpp`
  - approach: after the merge, and before the ops are applied in `updateFromTemplate`, give each node theirs added a position from P1.T1's helper: its theirs position plus the offset of its nearest matched neighbour in theirs. Keep it a separate function called from the result path, so it doesn't tangle with M646 - Template Node And Versioning's status changes to the same function.
  - verify: compiled at P1.T4. A gtest moves the artist's template by (500, 300); the supervisor adds a node beside an existing one; after the update the new node sits beside that node's live position.
  - size: M

- [x] M647.P1.T3 — Overlay and diff panel use placement and the moved state
  - files: `Gui/NodeGraphDiffOverlay.cpp`, `Gui/NodeGraphDiffOverlay.h`, `Gui/NodeDiffPanel.cpp`, `Tests/gui/m104_diff_overlay.py`
  - approach:
    - Ghosts draw at `placedX/placedY`.
    - A node with only `moved` gets an outline in the changed colour, with no fill tint.
    - The diff panel shows "Moved", and "Appearance changed" only for colour and size.
    - Extend the GUI test: move the whole template and check zero tinted nodes; move one node and check one outlined node; delete a node and check its ghost sits within one node width of its old neighbour's live position.
  - verify: compiled at P1.T4. The GUI test passes.
  - size: M

- [x] M647.P1.T4 — Build, ctest and GUI checkpoint
  - files: none
  - approach: syntax-check the touched TUs, then a remote gate build with ctest and `--gui` for `m104_diff_overlay.py` and `m97_template_merge.py`.
  - verify: green. The build is required.
  - size: M

**Verification gate:**
- Full ctest is green.
- `m104_diff_overlay.py` and `m97_template_merge.py` pass remotely.
- Moving the whole template shows nothing changed, and a node moved within it shows only as moved.
- Ghosts sit by their live neighbours.
- PR CI into the F33 branch is green.

## Decisions

- 2026-11-27 — **Moves get their own "moved" state (user, F33 UAT round 1 grill):** remove the template's overall offset, then outline a node moved relative to the template, with no tint. Colour and size still count as appearance.
- 2026-11-27 — Ghosts and nodes an update adds are placed by the nearest matched neighbour's offset, falling back to the group median (planner). The diagnosis found that import doesn't offset positions, so absolute base coordinates only line up when the template was never moved.
- 2026-11-27 — **Split from M646 - Template Node And Versioning (user):** two members, run in parallel.
- 2026-11-28 — Merged into F33 (PR #409, squash).
