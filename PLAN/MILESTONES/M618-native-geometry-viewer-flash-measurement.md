# M618 - Native Geometry Viewer Flash Measurement

Stage 1 of the M617 - Native Geometry Viewer Architecture Spike plan (`PLAN/DESIGN/2026-11-26-native-geometry-viewer-architecture.md`). The user is discussing the direction separately and more research is needed before this is claimed.

## Scope

**In:**
- Per-stage timers on the native geometry drag path: cook, serveFrame, syncScene (lock wait separately), GeoLayers::refresh (conversion, diff, Sdf), guide rebuild, Storm render.
- A presented-frame flash detector in `presentRenderTarget` (background-pixel fraction per frame, flags drops), and logging of blank/composing paints, setError, fill-step frames and resyncs.
- Runs: GeoTransform drag on a Box and on a 1-5M-point mesh, a GeoScatter count drag, on a modest machine and a GPU host.

**Out:**
- Fixes (M619).

## Phase 618.1: Measure

- [ ] M618.P1.T1 — Instrument the drag path and find every flash
  - files: see the design note's "Instrumenting it" list
  - approach: env-gated timers and the flash detector; run the drags; trace every flagged frame to a named path
  - verify: a report listing each flash with its cause and per-stage timings, sent to the user
  - size: L

## Decisions
