# M330 - Graph Draw Policy

Control of draw states from the node graph: USDRead's `Initial Draw`, a new USDExpandHierarchy,
and Bake to Node. Policy travels along scene edges beside the layer stack and never enters a
USD layer, so viewers follow it and USDRender and USDWrite never see it. Design:
`PLAN/DESIGN/2026-11-04-scene-graph-design.md`. Names from M329 - Viewer Draw States are
assumed here; re-check them at claim time (PLAN-FORMAT.md §5a).

## Scope

**In:**
- Policy rules on `ScenePayload`: pattern, draw state and options, in graph order.
- USDRead `Initial Draw`: BBox (default), Levels, To Kind, Full, Full + Load. Applied only to the prims that USDRead brought in.
- USDExpandHierarchy: rules of pattern and draw state, with deferred matching.
- Rows show whether their state comes from the viewer or the graph.
- Bake to Node.

**Out:**
- Affecting USDRender or USDWrite: never (user).
- Deferred matching in other nodes — M331 - Deferred Pattern Matching.
- USD-level load controls such as authored population masks — none.

**Edge cases:**
- A viewer's interactive state beats graph policy on the same path; among graph rules, the later node wins.
- USDMerge concatenates its inputs' policies in input order.
- Full + Load on a pattern re-matches after each load until nothing new matches, and can be cancelled from the panel.
- A rule matching nothing does nothing and shows no error, since its target may sit inside an unloaded payload. The node shows the unsearched note from M331 - Deferred Pattern Matching once that lands.
- Bake to Node with nothing set is disabled.
- Baking when the viewed node already has a USDExpandHierarchy directly below it adds to that node rather than chaining another.

## Phase 330.1: Policy along edges

- [x] M330.P1.T1 — Carry draw policy on ScenePayload
  - files: `Engine/ScenePayload.{h,cpp}`, `Engine/Nodes/Scene/SceneOps.cpp`, `Gui/SceneViewport.cpp`, `Tests/Scene/ScenePayload_Test.cpp`, `Tests/Scene/USDWrite_Test.cpp`
  - approach: a rules list on the payload, free of pxr types like the rest of `ScenePayload`. It feeds the viewer's effective-state resolution as the middle layer of precedence. A change of rules must not change the stack hash, so it never re-authors a layer or recomposes.
  - verify: gtest — equal stacks with different rules hash equal; a USDWrite export is byte-identical with and without rules; a USDRender stage has no trace of them.
  - size: M

- [x] M330.P1.T2 — USDRead Initial Draw knob
  - files: `Engine/Nodes/Scene/USDRead.{h,cpp}`, `Engine/Nodes/Scene/docs/USDRead.md`, `Tests/Scene/USDRead_Test.cpp`, `Tests/gui/scene_initial_draw.py`
  - approach: script name `initialDraw`, label "Initial Draw", plus `levels` and `toKind` shown when relevant. Emits rules scoped to the file's top-level prims; To Kind stops at the first prim of that kind or stronger. Full + Load loads payloads below in viewers only. No scene edit.
  - verify: gtest — each value emits the expected rules; GUI script — a new USDRead on the city draws one box per top-level prim, native primitives merged with it stay Full, and no building layer opens.
  - size: M

## Phase 330.2: USDExpandHierarchy

- [x] M330.P2.T1 — USDExpandHierarchy node
  - files: `Engine/Nodes/Scene/USDExpandHierarchy.{h,cpp}`, `Engine/Nodes/Scene/USDExpandHierarchy.reg.inc`, `Engine/Nodes/Scene/docs/USDExpandHierarchy.md`, `Tests/Scene/USDExpandHierarchy_Test.cpp`, `Tests/gui/scene_expand_hierarchy.py`
  - approach: authors no layer; passes its input stack through and appends rules. Holds a list of rules so Bake to Node can write several; check existing table knobs before adding a knob type. Pattern knobs carry the prim-path hint. Viewer resolution matches patterns against loaded prims. A Full + Load rule iterates its re-match after each load. Rewrites the node removed from M231 - Scene Graph Panel; take its docs and registration from git history.
  - verify: gtest — stack hash is unchanged by the node, and a rule's effect is resolved per viewer; GUI script — a Full + Load rule on one block loads only that block's payloads, and removing the node returns that block to its USDRead default draw while its payloads stay loaded until Unload.
  - size: L

## Phase 330.3: Row origin and baking

- [x] M330.P3.T1 — Show where each row's state comes from
  - files: `Gui/SceneGraphPanel.cpp`, `Gui/SceneGraphModel.{h,cpp}`, `Tests/gui/scene_draw_origin.py`
  - approach: the draw column distinguishes viewer-set, graph-set and inherited values, with a tooltip naming the node. Reset to Graph on a row or on the panel clears interactive states.
  - verify: GUI script — a graph-set row shows its node in the tooltip, an interactive change flips its origin, and Reset to Graph flips it back.
  - size: M

- [x] M330.P3.T2 — Bake to Node
  - files: `Gui/SceneGraphPanel.cpp`, `Gui/SceneSelectionActions.{h,cpp}`, `Tests/gui/scene_bake_to_node.py`
  - approach: a panel action writing the viewer's interactive states into a USDExpandHierarchy below the viewed node, as literal paths, and clearing them. One undo step covers node, rules and clearing. If a USDExpandHierarchy is directly below, add to it.
  - verify: GUI script — after baking, the viewer draws the same, the states are graph-origin, Undo restores both graph and interactive states; saving and reopening keeps the draw.
  - size: M

**Verification gate:** release gate green; GUI scripts above pass under Xvfb; a USDWrite export and a USDRender of the city are unaffected by any rule; harness budgets still pass on the GPU host; screenshots of the draw column's origins shared before sign-off; accepted from the UAT AppImage.

## Decisions
- 2026-11-13 — P1.T1 (f83c64de9): rules propagate in `EffectInstance::evaluateScene` for every node (inputs' rules in input order, the node's own last; a repeat keeps its first place), not per merge node. `SceneDrawStateEnum` moved into `ScenePayload.h` so the payload stays free of scene headers. A rules-only change re-resolves on the viewer's worker, with no `requestSync`. Plain-path rules apply without a stage. Known limit: rules keep their authored paths, so a node that moves prims downstream doesn't remap them. Gate deferred to the end of phase 1.
- 2026-11-13 — P1.T2 (3bacda51d):
  - Initial Draw emits one rule whose pattern lists the file's root-defined prims (sublayers followed, payloads never opened).
  - The draw knobs are left out of the node hash, so changing them changes only the rules, not the stack.
  - The BBox default is kept as specified. 30 existing viewer GUI scripts that inspect drawn geometry now set Initial Draw to Full; the 7 left unchanged never look at the 3D drawing.
  - Phase-1 gate running with all 31 changed scripts.
- 2026-11-13 — All tasks implemented:
  - P2.T1 (f2aef69b3): rules are rows of plain knobs (`pattern<i>`/`draw<i>`/`levels<i>` plus `ruleCount`, as in RFVSwitch). The table knob packs rows into one string and so can't carry the prim-path hint. A new rule defaults to Full, so adding one never loads payloads. Full + Load stops once a load brings in nothing new.
  - P3 (7140d4d7b): graph-set cells get a marker and a tooltip naming the node. Draw edits aren't undoable, so Reset to Graph isn't either.
  - Bake to Node puts descendants' rules before their ancestors'. A new node takes the viewed node's place in every input it fed. The whole bake is one undo step.
  - The phase-1 gate was cancelled in favour of one full gate (35 GUI scripts).
- 2026-11-13 — Gate run 43433874523 on 7140d4d7b failed: `USDWriteTest.DrawRulesReachDownstreamInInputOrderAndNeverReachAWrite` (both legs) and four GUI scripts (`scene_bake_to_node`, `scene_draw_column`, `scene_prim_inspector`, `scene_expand_hierarchy`). Being diagnosed; the branch may also lack M329 from F4.
- 2026-11-13 — All five gate failures were tests written before P1.T2's default Initial Draw rule, or not reading M329's inherited rows (USDWrite test, `scene_draw_column`, `scene_bake_to_node`), plus a reconnect without disconnect (`scene_expand_hierarchy`) and an unretried read during a panel switch (`scene_prim_inspector`, least certain). Fixed in 6e616718e; re-gating with all 35 GUI scripts. For review: `SceneViewport::setScene`'s same-stack shortcut drops the load set and reach `setSceneEvaluatedFor` just recorded.
- 2026-11-13 — Gate on 6e616718e: ctest and 23 of 24 GUI scripts green. `scene_bake_to_node.py` found a real engine bug: undo/redo's knob restore calls `evaluateValueChange(-1)`, `KnobChoice::onInternalValueChanged(-1)` read the index as 0, and so the choice cached its first option (BBox) while `getValue()` kept the real index. `getRules()` read the stale cached option. Fixed in `KnobTypes.cpp` (dimension -1 reads dimension 0), with a gtest covering the bake's undo/redo (07ca208b9). The diff now touches `Engine/Knob*`, so the GUI bench rule applies at the gate. Re-gating.
- 2026-11-13 — Gate green on 07ca208b9 (ctest and 24 GUI scripts). F4 merged in (a3c070155, no conflicts), PR #276 into F4 opened, and the review is running. The re-gate is running. The GUI bench is deferred until natron-dev is idle (agents are running syntax checks in it).
- 2026-11-13 — PR #276 review round 1: 16 findings. High: after a stale-skipped sync, the same-stack shortcut can leave the viewer on an old stack. Medium: big policy removals run inline on the GUI thread; first frames are drawn before the policy is matched; origin-only changes go stale; load batches re-match whole-stage; rule merges are O(n²); a big bake rebuilds thousands of knobs. PM calls: Bake's undo merges its snapshot back rather than replacing later edits; the panel gets an explicit cancel for pending Full + Load. Being fixed; the re-gate was cancelled.
- 2026-11-13 — Review round 2 (final): 13 of 16 fixed and 3 partly; the fix had introduced a 500 ms hold that froze camera repaints during playback. Fixed in 751a52e29. PM direction: never block paintGL. Only a newly viewed node's first sync hides the stage (at most 500 ms, with guides and camera still drawn); every other sync keeps drawing with the last policy. Accepted limit: a second Bake within 500 ms of the first can add duplicate rows (rules are only added). Gate running; the GUI bench follows.
- 2026-11-14 — GUI bench (Xvfb, gate AppImage of 751a52e29, against `baselines/xvfb.jsonl`): 0 regressions. `paste_1000` at 3000 nodes was already over budget in the baseline (6.2 s, now 4.8 s); most ops are faster. GUI failures on the gate were script issues: a name clash in the bake script, and the expand script not waiting for the viewer reconnect (fixed in the latest commit). `scene_set_on_node` 'missing' was the gate failing to read the job's conclusion; it ran clean locally. GUI-only rerun in progress.
- 2026-11-14 — The GUI-only rerun is green (bake 68/68, expand 32/32, set-on-node 54/54). Waiting on PR #276 CI, then merge into F4.
- 2026-11-14 — Merged into F4 (PR #276, a069803e6) after two review rounds, green PR CI and a clean GUI bench.
