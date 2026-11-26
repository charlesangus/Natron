# M617 - Native Geometry Viewer Architecture Spike: findings

**Recommendation:** keep Hydra/Storm. First spend 1-2 days measuring the flashes, then fix what that finds inside the current path (option 1). Prototype a native points-only scene index (option 3) only if the heavy-geometry timings justify it. Don't fork Storm (option 4), and don't build our own drawer (option 2).

Nothing in the code shows that the flashing is caused by going through USD/Hydra:
- **Storm frames are already double-buffered.** The last finished frame stays on screen until a newer one lands (`SceneViewport.cpp:12952-12968`, 13018-13021).
- **Points-only edits skip the resync.** They are already applied in place (`GeoSdfFileFormat.cpp:654-657`, 437-455).
- **The grey frames come from a few specific paths.** No test checks the pixels actually presented. `m559_geo_drag_keeps_frame.py` only counts paints.

These findings come from reading the code; nothing has been measured yet.

## 1. The drag path today

1. **Cook.** Knob edit → `evaluateGeometry`. An aborted cook publishes nothing.
2. **Handoff to the viewer.** Stale results are dropped, then `serveFrame` → `showEvaluatedScene` → `setScene` (keepsFrame).
3. **Stage lock.** The worker's `syncScene` takes the exclusive stage lock. It waits for the running Storm frame, which can only stop between passes. This costs latency, not grey frames.
4. **Conversion.** `GeoLayers::refresh` builds a full new conversion, then diffs every spec single-threaded: about 3-4 passes over the point data per step. Many-core machines don't help here. If children get reordered, the fast paths are lost for every later cook.
5. **Guides.** Markers, normals and packed bounds are rebuilt for every new detail, each an O(N) pass through Sdf.
6. **USD change processing.** Points-only edits are cheap. A topology change erases and re-authors the prim, which means a resync and a Storm rprim and batch rebuild.
7. **Render.** Storm sync (points re-uploaded), draw, then grade and selection passes, then present.

## Likely sources of the grey/flash frames (to confirm by measurement)

- **A failed cook clears the scene.** It syncs an empty scene and draws a blank clear. The next good cook re-adds every prim over several frames (`ViewerInstance.cpp:4035`, `SceneViewport.cpp:11153-11159`, `SceneDrawFilter.cpp:1310-1315`).
- **Partial fill frames.** New prim paths or topology rebuilds aren't held through admission, so fill frames show partial content.
- **Placeholder paints.** Composing or blank paints appear whenever no scene is presented yet.
- **Stand-in boxes.** They appear when the last frame was over budget (should only happen on camera moves).
- **The stale cue.** It toggles on every drag step, which may read as flashing.

## 2. Options

| Option | Cost | Upkeep | What it keeps / loses |
|---|---|---|---|
| **1. Fix in place.** Keep the last good scene when a cook fails (error shown as an overlay), hold resynced prims through admission, rebuild guides only when their inputs change | small-medium | none new | Keeps picking, handles, display modes, the stale cue, Cycles/MoonRay and export parity. Heavy geometry still pays the serial Sdf diff. |
| **2. Our own GL/Vulkan drawer** | large-very large | two renderers | Packed USD prims and USD in the same view still need Hydra, so two depth buffers get composited. Picking, highlights, grade/OCIO, materials and display modes must be redone. Viewer Cycles/MoonRay still need Hydra. Houdini does this, but ships its GL as a Hydra delegate in Solaris. |
| **3. A Natron scene index feeding Storm** (no Sdf layer) | medium-large | moderate | Blender's choice: its "HYDRA" export method gives fast interactive editing next to a "USD" method for accuracy. It reaches every delegate as a scene-index plugin (we already use that pattern). Gives exact points-only dirties and copy-free arrays, and removes the Sdf diff and USD change processing. Everything that reads geometry from the stage needs a native path. Export parity holds only if it is built from GeoUsdConvert's data. |
| **4. A forked Storm outside Hydra** | highest | every USD upgrade | No gain over option 3, which already reaches Storm directly. |

## 3. Staged plan

1. **Measure.** This is the cheapest prototype.
   - Timers on: cook, serve, `syncScene` (lock wait separately), refresh (conversion, diff and Sdf parts), guides, and Storm render.
   - A flash detector in `presentRenderTarget` that flags any presented frame whose coverage drops.
   - Run a GeoTransform drag on a Box and on a 1-5M-point mesh, plus a GeoScatter drag, on a modest machine and a GPU host.
   - Done when every flagged frame is traced to a named path.
2. **Fix those paths (option 1)** and add a GUI check on the presented pixels.
   - Done when a 10 s drag presents no coverage drops, points-only edits cause no resyncs, and every drag ends on the final value.
3. **Prototype option 3 only if** refresh plus USD change processing exceeds about 40% of each step on heavy geometry.
   - Build it behind an environment-variable switch, as a points-only override that skips `GeoLayers::refresh`.
   - Compare step-to-screen latency at p95 on the modest machine.
   - Adopt it only if it clearly wins.

## Not yet known

- Which of the candidates you actually see, or whether it is the stale cue.
- The real per-stage costs.
- Whether waiting for the stage lock behind a long Storm frame makes drags feel stuck.
- Whether a scene-index plugin's prims can be picked through UsdImagingGLEngine.
