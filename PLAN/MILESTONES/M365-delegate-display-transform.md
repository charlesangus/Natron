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

- [x] M365.P1.T1 — Choose the float frame and grading path
  - files: `Engine/Nodes/Scene/HydraRenderImpl.cpp` (`Frame`, `readFrame`), `Gui/SceneViewport.cpp` (`viewerGainTransform` :328, `cachedViewportConfig` :385-410, `setViewerTransform` ~:1960, lighting mode :648/:977), `## Decisions` here
  - approach: compare (a) a half-float `Frame` uploaded as `GL_RGBA16F` and graded by an OCIO GPU shader in the viewer, against (b) grading on the CPU with an OCIO processor before upload. Measure the per-frame cost at 1920x1080 and the cost of a gain drag for both. Write down how each lighting mode maps to Cycles and MoonRay (scene lights, default light, headlight). Send the choice and the mapping to the user before P2 starts.
  - verify: numbers, the chosen path and the lighting table in `## Decisions`, approved by the user.
  - size: L

## Phase 365.2: Graded delegate frames

- [x] M365.P2.T1 — Carry float frames from the worker
  - files: `Engine/Nodes/Scene/HydraRenderImpl.{h,cpp}` (`Frame`, `readFrame`, `srgbTable`), `Tests/Scene/ProgressiveRender_Test.cpp`
  - approach: `Frame` holds half-float RGBA in scene-linear from the colour AOV, with real alpha, no background and no clamp. Remove `srgbTable()` once nothing uses it.
  - verify: `ProgressiveRender_Test`: a Cycles frame of an emissive card at 4.0 reads back about 4.0 with alpha 1, and an empty pixel reads alpha 0.
  - size: M

- [x] M365.P2.T2 — Grade delegate frames with the viewer's transform
  - files: `Gui/SceneViewport.{h,cpp}`, `Tests/gui/` (a new `m365_delegate_display_transform.py`)
  - approach: upload the float frame as the P1.T1 design says and apply the `cachedViewportConfig` chain to it, then composite the background after, the same as Storm under M340 - Viewer Gain Gamma On Rendered Pixels. A gain, gamma or view change re-grades the last frame without touching the worker.
  - verify: GUI script with Cycles: gain 2 doubles a card pixel and leaves the background; a view change alters the pixel; neither restarts the render (worker generation unchanged).
  - size: M

- [x] M365.P2.T3 — Map the lighting mode onto delegates
  - files: `Gui/SceneViewport.cpp` (lighting mode switch :977), `Engine/Nodes/Scene/HydraRenderImpl.cpp`, `Tests/gui/m365_delegate_display_transform.py`
  - approach: apply the lighting table from P1.T1 to Cycles and MoonRay sessions; a mode a delegate can't honour uses the closest one and logs once.
  - verify: GUI script: switching the lighting mode with Cycles changes a lit card's pixel and matches the table.
  - size: M

## Phase 365.3: Renderer parity in the viewer

- [ ] M365.P3.T1 — Cycles light-level setting that matches Storm and MoonRay
  - files: the Cycles render-settings knobs in USDRenderSettings (Engine/Nodes/Scene/), the Cycles fork's Hydra light translation (charlesangus/cycles `src/hydra/light.cpp`, the pinned branch) or Natron's light handoff, the image pin, a Cycles light-level test in Tests/Scene/
  - approach: add a Cycles render setting (knob label = readable script name, e.g. `cyclesMatchLightLevels` "Match Light Levels") that backs out the π multiplier Cycles' Hydra plugin applies to non-distant light intensity, so Cycles renders at the same level as Storm and MoonRay for the same scene and exposure. Check whether the dome/area/sphere/rect/disk lights all carry the factor and treat each one the plugin scales; distant lights are already unscaled.
  - verify: a test renders a lit card with a sphere light and a dome light in Cycles with the setting on and Storm/MoonRay references, and the mean levels match within a few percent; with the setting off Cycles is ~π brighter. Lobby screenshots re-sent: Cycles beside Storm and MoonRay at matching brightness.
  - size: M

- [x] M365.P3.T2 — MoonRay reports its sample count to the viewer
  - files: hdMoonray fork (render pass / render buffer progress), Natron's progressive delegate path that fills the HUD sample count and convergence (Engine/Nodes/Scene/HydraRenderImpl.*, Gui/SceneViewport.cpp), the image pin
  - approach: read MoonRay's progress (pass / samples per pixel, or the render-progress fraction the Arras/RenderContext exposes) through hdMoonray's render pass or a render-buffer/settings stat, and feed it to the same sample-count and converged state the viewer HUD and the convergence check use for Cycles.
  - verify: the Lobby run shows "n / N samples" for MoonRay rising to convergence and the "MoonRay converges on the Lobby" check in build/m365-lobby/m365_lobby_shots.py passes; a unit test asserts a MoonRay render reports a sample count > 0 and converges.
  - size: L

**Verification gate:** `ProgressiveRender_Test`, `Tests/gui/m254_progressive_viewer.py` and `Tests/gui/m365_delegate_display_transform.py` pass. Screenshots of Lobby in Cycles and MoonRay at two OCIO views and gain 2, beside Storm, sent to the user for sign-off.

## Decisions

- 2026-11-07 — Delegate frames get the viewer's display transform in its own milestone, which answers the M254 - Progressive Viewer Render display transform question (user).
- 2026-11-20 — P1.T1 design (awaiting user approval; P2 does not start before it).
  - **Code since the brief:** M340 - Viewer Gain Gamma On Rendered Pixels replaced `viewerGainTransform`/`cachedViewportConfig`/`hydraTransform` with a GPU grade pass in `Gui/SceneViewport.cpp`: `displayProcessor()` :171, `gradeFragmentShader()` :212 (unpremult, gain, OCIO `toDisplay`, gamma, premult, blend over `backgroundShown`), `buildGrade`/`ensureGrade`/`drawGrade` :1269-1400, `setDisplayTransform` :2051 (gain/gamma are uniforms; only a display/view/look change rebuilds the shader), `kSrgbDisplayFunction` as the no-OCIO path. `renderProgressive` :2977 skips it: `GL_RGBA8` upload, then `glBlitFramebuffer`.
  - **Numbers** (Intel N100, 4 cores, 1920x1080 RGBA; values in [-0.5, 6]; best of 20 or 5 runs):
    | Step | (a) half frame + GPU grade | (b) CPU OCIO before upload |
    |---|---|---|
    | Worker colour loop per frame (today: float to sRGB8 table + background, 29.7 ms) | float to half (F16C), 5.7 ms | OCIO CPU, studio-config v4 / ACES 2.0 SDR 100 nits: 1158 ms on 1 thread, 464 ms on 4. Un-tone-mapped: 37 / 13 ms. Raw: 4 ms |
    | Upload per frame | 16.6 MB `GL_RGBA16F` (today 8.3 MB `GL_RGBA8`); not measured here, P2 records `uploadMs` at HD and 4K in `m254_progressive_viewer.py` | 8.3 MB `GL_RGBA8` |
    | Gain/gamma drag tick | one uniform and one full-screen pass, the pass Storm already runs on every paint; no upload, no CPU work | the whole OCIO CPU pass again and a re-upload: about 0.5 s per tick for the default view on 4 cores |
    Optimisation levels LOSSLESS, GOOD and DRAFT do not change the ACES 2.0 CPU cost (1336 / 1186 / 1185 ms).
  - **Proposed path: (a).** (b) stalls a gain drag for half a second under the default config, and it would also have to keep a float copy of the frame to re-grade. Design for (a):
    - `Frame::rgba` becomes half-float RGBA: premultiplied, scene-linear, from the colour AOV, with real alpha. No background and no clamp. When the AOV is already `HdFormatFloat16Vec4` the rows are copied. `srgbTable()` goes.
    - `refineTexture` becomes `GL_RGBA16F`, uploaded as `GL_HALF_FLOAT`. `drawGrade` is generalised to take the colour texture, optional depth and the source size, and `renderProgressive` calls it in place of the blit. One shader and one set of LUTs serve both paths, so the background, the no-OCIO sRGB path and unclamped OCIO input match Storm by construction. A frame whose size differs from the view's is sampled stretched (normalised coordinates, linear filtering) and writes no depth, as today.
    - The frame's depth moves from the PBO plus `glDrawPixels` to an `R32F` texture that the grade pass reads, the way Storm's depth AOV is read.
    - A display, gain or gamma change only repaints. It never calls into `ProgressiveRender`, so the worker generation is unchanged.
    - Half floats top out at 65504, and values above that become inf. Lighting and display values are far below that limit.
  - **Lighting table.** Today `ProgressiveRender` always renders with `enableSceneLights = true` and no headlight, so the viewer's mode is ignored. Proposed mapping, which also matches the reference DCC, where "Headlight Only" with a path-traced delegate disables the scene's lights and uses a default headlight:
    | Viewer mode | Storm (today) | Cycles | MoonRay |
    |---|---|---|---|
    | Scene Lights | stage lights, no camera light | `enableSceneLights = true`, no camera light; a stage with no lights renders unlit, as offline | same as Cycles |
    | Headlight | stage lights off, camera simple light | `enableSceneLights = false` (UsdImagingGL's light-pruning scene index removes the stage's lights for any delegate), plus `setHeadlight()` (`HydraRenderImpl.cpp` :628, already used by USDRender's Headlight knob). HdxTaskController turns it into a camera-following `distantLight` sprim on non-Storm delegates | same as Cycles, if MoonRay's delegate lists `distantLight` (`DelegateSupport::query`). If it does not, the closest mode is Scene Lights, and the viewer logs that once per renderer |
    | Auto | scene lights if the stage has any, else headlight | same rule (`usesSceneLights()`) | same rule |
    The simple light's ambient and material terms have no meaning to a path tracer and are dropped. A mode change remakes the engine and restarts the render (a headlight cannot be removed from an engine, :1492). That is expected, unlike a gain change. The headlight's brightness is whatever the task controller gives the distant light, so it will not match Storm's look.
  - **Open for the user:** approve (a); approve half over float32 (float32 doubles the upload, 33 MB at HD and 133 MB at 4K); approve the lighting table, including leaving the headlight at the task controller's intensity rather than tuning it to look like Storm.
- 2026-11-20 — **Option (a), half-float (user):** progressive Cycles and MoonRay frames arrive as half-float premultiplied RGBA and go through the viewer's GPU grade pass, with depth in R32F. **Lighting table as proposed (user):** Headlight switches the stage's lights off and adds a camera-following distant light at Hydra's default brightness. Auto follows Storm's rule. A mode change restarts the render. If MoonRay has no distant light, it falls back to Scene Lights with one warning.
- 2026-11-21 — P2.T1–T3 landed together in 79abe8726; main merged in e994456e6 (one conflict in Gui/SceneViewport.cpp: drawStormGrade, then main's M278 selection pass). Remote gate run 44940439968 (fast, both ctest legs): ProgressiveRender_Test passes with Cycles loaded (FramesAreSceneLinearWithTheirAlpha ran, no skip), m254_progressive_viewer 39/39, m365_delegate_display_transform 49/49. Lobby screenshots for sign-off are next, locally under Xvfb.
- 2026-11-22 — **Camera exposure in the viewer (user):** pulled into M365 from M367 - USD Camera Exposure's viewer task. 4783c0d14: the look-through camera's `ComputeLinearExposureScale` multiplies the viewer gain in the display grade (display only); knob `applyCameraExposure` ("Apply Camera Exposure", default on) as a toggle right of the gain slider, plus a read-only label with the exposure in stops. GUI test section added (Cycles). Lobby screenshots to re-send after the gate.
- 2026-11-27 (PM) — Lobby screenshots on a vast RTX GPU (fast AppImage f9bc022637): Apply Camera Exposure darkens every renderer (mean max on→off: Storm 12.9/213.3, Cycles 130.3/246.6, MoonRay 39.0/230.4); Storm+Cycles 51/51 checks, MoonRay's convergence check fails because it reports no sample count ("-" in the HUD) while its frames arrive; hdMoonray logs _MarkRprimDirty coding errors on /World/assembly_Lobby prims during payload load. Cycles renders ~3x brighter than Storm/MoonRay (π on non-distant lights in Cycles' Hydra plugin). Sent to the user with those two questions.
- 2026-11-27 — **Cycles light level (user):** add a Cycles render setting that backs out the π multiplier on non-distant lights so Cycles matches Storm and MoonRay (P3.T1). **MoonRay sample count (user):** fix in M365 (P3.T2).
- 2026-11-27 (PM) — P3.T2 landed Natron-side (6c548b02e1): hdMoonray already reports percentDone; samples = floor(percentDone/100 × pixel samples), converged = cap. P3.T1 finding: Cycles' Hydra plugin multiplies every non-distant light by π but the kernel divides sphere/rect/disk by π again, so only the dome light is π too bright; distant lights are ×4. Per the user's intent (Cycles matches Storm and MoonRay), the setting corrects each light type that differs, verified per type against Storm/MoonRay.
