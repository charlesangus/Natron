# M627 - Native Geometry Viewer Cook Fixes

Small native-geometry viewer bugs found by the M617 - Native Geometry Viewer Architecture Spike research (`PLAN/DESIGN/2026-11-26-m617-research/flash-root-cause.md`, §1 and §6). The F41 - Native Geometry Scene Index work won't fix them, because they sit in cook orchestration and conversion rather than in USD staging. This is a standalone milestone on main after F15 - Native Geometry lands (user, 2026-11-26), not part of F41. Line numbers are as of 11e3a1b115.

## Scope

**In:**
- Older geometry cooks can no longer replace newer ones during a drag.
- A failed cook keeps the last good scene and shows the error as an overlay.
- Packed native instancers keep their admission record across a resync.
- The extent of points accounts for their widths.
- GeoScatter pscale follows the Houdini widths convention, with a sensible slider range.

**Out:**
- Anything in the scene index: F41 - Native Geometry Scene Index.

**Assumptions:**
- Unconfirmed: the user's grey flash while dragging GeoScatter pscale is overlapping large grey points (pscale used as the full diameter, slider up to 1), not a pipeline frame. M627.P1.T5 fixes that case either way.

## Phase 627.1: Fixes

- [ ] M627.P1.T1 — Reject stale geometry renders
  - files: `Engine/ViewerInstance.cpp` (~4026-4074, 5255-5266), `Engine/ViewerInstancePrivate.h` (~442)
  - approach: apply the image path's render-age check to the geometry path before `serveFrame`, `lastScene` and `sceneEvaluated`, so a late older cook is dropped.
  - verify: a ctest or GUI script runs a slow upstream node, issues two value changes, and checks that the viewer ends on the second value with no intermediate frame showing the first after it.
  - size: M
- [ ] M627.P1.T2 — Keep the last good scene when a cook fails
  - files: `Engine/ViewerInstance.cpp` (~4026), `Gui/SceneViewport.cpp` (`setError` ~11037, composing ~9668), `Engine/Nodes/Scene/SceneDrawFilter.cpp` (~1249, 1310-1315)
  - approach: on a cook error, leave the last presented scene in place and draw the error text as an overlay. The next good cook updates in place with no composing placeholder.
  - verify: a GUI script makes a GeoWrangle error mid-drag and checks the geometry stays visible with the error overlay, then the fixed value shows without blank frames.
  - size: M
- [ ] M627.P1.T3 — Keep packed native instancers through a resync
  - files: `Engine/Nodes/Scene/SceneDrawFilter.cpp` (~489, 526-572)
  - approach: hold an instancer's admission record across `forget()` when it is re-added in the same resync, so instances are not queued for admission again.
  - verify: a SceneDrawFilter ctest resyncs an instancer with 1000 instances and checks every instance stays admitted.
  - size: M
- [ ] M627.P1.T4 — Pad points extent by their widths
  - files: `Engine/Nodes/Geometry/GeoUsdConvert.cpp` (extent ~1822)
  - approach: for points and curves with widths, grow the extent by half the largest width.
  - verify: `GeoUsdConvert_Test` checks the extent of a points detail with pscale 0.5.
  - size: M
- [ ] M627.P1.T5 — GeoScatter pscale range and widths convention
  - files: `Engine/Nodes/Geometry/GeoScatter.cpp` (~492, 594-596), `Engine/Nodes/Geometry/GeoUsdConvert.cpp` (~1241-1246)
  - approach: check Houdini's pscale → widths mapping in `.local/docs/houdini/solaris/sop_import.md` and USD's points docs, and match it (radius versus diameter) in GeoUsdConvert. Lower the GeoScatter pscale slider's display maximum to a range where default scatters don't merge into one blob (hard maximum unchanged).
  - verify: `GeoUsdConvert_Test` checks widths for a known pscale. A GUI screenshot of a default GeoScatter at the slider maximum shows separate points.
  - size: M

**Verification gate:** ctest and the GUI scripts above pass under Xvfb. PR CI is green. A UAT AppImage with a steps `.md` lets the user re-check the GeoScatter pscale drag.

## Decisions
- 2026-11-26 — Separate milestone after F15 (user): kept out of M600 - Native Geometry UAT Round 2 and out of F41 - Native Geometry Scene Index.
