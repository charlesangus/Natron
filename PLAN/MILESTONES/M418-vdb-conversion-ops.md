# M418 - VDB Conversion Ops

The VDB SOPs that move between surfaces, points and volumes, on M417 - VDB Volume Primitive and OpenVDB's tools: `VDBFromPolygons` (SDF or fog, plus attribute grids), `VDBFromParticles` (SDF from points with `pscale`, with trails), `ConvertVDB` (VDB to polygons, and SDF to fog), and `VDBResample`. `VDBFromPolygons`' core is a library function that M407 - Particle Collisions calls to build collider SDFs.

Member of F22 - Native VDB: branches off and merges into `feature/f22-native-vdb` (PLAN-FORMAT §9b); the Feature's UAT covers it. Epic: E9 - USD And 3D.

## Scope

**In:**
- `VDBFromPolygons`: voxel size (or count along an axis), exterior/interior band widths, SDF or fog output, fill interior, and point/vertex/prim attributes rasterised into extra grids (OpenVDB `meshToVolume` with an index grid).
- `VDBFromParticles`: SDF from spheres of radius `pscale` × scale (OpenVDB `ParticlesToLevelSet`), minimum radius, velocity trails, optional attribute grids.
- `ConvertVDB`: VDB → polygons (`volumeToMesh`, isovalue, adaptivity, optional reference surface attribute transfer), SDF → fog volume, fog → SDF.
- `VDBResample`: to a voxel size, a reference VDB's transform, or a scale, with linear or quadratic interpolation.
- A shared `GeoVDBConvert` library (`polygonsToSdf`, `pointsToSdf`, `sdfToPolygons`) used by these nodes and M407.

**Out:**
- SDF filters, CSG, reshape, analysis, `VolumeWrangle` (M419 - VDB SDF Ops And Wrangle).
- Non-VDB dense volumes (none).

**Edge cases:**
- Open meshes into SDF: unsigned distance with a band, and an info message suggesting fog or "fill interior" off.
- Very small voxel size: no cap; memory estimate shown in the node's info (memory: no hand-holding size caps).
- Particles smaller than the minimum radius: skipped and counted in the info message, as OpenVDB does.
- Empty inputs: empty grid out, no error.

**Assumptions:**
- Unconfirmed: OpenVDB's tools headers (`openvdb/tools/*.h`) are installed in the image along with the library.

## Phase 418.1: Conversion library

- [ ] M418.P1.T1 — `GeoVDBConvert` library
  - files: `Engine/Geo/GeoVDBConvert.{h,cpp}`, `Tests/GeoVDBConvert_Test.cpp`
  - approach: Wrap `meshToVolume` (from the detail's triangulated polygons, with an optional polygon-index grid), `ParticlesToLevelSet` (a point-list adapter over `P`/`pscale`/`v`), `volumeToMesh` and SDF/fog conversion, taking and returning native types (`GeoDetail` and VDB primitives).
  - verify: `GeoVDBConvert_Test`: a unit sphere mesh → SDF has distance ≈ −1 at the centre and ≈ 0 on the surface; SDF → polygons gives a mesh within one voxel of the sphere; one particle of `pscale` 0.5 gives a 0.5-radius sphere.
  - size: M

## Phase 418.2: Nodes

- [ ] M418.P2.T1 — `VDBFromPolygons` and `VDBFromParticles`
  - files: `Engine/Nodes/Geometry/VDBFromPolygons.{h,cpp,reg.inc}`, `Engine/Nodes/Geometry/VDBFromParticles.{h,cpp,reg.inc}`, `Tests/VDBFrom_Test.cpp`
  - approach: Nodes over the library, with the SOPs' knob sets and attribute-grid options; output VDB prims named from knobs.
  - verify: `VDBFrom_Test`: attribute `Cd` from a coloured mesh lands in a `Cd` grid with matching values near the surface; particle trails stretch the SDF along `v`.
  - size: M
- [ ] M418.P2.T2 — `ConvertVDB` and `VDBResample`
  - files: `Engine/Nodes/Geometry/ConvertVDB.{h,cpp,reg.inc}`, `Engine/Nodes/Geometry/VDBResample.{h,cpp,reg.inc}`, `Tests/ConvertVDB_Test.cpp`
  - approach: ConvertVDB modes Polygons / Fog / SDF with isovalue and adaptivity; resample via OpenVDB `GridTransformer` / `resampleToMatch`.
  - verify: `ConvertVDB_Test`: adaptivity 0.5 gives fewer polygons than 0 with the same bounds; resampling to double voxel size halves the active-voxel count per axis within tolerance.
  - size: M

## Phase 418.3: Proof

- [ ] M418.P3.T1 — GUI check and docs
  - files: `Tests/gui/m418_vdb_conversion.py`, `Engine/Nodes/Geometry/docs/*.md`
  - approach: Mesh → SDF → polygons round trip and particles → SDF → polygons in the viewer. Docs written fresh.
  - verify: the script passes under Xvfb; screenshots sent with SendUserFile for the Feature's UAT.
  - size: M

**Verification gate:** full ctest green including `GeoVDBConvert_Test`, `VDBFrom_Test` and `ConvertVDB_Test`; `Tests/gui/m418_vdb_conversion.py` passes under Xvfb.

## Decisions

- 2026-11-12 — **Basic VDB op set split on sizing (/cat-plan):** conversion here, SDF ops and the wrangle in M419 - VDB SDF Ops And Wrangle.

Shared design: `PLAN/DECISIONS/2026-11-12-geometry-simulation-expansion.md`.
