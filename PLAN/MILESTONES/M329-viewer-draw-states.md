# M329 - Viewer Draw States

Each viewer holds draw states by prim path, edited from a column on the tree and keys in the viewer, and applied to Hydra by a per-engine scene index filter with a GL box overlay. Boxes, markers, progressive reveal and saving with the viewer live here. Design: `PLAN/DESIGN/2026-11-04-scene-graph-design.md`; spike: `PLAN/DESIGN/2026-11-07-heavy-scene-spike.md`. Stacks on M328 - Deferred Viewer Stage; line references are at `85dd18982` on that branch and will move.

Acceptance sketch: the draw column shows BBox, Children, Full, Full + Load and Hidden, with inherited values visibly different from set ones, and twirling a row changes nothing in the viewer. Setting Full on a deep row switches blocking ancestors to Children, and siblings stay as boxes. Viewer keys on a picked box work: ↓ Children, Shift+↓ Full, Ctrl+Shift+↓ Full + Load, ↑ BBox on the parent. Full + Load at the root of the payload city reveals progressively, the viewer stays navigable, and changing the state stops the rest. Reopening a saved project restores the viewer's draw states and load set progressively, and the load can be cancelled.

## Scope

**In:**

- `SceneDrawStates`, a small per-viewer object keyed by path: pure logic with effective-state resolution, serialised with the viewer.
- The draw column on the tree. It always edits the linked viewer's states by path, even when the panel looks at another node.
- Applying effective states to Hydra through the filter, with progressive batches.
- Boxes computed from children's extents on a worker, for loaded and non-payload prims. Tinted boxes for unloaded payloads with authored bounds, tinted markers without.
- Picking a box selects its prim. A row badge marks unknown bounds.
- Viewer keys, rebindable in the 3D viewer's shortcut group.
- Reset to Graph.

**Out:**

- Graph policy, USDRead's knob, USDExpandHierarchy, row origin and Bake to Node — M330 - Graph Draw Policy. Until then native geometry and USDReads default to Full.
- Viewer render delegates — M335 - Viewer Render Draw States.
- Loaded reveal and cancel in the end-to-end run, and Island — M336 - Heavy Scene Acceptance.
- Measuring unknown bounds: never (user).
- Named bookmarks of draw states — none for now (user chose saved state plus Bake to Node).

**Edge cases:**

- A path that doesn't exist at the viewed node is ignored but kept, so returning restores it.
- A BBox or Hidden ancestor stops everything below; deeper states are remembered.
- A prim whose box can't be computed yet shows its marker until the worker finishes.
- An animated subtree's box is recomputed on a worker when the frame changes; the old box stands until it's ready.
- Selection inside a boxed branch selects in the tree; the viewer highlights the nearest drawn ancestor's box.
- A saved state naming a payload whose file is now missing: row badge and USD error, and the rest restores.
- A material or camera under a boxed row stays in Hydra, so drawn geometry keeps its binding.
- Changing a state during a reveal stops the rest; payloads already loaded stay loaded.

**Assumptions:**

- Rendering 10k boxes meets the 30 fps budget with the overlay: 51-60 fps measured in M327 - Heavy Scene Spike.
- The filter's engine binding works as on the spike, where it rests on undocumented behaviour (`renderInstanceId`, insertion order).

## Phase 329.1: State model

- [x] M329.P1.T1 — `SceneDrawStates` with effective-state resolution.
  - files: `Engine/Nodes/Scene/SceneDrawStates.{h,cpp}` (new), `Engine/CMakeLists.txt`, `Tests/Scene/SceneDrawStates_Test.cpp` (new).
  - approach: pxr-free; states BBox, Children, Full, Full + Load, Hidden by path. Nearest set state wins, a Children row's unset children default to BBox, BBox and Hidden stop what's below, and setting Full or Full + Load switches blocking ancestors to Children. Resolution takes a graph-policy layer, empty until M330 - Graph Draw Policy, then the default Full. It reports a delta of drawn, boxed and hidden paths plus load requests: Children set directly on an unloaded payload loads only that payload, Full + Load loads with descendants. Cost scales with the changed subtree, not all states.
  - verify: gtest — every rule above, kept missing paths, remembered deeper states, one delta with 100k stored states.
  - size: M

## Phase 329.2: Filter and the state-change budget

- [x] M329.P2.T1 — Port the filter.
  - files: `Gui/SceneDrawFilter.{h,cpp}` (new), `Gui/SceneViewport.{h,cpp}`, `Gui/CMakeLists.txt`, `Tests/SceneDrawFilter_Test.cpp` (new), `Tests/CMakeLists.txt`.
  - approach: port from `.worktrees/m327-filter/Gui/SceneHydraFilter.{h,cpp}`, don't cherry-pick; no session mechanism or `NATRON_DRAW_STATE_MECHANISM`. Register before render index construction and keep `UNDOCUMENTED` marks. Add exceptions below a pruned prefix for Children and hide only rprims and instancers, keeping materials and cameras. Changes go as `postToRenderThread` jobs (`SceneViewport.cpp:3863`), never `callOnRenderThread` (`:3874`), and a pending delta makes `needsBackgroundFrame` true so it takes the background frame path, not `paintGL`'s blocking one (`:5510-5520`). Under C++20 compare `get_pointer()`s, not weak with ref pointers.
  - verify: gtest over a retained scene index — notices and paths for prune, restore and nested exceptions; two viewers on one stage keep separate states; GUI pixel check on a control row.
  - size: L

- [x] M329.P2.T2 — Port the harness and add gate scenarios.
  - files: `tools/bench/heavy_scene_bench.py`, `tools/bench/heavy_scene_frame.py` (new), `tools/bench/heavy_scene_rows.py`, `tools/bench/heavy_scene_gpu_host.sh`, `tools/bench/heavy_scene_watchdog.sh`, `tools/bench/README.md`.
  - approach: port every spike scenario and control (`prune_restore`, `sibling_state`, `sibling_shot`, `reveal_cancel`, `orbit10k`, populate, pixel check with control row, per-scenario limits); all run under the watchdog, since a spike settle run hung the machine for a cause never established. Drive them through `SceneDrawStates`, not spike commands, and add them to `gate`.
  - verify: under Xvfb each scenario prints one JSON line, frame rates as not valid; `gate` lists the new scenarios.
  - size: M

- [x] M329.P2.T3 — Find where the frame goes at 1M loaded.
  - files: `Gui/SceneViewport.cpp`, `tools/bench/heavy_scene_bench.py`, this milestone's `## Decisions`.
  - approach: time apply, Hydra sync, draw-item regather, batch rebuild and GL draw with `TraceCollector` and `HdPerfLog`, on city, flat and Lobby, for one sibling and a wide row. Test the inference that any add or remove costs O(all draw items) (`hd/changeTracker.h:479`, `hdSt/commandBuffer.h:68-80`); Storm's source isn't in the image.
  - verify: GPU host table in Decisions, accounting for the 1.1 s within 10%, with what is measured marked apart from inferred.
  - size: M

- [ ] M329.P2.T4 — Meet the budget.
  - files: `Gui/SceneDrawFilter.{h,cpp}`, `Gui/SceneViewport.{h,cpp}`, `Tests/SceneDrawFilter_Test.cpp`.
  - approach: by P2.T3's numbers. First candidate: hide populated prims with a `HdVisibilitySchema` override and dirty notices, reaching `SyncDrawItemVisibility` (`hdSt/commandBuffer.h:80`), and remove only never-populated or unloaded prims. Also coalesce changes per commit. Rejected candidates go in Decisions with numbers. During a commit the viewer shows its last image plus overlay and takes camera input, or Decisions says why not.
  - verify: `gate` on the GPU host — main stall within 100 ms at every size, change to committed frame within 100 ms on loaded city and flat; a shortfall goes to the user with numbers, no silent revision.
  - size: L

- [ ] M329.P2.T5 — Lobby's Geometry row and native instances.
  - files: `tools/bench/heavy_scene_make.py`, `Tests/gui/scene_draw_instances.py` (new), `Gui/SceneDrawFilter.cpp`.
  - approach: Lobby has no instances, so find why `/World/assembly_Lobby/max_Lobby/Geometry` changed no pixels by checking its Hydra prims and depth against the grab. Separately, make a small native-instance fixture: the filter sits downstream of `UsdImagingNiPrototypePropagatingSceneIndex`, so pruning an instance path may leave propagated instancers drawing. Fix what this finds.
  - verify: GUI script — pruning changes pixels on the Geometry row, an instance and a whole instancer's row, and restoring returns the original image.
  - size: M

**Verification gate:** release gate green; `gate` on the GPU host passes P2.T4's verify; filter gtests pass; `Tests/gui/scene_draw_instances.py` passes.

## Phase 329.3: Boxes, markers and picking

- [ ] M329.P3.T1 — Box worker and overlay.
  - files: `Gui/SceneDrawBoxes.{h,cpp}` (new), `Gui/SceneViewport.{h,cpp}`, `Gui/CMakeLists.txt`, `Tests/SceneDrawBoxes_Test.cpp` (new), `tools/bench/heavy_scene_bench.py`.
  - approach: from the spike's `startBoxes`, drawing instanced GL lines beside `drawGuides` (`SceneViewport.cpp:3041`). The worker uses `UsdGeomBBoxCache` with `useExtentsHint` for current boxed rows only, off main, without holding the commit lock long. Publish whole lists atomically. A frame change recomputes animated subtrees while old boxes stand. Add `first_boxes_ms`, a real drawn-boxes signal beside `first_frame_ms`.
  - verify: gtest — box values on a small stage, stale results dropped; harness — no stall, first boxes within 2 s on the city, orbit at 10k boxes at or above 30 fps on the GPU host.
  - size: L

- [ ] M329.P3.T2 — Unloaded tints, markers and the unknown-bounds badge.
  - files: `Gui/SceneDrawBoxes.{h,cpp}`, `Gui/SceneGraphModel.{h,cpp}`, `Gui/SceneGraphPanel.cpp`, `Tests/gui/scene_draw_boxes.py` (new).
  - approach: authored bounds on an unloaded payload draw a tinted box, none draws a tinted marker at its transform, never measured. A model role drives the badge. Pending boxes draw as markers. Boxless rows stay in the list, unlike the spike, which dropped 360.
  - verify: GUI script on the city — boxes and markers both drawn, badge on the 10% without hints, no layers opened; one screenshot file per state.
  - size: M

- [ ] M329.P3.T3 — Pick boxes and hide guides with their row.
  - files: `Gui/SceneViewport.{h,cpp}`, `Gui/SceneDrawBoxes.{h,cpp}`, `Tests/gui/scene_draw_pick.py` (new).
  - approach: CPU ray against the published list, no stage read, merged with `pick` and the guide hit (`SceneViewport.cpp:3089`) by depth. Selection inside a boxed branch selects in the tree and highlights the nearest drawn ancestor's box. Guides follow the resolved state of their row.
  - verify: GUI script — pick box and marker, nearest wins, tree selects, ancestor highlights, guides under a Hidden row go; Lobby screenshot before and after.
  - size: M

**Verification gate:** release gate green; box scenarios in `gate` pass on the GPU host; GUI scripts pass; screenshots shared with the user.

## Phase 329.4: Column, keys and reset

- [ ] M329.P4.T1 — Connect states to filter, boxes and loads.
  - files: `Gui/SceneViewport.{h,cpp}`, `Gui/ViewerTab.cpp`, `Tests/SceneViewport_Test.cpp`.
  - approach: each `SceneViewport` owns one `SceneDrawStates`; an edit resolves off main when large, then posts the delta and calls `requestStageLoad`, cancelling queued loads when a state changes. The stage is never edited for a state.
  - verify: gtest — delta reaches filter and loads, stage edit count unchanged, no unrequested layer opened.
  - size: M

- [ ] M329.P4.T2 — The Draw State column and Reset to Graph.
  - files: `Gui/SceneGraphModel.{h,cpp}`, `Gui/SceneGraphPanel.cpp`, `Tests/SceneGraphModel_Test.cpp`, `Tests/gui/scene_draw_column.py` (new).
  - approach: `eColumnDrawState` (`SceneGraphModel.h:138-142`) with state and inherited roles, read from the linked viewer by path, never from the stage. A delegate and menu edit it, inherited values look visibly different, and it edits the linked viewer when the panel shows another node. Reset to Graph on a row and on the panel clears set states; M330 - Graph Draw Policy adds origin.
  - verify: `QAbstractItemModelTester` gtest; GUI script — all five states, inherited look, deep Full switches ancestors with siblings boxed, twirl changes nothing; one screenshot per state.
  - size: M

- [ ] M329.P4.T3 — Viewer keys.
  - files: `Gui/ActionShortcuts.{h,cpp}`, `Gui/SceneViewport.cpp`, `Tests/gui/scene_draw_keys.py` (new).
  - approach: add four actions to `getViewer3DDefaultKeyBindings` (`ActionShortcuts.cpp:475`), matched in `keyPressEvent` (`SceneViewport.cpp:6097`): ↓ Children, Shift+↓ Full, Ctrl+Shift+↓ Full + Load, ↑ BBox on the parent; neither file binds these arrows today. They act on the picked box or selection.
  - verify: GUI script with real key events, including one rebound in settings.
  - size: M

**Verification gate:** release gate green; column and key GUI scripts and existing panel scripts pass; screenshots shared with the user.

## Phase 329.5: Reveal, saving and acceptance

- [ ] M329.P5.T1 — Adaptive progressive reveal.
  - files: `Gui/SceneDrawFilter.{h,cpp}`, `Gui/SceneViewport.{h,cpp}`, `Tests/SceneDrawFilter_Test.cpp`, `tools/bench/heavy_scene_bench.py`.
  - approach: replace the spike's one root and fixed batch with any number of roots and batches sized from measured commit time, so modest and many-core machines both hold the frame target; no prim-count cap. Keep frontiers instead of the spike's O(revealed) set, walk iteratively so depth is safe, and yield to camera input between batches. Full + Load reveals as loads land. A state change stops the rest within one batch.
  - verify: gtest — order, cancel, overlapping roots; `gate` — Full + Load at the city root and the flat 1M wide row finish with no stall over 100 ms (spike: 2 fps, unfinished at 120 s), and cancel takes one batch (spike: 120-1207 ms).
  - size: L

- [ ] M329.P5.T2 — Save and restore.
  - files: `Serialization/NodeSerialization.{h,cpp}`, `Gui/ViewerTab.cpp`, `Gui/SceneViewport.{h,cpp}`, `Tests/Scene/SceneDrawStates_Test.cpp`, `Tests/gui/scene_draw_save.py` (new).
  - approach: `Serialization/` has no viewer structure, so settle the home at claim, most likely the viewer node's serialization, and record it in Decisions. Save set states and load set with the viewer; reopening restores them through P5.T1, cancellable. Missing files give badge and USD error, and the rest restores.
  - verify: gtest round trip with missing paths; GUI script — reopen restores progressively, cancel stops it, missing payload is badged.
  - size: M

- [ ] M329.P5.T3 — Viewer states stay out of output.
  - files: `Tests/Scene/SceneDrawStates_Output_Test.cpp` (new), `Tests/CMakeLists.txt`.
  - approach: compare the node's output scene, USDWrite's bytes and USDRender's stage with and without states and with a viewer load set applied.
  - verify: gtest — identical in every case, and no layer dirtied.
  - size: M

- [ ] M329.P5.T4 — Acceptance run and UAT.
  - files: `build/appimages/` (AppImage plus same-named steps `.md`), this milestone's `## Decisions`.
  - approach: stripped release AppImage; run `DISPLAY=:0 XAUTHORITY=$HOME/.Xauthority tools/bench/heavy_scene_gpu_host.sh gate <AppImage>`; record `build/bench-gpu/heavy-scene-gate.jsonl`.
  - verify: every gate record passes, or its failure is reported with numbers; user approves the screenshots and UAT.
  - size: M

**Verification gate:** release gate green; `heavy_scene_gpu_host.sh gate` passes on the GPU host (Xvfb can't prove timing); all M329 GUI scripts pass; the user has seen screenshots and accepted the UAT.

## Decisions

- 2026-11-08 — P2.T3 numbers (GPU host, RX 7900 XTX, `heavy-scene-budget-m329.jsonl` traced `stages_ms`): main-thread stall passes on every draw_budget row (city 30 ms). Time to picture misses 100 ms: one sibling on loaded city/flat 1.35–1.7 s (apply ~2 ms; hdExecute 1.7–1.8 s, renderPassExecute ~1.3 s, commandBuffer ~0.7 s, batchRebuild ~0.6 s, prepareDraw ~0.6 s, sync ~0.2 s, regather 50–77 ms — regather and batch rebuild cost O(all draw items) for one prim); wide row (~1M prims) 7–15 s (apply 4–9 s, adapterAdded 0.7–2.5 s). Lobby/island stages a few ms. Island first render still blocks the main thread ~1.8 s (M328). Bench quirks: traced picture times include trace collection, and the gate jsonl repeats the budget rows.

- 2026-11-07 — Gate green at c192c49da with opt-in draw timing (`NATRON_DRAW_STATE_TIMING`) and a `budget` mode judging time to picture and stall; bench AppImage refreshed. Island stays in, behind its RAM check.

- 2026-11-07 — Release gate green at 3242c5853 with Load All, Hidden-only light removal, Clear Below logic and the headlight fallback (only while Hidden rows hide every stage light; an earlier version oscillated because the headlight counted as a scene light). P2.T3 onwards waits on the GPU host bench session; AppImage staged as `build/appimages/Natron-m329-viewer-draw-states.AppImage`.

- 2026-11-07 — Lights, superseding the earlier answer (user): lights always light unless their row is Hidden. BBox and Children box geometry only, so drilling down never darkens the scene. The headlight fallback still applies when every light is hidden.

- 2026-11-07 — User: the viewer falls back to the headlight, in Auto and Scene modes, while no scene light is drawn. Load All skips Hidden rows as well as BBox. The recursive clear keeps the row's own state and is called **Clear Below**; Reset to Graph clears the row and everything below. Materials and cameras under a Hidden row stay in Hydra. Pending: whether lights directly under a Children row keep lighting (the user asked for more detail).

- 2026-11-07 — Load All and BBox (user): Load All skips payloads under a BBox row, so the load needs exclusions. The column also needs a recursive clear, so nobody has to clear child rows one at a time.

- 2026-11-07 — Names (user): the state called Full + Load in the design and earlier tasks is now **Load All** (`loadAll`), everywhere it appears: draw column, viewer key action, USDRead `Initial Draw`, USDExpandHierarchy rules. The column stays Draw State (`drawState`).

- 2026-11-07 — Budget (user): the 100 ms covers both the main-thread stall and the time until the changed picture appears. If Storm can't reach it, report the numbers rather than change the target.
- 2026-11-07 — Hidden prims (user): hidden loaded prims always leave Hydra and free their memory, even if that misses the budget. Keeping them hidden in Hydra is off the table for P2.T4.
- 2026-11-07 — Lights (user): lights under a BBox or Hidden row stop lighting in both states; only rows drawn in full contribute light.
- 2026-11-07 — P1.T1, P2.T1, P2.T2 written at 94dde8516 on milestone/m329-viewer-draw-states. Standalone state and filter tests pass; the Xvfb scenarios and GuiTests/EngineTests need the gate.

- 2026-11-07 — Filter only (user, M327 - Heavy Scene Spike). Elaboration rejected `UsdImagingDrawModeSceneIndex` because it reads authored draw modes inside the usdImaging chain, and `HdsiPrefixPathPruningSceneIndex` because a prefix is its only unit of change, so it can't reveal part of a subtree. `HdsiPrimManagingSceneIndex` exists only as `HdsiPrimManagingSceneIndexObserver`, an observer and not needed.
- 2026-11-07 — Per-viewer state follows Gaffer's small expansion object beside a lazy scene (`gaffer/1.7.5.0/WorkingWithScenes/AnatomyOfAScene`); neither Katana nor Gaffer holds it per viewer (research note). Solaris changes USD draw mode for expansion (`solaris/support/expansionstates.html`), which our rule that display state stays out of layers forbids.
- 2026-11-07 — Box sources: a location's bound as union of its contents (Gaffer), extentsHint and tint for unloaded (Katana, `all_tips.html`). Key family from Gaffer's Expand, Fully Expand and Collapse selection (`Interface/ControlsAndShortcuts`) and Katana's arrow keys (`ug/appendix_hotkeys/hotkeys.html`).
- 2026-11-07 — Lobby stage check: no instances or prototypes; Geometry holds 628 meshes and 594 Xforms, no lights or materials. 30 lights sit in `lights_Lobby`, so the yellow dots are probably light guides (`SceneViewport.cpp:1674-1678`; inferred, no grab inspected). Materials sit in `Looks` scopes beside geometry, hence geometry-only hiding.
