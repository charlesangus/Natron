# M556 - Scene Graph Bulk Commands

Load, unload and set draw states on whole branches of the Scene Graph panel in one action. A new toolbar row under the filter row holds a **scope** combo (Selected, Below, To Assembly, To Component, To Subcomponent, then each registered custom kind) and buttons Load, Unload, BBox, Children, Full, Load All, Hidden and Clear Below that act on the selected rows in that scope; the row menu gets the same scoped entries. "Load To Component" on an Island branch such as `isBayCedarA1` loads every payload down to and including its components; "Full Below" sets Full on the rows and clears every state below them. Member of F37 - Scene Graph Bulk Ops.

Today (origin/main): the row menu has Load, Load With Descendants and Unload over the selection (`Gui/SceneGraphPanel.cpp` `showRowMenu`, ~:2305), and a Draw State submenu calling `SceneViewport::setDrawStateOnRows`, `clearDrawStatesBelowRows` and `resetDrawStatesToGraphOnRows` (`Gui/SceneViewport.cpp` ~:9353), which batch `SceneDrawRowEdit`s into one `editDrawStateRows` change. Loads go through `SceneViewport::requestLoad` → `SceneStage::requestLoad(paths, withDescendants, excluded)` (`Engine/Nodes/Scene/SceneStage.cpp` ~:543), applied with `UsdLoadWithDescendants`/`UsdLoadWithoutDescendants`. Load All already "loads what it draws" (`Engine/Nodes/Scene/SceneDrawStates.h`), so Load All + Clear Below is the two-step version of the user's isBayCedarA1 example.

## Scope

**In:**
- A kind-walk helper classifying prims by kind level and finding, under a set of roots, the prims a scope acts on.
- Load To <kind>: load level by level down to and including prims of that kind; payloads nested below them stay unloaded. Async; Cancel Loads stops it.
- Unload Below <kind>: keep each prim of that kind loaded and unload the payloads below it.
- Draw <state> Below (set on the rows, clear below, one change) and Draw <state> To <kind> (set on every loaded prim of that kind under the rows).
- The toolbar row, the scoped row-menu entries, invokable methods for scripts, and editable shortcuts (unbound by default) for each action × scope.
- A status line naming what each action did ("Full set on 12 components under 1 row; 3 unloaded payloads skipped").

**Out:**
- Expand/collapse commands and the toolbar's Expand/Collapse buttons (M558 - Scene Graph Expand Commands).
- The Purpose column and `{purpose:}` predicate (M557 - Scene Graph Purpose Column).
- Undo for draw-state edits: none exists today and this milestone adds none.
- Loading payloads for Draw To <kind> (user: draw acts on loaded prims only).

**Edge cases:**
- Kind levels: assembly < component < subcomponent. A custom kind counts as the built-in it derives from (`KindRegistry::IsA`) for the walk's stop rule, but a custom-kind scope acts only on exact matches. Plain `group` prims and unkinded prims are passed through.
- Stop rule: the walk descends through pass-through prims and stops at the first prim whose level is at or below the target level — acting on it if it matches — and never descends further, so Load To Assembly stops at a component reached without passing an assembly, and Load To Subcomponent descends through components.
- Load To <kind>: pass-through prims with unloaded payloads are loaded without descendants, then the walk continues into what they composed; a matching prim is loaded without descendants. The walk ends when a pass ends with no new loads. Cancel Loads and a stage change end it.
- Unload Below <kind>: for each prim of that kind, unload its top-most descendants with payloads; the kind prim stays loaded. "Below" for Unload is disabled: USD's Unload already unloads everything below.
- Draw To <kind> visits composed prims only; prims inside unloaded payloads aren't touched and the status line counts the skipped payloads.
- Draw buttons show checked when every selected row has that state set explicitly (unconfirmed, see Assumptions).
- Load and Unload buttons are disabled off the viewer's stage, as the menu entries are today (`loadsOnShownStage`); draw buttons need a linked viewer, as the Draw State submenu does.
- The scope combo is sticky per panel while Natron runs, not saved (user).
- The toolbar row fits the panel at its default docked width on a 1920x1080 screen at scale 1.

**Assumptions:**
- Unconfirmed: draw buttons show checked when every selected row has that state set explicitly, unchecked otherwise.
- Scripts reach these through invokable `SceneViewport` methods (`QMetaObject::invokeMethod`, as `Tests/gui/m335_render_draw_states.py` drives `setDrawStateOnRows`); if NatronGui's Python API exposes the 3D viewer or the panel, the methods are added there too.

## Phase 556.1: Engine

- [ ] M556.P1.T1 — Kind levels and the scope walk
  - files: new `Engine/Nodes/Scene/SceneKindWalk.{h,cpp}` (register in Engine's CMake source list), new `Tests/Scene/SceneKindWalk_Test.cpp` (register in `Tests/CMakeLists.txt`)
  - approach: `sceneKindLevel(TfToken kind)` → assembly / component / subcomponent / pass-through, via `KindRegistry::IsA` so custom kinds take their base's level; `registeredCustomKinds()` lists registry kinds other than USD's built-ins and `model`. `walkToKind(stage, roots, target)` walks composed prims depth-first from each root under the Edge cases' stop rule, returning the matching prims, the pass-through prims with unloaded payloads it met, and the count of unloaded payloads below matches. Target is a kind token; built-ins match by IsA, custom kinds exactly. Pure function over a `UsdStageRefPtr`, no Qt. Test on in-memory stages (assembly → group → components with nested payload subcomponents, unkinded Xforms, a custom kind derived from component). Bundled gtest is old: no `GTEST_SKIP`, no `ASSERT_TRUE(ptr)`.
  - verify: `ctest -R SceneKindWalk` passes
  - size: M
- [ ] M556.P1.T2 — Load To <kind> and Unload Below <kind> on the scene stage
  - files: `Engine/Nodes/Scene/SceneStage.{h,cpp}` (`requestLoad` ~:543, `requestUnload` ~:565, the pending-load apply ~:871-913), `Tests/Scene/` (new `SceneStageLoadToKind_Test.cpp` with small `.usda` fixtures of nested payloads under `Tests/fixtures/`)
  - approach: add `requestLoadToKind(roots, kind)`: a job on the stage that loads the walk's pass-through payload prims and matches without descendants, waits for that load to apply, re-walks, and repeats until a pass queues nothing; Cancel Loads (the existing cancel path) and a stage edit end it. Add `requestUnloadBelowKind(roots, kind)`: walk, then unload each match's top-most payload descendants. Reuse `P1.T1`'s walk; reuse the existing load queue and its async apply rather than a second loading path. Test against fixtures: Load To Component loads assemblies' and components' payloads but not subcomponent payloads inside components; Unload Below Component leaves components loaded with their nested payloads unloaded; cancel mid-walk leaves a consistent load set.
  - verify: the new test passes; existing `ctest -R 'SceneLoadSet|SceneStage'` stays green
  - size: L

## Phase 556.2: Viewer And Panel

- [ ] M556.P2.T1 — Scoped operations on the viewer
  - files: `Gui/SceneViewport.{h,cpp}` (next to `setDrawStateOnRows` ~:9353 and `requestLoad` ~:9054), the pinned-node load path the panel uses (`Gui/SceneGraphPanel.cpp` `Implementation::requestLoad` ~:1120)
  - approach: invokable methods `loadRows(paths, scope)`, `unloadRows(paths, scope)`, `setDrawStateOnRowsScoped(paths, state, scope)`, `clearDrawStatesBelowRows` unchanged; scope is a string: `selected`, `below`, or a kind token. Draw Below = one `editDrawStateRows` batch of `eKindSet` + `eKindClearBelow` per row; Draw To <kind> = `walkToKind` over the viewer's composed stage, then one batch of `eKindSet` edits. Load Selected/Below map to the existing `requestLoad(…, false/true)`; kind scopes call P1.T2. Each returns a short summary string for the status line. Pinned nodes follow the same rules as the existing load path.
  - verify: a gtest or a small `Tests/gui/` script calls each method by `invokeMethod` on a fixture and checks the resulting draw states and load set
  - size: M
- [ ] M556.P2.T2 — Toolbar row with the scope combo and action buttons
  - files: `Gui/SceneGraphPanel.cpp` (layout ~:1785-1875, below the filter row and its message line)
  - approach: a row of a scope `QComboBox` (Selected, Below, To Assembly, To Component, To Subcomponent, a separator, then `registeredCustomKinds()` as "To <Kind>") and compact `QToolButton`s Load, Unload, BBox, Children, Full, Load All, Hidden, Clear Below, each with an objectName (`sceneGraphBulk_<action>`) and a tooltip naming what it does in the current scope (Unload in a kind scope reads "Unload Below <Kind>"). Enablement as the Edge cases say; draw buttons checked per Edge cases; refresh on selection and model changes. The scope is a panel member, not saved. Buttons call P2.T1 and write its summary to the status label.
  - verify: an Xvfb screenshot at the default docked width on 1920x1080 shows the row without clipping (shared with the user per the screenshot rule); the GUI test in P3.T1 drives every button
  - size: M
- [ ] M556.P2.T3 — Scoped entries in the row menu and editable shortcuts
  - files: `Gui/SceneGraphPanel.cpp` (`showRowMenu` ~:2305), `Gui/ActionShortcuts.{h,cpp}` (Viewer3D group, next to `kShortcutIDActionViewer3DSetSelectionOnNode`)
  - approach: Load becomes a submenu (Selected, Below — keeping the "Load With Descendants" objectName on Below — then To <kind> entries); Unload gains Below <kind> entries; each Draw State entry gains a scope submenu or a following "… Below" / "… To <kind>" group, whichever reads cleaner in a screenshot. Register an unbound, editable shortcut per action × built-in scope and route it through the panel's existing keybind matcher (~:646).
  - verify: the P3.T1 GUI test triggers menu entries by objectName; the shortcut editor lists the new actions
  - size: M

## Phase 556.3: Tests And Gate

- [ ] M556.P3.T1 — GUI test for bulk commands
  - files: new `Tests/gui/scene_graph_bulk.py`, fixtures under `Tests/fixtures/` (small `.usda`, nested payloads, assembly/component/subcomponent kinds, one custom kind)
  - approach: open the fixture in a 3D viewer; select an assembly row; run Load To Component and check components loaded and subcomponent payloads not; run Full To Component and check every loaded component's Draw State is Full and ancestors read Children; run Load All Below on a row with child states and check they're cleared; run Unload Below Component; check button enablement on a pinned non-viewer node. Follow the PySide lifetime rules (never `parentItem()`; keep the window active).
  - verify: passes under Xvfb locally (`build/deeprepro/run-gui.sh` recipe) and in the remote `--gui` gate
  - size: M

**Verification gate:** `tools/ci/remote/build.sh debug --tests --gui "scene_graph_bulk.py scene_graph_panel.py scene_graph_payloads.py scene_draw_column.py"` green, with the P1 gtests; toolbar and menu screenshots shared with the user; Island `isBayCedarA1` Load To Component / Full Below collected for F37 - Scene Graph Bulk Ops's UAT.

## Decisions
- 2026-11-23 — Load To <kind> stops at the kind (user): payloads from the selection down to and including prims of that kind load; nested payloads below stay unloaded.
- 2026-11-23 — Draw <state> To <kind> sets the state on each prim of that kind (user); it acts on loaded prims only and never loads (user).
- 2026-11-23 — One scope combo plus action buttons, under the filter row (user); sticky per panel, not saved (user).
- 2026-11-23 — Kind scopes: Assembly, Component, Subcomponent, plus every registered custom kind, the custom ones matched exactly (user).
- 2026-11-23 — Unload with a kind scope is "Unload Below <kind>": the kind prim stays loaded and everything below it unloads (user).
- 2026-11-23 — Script access and editable shortcuts for every action and scope (user).
- 2026-11-23 — A Load To <kind> walk is ended by Cancel Loads and by switching the viewer's node, not by every stage edit: pinned sources and the viewer re-sync on each commit, which would cut multi-level walks short (PM, from the implementer; revisit if stale loads show up).
- 2026-11-23 — "Below" means in the prim hierarchy: the prim and its descendants, recursively, not rows lower in the panel (user).
- 2026-11-23 — Walk cancellation confirmed as implemented (user).
- 2026-11-23 — Draw <state> To <kind>: only ancestors that would block the target rows (BBox, Hidden, or an unset child of a Children row) switch to Children; ancestors inheriting Full stay Full.
