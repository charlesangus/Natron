# M606 - Viewer Snapping

Snapping for 3D handle and component drags. **Grid snap** rounds translate to the 3D view's grid (`drawGrid`, `Gui/SceneViewport.cpp` ~:3231). **Increment snap** steps translate, rotate and scale by fixed amounts while Ctrl is held. A toggle button with a mode menu sits in M602 - 3D Viewer Toolbars' `snapSlot`, and holding V snaps temporarily.

Member of F40 - 3D Viewer Tools.

## Scope

**In:**
- Grid snap for translate drags (on the grid plane, and on the axis for single-axis drags), at the drawn grid spacing.
- Increment snap with Ctrl: translate step, rotate step (default 15°), scale step (default 0.1), set in the snap menu and saved as viewer settings.
- Snap toggle plus mode menu (Grid / Increments) in the left tool bar; V held inverts the toggle.
- Applies to USD prim handles (M603 - Combined Transform Handle), geometry node handles (M604 - GeoTransform Viewer Handles) and GeoEdit drags (M605 - GeoEdit Component Transforms).

**Out:**
- Snapping onto geometry points, edges or faces (user, 2026-11-25); no milestone.
- Gravity, priorities and orient-on-snap; no milestone.

**Edge cases:**
- Grid snap on a rotated Local-space axis snaps the world-space result's projection onto the grid, not the local offset.
- Increments while Shift fine-drag is held: the increment wins.
- V while a text field has focus does nothing.

**Assumptions:**
- Unconfirmed: the grid spacing is readable from the renderer (`SceneViewportRenderer`) for the current view distance.

## Phase 606.1: Snapping

- [ ] M606.P1.T1 — Snap maths for grid and increments
  - files: `Gui/SceneGizmo.{h,cpp}` (`computeDragChange`), `Tests/SceneGizmo_Test.cpp`
  - approach: A `SnapSettings` struct (mode, grid spacing, translate/rotate/scale steps) passed into `computeDragChange`, which rounds each kind's result: grid for translate (world-space result rounded on the active axes), steps for increments. With no snapping it is unchanged.
  - verify: gtest: translate 0.37 with grid 0.25 → 0.25; rotate 22° with a 15° step → 15°; scale 1.23 with a 0.1 step → 1.2.
  - size: M
- [ ] M606.P1.T2 — Snap button, menu, V hold and settings
  - files: `Gui/SceneToolBar.cpp`, `Gui/SceneViewport.cpp`, `Gui/ActionShortcuts.{h,cpp}`, `Engine/Settings.{h,cpp}`, `Gui/Resources/Images/`, `Tests/gui/m606_viewer_snapping.py`
  - approach: Fill `snapSlot` with a checkable button and a mode/step menu. V press and release invert the toggle while held, and Ctrl requests increments. Every drag path (prim handles, geometry handles, components) builds `SnapSettings` from this state, reading grid spacing from the renderer.
  - verify: Xvfb script: grid snap on, a translate drag on a USDXformPrim lands on a grid multiple; the same with a GeoTransform handle and a GeoEdit point drag; Ctrl-rotate gives 15° steps.
  - size: M

**Verification gate:** CI green; release gate green with `--gui` running `Tests/gui/m606_viewer_snapping.py`; `SceneGizmo_Test` passes; a screenshot of the snap menu is shared and collected for F40 - 3D Viewer Tools' UAT.

## Decisions
- 2026-11-25 — Split out of M602 - 3D Viewer Toolbars as its own milestone (user, grill).
- 2026-11-25 — Grid and increment snapping only; no snapping onto geometry (user, grill).
