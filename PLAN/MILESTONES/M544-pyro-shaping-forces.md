# M544 - Pyro Shaping Forces

`PyroSolver`'s Shape tab: disturbance, turbulence and shredding forces on a shared noise library with continuous and block-based modes, threshold and control fields, the hourglass filtering knobs, and the Shape Guides overlays.

Member of F35 - Pyro: branches off and merges into `feature/f35-pyro` (PLAN-FORMAT §9b); the Feature's UAT covers it. Epic: E22 - Simulation And Solvers. Shared design: `PLAN/DECISIONS/2026-11-22-vdb-pyro-solver.md`.

## Scope

**In:**
- Disturbance: Disturbance, Mode (Continuous, Block-Based), Reference Scale, Base Block Size, Roughness, Pulse Length, Max Octaves, Lacunarity, Threshold Field, Threshold Range, Use Control Field, Control Field, Control Range, Compute Range, Remap Control Field.
- Turbulence: Turbulence, Swirl Size, Roughness, Pulse Length, Max Octaves, Seed, Threshold Field, Influence Range, Use As Range, Use Control Field, Control Field, Control Range, Compute Range, Remap Control Field.
- Shredding: Shredding, Mode, Base Block Size, Roughness, Pulse Length, Max Octaves, Lacunarity, Threshold Field, Threshold Range, Use Control Field, Control Field, Control Range, Compute Range, Remap Control Field.
- Advanced Hourglass Filtering knobs (Amount, Scale By Divergence, Use Relative Divergence, Divergence Scale) driving M541 - Pyro Advection And Pressure's filter.
- Shape Guides: Shape Guide (No Guide, Disturbance, Turbulence, Shredding, Hourglass Filtering), Visualize As (3D, Plane), Plane Orientation, Plane Position, Guide Range, Compute Range, Divisions, through M543 - Pyro Fields And Combustion's guide builder.

**Out:**
- Wind and buoyancy (M542 - PyroSolver Node And Sourcing).

**Edge cases:**
- Block-based mode at a block size below one voxel: clamped to one voxel, with an info message.
- Seed and time: the noise is a function of world position, seed and time, so results are identical across thread counts, tile layouts and CPU/GPU within tolerance.

## Phase 544.1: Noise and forces

- [ ] M544.P1.T1 — Shaping noise library on CPU and GPU
  - files: `Engine/Geo/Pyro/PyroNoise.{h,cpp}`, `Engine/Gpu/Kernels/pyro_noise.slang`, `Engine/Gpu/Kernels/pyro_noise.cmake`, `Tests/Native/PyroNoise_Test.cpp`, `Tests/Gpu/PyroNoiseGpu_Test.cpp`
  - approach: Fractal curl noise (octaves, roughness, lacunarity, pulse length as time evolution, seed) and block-based random vectors per block size; continuous and block modes evaluated per voxel from world position. Same integer hashing on CPU and Slang. Bundled gtest is old: no `GTEST_SKIP`, no `ASSERT_TRUE(ptr)`.
  - verify: `PyroNoise_Test`: curl noise is divergence-free within discretisation error; output is identical between tile layouts; GPU matches CPU within 1e-5 relative.
  - size: M
- [ ] M544.P1.T2 — Disturbance, turbulence and shredding forces
  - files: `Engine/Geo/Pyro/PyroShape.{h,cpp}`, `Engine/Gpu/Kernels/pyro_forces.slang`, `Engine/Nodes/Pyro/PyroSolver.{h,cpp}`, `Tests/Native/PyroShape_Test.cpp`
  - approach: Disturbance adds noise force gated by a threshold field range; turbulence adds curl noise scaled by influence range; shredding stretches velocity along its gradient in high-temperature regions; each weighted by M543's control-field helper. Wired into M542's force stage. Knob labels match script names.
  - verify: `PyroShape_Test`: disturbance 0 leaves the sim identical to off; a threshold range excludes cells outside it; turbulence raises vorticity magnitude in the influence range only.
  - size: M

## Phase 544.2: Guides and filter knobs

- [ ] M544.P2.T1 — Hourglass Filtering knobs and Shape Guides
  - files: `Engine/Nodes/Pyro/PyroSolver.{h,cpp}`, `Engine/Nodes/Pyro/PyroGuides.{h,cpp}`, `Tests/Native/PyroGuides_Test.cpp`, `Engine/Nodes/Pyro/docs/PyroSolver.md`
  - approach: Wire the four hourglass knobs to M541's filter; Shape Guides draw the force magnitude of the chosen shape force through the guide builder. Docs for the Shape tab written fresh.
  - verify: `PyroGuides_Test`: the Turbulence plane guide is zero outside the influence range.
  - size: M
- [ ] M544.P2.T2 — GUI check: shaped smoke
  - files: `Tests/gui/m544_pyro_shape.py`
  - approach: M542's plume with disturbance (block-based), turbulence and shredding on, with the Turbulence shape guide.
  - verify: the script passes under Xvfb; screenshots sent with SendUserFile for the Feature's UAT.
  - size: M

**Verification gate:** full ctest green including `PyroNoise_Test`, `PyroNoiseGpu_Test`, `PyroShape_Test` and `PyroGuides_Test`; `Tests/gui/m544_pyro_shape.py` passes under Xvfb.
