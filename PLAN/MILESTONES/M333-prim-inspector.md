# M333 - Prim Inspector

Full inspection of the selected prim: attributes, relationships and metadata, composition arcs,
an opinion stack mapped back to nodes, what the viewed node changed, and layer text. Each node
emits one layer, so every opinion can name its node; none of the reference tools' docs describes
that. Design: `PLAN/DESIGN/2026-11-04-scene-graph-design.md`. usdview's tabs are the model for
USD API use (`usdviewq/appController.py:4298-4314,4441-4509,4604-4607`).

## Scope

**In:**
- A tabbed inspector in the panel, reading on a worker, for the selected prim only.
- Attributes, relationships and metadata at the current frame, each naming its authoring node or file.
- Composition tab, opinion stack, Changed Here marker and filter, read-only layer text.
- Tabs can be added, so a Geometry Spreadsheet can sit here later.

**Out:**
- Editing from the inspector — none; edits are nodes (M52 - USD Layer-Stack Completion's USDEditPrim).
- Per-element data — M337 - Geometry Spreadsheet.
- Diffing arbitrary nodes — none; Changed Here and two panels cover it.

**Edge cases:**
- An unloaded payload shows its authored arc with no target contents, read through `HasAuthoredPayloads` and `GetMetadata("payload")`. The expanded index has no target node for it, so the tab says "not loaded".
- `UsdPrimCompositionQuery` recomputes on every construction, so it runs on selection only and never per row.
- A prim instance proxy or prototype: `GetPrimIndex` is empty on prototypes, so show the prototype link.
- A selection change mid-read cancels the old read.
- A layer from a file shows the file path; only node layers open a node.
- Long values are cut in the table, with the full value in a tooltip.
- The stage lock is busy with a load: the inspector shows its last result greyed rather than blocking.

## Phase 333.1: Inspector

- [x] M333.P1.T1 — Tabbed inspector with attributes, relationships and metadata
  - files: `Gui/ScenePrimInspector.{h,cpp}` (new), `Gui/SceneGraphPanel.cpp`, `Gui/CMakeLists.txt`, `Tests/gui/scene_prim_inspector.py`
  - approach: move the attribute view out of `SceneGraphPanel.cpp:1160,1648-1671` into its own widget with a tab list, reading on a worker under the stage lock and posting results. Add relationships (`GetRelationships`, targets) and metadata (`GetAllMetadata`). Keep `authoringNodeOf` for the source column. Add `GetResolveInfo` source kind (default, value, samples, fallback).
  - verify: GUI script — on USDCamera → USDEditTransform, focal length names USDCamera and the transform names USDEditTransform; relationships and metadata list; no read runs on main (heartbeat within budget on a selection change on the flat file).
  - size: L

- [x] M333.P1.T2 — Composition tab
  - files: `Gui/ScenePrimInspector.cpp`, `Engine/Nodes/Scene/SceneOps.{h,cpp}`, `Tests/Scene/SceneProvenance_Test.cpp`
  - approach: walk `GetPrimIndex().GetRootNode()` for the arc tree, using `UsdPrimCompositionQuery` for arc type, ancestral flag and introducing layer. Layers map to nodes through the existing owner registry. Pxr types stay behind `SceneOps`.
  - verify: gtest — a prim with a reference, a payload, a variant and an inherit reports each arc and layer; an unloaded payload reports "not loaded". Screenshot shared.
  - size: M

- [x] M333.P1.T3 — Opinion stack
  - files: `Gui/ScenePrimInspector.cpp`, `Engine/Nodes/Scene/SceneOps.{h,cpp}`, `Tests/Scene/SceneProvenance_Test.cpp`
  - approach: for a selected attribute, `GetPropertyStack(time)`; for a prim, `GetPrimStackWithLayerOffsets`. List strongest to weakest with layer, node or file, and value where there is one. Double-clicking a node row opens its settings panel.
  - verify: gtest — USDCamera → USDEditTransform → USDSetAttribute on one attribute lists those nodes in strength order with their values; GUI script — double-click opens the right panel.
  - size: M

## Phase 333.2: Node contribution

- [ ] M333.P2.T1 — Changed Here marker and filter
  - files: `Gui/SceneGraphModel.{h,cpp}`, `Gui/SceneGraphPanel.cpp`, `Engine/Nodes/Scene/SceneOps.{h,cpp}`, `Tests/gui/scene_changed_here.py`
  - approach: read the viewed node's own layer specs (`SdfLayer::Traverse`) on a worker into a path set; rows on it or with a descendant on it get a marker. A Changed Here toggle filters the tree to those. Reads a layer only, so scene size doesn't matter.
  - verify: GUI script — viewing a USDEditTransform marks its target and ancestors only; viewing a node that authors nothing shows none; the toggle filters and restores.
  - size: M

- [ ] M333.P2.T2 — Layer text view
  - files: `Gui/ScenePrimInspector.cpp`, `Engine/Nodes/Scene/SceneOps.{h,cpp}`, `Tests/gui/scene_prim_inspector.py`
  - approach: a read-only text tab with two modes: the viewed node's contribution (its layer as USDA), and the selected prim's composed definition, flattened into an anonymous layer and exported as USDA without its descendants. Built on a worker, only while the tab is showing, with no size cap.
  - verify: GUI script — the node contribution contains the authored `over`, and switching the prim updates the composed mode.
  - size: M

**Verification gate:** release gate green; GUI scripts pass under Xvfb; a selection change on the flat file stays within the stall budget; screenshots of each tab shared before sign-off; accepted from the UAT AppImage.
