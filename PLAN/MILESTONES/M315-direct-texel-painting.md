# M315 - Direct Texel Painting

A second paint model for `USDTexturePaint`: each dab of a stroke is splatted straight into the UDIM texels under it, found by raycasting the brush centre through the session camera and rasterising the brush footprint over the mesh triangles it touches, in their own UV space. There is no screen buffer to project and no bake on navigate. A **Paint Model** knob (Buffer | Direct) picks the model for new sessions. Each session records the model it was painted with, so strokes from both models persist, replay, undo, freeze under Bake Output and compose in session order through the same code. Member of F11 - Texture Painting; follows M53 - Projected Texture Painting (user, 2026-11-02: "both, buffer first").

## Scope

**In:**
- `footprint` knob ("Footprint", Surface | View, default Surface) for Direct: Surface paints within a 3D radius of the hit; View paints exactly what the screen disc covers, as Buffer mode does (user, 2026-11-28).
- A 2-texel seam gutter in both paint models (user, 2026-11-28).
- `paintModel` node knob ("Paint Model", Buffer | Direct, default Buffer) and a saved `paintModel` session knob that each session takes from it.
- A cairo-free stroke dab walker shared by RotoPaint's renderer and direct mode, so both place dabs identically.
- `PaintMeshIndex`: an Embree index over the matched meshes (raycast, sphere query, batched visibility), mesh-source agnostic so M427's `GeoDetail` adapter can feed it.
- `TexelSplat`: footprint rasterisation into sparse paged UDIM tiles for all nine brush types.
- Replay of direct sessions in tile composition, Bake Output and undo; live incremental painting while the pen moves.
- The node serves every UDIM tile: `render()` follows the `udim.tile` render-context variable and the node declares `natron/udimTiles`, so both models show every tile live.
- gtests, a disabled-by-default throughput gtest, and an Xvfb GUI script with screenshots.

**Out:**
- `GeoTexturePaint` support: M427 - Native Geometry Texture Painting (its scope already carries M315 over).
- A GPU splat path: none. The paint path is CPU end to end (SceneRaster, TextureProjection, cairo strokes), so a GPU port is its own E14 stub covering both models.
- Paint-through inputs: M314 - Paint-Through Inputs.
- Deforming geometry: none (geometry is static, as in M53).

**Edge cases:**
- Dab centre ray misses every matched mesh: the dab paints nothing. The stroke is still stored and replays the same way.
- Footprint crosses a UV seam: each triangle rasterises in its own island, so both islands get paint with falloff by 3D distance and the texels between the islands stay untouched.
- Triangle whose UVs span tiles: clipped per tile with SceneRaster's tile convention (u = 1.0 belongs to the next tile).
- Texels facing away from the session camera, or occluded from it: not painted, matching the buffer model's back-face culling and occlusion (`USDTexturePaintCache.cpp:341-343`).
- Degenerate (zero-area) UV triangle: skipped.
- Clone or Smear source ray misses: that texel keeps its value.
- Output Mode Paint Only: direct sessions paint over transparent black, with their touched mask as alpha, as buffer sessions do (M312 decision 2026-11-27).
- Changing Paint Model mid-session: closes the session; the next stroke starts one in the new model.
- Undo of a direct stroke: restores the pre-stroke pages from the journal, or replays from the strokes when the journal has dropped it.
- Overlapping UVs: the existing overlap warning applies, and both meshes write.
- Animated mode: direct strokes honour Roto lifetimes, and the per-frame caches replay as in the buffer model.

**Assumptions:**
- The Embree 4 that `cmake/NatronUsd.cmake:90` requires exposes `rtcPointQuery` and `rtcOccluded16`, and Embree is linkable from `Engine/Nodes/Scene` sources (it is linked PRIVATE to NatronEngine in `Engine/Nodes/Scene/Utility/CMakeLists.txt:3`).
- The dab sequence of `RotoContextPrivate::renderStroke` (`Engine/RotoContext.cpp` around 1700-1780) is deterministic given the stroke points and knobs.
- Storm can reload a live 4K tile about every 100 ms without stalling the viewer. Otherwise the publish interval grows, and that is recorded.
- The buffer model's occluders are the matched meshes only (TextureProjection occlusion over `args.meshes`). P1.T3 confirms this before copying it.

## Phase 315.1: Substrate

- [x] M315.P1.T1 — Extract a cairo-free stroke dab walker used by RotoPaint's stroke renderer
  - files: new `Engine/RotoStrokeDabs.h`/`.cpp`, `Engine/RotoContext.cpp`, `Engine/RotoContextPrivate.h`, new `Tests/RotoStrokeDabs_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: lift the dab placement out of `RotoContextPrivate::renderStroke`/`renderDot` (`RotoContextPrivate.h:2536-2553`, `RotoContext.cpp:1570`, `:1744`, `:1771`) into `RotoStrokeDabs::walk(strokes, distToNext, item, time, mipmapLevel, sink)`. The sink receives each dab: centre, internal and external radius, pressure, opacity stops, build-up and opacity, and the walk returns the carried distance to the next dab. `renderStroke` becomes a sink that calls `renderDot`, so RotoPaint renders exactly as before. No cairo in the new files.
  - verify: the new test checks that the dab count, centres and radii for a fixed multi-segment stroke with pressure match what the cairo path draws (an instrumented sink in a test build); the existing Roto and RotoPaint ctests pass unchanged.
  - size: L

- [x] M315.P1.T2 — `SceneRaster::triangulateMeshes`: world-space triangles with per-corner st
  - files: `Engine/Nodes/Scene/SceneRaster.h`/`.cpp`, `Tests/Scene/SceneRaster_Test.cpp`
  - approach: expose what `UVLayout::prepare` builds privately (`SceneRaster.h:185-210`) as a public `TriangulatedMesh` (path, world points, triangle corners, per-corner st after HdMeshUtil's faceVarying, vertex and varying remap, flat normals) for a stage, time, mesh list and st name. `UVLayout::prepare` then uses it. Keep it plain data so M427's GeoDetail adapter can fill the same struct.
  - verify: test that a unit quad and a two-island cube give the expected triangle count, world corners and st per corner for indexed faceVarying and vertex st; the existing SceneRaster tests pass.
  - size: M

- [x] M315.P1.T3 — `PaintMeshIndex`: Embree raycast, sphere query and batched visibility over TriangulatedMeshes
  - files: new `Engine/Nodes/Scene/PaintMeshIndex.h`/`.cpp`, new `Tests/Scene/PaintMeshIndex_Test.cpp`
  - approach: one Embree scene over P1.T2's meshes, built once per (scene hash, meshes, time) and reused by every dab. The API is `raycast(camera, u, v)` → mesh, triangle, barycentrics, world position, normal, st, UDIM tile; `trianglesInSphere(centre, radius)` via `rtcPointQuery`; and `visible(points, normals, camera, bias)` in `rtcOccluded16` batches. Use `Engine/Nodes/Scene/Utility/EmbreeTracer.cpp` as the Embree API reference, not as a dependency (Tracer has no point query). Thread use stays inside Natron's thread budget. First confirm which occluders TextureProjection uses and mirror them.
  - verify: tests on hand-built meshes. A ray through the quad centre hits with barycentrics (1/3, 1/3) on the expected triangle. A sphere at a cube edge returns the triangles of both faces and none beyond the radius. A point behind another quad is reported occluded. A miss returns no hit. Old gtest: no `GTEST_SKIP`, no `ASSERT_TRUE(ptr)`.
  - size: M

## Phase 315.2: Texel splat engine

- [x] M315.P2.T1 — `TexelPages`: sparse paged UDIM working tiles with a touched mask
  - files: new `Engine/Nodes/Scene/TexelPages.h`/`.cpp`, new `Tests/Scene/TexelPages_Test.cpp`
  - approach: 64x64 half-float RGBA pages plus a per-texel touched alpha, keyed by (tile, plane, page). Pages are allocated only when touched. A page's base is filled lazily from the base file by region (`OiioReadSupport`), else from `USDTexturePaintCache::meshBaseColor` on covered texels, else zero (Paint Only). Pages export into a full tile for `composeTile` and support snapshot and restore for the undo journal. Past a memory budget derived from physical RAM, the least recently touched clean pages are dropped and reloaded later. That is spill, not a cap, and no user size is trimmed.
  - verify: tests check that untouched pages are never allocated, that a base-file region round-trips, that snapshot then write then restore gives the original, and that dropping under a tiny budget reloads the same texels.
  - size: M

- [x] M315.P2.T2 — `TexelSplat`: rasterise a dab's footprint over the triangles under it (Solid, Eraser)
  - files: new `Engine/Nodes/Scene/TexelSplat.h`/`.cpp`, new `Tests/Scene/TexelSplat_Test.cpp`, new fixture `Tests/fixtures/texture-paint-seam-cube.usda` (cube, two UV islands, one face in tile 1002)
  - approach: for each P1.T1 dab, raycast its centre through the session camera. The world radius is the dab radius in paint-space units converted at the hit depth (`viewHeight`, `kPaintSpaceSize`). `trianglesInSphere` gives the triangles, and each is clipped per UDIM tile to the UV bounding box of the footprint's intersection with its plane, then rasterised at Tile Resolution. Per texel: world position by barycentrics, falloff from the 3D distance to the hit through the dab's opacity stops and pressure (the same profile as `renderDot`), dropped if back-facing or not `visible`. Solid and Eraser write the stroke's channels and touched alpha with the same build-up rule as cairo. Gutter: texels within 2 of a touched island edge that no triangle covers take the extrapolated value. Texel lists are built in parallel over triangles and applied per page in dab order, so the result is deterministic at any thread count.
  - verify: tests on the seam cube. A dab on the shared edge paints both islands, and the texel values agree at equal 3D distances (tol 1e-3) across the seam and across tiles 1001/1002. No texel between the islands is touched. A back face is untouched. Output is bit-identical with 1 and N threads.
  - size: L

- [x] M315.P2.T3 — Texture-reading brushes in TexelSplat: Dodge, Burn, Reveal
  - files: `Engine/Nodes/Scene/TexelSplat.cpp`, `Tests/Scene/TexelSplat_Test.cpp`
  - approach: per texel, read the value under it from TexelPages (Dodge and Burn, with RotoPaint's formulas from the shared stroke code), or the base the buffer model reveals (`getPaintTreeRevealSource`'s source, sampled at the texel), weighted by the P2.T2 falloff.
  - verify: tests on a constant-grey quad check Dodge brightens and Burn darkens by the RotoPaint formula at the dab centre, and Reveal over painted red returns the base.
  - size: M

- [x] M315.P2.T4 — Offset-sampling brushes in TexelSplat: Clone and Smear
  - files: `Engine/Nodes/Scene/TexelSplat.cpp`, `Tests/Scene/TexelSplat_Test.cpp`
  - approach: project each texel's world position into the session view, add the clone offset (or, for Smear, the delta from the previous dab), raycast back to a surface point, and sample TexelPages bilinearly at its st. On a miss the texel is unchanged. This works across seams because the lookup goes through the surface.
  - verify: a test clones a painted square to an offset on the quad and recovers it at that offset (tol 1e-2). A Smear drag moves colour along the stroke. A source off the mesh leaves the texels unchanged.
  - size: L

- [x] M315.P2.T5 — Neighbourhood brushes in TexelSplat: Blur and Sharpen
  - files: `Engine/Nodes/Scene/TexelSplat.cpp`, `Tests/Scene/TexelSplat_Test.cpp`
  - approach: a world-space kernel. For each texel, take samples on a disc in its tangent plane with a radius tied to the brush, mapped back to st within the triangle through its UV Jacobian. Samples falling outside the triangle are resolved by a short raycast onto the surface, so the kernel crosses seams. Sharpen is unsharp mask against that blur, with RotoPaint's amounts.
  - verify: a test blurs a hard edge painted across the seam cube's seam, and the edge profile is symmetric on both islands (tol 2e-2). Sharpen increases the edge contrast.
  - size: L

- [x] M315.P2.T6 — View footprint and the Buffer-mode seam gutter
  - files: `Engine/Nodes/Scene/TexelSplat.cpp`, `Engine/Nodes/Scene/USDTexturePaintCache.cpp` (~344), `Engine/Nodes/Scene/USDTexturePaint.{h,cpp}` (`footprint` knob), `Tests/Scene/TexelSplat_Test.cpp`, `Tests/Scene/USDTexturePaint_Test.cpp`
  - approach: Footprint = View: a texel is in the dab when its world position projects inside the dab disc in the session view (falloff by screen distance, as renderDot), still back-face and occlusion tested. The `footprint` knob is saved per session like `paintModel`. Buffer mode's projection gains the same 2-texel gutter Direct uses (extrapolate into uncovered texels within 2 of a touched island edge).
  - verify: on an orthographic front-facing quad, View and Surface footprints agree within a texel; at a grazing quad View stretches and Surface doesn't. A Buffer stroke touching an island edge fills the 2-texel gutter and nothing beyond it.
  - size: M

## Phase 315.3: Node integration

- [x] M315.P3.T1 — Paint Model knob and the per-session `paintModel` knob
  - files: `Engine/Nodes/Scene/USDTexturePaint.h`/`.cpp`, `Engine/Nodes/Scene/USDTexturePaintInteract.cpp`, `Engine/Nodes/Scene/docs/USDTexturePaint.md`, `Tests/Scene/USDTexturePaint_Test.cpp`
  - approach: node knob `paintModel` ("Paint Model", Buffer | Direct, default Buffer, not animatable). Session host knob `paintModel` is added in `onCurveLayerCreated` beside `cameraToWorld` and the others (`USDTexturePaint.h:80-87`), set from the node knob when `addSession` runs and saved with the session. A knob change closes the open session. In Direct, `closeSession` queues no session bake. The doc describes both models and never names other applications.
  - verify: tests check that a session created under Direct saves and loads with `paintModel` Direct, that changing the knob closes the session, and that closing a Direct session queues no bake (`isSessionBakePending` false).
  - size: M

- [x] M315.P3.T2 — Compose, cache and Bake Output direct sessions alongside buffer sessions
  - files: `Engine/Nodes/Scene/USDTexturePaint.cpp` (`buildTiles` at ~3053, `getTileCacheKey`), `Engine/Nodes/Scene/USDTexturePaintCache.h`/`.cpp` (`PaintSession`, `composeTile`), `Tests/Scene/USDTexturePaint_Test.cpp`
  - approach: `PaintSession` gains a direct variant that replays the session's strokes through `TexelSplat` into TexelPages over the composed result below. It is laid by its touched mask in session order, so mixed sessions keep their order. The tile key hashes each session's paint model. Bake Output, freezing, Static/Animated, Paint Only and undo go through the existing paths unchanged. Only the per-session composition step differs.
  - verify: tests cover a buffer session followed by a direct session on one quad and the reverse, with order respected. Bake Output freezes a direct session, and its undo makes it live again with the same tile. Save, close and load rebuild tiles identical to those before. On a front-facing orthographic quad, a direct Solid stroke matches the buffer stroke within one texel of edge.
  - size: L

- [x] M315.P3.T3 — Live incremental direct painting and a page undo journal
  - files: `Engine/Nodes/Scene/USDTexturePaint.cpp`, `Engine/Nodes/Scene/USDTexturePaintBaker.h`/`.cpp`, `Engine/Nodes/Scene/USDTexturePaintInteract.cpp`, `Tests/Scene/USDTexturePaint_Test.cpp`
  - approach: in Direct, the interact hands new dabs of the open stroke to the baker, latest-wins, without blocking the main thread. They splat into the node's live TexelPages, and `render()` serves those pages for the current key without recomposing. The texture version (bakeGeneration) bumps at most every 100 ms and at pen-up. Each stroke snapshots the pages it first touches into a bounded journal, so undo and redo of recent strokes swap pages. Older ones replay from the strokes. Abort and teardown follow the baker's existing cancel and shutdown rules.
  - verify: tests show that pen-down, motion and pen-up in Direct change the output tile with no session bake, that undo restores it bit-exactly from the journal and also with the journal cleared (replay), and that redo matches. Deleting the node mid-stroke does not crash.
  - size: L

- [x] M315.P3.T4 — Serve every UDIM tile: `udim.tile` and `natron/udimTiles`
  - files: `Engine/Nodes/Scene/USDTexturePaint.cpp` (`render` at ~3219, `getPreferredMetadataAt`/`deriveOutputMetadata`), `Tests/Scene/USDTexturePaint_Test.cpp`
  - approach: `render()` takes the tile from `RenderContextName::udimTile()` (`Engine/RenderContext.h:71-74`) when set, else Output Tile (`USDTexturePaint.cpp:607-616`). Metadata declares `kNatronMetadataKeyUdimTiles` as the tiles the matched meshes cover (`SceneRaster::udimTiles`), so `USDLayerMaterial` stops reading the node as one texture (`USDLayerMaterial.cpp:698-708`). This applies to both models.
  - verify: tests show that the seam cube's node declares 1001 and 1002, that a render under a `udim.tile`=1002 context returns tile 1002's paint, and that USDLayerMaterial raises no UDIM warning.
  - size: M

## Phase 315.4: Viewer, tests and GUI

- [x] M315.P4.T1 — 3D view in Direct: live texture, no bake on navigate, open-stroke overlay
  - files: `Gui/SceneViewport.cpp`/`.h` (`refreshPaintOverlays`, `drawPaintOverlays`, `followPaintNode`)
  - approach: in Direct the open stroke's overlay is drawn as now until its pages are published, then cleared. Orbit, pan and zoom show no bake progress. The brush ring stays in view pixels. The layout still fits 1920x1080.
  - verify: the existing `m53_usd_texture_paint_overlay.py` passes, and a manual Xvfb run shows no progress line on orbit in Direct (screenshot in P4.T3).
  - size: M

- [x] M315.P4.T2 — Throughput and memory gtest for the hardware range (disabled by default)
  - files: new `Tests/Scene/TexelSplatBench_Test.cpp`
  - approach: a `DISABLED_` gtest that paints a fixed 200-dab stroke over a procedurally subdivided mesh (about 1M triangles) at 4K and 8K tile resolution. It prints dabs per second at 1 thread and at all threads, peak page memory and index build time. No pass/fail on timing.
  - verify: the PM runs it once with `--gtest_also_run_disabled_tests` on the N100 and once on the many-core host and records the numbers in Decisions. Within the bench budget.
  - size: M

- [x] M315.P4.T3 — Xvfb GUI script: direct painting across a seam and a tile boundary
  - files: new `Tests/gui/m315_direct_texel_paint.py` (modelled on `Tests/gui/m53_usd_texture_paint.py`)
  - approach: build USDRead(texture-paint-seam-cube.usda) into USDTexturePaint, with the panel open and Paint Model set to Direct. A left drag across the cube edge whose faces lie in 1001 and 1002 should change the output tiles without any orbit, waiting on bakeGeneration. Render tiles 1001 and 1002 through Write and check for red texels in both islands. Orbit and screenshot from the new angle. Undo the stroke and check that both tiles are clean. Paint one Buffer stroke after it to check that the models mix. Screenshots: before, after the stroke, orbited, and after undo, one file each.
  - verify: the script exits 0 under `Tests/gui/run-gui-test.sh` at 1920x1080 through `build.sh --gui`. The screenshots go to the user at F11's UAT.
  - size: M

**Verification gate:** full ctest green on the remote gate (and on PR CI, which runs with debug asserts). `m315_direct_texel_paint.py`, `m53_usd_texture_paint.py`, `m53_usd_texture_paint_overlay.py`, `m312_usd_layer_material.py` and `m428_uv_operators_paint.py` all pass via `build.sh --gui`. P4.T2 numbers recorded for the N100 and the many-core host. Screenshots and the UAT AppImage are collected at F11 - Texture Painting's UAT.

## Decisions
- 2026-11-26 (PM): waits for M312.P1.T1 (shared USDTexturePaint files); elaboration is in hand from the 2026-11-26 consultant pass and is written when M312.P1.T1 lands.
- 2026-11-28 (planner) — Footprint by rasterising the triangles under each dab into UV space, not by a per-texel inverse map: memory is a BVH and the touched pages, independent of Tile Resolution and tile count, which suits modest machines. Seams and tile boundaries are handled because each triangle paints its own island, the work parallelises over triangles, and it needs no precompute when Tile Resolution or UVs change.
- 2026-11-28 (planner) — CPU only: the paint path (SceneRaster, TextureProjection, cairo strokes) has no GPU stage, so a GPU splat is a separate stub covering both models.
- 2026-11-28 (planner) — USDTexturePaint only: M427 has not landed and no shared base exists, so the engine (`PaintMeshIndex`, `TexelPages`, `TexelSplat`) takes `TriangulatedMesh` data only, for M427 to adapt.
- 2026-11-28 (planner) — Paint model is recorded per session and the node knob applies to new sessions, so mixed projects replay unchanged after the knob moves.
- 2026-11-28 — **User:** Footprint is a knob (Surface default, View); all nine brushes in Direct; M315 serves every UDIM tile (P3.T4); the 2-texel seam gutter applies to both paint models.
- 2026-11-28 — Phase 1 in eb0d3d5c87 (m315-gate1 running). Embree 4.2.0 has rtcPointQuery/rtcOccluded16. Occluders: TextureProjection's depth buffer has no prim filter, so every drawn mesh occludes (not only matched meshes); PaintMeshIndex mirrors it — matched meshes are paint targets, all other drawn meshes only block. walk() also takes buildUp and opacity. PM: an occluder mesh that fails to triangulate is skipped with a warning rather than failing the index (fold into Phase 2).
- 2026-11-28 — Phase 2 in 7b6b10988b (gate1 cancelled, m315-gate2 covers Phases 1–2). Choices: Sharpen is an unsharp mask (RotoPaint has no Sharpen formula) with Blur's sigma; Reveal samples the base at the texel; dab edge hard at the outer radius; a stroke's working pages stay in RAM until written (committed pages spill). Test thresholds (smear gain, blur symmetry, gutter indices) are hand-derived and may need tuning.
- 2026-11-28 — Phase 3 in 39c9140515 (unbuilt). Follow-ups in flight with Phase 4: incremental live updates (only new dabs), baker as the only writer of working pages, no bake indication during Direct painting. Accepted limits: clone/reveal time offsets ignored in Direct; freezing a Direct session changes its cache key (rebuild from strokes).
- 2026-11-28 — Phase 4 + Phase 3 follow-ups committed (incremental dabs, single-writer snapshot, no bake HUD in Direct). Gate after m315-gate2 (P1–2) reports. Known costs: snapshot keeps painted pages; each undo repaints once on a render thread before the baker catches up.
- 2026-11-28 — m315-gate2 (Phases 1–2): ctest 1101/1102 — TexelSplat (seam, tiles, determinism, brushes) and RotoStrokeDabs green; TexelPages.BaseFileRegionsMatchTheWholeTile fails (one page differs, 4096 values) — fixing. m53/m428 GUI green. (roto_gui.py is a helper module, not a check — wrongly listed; the Roto GUI checks are m146/m261/m272.)
