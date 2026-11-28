# M648 - Cycles Live Volume Refresh

The Cycles Hydra delegate's `src/hydra/field.cpp` (~:140 on `natron/m554-shutter-framing-v2`) marks a single rprim dirty while the scene index syncs, which Hydra rejects ("requires emulation"). A VDB field edited on a live Cycles engine therefore probably never re-syncs the volume that binds it. M554 - Scene Camera For Delegate Renders fixed the same pattern in `src/hydra/mesh.cpp` by calling `MarkAllRprimsDirty(DirtyTopology)`. Reproduce it with a live-edit test, apply the same fix to field.cpp in the Cycles fork, and bump the Cycles pin. Member of F1 - VDB Volumes. Filed by the PM during M554 - Scene Camera For Delegate Renders.

## Scope

**In:**
- A live-edit test: a field edit on a reused Cycles engine refreshes the volume, with no "requires emulation" coding error.
- The field.cpp fix in the Cycles fork and the matching pin bump.

**Out:**
- MoonRay and Storm field refresh, unless the new test shows one of them failing too (record it under `# Open questions` and don't fix it here).
- Volume viewer support (M348 - Volume Viewer Support).

**Edge cases:**
- Frame changes already reach the volume on a reused engine (`expectTheVolumeDarkensItsSide`'s frame 2). The new test must edit the field prim itself on the live stage, not change the frame.

**Assumptions:**
- The bug is real. P1.T1 settles this. If the test passes on the current pin, cancel P1.T2 and record why.

## Phase 648.1: Live Field Refresh

- [ ] M648.P1.T1 — Test that a live field edit refreshes a Cycles volume
  - files: `Tests/Scene/VolumeRender_Test.cpp` (on `feature/f1-vdb-volumes`)
  - approach: reuse the file's `Session`/`Render` helpers. Render frame 1 on a Cycles engine. Then edit the field prim on the live stage, without changing the frame: point its `filePath` at the grid file the frame-2 render uses, or change `fieldName` to a grid that is missing. Render again on the same engine. Assert `trace.freshEngine` is false and `trace.remakes` is 0, that the image moved the way the edit implies (side swap, or the volume gone), and that `codingErrors` does not contain "emulation". Add the same test for MoonRay as a cross-check. If Cycles fails on the current pin, commit the test as `DISABLED_` with a comment naming P1.T2. The bundled gtest is old: no `GTEST_SKIP`, no `ASSERT_TRUE(ptr)`.
  - verify: the volume-render tests run. The MoonRay variant passes. The Cycles variant either passes (bug not real) or fails on the refresh or emulation assertion, and is committed `DISABLED_`.
  - size: M
- [ ] M648.P1.T2 — Fix field.cpp's dirtying in the Cycles fork and bump the pin
  - files: the Cycles fork `src/hydra/field.cpp` (charlesangus/cycles, branched from the pin M554 - Scene Camera For Delegate Renders landed), `tools/ci/local/Dockerfile` (`NATRON_CYCLES_COMMIT`/`NATRON_CYCLES_SHA256`), `Tests/Scene/VolumeRender_Test.cpp`
  - approach: mirror M554's mesh.cpp fix. In field.cpp, replace the single-rprim dirtying during sync with `MarkAllRprimsDirty(HdChangeTracker::DirtyTopology)` on the render index's change tracker, or the narrowest bit that makes the volume re-read its field, matching how mesh.cpp does it. Push it as `natron/m648-live-field-refresh`, bump the commit and archive SHA256 in the Dockerfile, and re-enable P1.T1's Cycles test.
  - verify: the Cycles live-field test passes on the new pin, and every other `VolumeRender*` test still passes.
  - size: M

**Verification gate:** a remote gate (debug, tests) on the F1 branch with the new Cycles pin. Every `VolumeRender*` test passes, including the new live-field tests, and no new "requires emulation" coding errors show up in the Scene tests.
