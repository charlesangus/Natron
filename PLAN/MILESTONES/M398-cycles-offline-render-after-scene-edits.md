# M398 - Cycles Offline Render After Scene Edits

USDRender with the Cycles delegate often renders black geometry after a scene change, until the seed (or another setting) changes. The offline path reuses the engine after a stage edit and stops at the first `IsConverged()` (`Engine/Nodes/Scene/HydraRenderImpl.cpp:1251-1262`, M376 branch line numbers). Cycles reports convergence when a reset cut the render short, so an empty buffer is read back and cached under the node hash. The viewer's progressive path already guards this with `restartSeen` (`HydraRenderImpl.cpp:3083-3107`, PR #243). A settings change rebuilds the engine (`:1110`), which is why the seed "fixes" it. Reported in M376 UAT round 1 (2026-11-12); not geometry work.

## Scope

**In:**
- The offline (non-progressive) Hydra render path after a stage edit, for Cycles and any delegate that reports early convergence after a reset.

**Out:**
- The progressive viewer path (already guarded).

## Phase 398.1: Fix

- [ ] M398.P1.T1 — Don't trust convergence until the render has restarted after an edit
  - files: `Engine/Nodes/Scene/HydraRenderImpl.cpp`, `Tests/Scene/USDRenderCycles_Test.cpp`
  - approach: after a stage edit, the offline loop waits until the sample count moves (as the progressive path's `restartSeen` does) before accepting `IsConverged()`; if that is not reliable across delegates, rebuild the engine on edit. Share the guard with the progressive path rather than duplicating it.
  - verify: new test renders, edits the stage (move or recolour a prim), renders again on the same engine, and gets non-black pixels matching a fresh-engine render; existing USDRender Cycles tests green.
  - size: S

**Verification gate:** CI green; the user's repro (scene change → render) no longer renders black.

## Decisions

- 2026-11-12 — Split from M376 UAT round 1 (user agreed): the cause is in the shared Hydra render path, not geometry.
