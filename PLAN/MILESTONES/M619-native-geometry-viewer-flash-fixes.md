# M619 - Native Geometry Viewer Flash Fixes

Stage 2 of the M617 plan (`PLAN/DESIGN/2026-11-26-native-geometry-viewer-architecture.md`): fix inside the current Hydra/Storm path whatever M618 finds. Candidates from the spike: a failed cook clearing the scene (keep the last good scene, show the error as an overlay), resynced or renamed prims re-admitted over several fill frames (hold them through admission), composing/blank paints, guide rebuilds on every detail, the stale cue toggling per drag step. Scope is set from M618's report.

## Phase 619.1: Fixes

- [ ] M619.P1.T1 — Fix the flash paths M618 names
  - approach: per M618; add a GUI check on presented pixels
  - verify: a 10 s drag on light and heavy geometry presents no coverage drop, no resyncs on points-only edits, and ends on the final value
  - size: L

## Decisions
