# M229 - 3D Viewer Guides And Display

Draw what Storm doesn't. Cameras get frustums and lights get icons in the Viewer's 3D view,
and both can be picked. The view also gains display controls: purpose toggles, scene lights
vs headlight, and draw-mode shortcuts. From the M19/M20 UAT feedback (2026-10-26). The code
survey found that Storm draws no camera or light guides, and that `showGuides` would not
add them (usdview draws its own). Our overlay pass sits next to `drawGrid` in
`Gui/SceneViewport.cpp`. Survey: `PLAN/DESIGN/2026-10-26-3d-mvp-survey.md`.

## Scope

**In:**
- Camera guides: a frustum from the camera's aperture, focal length and clipping, drawn to a fixed display length with the film-back rectangle. Light guides per UsdLux type: distant arrow, sphere, rect outline, dome icon at origin. Both are drawn as GL overlays at the composed world transform at the viewer's frame.
- Guides are pickable (they join the selection set from M228 - 3D Viewer Navigation) and highlight when selected. A guides toggle sits in the view menu.
- Display toggles for the purposes proxy, render and guide. A lighting toggle switches between scene lights and headlight (default: headlight when the scene has no lights, otherwise scene lights). Draw-mode and grid toggles get rebindable shortcuts in the 3DViewer group.

**Out:**
- Transform widgets on cameras and lights — M230 - 3D Transform Widgets.
- Light shadows and quality controls in Storm — none.
- Guides in `USDRender` output — none (viewer only).

**Edge cases:**
- An orthographic camera: the frustum is a box.
- A camera looked through: its own guide is hidden.
- Hundreds of cameras or lights (a USD layout file): guides are drawn as one batched line pass, and the toggle hides them all.
- An animated camera: its guide follows the frame.
- Picking a guide versus a prim behind it: the nearer one wins, with guide hits tested in screen space within a few pixels.

## Phase 229.1: Guides

- [x] M229.P1.T1 — Camera and light guide geometry
  - files: `Gui/SceneGuides.{h,cpp}` (new), `Gui/CMakeLists.txt`, `Tests/Scene/SceneGuides_Test.cpp`
  - approach: pure functions that build line lists (world-space segments) for a `UsdGeomCamera` (via `GfCamera`, frustum to a display length) and for each UsdLux type, from a composed stage at a time. Also a collector that walks the stage once (through the viewer's `SceneStage`) and returns (path, kind, world lines). No GL in this unit.
  - verify: gtest — a 50 mm 36×24 camera's frustum corners match `GfCamera::GetFrustum` at the display length; an orthographic camera gives a box; each light type yields non-empty lines at its world position.
  - size: M

- [x] M229.P1.T2 — Draw and pick guides in the 3D view
  - files: `Gui/SceneViewport.{h,cpp}`, `Tests/gui/scene_viewer_guides.py`
  - approach: after Storm, and beside `drawGrid`, draw the M229.P1.T1 lines in one immediate-mode/VBO pass with depth test on, colour per kind, and selected guides in the highlight colour. Recollect only when the stage syncs or the frame changes. Picking tests guide segments in screen space (within ~4 px) before the Storm pick and resolves nearest-first. Add a guides toggle to the view menu.
  - verify: Xvfb GUI script — a scene with a USDCamera and a USDLight shows guide pixels at their projected positions (pixel test); clicking the frustum selects the camera path; toggling guides off removes the pixels.
  - size: M

## Phase 229.2: Display controls

- [x] M229.P2.T1 — Purpose, lighting and draw-mode controls
  - files: `Gui/SceneViewport.{h,cpp}`, `Gui/ActionShortcuts.cpp` (3DViewer group entries), `Tests/gui/scene_viewer_display.py`
  - approach: `renderParams()` sets `showProxy`/`showRender`/`showGuides` from view-menu toggles (defaults proxy on, render off, guide off). The lighting mode chooses between scene lights and the headlight. Auto means headlight only when the composed stage has no UsdLux prims, done by disabling the headlight or passing an empty light set through `SetLightingState`. Draw modes (unshaded, shaded, wireframe), grid and guides get 3DViewer bindings (defaults 4/5/6-style, matching the Nuke preset, recorded in the preset tables).
  - verify: Xvfb GUI script — a scene with one red rect light renders reddish with lighting=Scene and neutral with Headlight; toggling the render purpose shows a render-purpose prim; the draw-mode shortcuts switch modes.
  - size: M

**Verification gate:** release gate green; guide and display GUI scripts pass under Xvfb; screenshots of camera and light guides (selected and not) shared before sign-off.

## Decisions

- 2026-10-27 — Display keys are Ctrl+4/5/6 (wireframe/shaded/unshaded), Ctrl+G grid, Ctrl+Shift+G guides: plain 4/5/6 already connect viewer inputs. Guides are 1 scene unit long. Show Grid moved into a Display submenu.
