# M54 - Native Geometry Data Model

Stage 3 of the 3D roadmap (`PLAN/DESIGN/2026-09-18-3d-roadmap-lops-paint-sops.md`,
Question 3), first member of F15 - Native Geometry. A new data kind, `eDataKindGeometry`,
carries a copy-on-write `GeoDetail`. The detail follows Houdini's SOP model, not UsdGeom's:
one shared point pool, vertices and primitives indexing into it, and detail/primitive/vertex/
point attribute classes. Storage is paged and copy-on-write per page, with tombstoned deletes
and no pxr types. This member delivers the detail, its cache, the `renderGeometry()` pull path
and `GeoPrimitive`. The rest of F15 - Native Geometry builds on it: M358 - Geometry Bridges And
Display, M359 - Geometry Operators, M360 - Geometry Kernel Language, M361 - Geometry Point
Editing and M337 - Geometry Spreadsheet.
Decisions: `PLAN/DECISIONS/2026-09-18-geometry-is-a-fourth-data-kind.md`,
`PLAN/DECISIONS/2026-10-27-geometry-bridges-follow-solaris.md`,
`PLAN/DECISIONS/2026-11-08-native-geometry-follows-sops.md`.

Member of F15 - Native Geometry: branches off and merges into `feature/f15-native-geometry` (PLAN-FORMAT §9b); the Feature's UAT covers it.

The Feature-wide design lives here, in this member's Scope, because every other member builds on
it; each member's file also carries its own Scope.

## Scope

**In (this member):**
- A Houdini-style `GeoDetail`: one point pool, vertices referencing points, and primitives (polygon, polyline/curve, packed) owning vertex lists. Detail, primitive, vertex and point attribute classes. Point, primitive, vertex and edge groups. Dense point/primitive/vertex numbers over tombstoned storage.
- Paged copy-on-write attribute storage (constant pages store one value) with no pxr types anywhere in native geometry.
- Attribute types int32/int64, float32/float64 and string, with tuple sizes 1–16, type info (vector, normal, point, colour, matrix, quaternion) and array attributes. `P` is float32, with a 64-bit option on the detail.
- Packed Geometry primitives (an embedded, shared, immutable detail) and packed USD primitives (source path, transforms, bounds).
- The geometry cache, the `renderGeometry()` pull path, and `GeoPrimitive`.
- Page-sharing unit tests.

**Out:**
- The bridges, the `natrongeo` plugin and Viewer display (M358 - Geometry Bridges And Display).
- The operators and the Python wrangle (M359 - Geometry Operators).
- The native kernel language (M360 - Geometry Kernel Language).
- Picking, `GeoEdit`, the integration test and the scale bench (M361 - Geometry Point Editing).
- The spreadsheet view (M337 - Geometry Spreadsheet).
- PolyExtrude, Boolean, Fuse, attribute create/promote nodes and dict attributes (M362 - Geometry Operators II).
- Template and footprint display and attribute visualisers (M363 - Geometry Display Extras).
- Packed disk and packed Alembic primitives (none).

**Edge cases (Feature-wide; members restate the ones they handle):**
- Heavily tombstoned details. Operators keep the holes. Dense consumers (bridges, the Python wrangle, the kernel batches, `GeoEdit`) iterate in point-number order through the index map, and the Python wrangle and bridges defragment into a dense view on access.
- An override targets a path that no longer exists upstream (the prim was pruned or renamed after `USDToGeo`). The `over` is inert in USD, and `GeoToUSD` lists the missing paths in a node warning.
- An unpacked prim's topology changed between `USDToGeo` and `GeoToUSD`. The override authors the full topology (`faceVertexCounts`, `faceVertexIndices`, every primvar) as well as `points`.
- Instance proxies and prims under an instanceable prim can't take overrides. `USDToGeo` skips them and lists them in its info message.
- Value-only changes, such as scrubbing a time-varying upstream or editing a point, keep the layer identifier, so Hydra dirties points and primvars instead of resyncing the prim. A topology change resyncs only the partitions whose content changed.
- A detail with no `path` attribute. It falls back to `name`, then to one USD prim per primitive type under the scope path.
- Vertex and edge groups have no USD equivalent. `GeoToUSD` drops them and names them in its info message. Primitive groups become `UsdGeomSubset`s and point groups `bool` primvars.
- The upstream point count changes under a `GeoEdit`. Edits key on global point numbers, as in the Edit SOP, so the node skips every edit and sets a warning rather than moving the wrong points.
- A Python wrangle writing an array of the wrong length, or a kernel snippet that fails to compile. Either sets a node error (traceback, or line and column) and renders nothing, without aborting the render thread.
- An empty detail. `GeoToUSD` contributes an empty layer, with no error.

**Assumptions:**
- Unconfirmed: pages hold 1024 elements, as Houdini's do.
- Unconfirmed: the detail keeps a lazily built, cached reverse topology (point → vertices → primitives) for operators that need adjacency (Normals, Subdivide, Scatter, Fuse later).
- Unconfirmed: the user's "4 as followup" in the display answer means the full SOP display extras (template/footprint display, attribute visualisers) go to M363 - Geometry Display Extras, outside the Feature.

Requires M52 (`PrimSelector`) and M227 (`PrimitiveMeshes`). All nodes are `NativeEffectBase` subclasses in `Engine/Nodes/Geometry/`, each registered through its own `.reg.inc` (the pattern in `Engine/Nodes/Scene/USDCard.reg.inc`), under `PLUGIN_GROUP_3D`. Native geometry uses SOP attribute names (`P`, `N`, `uv`, `Cd`, `pscale`, `orient`, `path`, `name`); only `GeoUsdConvert` maps them to USD names. Mirror M18's deep plumbing
wherever a choice arises.

## Phase 54.1: Data kind, detail, cache, pull path

- [x] M54.P1.T1 — `eDataKindGeometry` declared and drawn
  - files: `Global/Enums.h`, `Gui/Edge.cpp`, `Gui/NodeGui.cpp`, `Tests/DataKind_Test.cpp`, `Engine/Nodes/README.md`
  - approach: append `eDataKindGeometry` after `eDataKindRaw` in `DataKindEnum` (the enum now holds Image, Deep, Scene, Polymorphic and Raw). Style the edge and node silhouette per the 2026-09-05 design doc's "Communicating kind" rules, distinct from image, deep, scene and raw. Connection typing needs no change, since it is kind-agnostic. Document the kind in the README's "Data kinds" section with the same "declaration only until 54.1 lands" caveat deep carried.
  - verify: `DataKind_Test` extended — a geometry source into a scene input is refused, geometry → `Dot` → geometry consumer resolves; Xvfb GUI screenshot shows the new edge style.
  - size: M

- [x] M54.P1.T2 — Paged copy-on-write attribute storage
  - files: `Engine/Geo/GeoAttribute.h`/`.cpp`, `Engine/Geo/GeoPage.h`, `Tests/GeoAttribute_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: Houdini GA-style columns with no pxr types. An attribute is a name, a storage type (int32, int64, float32, float64, string), a tuple size 1–16, a type-info tag (none, vector, normal, point, colour, matrix, quaternion) and an array flag, stored as a page table of `shared_ptr<const Page>`. A page holds 1024 elements (Scope assumption). A page whose elements all share one value stores that value once, as a constant page. Writing an element detaches only its page. Strings are stored as indices into a shared per-attribute string table. Array attributes store per-element offsets into a paged value pool. Expose typed per-element get/set and a page visitor. Also expose `copyDense(range)`/`scatterDense(range)` for consumers that need contiguous data.
  - verify: unit tests — copying an attribute shares every page; writing 5 elements of a 10M-element attribute detaches exactly the pages containing them (page pointers compared); a constant page stays one value until written; round-trips for every storage type, tuple size and array attribute.
  - size: L

- [x] M54.P1.T5 — Element index maps with tombstones and defragment
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

Moved to other F15 - Native Geometry members on 2026-11-08 (IDs not reused here):
- M54.P1.T8 → M361.P2.T2
- M54.P2.T1 → M358.P1.T1
- M54.P2.T2 → M358.P1.T3
- M54.P2.T3 → M358.P1.T4
- M54.P2.T4 → M358.P2.T1
- M54.P2.T5 → M358.P1.T2
- M54.P2.T6 → M358.P1.T5
- M54.P2.T7 → M358.P2.T2
- M54.P3.T2 → M359.P1.T1
- M54.P3.T3 → M359.P1.T2
- M54.P3.T4 → M359.P1.T3
- M54.P3.T5 → M359.P1.T4
- M54.P3.T6 → M359.P1.T5
- M54.P3.T7 → M359.P1.T6
- M54.P3.T8 → M359.P1.T7
- M54.P3.T9 → M359.P1.T8
- M54.P3.T10 → M359.P1.T9
- M54.P3.T11 → M359.P2.T1
- M54.P3.T12 → M359.P1.T10
- M54.P3.T13 → M359.P1.T11
- M54.P4.T1 → M361.P1.T1
- M54.P4.T2 → M361.P1.T2
- M54.P5.T1 → M361.P2.T1
- M54.P6.T1 → M360.P2.T1
- M54.P6.T2 → M360.P3.T1
- M54.P6.T3 → M360.P4.T1

**Verification gate:** CI green, with every Phase 54.1 test passing; the page-sharing assertions hold (M54.P1.T2, M54.P1.T6); the pre-existing ctest suite is green; `Engine/GeoDetail.h` and `Engine/Geo/` include no pxr header.

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
- 2026-11-08 — **Split into F15 - Native Geometry (user, /cat-plan):** this file keeps the data-model member and is renamed M54 - Native Geometry Data Model. Bridges and display moved to M358 - Geometry Bridges And Display, operators and the Python wrangle to M359 - Geometry Operators, the kernel language to M360 - Geometry Kernel Language, and point editing, integration and the scale bench to M361 - Geometry Point Editing. The bench moved from this member to M361 because the chain it times (Transform → Edit → Delete → GeoToUSD) only exists once the other members land. M337 - Geometry Spreadsheet joined the Feature (user).
