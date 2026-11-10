# M376 - Geometry Display

Display native geometry in the Viewer's 3D view through the `natrongeo` layer path built by M358 - Geometry Bridges And Display. Built on M328 - Deferred Viewer Stage's viewer code, which only exists on `feature/f4-heavy-scene-viewer`, so the work branches off F4 with F15 merged in and merges into F15 only after F4 - Heavy Scene Viewer lands on main (user, 2026-11-09).

Member of F15 - Native Geometry, with that unusual branch base (PLAN-FORMAT §9b); the Feature's UAT covers it.

## Scope

**In:**
- Viewer 3D view displays a geometry node: input acceptance and the kind-driven view switch extended to the geometry kind, via the viewer's own copy of the `natrongeo` layer.
- The core SOP display toggles: point markers, point and primitive numbers, normals, group highlighting.

**Out:**
- Component picking and `GeoEdit` (M361 - Geometry Point Editing).
- Template/footprint display and attribute visualisers (M363 - Geometry Display Extras).
- The bridges themselves (M358 - Geometry Bridges And Display).

**Risks (unverified):**
- `SdfFileFormat` arguments as layer identity under `Reload`.
- M329 draw states saved by path versus partitions sharing paths.
- Per-frame re-split cost on 10M points is unmeasured.

Requires M328 - Deferred Viewer Stage and M358 - Geometry Bridges And Display. Display code lives in new files with thin hooks in `Gui/SceneViewport.cpp`; merge F4's tip at each phase.

## Phase 376.1: Display

- [ ] M376.P1.T1 — The Viewer's 3D view displays geometry nodes through its own `natrongeo` layer
  - files: `Gui/SceneViewport.cpp`/`.h`, `Engine/ViewerInstance.h`/`.cpp`, `Gui/ViewerTab.cpp`, plus new display files
  - approach: built on the viewer stage as F4 - Heavy Scene Viewer leaves it (deferred, worker-composed; re-read `PLAN/DESIGN/2026-11-04-scene-graph-design.md` at claim). Extend the Viewer's input acceptance and kind-driven view switch to the geometry kind. The viewer reads its own copy of the `natrongeo` layer, opened with a `consumer=<viewer>` file-format argument (no time in the key), and refreshes it in place from the cached detail with M358.P1.T2's `SdfChangeBlock` edits under the viewer's `stageMutex` (exclusive), so scrubbing dirties points without a resync and without `SceneWriteBarrier` (`SceneWriteBarrier.h:45-47`: a layer only its owner edits needs no barrier). `GeoToUSD` → `USDRender` keeps time-keyed layers; both come from the same `GeoSdfData`. Optional: `SceneStage::sync` swaps time-keyed `natrongeo` layers in a scene stack for the viewer's copy. Packed USD primitives draw as their bounds, and packed geometry as instances. Picking returns the prim path, which maps back to the partition and its primitive numbers.
  - verify: manual — a `GeoPrimitive` node viewed in the Viewer's 3D view shows the mesh; scrubbing a time-varying upstream node updates it without a stage rebuild (no resync notice); clicking a prim reports its primitive numbers; a packed `USDToGeo` output shows bounds boxes.
  - size: M+S

- [ ] M376.P1.T2 — Core SOP display toggles in the geometry context
  - files: `Gui/SceneViewport.cpp`/`.h`, `Gui/ViewerTab.cpp` (toolbar toggles), `Gui/GeoDisplayGuides.h`/`.cpp`
  - approach: viewer toggles shown only when the active input is geometry: point markers, point numbers, primitive numbers, normals (`N`, as lines scaled by a viewer setting) and highlighting of a chosen group. Point markers and normals are Hydra guide prims (purpose `guide`) authored into a viewer-private session layer, the same layer M361.P1.T1 uses for markers. Point and primitive numbers are a screen-space overlay beside `SceneViewport`'s `drawGuides`, because Storm cannot draw text; this departs from "nothing outside Hydra". Number labels are drawn only for elements in view, capped per frame, so a 10M-point detail stays responsive.
  - verify: manual (Xvfb screenshots shared before sign-off) — each toggle shows on a `GeoPrimitive` cube and clears when switched off; viewing a `GeoToUSD` downstream offers none of them; a 1M-point grid with point numbers on keeps the viewer interactive.
  - size: M

**Verification gate:** CI green; on a branch with F4 and F15 merged, built under Xvfb, a `GeoPrimitive` chain displays in the Viewer's 3D view through its `natrongeo` layer, each display toggle works (screenshots shared before sign-off), and scrubbing dirties points without a stage rebuild; the pre-existing ctest suite is green.

## Decisions

- 2026-11-09 — Display split out of M358 - Geometry Bridges And Display (user); M358 no longer waits on F4.
- 2026-11-09 — Merge target (user): the work merges into F15 only after F4 - Heavy Scene Viewer lands on main; until then it branches off F4 with F15 merged in.
- 2026-11-09 — A direct GeoDetail → Hydra scene index was considered and rejected (consultant): feasible via `sceneIndexPluginRegistry`, but it is a second Hd translation of topology, primvars and instancing with parity tests, picking via USD proxy paths is unverified, and the effort is about L+M against S. The viewer's layer copy gives the same no-reload, no-barrier benefit with one conversion.
- 2026-11-09 — Point and primitive numbers are a screen-space overlay beside `drawGuides`, a departure from "nothing outside Hydra" (consultant): Storm cannot draw text. Point markers and normals stay Hydra guide prims.
- 2026-11-10 — Resume: P1.T1 and most of P1.T2 were already committed (adb329912, 3e6b2228a), but the plan's checkboxes had not been updated. The first remote GUI run failed with "connect the Viewer to the GeoTransform": `Node::isViewerOnlyAdapter` only let raw and scene data onto the Viewer. Fixed in 01435f99e, with test `GeometrySourceAcceptedByViewer`; the script now picks at the frame the camera framed. The gate is re-running.
