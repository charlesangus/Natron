# M494 - Delegate Subdivision

Render USD subdivision surfaces in Cycles and MoonRay the way Karma and Gaffer do: a mesh's
`subdivisionScheme` decides whether it is a subdivision surface, and screen-space dicing
settings decide how finely it is tessellated. A per-renderer `Subdivision Surfaces` toggle,
off by default, gates it, so USD's `catmullClark` fallback doesn't silently subdivide every
mesh. Both delegates are built with OpenSubdiv today, but each subdivides only when Hydra's
display `refineLevel` is at least 1, and Natron never raises it, so nothing is ever
subdivided (cycles fork `src/hydra/mesh.cpp:411-418`; hdMoonray `lib/hydramoonray/Mesh.cc:188-240`).
The fix lives in our delegate forks (charlesangus/cycles, charlesangus/hdMoonray on
`natron/v2026.29.1`), with the image pins in `tools/ci/local/Dockerfile` bumped to match.

## Scope

**In:**
- Cycles and MoonRay: with `Subdivision Surfaces` on, every mesh whose scheme is not `none` renders as a subdivision surface, whatever Hydra's `refineLevel` says. Off (the default) renders the cage, exactly as today.
- Cycles dicing controls: `Dicing Rate` (pixels per micropolygon edge, Cycles' own default), `Max Subdivision Level` (cap), `Adaptive Space` (pixel or object) if the pinned Cycles has the socket, `Dicing Camera` (camera prim path; empty means the render camera). Per-prim constant primvars `cycles:dicing_rate` (a multiplier on the global rate, as in Gaffer) and `cycles:max_level` override per mesh.
- MoonRay dicing controls: `Mesh Resolution` and `Adaptive Error` as global settings, which hdMoonray's existing per-prim primvars `moonray:mesh_resolution` and `moonray:adaptive_error` override per mesh; `Dicing Camera` through MoonRay's `dicing_camera` scene variable.
- MoonRay's `Force Polygon` knob is replaced by `Subdivision Surfaces`, which drives `forcePolygon` inverted.
- Loop-scheme meshes render as Catmull-Clark in both renderers, with a warning that names the prim.
- Toggling or changing a subdivision setting re-syncs the affected meshes in a running render and in the viewer.

**Out:**
- A per-mesh opt-in or opt-out primvar over the toggle — none (user, 2026-11-22: toggle only; per-mesh control is the authored scheme).
- The Utility renderer moving to this model — M289 - Utility Adaptive Subdivision.
- Displacement in Cycles or MoonRay — none.
- A triangle or memory budget — none.
- Hydra's `refineLevel` (display complexity) as a control for these renderers — none; it no longer gates or sets anything in them.

**Edge cases:**
- Scheme `none`: never subdivided, toggle on or off.
- Bilinear scheme with the toggle on: Cycles linear subdivision, MoonRay bilinear; `CyclesAovs.cpp`'s shading check (line ~206) follows the toggle, not `refineLevel`.
- Authored normals on a subdivided mesh: ignored, as USD and Karma specify; limit normals shade it.
- Creases, corners, holes, boundary and face-varying rules: passed through as both delegates already do.
- Loop scheme: Catmull-Clark plus a warning naming the prim, through each renderer's existing warning path (MoonRay: `MoonRayAovs::warningText`).
- Toggle off: output is identical to today's, so no existing golden or parity reference changes.

**Assumptions:**
- Unconfirmed: dicing settings default to each renderer's own defaults (Cycles dicing rate 1 px and max level 12; MoonRay mesh resolution 2 and adaptive error 0).
- Cycles' dicing camera can be set from the Hydra delegate; if the pinned Cycles only dices from the render camera, `Dicing Camera` is dropped for Cycles and the PM records it.

## Phase 494.1: Cycles

- [ ] M494.P1.T1 — Cycles fork: subdivide by scheme behind a Subdivision Surfaces setting
  - files: charlesangus/cycles `natron/...` branch `src/hydra/mesh.cpp`, `src/hydra/render_delegate.{h,cpp}`; `tools/ci/local/Dockerfile` (`NATRON_CYCLES_COMMIT`, `NATRON_CYCLES_SHA256`); `Tests/Scene/USDRenderCycles_Test.cpp`
  - approach: add a boolean render setting (`natron:subdivisionSurfaces`, default false) to HdCyclesDelegate. In `HdCyclesMesh`, replace the `GetRefineLevel() > 0` gate: Catmull-Clark and Loop → `SUBDIVISION_CATMULL_CLARK` (Loop also logs a warning naming the prim through the delegate's existing TF_WARN/notice path), bilinear → `SUBDIVISION_LINEAR`, only when the setting is on. `subd_max_level` stops reading `refineLevel` and takes Cycles' default for now (T2 adds the control). A change to the setting marks every mesh rprim's topology dirty, the way other scene-affecting settings are handled in `render_delegate.cpp`. Push the fork commit, then bump the Dockerfile pin and checksum.
  - verify: `build-cycles.sh` builds the pinned commit in natron-dev. A gtest renders a Catmull-Clark cube with the setting set on the engine directly: on differs from off at the silhouette, off matches the current output, a scheme-`none` cube is unchanged either way, and a Loop mesh renders smooth with the warning captured.
  - size: L

- [ ] M494.P1.T2 — Cycles fork: dicing rate, max level, adaptive space, dicing camera and per-prim overrides
  - files: charlesangus/cycles `src/hydra/mesh.cpp`, `src/hydra/render_delegate.{h,cpp}`; `tools/ci/local/Dockerfile`; `Tests/Scene/USDRenderCycles_Test.cpp`
  - approach: render settings `natron:dicingRate`, `natron:maxSubdivisionLevel`, `natron:adaptiveSpace` (only if the pinned Cycles `Mesh` has an adaptive-space socket) and `natron:dicingCamera` (SdfPath; empty means the render camera → `scene->dicing_camera`). Per mesh: `subd_dicing_rate` = global rate × constant primvar `cycles:dicing_rate` (default 1), `subd_max_level` = primvar `cycles:max_level` if authored, else the global value. Changes dirty the meshes as in T1. Bump the pin.
  - verify: gtest — a subdivided sphere at dicing rate 0.5 and 8 renders differently, and the coarse one matches the cage-like faceting; a `cycles:max_level` of 1 on one of two meshes coarsens only that mesh; a dicing camera far from the render camera coarsens the result.
  - size: L

- [ ] M494.P1.T3 — Natron Cycles subdivision knobs, shading check and docs
  - files: `Engine/Nodes/Scene/CyclesSettings.cpp`, `Engine/Nodes/Scene/CyclesAovs.cpp`, `Engine/Nodes/Scene/docs/USDRender.md`, `Tests/gui/m253_render_settings_layout.py`, `Tests/Scene/USDRenderCycles_Test.cpp`
  - approach: register `subdivisionSurfaces` ("Subdivision Surfaces"), `dicingRate`, `maxSubdivisionLevel`, `adaptiveSpace` (menu, if T2 added it) and `dicingCamera` in a new "subdivision" group, mapped to T1/T2's setting keys, with tooltips that explain the scheme rule, that the toggle is off by default, and the per-prim primvars. In `CyclesAovs.cpp`, the bilinear shading branch tests the toggle instead of `refineLevel`, and the scene index's dirty locators follow. Document the subdivision controls in USDRender.md. Add the knobs to the layout script.
  - verify: gtest — USDRender's Subdivision Surfaces knob subdivides a Catmull-Clark cube and Dicing Rate changes it; the m253 layout script passes under Xvfb.
  - size: M

## Phase 494.2: MoonRay

- [ ] M494.P2.T1 — hdMoonray fork: subdivide by scheme, dicing settings, Loop warning
  - files: charlesangus/hdMoonray `natron/v2026.29.1` `lib/hydramoonray/Mesh.cc`, `lib/hydramoonray/RenderDelegate.{h,cc}`; the openmoonray super-repo gitlink; `tools/ci/local/Dockerfile` (`NATRON_MOONRAY_COMMIT` and checksum); `Tests/Scene/USDRenderMoonRay_Test.cpp`
  - approach: in `Mesh::syncSubdivScheme`, drop `refineLevel < 1` from `disableSubd`, so `forcePolygon`, flat shading and scheme `none` remain the only gates. `mesh_resolution` comes from a new render setting `natron:meshResolution` (MoonRay's default) unless the `moonray:mesh_resolution` primvar is authored, and `adaptive_error` from `natron:adaptiveError` unless `moonray:adaptive_error` is. A Loop mesh logs a warning starting with its prim path (MoonRayAovs surfaces it). Settings changes re-sync meshes the way `forcePolygon` does. Push the fork, update the super-repo gitlink, bump the Dockerfile pin.
  - verify: `build-moonray.sh` builds the pin in natron-dev. A gtest with `forcePolygon` false renders a Catmull-Clark cube rounder than with it true; a `moonray:mesh_resolution` primvar on one mesh overrides the global setting; a Loop mesh warns, naming the prim.
  - size: L

- [ ] M494.P2.T2 — Natron MoonRay Subdivision Surfaces knob replaces Force Polygon
  - files: `Engine/Nodes/Scene/MoonRaySettings.cpp`, `Engine/Nodes/Scene/MoonRayAovs.cpp` (warning text, if the Loop notice needs a mapping), `Engine/Nodes/Scene/docs/USDRender.md`, `Tests/Scene/USDRenderMoonRay_Test.cpp` (lines ~898 and ~932), `Tests/gui/m253_render_settings_layout.py`
  - approach: remove the `forcePolygon` declared setting and register `subdivisionSurfaces` ("Subdivision Surfaces", default off) that writes `forcePolygon` = !value, plus `meshResolution`, `adaptiveError` and `dicingCamera` (the `dicing_camera` scene variable, empty = render camera) in a "subdivision" group like Cycles'. No old-project mapping. Update the knob-list tests, the layout script and USDRender.md.
  - verify: gtest — the knob off renders the cage, on subdivides; the knob-list tests list the new names and not `moonrayForcePolygon`; the m253 layout script passes under Xvfb.
  - size: M

**Verification gate:** full build and ctest green on both scheduler legs, PR CI green, and a remote GUI gate (`--gui`) that includes `Tests/gui/m253_render_settings_layout.py`. Shared with the user before sign-off, one file per image: Cycles and MoonRay renders of a Catmull-Clark test mesh with Subdivision Surfaces off and on, at two dicing settings each, plus screenshots of both renderers' subdivision knob groups. A UAT AppImage with its steps .md.

## Decisions

- 2026-11-22 — **Scheme plus dicing, behind a default-off toggle (user):** follows Karma (scheme decides; per-prim Dicing Quality; scene Dicing Camera) and Gaffer (MeshType; `cycles:max_level`/`dicing_rate`/`adaptive_space` per object; `cycles:dicing_camera`), with an off switch so USD's `catmullClark` fallback doesn't subdivide unauthored meshes, which is M284 - Utility Renderer Subdivision's "level 0 unless specifically changed" call. Project-wide record: `PLAN/DECISIONS/2026-11-22-delegate-subdivision-model.md`.
- 2026-11-22 — **Toggle only, no per-mesh opt-in primvar (user).**
- 2026-11-22 — **One MoonRay toggle (user):** Subdivision Surfaces replaces Force Polygon and drives it inverted, so Cycles and MoonRay show the same control.
- 2026-11-22 — **Loop renders as Catmull-Clark with a warning in both renderers (user).**
- 2026-11-22 — **Fork patches, not a Natron scene-index filter:** the chosen model removes the delegates' `refineLevel` gate, which only the forks can do. Settings are read where hdMoonray already reads `forcePolygon`, and no render-instance side channel is needed.
- 2026-11-22 — **No triangle budget:** standing rule against size caps on user data.
- 2026-11-23 — PR #345 review: the Cycles Denoise group starts collapsed so the render-settings panel with the new Subdivision group fits 1920x1080 (layout gate fix 56fdb0203). Settings changes reach a running render by rebuilding the engine (HydraRenderImpl), not by the forks dirtying meshes; the tests cover that path. Fixing: MoonRay forcePolygon as a base setting, Cycles bilinear shading follows the toggle, MoonRay dicing-camera test, Dicing Camera enable conditions.
- 2026-11-23 — MoonRay dicing measures depth from the render camera; the dicing camera only sets field of view and which faces are visible (faces outside its view get factor 0). The test now uses a camera that misses the mesh instead of a far one, and the tooltip/docs say so. Cycles keeps distance-based dicing.
