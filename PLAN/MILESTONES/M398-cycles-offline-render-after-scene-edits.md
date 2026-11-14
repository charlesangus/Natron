# M398 - Cycles Offline Render After Scene Edits

USDRender with the Cycles delegate often renders black geometry after a scene change, until the seed (or another setting) changes. The offline path reuses the engine after a stage edit and stops at the first `IsConverged()` (`Engine/Nodes/Scene/HydraRenderImpl.cpp:1251-1262`, M376 branch line numbers). Cycles reports convergence when a reset cut the render short, so an empty buffer is read back and cached under the node hash. The viewer's progressive path already guards this with `restartSeen` (`HydraRenderImpl.cpp:3083-3107`, PR #243). A settings change rebuilds the engine (`:1110`), which is why the seed "fixes" it. Reported in M376 UAT round 1 (2026-11-12); not geometry work.

## Scope

**In:**
- The offline (non-progressive) Hydra render path after a stage edit, for Cycles and any delegate that reports early convergence after a reset.

**Out:**
- The progressive viewer path (already guarded).

## Phase 398.1: Fix

- [x] M398.P1.T1 — Don't trust convergence until the render has restarted after an edit
  - files: `Engine/Nodes/Scene/HydraRenderImpl.cpp`, `Tests/Scene/USDRenderCycles_Test.cpp`
  - approach: after a stage edit, the offline loop waits until the sample count moves (as the progressive path's `restartSeen` does) before accepting `IsConverged()`; if that is not reliable across delegates, rebuild the engine on edit. Share the guard with the progressive path rather than duplicating it.
  - verify: new test renders, edits the stage (move or recolour a prim), renders again on the same engine, and gets non-black pixels matching a fresh-engine render; existing USDRender Cycles tests green.
  - size: S

- [x] M398.P1.T2 — Forward every Cycles render setting, and a transparent background
  - files: the Cycles fork (`charlesangus/cycles` natron/deep-5.2, `src/hydra/render_delegate.cpp`, `src/hydra/session.cpp`), `tools/ci/local/Dockerfile` (pin and sha256), `Engine/Nodes/Scene/RenderSettingsPrim.*`, `Engine/Nodes/Scene/USDRenderSettings.*`, `Engine/Nodes/Scene/HydraRenderImpl.cpp`, `Gui/SceneViewport.cpp`
  - approach: the delegate forwards `cycles:<node>:<socket>` generically for every scene-level node with sockets, not only the integrator. An explicitly set `cycles:background:transparent` wins over the dome-driven default; unset keeps today's behaviour. Natron forwards every `cycles:` attribute on the active RenderSettings prim. USDRenderSettings gets a way to set arbitrary renderer settings, plus a "Transparent Background" toggle that also drives Storm's `domeLightCameraVisibility`. The fork commit is pushed and pinned.
  - verify: gtest that a `cycles:*` attribute reaches the delegate; a Cycles render with a dome and Transparent Background on has alpha 0 in the background and the same lit geometry.
  - size: M

**Verification gate:** CI green; the user's repro (scene change → render) no longer renders black.

## Decisions

- 2026-11-12 — Split from M376 UAT round 1 (user agreed): the cause is in the shared Hydra render path, not geometry.
- 2026-11-12 — Scope widened (user): "we should be able to set/forward all renderer settings". It is OK to patch the Cycles delegate; a background-visibility knob on the renderer side is fine where that is idiomatic, which for Cycles it is (transparent film or background). It goes on USDRenderSettings, not USDRender (user). The delegate patch is on top of main's pin, b0d961e3e.
- 2026-11-12 — T1 (a91d2aa87) and T2 (2fadc616c) done. The fork patch b602afeb0 (pushed to natron/deep-5.2) forwards every integrator, film and background socket. Session params, the background shader, film deep sockets, camera and scene time are not forwardable. Arbitrary settings were already authorable with USDSetAttribute, and each declared socket has an auto knob. The Dockerfile pin was bumped, so CI rebuilds the dev image. Gate: run 43157056360. Unverified: whether Storm reads `domeLightCameraVisibility` from the renderer settings; check it in the GUI at UAT.
- 2026-11-12 — The patched delegate's film and background settings now sit in Film, Passes, World and Denoise groups (0ca7a0e28, 26f55d561). `cycles:film:use_sample_count` is excluded (978ac69a0): Cycles sets it from the tile count every sync (session.cpp:734), so it is not a user setting; drop it from the fork's descriptors at the next fork change. Gate re-running (run 43199316751).
- 2026-11-12 — The audit test now expects `use_sample_count` as set by Natron (e0335a196); release gate green (run 43212024829). Main merged in (4f99144b6), PR #262 opened, review round running. UAT AppImage `Natron-m398-cycles-offline-render-after-scene-edits` with its steps is out; whether Storm hides the dome is a UAT check.
- 2026-11-13 — UAT approved (user). Main merged in (no conflicts); merges once PR #262's CI is green on the new head.
- 2026-11-14 — Merged to main (PR #262, 7b1b9a9b6). The debug task-graph CI leg passed on its second re-run. `GraphScalingConnectTest` takes about 213 s against a 300 s limit in debug and timed out twice on loaded runners; it is unrelated to this milestone.
