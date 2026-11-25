# M603 - Combined Transform Handle

A fifth 3D tool, **Transform** (key Y), with one gizmo that has move arrows and planes, rotate rings and scale boxes together, as in the reference app. It sits beside the single-mode Select/Translate/Rotate/Scale tools, which stay. The gizmo also gets the reference app's full set of handle options: World/Local/Parent space; the existing pivots; an editable pivot with pivot-move mode (Insert); handle size (+/-); a persistent handle; a detached handle ('); MMB drag anywhere; and K to key the target's transform knobs. The options are surfaced in the left tool bar and the handle's own right-click menu. Built on the existing GL-free gizmo (`Gui/SceneGizmo.h`), the multi-prim widget (`Gui/SceneTransformWidget.h`) and the target policy (`Gui/SceneTransformTarget.{h,cpp}`), so multi-selection editing works as it does today.

Member of F40 - 3D Viewer Tools.

## Scope

**In:**
- `SceneTransform::eToolTransform` with key Y (rebindable, in `ActionShortcuts`), a left-bar button in M602 - 3D Viewer Toolbars' `transformToolSlot`, and a context-menu entry.
- Combined gizmo: translate arrows plus plane squares, rotate rings, scale boxes plus a centre uniform-scale box, hit-tested together without overlap (rings outside, boxes at the arrow tips).
- Handle options: Space (World / Local / Parent); Pivot (Selection Centre / Current Prim, existing, plus an edited pivot); pivot-move mode (Insert toggles it; dragging moves the pivot only); handle size (+/-, saved as a viewer setting); Persistent (the handle stays on its target while the selection changes); Detach (' leaves the pivot where it is); MMB drag anywhere in the view drives the last-used handle part.
- K keys every transform knob the handle drives, at the current time. Drags on already-animated knobs auto-key, as knob edits do now.
- The options live in the left tool bar (space and pivot menus) and in a right-click menu on the handle itself. The 3D view's general context menu keeps its Transform submenu.

**Out:**
- Geometry handles (M604 - GeoTransform Viewer Handles) and component transforms (M605 - GeoEdit Component Transforms); this milestone covers USD prims.
- Snapping (M606 - Viewer Snapping).

**Edge cases:**
- An edited pivot is viewer state per target node. When the target node has a pivot knob (USDEditTransform), the edited pivot writes that knob instead.
- K on a target with no animatable transform knobs does nothing and shows a status message.
- K or Y collides with another 3D-view shortcut: the implementer checks `ActionShortcuts` and reports any collision rather than silently rebinding.
- Multi-selection with Local space: each prim rotates and scales about its own frame, with the handle on the current prim, as `SceneTransformWidget::computeFrame` does now.
- Persistent handle whose target is deleted: the handle clears.

**Assumptions:**
- Unconfirmed: Parent space is the current prim's parent world frame, computed from the evaluated stage, the same way Local space is.

## Phase 603.1: Combined gizmo

- [ ] M603.P1.T1 — Combined gizmo geometry and hit-testing
  - files: `Gui/SceneGizmo.{h,cpp}`, `Tests/SceneGizmo_Test.cpp` (new or extended), `Tests/CMakeLists.txt`
  - approach: Add a combined handle set to `buildHandles` that places translate arrows, plane squares, rotate rings, axis scale boxes and a centre uniform box with no overlapping hit areas. Extend `HandleKindEnum` (`SceneGizmo.h` ~:44) only where a kind is missing, such as plane-translate or uniform-scale. `hitTest` priority is centre box, then arrow tips/boxes, then arrows, then planes, then rings. `computeDragChange` is unchanged per kind.
  - verify: gtest builds the combined set for a camera and frame and checks that a ray through each part hits only that part, plus one drag per kind gives the same change as the single-mode tool.
  - size: M
- [ ] M603.P1.T2 — Transform tool, key Y and the left-bar button
  - files: `Gui/SceneTransformTarget.h` (`ToolEnum` ~:46), `Gui/SceneViewport.cpp` (`kTransformTools` ~:7125, `setTransformTool` ~:12113, `cycleTransformTool` ~:12131, `showContextMenu` Transform submenu ~:14070), `Gui/ActionShortcuts.{h,cpp}` (~:533), `Gui/SceneToolBar.cpp`, `Gui/Resources/Images/`
  - approach: Add `eToolTransform` and a `kTransformTools` entry with a new shortcut ID, default Y. Draw the combined set from T1 when the tool is active. Show the button in M602's `transformToolSlot`. Include the tool in `cycleTransformTool`.
  - verify: `Tests/gui/m603_combined_handle.py` under Xvfb: Y selects the tool and checks its button; a scripted drag on the rotate ring of a USDXformPrim's prim changes its rotate knob; a drag on an arrow changes translate.
  - size: M

## Phase 603.2: Handle options

- [ ] M603.P2.T1 — Parent space and the space/pivot menus in the tool bar
  - files: `Gui/SceneTransformWidget.{h,cpp}` (`computeFrame` ~:60), `Gui/SceneToolBar.cpp`, `Gui/SceneViewport.cpp`
  - approach: Add Parent to the space enum, as the current prim's parent world frame. Add Space and Pivot drop-down buttons to the left tool bar, sharing actions with the context-menu Transform submenu.
  - verify: gtest on `computeFrame` for Parent space under a rotated parent; the GUI script switches space from the bar and checks that the context menu agrees.
  - size: M
- [ ] M603.P2.T2 — Editable pivot and pivot-move mode
  - files: `Gui/SceneTransformWidget.{h,cpp}`, `Gui/SceneViewport.cpp`, `Gui/ActionShortcuts.{h,cpp}`, `Engine/Nodes/Scene/USDEditTransform.cpp` (pivot knob, if present)
  - approach: Insert toggles pivot-move mode; in it, drags move the pivot and not the target. Store the pivot per target node in viewer state; when the target has a pivot knob, write that knob instead. Add an "Edited Pivot" pivot choice, selected automatically after a pivot move. Draw the handle differently in pivot mode (an outline only).
  - verify: GUI script: Insert, drag, Insert, rotate; the prim rotates about the moved pivot; with USDEditTransform its pivot knob changed.
  - size: M
- [ ] M603.P2.T3 — Handle size, persistent and detached handles, MMB drag
  - files: `Gui/SceneViewport.cpp`, `Gui/SceneGizmo.cpp`, `Engine/Settings.{h,cpp}` (handle size setting), `Gui/ActionShortcuts.{h,cpp}`
  - approach: +/- scale the on-screen handle size (a viewer setting). Persistent keeps the current target and frame while the selection changes. ' detaches: the handle stays put and drags apply as deltas to the selection. MMB-drag outside the handle repeats the last handle part used. Add all of these to a right-click menu on the handle (a hit on the handle opens it instead of the general menu).
  - verify: the GUI script exercises each: +/- changes the drawn size, a persistent handle survives a selection change, an MMB drag in empty space translates along the last axis.
  - size: M
- [ ] M603.P2.T4 — K keys the handle's transform knobs
  - files: `Gui/SceneViewport.cpp`, `Gui/SceneTransformTarget.{h,cpp}` (`SceneTransformPolicyTarget::commit` ~:651), `Gui/ActionShortcuts.{h,cpp}`
  - approach: K resolves the same target `commit` would write, and sets a key at the current time on each transform knob it drives (translate, rotate, scale, uniformScale), as one undo step. Report through a status message when there is no target.
  - verify: GUI script: K on a USDXformPrim target puts keys on its four transform knobs at the current frame; undo removes them.
  - size: M

**Verification gate:** CI green; release gate green with `--gui` running `Tests/gui/m603_combined_handle.py` under Xvfb at 1920x1080; `SceneGizmo_Test` passes; screenshots of the combined handle, pivot mode and the handle menu are shared one file per screenshot and collected for F40 - 3D Viewer Tools' UAT.

## Decisions
- 2026-11-25 — The "multi-edit widget" means a combined move/rotate/scale handle; multi-selection editing comes from the existing multi-prim widget (user, grill).
- 2026-11-25 — The combined handle is a fifth tool on Y, with the single-mode tools kept (user, grill).
- 2026-11-25 — The full set of reference-app handle options: space incl. parent, editable pivot, size, persistent, detach, MMB anywhere (user, grill).
- 2026-11-25 — K keys the target's transform knobs; drags on animated knobs auto-key (user, grill).
