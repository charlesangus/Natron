# M367 - USD Camera Exposure

Apply USD camera exposure — `exposure`, and the fStop, shutter and ISO terms as USD specifies them — in every renderer delegate, USDRender and the viewer. Lobby's cameras author `exposure = -10`, nothing in Natron reads it, and Cycles frames average about 8,000. Answers the M251 - Cycles Rendering camera exposure question (user, 2026-11-07). Nothing reads `GetExposureAttr` today; `USDCamera.cpp` handles fStop only as a lens parameter (:621, :973).

## Scope

**In:**
- One linear exposure scale per camera and time, computed as USD defines it (`ComputeLinearExposureScale` where the bundled USD has it).
- Applied once to colour output in Storm, Cycles and MoonRay, offline (USDRender) and in the viewer.
- USDCamera shows and authors the exposure attributes it carries over from a file camera.

**Out:**
- Natron Utility: F7 - Natron Utility Core applies the same scale through the shared helper.
- Display transform: M365 - Delegate Display Transform.

**Edge cases:**
- A camera with no exposure attributes: scale 1, output unchanged.
- A delegate that already applies camera exposure natively: Natron does not apply it again.
- Non-colour AOVs (depth, ids, normals, deep samples' non-colour channels) are never scaled.
- Animated exposure is sampled per frame.

**Assumptions:**
- The bundled USD carries the `exposure:*` attributes and `ComputeLinearExposureScale`; P1.T1 checks the headers in natron-dev.

## Phase 367.1: What USD and the delegates do

- [ ] M367.P1.T1 — Audit exposure in USD and the delegates
  - files: the bundled pxr `usdGeom/camera.h` and `imaging/hd/camera.h` (natron-dev container), the bundled hdCycles and hdMoonRay camera sources, `## Decisions` here
  - approach: record the USD version, the exposure attributes and formula it defines, whether `HdCamera` exposes a linear exposure scale, and whether each delegate applies it. Pick where Natron applies the scale for each delegate, so it happens exactly once.
  - verify: the findings and the per-delegate choice in `## Decisions`.
  - size: M

## Phase 367.2: Apply it

- [ ] M367.P2.T1 — Compute the camera's exposure scale
  - files: `Engine/Nodes/Scene/HydraRenderImpl.cpp` (camera lookup ~:1920/:1958), a new helper beside it, `Tests/Scene/USDCamera_Test.cpp`
  - approach: one helper returns the linear scale for a camera prim at a time, per the USD formula; 1 when nothing is authored.
  - verify: `USDCamera_Test`: exposure -10 gives 2^-10; fStop, shutter and ISO terms combine as USD's formula says; no attributes give 1; an animated value samples per frame.
  - size: M

- [ ] M367.P2.T2 — Apply exposure offline in USDRender
  - files: `Engine/Nodes/Scene/HydraRenderImpl.cpp` (offline colour readback), `Engine/Nodes/Scene/USDRender.cpp`, `Tests/Scene/RenderDelegates_Test.cpp`
  - approach: scale colour AOVs by the helper's value for delegates the audit marked as not applying it; leave the rest and every non-colour AOV alone.
  - verify: `RenderDelegates_Test`: an emissive card through a camera at exposure -1 renders half as bright as at 0 in Storm, Cycles and MoonRay; depth is identical.
  - size: M

- [ ] M367.P2.T3 — Apply exposure in the viewer
  - files: `Engine/Nodes/Scene/HydraRenderImpl.cpp` (`readFrame`), `Gui/SceneViewport.cpp` (Storm colour path), `Tests/gui/m254_progressive_viewer.py`
  - approach: apply the same scale to progressive delegate frames and to Storm's viewer render when looking through a scene camera, before gain and the display transform. The free viewer camera has no exposure.
  - verify: GUI script: looking through a camera at exposure -1 halves a card pixel in Storm and Cycles; the free camera leaves it unchanged.
  - size: M

- [ ] M367.P2.T4 — Carry exposure through USDCamera
  - files: `Engine/Nodes/Scene/USDCamera.cpp` (~:961-973), `Tests/Scene/USDCamera_Test.cpp`
  - approach: add the exposure attributes to the aspects USDCamera reads from a file camera and authors, with knobs labelled after their script names.
  - verify: `USDCamera_Test`: a file camera's exposure survives USDCamera unchanged, and setting the knob authors it.
  - size: M

**Verification gate:** `USDCamera_Test`, `RenderDelegates_Test` and `Tests/gui/m254_progressive_viewer.py` pass. A Lobby render in Cycles averages near the Storm render's level instead of about 8,000, with a screenshot of both sent to the user.

## Decisions

- 2026-11-07 — USDRender, the viewer and every delegate apply camera exposure, which answers the M251 - Cycles Rendering camera exposure question (user).
