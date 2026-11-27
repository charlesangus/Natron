# M417 - VDB Volume Primitive

An OpenVDB volume primitive in native geometry, as Houdini's VDB primitive: `eGeoPrimitiveVDB` holds a shared, immutable `openvdb::GridBase` with a name, so SOP-style volume work happens in the geometry context. OpenVDB is already in the image as a USD dependency (`cmake/usd-image-deps/OpenVDBConfig.cmake`); the no-pxr rule for native geometry doesn't cover OpenVDB types. `GeoToUSD` hands volumes to renderers as content-hashed `.vdb` files in the session cache, authoring a `UsdVolVolume` with `OpenVDBAsset` fields (user, 2026-11-12), so Storm, Cycles and MoonRay read them unchanged. Native VDB ops are M418 - VDB Conversion Ops and M419 - VDB SDF Ops And Wrangle.

Member of F22 - Native VDB: branches off and merges into `feature/f22-native-vdb` (PLAN-FORMAT §9b); the Feature's UAT covers it. Epic: E9 - USD And 3D.

## Scope

**In:**
- Link OpenVDB into `Engine` from the image's install (already found for USD), with `openvdb::initialize()` once at startup.
- `eGeoPrimitiveVDB`: one point (the volume's position, as in Houdini), a grid pointer shared copy-on-write, grid name from the `name` primitive attribute, transform from the grid.
- Detail hashing includes a per-grid content hash (computed once and cached on the primitive).
- `GeoVDBRead` node: reads `.vdb` files (sequences, grid selection by name) into VDB primitives. (`USDVolume`, M346 in F1 - VDB Volumes, stays the scene-side reader.)
- `GeoToUSD`: VDB prims → content-hashed `.vdb` files under the session cache dir and `UsdVolVolume` + `OpenVDBAsset` fields; an unchanged grid reuses its file. `USDToGeo`: `Volume` prims with file-backed fields → VDB primitives.
- Display: the geometry display shows VDB prims through the bridge (Storm draws volumes) plus a bounding-box guide; points/numbers display treat a VDB prim as one point.
- `GeoFileFormat` (M403 - Geometry File Cache) stores VDB prims by embedding the grid via OpenVDB's stream I/O — whichever of the two lands second adds it.

**Out:**
- VDB ops (M418 - VDB Conversion Ops, M419 - VDB SDF Ops And Wrangle).
- In-memory Hydra field plugin (user chose files).
- Dense (Houdini native) volumes (none).
- Fixing delegate volume rendering bugs (M345 - VDB Rendering Fixes, F1 - VDB Volumes).

**Edge cases:**
- Session cache growth: files are content-addressed and pruned with the session cache's existing eviction; never capped by size (memory: no hand-holding size caps).
- Grids with unsupported value types for a delegate: written anyway; the delegate's own warning stands.
- Two VDB prims with the same name: the second starts the next Volume prim (`volume_1`), with an info message (decision 2026-11-25).
- Cycles drawing nothing for unbound volumes until M345 - VDB Rendering Fixes lands: known; Storm is the reference for this milestone's tests.

**Assumptions:**
- Unconfirmed: the image's OpenVDB build is ABI-compatible with linking directly from `Engine` (it was built for USD's `hioOpenVDB`).

## Phase 417.1: Primitive

- [x] M417.P1.T1 — Link OpenVDB and add the VDB primitive type
  - files: `Engine/CMakeLists.txt`, `cmake/NatronUsd.cmake`, `Engine/Geo/GeoTopology.h`, `Engine/Geo/GeoVDB.{h,cpp}`, `Engine/GeoDetail.{h,cpp}`, `Tests/GeoVDB_Test.cpp`
  - approach: `find_package(OpenVDB)` against the image's install (the config USD already uses), link to Engine, initialise once in app startup. Add the primitive type holding a `std::shared_ptr<const openvdb::GridBase>`, builder API to add and replace grids (replacing creates a new pointer, never mutates), and cached per-grid content hash (hash of active voxel topology and values, computed by a parallel tree walk).
  - verify: `GeoVDB_Test`: a sphere SDF grid added to a detail survives copy-on-write sharing (same pointer in a copy, new pointer after replace); the detail hash changes when a voxel changes.
  - size: L

## Phase 417.2: I/O and bridges

- [x] M417.P2.T1 — `GeoVDBRead` and the USD bridges
  - files: `Engine/Nodes/Geometry/GeoVDBRead.{h,cpp,reg.inc}`, `Engine/Nodes/Geometry/GeoUsdConvert.cpp`, `Engine/Nodes/Geometry/GeoSdfData.cpp`, `Tests/GeoVDBRead_Test.cpp`, `Tests/GeoToUSD_Test.cpp` (or M358's bridge test)
  - approach: Reader follows `GeoImportOBJ`/`GeoFile` file handling. Bridge: a VDB partition writes each grid to `<session cache>/vdb/<hash>.vdb` if absent and authors `UsdVolVolume` with `OpenVDBAsset` fields (`filePath`, `fieldName`, `fieldDataType`, `vectorDataRoleHint`) under the partition path; `USDToGeo` reads file-backed `OpenVDBAsset` fields into prims.
  - verify: `GeoVDBRead_Test` reads a fixture `.vdb` (small, synthetic, under the LFS budget) and lists its grids; the bridge test round-trips a grid through `GeoToUSD` and `USDToGeo` with an identical content hash and checks the same file is reused on a second export.
  - size: M

## Phase 417.3: Proof

- [x] M417.P3.T1 — GUI check
  - files: `Tests/gui/m417_vdb_primitive.py`
  - approach: `GeoVDBRead` → `GeoToUSD` → viewer and `USDRender` (Storm); check the volume draws and the bounding-box guide shows.
  - verify: the script passes under Xvfb; a screenshot sent with SendUserFile for the Feature's UAT.
  - size: M

**Verification gate:** full ctest green including `GeoVDB_Test`, `GeoVDBRead_Test` and the bridge test; `Tests/gui/m417_vdb_primitive.py` passes under Xvfb.

## Decisions

- 2026-11-26 (PM): known limitation: GeoToUSD's Unpack mode writes one VDB file per copy (each copy's grid carries its own transform). Instance mode writes one file per prototype. Sharing files across unpacked copies needs per-field prim transforms that every delegate honours; not in M417.

- 2026-11-26 (PM): the Cycles fixes M417's own copies check needed move into M417 and come out of M345's scope: NanoVDB in the Cycles build, the default volume shader for unbound volumes, and the Finalize crash (fork natron/m417-default-volume-shader, built on M616's instancer fix). Merge M616 (#393) before M417 (#377).

- 2026-11-12 — **Volumes reach renderers as content-hashed `.vdb` files (user, /cat-plan grill),** not an in-memory Hydra field plugin: works with Storm, Cycles and MoonRay unchanged.
- 2026-11-12 — **Ships as F22 - Native VDB with M418 - VDB Conversion Ops and M419 - VDB SDF Ops And Wrangle (user).**

Shared design: `PLAN/DECISIONS/2026-11-12-geometry-simulation-expansion.md`.

- 2026-11-25 (user + PM research): volume export follows Solaris SOP Import.
  - **Grouping:** fields gather under one Volume prim. A repeated field name starts a new Volume (`volume_1`) rather than taking a `_1` suffix, and a path attribute can place a Volume.
  - **Files:** all of a Volume's grids go into one .vdb, named by the content hash of its grid set. MoonRay needs one file per volume. Fields are referenced by `filePath` + `fieldName`, with `fieldIndex` only for same-named grids.
  - **Copies:** CopyToPoints/packed volume copies export as one Volume prototype under a UsdGeomPointInstancer, so there is one file per prototype. User: "prefer instancing, fall back to a Volume prim per copy if not feasible or extensive upstream work". The research found that Storm (shader), Cycles (`geometry.inl` instancer path) and hdMoonray (`Volume.cc` `_UpdateInstancer`) all apply instancer transforms to volumes. MoonRay gives instanced volumes no transform motion blur; document that.
  - **Task:** M417.P9.T1 below.

- [ ] M417.P9.T1 — Solaris-style volume export: grouping, one file per Volume, instanced copies
  - files: Engine/Nodes/Geometry/GeoUsdConvert.cpp, GeoCopyToPoints.cpp, Engine/Geo/GeoVDB.cpp, Tests/Scene/GeoVDBBridge_Test.cpp
  - approach: per the decision above; a multi-copy fixture rendered in Storm, Cycles and MoonRay, plus a 500-copy case writing one file
  - verify: ctest GeoVDBBridge + GeoVDB; GUI or headless render check that every copy draws
  - size: L
