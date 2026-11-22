# M540 - Sparse Tile Field Core

The data structure every pyro kernel runs on: a sparse grid of dense 8³ tiles (one OpenVDB leaf each), holding scalar fields and a MAC-staggered velocity field, mirrored on the GPU, converted to and from M417 - VDB Volume Primitive grids, and regrown each step from the smoke's extent. M541 - Pyro Advection And Pressure builds the solver kernels on it; M542 - PyroSolver Node And Sourcing exposes its Bounds controls.

Member of F35 - Pyro: branches off and merges into `feature/f35-pyro` (PLAN-FORMAT §9b); the Feature's UAT covers it. Epic: E22 - Simulation And Solvers. Shared design: `PLAN/DECISIONS/2026-11-22-vdb-pyro-solver.md`.

## Scope

**In:**
- `SparseTileField`: a tile table (tile coordinate → slot), dense per-slot storage, named scalar and vector fields sharing one topology, a velocity field on its own voxel size (the Velocity Voxel Scale knob), a 26-neighbour slot table rebuilt per topology change, trilinear and monotone cubic sampling across tile borders, parallel per-tile iteration.
- VDB conversion both ways by leaf copy when transforms align, resampling otherwise.
- Active-region rebuild with every Bounds control: Limit Maximum Size, Size, Center, Padding, Reference Fields, Extra Fields, Resize in Full Tiles, Extrapolate Velocity into New Tiles, Falloff, Blendwidth, Expand by Velocity, Min Padding, Max Padding, Expansion Rate, Tangential Rate; Dense mode activates every tile inside the bounds.
- GPU mirror: device buffers from `GpuBufferPool`, an uploaded tile table and neighbour table, a Slang sampling module, and a scan/compaction kernel for activation flags.

**Out:**
- Advection, pressure and forces (M541 - Pyro Advection And Pressure).
- Node knobs (M542 - PyroSolver Node And Sourcing wires the Bounds tab).
- Adaptive (multi-resolution) tiles (none).

**Edge cases:**
- A source grid with a different voxel size or transform: resampled into the field's transform; leaf copy only when they match.
- An empty active set (no smoke yet): valid; every kernel is a no-op.
- Topology grows past the VRAM budget: the field reports it and the solver falls back to the CPU path for that frame (no size cap; memory: no hand-holding size caps).
- Tiles leaving the active set: their values drop; tiles entering it take each field's background value, with velocity extrapolated when that option is on.

**Assumptions:**
- Unconfirmed: 8³ tiles keep GPU occupancy acceptable (512 threads per tile); a 16³ variant is not planned.

## Phase 540.1: CPU field

- [ ] M540.P1.T1 — `SparseTileField` storage, topology and sampling
  - files: `Engine/Geo/Sparse/SparseTileField.{h,cpp}`, `Engine/CMakeLists.txt`, `Tests/Native/SparseTileField_Test.cpp`
  - approach: Hash map from tile coordinate to slot plus dense slot-ordered arrays per field; MAC velocity as three face-centred components on the velocity voxel size; neighbour slot table (−1 for inactive) rebuilt on topology change; trilinear and monotone cubic samplers that cross tile borders through the neighbour table; `forEachTile` over the engine thread pool. Bundled gtest is old: no `GTEST_SKIP`, no `ASSERT_TRUE(ptr)`.
  - verify: `SparseTileField_Test`: a linear ramp sampled anywhere, across tile borders, returns the exact ramp value (trilinear) and within 1e-5 (cubic); removing a tile makes its samples the background; results are identical at 1 and 8 threads.
  - size: L
- [ ] M540.P1.T2 — VDB conversion
  - files: `Engine/Geo/Sparse/SparseTileVDB.{h,cpp}`, `Tests/Native/SparseTileVDB_Test.cpp`
  - approach: `FloatGrid`/`Vec3fGrid` leaves copy straight into slots when the grid transform matches the field's; otherwise resample with `openvdb::tools::resampleToMatch` first. Output builds one grid per named field, staggered velocity written as a `Vec3fGrid` with the staggered class set, and returns VDB primitives (M417 - VDB Volume Primitive).
  - verify: `SparseTileVDB_Test`: a sphere fog grid round-trips with an identical content hash; a grid at double the voxel size lands within interpolation tolerance; velocity round-trips with its grid class.
  - size: M

## Phase 540.2: Active region

- [ ] M540.P2.T1 — Active-region rebuild with the resizing rules
  - files: `Engine/Geo/Sparse/SparseTileResize.{h,cpp}`, `Tests/Native/SparseTileResize_Test.cpp`
  - approach: From reference fields above a threshold, add padding (fixed, or expanded by velocity between min and max padding at the expansion and tangential rates), optionally round to full tiles, clip to the maximum size box (size, center), add extra fields' tiles; Dense mode activates every tile in the box. New tiles get backgrounds, extrapolated velocity (fast-sweep from active neighbours) and the falloff/blendwidth blend toward the background velocity.
  - verify: `SparseTileResize_Test`: a moving blob's active set follows it frame to frame and never exceeds the size box; expand-by-velocity grows padding ahead of motion only; extrapolated velocity in a new tile equals its neighbour's within blend tolerance.
  - size: M

## Phase 540.3: GPU mirror

- [ ] M540.P3.T1 — Device-side tile field, sampling module and compaction
  - files: `Engine/Geo/Sparse/SparseTileFieldGpu.{h,cpp}`, `Engine/Gpu/Kernels/sparse_tiles.slang`, `Engine/Gpu/Kernels/sparse_tiles.cmake`, `Engine/Gpu/Kernels/scan_compact.slang`, `Engine/Gpu/Kernels/scan_compact.cmake`, `Tests/Gpu/SparseTileFieldGpu_Test.cpp`
  - approach: Upload slot arrays, the sorted tile-coordinate table and neighbour table into `GpuBufferPool` buffers; a Slang module with the same samplers as the CPU; a work-efficient scan and compaction kernel for activation flags (none exists under `Engine/Gpu/`); kernels registered through `slang_add_kernel` as `Kernels/blur.cmake` does and recorded via `GpuRecordContext`.
  - verify: `SparseTileFieldGpu_Test` on lavapipe: sampling a random field at random points matches the CPU within 1e-5 relative; compaction of a random flag array matches a CPU reference exactly.
  - size: L

**Verification gate:** full ctest green including `SparseTileField_Test`, `SparseTileVDB_Test`, `SparseTileResize_Test` and `SparseTileFieldGpu_Test`.
