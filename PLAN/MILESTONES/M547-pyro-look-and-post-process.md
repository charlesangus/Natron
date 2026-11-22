# M547 - Pyro Look And Post-Process

Make the sim renderable: `PyroBakeVolume` bakes smoke colour, scatter and fire emission into render-ready grids and binds a volume material (M347 - Volume Material Node); `PyroPostProcess` prepares the output (cull, resample, 16-bit, combine). `PyroSolver` gets the same controls on its Look tab and Output Post-Process section.

Member of F35 - Pyro: branches off and merges into `feature/f35-pyro` (PLAN-FORMAT §9b); the Feature's UAT covers it. Epic: E22 - Simulation And Solvers. Shared design: `PLAN/DECISIONS/2026-11-22-vdb-pyro-solver.md`.

## Scope

**In:**
- `PyroBakeVolume`: General (Initialize, Assign Material, Max Vis Resolution); Smoke (Enable Smoke, Density Scale, Smoke Color, Density Range, Compute Range, Smoke Color Ramp, Shadow Color, Shadow Density, Ambient Shadow Scale); Scatter (Enable Scatter, Intensity Scale, Hot Core Scale, Color Mode, Source Range, Compute Range, Scatter Color Ramp, Temperature Scale, Temperature At 0 (K), Temperature At 1 (K), Enable Tone Mapping, Adaptation, Burn); Blur (Blur Scatter, Use Blur Steps, Number Of Blur Steps, Blur Step Falloff, Use World Space Radius Units, Filter Voxel Radius, Filter Radius, Blur Iterations, Downsample); both Masking groups (Enable Mask, Mask Center, Mask Width, Use Mask Ramp, Masking Ramp); Fire and Second Fire (Enable, Intensity Scale, Source Range, Compute Range, Fire Intensity Ramp, Color Mode, Fire Color Ramp, Temperature Scale, Temperatures At 0/1 (K), Enable Tone Mapping, Adaptation, Burn); bindings for every input and output volume; Bake Emission Volume and Bake Fire Density.
- `PyroSolver` Look tab: Max Vis Resolution, Smoke, Fire, Assign Material, Bindings, on the same bake core.
- `PyroPostProcess` and `PyroSolver`'s Output Post-Process: Add Wind To Velocity Field, Compute Min/Max Values, Combine Volumes, Use 16bit Float, Cull Volume, Resample Volumes, Voxel Size Scale, Flame Density, Vector VDBs, Cull Mask Volume, Density Volume, Flame Volume.
- Blackbody colour mode for fire and scatter.

**Out:**
- Convert To VDB: output is always VDB (see Decisions).
- Renderer-side volume bugs (M345 - VDB Rendering Fixes).

**Edge cases:**
- Assign Material with no volume material node available: the baked grids still output, with a warning.
- 16-bit float output: half grids; a renderer that reads them as float is unaffected.
- Max Vis Resolution only limits the viewer's preview grids, never the render output.

## Phase 547.1: Bake

- [ ] M547.P1.T1 — Bake core and `PyroBakeVolume`
  - files: `Engine/Geo/Pyro/PyroBake.{h,cpp}`, `Engine/Nodes/Pyro/PyroBakeVolume.{h,cpp,reg.inc}`, `Tests/Native/PyroBake_Test.cpp`
  - approach: Per voxel, map density through range and ramp to smoke colour with shadow tint; scatter from a source field (temperature or flame) with a hot-core boost, blurred by the chosen steps and radius; fire intensity and colour from ramps or blackbody (Kelvin range, tone mapping with adaptation and burn); masks by centre, width and ramp; write `emission`, `Cd` and density grids. Bundled gtest is old: no `GTEST_SKIP`, no `ASSERT_TRUE(ptr)`.
  - verify: `PyroBake_Test`: blackbody at 1500 K gives a red-dominant emission and 6500 K near-white; density scale 2 doubles the density grid; a mask of width 0 at the centre zeroes scatter outside it.
  - size: L
- [ ] M547.P1.T2 — Look tab and material assignment
  - files: `Engine/Nodes/Pyro/PyroSolver.{h,cpp}`, `Engine/Nodes/Pyro/PyroBakeVolume.{h,cpp}`, `Tests/Native/PyroBake_Test.cpp`
  - approach: `PyroSolver`'s Look tab calls the bake core on its output; Assign Material authors a binding to a volume material through M347 - Volume Material Node's API, with the bindings naming the grids the material reads.
  - verify: `PyroBake_Test`: a solver with Look on exports the baked `emission` grid; the bridged USD carries a material binding on the volume prim.
  - size: M

## Phase 547.2: Post-process

- [ ] M547.P2.T1 — `PyroPostProcess` and Output Post-Process
  - files: `Engine/Geo/Pyro/PyroPost.{h,cpp}`, `Engine/Nodes/Pyro/PyroPostProcess.{h,cpp,reg.inc}`, `Engine/Nodes/Pyro/PyroSolver.{h,cpp}`, `Tests/Native/PyroPost_Test.cpp`
  - approach: Cull by a mask volume or below a threshold (prune), resample by voxel size scale, half-float conversion, combine named vector components into vector grids, add wind to velocity, compute and store min/max metadata, and fold flame into density by Flame Density.
  - verify: `PyroPost_Test`: culling a below-threshold region drops its leaves; voxel size scale 2 halves per-axis active voxels within tolerance; 16-bit output round-trips within half precision.
  - size: M

## Phase 547.3: Proof

- [ ] M547.P3.T1 — GUI check: rendered fire and docs
  - files: `Tests/gui/m547_pyro_look.py`, `Engine/Nodes/Pyro/docs/PyroBakeVolume.md`, `Engine/Nodes/Pyro/docs/PyroPostProcess.md`
  - approach: M543's fire through `PyroBakeVolume` with Assign Material → `GeoToUSD` → `USDRender` in Storm and Cycles; docs written fresh, naming no other application.
  - verify: the script passes under Xvfb; renders sent with SendUserFile for the Feature's UAT.
  - size: M

**Verification gate:** full ctest green including `PyroBake_Test` and `PyroPost_Test`; `Tests/gui/m547_pyro_look.py` passes under Xvfb.

## Decisions

- 2026-11-22 — **Convert To VDB dropped (/cat-plan):** native volumes are always VDB, so the toggle has nothing to switch.
