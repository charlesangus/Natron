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

- [x] M376.P1.T1 — The Viewer's 3D view displays geometry nodes through its own `natrongeo` layer
  - files: `Gui/SceneViewport.cpp`/`.h`, `Engine/ViewerInstance.h`/`.cpp`, `Gui/ViewerTab.cpp`, plus new display files
  - approach: built on the viewer stage as F4 - Heavy Scene Viewer leaves it (deferred, worker-composed; re-read `PLAN/DESIGN/2026-11-04-scene-graph-design.md` at claim). Extend the Viewer's input acceptance and kind-driven view switch to the geometry kind. The viewer reads its own copy of the `natrongeo` layer, opened with a `consumer=<viewer>` file-format argument (no time in the key), and refreshes it in place from the cached detail with M358.P1.T2's `SdfChangeBlock` edits under the viewer's `stageMutex` (exclusive), so scrubbing dirties points without a resync and without `SceneWriteBarrier` (`SceneWriteBarrier.h:45-47`: a layer only its owner edits needs no barrier). `GeoToUSD` → `USDRender` keeps time-keyed layers; both come from the same `GeoSdfData`. Optional: `SceneStage::sync` swaps time-keyed `natrongeo` layers in a scene stack for the viewer's copy. Packed USD primitives draw as their bounds, and packed geometry as instances. Picking returns the prim path, which maps back to the partition and its primitive numbers.
  - verify: manual — a `GeoPrimitive` node viewed in the Viewer's 3D view shows the mesh; scrubbing a time-varying upstream node updates it without a stage rebuild (no resync notice); clicking a prim reports its primitive numbers; a packed `USDToGeo` output shows bounds boxes.
  - size: M+S

- [x] M376.P1.T2 — Core SOP display toggles in the geometry context
  - files: `Gui/SceneViewport.cpp`/`.h`, `Gui/ViewerTab.cpp` (toolbar toggles), `Gui/GeoDisplayGuides.h`/`.cpp`
  - approach: viewer toggles shown only when the active input is geometry: point markers, point numbers, primitive numbers, normals (`N`, as lines scaled by a viewer setting) and highlighting of a chosen group. Point markers and normals are Hydra guide prims (purpose `guide`) authored into a viewer-private session layer, the same layer M361.P1.T1 uses for markers. Point and primitive numbers are a screen-space overlay beside `SceneViewport`'s `drawGuides`, because Storm cannot draw text; this departs from "nothing outside Hydra". Number labels are drawn only for elements in view, capped per frame, so a 10M-point detail stays responsive.
  - verify: manual (Xvfb screenshots shared before sign-off) — each toggle shows on a `GeoPrimitive` cube and clears when switched off; viewing a `GeoToUSD` downstream offers none of them; a 1M-point grid with point numbers on keeps the viewer interactive.
  - size: M

**Verification gate:** CI green; on a branch with F4 and F15 merged, built under Xvfb, a `GeoPrimitive` chain displays in the Viewer's 3D view through its `natrongeo` layer, each display toggle works (screenshots shared before sign-off), and scrubbing dirties points without a stage rebuild; the pre-existing ctest suite is green.

## Phase 376.2: UAT round 1 fixes

User feedback on the 0ab97da4e AppImage (2026-11-12); investigation in Decisions. Paths are on the M376 branch.

- [x] M376.P2.T1 — Geometry playback shows the frame on the timeline, at the playback rate
  - files: `Engine/Nodes/Geometry/GeoViewerLayer.cpp`, `Engine/ViewerInstance.cpp`, `Gui/ViewerTab.cpp`, `Gui/GeoDisplay.cpp`, `Engine/Nodes/Geometry/GeoSdfFileFormat.cpp`
  - approach: profile first with the `natronGeoRefreshes`/`natronSceneSyncedAtMs` properties and stack samples on a GeoScatter → GeoCopyToPoints → GeoWrangle (animated P) chain. The user sees about 1 fps that does not improve on repeat playback and is uneven, so the display path is the suspect, not the cook. Likely fixes: key `GeoViewerLayer`'s slot by frame and refresh for the displayed frame rather than the latest cook; convert outside the exclusive stage lock; when topology is unchanged, author only the changed point columns; recompute guides, labels and bounds only when a toggle needs them.
  - verify: playback of that chain in the Viewer's 3D view runs at or near the project rate on the GPU host, repeatably; refresh count per played frame is 1; scrubbing still dirties points without a resync.
  - size: M
- [x] M376.P2.T2 — Point widths from pscale, with a default
  - files: `Engine/Nodes/Geometry/GeoUsdConvert.cpp`, `Engine/Nodes/Geometry/GeoScatter.cpp`, `Engine/Nodes/Geometry/GeoToUSD.cpp`
  - approach: GeoScatter gets a `pscale` knob that writes a float point attribute. Float64 `pscale` maps to `widths` like float32 (check Solaris docs in `.local/docs` for whether widths = 2 × pscale). With no `pscale`, UsdGeomPoints get a default width, a GeoToUSD "Default Point Width" knob, instead of USD's fallback 1.0.
  - verify: points scattered on a 1×1 card draw small by default and follow pscale; gtest on the conversion of float32, float64 and missing pscale.
  - size: S
- [x] M376.P2.T3 — Time-sampled GeoToUSD without a GUI freeze
  - files: `Engine/Nodes/Scene/USDRender.cpp`, `Engine/Nodes/Scene/USDRenderSettings.cpp`, `Engine/Nodes/Scene/USDVariant.cpp`, `Engine/Nodes/Geometry/GeoToUSD.cpp`
  - approach: menus that list prims of the input scene (USDRender, USDRenderSettings, USDVariant, USDProject nodes) fill from a worker or from the last evaluated scene, never by evaluating the input on the main thread. GeoToUSD's "Copy Into Layer" + "Author Time Samples" cooks the frame range in parallel with cancellation.
  - verify: turning both on with a 100-frame animated chain leaves the GUI responsive (no main-thread stall over 100 ms in the stall monitor); the authored layer has every time sample.
  - size: M
- [x] M376.P2.T4 — Wrangle noise from FastNoise2
  - files: `Engine/Kernel/KernelTypes.h`, `Engine/Kernel/KernelParser.cpp`, `Engine/Kernel/KernelRunner.cpp`, `Engine/Nodes/Geometry/docs/GeoWrangle.md`
  - approach: implement the reserved names on `libs/noise/Noise4D.h`: `pnoise`, `onoise` (fBm with octaves, lacunarity, gain), `anoise`, `curlnoise`, a cellular noise, plus simplex/value variants. Inputs are float, vector or vector4; the fourth component is whatever the user passes (user: never hardcode time). Keep `noise()` as it is.
  - verify: gtests for each function's range, determinism and 4D input; GeoWrangle.md lists them.
  - size: M
- [x] M376.P2.T5 — Attributes feed USDMaterial colours, multiplied
  - files: `Engine/Nodes/Scene/USDMaterial.cpp`, `Engine/Nodes/Scene/CyclesAovs.cpp` if needed
  - approach: USDMaterial colour inputs get a "Multiply By Primvar" option with a primvar name (default `displayColor`, which Cd exports to). The authored network multiplies a `UsdPrimvarReader_float3` by the knob colour or the texture (user: multiply, not replace). Check Storm and Cycles both render it.
  - verify: a GeoPrimitive with Cd through GeoToUSD and a USDMaterial with a texture renders tinted by Cd in Storm and Cycles; gtest on the authored network.
  - size: S/M
- [x] M376.P2.T6 — Render visibility on USDLight
  - files: `Engine/Nodes/Scene/USDLight.cpp`
  - approach: a "Visible In Render" (camera visibility) knob on USDLight for every light type (user: on the light node, not the render node). For a dome it authors `domeLightCameraVisibility` / the light's camera visibility; check the USD and Cycles docs for the attribute each light type takes.
  - verify: a dome with the knob off lights the scene but is not seen in the background in Storm and Cycles.
  - size: S
- [x] M376.P2.T7 — GeoWrangle and GeoPython reference in node help
  - files: `Engine/Nodes/Geometry/docs/GeoWrangle.md`, new `Engine/Nodes/Geometry/docs/GeoPython.md`, the node-help plumbing
  - approach: the node help shows the docs/*.md reference. Write GeoPython.md: `geo.points/vertices/prims/detail`, groups, topology calls, numpy buffers, `frame`, errors. GeoWrangle.md lists reserved but unimplemented names until T4 lands.
  - verify: both nodes' help shows the reference (Xvfb shot).
  - size: M

## Decisions

- 2026-11-09 — Display split out of M358 - Geometry Bridges And Display (user); M358 no longer waits on F4.
- 2026-11-09 — Merge target (user): the work merges into F15 only after F4 - Heavy Scene Viewer lands on main; until then it branches off F4 with F15 merged in.
- 2026-11-09 — A direct GeoDetail → Hydra scene index was considered and rejected (consultant): feasible via `sceneIndexPluginRegistry`, but it is a second Hd translation of topology, primvars and instancing with parity tests, picking via USD proxy paths is unverified, and the effort is about L+M against S. The viewer's layer copy gives the same no-reload, no-barrier benefit with one conversion.
- 2026-11-09 — Point and primitive numbers are a screen-space overlay beside `drawGuides`, a departure from "nothing outside Hydra" (consultant): Storm cannot draw text. Point markers and normals stay Hydra guide prims.
- 2026-11-10 — Resume: P1.T1 and most of P1.T2 were already committed (adb329912, 3e6b2228a), but the plan's checkboxes had not been updated. The first remote GUI run failed with "connect the Viewer to the GeoTransform": `Node::isViewerOnlyAdapter` only let raw and scene data onto the Viewer. Fixed in 01435f99e, with test `GeometrySourceAcceptedByViewer`; the script now picks at the frame the camera framed. The gate is re-running.
- 2026-11-10 — Both tasks done (adb329912, 3e6b2228a, 01435f99e, 0ab97da4e). Release gate green with `m376_geometry_display.py` and the scene viewer scripts (run 42791093755). Per the user's merge rule, the PR into F15 waits until F4 - Heavy Scene Viewer lands on main and main is merged into F15; until then a PR diff would include all of F4. Gate screenshots go to the F15 UAT.
- 2026-11-12 — UAT round 1 on the 0ab97da4e AppImage. Consultant findings: playback goes through a single shared `GeoViewerLayer` slot and a full `GeoUsdConvert` rebuild under the exclusive stage lock each frame; GeoScatter writes no `pscale`, so USD's fallback width 1.0 applies; GeoToUSD authors every time sample eagerly and USDRender/USDRenderSettings evaluate their input on the main thread to fill menus; Cd exports to `primvars:displayColor` but USDMaterial reads only `st`; `noise()` is one Perlin octave while FastNoise2 is ported in `libs/noise/Noise4D.h`. `@Frame` and `@Time` (seconds) already work in wrangles. The Cycles black frames are not geometry work and are M398. Phase 376.2 holds the fixes; the user agreed the recommendations, with these calls (user): playback does not improve on repeat and is uneven; render visibility belongs on USDLight for every light; primvar colour multiplies the material colour or texture; noise's fourth dimension is user input, never hardcoded time.
- 2026-11-12 — P2.T5: the primvar multiply is authored as a `cycles` render-context surface (a `cycles_mix` multiply node), so only Cycles renders it; Storm shows the unmultiplied colour. Cycles-only is accepted for now (user); the Storm route (a MaterialX network on `outputs:mtlx:surface`) is a task in M363 - Geometry Display Extras.
- 2026-11-12 — P2.T2–P2.T7 implemented (56e84a57f), test fixes in 8b1135d8f (time-sampled authoring reused no existing property spec; the Cycles-surface test counted the universal fallback). Release gate green (run 43168704534). P2.T1 profiling next.
- 2026-11-12 — P2.T1 (steppy playback) root cause: playback evaluates frames ahead in batches of one per hardware thread, and the view refreshed when an evaluation landed rather than when the timeline reached its frame, so each batch collapsed into one refresh showing the furthest-ahead frame (Xvfb, 1000 cards: 0.22 refreshes per played frame, about 0.7/s, unchanged on a second playback). Fix (f1532ae79): the geometry layer holds frames evaluated ahead (up to 64) and the view shows each one as the timeline reaches it; `m376_geo_playback.py` checks one refresh per played frame. Build run 43231665038. Conversion still runs under the exclusive stage lock; revisit if the GPU-host numbers need it.
- 2026-11-13 — **Automated gate green:** ctest in both legs (run 43376619403, after making `CoalescesEditsIntoOneRestart` count engine rebuilds under load, 44e08361c); `m376_geo_playback.py` 36/36, including the no-repeated-frame check, and `m376_geometry_display.py` 50/50 (run 43389237651). P2.T1 is checked off on one refresh per played frame. Playback rate on the GPU host and the screenshots go to F15 - Native Geometry's UAT. The PR into F15 waits until F4 - Heavy Scene Viewer lands on main (user's merge rule).
