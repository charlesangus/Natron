# M542 - PyroSolver Node And Sourcing

The `PyroSolver` geometry node: M541 - Pyro Advection And Pressure's kernels run per substep inside M404 - Geometry Solver's frame-to-frame core and sim cache. Inputs are source volumes and collision volumes (collision is M545 - Pyro Collisions). This milestone wires the Setup, Bounds, Sourcing, Buoyancy, Wind, External Forces, Solving, Sparse Solve and Export Fields controls, plus a resident Minimal GPU mode. A plume of smoke from a VDB source simulates, caches, scrubs and resets on edit.

Member of F35 - Pyro: branches off and merges into `feature/f35-pyro` (PLAN-FORMAT §9b); the Feature's UAT covers it. Epic: E22 - Simulation And Solvers. Shared design: `PLAN/DECISIONS/2026-11-22-vdb-pyro-solver.md`.

## Scope

**In:**
- Node shell: inputs Volumes For Sourcing and Volumes Or Geometry For Collision; Reset Simulation, Start Frame; a Quick Setups menu placeholder filled by M548 - Pyro Quick Setups And Checkpoints.
- Setup: Voxel Size, Velocity Voxel Scale, Time Scale, Simulation Type (Sparse, Dense, Minimal GPU), Use GPU (Auto, On, Off), Global Substeps, Max Substeps, CFL Condition, Advection-Reflection, Reflection Amount, Cache Simulation, Cache Memory (MB).
- Bounds: every control M540 - Sparse Tile Field Core implements.
- Sourcing: Limit Source Range (Range Type, Frame, Source Frame Range, Cycle Length) and the Source Volumes list (Source Instances, Number Of Sources, Activation, Operation: Copy, Add, Subtract, Multiply, Divide, Maximum, Minimum, Average, Pull, Blend, None; Source Rank, Source Volume, Target Field, Source Weight, Target Weight, Source Scale, Use Timestep, Acceleration Strength, Deceleration Strength, Direction Strength, Use Vector Length, Avoid Negatives).
- Buoyancy (Buoyancy Scale, Ambient Temp, Reference Temp, Gravity Acceleration, Gravity Direction, Enable Density Influences Gravity, Gravity Scale, Density Field, Density Range, Compute Range, Density Ramp, Terminal Velocity), Wind (Wind Speed, Wind Direction), External Forces (Scaled Forces, Absolute Forces).
- Advanced Solving (Minimal GPU Solve, Enable Sparse Solving, Quantize To Max Substeps, Frames Before Solve, Single V-Cycle For Pressure Projection), Sparse Solve (Reset Rule, Fields To Reset); Output Export Fields (Fields To Export, Export, Field, Visualization).
- Output: one named VDB primitive per exported field (`density`, `temperature`, `vel`, …).

**Out:**
- The Fields tab beyond the base fields and their sourcing (M543 - Pyro Fields And Combustion).
- Shape forces (M544 - Pyro Shaping Forces), collisions (M545 - Pyro Collisions), Look and Post-Process (M547 - Pyro Look And Post-Process), checkpoints (M548 - Pyro Quick Setups And Checkpoints).

**Edge cases:**
- Minimal GPU mode: tiles stay resident between frames, no sim cache, Dense only, static bounds, fixed substeps, sources and colliders cycled over their frame range; the other controls that mode can't honour are disabled with a tooltip, not hidden.
- A source volume name that is missing from the input: skipped with one info message naming it.
- Use GPU On with no usable device: falls back to CPU with a warning; Auto follows weight-based placement (M239).
- Editing any knob or input: the sim cache resets after the start frame (M404 - Geometry Solver's rule).
- Viewing a frame before the start frame: empty output.

**Assumptions:**
- Unconfirmed: M404's last-frame slot can hold the resident tile field alongside the cached details, so playback forward skips VDB reconversion.

## Phase 542.1: Node and step

- [ ] M542.P1.T1 — `PyroSolver` node on the solver core with the Setup tab
  - files: `Engine/Nodes/Pyro/PyroSolver.{h,cpp,reg.inc}`, `Engine/Geo/Pyro/PyroStep.{h,cpp}`, `Engine/CMakeLists.txt`, `Tests/Native/PyroSolver_Test.cpp`
  - approach: Build on `GeoSolverBegin`/`End`'s core (M404 - Geometry Solver) the way `ParticleSolverBegin`/`End` (M405) does, but as one node: per substep, source → forces → advect → project (M541), with CFL-driven substeps up to Max Substeps and global substeps outside them; state saved as VDB primitives in the sim cache and kept resident as a tile field for the next frame. Use GPU picks the kernel path. Knob labels match script names.
  - verify: `PyroSolver_Test`: a constant density source with buoyancy rises frame over frame; frames 1–20 cached then frame 10 re-requested does no new step; an edit resets; CPU and GPU frame 10 density agree within tolerance.
  - size: L
- [ ] M542.P1.T2 — Bounds tab and Minimal GPU mode
  - files: `Engine/Nodes/Pyro/PyroSolver.{h,cpp}`, `Tests/Native/PyroSolver_Test.cpp`
  - approach: Wire the Bounds knobs to M540's resize options; Simulation Type selects sparse, dense or the resident Minimal GPU path (no cache, static bounds, fixed substeps, sources cycled over Limit Source Range), disabling knobs it can't honour.
  - verify: `PyroSolver_Test`: Limit Maximum Size clips the active region; Minimal GPU mode plays 20 frames with no sim-cache entries and matches Dense mode within tolerance.
  - size: M

## Phase 542.2: Sourcing and forces

- [ ] M542.P2.T1 — Sourcing tab
  - files: `Engine/Geo/Pyro/PyroSource.{h,cpp}`, `Engine/Nodes/Pyro/PyroSolver.{h,cpp}`, `Tests/Native/PyroSourcing_Test.cpp`
  - approach: A Source Volumes list knob; each entry converts its named input grid into tiles (M540 - Sparse Tile Field Core) and applies its operation to the target field in rank order, scaled by weights and dt when Use Timestep is on; Pull uses acceleration, deceleration and direction strengths on vectors; activation and the source range gate it per frame.
  - verify: `PyroSourcing_Test`: each operation gives its expected value on a constant field; Pull converges velocity toward the source; a source range of 1–5 sources nothing at frame 6; Cycle Length repeats the range.
  - size: M
- [ ] M542.P2.T2 — Buoyancy, Wind and External Forces
  - files: `Engine/Geo/Pyro/PyroForces.{h,cpp}`, `Engine/Gpu/Kernels/pyro_forces.slang`, `Engine/Gpu/Kernels/pyro_forces.cmake`, `Engine/Nodes/Pyro/PyroSolver.{h,cpp}`, `Tests/Native/PyroForces_Test.cpp`
  - approach: Buoyancy from temperature against ambient and reference temperatures, density-weighted gravity through a ramp, terminal-velocity clamp; wind as a background velocity pull; scaled and absolute force fields from named input volumes. CPU and Slang forms.
  - verify: `PyroForces_Test`: hot cells accelerate along −gravity at the buoyancy scale; terminal velocity caps speed; an absolute force field adds a·dt independent of density; GPU matches CPU within 1e-4 relative.
  - size: M

## Phase 542.3: Solving and output

- [ ] M542.P3.T1 — Solving, Sparse Solve and Export Fields
  - files: `Engine/Nodes/Pyro/PyroSolver.{h,cpp}`, `Tests/Native/PyroSolver_Test.cpp`, `Engine/Nodes/Pyro/docs/PyroSolver.md`
  - approach: Frames Before Solve warms up sources; Quantize To Max Substeps rounds substeps; Single V-Cycle maps to M541's mode; Reset Rule clears listed fields in newly occupied or deoccupied tiles; the export list picks which fields become output primitives. Node docs written fresh, naming no other application.
  - verify: `PyroSolver_Test`: exporting only `density` outputs one primitive; Reset Rule "Newly Occupied" zeroes listed fields in new tiles.
  - size: M

## Phase 542.4: Proof

- [ ] M542.P4.T1 — GUI check: smoke plume
  - files: `Tests/gui/m542_pyro_smoke.py`
  - approach: `GeoPrimitive` sphere → `VDBFromPolygons` fog → `PyroSolver` → `GeoToUSD` → viewer; play 1–48, scrub back to 10, edit Buoyancy Scale and check the reset; repeat with Use GPU Off.
  - verify: the script passes under Xvfb; screenshots of frames 12, 24 and 48 sent with SendUserFile for the Feature's UAT.
  - size: M

**Verification gate:** full ctest green including `PyroSolver_Test`, `PyroSourcing_Test` and `PyroForces_Test`; `Tests/gui/m542_pyro_smoke.py` passes under Xvfb.

## Decisions

- 2026-11-22 — **Minimal OpenCL becomes Minimal GPU, Use OpenCL becomes Use GPU (/cat-plan):** the kernels are Slang; the modes keep their meaning.
