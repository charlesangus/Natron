# M652 - Cycles Shutter Dirtying Without Emulation

Found by M648 - Cycles Live Volume Refresh's review (PR #412): the Cycles fork's render pass still calls `HdChangeTracker::MarkAllRprimsDirty` in `SetMotionShutter` (`src/hydra/render_pass.cpp` ~:372) and on subdivision setting changes (~:422). Under USD 26.08's terminal scene index the change tracker has emulation off, so each call logs "Calling method on HdChangeTracker that requires emulation" and may not dirty anything. The comment above :422 claiming Hydra accepts it is wrong. Both date from M554 - Scene Camera For Delegate Renders' pin.

Acceptance sketch:
- Shutter and subdivision changes reach the rprims through the scene index (a Cycles filtering scene index that dirties the affected locators, or the render settings / camera data sources Hydra already forwards), with no emulation-only calls left in the fork.
- An open-shutter volume or motion-blur test, and a subdivision setting change test, assert no "emulation" coding errors and that the image updates.
- Re-pinned in Natron.

## Scope

**In:**
- Replace both `MarkAllRprimsDirty` calls in the fork's `src/hydra/render_pass.cpp` with scene-index dirtying, the way M648 handled fields.
- Tests: an open-shutter motion-blur render and a subdivision-setting change on a live engine, both asserting no "emulation" coding error and that the image updates.
- Re-pin in Natron.

**Out:**
- MoonRay (M651 - MoonRay Live Volume Refresh).

## Phase 652.1: Fix

- [ ] M652.P1.T1 — Shutter and subdivision changes without emulation-only calls
  - files: the Cycles fork `src/hydra/render_pass.cpp` (~:372, ~:422), `src/hydra/plugin.cpp`, a filtering scene index beside `src/hydra/dependency_scene_index.*` if needed; `tools/ci/local/Dockerfile`; `Tests/Scene/` (Cycles motion-blur / subdivision tests)
  - approach: find how the shutter interval and subdivision settings reach rprims under scene-index Hydra (camera / render-settings data sources, the dirty-bits translator); dirty the affected rprims through a Cycles-registered filtering scene index (or let the existing forwarded locators do it) instead of `HdChangeTracker::MarkAllRprimsDirty`; fix the wrong comment above :422.
  - verify: the new tests pass on the new pin with no "emulation" coding errors and the image updates; all Cycles / VolumeRender tests still pass.
  - size: M

**Verification gate:** a remote gate on the F1 branch with the new Cycles pin: the new tests and all Cycles, VolumeRender and USDRenderCycles tests green; PR CI into F1 green.
