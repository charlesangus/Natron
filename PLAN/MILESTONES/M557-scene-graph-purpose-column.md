# M557 - Scene Graph Purpose Column

Add a Purpose column to the Scene Graph panel showing each imageable prim's computed purpose (default, render, proxy, guide), with inherited values dimmed and a tooltip naming the prim they come from, and a `{purpose:<value>}` predicate that works both in the panel's filter and in nodes' prim patterns. Member of F37 - Scene Graph Bulk Ops.

Today (origin/main): the model's columns are `eColumnName`, `eColumnType`, `eColumnKind`, `eColumnDrawState` (`Gui/SceneGraphModel.h`, `enum ColumnEnum`); kind is read with `prim.GetKind` (`Gui/SceneGraphModel.cpp` ~:131); headers in `headerData` (~:161), resize modes at `Gui/SceneGraphPanel.cpp` ~:1903-1906. The filter evaluates path expressions with a panel-only predicate library adding `{loaded:}` and `{active:}` to USD's (`filterPredicateLibrary`, `Gui/SceneGraphModel.cpp` ~:2970), while nodes evaluate prim patterns with USD's stock `UsdObjectCollectionExpressionEvaluator` (`Engine/Nodes/Scene/SceneOps.cpp` ~:941, ~:1238).

## Scope

**In:**
- A read-only, Purpose column after Kind, computed with `UsdGeomImageable::ComputePurposeInfo`; inherited values dimmed, tooltip "Purpose: proxy (from /a/b)".
- A `{purpose:<value>}` predicate (computed purpose) shared by the panel filter and node prim patterns; filter tooltip updated.

**Out:**
- Authoring or editing purpose from the panel (a node's job).
- Making `{loaded:}` / `{active:}` work in node patterns: they stay panel-only so a node's output never depends on viewer load state.
- Bulk load/draw commands (M556 - Scene Graph Bulk Commands) and expand commands (M558 - Scene Graph Expand Commands).

**Edge cases:**
- Non-imageable prims (scopes without imageable schema, materials, shaders): blank cell, never matched by `{purpose:}`.
- Unloaded payload prims: purpose computed from what is composed (the prim's own authored opinion and its ancestors').
- Cost: per-row purpose is computed from the parent row's cached purpose info (`ComputePurposeInfo(parentInfo)`), not by walking ancestors per `data()` call, and the cache is dropped with the row on model resets and stage edits.
- `{purpose:render}` in a node pattern evaluates against the node's input stage, so it agrees with what the column shows for the same stage.

## Phase 557.1: Column

- [x] M557.P1.T1 — Purpose column in the scene graph model (PR #360)
  - files: `Gui/SceneGraphModel.{h,cpp}`, `Tests/SceneGraphModel_Test.cpp`
  - approach: add `eColumnPurpose` after `eColumnKind`, a `ePurposeInheritedRole` (true when the purpose isn't authored on the prim) and a `ePurposeOriginRole` (source prim path). Cache `UsdGeomImageable::PurposeInfo` on the model's row node, computing a child's from its parent's cached info; blank for non-imageable prims. `DisplayRole` is the purpose token; sorting matches Kind's. Extend the model test with a fixture: an Xform authored `proxy` with a mesh child (inherited proxy), a mesh authored `guide`, a non-imageable `Scope`/`Material`.
  - verify: `ctest -R SceneGraphModel` passes with the new cases
  - size: M
- [x] M557.P1.T2 — Show the column with inherited values dimmed (PR #360)
  - files: `Gui/SceneGraphPanel.cpp` (header resize modes ~:1903-1906, delegates ~:396-445)
  - approach: set the Purpose column's resize mode like Kind's; draw inherited values with the disabled text colour the Draw State column uses for inherited states (or the same delegate approach), and set the tooltip from `ePurposeOriginRole`. Check the default panel still fits a 1920x1080 screen at scale 1 without a horizontal scrollbar at its default docked width; narrow Kind/Purpose if not.
  - verify: an Xvfb screenshot of the panel on a fixture with each purpose, shared with the user; `Tests/gui/scene_graph_panel.py` extended to read a purpose cell and its inherited flag
  - size: M

## Phase 557.2: Purpose Predicate

- [x] M557.P2.T1 — A shared `{purpose:}` predicate for the filter and node patterns (PR #360)
  - files: new `Engine/Nodes/Scene/ScenePredicates.{h,cpp}` (register in Engine's CMake), `Engine/Nodes/Scene/SceneOps.cpp` (~:941, ~:1238), `Gui/SceneGraphModel.cpp` (`filterPredicateLibrary` ~:2970, filter tooltip in `Gui/SceneGraphPanel.cpp` ~:1606), `Tests/Scene/` (new `ScenePredicates_Test.cpp`)
  - approach: the engine library is `UsdGetCollectionPredicateLibrary()` plus `purpose` (`{purpose:proxy}` matches imageable prims whose computed purpose is that token; a bare `{purpose}` is an error). The panel's library becomes the engine library plus `loaded` and `active`. Switch SceneOps' two evaluators to evaluate with the engine library (as the panel does through `PathExprEval` with a custom library), keeping their incremental-search behaviour. Add `{purpose:…}` to the filter tooltip. Test the predicate on a fixture through both entry points (a node pattern evaluation and the panel walk).
  - verify: `ctest -R 'ScenePredicates|SceneOps|SceneGraphModel'` green; existing prim-pattern tests unchanged
  - size: M

**Verification gate:** `tools/ci/remote/build.sh debug --tests --gui "scene_graph_panel.py"` green including the new gtests; the column screenshot approved by the user; the column and `{purpose:}` filter collected for F37 - Scene Graph Bulk Ops's UAT.

## Decisions
- 2026-11-23 — Computed purpose, inherited values dimmed, read-only (user).
- 2026-11-23 — `{purpose:}` filtering, in the panel and in node prim patterns (user).
- 2026-11-23 — `{loaded:}` and `{active:}` stay panel-only: node output must not depend on viewer load state (planner, from the deferred-matching rule).

- 2026-11-24 (PM): no Scene Graph column sorts, so Purpose does not either ("sorting matches Kind's"); review finding on #360 declined. Shared ScenePurposeDelegate dims inherited purposes in the prim picker too.
