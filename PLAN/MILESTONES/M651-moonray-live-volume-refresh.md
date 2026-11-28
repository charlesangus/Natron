# M651 - MoonRay Live Volume Refresh

Found by M648 - Cycles Live Volume Refresh's live-field test (m648-gate1, run 46114401928): on MoonRay, editing a VDB field prim on a live engine logs "HdChangeTracker::MarkRprimDirty: Calling method on HdChangeTracker that requires emulation" from hdMoonray's field sync, and the render after the edit carries a volume warning. USD 26.08's scene-index Hydra already forwards field→volume dependencies, so the delegate's own dirtying is unneeded there (the Cycles fix removed it).

Acceptance sketch:
- `VolumeRenderMoonRay.ALiveFieldEditMovesTheVolume` (committed DISABLED_ by M648) is re-enabled and passes: no emulation coding error, no volume warning, the volume moves, no engine remake.
- The fix lands in the hdMoonray fork on a branch off F1's pin and is re-pinned.
- 2026-11-28 — From M648: the fix is to register a volume-field dependency scene index and an HdDependencyForwardingSceneIndex for MoonRay's renderer name (as Storm/hdPrman/our Cycles fork do) and drop hdMoonray's MarkRprimDirty in field sync.

## Scope

**In:** hdMoonray (Natron's openmoonray fork) registers a volume→field dependency scene index plus an HdDependencyForwardingSceneIndex for its renderer name, and drops its emulation-only change-tracker calls in field sync; re-pin; re-enable the M648 MoonRay live-field test.
**Out:** Cycles (done in M648/M652); MoonRay subdivision holes (M644).

## Phase 651.1: Fix

- [ ] M651.P1.T1 — MoonRay field edits reach volumes through scene-index dependencies
  - files: the openmoonray fork's hdMoonray sources (field/volume sync, renderer plugin registration), `tools/ci/local/Dockerfile` (NATRON_MOONRAY_COMMIT and its checksum), `Tests/Scene/VolumeRender_Test.cpp` (re-enable `VolumeRenderMoonRay.ALiveFieldEditMovesTheVolume`)
  - approach: mirror the Cycles fork's `src/hydra/dependency_scene_index.{h,cpp}` + plugin.cpp registration (e42c0987f8) for hdMoonray's renderer display name; remove MarkRprimDirty/MarkAllRprimsDirty (and any other emulation-only HdChangeTracker calls) from hdMoonray; grep for the rest.
  - verify: the re-enabled test passes with no "emulation" coding error and no volume warning, the volume moves, no engine remake; all VolumeRender/USDRenderMoonRay tests green.
  - size: M

**Verification gate:** remote gate on the F1 branch with the new MoonRay pin, full ctest green (MoonRay load flakes excepted only if they pass on rerun); PR CI into F1 green.
