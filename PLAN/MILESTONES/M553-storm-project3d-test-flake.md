# M553 - Storm Project3D Test Flake

`USDRenderStormTest.StormProject3DProjectOnKeepsTheFacesOfTheHoldAsTheMeshTurns` fails intermittently in PR CI on main-based branches since F4 - Heavy Scene Viewer landed (PRs #342, #334, #341 on 2026-11-23, both schedulers). Its log shows "Could not load sublayer @/tmp/Natron-*/moving.usda@" just before. Make it deterministic.

## Phase 553.1: Fix

- [ ] M553.P1.T1 — Find and fix the flake
  - files: `Tests/Scene/USDRenderStorm_Test.cpp` (or wherever the case lives), the code under test
  - approach: find why the sublayer fails to load (temp file written after the stage opens, a shared temp name between parallel tests, an async write) and whether the product or the test is wrong; fix the cause, not the timing.
  - verify: remote gate with `--repeat-test USDRenderStormTest:20` green.
  - size: M

## Decisions

- 2026-11-23 — Filed by the PM after the same failure blocked three PRs.
- 2026-11-23 — Merged to main as 4e478e0ff (#355); review round closed.
