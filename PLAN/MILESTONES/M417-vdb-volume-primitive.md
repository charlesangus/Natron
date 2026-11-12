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
- Two VDB prims with the same name: both exported, the second's USD field name suffixed, with an info message.
- Cycles drawing nothing for unbound volumes until M345 - VDB Rendering Fixes lands: known; Storm is the reference for this milestone's tests.

**Assumptions:**
- Unconfirmed: the image's OpenVDB build is ABI-compatible with linking directly from `Engine` (it was built for USD's `hioOpenVDB`).

## Phase 417.1: Primitive

- [ ] M417.P1.T1 — Link OpenVDB and add the VDB primitive type
  - files: `Engine/CMakeLists.txt`, `cmake/NatronUsd.cmake`, `Engine/Geo/GeoTopology.h`, `Engine/Geo/GeoVDB.{h,cpp}`, `Engine/GeoDetail.{h,cpp}`, `Tests/GeoVDB_Test.cpp`
  - approach: `find_package(OpenVDB)` against the image's install (the config USD already uses), link to Engine, initialise once in app startup. Add the primitive type holding a `std::shared_ptr<const openvdb::GridBase>`, builder API to add and replace grids (replacing creates a new pointer, never mutates), and cached per-grid content hash (hash of active voxel topology and values, computed by a parallel tree walk).
  - verify: `GeoVDB_Test`: a sphere SDF grid added to a detail survives copy-on-write sharing (same pointer in a copy, new pointer after replace); the detail hash changes when a voxel changes.
  - size: L

## Phase 417.2: I/O and bridges

- [ ] M417.P2.T1 — `GeoVDBRead` and the USD bridges
  - files: `Engine/Nodes/Geometry/GeoVDBRead.{h,cpp,reg.inc}`, `Engine/Nodes/Geometry/GeoUsdConvert.cpp`, `Engine/Nodes/Geometry/GeoSdfData.cpp`, `Tests/GeoVDBRead_Test.cpp`, `Tests/GeoToUSD_Test.cpp` (or M358's bridge test)
  - approach: Reader follows `GeoImportOBJ`/`GeoFile` file handling. Bridge: a VDB partition writes each grid to `<session cache>/vdb/<hash>.vdb` if absent and authors `UsdVolVolume` with `OpenVDBAsset` fields (`filePath`, `fieldName`, `fieldDataType`, `vectorDataRoleHint`) under the partition path; `USDToGeo` reads file-backed `OpenVDBAsset` fields into prims.
  - verify: `GeoVDBRead_Test` reads a fixture `.vdb` (small, synthetic, under the LFS budget) and lists its grids; the bridge test round-trips a grid through `GeoToUSD` and `USDToGeo` with an identical content hash and checks the same file is reused on a second export.
  - size: M

## Phase 417.3: Proof

- [ ] M417.P3.T1 — GUI check
  - files: `Tests/gui/m417_vdb_primitive.py`
  - approach: `GeoVDBRead` → `GeoToUSD` → viewer and `USDRender` (Storm); check the volume draws and the bounding-box guide shows.
  - verify: the script passes under Xvfb; a screenshot sent with SendUserFile for the Feature's UAT.
  - size: M

**Verification gate:** full ctest green including `GeoVDB_Test`, `GeoVDBRead_Test` and the bridge test; `Tests/gui/m417_vdb_primitive.py` passes under Xvfb.

## Decisions

- 2026-11-12 — **Volumes reach renderers as content-hashed `.vdb` files (user, /cat-plan grill),** not an in-memory Hydra field plugin: works with Storm, Cycles and MoonRay unchanged.
- 2026-11-12 — **Ships as F22 - Native VDB with M418 - VDB Conversion Ops and M419 - VDB SDF Ops And Wrangle (user).**

Shared design: `PLAN/DECISIONS/2026-11-12-geometry-simulation-expansion.md`.
