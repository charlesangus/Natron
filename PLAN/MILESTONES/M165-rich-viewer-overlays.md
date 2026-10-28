# M165 - Rich Viewer Overlays

Let nodes draw rich, arbitrary viewer overlays: overlaid images (per-pixel data such as masks, drawn over the viewer image, never into renders), handles, points, lines, text and free-form drawing from native nodes, not just the stock Transform/CornerPin/Position/Rectangle interacts. Its first consumers are the darktable darkroom-only aids M80 - Native RAW Support deferred here: the highlights mask views (clipped, candidates, combine, strength), the dual-demosaic mask, the capture-sharpen sharpened-areas and radius views, and hot-pixel Mark Fixed (rawprepare/demosaic/highlights/hotpixels at darktable `release-5.6.2`). A later consumer is M216 - Auto-Straighten's detected-line display and click/drag toggling plus its drawn-lines guide mode (darktable `ashift`), reading the node's `lines(time)` accessor.

Scouted facts (2026-10-23, re-check at elaboration): `EffectInstance::drawOverlay` (`Engine/EffectInstance.h:2552`) is an empty virtual no native node overrides; native nodes only call `Node::addRectangleInteract` (`Engine/NodeOverlay.cpp:317`); host primitives live in `Gui/HostOverlay.cpp` (points `:1172`, handles `:1246`, line loops `:1312`, text `:1036`); OFX overlays get GL via `Engine/OfxOverlayInteract.cpp`; no overlay path receives image data (viewer pixels go only to the viewer texture, `Engine/ViewerInstance.cpp:2037-2112`); the viewer-render flag exists as `isRenderUserInteraction` in `ParallelRenderArgs` (`Engine/ParallelRenderArgs.h:308`) but nodes can't query it.

Filed 2026-10-23 at the user's request.

## Scope

**In:** an engine-side overlay draw list for native nodes (points, lines/polylines, filled polygons, hit-testable handles, text, free-form strokes) with pen events; a way for nodes to ask whether a render is for the viewer; per-pixel overlay images composited by the viewer (design per Q1); the deferred M80 darktable aids, and pending Q3 RawColorCalibration's checker patch grid and ΔE overlay.

**Out:** M216's detected-line display/toggling and drawn-lines guide mode (follow-up stub after M216 merges, Q4); darktable's on-image hue picker (M215); porting Tracker/RotoPaint GL interacts to the draw list (E14 stub); GPU region outlines (M116).

**Edge cases:**
- An overlay never enters a render, an image or a cache key (tested).
- Overlays draw in the node's canonical space, as host interacts do, even when the viewer shows a downstream node.
- Single-pixel overlay data (hot pixels) max-downsamples when zoomed out.
- Thousands of primitives (M216 line lists) draw in one batched upload per list.

**Assumptions:** `drawOverlay` is now at `Engine/EffectInstance.h:3076`; TrackerNode.cpp:722 and RotoPaint.cpp:997 override it; other scouted line numbers re-checked in P1.T1.

## Phase 165.1: Native overlay drawing

- [ ] M165.P1.T1 — Overlay draw list and viewer-render query
  - files: Engine/OverlayDrawList.{h,cpp} (new), Engine/EffectInstance.{h,cpp}, Tests/OverlayDrawList_Test.cpp
  - approach: virtual buildOverlay(time, scale, view, OverlayDrawList*); widths in screen px, positions canonical, handles carry ids; isViewerRender() reads isRenderUserInteraction from ParallelRenderArgs.
  - verify: gtests on list building, handle hit-test maths and the flag.
  - size: M
- [ ] M165.P1.T2 — Draw the lists in ViewerGL
  - files: Gui/NodeOverlayRenderer.{h,cpp} (new), Gui/ViewerGL.cpp, Gui/HostOverlay.cpp
  - approach: one batched VBO per list, the host overlays' shadow pass, text via renderText.
  - verify: Xvfb screenshot of a test-built list.
  - size: L
- [ ] M165.P1.T3 — Route pen events to handles
  - files: Engine/EffectInstance.{h,cpp}, Gui/NodeOverlayRenderer.cpp, Gui/ViewerGL.cpp
  - approach: hover highlight; handle-id callbacks whose knob edits go on the global undo stack.
  - verify: Xvfb drag moves a knob; undo reverts.
  - size: M
- [ ] M165.P1.T4 — Test-only primitives node and GUI script
  - files: Tests/Native/OverlayTestNode.cpp, Tests/gui/m165_overlay_primitives.py
  - approach: a test-registered node drawing every primitive type, no OFX.
  - verify: remote GUI job passes; screenshots.
  - size: M

## Phase 165.2: Overlay images (waits on Q1)

- [ ] M165.P2.T1 — Overlay planes in the engine
  - files: Engine/EffectInstance.{h,cpp}, Engine/ViewerInstance.cpp, Engine/Node.{h,cpp}
  - approach: per Q1; recommended (a) overlay-only planes hidden from downstream, requested by the viewer through the normal render/cache path at its RoI and scale.
  - verify: gtest: with an overlay active, the main render's hashes and cache entries are unchanged.
  - size: L
- [ ] M165.P2.T2 — Composite overlay images in the viewer
  - files: Gui/ViewerGL.cpp, Gui/NodeOverlayRenderer.cpp
  - approach: after the display transform, with tint and opacity; nearest filtering and max-downsampling.
  - verify: Xvfb screenshots at 100% and 25% zoom.
  - size: L

## Phase 165.3: darktable aids (waits on Q1)

- [ ] M165.P3.T1 — RawHighlights mask views (clipped, candidates, combine, strength)
  - files: Engine/Nodes/Raw/RawHighlights*.cpp, Tests/Native/
  - approach: darktable release-5.6.2 highlights.c mask display.
  - verify: gtest on the mask planes; Xvfb screenshot.
  - size: M
- [ ] M165.P3.T2 — Dual-demosaic mask and capture-sharpen sharpened-areas/radius views
  - files: Engine/Nodes/Raw/RawDemosaicDual.cpp, RawDemosaicCapture.cpp, RawDemosaic.cpp, Tests/Native/
  - approach: darktable release-5.6.2.
  - verify: gtest; Xvfb screenshot.
  - size: M
- [ ] M165.P3.T3 — RawHotPixels Mark Fixed
  - files: Engine/Nodes/Raw/RawHotPixels.{h,cpp}, Tests/Native/
  - approach: replaces the cancelled M80.P7.T24.
  - verify: gtest on marked pixels; Xvfb screenshot.
  - size: M
- [ ] M165.P3.T4 — RawColorCalibration patch grid and ΔE overlay (Q3)
  - files: Engine/Nodes/Raw/RawColorCalibrationChecker*.cpp
  - approach: draw-list primitives (P1).
  - verify: Xvfb screenshot.
  - size: M
- [ ] M165.P3.T5 — Parity audit and GUI script
  - files: the M80 parity-audit data, Tests/gui/m165_raw_aids.py
  - approach: DEFERRED rows become mapped.
  - verify: parity-audit.py exits 0; remote GUI job passes.
  - size: M

**Verification gate:** full build + ctest incl. the no-render-impact test; both GUI checks pass; no DEFERRED overlay rows; screenshots shared; AppImage for UAT.

## Open questions (for the user)
- Q1 (blocks phases 2–3) How does a node publish an overlay image? (a) overlay-only planes rendered on request through the normal render/cache path (recommended: caching, RoI, proxy and GPU come free) or (b) a side buffer filled during the viewer render (cheaper, a side channel, no caching)?
- Q2 Native overlays as a retained engine-side draw list (recommended; keeps Engine free of GL) or nodes issuing GL in drawOverlay like Tracker/RotoPaint?
- Q3 RawColorCalibration's checker grid and ΔE overlay in scope? Recommendation: yes.
- Q4 M216's line display: a follow-up stub (recommended) or a task here waiting for M216's merge?
- Q5 When does an overlay show? Recommendation: while the node's panel is open and its view knob is on.

## Decisions

- 2026-10-28: elaborated (consultant); Phase 1 can start now; phases 2–3 wait on Q1.
