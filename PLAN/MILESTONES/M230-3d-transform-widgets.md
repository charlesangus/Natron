# M230 - 3D Transform Widgets

Houdini-style transform tools in the Viewer's 3D view. Translate, rotate and scale widgets
work on any selected prim, with shortcuts. A preference chooses where a drag writes its
change; the user wants to try all three policies (2026-10-26 grill). Today the only gizmo is
M20's: `ViewerTab.cpp` picks the USDEditTransform whose panel was opened last and places
handles on its first match (`drawTransformHandles`, `getHandleTarget` in
`Gui/SceneViewport.cpp`). Survey: `PLAN/DESIGN/2026-10-26-3d-mvp-survey.md`.

## Scope

**In:**
- Translate, rotate and scale widgets on the current selection, using Houdini shortcuts: `T` translate, `R` rotate, `E` scale, `Y` cycles, `Esc`/`Q` select tool. All are rebindable in the 3DViewer group (M228 - 3D Viewer Navigation).
- Local and world space, with pivot at the selection centroid or the current prim's pivot.
- A Preferences choice for the drag target policy:
  1. **Owner, else edit (default).** Write the knobs of the node that created the prim when it has TRS knobs and sits in the viewed branch (USDXformPrim, USDCamera, USDLight, the primitives). Otherwise use an open USDEditTransform that targets the prim. Otherwise create a USDEditTransform.
  2. **Open edit node.** Write the USDEditTransform whose panel is open, creating one if none is.
  3. **Always edit node.** Solaris-style: drags land in a USDEditTransform at the end of the viewed branch, reused across drags of the same selection.
- A created USDEditTransform is inserted downstream of the viewed node, with its pattern set to the selection.
- One drag is one undo step, including node creation.

**Out:**
- Point/component editing — M54 - SOPs-Style Geometry.
- Snapping and numeric entry in the viewport — none.

**Edge cases:**
- A multi-prim selection under policy 1 whose prims have different owners: each owner's knobs are written in the same undo step. Prims with no owner share one created USDEditTransform.
- A prim whose owner node sits upstream of a re-parenting USDXformPrim: the owner's knobs are in its local space, so the drag converts the world delta into the owner's parent space.
- An animated knob: the drag keys at the current frame.
- Policy 3 when the viewed node is itself a USDEditTransform targeting exactly the selection: reuse it.
- A prim from a USDRead (no owner) under policy 1: falls through to an edit node.

**Assumptions:**
- A prim's owner node can be found by matching the prims each node in the viewed branch defines (each create node's Prim Path, offset by any downstream re-parenting) without a new engine API. If that proves fragile, record a provenance map from M231 - Scene Graph Panel's "authored by" query instead.

## Phase 230.1: Widgets

- [x] M230.P1.T1 — Translate/rotate/scale widgets on the selection
  - files: `Gui/SceneTransformWidget.{h,cpp}` (new, factoring the M20 gizmo out of `Gui/SceneViewport.cpp`), `Gui/SceneViewport.{h,cpp}`, `Gui/ActionShortcuts.cpp` (T/R/E/Y/Q bindings), `Gui/CMakeLists.txt`
  - approach: a tool state on SceneViewport, with handles drawn at the selection pivot in local or world space. A drag produces a world-space delta matrix per selected prim and hands it to a target-policy interface (M230.P2.T1). In this task a test policy just records the deltas. Move the M20 USDEditTransform-panel gizmo onto this widget.
  - verify: Xvfb GUI script with a recording test policy — `T` then dragging the X handle 100 px yields a translation along X only; `R` and `E` produce rotation and scale deltas; the space toggle changes the axis frame.
  - size: L

## Phase 230.2: Target policies

- [x] M230.P2.T1 — Owner lookup and the three drag-target policies
  - files: `Gui/SceneTransformTarget.{h,cpp}` (new), `Engine/Settings.{h,cpp}` (policy preference), `Gui/ViewerTab.cpp`, `Tests/gui/scene_transform_policies.py`
  - approach: resolve each selected prim's owner by walking the viewed branch upstream: create nodes match by Prim Path rewritten through downstream USDXformPrim/USDScopePrim re-parenting; USDEditTransform nodes match by pattern. Implement the three policies. Owner writes convert the world delta into the owner's parent space and set its TRS knobs. Edit-node writes set the USDEditTransform's matrix knobs. Node creation goes through the existing node-creation + insert-after path, with the pattern set to the selection's paths. Wrap each drag in one undo macro.
  - verify: Xvfb GUI script, per policy — dragging a USDCard under a USDXformPrim writes the expected node (Card's translate / the open edit / a new edit node at the branch end); Undo once restores the graph and knobs exactly; a USDRead prim under policy 1 creates an edit node.
  - size: L

**Verification gate:** release gate green; widget and policy GUI scripts pass under Xvfb; the user tries all three policies on real hardware and picks the default; screenshots shared before sign-off.

## Decisions

- 2026-10-28: GUI checks pass on Actions (M269 remote runs); AppImage and UAT doc in build/appimages/, contact sheet build/uat-shots/; awaiting user UAT.
- 2026-10-27 — PM: R/Y act as transform tools while the 3D view has focus (over the Viewer's own R/Y); the pivot toggle is menu-only. A created edit node is inserted after the viewed node and takes all its outputs (the scene edit applies downstream, not only in the Viewer). Handle drags preview and write once on release as one undo step.
