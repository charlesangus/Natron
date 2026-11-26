# M459 - ColorWarp 3D View

Third member of F28 - Colour Warp. A viewer mode that swaps the image for a 3D view of colour space: the image's pixels as a point cloud, ColorWarp's pins as source→target arrows, and pin targets draggable in 3D. Camera navigation reuses M228 - 3D Viewer Navigation's presets (`Gui/SceneViewport.{h,cpp}`, the preset choice in `Engine/Settings`), but this is a plain OpenGL view inside the 2D viewer (`Gui/ViewerGL.cpp`, `Gui/ViewerTab*.cpp`), not a Hydra viewport.

## Scope

**In:**
- A viewer toolbar toggle, "Colour Space", available when the viewed node is (or the selected node is) a ColorWarp; it swaps the image for the 3D view and back.
- Point cloud: subsampled pixels of ColorWarp's input or output (a toggle), drawn in their own display colours, placed in the node's warp space, with labelled axes and a neutral axis.
- Pins drawn as source→target arrows; the selected pin's falloff as a translucent sphere; selection synced with the panel list and the image overlay.
- Dragging a pin's target end in 3D (screen-plane drag, axis handles to constrain), keyed like any knob edit, one undo step per drag.

**Out:**
- Adding pins from the 3D view — none (user: drag targets only).
- A dockable pane or in-panel widget — none (user chose viewer mode).
- A warp lattice display — none.
- Showing input and output clouds at once — none.

**Edge cases:**
- Large images: the cloud is a deterministic subsample (fixed point budget, stratified over the image) of a proxy-scale render, so it is stable between redraws.
- HDR values: the cloud and axes cover the data's actual extent in the warp space; nothing is clamped.
- The viewed node isn't a ColorWarp: the toggle is disabled; turning it off restores the image view with its previous pan/zoom.
- Pins edited elsewhere (panel, image overlay, global mode) update the 3D view live.

**Assumptions:**
- Unconfirmed: the axes are the warp space M457 - ColorWarp Space Spike's decision picks; if that space is XYZ with HSV/RGB-presented shifts, the view offers an HSV-cylinder or RGB-cube frame for orientation.

## Phase 459.1: Viewer mode

- [x] M459.P1.T1 — Colour Space viewer mode with 3D camera
  - files: `Gui/ViewerGL.cpp`, `Gui/ViewerGL.h`, `Gui/ViewerGLPrivate.cpp`, `Gui/ViewerTab.cpp`, `Gui/ViewerTab10.cpp`, `Gui/ColorSpaceView.{h,cpp}` (new), `Gui/CMakeLists.txt`
  - approach: a toolbar toggle that, when on, makes `ViewerGL` delegate paint and mouse/wheel events to a new `ColorSpaceView` drawing in the same GL context with its own orbit camera, using the navigation preset tables M228 - 3D Viewer Navigation added; restore the 2D view state when off. Draw axes and the neutral axis.
  - verify: syntax-check; P3.T1's script.
  - size: L

- [x] M459.P1.T2 — Point cloud of input or output
  - files: `Gui/ColorSpaceView.{h,cpp}`, `Engine/Nodes/Color/ColorWarp/ColorWarp.{h,cpp}` (an accessor for the field's space conversion)
  - approach: request the ColorWarp's input or output at proxy scale for the current frame through the normal render path, take a stratified fixed-budget subsample, convert to the warp space with M457's `ColorWarpSpace`, and upload as a GL point buffer coloured by display value; re-fetch on frame change, pin change or toggle.
  - verify: syntax-check; P3.T1's script checks point count and a known chart patch's position.
  - size: M

## Phase 459.2: Pins in 3D

- [x] M459.P2.T1 — Arrows, falloff sphere and selection
  - files: `Gui/ColorSpaceView.{h,cpp}`, `Gui/ColorWarpPanel.cpp`
  - approach: draw each enabled pin as an arrow from source to target in the warp space (hold pins as a marker), the selected pin's falloff as a translucent sphere; clicking a pin selects it and syncs with the panel list and image overlay.
  - verify: syntax-check; P3.T1's script.
  - size: M

- [x] M459.P2.T2 — Drag targets in 3D
  - files: `Gui/ColorSpaceView.{h,cpp}`
  - approach: dragging a target end moves it in the camera's screen plane; three axis handles on the selected target constrain to one warp-space axis; the move converts back to working-space RGB and sets `targetColor` as a single undoable edit, keyed per the knob's autokey rules.
  - verify: P3.T1's script drags a target and reads the knob back.
  - size: M

## Phase 459.3: GUI check

- [x] M459.P3.T1 — GUI script for the 3D mode
  - files: `Tests/gui/m459_colorwarp_3d_view.py` (new)
  - approach: a ColorWarp on a generated chart with two pins; turn the mode on, check the cloud and arrows are drawn (via a read-only accessor on `ColorSpaceView`), orbit, toggle input/output, select a pin and check the panel follows, drag its target along one axis and read the knob, undo; turn the mode off and check the 2D view state is restored; screenshot each state.
  - verify: the script passes in the remote gate.
  - size: M

**Verification gate:** build and ctest green; the GUI script passes under the remote gate; GUI bench shows no regression; member gate only — screenshots and UAT are collected at F28 - Colour Warp's UAT.

## Decisions

- 2026-11-16 — Viewer mode, not a dockable pane or in-panel widget (user).
- 2026-11-16 — Drag existing targets only; pins are added in the image viewer (user).
- 2026-11-16 — One cloud, input or output by toggle (user).
