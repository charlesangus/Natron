# M309 - Roto Feather Falloff Banding

Stub. Follow-up from M224 - Fluid Roto And RotoPaint Interaction UAT (user, 2026-11-02): the feather falloff of Roto/RotoPaint shapes shows visible banding in some cases. Screenshots are on the bosley-native-nodes host only: `.local/rotopaint_banding_annotated.png` and `.local/rotopaint_banding_clean.png` (copy them into the milestone's investigation, as small crops, before elaborating).

Investigate first: whether the banding comes from the feather mesh/ramp generation (too few falloff steps, linear interpolation across large triangles), the falloff curve, quantisation in an 8-bit intermediate, or the viewer's display path; then fix at the source.

Blocked on: nothing; elaborate when claimed.

## Phase 309.1: Per-pixel feather

- [x] M309.P1.T1 — Diagnose the banding
  - approach: the feather was drawn by Cairo as hundreds of thin gradient patches, each sampled without antialiasing and overwriting its neighbours (rows alternating ±3–5 display levels about 2 px apart, some profiles rising), then quantised through an 8-bit mask. The dark band near the feather edge is mostly the display transform crushing low alpha.
  - size: M
- [x] M309.P1.T2 — Per-pixel float feather rasteriser (`Engine/RotoFeatherRaster.{h,cpp}`); same ramp shape, exact at pixel centres, rows split across threads
  - size: M
- [x] M309.P1.T3 — Roto/RotoPaint masks, Retouch coverage and the SplineWarp cookie-cutter mask use it (`Engine/RotoContext.cpp`, `RotoContextPrivate.h`, `SplineWarpCookie.cpp`)
  - size: M
- [x] M309.P1.T4 — Banding regression test (`Tests/RotoFeatherBanding_Test.cpp`): HD circle with a 150 px feather within 1.5e-3 of the exact ramp, monotonic with no plateaus or ripple along five lines, fall-off 2.5, half-res proxy, a concave shape
  - size: S
- [ ] M309.P1.T5 — Remote gate (ctest incl. `NativeRetouch_Test`, m224 GUI scripts) and before/after screenshots for the user
  - size: S
