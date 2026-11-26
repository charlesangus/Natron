# M626 - Native Geometry Viewer Bench And Checks

Last member of F41 - Native Geometry Scene Index. Prove the cutover: no flash frames, every viewer feature on native geometry still works, and drags are faster on heavy geometry. Before (main) and after (the Feature) are measured in one sitting on the N100.

## Scope

**In:**
- A presented-frame coverage check that catches a grey or partial frame during knob drags.
- GUI checks across pick, selection, highlight, framing, handles, guides and delegate switching.
- A one-command bench comparing a main AppImage with the Feature's AppImage.

**Out:**
- GPU-host timings, under the project-wide rule against GPU-host timing gates.

**Edge cases:**
- The bench stays inside the ~30 min N100 budget. Trim reps or sizes rather than overrun.

## Phase 626.1: Checks and bench

- [ ] M626.P1.T1 — Presented-frame coverage check on knob drags
  - files: `Gui/SceneViewport.cpp` (`presentRenderTarget`), new `Tests/gui/m626_geo_drag_coverage.py`
  - approach: add an env-gated per-present record of the fraction of background pixels, and which `paintGL` branch drew the frame. The script drags GeoTransform translate on a Box, GeoScatter count and GeoScatter pscale (inside a range where points don't merge), and fails on any presented frame whose coverage drops below the steady frames, or if the drag doesn't end on the final value.
  - verify: the script passes on the Feature build under Xvfb, and fails when a blank clear is forced (temporary local edit, not committed).
  - size: M
- [ ] M626.P1.T2 — Native geometry viewer GUI regression set
  - files: new `Tests/gui/m626_geo_viewer_features.py`, the remote GUI check list
  - approach: one script covering prim, marquee and component pick, selection tint, display highlight, framing, prim handles, GeoDisplay guides, and a Storm → Cycles → Storm switch with native points visible in each.
  - verify: the script passes under Xvfb and in the remote GUI checks (`build.sh --gui`).
  - size: M
- [ ] M626.P1.T3 — Before/after drag bench on the N100
  - files: new `tools/bench/geo_viewer_drag_bench.py`, reusing `tools/bench/edit_latency_bench.py` and `tools/bench/geo_scale.py`
  - approach: one command takes two AppImages (main and the Feature). It runs a GeoTransform drag on a Box, a 1M-point and a 5M-point mesh, plus a GeoScatter count drag, and records p95 step-to-screen latency and the USD resync count per step. It writes a table to the report.
  - verify: the bench runs end to end on the N100 within the 30 min budget and prints both columns.
  - size: M

**Verification gate:** M626.P1.T1 and T2 pass under Xvfb and in the remote GUI checks. The bench shows p95 step-to-screen is never worse than main at any size and clearly faster at 1M points and above, and points-only steps send only `primvars/points` dirties, with no resync. Bench table, coverage results and screenshots go to the user at F41 - Native Geometry Scene Index's UAT.

## Decisions
- 2026-11-26 — Perf bar (user): faster on heavy, never slower. An absolute fps target was rejected as a guess before measurement.
