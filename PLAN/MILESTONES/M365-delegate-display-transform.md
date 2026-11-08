# M365 - Delegate Display Transform

Cycles, MoonRay and Natron Utility frames in the viewer get the viewer's display transform — OCIO view, gain, gamma and lighting mode — as Storm's do. Today `readFrame` (`Engine/Nodes/Scene/HydraRenderImpl.cpp` ~:2630) composites a flat background, clamps to [0,1] and encodes through `srgbTable()` into 8-bit RGBA, which the viewer uploads as `GL_RGBA8` (`Gui/SceneViewport.cpp` ~:2997). Frames must carry float, so this needs a design pass. Answers the M254 - Progressive Viewer Render display transform question (user, 2026-11-07).

## Scope

**In:**
- Progressive delegate frames carry scene-linear float colour and alpha to the viewer.
- The viewer applies the same gain → OCIO view → gamma chain Storm gets (`cachedViewportConfig`, `hydraTransform`), and changing it re-grades without re-rendering.
- The viewer's lighting mode maps onto each delegate as closely as it can.
- The background composites after the transform, as M340 - Viewer Gain Gamma On Rendered Pixels does for Storm.

**Out:**
- Natron Utility's own delegate: F7 - Natron Utility Core routes through the same worker frame path and gets this for free.
- USDRender's offline output, which is scene-linear and has no display transform.
- Camera exposure: M367 - USD Camera Exposure.

**Edge cases:**
- Values above 1 and negative values reach the OCIO chain unclamped.
- No OCIO config: the `srgbViewportConfig` path, matching Storm.
- A lighting mode a delegate can't honour (e.g. a headlight on MoonRay): the closest mode is used and the viewer says so once.

## Phase 365.1: Design

- [ ] M365.P1.T1 — Choose the float frame and grading path
  - files: `Engine/Nodes/Scene/HydraRenderImpl.cpp` (`Frame`, `readFrame`), `Gui/SceneViewport.cpp` (`viewerGainTransform` :328, `cachedViewportConfig` :385-410, `setViewerTransform` ~:1960, lighting mode :648/:977), `## Decisions` here
  - approach: compare (a) a half-float `Frame` uploaded as `GL_RGBA16F` and graded by an OCIO GPU shader in the viewer, against (b) grading on the CPU with an OCIO processor before upload. Measure the per-frame cost at 1920x1080 and the cost of a gain drag for both. Write down how each lighting mode maps to Cycles and MoonRay (scene lights, default light, headlight). Send the choice and the mapping to the user before P2 starts.
  - verify: numbers, the chosen path and the lighting table in `## Decisions`, approved by the user.
  - size: L

## Phase 365.2: Graded delegate frames

- [ ] M365.P2.T1 — Carry float frames from the worker
  - files: `Engine/Nodes/Scene/HydraRenderImpl.{h,cpp}` (`Frame`, `readFrame`, `srgbTable`), `Tests/Scene/ProgressiveRender_Test.cpp`
  - approach: `Frame` holds half-float RGBA in scene-linear from the colour AOV, with real alpha, no background and no clamp. Remove `srgbTable()` once nothing uses it.
  - verify: `ProgressiveRender_Test`: a Cycles frame of an emissive card at 4.0 reads back about 4.0 with alpha 1, and an empty pixel reads alpha 0.
  - size: M

- [ ] M365.P2.T2 — Grade delegate frames with the viewer's transform
  - files: `Gui/SceneViewport.{h,cpp}`, `Tests/gui/` (a new `m365_delegate_display_transform.py`)
  - approach: upload the float frame as the P1.T1 design says and apply the `cachedViewportConfig` chain to it, then composite the background after, the same as Storm under M340 - Viewer Gain Gamma On Rendered Pixels. A gain, gamma or view change re-grades the last frame without touching the worker.
  - verify: GUI script with Cycles: gain 2 doubles a card pixel and leaves the background; a view change alters the pixel; neither restarts the render (worker generation unchanged).
  - size: M

- [ ] M365.P2.T3 — Map the lighting mode onto delegates
  - files: `Gui/SceneViewport.cpp` (lighting mode switch :977), `Engine/Nodes/Scene/HydraRenderImpl.cpp`, `Tests/gui/m365_delegate_display_transform.py`
  - approach: apply the lighting table from P1.T1 to Cycles and MoonRay sessions; a mode a delegate can't honour uses the closest one and logs once.
  - verify: GUI script: switching the lighting mode with Cycles changes a lit card's pixel and matches the table.
  - size: M

**Verification gate:** `ProgressiveRender_Test`, `Tests/gui/m254_progressive_viewer.py` and `Tests/gui/m365_delegate_display_transform.py` pass. Screenshots of Lobby in Cycles and MoonRay at two OCIO views and gain 2, beside Storm, sent to the user for sign-off.

## Decisions

- 2026-11-07 — Delegate frames get the viewer's display transform in its own milestone, which answers the M254 - Progressive Viewer Render display transform question (user).
