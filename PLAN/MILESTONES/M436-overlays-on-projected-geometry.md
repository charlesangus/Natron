# M436 - Overlays On Projected Geometry

Carry a 2D node's overlay onto the geometry its image is projected or textured onto. Take a Roto feeding the plate of a `USDProject3D`, or an image bound through M233 - Live Node Textures to a UV-mapped mesh (`USDProjectUV` UVs included). Its shapes and handles are drawn on the surface in the Viewer's 3D view. They are also mapped into the 2D output of any `USDRender` downstream, and they are editable in both places. The mapping is computed host-side from the USD stage, so it is the same for every render delegate (Storm, Cycles, MoonRay, Nucleon Utility).

Scouted 2026-11-14 on `origin/main`:
- Projection lives in `Engine/Nodes/Scene/USDProject3D.cpp`, with its camera dropdown, fit mode (`:204`) and frame hold, and `SceneProjectionCameras.{h,cpp}`.
- UV projection is in `USDProjectUV.cpp`, and live textures came with M233 - Live Node Textures.
- The 3D view (`Gui/SceneViewport.{h,cpp}`) picks through Hydra (`pick`, `:520`) and draws transform handles (`drawTransformHandles`, `:605`+).
- There is no CPU ray/mesh intersection in the tree.

## Scope

**In:**
- A **surface map** leg type for M433 - Overlay Point Maps' chain. It maps a canonical point in a node's image to zero or more world-space surface points, and back.
  - Projection (`USDProject3D`): through the projection camera (fit mode and frame hold as the material uses them) as a ray, intersected with the matched prims. Near/far clip and back-face cull are respected as the material applies them.
  - UV textures (live texture on `st`): image point → uv. Find every mesh triangle whose `st` contains it, using a UV-space BVH and barycentrics, and give the world point.
- A **render map** leg for `USDRender`: world point → render camera → the render's 2D output (canonical, fit mode as the render uses it). The inverse is a camera ray against the scene.
- The resolver, from M432 - Overlays Off The Render Path and M435 - Overlays Deform Through Warps, finds paths that run from an image input of a scene node to a downstream `USDRender`, or to the 3D view.
- 3D view: overlays of open-panel 2D nodes whose image reaches a displayed projection or texture are drawn as world-space polylines and handles in `SceneViewport`. They are depth-tested against the scene: visible parts are solid, and occluded parts are faint.
- USDRender 2D view: the same overlay mapped through the render map. Occlusion is judged by a host ray test against the stage, and occluded parts are drawn faint.
- Editing in both views. The 3D pick ray, or the render's camera ray from a 2D pen, hits the surface and maps back through projection or UV to the node's image space.
- A CPU triangle BVH over the stage's matched meshes at a time, in world space and in UV space. It is built off the GUI thread and cached by stage revision, time and prim set.

**Out:**
- Native geometry (F15 - Native Geometry) textures and projections: a follow-up once F15 lands.
- Texture painting overlays (F11 - Texture Painting).
- CardProject25D's own map: M437 - CardProject25D, which is analytic.
- Deformation by other scene ops between the projection and the render, beyond what the stage at that time already contains: none needed, since the stage is evaluated at the time.
- Delegate-specific displacement: overlays sit on the authored surface, not the displaced one.

**Edge cases:**
- A projection ray that misses all geometry: the point is unmappable and the polyline breaks.
- A projection ray that hits several surfaces (projection goes through by default, as in M234 - Camera Projection Nodes): every hit is drawn. The nearest to the projection camera is solid when it is visible from the view. Pen picks choose the hit under the cursor.
- A curve crossing a UV seam or a UDIM boundary: it is subdivided in image space, and it breaks where neighbouring samples land on non-adjacent triangles (world distance jump above a threshold).
- Overlapping UVs (several triangles share a uv): every surface point is drawn.
- Animated geometry or cameras: the BVH and the cameras are evaluated at the overlay's mapped time (frame-held projection camera at its hold frame, geometry at the viewer's time).
- Heavy scenes (F4 - Heavy Scene Viewer sizes): the BVH builds only over prims matched by projection or texture bindings, never the whole stage. Before it is ready the overlay is `ePending`, with the same cue as M435 - Overlays Deform Through Warps.

**Assumptions:**
- Unconfirmed: a simple binned-SAH BVH written here is fast enough for the matched prims of production scenes. Reuse a BVH from E19 - Utility Renderer if one has landed on main by elaboration time.
- Unconfirmed: the live-texture binding records which node image feeds which prim and primvar, so the resolver can find texture legs without parsing materials. P1.T1 confirms it, and adds that record if it doesn't.

## Phase 436.1: Scene queries

- [ ] M436.P1.T1 — Binding lookup: which node images reach which prims
  - files: `Engine/Nodes/Scene/USDProject3D.{h,cpp}`, the M233 live-texture code (grep `LiveTexture` under `Engine/Nodes/Scene/`), new `Engine/Nodes/Scene/SceneOverlayBindings.{h,cpp}`, `Tests/Scene/SceneOverlayBindings_Test.cpp`
  - approach:
    - From a scene node's output stage at a time, list (source node, input, kind = projection|uv, prims, primvar, projection camera path, fit mode, hold frame, clip/cull flags).
    - Read these from the authored material parameters and the live-texture resolver's node mapping.
    - Add the missing piece to the binding where it isn't recorded.
  - verify: gtest — a USDCard with USDProject3D fed by a Constant lists one projection binding with the right camera; a live-texture USDPreviewSurface on `st` lists a uv binding.
  - size: M

- [ ] M436.P1.T2 — CPU triangle BVH in world and UV space
  - files: new `Engine/Nodes/Scene/SceneTriangleBvh.{h,cpp}`, CMake list, `Tests/Scene/SceneTriangleBvh_Test.cpp`
  - approach:
    - Triangulate the matched meshes (`UsdGeomMesh` face counts and indices) at a time, in world space with the xform cache.
    - Build a binned-SAH BVH. Queries: `intersect(ray, cull) → hits sorted by t`, and `occluded(segment)`.
    - A second BVH in `st` space answers `pointInUv(uv) → (prim, tri, barycentrics)` lists, with faceVarying/vertex `st` interpolation.
    - Build off the GUI thread, cached by (stage revision, time, prim set).
  - verify: gtest — ray hits on a unit cube are exact; UV lookup on a 2×2 UV-mapped plane returns the right world point; a 100k-triangle mesh builds in < 1 s on the dev host (logged, not a gate).
  - size: L

## Phase 436.2: Map legs

- [ ] M436.P2.T1 — Surface map legs (projection and UV)
  - files: `Engine/OverlayPointMap.{h,cpp}` (multi-valued leg type: point → world points), `Engine/Nodes/Scene/SceneOverlayMaps.{h,cpp}` (new), `Tests/Scene/SceneOverlayMaps_Test.cpp`
  - approach:
    - Projection forward: image canonical → projection camera NDC (inverse of the fit mode) → world ray → BVH hits.
    - UV forward: canonical → uv (the image format normalised to 0–1, UDIM tile offset) → `pointInUv`.
    - Inverse for both: world point → camera NDC → canonical, or barycentric `st` → canonical.
    - `ePending` until the BVH is ready.
  - verify: gtest — a plate corner projected onto a camera-facing card lands on the card's corner; the inverse returns the plate corner within 1e-6; a UV-mapped plane maps image centre to the plane centre.
  - size: M

- [ ] M436.P2.T2 — Render map leg and resolver paths
  - files: `Engine/Nodes/Scene/SceneOverlayMaps.{h,cpp}`, `Engine/Nodes/Scene/USDRender.{h,cpp}` (expose the render camera, fit mode and time), `Engine/OverlayPath.{h,cpp}`
  - approach:
    - Render forward: world point → render camera → canonical in the render's output format, using the render's fit mode. Occlusion is an `occluded` query from the render camera, which flags the vertex "hidden".
    - Inverse: canonical → camera ray → nearest hit → world.
    - The resolver walks image → scene node (via P1.T1's binding) → … → USDRender, chaining 2D legs, the surface leg and the render leg.
  - verify: gtest — Constant → Roto(square) → USDProject3D onto a card → USDRender through the projection camera maps the square's corners to themselves (identity setup) within 0.5 px; a second render camera at 45° maps them to the analytically projected corners.
  - size: M

## Phase 436.3: Drawing and editing

- [ ] M436.P3.T1 — USDRender 2D: draw and edit mapped overlays
  - files: `Gui/ViewerTab20.cpp`, the draw-list drawer (as in M435.P1.T2), `Gui/ViewerTab20.cpp` pen handlers
  - approach: reuse M435's deformed-list drawing with the new chain. Hidden vertices draw at 30% alpha, with segments split at visibility changes. Pen events inverse-map through the render leg, the surface leg and the 2D legs.
  - verify: syntax-check; under Xvfb, viewing the USDRender of a Roto projected onto a card shows the Roto shape on the rendered card and a corner drag moves the Roto knob (screenshots shared with the user).
  - size: M

- [ ] M436.P3.T2 — 3D view: draw and edit overlays on surfaces
  - files: `Gui/SceneViewport.{h,cpp}`, `Gui/SceneGuides.*` (if guides already draw world-space lines), `Gui/ViewerTab20.cpp` (routing 3D-view pen events to overlay nodes)
  - approach:
    - For each overlay node with a surface binding in the displayed stage, map its list through the 2D legs and the surface leg to world space.
    - Draw it after the scene in two passes: depth-tested solid, then depth-failing at 30% alpha. Handles and text are billboards at their anchors.
    - Pen: a pick ray from the cursor goes through the BVH, then the inverse surface leg, then the inverse 2D legs into node space. It goes to the node's pen handlers with the Jacobian-based pixel scale.
    - The overlay takes the pen before the transform widgets when a handle is under it.
  - verify: syntax-check; under Xvfb, the 3D view shows the Roto shape on the card, dimmed where a second card occludes it, and a corner drag in 3D moves the Roto knob (screenshots shared with the user).
  - size: L

- [ ] M436.P3.T3 — GUI test and docs
  - files: `Tests/gui/m436_overlays_on_geometry.py`, the viewer overlay docs page (from M432.P3.T2), `Engine/Nodes/Scene/docs/USDProject3D.md`, `USDRender.md`
  - approach:
    - Constant → Roto(square) → USDProject3D onto a USDCard, with a USDCamera projector and a second render camera.
    - Assert the Roto corner handles in USDRender's 2D view sit at the analytic positions within 1 px.
    - Drag one corner and assert the Roto knob change.
    - In the 3D view, assert a handle hit at the corner's projected screen position.
    - Occlude half the card with a second card and assert the dimmed draw: read the pixel alpha at a hidden handle.
    - Screenshots go to the user.
    - The docs describe overlays on projected and textured geometry. No Nuke mentions.
  - verify: passes under `build/m63-gui/run-gl-tests.sh` or the remote `--gui` gate; the docs build.
  - size: M

**Verification gate:** `SceneOverlayBindings_Test`, `SceneTriangleBvh_Test`, `SceneOverlayMaps_Test` and `m436_overlays_on_geometry.py` pass on a remote release build with `--gui`. Screenshots of the 3D view and the USDRender view go to the user. The UAT happens at F25 - Overlay Flow's landing.

## Decisions

- 2026-11-14 — Overlays show both in the 3D view and in USDRender's 2D output (user, grill).
- 2026-11-14 — Occluded parts are drawn dimmed, not hidden (user, grill): handles are never lost entirely.
- 2026-11-14 — Editable in both views (user, grill), the same rule as M432 - Overlays Off The Render Path.
