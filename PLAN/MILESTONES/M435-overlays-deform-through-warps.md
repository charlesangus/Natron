# M435 - Overlays Deform Through Warps

Overlays follow the image through every warp between the overlay node and the viewed node. A Roto drawn upstream of a LensDistortion, SplineWarp, STMap or IDistort is drawn bent exactly as the image is, and dragging its points works through the warp. It brings together:
- M432 - Overlays Off The Render Path's path resolver, which finds the legs;
- M433 - Overlay Point Maps' chain, which maps the points;
- M434 - Interacts On The Overlay Draw List's deformable primitives, which give the geometry to map.

OFX overlays, which draw raw GL, get a fallback: they are rendered to an offscreen texture and that texture is warped.

## Scope

**In:**
- The resolver from M432 - Overlays Off The Render Path returns an `OverlayMapChain` in place of a single matrix whenever any leg's `overlayMapKind()` is non-linear. The matrix path stays as the fast path when every leg is a matrix.
- Draw-list overlays: every `deformable` primitive is mapped through `mapPolyline` at the viewport's pixel tolerance. Handles, points, text and anchored shapes are mapped at their anchor only, so they stay screen-sized. An anchored shape's direction is mapped by the local Jacobian.
- Pen and key events: the viewport pen position is inverse-mapped through the chain into the node's space. Hit tolerances, which the interacts express as canonical units per screen pixel, use the chain's local Jacobian at the pen, so a handle in a compressed region is as easy to grab as anywhere else.
- `ePending` legs: the overlay draws through the remaining legs, treating the pending ones as identity, with a viewer cue ("overlay mapping pending"). It redraws when `overlayMapChanged` fires.
- OFX overlay fallback: when the chain is non-linear, render the OFX overlay into an offscreen RGBA texture in the node's canonical frame, matrix-mapped, at viewport resolution. Draw that texture through the chain as a tessellated grid mesh, with grid vertices mapped forward. Pen events inverse-map the same way.
- An `eUnmappable` region: vertices there are dropped, and the polyline breaks.

**Out:**
- Maps through 3D projection and render nodes: M436 - Overlays On Projected Geometry.
- Render-side concatenation: M153 - Distortion Concatenation.
- Interacts not ported in M434 - Interacts On The Overlay Draw List: none remain native. Third-party OFX overlays use the texture fallback.

**Edge cases:**
- A warp that folds (a SplineWarp overlap): the drawn curve may cross itself, as the image does. A pen preimage is chosen nearest the handle's last known position.
- A drag that crosses into an unmappable region: the last mappable position holds and the drag continues once the pen is back in the domain.
- A heavy chain (several non-linear warps, thousands of Roto points): mapping runs at most once per redraw per overlay and is cached by (chain hash, time, zoom bucket). If it exceeds the frame budget it degrades to coarser tolerance, never to a stall.
- A viewer zoom change: re-tessellates at the new tolerance.
- An OFX overlay with a non-linear chain at extreme zoom: the texture size is capped at viewport size, so it never exceeds it.

**Assumptions:**
- Unconfirmed: a 4 px viewport tolerance for polyline subdivision looks smooth. P2.T3's screenshots settle it with the user.

## Phase 435.1: Resolver and drawing

- [ ] M435.P1.T1 — Resolver returns map chains
  - files: `Engine/OverlayPath.{h,cpp}` (from M432), `Engine/OverlayPointMap.{h,cpp}` (from M433), `Tests/OverlayPath_Test.cpp`
  - approach:
    - Extend `resolveOverlayPath` to record each leg as (node, inputNb, direction).
    - Build an `OverlayMapChain` when any leg is non-linear. Otherwise keep the composed matrix.
    - Upstream and sibling inverse legs use the inverse direction.
    - Keep time mapping per leg as before.
  - verify: gtest — Read → LensDistortion → Roto viewed at Read gives a chain whose forward point matches LensDistortion's inverse; an all-matrix path still yields a matrix and no chain.
  - size: M

- [ ] M435.P1.T2 — Deform draw-list primitives
  - files: the viewport draw-list drawer (`Gui/ViewerGL*.cpp`, `drawOverlayDrawList`), `Gui/ViewerTab20.cpp` (`drawOverlays`), `Engine/OverlayDrawList.{h,cpp}` (mapped-copy helper)
  - approach:
    - With a chain, produce a mapped copy of the list: deformable geometry goes through `mapPolyline` at 4 px tolerance (viewport scale from the GL projection), and anchors go through `mapPoint`.
    - Draw it with an identity modelview.
    - Cache by (list version, chain hash, time, zoom bucket).
    - Show a small "mapping pending" viewer cue while any leg is pending, and redraw on `overlayMapChanged`.
  - verify: syntax-check; under Xvfb, a Roto square upstream of a LensDistortion (k1 = 0.1) draws with bowed edges matching the rendered matte (screenshot shared with the user).
  - size: M

- [ ] M435.P1.T3 — Pen and key events through the chain
  - files: `Gui/ViewerTab20.cpp` (pen and key handlers), `Engine/EffectInstance.cpp` (`onOverlayDrawListPen*` pixel-scale input), `Engine/RotoCurveInteract.cpp`, `TrackerNodeInteract.cpp`, `SplineWarpInteract.cpp`, `Gui/HostOverlay.cpp` (wherever pixel scale for tolerance is read)
  - approach:
    - Inverse-map the pen position through the chain, seeded from the handle under the pen or the last pen preimage.
    - Compute the local Jacobian by finite difference over ±1 px and pass the per-axis canonical-per-pixel scale the interacts already take, in place of the global zoom scale.
    - When the pen is unmappable, hold the last mappable position.
  - verify: syntax-check; under Xvfb, dragging a Roto corner viewed through the LensDistortion keeps the point under the cursor (two screenshots, before and after the drag).
  - size: M

## Phase 435.2: OFX fallback

- [ ] M435.P2.T1 — Texture-warp fallback for OFX overlays
  - files: `Gui/ViewerTab20.cpp`, `Gui/ViewerGL*.cpp` (an FBO render of the OFX overlay and a warped-grid draw), `Engine/OfxOverlayInteract.cpp` (read-only unless the viewport hook needs it)
  - approach:
    - When an OFX overlay's chain is non-linear, bind an FBO the size of the viewport and draw the overlay with the matrix part of the chain. Then draw the FBO texture on a 32×32 grid (refined where `mapPolyline` would subdivide) whose vertices are forward-mapped.
    - Pen events inverse-map as in P1.T3.
    - Add a one-time viewer note that this overlay is approximated.
  - verify: under Xvfb, an OFX overlay node from the bundled plug-ins (any OFX plug-in with a handle overlay, found in `Tests`' bundled list) behind a LensDistortion draws its handle within 2 px of where the render moves the handle's point (screenshot shared with the user).
  - size: L

## Phase 435.3: Tests

- [ ] M435.P3.T1 — GUI test: overlays through warps
  - files: `Tests/gui/m435_overlays_through_warps.py`
  - approach:
    - Read → Roto (one square Bezier) → LensDistortion (k1 = 0.1) → viewer, Roto panel open.
    - Assert the drawn Roto edge midpoint sits where the render's edge does: render the Roto matte through the LensDistortion and compare edge positions within 1 px.
    - Drag a corner by a viewer offset and assert the Roto knob moved by the inverse-mapped amount.
    - Repeat with SplineWarp and with an STMap (a generated shift map), waiting for the pending cue to clear.
    - Repeat with an OFX overlay per P2.T1.
    - Share screenshots of each with the user.
  - verify: passes under `build/m63-gui/run-gl-tests.sh` or the remote `--gui` gate.
  - size: M

- [ ] M435.P3.T2 — Docs
  - files: the viewer overlay docs page edited in M432.P3.T2
  - approach: describe overlays bending through warps, the pending cue, and the OFX approximation note. No Nuke mentions.
  - verify: the docs build.
  - size: S

**Verification gate:** `OverlayPath_Test`, `OverlayPointMap_Test` and `m435_overlays_through_warps.py` pass on a remote release build with `--gui`. The GUI bench shows no regression, and the result is recorded in `## Decisions`. Screenshots go to the user for approval. The UAT happens at F25 - Overlay Flow's landing.

## Decisions

- 2026-11-14 — Native interacts deform per vertex through the draw list, and OFX overlays fall back to a warped texture (user, grill: "Both"): crisp, exactly hit-tested handles where we own the code, and coverage for third-party overlays.
