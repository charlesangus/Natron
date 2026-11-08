# M54 - SOPs-Style Geometry

Full title: SOPs-style geometry — a Houdini-style GeoDetail data kind, bridges, operators, wrangles, point editing

Stage 3 of the 3D roadmap (`PLAN/DESIGN/2026-09-18-3d-roadmap-lops-paint-sops.md`,
Question 3). A new data kind, `eDataKindGeometry`, carries a copy-on-write `GeoDetail`. The
detail follows Houdini's SOP model, not UsdGeom's: one shared point pool, vertices and
primitives indexing into it, and detail/primitive/vertex/point attribute classes. Storage is
paged and copy-on-write per page, with tombstoned deletes and no pxr types. Explicit bridges
connect it to and from the scene stack. The Viewer's 3D view displays it. The Feature also
delivers a classic operator set plus Scatter and CopyToPoints, a Python wrangle, a native
per-element kernel language, and Edit-SOP-style point editing.
Decisions: `PLAN/DECISIONS/2026-09-18-geometry-is-a-fourth-data-kind.md`,
`PLAN/DECISIONS/2026-10-27-geometry-bridges-follow-solaris.md`,
`PLAN/DECISIONS/2026-11-08-native-geometry-follows-sops.md`.

**Ships as a Feature of five member milestones (user, 2026-11-08).** `/cat-plan` splits this
file along the phase map in `## Decisions` and registers the Feature. Until then the phases
below are the members' task lists.

The bridges follow Solaris. `GeoToUSD` works like SOP Import. It doesn't copy the detail
into a layer. Instead it contributes a layer served by an Sdf file format plugin backed by
the node's `GeoDetail`, and the plugin splits the detail into USD prims lazily, by the `path`
primitive attribute. The Viewer and `USDRender` read that same layer. Copying into an
anonymous layer is an explicit, slower opt-in. `USDToGeo` → Geo operators → `GeoToUSD` works
like SOP Modify. Prims arrive as packed USD prims that can be transformed without unpacking.
Unpacking happens only on request, and the result is written back as overrides on the
source prim paths. Component-level display, picking and editing exist only in the geometry
context. In the scene context, picking and editing stay at prim level, as in LOPs.

## Scope

**In:**
- A Houdini-style `GeoDetail`: one point pool, vertices referencing points, and primitives (polygon, polyline/curve, packed) owning vertex lists. Detail, primitive, vertex and point attribute classes. Point, primitive, vertex and edge groups. Dense point/primitive/vertex numbers over tombstoned storage.
- Paged copy-on-write attribute storage (constant pages store one value) with no pxr types anywhere in native geometry.
- Attribute types int32/int64, float32/float64 and string, with tuple sizes 1–16, type info (vector, normal, point, colour, matrix, quaternion) and array attributes. `P` is float32, with a 64-bit option on the detail.
- Packed Geometry primitives (an embedded, shared, immutable detail) and packed USD primitives (source path, transforms, bounds).
- Its cache, the `renderGeometry()` pull path, and `GeoPrimitive`.
- The `natrongeo` Sdf file format plugin. It partitions by `path`, then `name`, then primitive type, and builds each partition's split arrays lazily, caching them per (detail hash, path).
- `GeoToUSD` (new prims, overrides, and a "Packed Primitives" knob: Point Instancer by default, Native Instances, Unpack) and `USDToGeo` (packed by default, unpack on request, writes `path`).
- Geometry display in the Viewer's 3D view through the plugin layer, built on F4 - Heavy Scene Viewer's viewer stage, with core SOP display toggles: point markers, point and primitive numbers, normals, and group highlighting. Component picking and highlighting only in the geometry context.
- The operators: `GeoImportOBJ`, `GeoTransform`, `GeoMerge`, `GeoGroup`, `GeoDelete`, `GeoNormals`, `GeoDisplace`, `GeoSubdivide`, `GeoUVProject`, `GeoScatter`, `GeoCopyToPoints`.
- A Python wrangle (flatten on access, scatter back) and a native, multithreaded per-element kernel language with its own node, run over the pages in batches like VEX.
- `GeoEdit`, keyed by global point number, moving packed primitives by their transforms.
- Page-sharing unit tests and one before/after bench on the GPU host for a 10M-point chain.

**Out:**
- Component-level selection or editing in the scene context. USD nodes select and edit whole prims (M232 - Scene Selection To Nodes owns scene picking).
- Copying geometry into a layer by default. Copying happens only through `GeoToUSD`'s opt-in knob.
- A standalone `GeoUnpack` node and full-shape display of packed USD primitives. They carry no topology, so unpacking happens in `USDToGeo`, where the stage is available, and they display as bounds. Packed Geometry displays as instances.
- Writing `natrongeo` layers to disk in their own format. `USDWrite` exports the composed result as usual.
- PolyExtrude, Boolean, Fuse, attribute create/promote nodes and dict attributes. A new follow-up stub, "Geometry Operators II", owns them; `/cat-plan` adds it.
- Template and footprint display of other nodes and attribute visualisers. A follow-up display milestone outside this Feature owns them; `/cat-plan` adds it.
- Packed disk and packed Alembic primitives (none).
- Timing or memory thresholds on the bench (none). The bench records numbers only.

**Edge cases:**
- An override targets a path that no longer exists upstream (the prim was pruned or renamed after `USDToGeo`). The `over` is inert in USD, and `GeoToUSD` lists the missing paths in a node warning.
- An unpacked prim's topology changed between `USDToGeo` and `GeoToUSD`. The override authors the full topology (`faceVertexCounts`, `faceVertexIndices`, every primvar) as well as `points`.
- Instance proxies and prims under an instanceable prim can't take overrides. `USDToGeo` skips them and lists them in its info message.
- Value-only changes, such as scrubbing a time-varying upstream or editing a point, keep the layer identifier, so Hydra dirties points and primvars instead of resyncing the prim. A topology change resyncs only the partitions whose content changed.
- A detail with no `path` attribute. It falls back to `name`, then to one USD prim per primitive type under the scope path.
- Vertex and edge groups have no USD equivalent. `GeoToUSD` drops them and names them in its info message. Primitive groups become `UsdGeomSubset`s and point groups `bool` primvars.
- The upstream point count changes under a `GeoEdit`. Edits key on global point numbers, as in the Edit SOP, so the node skips every edit and sets a warning rather than moving the wrong points.
- Heavily tombstoned details. Operators keep the holes. Dense consumers (bridges, the Python wrangle, the kernel batches, `GeoEdit`) iterate in point-number order through the index map, and the Python wrangle and bridges defragment into a dense view on access.
- A Python wrangle writing an array of the wrong length, or a kernel snippet that fails to compile. Either sets a node error (traceback, or line and column) and renders nothing, without aborting the render thread.
- An empty detail. `GeoToUSD` contributes an empty layer, with no error.

**Assumptions:**
- `SdfLayer::Reload(true)` on a file-format layer with an unchanged identifier diffs the new data against the old and sends field-level change notices, not a full resync. M54.P2.T5 verifies this first. If it's false, the fallback is to push the diff through `SdfChangeBlock` edits.
- Unconfirmed: pages hold 1024 elements, as Houdini's do.
- Unconfirmed: the detail keeps a lazily built, cached reverse topology (point → vertices → primitives) for operators that need adjacency (Normals, Subdivide, Scatter, Fuse later).
- Unconfirmed: following Houdini's names, the native kernel node is `GeoWrangle` and the Python node is `GeoPython`.
- Unconfirmed: primitives of mixed types sharing one `path` become per-type child prims under that path, with a node warning.
- Unconfirmed: the user's "4 as followup" in the display answer means the full SOP display extras (template/footprint display, attribute visualisers) go to a follow-up milestone outside the Feature.

Requires M52 (`PrimSelector`) and M20.P2.T1 (gizmo). The display and point-editing members
also require F4 - Heavy Scene Viewer to have landed. All nodes are `NativeEffectBase`
subclasses in `Engine/Nodes/Geometry/`, each registered through its own `.reg.inc` (the
pattern in `Engine/Nodes/Scene/USDCard.reg.inc`), under `PLUGIN_GROUP_3D`. Mirror M18's deep
plumbing wherever a choice arises. Native geometry follows SOPs' attribute names (`P`, `N`,
`uv`, `Cd`, `pscale`, `orient`, `path`, `name`), and only `GeoUsdConvert` maps them to USD
names (`points`, `normals`, `primvars:st`, `primvars:displayColor`, `widths`).

## Phase 54.1: Data kind, detail, cache, pull path (member 1)

- [ ] M54.P1.T1 — `eDataKindGeometry` declared and drawn
  - files: `Global/Enums.h`, `Gui/Edge.cpp`, `Gui/NodeGui.cpp`, `Tests/DataKind_Test.cpp`, `Engine/Nodes/README.md`
  - approach: append `eDataKindGeometry` after `eDataKindRaw` in `DataKindEnum` (the enum now holds Image, Deep, Scene, Polymorphic and Raw). Style the edge and node silhouette per the 2026-09-05 design doc's "Communicating kind" rules, distinct from image, deep, scene and raw. Connection typing needs no change, since it is kind-agnostic. Document the kind in the README's "Data kinds" section with the same "declaration only until 54.1 lands" caveat deep carried.
  - verify: `DataKind_Test` extended — a geometry source into a scene input is refused, geometry → `Dot` → geometry consumer resolves; Xvfb GUI screenshot shows the new edge style.
  - size: M

- [ ] M54.P1.T2 — Paged copy-on-write attribute storage
  - files: `Engine/Geo/GeoAttribute.h`/`.cpp`, `Engine/Geo/GeoPage.h`, `Tests/GeoAttribute_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: Houdini GA-style columns with no pxr types. An attribute is a name, a storage type (int32, int64, float32, float64, string), a tuple size 1–16, a type-info tag (none, vector, normal, point, colour, matrix, quaternion) and an array flag, stored as a page table of `shared_ptr<const Page>`. A page holds 1024 elements (Scope assumption). A page whose elements all share one value stores that value once, as a constant page. Writing an element detaches only its page. Strings are stored as indices into a shared per-attribute string table. Array attributes store per-element offsets into a paged value pool. Expose typed per-element get/set and a page visitor. Also expose `copyDense(range)`/`scatterDense(range)` for consumers that need contiguous data.
  - verify: unit tests — copying an attribute shares every page; writing 5 elements of a 10M-element attribute detaches exactly the pages containing them (page pointers compared); a constant page stays one value until written; round-trips for every storage type, tuple size and array attribute.
  - size: L

- [ ] M54.P1.T5 — Element index maps with tombstones and defragment
  - files: `Engine/Geo/GeoIndexMap.h`/`.cpp`, `Tests/GeoIndexMap_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: Houdini's offset/index split. Each element class (point, vertex, primitive) maps dense element numbers to storage offsets, holes included. Deleting tombstones offsets without moving attribute data, and appending reuses nothing until a defragment. `defragment()` returns a new map plus a remap table that the detail applies to every attribute of that class (page-aware, so untouched pages stay shared where offsets don't move). The map is itself copy-on-write.
  - verify: unit tests — delete then iterate yields the surviving elements in number order; number ↔ offset round-trips; defragment compacts and the remap table moves the right values; a defragment with no holes is a no-op that shares everything.
  - size: L

- [ ] M54.P1.T6 — `GeoDetail`: classes, topology, groups, hash
  - files: `Engine/GeoDetail.h`/`.cpp`, `Engine/Geo/GeoTopology.h`/`.cpp`, `Engine/Geo/GeoGroup.h`/`.cpp`, `Tests/GeoDetail_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: the detail holds four attribute sets (detail, primitive, vertex, point) on M54.P1.T2's columns and M54.P1.T5's index maps. Topology is stored as attributes, as in Houdini: vertex → point offset, primitive → vertex list (paged start and count), and primitive → type (polygon, polyline, packed geometry, packed USD). `P` is a required float32 point attribute, with a detail flag for 64-bit. Point, primitive, vertex and edge groups are paged bitsets, or sets of point-number pairs for edges. A lazily built, cached reverse topology (point → vertices → primitives) is dropped whenever topology changes (Scope assumption). `contentHash` is a U64 combined from per-page hashes and cached. `getSizeInBytes()` is aliasing-blind like `DeepImage`. Copies share every page; mutation goes through a builder that detaches only what it writes.
  - verify: unit tests — moving 5 points detaches only `P` pages holding them, and topology and other attributes stay shared; identical content hashes identically; adding a polygon and deleting a point keep vertex → point consistent; groups survive a defragment; `Engine/GeoDetail.h` and `Engine/Geo/` include no pxr header (grep-enforced, as M19.P1.T2).
  - size: L

- [ ] M54.P1.T7 — Packed primitives
  - files: `Engine/Geo/GeoPacked.h`/`.cpp`, `Engine/GeoDetail.cpp`, `Tests/GeoPacked_Test.cpp`
  - approach: a packed primitive is Houdini's: one vertex on one point (its pivot), plus intrinsics stored per primitive — local transform, and bounds. Packed Geometry also holds a `shared_ptr<const GeoDetail>` (immutable and shared, so a million copies cost a million pointers). Packed USD holds a source prim path and its parent's world transform, so a writeback can turn a new world transform back into a local one. The contained detail's hash feeds the outer hash once per unique pointer.
  - verify: unit tests — copying a detail with 1000 packed-geometry instances of one detail shares that detail (one allocation); changing one instance's transform changes the hash and detaches nothing else; bounds transform with the packed primitive.
  - size: M

- [ ] M54.P1.T3 — `Cache<GeoDetail>` with its own budget
  - files: `Engine/GeoDetailKey.h`/`.cpp`, `Engine/GeoDetailCacheEntry.h`/`.cpp`, `Engine/Settings.cpp`/`.h` (budget knob), `Tests/GeoDetailCache_Test.cpp`
  - approach: third instantiation of the templated cache, mirroring `DeepImageKey`/`DeepImageCacheEntry` from M18. The key is (nodeHash, time, view), and time is part of the key only when something upstream varies with time (2026-10-25 decision). Cost = `getSizeInBytes()`, a pure function of shape at allocate and deallocate (the 2026-09-09 amendment rule). A separate memory budget knob keeps geometry from ever evicting the image cache.
  - verify: unit test — store/fetch by key; a time-invariant chain hits the cache at a different frame; budget clamp evicts; counter returns to zero after eviction.
  - size: M

- [ ] M54.P1.T4 — `renderGeometry()` capability virtual and pull path
  - files: `Engine/EffectInstance.h`/`.cpp`, `Engine/Nodes/NativeEffectBase.h`/`.cpp`, `Tests/GeoRender_Test.cpp`
  - approach: mirror the deep path M18 added (`renderDeep()` and its `renderDeepRoI`-shaped pull): `EffectInstance::renderGeometry(const GeoRenderActionArgs&, GeoDetailPtr*)` plus a pull that walks typed inputs, honours `getFramesNeeded()`, abort flags and the scheduler, consults `Cache<GeoDetail>` first, and ignores RoI (whole-detail pull, as scene). `NativeEffectBase` gains `renderGeometryFromInput(args, input, mutate)` (fetch input detail, hand a COW copy to `mutate`) and a generator variant. Trivial proof node `GeoConstant` (one triangle), used by the test only (`internalUseOnly = true`).
  - verify: unit test — `GeoConstant` → passthrough → consumer pulls the detail through the cache (second pull is a cache hit); abort mid-pull returns cleanly.
  - size: L

- [ ] M54.P3.T1 — `GeoPrimitive`
  - files: `Engine/Nodes/Geometry/GeoPrimitive.h`/`.cpp`/`.reg.inc`, `Engine/Nodes/Scene/PrimitiveMeshes.h`/`.cpp`, `Tests/GeoPrimitive_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: generator; type knob `card | grid | cube | sphere | cylinder`, size/segment knobs per type, emits `uv` (vertex class) and `N`. Shares M227's `PrimitiveMeshes` builders (2026-10-26 decision): factor a pxr-free output form out of them if they return pxr types. Card is a single quad in XY with optional subdivisions.
  - verify: unit test — each type yields the expected point/primitive/vertex counts and a closed topology; `uv` covers 0–1; the M227 USD primitives still pass their tests.
  - size: M

- [ ] M54.P1.T8 — Scale bench on the GPU host
  - files: `tools/bench/geo_scale.py` (or the existing bench harness's equivalent), `Tests/GeoBench_Test.cpp` (bench-only, not in ctest)
  - approach: build a 10M-point grid and time `GeoTransform` → `GeoEdit`(5 points) → `GeoDelete`(half) → `GeoToUSD`, before and after a change, in one command on the GPU host (memory: benches together on the GPU host). Record wall time and peak memory, with no thresholds. Lands with the first member whose operators exist. Until `GeoTransform`/`GeoEdit` land, it times the detail API directly.
  - verify: the bench runs to completion on the GPU host and its numbers are recorded in the member's PR body.
  - size: M

## Phase 54.2: Bridges and viewing (member 2; waits on F4 - Heavy Scene Viewer)

- [ ] M54.P2.T1 — `GeoUsdConvert`: partitioning and the attribute table
  - files: `Engine/Nodes/Geometry/GeoUsdConvert.h`/`.cpp`, `Tests/GeoUsdConvert_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: the one pxr-linked translation layer. It partitions the detail into USD prims by the `path` primitive attribute, falling back to `name`, then to one prim per primitive type under the scope (mixed types under one path: Scope assumption). Each partition gathers its points, re-indexes its vertices and maps by type: polygons → `UsdGeomMesh`, polylines → `UsdGeomBasisCurves`, unconnected points → `UsdGeomPoints`. Attribute classes map detail → `constant`, primitive → `uniform`, point → `vertex`, vertex → `faceVarying`. Names map per Solaris (`P`→`points`, `N`→`normals`, `uv`→`primvars:st`, `Cd`→`primvars:displayColor`, `pscale`→`widths`, the rest → `primvars:<name>`). Primitive groups → `UsdGeomSubset`, point groups → `bool` primvar; vertex and edge groups are dropped and reported. Packed USD → `over` at its source path with `xformOp:transform`. Packed Geometry → Point Instancer, native instances or unpacked, by mode. Two faces over one table. A **field-level query** (partition + field → spec type, field names, `VtValue`) is what M54.P2.T5's `SdfAbstractData` answers from. **Authoring** onto an `SdfLayer` at a `UsdTimeCode` serves the copy opt-in. Reading back works on a `UsdPrim` and writes `path`.
  - verify: round-trip unit test on an anonymous layer — a detail with two `path` partitions, a point `Cd` attribute and a primitive group authors two `UsdGeomMesh` prims with matching points and indices, `primvars:displayColor` with `vertex` interpolation, and a `UsdGeomSubset`; converting back yields a detail with the same content hash; the field-level query returns the same values the authoring path wrote, field by field; a no-`path` detail falls back as specified.
  - size: L

- [ ] M54.P2.T5 — `natrongeo` Sdf file format plugin backed by `GeoDetail`
  - files: `Engine/Nodes/Geometry/GeoSdfFileFormat.h`/`.cpp`, `Engine/Nodes/Geometry/GeoSdfData.h`/`.cpp`, `Engine/Nodes/Geometry/plugInfo.json` (+ install rule), `Engine/Nodes/Scene/RenderDelegates.cpp` (register the plug-in directory beside the delegates), `Tests/GeoSdfFileFormat_Test.cpp`
  - approach: the Solaris SOP-import model. An `SdfFileFormat` (format id and extension `natrongeo`, read-only: `CanRead` true, write and export refused) whose `Read` installs a `GeoSdfData : SdfAbstractData`. It answers `HasSpec`, `Get`, `List` and `VisitSpecs` lazily through M54.P2.T1's field-level query. The partition table (paths and types) is cheap and built on open. A partition's split arrays are built on the first field query that needs them and cached per (detail hash, path), so unvisited prims cost nothing. Register with the existing `registerPluginDirectory` path in `RenderDelegates.cpp`, which already calls `PlugRegistry::RegisterPlugins` at startup. Layer identity is **node only**, never the content hash: `<node-uuid>.natrongeo` plus file-format args for output, view and mode (new prims vs overrides). A process-wide registry maps the identifier to a provider that returns the node's current detail. When the detail hash changes, the owner calls `layer->Reload(true)`, and only partitions whose per-partition hash changed are rebuilt. Check the Scope assumption about field-level reload notices first. If it fails, push the diff through `SdfChangeBlock` edits and record that in `## Decisions`. No node logic here; the test drives a registry entry directly.
  - verify: unit test — `SdfLayer::FindOrOpen` on a registered `GeoConstant` identifier gives a stage whose mesh matches the detail; querying one of two partitions builds only that partition's arrays; changing one point value and reloading keeps the identifier, and `UsdNotice::ObjectsChanged` reports `points` under changed-info-only, not as a resync; a topology change resyncs that prim alone; the layer refuses `Save`.
  - size: L

- [ ] M54.P2.T2 — `GeoToUSD`
  - files: `Engine/Nodes/Geometry/GeoToUSD.h`/`.cpp`/`.reg.inc`, `Tests/GeoToScene_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: inputs Geometry (+ optional Scene to append to). Knobs: root scope path; "author time samples across range" (default off, since vertex data is never auto-baked); "Packed Primitives" (Point Instancer default, Native Instances, Unpack, as SOP Import); "Copy Into Layer" (default off). Output kind **scene**. By default the node contributes its `natrongeo` layer (M54.P2.T5) to the `ScenePayload` layer stack, over the optional Scene input, with the detail's partitions under the scope path. Nothing is copied, and the plugin converts on demand. The layer is keyed per M19's per-node read keys, and a detail-hash change reloads it in place. "Copy Into Layer" authors the detail into the node's anonymous layer through `GeoUsdConvert` instead. This is the slower path, and the knob's tooltip says so. An empty detail contributes an empty layer.
  - verify: unit test — a `GeoConstant` detail appears under the scope path on the composed stage, and the stack holds a `natrongeo` layer and no anonymous copy; re-evaluating with an unchanged detail re-emits the same layer handle; a changed point value keeps the layer identifier; 100 packed-geometry copies author one `UsdGeomPointInstancer` with 100 instances by default, and native instances or meshes in the other modes; with "Copy Into Layer" on, the same prims come from an anonymous layer instead.
  - size: M

- [ ] M54.P2.T3 — `USDToGeo`: packed by default, unpack on request
  - files: `Engine/Nodes/Geometry/USDToGeo.h`/`.cpp`/`.reg.inc`, `Tests/SceneToGeo_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: the SOP Modify model. Inputs Scene. Knobs: `PrimSelector`, "Unpack" (default off), "flatten transforms" (bake world xform into `P`; enabled only when unpacked); time follows the frame. Packed: each matched imageable prim becomes one packed USD primitive (M54.P1.T7) with its source path, local and parent-world transforms and bounds, plus a `path` primitive attribute. No point arrays are read, so transform-only chains stay cheap. Unpacked: mesh/points/curves read through `GeoUsdConvert` into the one detail, each primitive's `path` set to its source prim path. Unsupported types, instance proxies and prims under instanceable prims are skipped and listed in the node's info message.
  - verify: unit test — `USDRead`(reference `.usda`) → `USDToGeo` gives one packed primitive per matched prim with the right `path` and transforms, and no topology; with Unpack on it yields the expected polygons in one point pool; with flatten also on, a translated prim's points move; `GeoToUSD` → `USDToGeo`(unpacked) is the identity on a detail.
  - size: M

- [ ] M54.P2.T6 — `GeoToUSD` writes back overrides on source paths
  - files: `Engine/Nodes/Geometry/GeoToUSD.cpp`, `Engine/Nodes/Geometry/GeoSdfData.cpp`, `Engine/Nodes/Geometry/GeoUsdConvert.cpp`, `Tests/GeoToSceneOverrides_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: the SOP Modify writeback. When a Scene input is connected, partitions whose `path` exists on the input stage are served as `over` specs on that path in the `natrongeo` layer (the overrides mode arg) instead of new prims under the scope path. Other partitions still go under the scope. Packed USD → `xformOp:transform` local transform (new world × inverse parent world). Unpacked → `points` and changed primvars, plus full topology when the counts differ from the source. Paths missing upstream become a node warning (Scope edge cases).
  - verify: unit test — `USDRead` → `USDToGeo`(packed) → a test-only node that translates each packed primitive through the `GeoDetail` API → `GeoToUSD` moves the source prims on the composed stage at their original paths and adds no new prims; the unpacked chain overrides `points` only; a pruned source path raises the warning.
  - size: M

- [ ] M54.P2.T4 — The Viewer's 3D view displays geometry nodes through the `natrongeo` layer
  - files: `Gui/SceneViewport.cpp`/`.h`, `Engine/ViewerInstance.h`/`.cpp`, `Gui/ViewerTab.cpp`
  - approach: built on the viewer stage as F4 - Heavy Scene Viewer leaves it (deferred, worker-composed; re-read `PLAN/DESIGN/2026-11-04-scene-graph-design.md` at claim). Extend the Viewer's input acceptance and kind-driven view switch to the geometry kind. When the Viewer's active input is geometry, the viewer stage opens that node's `natrongeo` layer (M54.P2.T5), the same layer `GeoToUSD` renders from, so viewing and rendering share one conversion. A detail-hash change reloads the layer in place, so scrubbing and value edits dirty points instead of rebuilding the stage (the 2026-10-25 decision). Packed USD primitives draw as their bounds, and packed geometry as instances. Picking returns the prim path, which maps back to the partition and its primitive numbers.
  - verify: manual — a `GeoPrimitive` node viewed in the Viewer's 3D view shows the mesh; scrubbing a time-varying upstream node updates it without a stage rebuild (no resync notice); clicking a prim reports its primitive numbers; a packed `USDToGeo` output shows bounds boxes.
  - size: M

- [ ] M54.P2.T7 — Core SOP display toggles in the geometry context
  - files: `Gui/SceneViewport.cpp`/`.h`, `Gui/ViewerTab.cpp` (toolbar toggles), `Gui/GeoDisplayGuides.h`/`.cpp`
  - approach: viewer toggles shown only when the active input is geometry: point markers, point numbers, primitive numbers, normals (`N`, as lines scaled by a viewer setting) and highlighting of a chosen group. All are drawn as guide prims (purpose `guide`) authored into a viewer-private session layer, the same layer M54.P4.T1 uses for markers. Nothing is drawn by a second GL pass outside Hydra. Number labels are drawn only for elements in view, capped per frame, so a 10M-point detail stays responsive.
  - verify: manual (Xvfb screenshots shared before sign-off) — each toggle shows on a `GeoPrimitive` cube and clears when switched off; viewing a `GeoToUSD` downstream offers none of them; a 1M-point grid with point numbers on keeps the viewer interactive.
  - size: M

## Phase 54.3: Operators and the Python wrangle (member 3)

- [ ] M54.P3.T2 — `GeoImportOBJ`
  - files: `Engine/Nodes/Geometry/GeoImportOBJ.h`/`.cpp`/`.reg.inc`, `Engine/Nodes/Geometry/ObjParser.h`/`.cpp`, `Tests/GeoImportOBJ_Test.cpp`, a tiny committed `.obj`
  - approach: minimal OBJ parser (v/vt/vn/f with n-gons) into one shared point pool. `vt` → vertex `uv`, `vn` → vertex `N`. Groups become primitive groups, and objects set the `name` primitive attribute, as Houdini's File SOP does. USD/Alembic import is `USDRead` → `USDToGeo`, so this node is OBJ only. A missing file sets a node error.
  - verify: unit test — the committed `.obj` loads with matching counts, `uv`, two groups and two `name` values.
  - size: M

- [ ] M54.P3.T3 — `GeoTransform`
  - files: `Engine/Nodes/Geometry/GeoTransform.h`/`.cpp`/`.reg.inc`, `Tests/GeoTransform_Test.cpp`
  - approach: TRS + pivot knobs (same layout as `USDEditTransform`), optional group restriction. It transforms `P` and every attribute by its type info, as the Transform SOP does: point and vector attributes by the matrix, normals by the inverse transpose, quaternions by rotation. Only the pages it writes detach. Packed primitives get their transform multiplied instead, since they have no points to move.
  - verify: unit test — transform moves points; `uv` pages are shared with the input's; `N` stays unit and correct under non-uniform scale; a packed primitive's transform is multiplied and its bounds move with it.
  - size: M

- [ ] M54.P3.T4 — `GeoMerge`
  - files: `Engine/Nodes/Geometry/GeoMerge.h`/`.cpp`/`.reg.inc`, `Tests/GeoMerge_Test.cpp`
  - approach: multi-input concatenation as the Merge SOP does it. Point, vertex and primitive pools are appended in input order. An attribute missing on one input is created with its default there, and conflicting types promote as Houdini does (int → float, smaller tuple → larger). Groups union by name. `path` and `name` are ordinary attributes and are never renamed.
  - verify: unit test — merging two details keeps every primitive with consistent vertex → point indices; an attribute present on one input only is defaulted on the other's elements; same-named groups union.
  - size: M

- [ ] M54.P3.T5 — `GeoGroup`
  - files: `Engine/Nodes/Geometry/GeoGroup.h`/`.cpp`/`.reg.inc`, `Tests/GeoGroup_Test.cpp`
  - approach: creates a named point, primitive, vertex or edge group from one of: a bounding box, a number pattern (`0-10 20 !5`, Houdini group syntax, plus existing group names), or an attribute comparison (`@attr op value`, scalar attributes). The output is the input with the group added, and only group storage detaches.
  - verify: unit test — a bbox group of a grid selects exactly the left-half points; a number pattern selects exactly those primitives; an attribute comparison over a point float attribute selects the expected set; an edge group from a bbox holds exactly the edges inside it.
  - size: M

- [ ] M54.P3.T6 — `GeoDelete`
  - files: `Engine/Nodes/Geometry/GeoDelete.h`/`.cpp`/`.reg.inc`, `Tests/GeoDelete_Test.cpp`
  - approach: removes the members of a named group, or its complement, by tombstoning (M54.P1.T5) rather than compacting. Deleting points removes every primitive and vertex using them; deleting primitives optionally deletes their now-unused points, as the Blast SOP does. Attribute data does not move.
  - verify: unit test — deleting a grid's left-half point group leaves the right half valid in number order, and attribute pages not containing deleted elements stay shared; "delete non-selected" gives the complement; a defragment afterwards gives a dense, identical-content detail.
  - size: M

- [ ] M54.P3.T7 — `GeoNormals`
  - files: `Engine/Nodes/Geometry/GeoNormals.h`/`.cpp`/`.reg.inc`, `Tests/GeoNormals_Test.cpp`
  - approach: computes area-weighted `N` on points (default) or vertices (with a cusp angle, as the Normal SOP does), or on primitives. It uses the detail's cached reverse topology. Exposes the computation as a static helper that other nodes (`GeoDisplace`, `GeoEdit`) call after moving points.
  - verify: unit test — normals of a cube are unit length and outward; a flat grid's are all +Z; vertex normals with a 60° cusp keep a cube's faces flat.
  - size: M

- [ ] M54.P3.T8 — `GeoDisplace`
  - files: `Engine/Nodes/Geometry/GeoDisplace.h`/`.cpp`/`.reg.inc`, `Tests/GeoDisplace_Test.cpp`
  - approach: moves points along `N` by a scalar from a noise knob or an optional **Image** input sampled at `uv` (luminance), with amplitude and offset knobs and optional group restriction. Recomputes normals via `GeoNormals`' helper afterwards, and computes them first if the input has none.
  - verify: unit test — a flat grid displaced by a constant image moves every point by the same amount along +Z and its normals stay +Z.
  - size: M

- [ ] M54.P3.T9 — `GeoSubdivide` via OpenSubdiv
  - files: `Engine/Nodes/Geometry/GeoSubdivide.h`/`.cpp`/`.reg.inc`, `Tests/GeoSubdivide_Test.cpp`, CMake (link OpenSubdiv, already found in `cmake/NatronUsd.cmake`)
  - approach: Catmull-Clark or Loop, level knob 1–4. It interpolates point and vertex attributes through the refiner (vertex `uv` as face-varying channels), and primitive attributes and groups carry to child faces. Uses OpenSubdiv's Far refiner directly rather than Hydra.
  - verify: unit test — cube at level 1 has the expected face count and stays closed; `uv` is interpolated; a primitive group survives on the child faces.
  - size: M

- [ ] M54.P3.T10 — `GeoUVProject`
  - files: `Engine/Nodes/Geometry/GeoUVProject.h`/`.cpp`/`.reg.inc`, `Tests/GeoUVProject_Test.cpp`
  - approach: writes vertex `uv` from a planar (axis + TRS) or camera projection (inputs: optional Scene for the camera; camera path knob).
  - verify: unit test — planar projection of a grid yields `uv` equal to its normalized XY; camera projection of a camera-facing quad yields 0–1.
  - size: M

- [ ] M54.P3.T11 — Python wrangle over the detail
  - files: `Engine/Nodes/Geometry/GeoPython.h`/`.cpp`/`.reg.inc`, `Engine/Nodes/Geometry/GeoPyBuffer.h`/`.cpp`, `Tests/GeoPython_Test.cpp`
  - approach: Houdini's Python SOP model (node name: Scope assumption). A script knob (multi-line `KnobString`) runs in Natron's embedded Python under the GIL during `renderGeometry()`. A `geo` object exposes attributes per class (`geo.points["P"]`, `geo.prims["name"]`, `geo.vertices["uv"]`, `geo.detail[...]`) via the CPython buffer protocol (a small hand-written extension type, no Shiboken typesystem changes, no pxr Python). On first access it builds a dense, defragmented copy in number order (`copyDense`), shaped (N, tupleSize). At script end it scatters written arrays back (`scatterDense`), so only changed pages detach. Scripts never see pages or holes. Creating an attribute by assignment is allowed. numpy is optional (`np.frombuffer(view)` if present). A wrong-length write or a script error becomes a node error with the traceback.
  - verify: unit test — a script doubling every `P` z doubles Z and leaves untouched pages shared; a syntax error and a wrong-length write each report on the node without aborting the render thread.
  - size: L

- [ ] M54.P3.T12 — `GeoScatter`
  - files: `Engine/Nodes/Geometry/GeoScatter.h`/`.cpp`/`.reg.inc`, `Tests/GeoScatter_Test.cpp`
  - approach: the Scatter SOP's core. Scatters a point count (or density × area) over polygon surfaces, area-weighted, with a seed and an optional primitive-group restriction. Optional relaxation iterations. Optionally interpolates chosen source attributes (`N`, `uv`, `Cd`) onto the points and records `sourceprim`/`sourceprimuv`, as Houdini does. Output is unconnected points.
  - verify: unit test — 1000 points on a unit grid all lie on it, the count is exact, the same seed reproduces them, and interpolated `N` is +Z.
  - size: M

- [ ] M54.P3.T13 — `GeoCopyToPoints`
  - files: `Engine/Nodes/Geometry/GeoCopyToPoints.h`/`.cpp`/`.reg.inc`, `Tests/GeoCopyToPoints_Test.cpp`
  - approach: the Copy to Points SOP. Inputs are source geometry and target points. Each copy is oriented by Houdini's template-point attributes, in the same precedence: `transform`, `orient`, `N`/`up`, `pscale`/`scale`, `trans`, `pivot`. "Pack and Instance" (default off, as in Houdini) emits one Packed Geometry primitive per point, all sharing the source detail. Otherwise copies are expanded into the pool. Chosen target attributes are copied onto each copy's elements.
  - verify: unit test — copying a cube onto 100 scattered points yields 100 cubes at the point positions, oriented by `N` and scaled by `pscale`; with Pack and Instance on, 100 packed primitives share one detail allocation.
  - size: M

## Phase 54.6: Native kernel language (member 4)

- [ ] M54.P6.T1 — Kernel language front end
  - files: `Engine/Geo/GeoKernelParser.h`/`.cpp`, `Engine/Geo/GeoKernelTypes.h`, `Tests/GeoKernelParser_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: a small VEX-like snippet language, growing from `Engine/Nodes/Deep/DeepExpressionEvaluator`'s lexer and precedence table but typed. Types are int, float, vector2/3/4, matrix3/4 and string. Statements: declarations, assignment and compound assignment, `if`/`else`, `for`, `while`. Attribute bindings are `@name` with a type prefix (`v@N`, `i@id`, `s@name`) or inferred for known names (`@P`, `@N`, `@Cd`, `@uv`, `@pscale`, `@orient`), plus `@ptnum`, `@primnum`, `@vtxnum`, `@numpt`, `@Frame`, `@Time`. Builtins cover maths, vector and matrix ops, `fit`, `clamp`, `lerp`, noise, `rand`, and read-only `point()`/`prim()` lookups. The front end compiles to a typed, register-based bytecode and reports errors with line and column.
  - verify: unit tests — a corpus of valid snippets compiles to the expected bytecode shape; type errors and unknown names report the right line and column; `@P.y += 1` binds `P` as a vector write.
  - size: L

- [ ] M54.P6.T2 — Batched, multithreaded kernel runner over pages
  - files: `Engine/Geo/GeoKernelRunner.h`/`.cpp`, `Tests/GeoKernelRunner_Test.cpp`
  - approach: how VEX hides pages. Run the bytecode over one element class (points, primitives, vertices or detail) in batches of a page. The runner gathers bound attributes into batch registers, runs the program across the batch, and scatters writes back, so only written pages detach. Batches run on the host thread pool, skip tombstones, and present `@ptnum` as the dense number. Writes to an attribute that doesn't exist create it, typed by the binding. Abort is checked between batches.
  - verify: unit tests — `@P.y += 1` over 10M points matches a scalar reference; pages not written stay shared; results are identical across thread counts; holes are skipped and `@ptnum` stays dense; an abort stops within one batch per thread.
  - size: L

- [ ] M54.P6.T3 — The kernel wrangle node
  - files: `Engine/Nodes/Geometry/GeoWrangle.h`/`.cpp`/`.reg.inc`, `Engine/Nodes/Geometry/docs/GeoWrangle.md`, `Tests/GeoWrangle_Test.cpp`
  - approach: the Attribute Wrangle (node name: Scope assumption). Knobs: snippet (multi-line `KnobString`), "Run Over" (points, primitives, vertices, detail), optional group restriction. It compiles once per snippet change and runs through M54.P6.T2. A compile error becomes a node error with line and column. The doc page lists the language, bindings and builtins.
  - verify: unit test — a snippet doubling `@P.z` doubles Z; running over primitives with a group touches only that group; a compile error reports line and column on the node without aborting the render thread.
  - size: M

## Phase 54.4: Point editing (member 5, with 54.5; waits on F4 - Heavy Scene Viewer)

- [ ] M54.P4.T1 — 3D-view point and primitive picking
  - files: `Gui/SceneViewport.cpp`/`.h`
  - approach: point and primitive pick modes using `UsdImagingGLEngine::TestIntersection` with the pick target set to points or faces (Storm renders the id AOVs for this), with click and marquee select. Selection is stored as global point numbers or primitive numbers, mapped from (prim path, index) through the partition table. Highlighting uses Storm's own selection: an `HdSelection` with point- or element-index highlighting through the engine's selection tracker, plus guide prims (purpose `guide`) in the viewer-private session layer for point markers and the marquee. Nothing is drawn by a second GL pass outside Hydra. Component display and picking exist **only in the geometry context**, when the Viewer's active input is geometry. In the scene context, picking stays at prim level (M19.P2.T3, M232 - Scene Selection To Nodes), as in Solaris. Packed primitives pick as whole primitives.
  - verify: manual — clicking a vertex of a `GeoPrimitive` cube highlights it; marquee selects the expected set; a packed `USDToGeo` output picks whole packed primitives; viewing a `GeoToUSD` downstream of the same cube offers prim picking only, with no point mode.
  - size: M

- [ ] M54.P4.T2 — `GeoEdit` node with viewport drag
  - files: `Engine/Nodes/Geometry/GeoEdit.h`/`.cpp`/`.reg.inc`, `Gui/SceneViewport.cpp` (drag → knob writeback), `Tests/GeoEdit_Test.cpp`
  - approach: the Edit SOP. It stores a sparse map from global point number to offset, and from packed primitive number to transform delta, in a serialized knob (a `KnobString` holding a compact text encoding is acceptable; a table knob if one fits). It also records the input's point and primitive counts at edit time. Render applies the offsets to `P` (detaching only the touched pages) and multiplies the packed primitives' transforms. It then recomputes `N` if present. In the viewport, with the node selected and points or packed primitives picked (M54.P4.T1), M20.P2.T1's translate gizmo drags them, and release writes one knob change (one undo entry). Topology guard: if the input's point or primitive count differs from the recorded count, skip all edits and set a node warning.
  - verify: unit test — an edit map round-trips through serialization and moves exactly the listed points; a packed primitive's edit moves its transform only; changing the upstream point count triggers the warning and leaves points unmodified. Manual — drag creates one undo entry.
  - size: L

## Phase 54.5: Integration (member 5)

- [ ] M54.P5.T1 — Geometry pipeline integration test in CI
  - files: `Tests/GeoPipeline_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: headless graph — `GeoPrimitive`(grid) → `GeoTransform` → `GeoGroup`(bbox) → `GeoDelete` → `GeoDisplace`(constant) → `GeoNormals` → `GeoWrangle`(`@P.y += 0.1`) → `GeoEdit`(one point) → `GeoToUSD` → `USDMerge`(with `USDRead`) → `USDWrite`. Assert the exported mesh's point count, one edited point's position, and `normals`. Also `USDToGeo`(unpacked) of the same export equals the pre-bridge detail after defragment. A second chain is the SOP Modify round trip: `USDRead` → `USDToGeo`(packed) → `GeoTransform` → `GeoToUSD`(Scene input connected) → `USDWrite`. Assert the source prims moved at their original paths and the export has no extra prims. A third is scatter-instancing: `GeoScatter` → `GeoCopyToPoints`(Pack and Instance) → `GeoToUSD` → `USDWrite`, asserting one `PointInstancer` with the scattered count.
  - verify: test green in `build-and-test`.
  - size: M

**Verification gate:** each member's own phase tests green in CI, with M54.P5.T1 in member 5. Page-sharing assertions hold (M54.P1.T2, M54.P1.T6), and the M54.P1.T8 bench numbers are recorded on the GPU host. On the Feature's UAT AppImage: a geometry chain displays in the Viewer's 3D view through its `natrongeo` layer, with the core display toggles; scrubbing dirties points without a stage rebuild; point picking and `GeoEdit` drag work with undo in the geometry context only; and the result renders through `GeoToUSD` → `USDRender` to an image with no copied layer. Both wrangles edit points. The geometry cache respects its own budget. The pre-existing ctest suite is green. `Engine/GeoDetail.h`, `Engine/Geo/` and every non-bridge geometry node stay pxr-free.

## Decisions

- 2026-10-25 — **Long-lived USD scene (bosley-native-nodes, from the review in `PLAN/DESIGN/2026-10-25-usd-long-lived-scene-review.md`):** The scratch stage "keyed by detail hash" would rebuild per frame: use a viewer-private geometry layer edited in place instead, so value-only changes become dirty points. GeoToUSD layer keys follow M19's per-node read keys. Geometry keys on time only when something upstream varies with time (user).
- 2026-10-26 — **USD primitives come first (user, M19/M20 UAT grill):** card, sphere, cube and cylinder ship as USD-authoring source nodes in M227 - USD Primitive Nodes; `GeoPrimitive` (P3.T1) stays the native-geometry generator and should share M227's `PrimitiveMeshes` builders rather than duplicate them. Read `USDTransform` as `USDEditTransform` (M226 - Scene Hierarchy From Topology).
- 2026-10-27 — **Bridges follow Solaris (user, from a Houdini SOP Import/SOP Modify investigation; `PLAN/DECISIONS/2026-10-27-geometry-bridges-follow-solaris.md`):** `GeoToUSD` serves the detail through a `natrongeo` Sdf file format plugin (new M54.P2.T5) rather than authoring into a layer, and copying is the "Copy Into Layer" opt-in. `USDToGeo` brings prims in packed by default with Unpack on request, and `GeoToUSD` writes the result back as overrides on the source paths (new M54.P2.T6). Component display and picking exist only in the geometry context, through `HdSelection` highlighting plus guide prims. The plugin layer is identified by node, not content hash (user): a hash change reloads it in place, which keeps the 2026-10-25 dirty-points behaviour, and the viewer-private layer in that decision is now this layer. The scratch stage and `GeoScratchStage` are gone.
- 2026-11-02 — No longer depends on M53 - Projected Texture Painting (user, inbox drain): the only link was `GeoUVProject` pairing with `ProjectTexture`, which M53's redesign unregisters.
- 2026-11-08 — **Ships as a Feature of five members (user, /cat-grill):** one PR, review and UAT for all of it was too large. Members, for `/cat-plan` to split into milestone files: (1) data model — P1.T1–T8 and P3.T1 `GeoPrimitive`; (2) bridges and display — P2.T1–T7; (3) operators and the Python wrangle — P3.T2–T13; (4) native kernel language — P6.T1–T3; (5) point editing and integration — P4.T1–T2 and P5.T1. Members 2 and 5 also depend on F4 - Heavy Scene Viewer landing (user): it is replacing the viewer stage they build on. Members 1, 3 and 4 don't touch the Viewer. M337 - Geometry Spreadsheet's `M54` dep should point at member 1 (plus member 2 if it shows the viewer's selection).
- 2026-11-08 — **Native geometry follows SOPs (user):** "prefer doing things the same way SOPs does"; `path` is an ordinary attribute, not a USD-style identity. Project-wide: `PLAN/DECISIONS/2026-11-08-native-geometry-follows-sops.md`. Under it, without a separate question: point/primitive/vertex/edge groups, detail/primitive/vertex/point classes, SOP attribute names (`P`, `N`, `uv`, `Cd`) mapped to USD names only in `GeoUsdConvert`, Transform SOP type-info handling, Merge SOP attribute promotion, and Houdini group syntax.
- 2026-11-08 — **Houdini unified detail, not a UsdGeom prim list (user):** one point pool with vertices and primitives indexing it. This supersedes the 2026-09-18 container sketch and the 2026-10-27 decision's zero-copy serving: the bridges now partition and re-index.
- 2026-11-08 — **Partition into USD prims by the `path` primitive attribute (user):** then `name`, then one prim per primitive type, matching Solaris SOP Import; `USDToGeo` writes `path`, so a round trip is stable.
- 2026-11-08 — **natrongeo splits lazily per partition, cached by (detail hash, path) (user):** unvisited prims cost nothing, and a reload rebuilds only changed partitions.
- 2026-11-08 — **No pxr types in native geometry; paged copy-on-write storage like Houdini GA (user):** chosen over contiguous columns, after comparing contiguous SoA, SoA with constant columns and derived topology, paged COW, a half-edge mesh, and array-of-structs. Paged COW scales best on huge details in deep chains, since a sparse edit detaches pages rather than columns, and constant pages compress.
- 2026-11-08 — **Tombstones plus defragment, not compaction on delete (user):** Houdini's offset/index maps. Dense consumers iterate in number order.
- 2026-11-08 — **Both a Python wrangle and a native per-element kernel language (user):** after learning that VEX hides pages by being a compiled per-element kernel the runtime batches over pages, while the Python SOP flattens and scatters. The Python node flattens on access and scatters back. The kernel language gets its own member (user).
- 2026-11-08 — **Scatter and CopyToPoints join the operators (user);** PolyExtrude, Boolean, Fuse, attribute create/promote and dict attributes go to a new "Geometry Operators II" stub.
- 2026-11-08 — **GeoEdit keys on global point number, like the Edit SOP (user),** reversing the same session's earlier "prim path + point index" once `path` became an ordinary attribute. Packed primitives are moved by their transforms (user).
- 2026-11-08 — **Packed kinds: Packed Geometry and packed USD (user);** `GeoToUSD` has SOP Import's "Packed Primitives" knob with Point Instancer as the default (user). Disk and Alembic packed kinds are out.
- 2026-11-08 — **Attribute types: everything but dict (user):** int32/64, float32/64, string, tuple sizes 1–16 with type info, and arrays; `P` float32 with a 64-bit option. Dict has no USD primvar equivalent.
- 2026-11-08 — **Display: core SOP toggles in this Feature (user):** point markers, point and primitive numbers, normals, and group highlighting. Template/footprint display and attribute visualisers go to a follow-up outside the Feature (user).
- 2026-11-08 — **Scale is proven by page-sharing unit tests plus one GPU-host bench with no thresholds (user).**
- 2026-11-08 — Code findings at grill time: `DataKindEnum` already holds `eDataKindRaw`, so geometry is the sixth value; nodes register through per-node `.reg.inc` files, not `Engine/AppManager.cpp`; `PlugRegistry::RegisterPlugins` already runs at startup in `Engine/Nodes/Scene/RenderDelegates.cpp`; `PrimitiveMeshes` lives in `Engine/Nodes/Scene/`. Task file lists updated to match.
