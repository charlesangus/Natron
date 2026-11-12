# M419 - VDB SDF Ops And Wrangle

The VDB SOPs that edit and analyse volumes, plus a `VolumeWrangle` (user, 2026-11-12) that runs M360 - Geometry Kernel Language snippets per active voxel through a new volume binding. M360 kept volume wrangles out but left its front end and runner behind a binding interface for this. Nodes: `VDBSmoothSDF`, `VDBCombine`, `VDBReshapeSDF`, `VDBAnalysis` and `VolumeWrangle`.

Member of F22 - Native VDB: branches off and merges into `feature/f22-native-vdb` (PLAN-FORMAT §9b); the Feature's UAT covers it. Epic: E9 - USD And 3D.

## Scope

**In:**
- `VDBSmoothSDF`: mean, gaussian, median and mean-curvature flow filters with iterations and width (OpenVDB `LevelSetFilter`), optional mask.
- `VDBCombine`: SDF union/intersection/difference (`csgUnion` etc.), and value ops add/subtract/multiply/max/min/replace/blend, with resampling when transforms differ.
- `VDBReshapeSDF`: dilate, erode, open, close, offset (`LevelSetFilter` offset, morphology).
- `VDBAnalysis`: gradient, curvature, Laplacian, closest-point, magnitude, normalise, as new grids.
- `VolumeWrangle`: a volume binding for the kernel language that runs per active voxel of a chosen grid (or a union of named grids), with `@density`-style grid access by name, `@P`, `@ix/@iy/@iz`, `volumesample`, `volumegradient`, `volumeindex`, second-input volume reads, and `ch()` knobs. CPU only.

**Out:**
- GPU volume wrangle (none; M408 - Kernel Language GPU Backend covers geometry only).
- Volume rendering or shading (F1 - VDB Volumes covers scene-side materials).
- Sparse-to-dense volume ops (none).

**Edge cases:**
- Combining grids with different transforms: the B grid is resampled into A's transform first, as the SOP does.
- Writing a grid the wrangle didn't bind: a compile error naming the grid; `VolumeWrangle` never creates grids implicitly unless "Create missing grids" is on.
- Voxels outside the active set: untouched; activation follows the SOP's "Activate" options (none / by value).
- Thread-count determinism: voxels are independent, so results are identical across thread counts.

**Assumptions:**
- Unconfirmed: the binding iterates leaf nodes in parallel through `openvdb::tree::LeafManager`, batching leaf voxels like M360's page batches.

## Phase 419.1: SDF ops

- [ ] M419.P1.T1 — `VDBSmoothSDF` and `VDBReshapeSDF`
  - files: `Engine/Nodes/Geometry/VDBSmoothSDF.{h,cpp,reg.inc}`, `Engine/Nodes/Geometry/VDBReshapeSDF.{h,cpp,reg.inc}`, `Tests/VDBSdfOps_Test.cpp`
  - approach: Thin nodes over OpenVDB `LevelSetFilter` (smooth filters, offset) and morphology, copying the grid before editing (copy-on-write), with group/name selection of grids.
  - verify: `VDBSdfOps_Test`: offset by +0.1 grows a sphere's radius by 0.1 within a voxel; mean-curvature smoothing reduces surface noise variance.
  - size: M
- [ ] M419.P1.T2 — `VDBCombine` and `VDBAnalysis`
  - files: `Engine/Nodes/Geometry/VDBCombine.{h,cpp,reg.inc}`, `Engine/Nodes/Geometry/VDBAnalysis.{h,cpp,reg.inc}`, `Tests/VDBSdfOps_Test.cpp`
  - approach: Combine by name-matching or flattening, with resample-to-match for differing transforms; analysis through `tools::Gradient`, `MeanCurvature`, `Laplacian`, `ClosestPointTransform`, writing new named grids.
  - verify: `VDBSdfOps_Test`: union of two overlapping spheres is ≤ both SDFs everywhere; difference removes the overlap; the gradient of a sphere SDF is unit length near the surface.
  - size: M

## Phase 419.2: Volume wrangle

- [ ] M419.P2.T1 — Volume binding for the kernel language and `VolumeWrangle`
  - files: `Engine/Geo/GeoVolumeKernelBinding.{h,cpp}`, `Engine/Nodes/Geometry/VolumeWrangle.{h,cpp,reg.inc}`, `Tests/VolumeWrangle_Test.cpp`
  - approach: Implement M360's binding interface over VDB grids: bind named grids as `@name`, provide `@P`/index variables, `volumesample`/`volumegradient`/`volumeindex` on input grids, iterate active voxels per leaf in parallel and write into a copied grid. Reuse `GeoWrangle`'s node UI (`ch()`, Create Parameters).
  - verify: `VolumeWrangle_Test`: `@density *= 2;` doubles every active voxel; `@density = volumesample(1, "density", @P);` copies a second input's grid; an unbound write errors with the grid name; results are identical at 1 and 8 threads.
  - size: L

## Phase 419.3: Proof

- [ ] M419.P3.T1 — GUI check and docs
  - files: `Tests/gui/m419_vdb_sdf_ops.py`, `Engine/Nodes/Geometry/docs/*.md`
  - approach: Two meshes → SDFs → combine, smooth, reshape → `ConvertVDB` to polygons; a `VolumeWrangle` noise on a fog volume rendered in Storm. Docs written fresh.
  - verify: the script passes under Xvfb; screenshots sent with SendUserFile for the Feature's UAT.
  - size: M

**Verification gate:** full ctest green including `VDBSdfOps_Test` and `VolumeWrangle_Test`; `Tests/gui/m419_vdb_sdf_ops.py` passes under Xvfb.

## Decisions

- 2026-11-12 — **`VolumeWrangle` ships with the VDB ops (user, /cat-plan grill),** through a volume binding on M360 - Geometry Kernel Language's core.

Shared design: `PLAN/DECISIONS/2026-11-12-geometry-simulation-expansion.md`.
