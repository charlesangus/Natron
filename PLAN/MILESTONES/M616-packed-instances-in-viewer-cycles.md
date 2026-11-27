# M616 - Packed Instances In Viewer Cycles

From the user's F15 - Native Geometry UAT (2026-11-26): GeoCopyToPoints with Pack and Instance on renders wrongly in the interactive Cycles viewer — every instance appears at (or near) the origin instead of at its point. Storm's view is expected to be right; check and say.

## Scope

**In:**
- Packed instances from native geometry (GeoCopyToPoints pack + instance, and any other packed-instance source) render at their points' transforms in the viewer's Cycles render, matching Storm and the final USDRender.
- MoonRay and the Natron Utility viewer renders checked for the same bug.

**Out:**
- Other instancing features.

## Phase 616.1: Fix

- [x] M616.P1.T1 — Instance transforms reach viewer Cycles
  - files: the native geometry → USD conversion of packed instances (GeoUsdConvert / PointInstancer authoring), the viewer render delegate path for Cycles (HydraRenderImpl, the Cycles delegate's instancer handling)
  - approach: build GeoCopyToPoints (pack + instance) on a few points; compare what the conversion authors (PointInstancer positions/orientations/scales or instance transforms) with what Storm, viewer Cycles and USDRender show; find where the transforms are dropped (unauthored attribute the Cycles delegate reads, instancer primvar names, a time-sample/default mismatch, nested instancer transforms, or the viewer's live scene index not forwarding instancer transforms to non-Storm delegates)
  - verify: ctest renders the setup with viewer Cycles (headless) and checks instance pixels land at the points; the same for USDRender
  - size: M

**Verification gate:** gate green; the user sees instances in place in viewer Cycles.

## Decisions
- 2026-11-27 — UAT approved (user); merged to main as d4d6acde2f (#393).
