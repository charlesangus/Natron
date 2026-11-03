# M254 - Progressive Viewer Render

The 3D viewer renders with Cycles or MoonRay progressively: any edit restarts the render at
low quality and it refines in the background, painting each update, until a Max Samples
value is reached or the user stops it (user, 2026-10-27). Builds on M248 - Render Delegate
Selection's viewer dropdown and M251 - Cycles Rendering.

## Scope

**In:**
- A background refinement loop in the viewer for non-Storm delegates: render, publish the buffer, repeat until the delegate reports converged or Max Samples is reached.
- Restart on any scene, camera, viewport-size or setting edit; a stale in-flight pass is discarded.
- A Max Samples knob on the viewer's renderer group, and Stop/Resume buttons.
- A sample counter in the viewer overlay.
- CPU colour buffers displayed by uploading to a viewer texture.

**Out:**
- Interactive camera look-through for Storm — already M19 - USD/Hydra Foundation's.
- Denoising during refinement beyond what the delegate does — none.
- MoonRay-specific progressive controls beyond the shared loop — M252 - MoonRay Rendering's settings in M253 - Delegate Render Settings.

**Edge cases:**
- Edits arriving faster than passes finish: coalesce, restart once.
- Closing the viewer or switching input mid-render: stop and release threads before the engine is dropped.
- The viewer renders on the same cores as the compositor: refinement runs at low priority and respects the scheduler's thread budget so the UI stays responsive.
- A project with Cycles and a USDRender node both rendering: independent engines, the shared thread budget applies.

**Assumptions:**
- Unconfirmed: the delegates support repeated `Render` calls on one engine accumulating samples, and a convergence query (`IsConverged`). Verify in `UsdImagingGLEngine` and each delegate.
- The viewer GL context can upload a CPU buffer each pass at interactive rates up to 4K; measure.

## Phase 254.1: Refinement loop

- [x] M254.P1.T1 — Refinement loop and convergence
  - files: `Gui/SceneViewport.cpp`, `Gui/SceneViewport.h`, `Engine/Nodes/Scene/HydraRenderImpl.cpp`, `Engine/Nodes/Scene/HydraRenderImpl.h`
  - approach: a worker that calls render repeatedly on the viewer's engine, publishes each colour buffer, stops on `IsConverged` or the sample cap. Restart discards the accumulated state via the engine's invalidation.
  - verify: a headless test drives the loop on Cycles and sees sample count increase monotonically to the cap and stop.
  - size: L

- [x] M254.P1.T2 — Restart on edit and coalescing
  - files: `Gui/SceneViewport.cpp`, `Engine/Nodes/Scene/SceneStage.cpp`
  - approach: hook the long-lived scene's change notification so any edit, camera move, resize or setting change restarts the loop; coalesce bursts to one restart.
  - verify: test edits a prim ten times in 50 ms and observes one restart; a late stale pass is not displayed.
  - size: M

- [x] M254.P1.T3 — Max Samples, Stop/Resume and counter
  - files: `Gui/SceneViewport.cpp`, `Gui/SceneViewport.h`
  - approach: Max Samples knob (default from the delegate), Stop/Resume buttons, overlay text "N / Max samples".
  - verify: GUI test under Xvfb: Stop halts, Resume continues from the current count, the counter matches the engine's.
  - size: M

- [x] M254.P1.T4 — Display and responsiveness
  - files: `Gui/SceneViewport.cpp`, `Engine/Nodes/Scene/HydraHandoff.cpp`
  - approach: upload each CPU colour buffer to a viewer texture; run the refinement at lowered thread priority within the scheduler's budget; measure frame upload time at HD and 4K.
  - verify: with refinement running at 4K, the Qt event loop in the Xvfb harness stays under 33 ms per iteration (numbers recorded); screenshots shared with the user.
  - size: M

**Verification gate:** CI green; in the Xvfb GUI a Cycles viewer refines, restarts on edit, stops at Max Samples and on Stop; no engine or thread leaks on viewer close; screenshots of an early and a converged frame are shared with the user.

## Phase 254.2: Max Samples on the Viewer node

- [x] M254.P2.T1 — Max Samples and Stop/Resume as Viewer knobs in a viewer toolbar (b5c839907, review fixes e24d17f44, gate+GUI green, PR #184, UAT AppImage ready: `maxSamples` int "Max Samples", 0 = delegate default; `refinementPaused` bool "Refinement Paused"; toolbar row under the second settings row, only for non-Storm scene viewers; undoable like other viewer toggles)
  - files: `Engine/ViewerNode*.{h,cpp}` (Viewer knobs), `Gui/ViewerTab*`/`Gui/SceneViewport.{h,cpp}` (toolbar), `Engine/Nodes/Scene/HydraRenderImpl.{h,cpp}`, the M254 tests
  - approach: replace the Renderer-submenu spin box with Viewer node knobs `maxSamples` ("Max Samples") and a Stop/Resume button knob; show them in a viewer toolbar, visible when the viewer's renderer is progressive (non-Storm), driven by the knobs (so they save with the project and are scriptable). Changing Max Samples restarts refinement as before; the sample counter stays in the toolbar. The 4K event-loop check records only on remote runners.
  - verify: release gate green; GUI check sets `maxSamples` via Python and sees the toolbar value and refinement stop at the cap; screenshot of the toolbar shared with the user.
  - size: L

## Decisions

- 2026-10-28: Implemented (build pending). HydraRender::ProgressiveRender runs a background refine loop for any non-Storm renderer; stops at converged + cap, or no progress for 1 s. SceneStage::sync changes → invalidate(); edits coalesced (20 ms quiet, 100 ms cap); stale passes dropped. Deviation: Max Samples is a spin box in the viewer's Renderer submenu (M248 put renderer choice there), with Stop/Resume; changing it restarts. Edits wait while stopped. Render at nice 10 on effect CPU budget − 1. Open (user): menu spin box OK instead of a knob? 4K event-loop check hard-fail or record-only?
- 2026-10-27 — **Restart on edit, refine to a cap (user):** matches Blender and Solaris viewports; an uncapped or time-boxed schedule was rejected.
- 2026-11-02 — Audit: all four tasks at 4cd9d8354 (ProgressiveRender_Test covers the cap, coalescing and Stop/Resume; m254_progressive_viewer.py times the event loop at the view size and 3840x2160 against 33 ms), plus 867bf876c. Pushed, clean, no PR, 23 behind origin/main; stacked on M251. The two open user questions (menu spin box instead of a knob; 4K loop check hard-fail or record-only) are unanswered (audit).
- 2026-11-03 — Answers (user): Max Samples (and Stop/Resume) is a **knob on the Viewer node, exposed in a viewer toolbar**, not a Renderer submenu spin box; the 4K event-loop check **records only** on remote runners (UAT shows whether it matters on a real host).
