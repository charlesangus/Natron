# M612 - USDGeoModify Node

A scene node that edits USD prims in place with native geometry nodes, modelled on the reference app's SOP Modify:
- **Outside**, it takes a Scene input and a **Primitives** pattern (the existing prim-path picker, `KnobString::setPrimPathHint(Pattern)`), and outputs the input scene with those prims edited.
- **Inside** is a group (M159 - Kind-Agnostic Groups): its input node yields the matched prims as native geometry through USDToGeo with M611 - USDToGeo Source Controls; you add any geometry nodes; its output goes through GeoToUSD with M609 - GeoToUSD Path And Placement and M610 - GeoToUSD Attribute And Material Controls in Overlay mode.
- **On the way out**, the result is authored as overs at the original paths. The prims keep their place in the hierarchy, their parents' xforms, their bindings and every property the edit did not touch. Prims whose geometry was deleted inside are deactivated (toggle, default on). New geometry with a relative path goes beside its source prim, and geometry with no source goes under Path Prefix.
- **While you are inside**, the 3D viewer shows the rest of the stage as context (toggle, default on).

Registered in the `PLUGIN_GROUP_USD_*` Edit submenu (PLAN.md context, M520 - USD Toolbar Category And Submenus). Ships on its own under E9 - USD And 3D.

## Scope

**In:**
- Node USDGeoModify (Edit submenu): input Scene, a dive-in group with fixed Input and Output nodes.
- Outside knobs, labels matching script names:
  - `primitives` (pattern);
  - `unpackToPolygons` (off: packed, so point edits move whole prims, as in the reference);
  - the import options re-exposed from the inner USDToGeo (`importPrimvars`, `importAttributes`, `timeSample`, `applyInverseUsdTransform` on);
  - `topologyAttributes` (default Animated), re-exposed from the inner GeoToUSD;
  - `deactivateRemovedPrimitives` (on);
  - `pathPrefix` (default `/<node name>`);
  - `showStageInGeometry` (on).
- The inner GeoToUSD is fixed to Overlay with Adjust Transforms For Input Hierarchy on. Relative paths resolve against the source prim's parent; absolute paths are used as written.
- Viewer: while a node inside is viewed, the unmatched input stage draws as non-selectable context, cameras stay usable for look-through, and the edited geometry gets the full geometry display (markers, picking).
- Creation from a scene selection fills `primitives` (as M575 - USD Restructure And Graft does for its pattern).
- Docs `Engine/Nodes/Scene/docs/USDGeoModify.md`, gtests, an Xvfb GUI script and a params→knob audit.

**Out:**
- Layer breaks ("Perform Layer Break Before Import"): F27 - USD Layer Authoring owns layer breaks.
- Editing several unrelated stages; one Scene input only; no milestone.

**Edge cases:**
- Pattern matches nothing: the output is the input unchanged, and the node message says no prims matched.
- Topology changed inside with Topology Attributes None: the node warns that the counts don't match (M610's warning).
- A matched prim is an instance proxy: it is read through its prototype and edited as an over on the instance prim when the edit changes nothing but its xform. Other edits warn that instance proxies can't be overridden, and leave the instance untouched.
- New geometry whose relative path collides with an existing sibling: suffixed `_0`, `_1` (M609's rule).
- Deleting every prim's geometry with `deactivateRemovedPrimitives` off: the output equals the input.

**Assumptions:**
- Unconfirmed: a Group subclass whose fixed Input/Output nodes are kind-converting (scene in, geometry inside, scene out) fits M159 - Kind-Agnostic Groups' model without new group plumbing.
- Unconfirmed: "beside its source" is decided from a source-path attribute that survives the geometry ops (M611's `pathAttribute`). Primitives with no source path go under `pathPrefix`.

## Phase 612.1: Node

- [ ] M612.P1.T1 — Group node with scene-to-geometry Input and geometry-to-scene Output
  - files: new `Engine/Nodes/Scene/USDGeoModify.{h,cpp,reg.inc}`, `Engine/NodeGroup.{h,cpp}` (only if a hook is missing), `Engine/Nodes/Geometry/USDToGeo.h`, `Engine/Nodes/Geometry/GeoToUSD.h`, `Engine/CMakeLists.txt`, `Tests/Scene/USDGeoModify_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: Subclass the group node, which is kind-agnostic since M159. The inner Input node wraps a USDToGeo on the group's Scene input, with `primPattern` linked to the outer `primitives`. The inner Output wraps a GeoToUSD whose Scene input is the outer Scene input, fixed to Overlay with Adjust Transforms For Input Hierarchy on. The output scene is the input plus GeoToUSD's layer. Re-expose the listed knobs as links on the outer node. Register in the Edit submenu.
  - verify: gtest: a USDGeoModify with no inner nodes outputs a stage equal to its input (round trip, as in `GeoRoundTrip_Test`); with a GeoTransform inside moving +1 in X, the matched prim's points move and its parent xform and material binding are unchanged.
  - size: L
- [ ] M612.P1.T2 — Removed prims and new geometry placement
  - files: `Engine/Nodes/Scene/USDGeoModify.cpp`, `Engine/Nodes/Geometry/GeoUsdConvert.cpp`, `Tests/Scene/USDGeoModify_Test.cpp`
  - approach: After the inner cook, compare the matched set with the paths present in the output detail. With `deactivateRemovedPrimitives`, author `active=false` overs on the missing ones. For partitions with relative paths, resolve each against its source prim's parent, taken from the primitive's source-path attribute. With no source, use `pathPrefix`.
  - verify: gtest: GeoDelete of one of two matched prims deactivates it (and leaves it alone with the toggle off); a GeoScatter whose points keep the source path lands beside the source prim; a GeoPrimitive with no path lands under `/<node name>`.
  - size: M
- [ ] M612.P1.T3 — Stage context in the viewer while inside
  - files: `Gui/SceneViewport.cpp`, `Gui/GeoDisplay.{h,cpp}`, `Gui/ViewerTab.cpp`, `Engine/Nodes/Scene/USDGeoModify.cpp`
  - approach: When the viewed node is inside a USDGeoModify with `showStageInGeometry` on, the 3D view also loads the group's input stage with the matched prims hidden (a session-layer deactivation in the viewer's private layer). It is non-selectable, drawn as context, and its cameras are offered in Look Through. The geometry display and picking stay on the viewed detail.
  - verify: Xvfb script `Tests/gui/m612_usdgeomodify.py`: dive in, view the inner GeoTransform; the rest of the stage draws, a scene camera is listed in Look Through, and clicking a context prim selects nothing.
  - size: M
- [ ] M612.P1.T4 — Create from selection, docs and the params→knob audit
  - files: `Engine/Nodes/Scene/USDGeoModify.cpp`, `Gui/SceneSelectionActions.cpp`, `Engine/Nodes/Scene/docs/USDGeoModify.md`, `Tests/gui/m612_usdgeomodify.py`, this file's `## Decisions`
  - approach: Creating the node with prims selected in the viewer fills `primitives`, using the same hook as USDRestructure (M575). Write the docs without naming other applications. Audit every control of the reference SOP Modify (`.local/docs/houdini/nodes/lop/sopmodify.md`) as In or Out with a reason, and record the audit in `## Decisions`.
  - verify: the GUI script creates the node from a selection, dives in, adds a GeoTransform through the handle, and checks the output prim moved at its original path.
  - size: M

**Verification gate:** CI green; release gate green with `--gui` running `Tests/gui/m612_usdgeomodify.py`; `USDGeoModify_Test` and `GeoRoundTrip_Test` pass; the params→knob audit has no gaps; screenshots of the inside view with stage context are shared one file per screenshot and approved by the user before sign-off.

## Decisions
- 2026-11-25 — A dedicated in-place edit node built on the GeoToUSD/USDToGeo controls, which stay available for manual chains (user, grill).
- 2026-11-25 — Prims whose geometry is removed inside are deactivated, under a toggle that defaults on (user, grill).
- 2026-11-25 — New geometry with a relative or empty path goes beside its source prim, and under Path Prefix when it has no source (user, grill).
- 2026-11-25 — While inside, the rest of the stage shows as non-selectable context, under a toggle that defaults on (user, grill).
