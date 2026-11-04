## Pending

### 2026-11-04T12:00:00Z — change-request
- refs: M231, M232
- Scene graph redesign (user, 2026-11-04; `PLAN/DESIGN/2026-11-04-scene-graph-design.md`, decision `DECISIONS/2026-11-04-scene-graph-draw-model.md`). Folding is removed from both milestones; they merge as a plain panel and selection tools, and M327 - Heavy Scene Spike onward build the new model on top. This answers the 2026-11-02 hold on M231 - Scene Graph Panel, so set its `Status` back to `doing` when you resume it. Merge M231 - Scene Graph Panel first, then M232 - Scene Selection To Nodes. Both need the user's UAT before merging: the "approved" and "Merged without UAT" text on PRs #165 and #166 is stale, so correct the PR bodies. Update the UAT steps (drop steps 9-13 and 15 from M231's) and rebuild the AppImages.

  **M231-scene-graph-panel.md**

  In `## Scope`: delete the In bullet starting "Deferred loading." and the Out bullet "Saving fold state in the project". Change the Out bullet "Layer-stack/composition inspector panes — none." to "Inspector tabs beyond attributes — M333 - Prim Inspector." Add Out bullets: "Deferred loading, draw states and boxes — M328 - Deferred Viewer Stage, M329 - Viewer Draw States." and "Graph-authored expansion — M330 - Graph Draw Policy." Delete the Edge cases "Folding a prim that is not a model kind" and "A selection in the viewer inside a folded branch", and the whole `**Assumptions:**` list.

  Append:

  ## Phase 231.5: Strip folding

  - [ ] M231.P5.T1 — Remove graph-side folding
    - files: `Engine/Nodes/Scene/USDRead.{h,cpp}`, `Engine/Nodes/Scene/docs/USDRead.md`, `Engine/Nodes/Scene/USDExpandHierarchy.*` and its docs (delete), `Engine/Nodes/Scene/USDWrite.cpp`, `Tests/Scene/USDRead_Test.cpp`, `Tests/Scene/USDWrite_Test.cpp`, `Tests/Scene/USDExpandHierarchy_Test.cpp` (delete)
    - approach: remove Open Mode, Open Levels and the `natron:viewerFold` and `natron:viewerOpenLevels` custom data, including USDWrite's strip. Delete USDExpandHierarchy and its registration. M330 - Graph Draw Policy rewrites it later and takes what it needs from history.
    - verify: grep finds no `viewerFold`, `viewerOpenLevels` or `USDExpandHierarchy` in the tree; USDRead and USDWrite gtests pass; a USDWrite export carries no Natron custom data.
    - size: M

  - [ ] M231.P5.T2 — Remove viewer folding
    - files: `Engine/Nodes/Scene/SceneStage.{h,cpp}`, `Gui/SceneViewport.{h,cpp}`, `Gui/SceneGraphPanel.cpp`, `Engine/Settings.{h,cpp}`, `Tests/Scene/SceneFold_Test.cpp` (delete), `Tests/gui/scene_graph_fold.py` (delete)
    - approach: remove `SceneStage`'s fold API, listener, session-layer draw-mode authoring and fold load rules, `computeViewerFolds`, the viewport's fold state, the panel's row mirroring and fold delegate, and the prim budget preference. Twirling becomes plain tree state that never reaches the stage. The viewer draws everything, exactly as main does.
    - verify: `SceneGraphModel` gtests and `scene_graph_panel.py`, `scene_graph_follow.py`, `scene_graph_drag.py` pass; a GUI check that twirling a row produces no stage notice.
    - size: L

  Replace the current `**Verification gate:**` line with: `**Verification gate:** release gate green; panel GUI scripts pass under Xvfb; the plain panel (tree, filter, selection sync, attribute view, drag, follow and pin) is accepted from the replacement AppImage.`

  Add to `## Decisions`: `- 2026-11-04 — All folding is removed from this milestone: it loaded every payload first, edited the scene on each twirl and stored display state in USD layers. Replaced by the design in PLAN/DESIGN/2026-11-04-scene-graph-design.md (user). Earlier fold tasks stay checked as history; Phase 231.5 removes their code.`

  **M232-scene-selection-to-nodes.md**

  Append:

  ## Phase 232.5: On the plain panel

  - [ ] M232.P5.T1 — Rebase onto the stripped panel
    - files: `Gui/SceneGraphPanel.cpp`, `Gui/ScenePrimPickerPopup.cpp`, `Gui/SceneNodeStageSource.cpp`, `Gui/SceneViewport.cpp`
    - approach: bring in the stripped M231 - Scene Graph Panel branch and current main, and remove any remaining fold calls and references to `USDExpandHierarchy`. The picker's fully loaded stage stays for now; M334 - Knob Prim Tools changes it.
    - verify: `scene_prim_picker.py`, `scene_set_on_node.py` and `scene_create_from_selection.py` pass; grep finds no fold API use.
    - size: M

  - [ ] M232.P5.T2 — One consistent prim for path knobs, and flag USDAssignMaterial
    - files: `Gui/SceneSelectionActions.cpp`, `Gui/KnobGuiString.cpp`, `Engine/Nodes/Scene/USDAssignMaterial.cpp`, `Tests/gui/scene_set_on_node.py`, `Tests/gui/scene_graph_drag.py`
    - approach: a path knob takes the first prim of a multi-selection in every route (today drag takes the first and Set Selection takes the last, `SceneSelectionActions.cpp:203-243`), by sharing one write helper. Give USDAssignMaterial's Prim Pattern knob the prim-pattern hint.
    - verify: GUI scripts — drag and Set Selection write the same prim to a path knob; dragging onto USDAssignMaterial's pattern sets it and Undo restores it.
    - size: M

  Replace the current `**Verification gate:**` line with: `**Verification gate:** all selection and Tab-menu tests pass; patterns, wrapping, and typed filtering are accepted in a replacement AppImage built on the plain panel (this milestone has never been UAT-tested).`
