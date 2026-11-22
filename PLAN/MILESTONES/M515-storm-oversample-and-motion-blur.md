# M515 - Storm Oversample And Motion Blur

USDRender's Storm path renders each frame once, through `HydraRender::render`/`withRender` → `RenderThread::renderEngine` (`Engine/Nodes/Scene/HydraRenderImpl.cpp:1437`). The camera is a free camera there (`SetCameraState(view, projection)`, :1538), and the frame time is `params.frame` (:1541). This milestone adds two USDRender controls, both off by default. Samples renders N sub-pixel-jittered passes and averages them. Motion Blur spreads those passes over the camera prim's shutter (`shutter:open`/`shutter:close`, frame-relative) unless the settings prim's `disableMotionBlur` is on (read into `RenderSettingsPrim::StandardAttributes`, `RenderSettingsPrim.h:102`, and unused today). Colour is averaged. Depth and primId come from one representative pass and are never averaged.

## Scope

**In:**
- `HydraRender::Request` gains a sub-sample list, and `renderEngine` renders and accumulates it on both the pack (GPU) and CPU readback paths.
- USDRender gets the Samples (`samples`) and Motion Blur (`motionBlur`) knobs. It builds the schedule from the camera shutter and computes one frustum per sub-frame time. Docs go in `Engine/Nodes/Scene/docs/USDRender.md`.
- Unit, Storm and node tests, plus an Xvfb GUI script with screenshots.

**Out:**
- The Utility raster renderer's sub-frame accumulation: M514 - Utility Raster Mode.
- Anti-aliasing and motion blur for Cycles and MoonRay. They keep their own settings (`cycles:integrator:motion_blur`, `enableMotionBlur`).
- Shutter knobs on USDCamera. The shutter is authored in a file or with USDSetAttribute.
- Progressive viewer display of the accumulation. `USDRender::render` (`USDRender.cpp:720`) returns one finished image, and `ProgressiveRender` serves non-Storm delegates only, so this is not cheap.
- Reconstruction filters other than box, adaptive sampling, velocity-based blur, and depth-of-field sampling.

**Edge cases:**
- Samples 1 with Motion Blur off: today's single-render path, bit-identical, with the same reuse key.
- Motion Blur on with Samples 1: one pass at the frame time, so no blur. The tooltip says blur needs Samples above 1.
- Motion Blur on with a closed shutter (open == close, the USD fallback 0/0) or with close < open: no time spread, and the node warns that the camera's shutter is closed. When `disableMotionBlur` is on there is no spread and no warning.
- The representative pass has zero jitter and the sub-frame time nearest the frame (clamped into the shutter). It renders **last**, so the engine's depth and primId AOVs, `decodeIds` and `primPaths` are its own with no copies.
- Planes asked for one at a time reuse one accumulation. The `renderEngine` reuse check (:1509) also compares the sub-sample list.
- Cancel is checked before each pass. A cancelled accumulation returns "cancelled" and keeps no partial colour.
- `StormGLSection` is entered per pass, not across the loop, so the main thread's scene view can draw between passes.
- The accumulation buffer is RGBA32F at render size and counts against the engine GPU budget (`setMemoryBudget`).
- A renderer other than Storm: both knobs are disabled and ignored. `knobChanged` (`USDRender.cpp:184`) already reacts to the renderer and settings-prim knobs.
- Proxy (mipmap) renders: same pass count, with jitter in render-resolution pixels.

**Assumptions:**
- UsdImaging evaluates transforms and points at non-integer `UsdTimeCode`s (interpolated), so sub-frames move.
- Live textures resolve per frame. Sub-frames show the frame's texture image, and only the stage moves.
- Unverified: Cycles and MoonRay may see no prim shutter through the free camera. That is not this milestone's concern.

## Decisions

- 2026-11-22 — Storm through USDRender gains a sample count, each sample a sub-pixel jittered render, averaged (user, grilling): anti-aliasing for Storm frames.
- 2026-11-22 — Motion blur accumulates sub-frames over the scene's shutter, the camera prim's `shutter:open`/`shutter:close`, gated by the settings prim's `disableMotionBlur` (user, grilling; planner chose the attributes, as Natron reads no shutter today).
- 2026-11-22 — Both default off, at Samples 1 and Motion Blur off (user, grilling): interactive Storm speed is unchanged.
- 2026-11-22 — With both on, time and AA share one count, and each sub-frame is jittered (user, grilling): N passes, not N×M.
- 2026-11-22 — id and depth AOVs take one sample's value and are never averaged (planner): averaged ids name no prim.
- 2026-11-22 — The policy (schedule, shutter, frusta) lives in USDRender and the mechanism (render, jitter, accumulate) in HydraRender (planner): HydraRender stays camera-agnostic, and the schedule is unit-testable without GL.

## Phase 515.1: Sample schedule

- [ ] M515.P1.T1 — Pure sub-sample schedule helper
  - files: `Engine/Nodes/Scene/StormSampling.h`, `Engine/Nodes/Scene/StormSampling.cpp` (new; picked up by the `Nodes/Scene/*.cpp` glob, `Engine/CMakeLists.txt:59`)
  - approach: `StormSampling::schedule(int samples, bool motionBlur, double shutterOpen, double shutterClose)` returns N entries of {time offset in frames, jitter in pixels within [-0.5, 0.5]²}. Jitter uses Halton(2,3) centred on the pixel. Times are stratified, `open + (i+0.5)/N·(close-open)`, and assigned to jitter indices through a fixed coprime stride so time and jitter don't correlate. When motion blur is off or the shutter is closed, every offset is 0. The representative entry (jitter 0, offset nearest 0) goes last. Include `closedShutter(open, close)`. No GL or USD dependencies.
  - verify: `tools/ci/local/syntax-check.py --root <wt> Engine/Nodes/Scene/StormSampling.cpp`
  - size: M
- [ ] M515.P1.T2 — Unit tests for the schedule
  - files: `Tests/Scene/StormSampling_Test.cpp` (new)
  - approach: test that N=1 gives {0, (0,0)}; that the last entry has zero jitter and the offset nearest 0; that all jitters are within half a pixel and distinct; that times are stratified and cover the shutter; that motion blur off or a closed shutter gives all-zero offsets; and that the output is deterministic. Bundled gtest is old: `INSTANTIATE_TEST_CASE_P` only, no `GTEST_SKIP`, no `ASSERT_TRUE(ptr)`.
  - verify: `ctest -R StormSampling` passes at the phase build
  - size: M

## Phase 515.2: Accumulation in HydraRender

- [ ] M515.P2.T1 — Render and accumulate sub-samples on the pack path
  - files: `Engine/Nodes/Scene/HydraRenderImpl.h` (`Request`, :117), `Engine/Nodes/Scene/HydraRenderImpl.cpp` (`renderEngine` :1437, reuse check :1509, pass loop :1552, `EngineEntry` :493, `pack` :2057, `kPackShader` :556)
  - approach: add `struct SubSample { UsdTimeCode time; GfFrustum frustum; GfVec2d jitter; }` and `std::vector<SubSample> subSamples` to `Request`. Empty keeps today's path unchanged. Otherwise, for each entry: check cancel; enter a `StormGLSection`; set the camera state with the projection post-multiplied by an NDC translation `(2·jx/width, 2·jy/height, 0)` (valid for perspective and ortho); render at `params.frame = time`; and add the colour AOV into an RGBA32F accumulation texture on the entry with a small compute pass. Then scale by 1/N. `pack()` reads colour from the accumulation texture when there is one, and depth and primId from the engine (the representative pass, rendered last). Extend the reuse key with the sub-sample list, and count the texture in the engine GPU memory.
  - verify: `ctest -R "USDRenderStorm|USDRenderHandoff"` passes unchanged at the phase build (the empty-list path is identical)
  - size: L
- [ ] M515.P2.T2 — Accumulated colour on the CPU readback path
  - files: `Engine/Nodes/Scene/HydraRenderImpl.cpp` (`cpuFrame` :1724, `cpuWindow` :1832)
  - approach: when the context cannot pack, `cpuFrame`/`cpuWindow` take colour from the entry's accumulation (read back once per accumulation, not per pass). Depth and primId stay those of the last (representative) pass.
  - verify: `ctest -R USDRenderStorm` passes unchanged at the phase build
  - size: M
- [ ] M515.P2.T3 — Request-level Storm tests
  - files: `Tests/Scene/USDRenderStorm_Test.cpp` (`USDRenderStormTest`, :762)
  - approach: call `HydraRender::render` directly. Check that a one-entry zero-jitter list matches an empty-list render bit for bit. With 8 jittered entries, a card edge has intermediate alpha where the single render is 0 or 1. With 8 entries, every primId value appears in the representative single render and depth equals it exactly. An `aborted` that turns true after the first pass fails the render with "cancelled", checked by result, not timing. Old gtest rules as in P1.T2.
  - verify: `ctest -R USDRenderStorm` passes at the phase build
  - size: M

## Phase 515.3: USDRender controls

- [ ] M515.P3.T1 — Samples and Motion Blur knobs, and the schedule from the camera shutter
  - files: `Engine/Nodes/Scene/USDRender.h` (`kUSDRenderParam*`, :44), `Engine/Nodes/Scene/USDRender.cpp` (`initializeKnobs` ~:123, `knobChanged` :184, `prepareRender` :620)
  - approach: add "Samples" (`samples`, int, default 1, min 1, display max 64) and "Motion Blur" (`motionBlur`, bool, default off), both non-animated, with tooltips that say they apply to Storm and that blur needs Samples above 1. Disable both when the active renderer isn't Storm. In `prepareRender`, for Storm with Samples above 1: read the camera prim's `shutter:open`/`shutter:close` at `active.time`, and apply `disableMotionBlur` from `active.standard`. Call `StormSampling::schedule`, compute a `cameraFrustum` per sub-frame time, and fill `request.subSamples`. When the shutter is closed, add the closed-shutter warning to `setup->warning`. No other product names in code or tooltips.
  - verify: syntax-check `USDRender.cpp`; `ctest -R "USDRender|USDRenderStorm"` passes unchanged at the phase build (defaults are off)
  - size: M
- [ ] M515.P3.T2 — Node docs
  - files: `Engine/Nodes/Scene/docs/USDRender.md` (Controls)
  - approach: document Samples and Motion Blur: Storm only, off by default, one shared count, where the shutter comes from, the `disableMotionBlur` override, and that depth and primId come from one sample.
  - verify: the doc names both knobs with their script names, matching the P3.T1 tooltips
  - size: S
- [ ] M515.P3.T3 — Node-level tests
  - files: `Tests/Scene/USDRenderStorm_Test.cpp`
  - approach: build a scene with a card translating across frames and a camera with shutter -0.25..0.25. Check that the defaults equal today's render; that Motion Blur with Samples 8 smears the card over its motion span; that `disableMotionBlur` on the settings prim, or a closed shutter, gives the unblurred AA render, plus the warning in the closed-shutter case; and that a Cycles renderer leaves the knobs disabled. Old gtest rules as in P1.T2.
  - verify: `ctest -R USDRenderStorm` passes at the phase build
  - size: M

## Phase 515.4: GUI check

- [ ] M515.P4.T1 — Xvfb GUI script and screenshots
  - files: `Tests/gui/m515_storm_oversample.py` (new; neutral names)
  - approach: at 1920x1080 under the Xvfb recipe, set up the P3.T3 scene in a viewer. Take screenshots of the USDRender panel, of Samples 1 vs 16 (edge crop), and of Motion Blur on. While a Samples 64 render runs, change a knob and confirm the viewer repaints and the render restarts with the new value. Check behaviour only, with no timing lines.
  - verify: the script exits 0 under the Xvfb recipe; screenshots are shared with the user one file each and approved
  - size: M

**Verification gate:** `ctest -R "StormSampling|USDRenderStorm|USDRenderHandoff|USDRender"` green; full debug PR CI green; `Tests/gui/m515_storm_oversample.py` passes under Xvfb and the user approves its screenshots; no GPU-host timing lines.
