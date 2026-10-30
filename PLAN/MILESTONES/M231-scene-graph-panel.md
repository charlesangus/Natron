# M231 - Scene Graph Panel

A dockable Scene Graph panel. It shows the composed scene of the node the viewer is looking
at, or of a node it is pinned to. It has a prim tree with type icons and search, two-way
selection sync with the Viewer's 3D view, and a read-only attribute view that names the
node whose layer authored each value. Prims can be dragged onto path knobs. Folding uses
Katana-style deferred loading: a folded branch is not drawn in the viewer, only its bbox.
No tree widget for scenes exists today. From the M19/M20 UAT feedback (2026-10-26). Survey:
`PLAN/DESIGN/2026-10-26-3d-mvp-survey.md`.

## Scope

**In:**
- The panel follows the active viewer's active input node, or a pinned node (a pin button and a node chooser), and updates when that node's scene re-evaluates.
- Prim tree with type/kind icons, name filter, and lazy child population.
- Selection syncs both ways with the 3D view's selection set (M228 - 3D Viewer Navigation), with multi-select.
- Attribute view for the current prim at the viewer's frame: name, type, value, time-sampled flag, and the authoring node (the strongest layer with an opinion, mapped back to its node).
- Drag one or more prims onto any prim path/pattern knob. Several prims join with spaces.
- Deferred loading. Folding a branch makes the viewer show only that branch's bbox, through a viewer-private session layer, never affecting USDRender or USDWrite. When the stage is under a prim budget (a preference, default 50,000 prims) the tree starts expanded and fully drawn; above it the tree starts folded at the top level.

**Out:**
- Editing attributes from the panel — none (edits are nodes; M52 - USD Layer-Stack Completion's USDEditPrim).
- Picker buttons on knobs and create-node-from-selection — M232 - Scene Selection To Nodes.
- Layer-stack/composition inspector panes — none.
- Saving fold state in the project — none (per-session, per node).

**Edge cases:**
- The viewed node changes kind (scene ↔ image): the panel shows "no scene" and keeps its pin if pinned.
- The pinned node is deleted: the panel unpins and follows the viewer.
- A 1M-prim stage: the tree populates children only on expand, and the filter searches incrementally on a worker thread with a cap and a "more…" row.
- Folding a prim that is not a model kind: `model:drawMode = bounds` needs `model:applyDrawMode = true`, so the session layer authors both on the folded prim.
- A selection in the viewer inside a folded branch (a picked bbox): it selects the folded prim.
- A filtered tree: selection sync still works for prims hidden by the filter (they stay selected).

**Assumptions:**
- `UsdGeomModelAPI` draw-mode `bounds` on the viewer stage's session layer makes UsdImaging skip populating the folded subtree, so folding really lightens Storm. M231.P3.T1 measures it.
- Payload prims in folded branches can stay unloaded (`UsdStage::Unload`) on the viewer stage, so heavy payloads never load until expanded.

## Phase 231.1: Panel and tree

- [ ] M231.P1.T1 — Scene graph model over a composed stage
  - files: `Gui/SceneGraphModel.{h,cpp}` (new `QAbstractItemModel`), `Gui/CMakeLists.txt`, `Tests/Scene/SceneGraphModel_Test.cpp`
  - approach: a lazy item model over a `SceneStage` (borrowed from the viewer, or from `SceneQueryPool` for a pinned node not being viewed): `canFetchMore`/`fetchMore` per prim, columns name/type/kind, icons by type, refreshed incrementally from `UsdNotice::ObjectsChanged` resynced paths instead of a reset. Thread rule: the model reads the stage only under the owner's lock (the viewer's worker lock from M19.P3.T2).
  - verify: gtest with a `QAbstractItemModelTester` — a reference `.usda` shows the expected tree; editing one node resyncs only its subtree (rows outside are not reset); a 100k-prim stage populates the root in under 50 ms.
  - size: M

- [ ] M231.P1.T2 — The Scene Graph panel: follow or pin, filter, selection sync
  - files: `Gui/SceneGraphPanel.{h,cpp}` (new `PanelWidget`), `Gui/Gui*.cpp` (panel registration in the pane menus), `Gui/SceneViewport.{h,cpp}` (selection signals), `Gui/ViewerTab.cpp`, `Tests/gui/scene_graph_panel.py`
  - approach: a dockable panel registered like the other panes. It follows the last active viewer's active input, or a pinned node (pin button plus a node-name chooser). A name filter runs on a worker with a result cap. Selection syncs both ways through the viewport's `selectionChanged(QStringList)` and a setter; guard against feedback loops.
  - verify: Xvfb GUI script — the panel lists the viewed scene; selecting a tree row highlights the prim in the 3D view and vice versa; pinning keeps the tree when the viewer switches input; deleting the pinned node unpins.
  - size: M

## Phase 231.2: Inspection and drag

- [ ] M231.P2.T1 — Attribute view with the authoring node
  - files: `Gui/SceneGraphPanel.cpp`, `Engine/Nodes/Scene/SceneOps.{h,cpp}` (layer → node lookup), `Engine/Nodes/Scene/SceneLayerMemo.h`, `Tests/Scene/SceneProvenance_Test.cpp`
  - approach: list the current prim's attributes at the viewer's frame (`UsdAttribute::Get`, `GetResolveInfo`). For the strongest opinion's layer, map the layer identifier to the node that emitted it, via a registry the memo fills when a node emits its layer. External file layers show the file path. Expose the lookup as `SceneOps::authoringNodeOf(attr)` for M230 - 3D Transform Widgets to reuse.
  - verify: gtest — on a USDCamera → USDEditTransform graph, the camera's focalLength resolves to USDCamera's node and its xformOp to USDEditTransform's; a USDRead attribute reports the file path. Xvfb screenshot of the view.
  - size: M

- [ ] M231.P2.T2 — Drag prims onto path and pattern knobs
  - files: `Gui/SceneGraphPanel.cpp` (mime data), `Gui/KnobGuiString.cpp` (accept drops on knobs flagged as prim path/pattern), `Engine/Nodes/Scene/SceneNodeKnobs.h` (the flag), `Tests/gui/scene_graph_drag.py`
  - approach: the tree emits `application/x-natron-prim-paths`. Scene nodes mark their Prim Path/Pattern `KnobString`s with a prim-path hint. Such knobs accept the drop: a path knob takes the first path, and a pattern knob takes all paths joined by spaces (valid for M52 - USD Layer-Stack Completion's `SdfPathExpression` and for today's union). The drop is one undo step.
  - verify: Xvfb GUI script — dragging two rows onto a USDPrune pattern knob sets `"/a /b"` and prunes both; Undo restores the old value.
  - size: M

## Phase 231.3: Deferred loading

- [ ] M231.P3.T1 — Fold to bbox in the viewer
  - files: `Gui/SceneViewport.{h,cpp}`, `Engine/Nodes/Scene/SceneStage.{h,cpp}` (a viewer session layer and load rules), `Gui/SceneGraphPanel.cpp`, `Engine/Settings.{h,cpp}` (prim budget), `Tests/Scene/SceneFold_Test.cpp`, `Tests/gui/scene_graph_fold.py`
  - approach: the viewer's `SceneStage` gets a private session layer. Folding a tree item authors `model:applyDrawMode = true` and `model:drawMode = bounds` on that prim there, and unloads payloads below it. Expanding removes both and loads. On first view, count prims (capped traversal): under the budget, expand all; over it, fold every top-level prim. Fold state lives per (viewer, viewed node) for the session. USDRender and query stages never see the session layer.
  - verify: gtest — folding a subtree on a SceneStage leaves Hydra's render index with no rprims under it (count via the engine's render index in a Storm test) and a bounds draw for it; a USDRender of the same node still renders the subtree. Perf: folding the top of the 100k-cube file drops viewer sync+draw time by an order of magnitude (record numbers in `## Decisions`). Xvfb screenshot of a folded branch's bbox.
  - size: L

**Prior verification gate:** release gate green; panel GUI scripts pass under Xvfb; fold perf numbers recorded; screenshots (tree, attribute view, folded bbox) shared before sign-off.

## Decisions

- 2026-10-27: release gate green with the C++20 TfWeakPtr compare fix, the model re-entrancy fix and valid usda in the fold tests; next: GUI check and screenshots, then AppImage/UAT.

## Phase 231.4: UAT folding model

- [ ] M231.P4.T1 — Separate transient tree expansion from authored scene folding
  - files: Scene Graph model/panel, 3D viewport population and bbox display, USDRead controls, GUI tests
  - approach: Make closed tree items draw their bounding boxes; keep panel twirl state transient; add an authored graph operation for persistent expand/fold and a USDRead default-loading policy that can open the first N layers fully and leave deeper content as bboxes.
  - verify: Heavy-scene fixture opens with configured depth, closed prims show bboxes, transient twirls reset when leaving the node, and the authored operation persists.
  - size: L

- [ ] M231.P4.T2 — Let the Scene Graph follow node selection or the Viewer
  - files: Scene Graph panel source binding, node/viewer selection signals, GUI tests
  - approach: Add an explicit panel mode that follows the selected scene-producing node or the active Viewer, with pinning still available and no ambiguous automatic switching.
  - verify: The GUI test switches nodes and viewers in each mode and confirms the tree displays the chosen source.
  - size: M

**Verification gate:** panel tests and fold performance pass; bbox folding, load-depth policy and follow mode are accepted from the replacement AppImage.
