# M572 - Render Region

Render only part of the frame: a crop drawn in the viewer for interactive delegate renders, and a Data Window on USDRenderSettings that USDRender honours for final renders. The Data Window knob and its `dataWindowNDC` attribute exist already but do nothing: `USDRenderSettings.cpp` ~line 121 authors it, `RenderSettingsPrim::standardAttributes()` (`Engine/Nodes/Scene/RenderSettingsPrim.cpp:254`) reads it into `dataWindowNDC`, and nothing uses the value; `docs/USDRenderSettings.md` says "USDRender always renders the whole format and does not read these". A render takes a `HydraRender::Request` with a `GfFrustum` and a `width` x `height` (`Engine/Nodes/Scene/HydraRenderImpl.h:118`), so a crop is a narrowed frustum window plus a smaller buffer, which every delegate takes (no delegate needs to honour `CameraUtilFraming`). Viewer side, the progressive delegate render of M254 - Progressive Viewer Render runs in `Gui/SceneViewport.cpp` (its engine calls `SetFraming` ~line 2785), and the Viewer node's scene row already carries delegate controls (`Gui/ViewerTab.cpp` ~line 918, `kViewerParamRenderEverything` in `Engine/ViewerInstance.h`).

## Scope

**In:**
- USDRender honours `dataWindowNDC`: the render covers only the window; the output's region of definition is the window in pixels, the output format stays the full format (a crop, not a reformat).
- `dataWindowNDC` values outside 0..1 render overscan (the knob's tooltip already says so): the region of definition grows past the format.
- Viewer: a render region on the Viewer node (`Enable Render Region`, which only shows or hides the rectangle) whose rectangle is the Data Window of the active USDRenderSettings node, drawn with handles over the viewport and dragged with the mouse; each drag writes that node's `dataWindow` knob, and editing the knob moves the rectangle. The progressive delegate render honours it; changing it restarts the render on the region only.
- One shared value (user, 2026-11-24): the viewer's rectangle is the Data Window knob of the active USDRenderSettings node, edited directly. There is no separate viewer value and no transfer buttons.
- No RenderSettings node upstream: the region tool is disabled with a tooltip "Add a USDRenderSettings node to set a render region"; a stage whose active settings prim comes from a file (USDRead) is read-only, so the region shows that prim's window and cannot be dragged, with the tooltip naming why.
- Docs: `USDRenderSettings.md` and `USDRender.md` updated (Data Window now read); viewer docs for the region; tests.

**Out:**
- Disable Motion Blur and Disable Depth Of Field authored by USDRenderSettings but not read by USDRender: unchanged, they stay as the docs say (no milestone owns them).
- Region render for the Storm viewer: Storm draws the whole viewport every frame at interactive cost, so the region tool is disabled with a tooltip while Storm is the renderer.
- Tile or bucket order and Natron's 2D Viewer RoI: the 2D viewer's own `kViewerParamEnableUserRoI` is unrelated and untouched.
- Per-frame animated data windows: the knob is not animated (`setAnimationEnabled(false)`).
- Cropping or `pixelAspectRatio` work in compositing nodes: none.

**Edge cases:**
- Window of zero or negative size (xmax <= xmin): USDRender errors "Data Window is empty" naming the values; the viewer clamps the drag so it cannot go empty.
- Window not on whole pixels: the pixel window is rounded outward to whole pixels (`toPixelEnclosing`), the frustum narrowed to that rounded window so pixels line up with the full render.
- Conform policy: the window is in the conformed frame, the same one the full render uses, so a windowed render equals a crop of the full render.
- Viewer looking through a camera other than the render settings' camera: the rectangle is drawn only when the viewport is looking through the settings prim's camera (the Data Window is in that camera's conformed frame); otherwise the tool is disabled with a tooltip and the render is not cropped.
- Viewport resized: the rectangle keeps its Data Window coordinates in the camera frame.
- Several USDRenderSettings nodes: the one that is active (Make Active, the stage's `renderSettingsPrimPath`) owns the region; a drag edits its node.
- Outside the region while a delegate renders: the viewport's usual draw, dimmed (user).
- Drag edits the knob as one undoable edit per drag, not one per mouse move.
- Render in flight when the region changes: cancelled quickly and restarted on the new region; a stale pass is discarded as for any edit.
- Region on and the delegate switched to Storm: the rectangle stays drawn, dimmed, and has no effect until a delegate is chosen. Final renders (USDRender) honour the Data Window whatever the viewer shows.
- Render cost: a region render allocates only the window's buffer.

**Assumptions:**
- Unconfirmed: `dataWindowNDC` follows the UsdRenderSettingsBase convention (origin at the lower-left of the frame, y up). Task M572.P1.T1 reads the schema documentation in the bundled USD before fixing the conversion.
- Unconfirmed: all three delegates take a narrowed `GfFrustum` window with a smaller buffer without per-delegate changes; M572.P1.T3 checks the windowed render against a crop of the full render for each.

## Phase 572.1: Data Window in final renders

- [ ] M572.P1.T1 — Data window math
  - files: `Engine/Nodes/Scene/RenderSettingsPrim.{h,cpp}`, `Tests/Scene/RenderSettingsDataWindow_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: add `RenderSettingsPrim::dataWindowPixels(const GfVec4f& ndc, const GfVec2i& resolution)` returning the enclosing integer rectangle in Natron's bottom-up pixels (overscan allowed), and `RenderSettingsPrim::narrowFrustum(const GfFrustum& full, const GfVec4f& ndc)` returning the frustum whose window covers only that part (orthographic and perspective). Free of GL; check the NDC origin convention against the bundled `UsdRenderSettingsBase` documentation first.
  - verify: gtest — full window gives the full frustum and rectangle; a centred half window; a window with fractional edges rounded outward; an overscan window (-0.1 to 1.1); empty and inverted windows reported.
  - size: M

- [ ] M572.P1.T2 — USDRender renders and reports the window
  - files: `Engine/Nodes/Scene/USDRender.{h,cpp}`, `Engine/Nodes/Scene/HydraRenderImpl.cpp`
  - approach: USDRender takes `dataWindowNDC` from the active prim (as it takes the camera and resolution), builds the `Request` with the narrowed frustum and the window's `width` x `height`, and answers `getRegionOfDefinition()` with the window in canonical coordinates (`Format(...).toCanonicalFormat()` of the window rectangle) while `outputFormat()` stays the full format. `getRegionsOfInterest()` stays empty. The render cache key (the `Request` equality) includes the window.
  - verify: gtest — a scene with a data window of (0.25, 0.25, 0.75, 0.75) reports a RoD of half the format centred, and a format equal to the full one; an empty window sets the node error.
  - size: M

- [ ] M572.P1.T3 — Windowed render equals a crop of the full render
  - files: `Tests/Scene/USDRenderDataWindow_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: parametrised over delegates with `INSTANTIATE_TEST_CASE_P` (Storm, Cycles, MoonRay; absent ones return early as the other delegate tests do, no `GTEST_SKIP`): render a fixed scene at full size and with a data window, then compare the window's pixels with the same pixels of the full render within the delegate's tolerance (Storm exact; the path tracers with a fixed seed and enough samples, or compared on the depth and primId planes). Also an overscan window: the part outside the format is empty.
  - verify: release gate runs the test green for each available delegate.
  - size: L

- [ ] M572.P1.T4 — Docs and knob tooltip
  - files: `Engine/Nodes/Scene/docs/USDRenderSettings.md`, `Engine/Nodes/Scene/docs/USDRender.md`, `Engine/Nodes/Scene/USDRenderSettings.cpp`
  - approach: Data Window now says USDRender renders only that part, its output covers the window inside the full format, and overscan; the sentence that USDRender does not read it keeps Disable Motion Blur and Disable Depth Of Field only; the tooltip is aligned with the docs. No reference-app names.
  - verify: docs name Data Window as read; the tooltip matches.
  - size: M

## Phase 572.2: Viewer region

- [ ] M572.P2.T1 — Region geometry, hit testing and dragging
  - files: `Gui/SceneRenderRegion.{h,cpp}`, `Tests/Scene/SceneRenderRegion_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: a GL-free helper like `Gui/SceneGuides.h`: the region as a normalized rectangle, the eight handles and the body, hit test with a pixel tolerance, drag of a handle or the body clamped to the viewport and a minimum size, and the pixel rectangle for a viewport size. Includes no USD or Qt. Its coordinates are the Data Window's NDC (rectangle in the render camera's conformed frame), and it converts to and from viewport pixels given the frame's placement in the viewport.
  - verify: gtest — hit tests on each handle and the body, drags clamp, minimum size holds, pixel rectangle rounds outward.
  - size: M

- [ ] M572.P2.T2 — Viewer toggle, overlay and the shared knob
  - files: `Engine/ViewerInstance.{h,cpp}` (knob `enableRenderRegion`), `Engine/Nodes/Scene/USDRenderSettings.{h,cpp}` (a setter for the viewer's edit), `Gui/ViewerTab.cpp` (a toggle in the scene render row), `Gui/SceneViewport.{h,cpp}` (draw and mouse handling)
  - approach: one Viewer node knob, saved with the project, shown as a toggle button next to Render Everything; the rectangle is read from the active settings node's `dataWindow` and written back to it as one undoable edit per drag, found through the active settings prim's node; the viewport draws the rectangle, its handles and a dimmed outside only while the toggle is on and the renderer is not Storm; the mouse drag uses `SceneRenderRegion` and writes the knob when the drag ends and live while it moves. The Region tool follows the viewer's existing mouse-mode conventions so it does not fight the camera and selection tools; the default shortcut is chosen to avoid the existing ones and listed in the docs.
  - verify: gtest of the knob default and serialization, and that a drag's write changes `dataWindow` once and an undo restores it, and that no settings node (or a file-sourced prim) disables the tool.
  - size: L

- [ ] M572.P2.T3 — Progressive render on the region
  - files: `Gui/SceneViewport.cpp`, `Engine/Nodes/Scene/HydraRenderImpl.cpp` (only if the progressive worker builds its frustum there)
  - approach: when the region is on and a delegate renders, the worker's frustum is narrowed with `RenderSettingsPrim::narrowFrustum()` (the active settings node's `dataWindowNDC`, the same value USDRender reads, so the viewer and a final render crop alike) and its buffer is the region's pixel size; the published buffer is composited at the region's offset; a region change restarts the render on the new region and cancels the one in flight quickly. The sample counter overlay and Max Samples apply to the region render as before. Never loads a payload.
  - verify: gtest on the frustum and offset math through `SceneViewport`'s existing test seams; the Xvfb check shows the render only inside the region and the region's pixels equal the same pixels of a full progressive render at equal samples.
  - size: L

- [ ] M572.P2.T4 — Xvfb GUI check
  - files: `Tests/gui/scene_render_region.py`
  - approach: build sphere → delegate viewer with a delegate chosen, enable the region, set it by the USDRenderSettings Data Window knob and by a simulated drag (and assert the drag changed the knob, and a knob edit moved the rectangle), assert the render restarts and covers only the region (pixel check outside and inside), toggle the renderer to Storm and assert the tool disables, remove the settings node and assert the tool disables with its tooltip, save and reload the project and assert the region persists. Screenshots go to the user.
  - verify: the script passes under Xvfb at 1920x1080; the user OKs the screenshots.
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; `RenderSettingsDataWindow_Test`, `USDRenderDataWindow_Test` and `SceneRenderRegion_Test` pass; `Tests/gui/scene_render_region.py` passes under Xvfb at 1920x1080; the user OKs the screenshots.

## Decisions

- 2026-11-24 — **Viewer + RenderSettings knob (user):** a viewer-drawn crop for interactive renders and a crop knob on USDRenderSettings that final renders honour, mapping to `UsdRenderSettings` `dataWindowNDC`.
- 2026-11-24 — **Shared with the Data Window knob (user):** the viewer region is the active USDRenderSettings node's Data Window knob, edited directly; no separate viewer value and no transfer buttons. With no RenderSettings node upstream the tool is disabled (planner's definition of the empty case, for the user to overrule).
- 2026-11-24 — **Outside the region (user):** while rendering, the viewport's usual draw, dimmed.
