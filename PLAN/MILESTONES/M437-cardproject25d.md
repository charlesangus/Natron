# M437 - CardProject25D

A quick 2.5D projection node, in the Transform menu, that does more than a Card3D.
- It projects the **Source** image from a camera onto a card and renders the card through the **Render** camera.
- With a **Projection** camera connected, the plate projects from that camera.
- Without one, it projects from the Render camera held at the projection frame. That is the usual "freeze a frame, then re-render it through the moving camera" patch.
- Optional **Undistort** and **Redistort** map inputs (STMap or IDistort) take the lens distortion out of the plate before projection and put it back into the result.
- Overlay handles from upstream nodes flow through it, and it has a 3D card gizmo of its own.

Without lens maps, plate → card → render camera is an exact homography. The node is analytic, with no USD render, so it is fast on CPU and GPU, and it concatenates with upstream and downstream Transforms.

Scouted 2026-11-14 on `origin/main`:
- Cameras reach image nodes as scene inputs with a camera-path dropdown, as in `USDProject3D` (`_camera`, `HydraRender::cameraPaths`, fit mode at `Engine/Nodes/Scene/USDProject3D.cpp:204`) and `USDRender`.
- Homography maths and the solver are in `Engine/Nodes/Transform/CornerPin.{h,cpp}` (`CornerPin.h:125–138`) and `TransformMath.h`.
- The sampler, filters and the GPU warp scaffolding are from M113 - Native Warp Nodes (`WarpGpu.h`, `Engine/Gpu/Kernels/stmap.slang`).
- Transform's motion-blur knobs and sampling are in `Engine/Nodes/Transform/Transform.cpp`.

## Scope

**In:**
- Inputs:
  - **Source**: the image to project.
  - **Render**: a scene, with a Render Camera path dropdown.
  - **Projection**: an optional scene, with a Projection Camera path dropdown. It defaults to the Render input's stage when unconnected.
  - **Undistort** and **Redistort**: optional images, each with its own mode knob (STMap / IDistort) and the STMap/IDistort conventions of the native nodes (UV channels, IDistort scale).
- Card knobs, in world space:
  - Translate, Rotate, Scale, Uniform Scale, with Transform Order and Rotation Order;
  - Card Width/Height in world units.
- A button, **Place In Front Of Camera**, which sets the card to fill the projecting camera's frustum at a Distance knob.
- Frame hold, one set of knobs that controls both modes:
  - **Frame Hold** on/off: off makes the projecting camera follow the current frame;
  - **Projection Frame**;
  - **Hold Source** on/off: holds the Source image at the projection frame too.
- Lens maps:
  - Undistort is applied to the Source before projection, and Redistort to the result after.
  - With only one map connected, the other direction is its numerical inverse, using M433 - Overlay Point Maps' inverter at map resolution.
  - With neither connected, there is no lens handling.
- Output: Format (default the Source's format), Fit Mode for both cameras (as `USDProject3D`'s), Outside mode (black / transparent), Filter (M113 - Native Warp Nodes' sampler filters), Back-Face Cull on/off.
- Motion blur: Motion Blur samples, Shutter, Shutter Offset and Custom Offset, as Transform has, sampling the homography across the shutter. Off by default.
- `getTransform` returns the homography when no lens map is connected and motion blur is off, so Transforms concatenate through it.
- Overlay flow: `mapOverlayPoint` composes the inverse Undistort map, the homography and the Redistort map, with STMap/IDistort legs fetched through M433 - Overlay Point Maps' async fetch. Upstream Roto, Tracker and similar overlays deform through the node.
- An own overlay in the 2D viewer:
  - the card's outline, projected through the render camera;
  - a 3D axis gizmo at the card centre, projected, with drag-to-translate along each axis and Ctrl-drag to rotate;
  - a corner scale handle.
  It uses M434 - Interacts On The Overlay Draw List's anchored shapes.
- CPU and GPU kernels with identical results within the M113 - Native Warp Nodes parity tolerance.
- Node docs page and tooltips.

**Out:**
- Projection onto arbitrary scene geometry: `USDProject3D` + `USDRender` cover it.
- Several cards: none.
- Lens distortion carried in the USD scene with the camera: M438 - Lens Distortion In The USD Scene explores it. CardProject25D gains that source only if the spike's design is adopted.
- Showing the card in the Viewer's 3D view: none in v1.
- Occlusion or depth output: none.

**Edge cases:**
- Card behind the projection or render camera, or seen edge-on: those output pixels are outside (homography w ≤ 0 is rejected), with no wrap-around.
- Back-Face Cull on and the card facing away from the render camera: the output is all outside.
- A camera path that doesn't resolve: node error naming the path, as `USDRender` does.
- Projection input unconnected and Frame Hold off: projection and render camera are the same at every frame, so the result is the plate re-rendered through itself. That is valid: identity when the card fills the frustum.
- Only Redistort connected: Undistort is its inverse, and the same for only Undistort. If the inversion fails at a pixel (a fold), that pixel is outside.
- A projection camera with a different lens from the render camera: the two-map setup handles it. The tooltip says single-map inversion assumes one lens.
- Source format differs from the projection camera's aperture: Fit Mode decides, as in `USDProject3D`.
- Animated cameras with motion blur: each shutter sample evaluates the render camera at the sample time. The projection camera follows the sample time only when Frame Hold is off.

**Assumptions:**
- Unconfirmed: Frame Hold and Hold Source default on, and Projection Frame defaults to the current frame when the node is created.
- Unconfirmed: the card's default placement on creation is in front of the projecting camera at Distance 10, filling its frustum.

## Phase 437.1: Core node

- [ ] M437.P1.T1 — Node skeleton, inputs and knobs
  - files: new `Engine/Nodes/Transform/CardProject25D.{h,cpp,reg.inc}`, `Engine/AppManager.cpp` (registration), CMake source list, `Tests/CardProject25D_Test.cpp`
  - approach:
    - Native effect with the five inputs and all knobs listed in `## Scope` In. Knob labels follow the script names.
    - Camera path dropdowns are populated from each scene input's stage, as `USDProject3D` does.
    - Place In Front Of Camera is a button callback setting the card knobs.
    - RoD = output format. RoI on Source = the bounding box of the four output corners back-projected (all of Source when a map or the projection is degenerate).
  - verify: gtest — the node instantiates with defaults; the knob script names and defaults match the `## Scope` list; an unresolved camera path gives the error naming it.
  - size: M

- [ ] M437.P1.T2 — Projection homography maths
  - files: new `Engine/Nodes/Transform/CardProjectMath.{h,cpp}`, `Tests/CardProject25D_Test.cpp`
  - approach:
    - From both cameras' `GfCamera` at their times (fit mode applied) and the card's world matrix, build H_proj (Source canonical → card-plane coordinates by ray–plane intersection) and H_render (card plane → render canonical).
    - Get each by mapping the four corners analytically and solving with CornerPin's homography solver.
    - The output is H = H_render ∘ H_proj⁻¹, plus validity: the sign of w per point, and back-face cull.
  - verify: gtest — identical cameras and a card filling the frustum give H = identity within 1e-9; a 10° render-camera pan gives the analytic rotation homography; a card behind the camera is flagged invalid.
  - size: M

- [ ] M437.P1.T3 — CPU render with lens maps and frame hold
  - files: `Engine/Nodes/Transform/CardProject25D.cpp`, `Tests/CardProject25D_Test.cpp`
  - approach:
    - Per output pixel: Redistort-map inverse lookup → H⁻¹ → Undistort-map lookup → sample Source with the chosen filter.
    - When only one map is connected, invert it numerically once per render at map resolution into a cached field, using M433's `invertNumerically`.
    - Frame Hold and Hold Source set the time used for the projection camera and the Source fetch.
    - Outside mode applies to rejected or failed pixels.
    - Implement `getTransform` returning H when no map is connected and motion blur is off.
  - verify: gtest — a Grid source with identical cameras renders identical to the source (≤1e-4); with an STMap pair that cancels, identical; one map plus its numerical inverse matches the two-map result within 0.5 px; Hold Source with an animated source uses the held frame.
  - size: L

- [ ] M437.P1.T4 — Motion blur
  - files: `Engine/Nodes/Transform/CardProject25D.cpp`, `Tests/CardProject25D_Test.cpp`
  - approach: Transform's knob set and shutter sampling. Per sample, build H at the sample time and accumulate. Bypass `getTransform` concatenation when motion blur is on.
  - verify: gtest — a static setup with motion blur on equals off; a panning render camera with 0.5 shutter blurs a vertical edge to the analytic width within 1 px.
  - size: M

## Phase 437.2: GPU and overlays

- [ ] M437.P2.T1 — GPU kernel
  - files: new `Engine/Gpu/Kernels/cardproject.slang`, `Engine/Nodes/Transform/CardProject25D.cpp` (GPU path through `WarpGpu.h`), the GPU kernel registration list, `Tests/CardProject25D_Test.cpp` (parity)
  - approach: the same per-pixel chain, with H as a uniform and maps as textures, using M113's GPU sampler filters. A numerically inverted map is uploaded as a field texture.
  - verify: the GPU parity gtest against CPU within the M113 - Native Warp Nodes tolerance; it runs in the published image per the GL/GPU test rules.
  - size: L

- [ ] M437.P2.T2 — Overlay point map
  - files: `Engine/Nodes/Transform/CardProject25D.{h,cpp}`, `Tests/OverlayPointMap_Test.cpp`
  - approach:
    - `overlayMapKind()` is matrix with no maps and no motion blur, else non-linear.
    - Forward: Undistort inverse → H → Redistort forward, through `OverlayMapFetch` for the map legs. Inverse is the reverse.
    - Invalid points are `eUnmappable`.
  - verify: gtest — a Roto point upstream maps to where the render puts a one-pixel dot, within 0.5 px, with and without an STMap pair.
  - size: M

- [ ] M437.P2.T3 — Card gizmo
  - files: `Engine/Nodes/Transform/CardProject25D.{h,cpp}` (`buildOverlay` and draw-list pen handlers)
  - approach:
    - Draw the card outline as the projected card corners (deformable polyline), and the axis gizmo at the projected card centre as anchored shapes with the projected axis directions (X red, Y green, Z blue).
    - Drag an axis translates along it, solving the pen's screen delta against the projected axis.
    - Ctrl-drag rotates about that axis.
    - A corner handle sets Uniform Scale.
    - Undo groups per drag.
  - verify: syntax-check; under Xvfb, the gizmo draws on the projected card and dragging its X axis changes Translate X (screenshots shared with the user).
  - size: M

## Phase 437.3: Tests and docs

- [ ] M437.P3.T1 — GUI test and docs
  - files: `Tests/gui/m437_cardproject25d.py`, `Engine/Nodes/Transform/docs/CardProject25D.md`
  - approach:
    - Build USDCamera (animated pan) → CardProject25D Render, plus a Grid Source and an upstream Roto.
    - Assert the Roto handle sits on the projected position.
    - Drag the gizmo's X axis and assert Translate X changed.
    - Toggle Frame Hold and check the output changes.
    - Screenshots go to the user.
    - The docs cover inputs, the frame-hold modes, lens maps (two-map and one-map inversion, single-lens assumption) and motion blur. No Nuke mentions.
  - verify: passes under `build/m63-gui/run-gl-tests.sh` or the remote `--gui` gate; the docs build.
  - size: M

**Verification gate:** `CardProject25D_Test` (CPU, GPU parity, motion blur) and `m437_cardproject25d.py` pass, and the full ctest suite is green on a remote release build with `--gui`. Screenshots go to the user. The UAT happens at F25 - Overlay Flow's landing, with a projection patch on a panning camera, a lens-map pair, and a Roto flowing through.

## Decisions

- 2026-11-14 — Named `CardProject25D` (user, grill).
- 2026-11-14 — One built-in card, analytic homography, no USD render (user, grill): fast, GPU-friendly, concatenates.
- 2026-11-14 — Two lens-map inputs (Undistort, Redistort), and one connected map inverts for the other direction (user, grill).
- 2026-11-14 — One Frame Hold knob set controls both modes: Frame Hold on/off, Projection Frame, Hold Source on/off (user, grill).
- 2026-11-14 — Motion blur by shutter samples, off by default (user, grill).
- 2026-11-14 — Member of F25 - Overlay Flow, so it builds on M433 - Overlay Point Maps without waiting for the Feature to land (user).
