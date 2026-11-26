# Blender and Omniverse: direct Hydra feeds vs USD round-trips

Tags: [F] fact, read from a cited source. [I] my inference. [M] from memory, not re-verified this session.

Access caveats: WebFetch returned 403 for projects.blender.org and docs.omniverse.nvidia.com HTML pages. I did not work around this by spoofing a browser (declined by the permission layer). Blender facts below come from the Gitea API/raw endpoints (curl worked until Cloudflare challenged), which gave current main source. Omniverse facts come from WebSearch result summaries of NVIDIA docs, not full page reads. Raw Blender source copies are in `build/m617-research/raw/`.

## 1. Blender Hydra integration

### 1.1 Python API (local docs)
- [F] `bpy.types.HydraRenderEngine` is "Base class for integrating USD Hydra based renderers" with `bl_delegate_id`, `bl_use_materialx`, `get_render_settings(engine_type)` for PREVIEW/FINAL/VIEWPORT, and inherited `update/render/view_update/view_draw` (`.local/docs/blender/python-api-5.3/bpy.types.HydraRenderEngine.md`).
- [F] `SceneHydra.export_method`, default `'HYDRA'`:
  - `HYDRA`: "Fast interactive editing through native Hydra integration."
  - `USD`: "Export scene through USD file, for accurate comparison with USD file export." (`bpy.types.SceneHydra.md`)
- [F] Blender 4.0 shipped the Storm render engine as an add-on (AMD authored, upstreamed from GPUOpen BlenderUSDHydraAddon; PR #104597) on top of the C++ Hydra engine (PR #104712). https://wiki.blender.org/release_notes/4.0/add_ons/ , https://projects.blender.org/blender/blender/pulls/104712 (search-result summaries only).
- [F] The manual notes the Hydra viewport does not support materials on geometry subsets (USD issue #542), so Blender applies the first material to the mesh itself too (`manual-5.3/files/import_export/usd.md` line ~507).

### 1.2 The two export methods
Source: `source/blender/render/hydra/engine.cc` lines ~113-140 (main, fetched 2026-11-26).
- [F] `HYDRA` mode: `HydraSceneIndex` keeps persistent C++ state; each viewport update calls `populate(depsgraph, view3d)` which diffs against what it emitted last time and pushes only the changes into an `HdRetainedSceneIndex`.
- [F] `USD` mode: `USDSceneIndex::populate` (`io/usd/hydra/usd_scene_index.cc`) runs the full USD exporter to a temp `scene.usdc`, throws away the previous stage and scene indices, builds a new `UsdStage`, builds `UsdImagingCreateSceneIndices`, and inserts it. No diffing. Engine comment: "Slow USD export for reference."
- [F] Why both exist: USD mode is a correctness oracle. It guarantees the Hydra view matches what a file export would give. HYDRA mode is the fast interactive path. [I] The oracle role also lets reviewers diff the two for regressions.
- [F] Perf trade-off is stated in the enum text itself: native = "Fast interactive editing"; USD = "accurate comparison".

### 1.3 Delegate vs scene index: it changed
- [F] Originally (4.0 to about 5.2) it was a `HdSceneDelegate` subclass (`HydraSceneDelegate`, `ObjectData`, `id.cc`, `usd_scene_delegate.cc`) answering Hydra callbacks and marking rprims dirty. [M] on the dirty-bit details (MarkRprimDirty with DirtyPoints/DirtyTopology/DirtyTransform etc.). The removed file list is a fact: commit 590af512e3 deleted `hydra_scene_delegate.*`, `id.*`, `object.*`, `usd_scene_delegate.*`.
- [F] Commit 590af512e3 (2026-05-27, "Render: Hydra: Migrate render delegate to use Hydra 2.0 API", PR #158456): "The 1.0 API is deprecated (though not yet removed) in OpenUSD 26.xx releases... The API is now based on constructing a scene index with prims and schemas, rather than getting callbacks through HdSceneDelegate to query data. This simplifies the implementation somewhat and makes it more similar to Cycles and EEVEE. The tricky handling of updates, objects, instances and geometry sets was made to work more similar to Cycles... Storm render tests render identical to before... the latest Maya and Houdini version are using Hydra 2.0." https://projects.blender.org/blender/blender/commit/590af512e323719a9b11eeca8e5d9d06d963abf6
- [F] Current code lives in `source/blender/io/usd/hydra/` (scene_index.cc/.hh, populate_context.cc/.hh, mesh.cc, instancer.cc, material.cc, curves.cc, ...) and `source/blender/render/hydra/` (engine, viewport_engine, render_task_delegate, python.cc). Tasks are also inserted as prims into a retained scene index, not a task delegate.
- [F] Engine inserts a filter chain after the retained index: `HdSiExtComputationPrimvarPruningSceneIndex` -> `HdFlatteningSceneIndex` -> `UsdImagingMaterialBindingsResolvingSceneIndex`, then `render_index_->InsertSceneIndex`.
- [F] `HydraSceneIndex` deliberately does not subclass `HdRetainedSceneIndex`, it wraps one by ref-ptr: "builds with assert have an incompatible ABI on Windows and this causes a crash when the destructor destroys STL containers" (scene_index.hh). Relevant to our Linux-only build only as a note: a debug/release pxr ABI mismatch trap.

### 1.4 How `populate()` maps depsgraph updates to Hydra changes (scene_index.cc)
All [F], read from source.
1. Collect `id->recalc` for updated IDs via `DEG_iterator_ids` with `only_updated`.
2. Drop caches keyed by dirty IDs: materials with any recalc; geometry whose ID has `ID_RECALC_GEOMETRY`, plus geometry-set (instance) sources whose owner object is dirty.
3. Iterate all evaluated objects (including dupli instances). Per real object:
   - No geometry change and no transform change: reuse the previously emitted prims unchanged (re-register paths only).
   - Transform-only: build a new xform overlay data source, re-compose the prim, push a `DirtyPrims` entry with `HdXformSchema::GetDefaultLocator()`. The geometry data source (`base_data_source`) is untouched and shared. This is the fast path, with per-prim fine-grained locator dirtying.
   - Geometry change: remove the cached emitted geometry, rebuild it (mesh -> per-material `SubMesh` with triangulated topology, points, per-corner normals, UVs; built with `threading::parallel_for` per material), compose with object transform and visibility, `AddPrims` (re-adds the path).
4. Final reconcile: `RemovePrims` for paths no longer present, `AddPrims` for new/changed, `DirtyPrims` for xform-only. Paths set is diffed each pass (`emitted_paths_`).
- [F] There is no "points only" fast path. Any `ID_RECALC_GEOMETRY` rebuilds the whole geometry prim (topology + points + normals + uvs) as retained data sources (`HdRetainedTypedSampledDataSource<VtVec3fArray>`). Deform-only edits of a heavy mesh therefore pay a full copy. [I] Blender accepted this because Storm is secondary to its own viewport and a typical use is final-ish rendering.
- [F] A known TODO in source: backface culling lives on the material in Blender and on geometry in USD; updates to it are not propagated.
- [F] Mesh data is copied out of Blender types into `VtArray`s: single float32 position/normal copies; submeshes by material; unused vertices stripped "since Storm warns about indices into unused vertices".

### 1.5 Instancing
- [F] Dupli/instances: the source object's prototype is emitted once (`emit_mesh_proto`, `proto_range_by_source`) and a single instancer prim gets `instance_transforms` and per-proto indices (`build_instancer_prim_data_source`). Lights and volumes are not instancer-capable here and get one prim per dupli. Geometry sets are tracked via `instance_geometries_by_object_` to invalidate on updates. Hair emitted separately.

### 1.6 Viewport integration
- [F] `ViewportEngine::render` (viewport_engine.cc) runs `engine_->Execute(render_index, tasks)`, then copies the AOV colour (GPU texture or mapped CPU buffer) into a texture and blits it with `GPU_SHADER_3D_IMAGE` into Blender's active framebuffer. Hydra is rendered as an image; Blender's own overlays still draw on top via DRW. [I] This is also why Hydra mode lacks Blender-native selection, edit-mode, and tool overlays: it is a render engine behind a texture, not the viewport drawer.
- [F] `engine.cc` has a "Hydra Storm: Vulkan HGI" selection (commit 425e8743fb, 2025-05-27).

### 1.7 Status and maintenance
- [F] Actively maintained: commits through 2026-11-25 in `io/usd/hydra` (fixes: volume material after update 2026-09-19, viewport-local camera 2026-09-01, USDPreviewSurface fallback 2026-06-23, crash GetCullStyle for non-mesh 2026-03-09, Hydra 2.0 migration 2026-05-27). Fixes in `render/hydra` through 2026-11-12 (points drawn at 1px).
- [F] Nothing was removed from the feature. The 4.x Python add-on was upstreamed; the C++ scene-delegate backend was reworked to a scene index in 5.2/5.3 era (merge date 2026-05-27).
- [F] Upstream OpenUSD: from 25.11 the UsdImagingGL scene-index path is the default; 26.03 emits a deprecation warning for the scene delegate path; removal date undecided. https://forum.aousd.org/t/deprecating-hydra-1-eventually/2804 . `HdSceneIndexAdapterSceneDelegate`/`HdRenderIndexAdapterSceneIndex` exist for mixed use. Relevant for us: a hand-written `HdSceneDelegate` is now a dead-end API; build the native feed as a scene index.

## 2. Blender's own draw manager

- [F] Mesh GPU data lives in `Mesh::runtime->batch_cache` (`MeshBatchCache`), `source/blender/draw/intern/draw_cache_impl_mesh.cc`. Batches and VBOs/IBOs are requested lazily by the engines (`batch_requested` flags, e.g. `MBC_SURFACE`, `MBC_SURFACE_PER_MAT`), then built in `DRW_mesh_batch_cache_create_requested(task_graph, ...)` using a task graph (parallel extraction, `draw_cache_extract_mesh.cc`, extractors per VBO type such as `extract_positions`).
- [F] Invalidation: `DRW_mesh_batch_cache_dirty_tag(mesh, mode)` with modes SELECT, SELECT_PAINT, SHADING, UVEDIT_ALL, UVEDIT_SELECT, ALL. ALL sets `cache.is_dirty`; `mesh_batch_cache_valid` then fails and the whole cache is re-initialised. Narrower tags discard only particular VBOs/IBOs (e.g. EditData, PaintOverlayFlag, shaded tris). So topology-class edits rebuild everything; selection/shading edits rebuild slices. (read lines 395-440, 549-580)
- [F] Rendering is synchronous within the same redraw: engines request batches during cache-populate, the manager creates them, then draws. [I] This request-then-build-then-draw ordering within one frame is the reason there is no visible "blank frame" or flash: old GPU buffers are never drawn partially, and a stale batch is replaced before the draw call in the same frame. Blender does not show "loading" states.
- [F] Perf history: a 2019 task reported batch-cache creation took ~half the time when moving a single vertex and proposed parallelising it (https://developer.blender.org/T64261); a 2024 tracker item covers extraction improvements (https://projects.blender.org/blender/blender/issues/118599); 2019 draw-manager rewrite goals (https://developer.blender.org/T75120). So even Blender, with a mature native path, found re-extraction on deform edits costly and invested in parallel extraction, GPU subdivision (`draw_cache_impl_subdivision.cc`), and PBVH-based sculpt drawing (`DRW_pbvh.hh`) which updates only touched nodes. [I] The high-frequency deform case got special-cased (sculpt PBVH, GPU subdiv, mesh-wrapper "mapped" extraction) rather than made generic.
- [F] Maintenance footprint: `draw/` has `engines/` plus intern; per-type cache impls for mesh, curve, curves, grease pencil, gsplat, lattice, particles, pointcloud, volume, plus edit-mode, overlay, selection buffers (directory listing from API).
- Why Blender keeps its own viewport rather than Hydra: [I], not found stated anywhere. Evidence: (a) edit-mode/overlay/selection/paint/sculpt need per-element data (edit flags, select state, weights, UV edit, face dots) that Hydra schemas do not carry; (b) Hydra is wired in as a render engine whose AOV is blitted, with Blender overlays on top; (c) the HYDRA export copies geometry (`VtArray`) every geometry edit, which Blender's native cache avoids by sharing GPU buffers; (d) EEVEE/Workbench integrate with Blender's GPU module and materials natively. Treat as inference.

## 3. USD / Hydra community and Omniverse

### 3.1 Stage round-trip costs (OpenUSD forum, search summaries)
- [F] Adding an xformOp triggered a full subtree resync (render delegate recreates rprim) while changing an existing op's value did not: https://forum.aousd.org/t/usdimaging-delegate-odd-resync-dirty-bits/1252
- [F] ~10k reparentings took 8 s inside `UsdStage::_Recompose`; keep child counts small: https://forum.aousd.org/t/performance-issues-with-prim-reparenting-and-creation-due-to-usdstage-recompose/843
- [F] Bulk prim creation without `SdfChangeBlock` is very slow; inside a change block use only Sdf APIs: https://forum.aousd.org/t/suggestions-on-creating-usd-scenes-fast-while-rendering-them/1878
- [F] UsdStage is many-readers/single-writer; no concurrent authoring to layers feeding one stage.
- [I] Implication for our Sdf-layer-diff path: every edit is Sdf spec change -> notice -> stage recompose -> UsdImaging resync/dirty -> Hydra. Whether the edit is a "resync" (rebuild prim) or an "update" (dirty bits) depends on what kind of spec changed. Replacing a prim spec or adding attributes resyncs; changing an attribute value only dirties. Large `VtArray` attributes authored into an Sdf layer are also copied (VtArray copy-on-write helps if the same array handle is moved in).
- [F] Houdini has a "Background Update USD Stage" viewport option for the same problem (Solaris LOPs stage cook is slow vs interactive): https://www.sidefx.com/forum/post/380123/ (title only seen).

### 3.2 NVIDIA Omniverse: Fabric, USDRT, FSD (NVIDIA docs, via search summaries)
- [F] Fabric: "fast access, modification, topology editing and change notification for a composed USD stage, gpu-cpu interoperability, and replication support" (USD, Fabric, and USDRT: https://docs.omniverse.nvidia.com/kit/docs/usdrt/latest/docs/usd_fabric_usdrt.html). A doc comparison table lists data write speed as USD slow, Fabric fastest, USDRT fast. USDRT arrays stay attached to Fabric storage so edits write directly.
- [F] USDRT does not replace USD composition: "USDRT does not replace USD's composition engine or sidestep the need for USD stages"; it still maintains a stage underneath to populate Fabric. Fabric works on the post-composition view. Composition changes (variant select) must go through USD.
- [F] OmniHydra: older design, reads USD (optionally some Fabric), originally built "to work around UsdImagingDelegate's performance limitations for dynamic content".
- [F] Fabric Scene Delegate (FSD): "using Fabric as the exclusive data source for all rendering operations, eliminating the USD bottleneck"; development focus moved to FSD; OmniHydra may be deprecated. https://docs.omniverse.nvidia.com/kit/docs/usdrt.scenegraph/latest/fabricsd/fsd_vs_omnihydra.html ; FSD in USD Explorer by default since Kit 105.2.
- [F] FSD trade-offs: USD-to-Fabric sync is explicit/delayed (stale reads from USDRT unless synced); config docs say FSD may help a lot when most of the hierarchy changes every frame and may hurt when only a small subset changes; Fabric is "highly optimized for batch processing, but propagating changes one-by-one can be very slow"; recommends authoring through USD where possible, some direct Fabric edits may go undetected; extent workarounds can hurt big scenes. https://docs.omniverse.nvidia.com/kit/docs/usdrt.scenegraph/latest/fabricsd/known-issues.html , .../configuration.html
- [F] Threading: Fabric and USD are not thread-safe for writes; keep stage and Fabric writes on the main thread (NVIDIA dev forum thread 386491).
- [I] Lesson: NVIDIA, with full control of the stack, concluded the stage is the wrong runtime store for per-frame dynamic data, and moved to a flat, typed, array-oriented store with Hydra reading it directly. Their remaining USD stage is for persistence/composition, not interactive transport. This matches Blender's HYDRA mode: persistent native state + diffs into a retained scene index, with the USD path kept as a slow reference.

### 3.3 Pixar side
- [F] Hydra 2.0 guide: scene index replaces `HdSceneDelegate`; apps can add scene-index plugins for proceduralism even if the integration is delegate-based. `HdRetainedSceneIndex` supports `AddPrims`, `RemovePrims`, `DirtyPrims` with schema locators. https://openusd.org/dev/api/_page__hydra__getting__started__guide.html
- Gap: found no published benchmark of scene-index vs emulated delegate, nor of Sdf-layer-diff vs retained-index feeding.

## 4. Lessons for the decision (all [I] unless tagged)

1. Blender, a well-resourced project that wanted Hydra, did not route interactive edits through Sdf/UsdStage. It built a native-to-Hydra feed that diffs against retained emitted state and pushes `AddPrims/RemovePrims/DirtyPrims` into `HdRetainedSceneIndex`, and kept the full USD export as a slow "reference". [F]
2. Option (a) is validated and is now the Hydra-2 canonical shape. A scene index, not an `HdSceneDelegate` (Blender migrated for exactly the API-deprecation reason, and said it simplified updates). [F]
3. Blender's own path is not magic: any geometry recalc rebuilds the whole prim copy (no P-only fast path); only transform/visibility changes use fine locators. For us the P-only fast path (dirty `primvars/points` locator on an existing prim, topology data source reused) is an extra gain Blender does not have.
4. The data source granularity matters: keep topology, points, normals, UVs as separate data sources/handles so a knob that only moves points can re-emit points and dirty `HdPrimvarsSchema` points + extent, leaving topology handle shared (Blender shares geometry data sources between objects and composes xform/visibility overlays on top).
5. Instancing: one prototype prim + one instancer prim with per-instance transforms and proto indices; do not emit prim-per-instance. [F]
6. Option (b) own drawer: Blender pays for a large per-type draw cache, extraction code, overlay/selection, and kept optimising it for years (parallel extraction, GPU subdivision, PBVH). It is justified by edit-mode/paint features Hydra lacks. For a compositor viewport of procedural geometry without edit-mode, that cost is mostly avoidable. Hydra Storm remains the shading/AOV engine. (inference; no source states Blender's rationale)
7. Flicker: Blender avoids it by building before drawing in the same frame (synchronous request-build-draw) and by retaining old prims until replaced. A flash in our case likely comes from remove+add of a prim path (resync) or a rebuilt stage/scene index between frames; Blender's Hydra path also re-adds on geometry change but within one populate with `RemovePrims` only for truly gone paths. Keep stable prim paths across edits. USD mode in Blender tears down the stage and scene index and rebuilds, which is the flash-prone shape. [I]
8. Omniverse corroborates that the stage is the bottleneck for dynamic data; note FSD does worse for small-subset updates, so batch/coalesce changes and prefer per-prim dirtying. FSD also keeps USD as persistence only and requires explicit sync for consistency. Write on the main thread.
9. Keep the USD layer path as a debug oracle (like Blender's `USD` export_method) for parity tests and for saving/exporting; do not use it for interactive transport.
10. Keep the retained-scene-index handle ABI aware: pxr must match build config (Blender's comment about assert/ABI).

## Source list
- Blender main source (fetched via Gitea API): `source/blender/io/usd/hydra/{scene_index,populate_context,mesh,instancer,usd_scene_index}.{cc,hh}`, `source/blender/render/hydra/{engine,viewport_engine}.cc`, `source/blender/draw/intern/draw_cache_impl_mesh.cc`.
- Commit https://projects.blender.org/blender/blender/commit/590af512e323719a9b11eeca8e5d9d06d963abf6
- Blender 4.0 add-ons notes https://wiki.blender.org/release_notes/4.0/add_ons/
- Local: `.local/docs/blender/python-api-5.3/bpy.types.HydraRenderEngine.md`, `bpy.types.SceneHydra.md`, `manual-5.3/files/import_export/usd.md`
- Omniverse: https://docs.omniverse.nvidia.com/kit/docs/usdrt.scenegraph/latest/fabricsd/ (fsd_vs_omnihydra, known-issues, configuration, overview), https://docs.omniverse.nvidia.com/kit/docs/usdrt/latest/docs/usd_fabric_usdrt.html, https://forums.developer.nvidia.com/t/supported-threading-model-for-updating-usd-fabric-off-the-main-thread/386491
- OpenUSD: https://forum.aousd.org/t/deprecating-hydra-1-eventually/2804 , https://openusd.org/dev/api/_page__hydra__getting__started__guide.html , forum threads 1252, 843, 1878.
