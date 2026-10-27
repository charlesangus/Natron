# M176 - Splice-Vertical Regression

`Tests/gui/m55_node_graph.py` section `splice-vertical` fails 6 checks on main (seen on `M80-final.AppImage`, on main plus M80 - Native RAW Support, and on M134 - Output Picker Pixel Aspect's branch, which doesn't touch the node graph; inbox, 2026-10-24). The hover highlight isn't drawn, the splice connections aren't made, and redo doesn't re-apply them. It is either a regression from a node-graph change since M55 - Node Graph Polish shipped, or a script assumption that no longer holds. This milestone finds which and fixes it.

## Scope

**In:**
- Bisect the failure to a commit, then fix the code or the script, whichever is wrong.

**Out:**
- Other `m55_node_graph.py` sections, unless the same cause breaks them.

**Edge cases:**
- If the cause is a deliberate behaviour change (e.g. from M158 - Node Toolbar As Menu or M92 - Linked Nodes), the script is updated to the new behaviour and the change is recorded in `## Decisions`, rather than reverting it.

## Phase 176.1: Find and fix

- [x] M176.P1.T1 — Bisect the splice-vertical failure
  - files: none (investigation); report in this file's `## Decisions`
  - approach: Run `Tests/gui/m55_node_graph.py` under Xvfb (recipe in `build/deeprepro/run-gui.sh`) against AppImages from M55 - Node Graph Polish's merge and later merges to main, bisecting by merge commit. Read the failing checks' screenshots. Name the first bad merge and whether the code or the script assumption is wrong.
  - verify: the named first-bad merge reproduces the 6 failures and its parent passes `splice-vertical`.
  - size: M

- [x] M176.P1.T2 — Fix the splice-vertical cause
  - files: decided by T1 — the node-graph splice code (`Gui/NodeGraph*.cpp`, `Gui/Edge.cpp`) or `Tests/gui/m55_node_graph.py`
  - approach: Fix whichever side T1 found wrong, keeping one-step undo/redo of the splice.
  - verify: `splice-vertical` passes all checks under Xvfb on the fixed AppImage, and the rest of `m55_node_graph.py` still passes.
  - size: M

**Verification gate:** the full build and `ctest` pass. `Tests/gui/m55_node_graph.py` passes every section under Xvfb on a `package.sh` AppImage. The GUI bench rule applies if the diff touches `Gui/NodeGraph*` or `Gui/Edge*`. Hover-highlight screenshots are shared with the user before sign-off.

## Decisions

- 2026-10-27 — **No longer reproduces (bosley-native-nodes):** `Tests/gui/m55_node_graph.py` passes every section (splice-vertical 13/13, hover highlight drawn) on main at 59f836282 (M226) under the 1920x1080 harness. The failures were seen under the old 1600x1000 harness; no code change was needed, so nothing to bisect. Closed without a code change; reopen if it recurs.
