# M541 - Pyro Advection And Pressure

The solver kernels on M540 - Sparse Tile Field Core, on CPU and GPU: field and velocity advection, a sparse multigrid-preconditioned CG pressure solve with vacuum, wall and solid boundaries and a goal divergence, advection-reflection, viscosity and hourglass filtering. No node yet; M542 - PyroSolver Node And Sourcing drives them.

Member of F35 - Pyro: branches off and merges into `feature/f35-pyro` (PLAN-FORMAT §9b); the Feature's UAT covers it. Epic: E22 - Simulation And Solvers. Shared design: `PLAN/DECISIONS/2026-11-22-vdb-pyro-solver.md`.

## Scope

**In:**
- Advection with every Field Advection control: Advection Scheme (semi-Lagrangian, modified MacCormack, BFECC), Clamp Values, Blend, Trace Method (each option in the reference menu), CFL Condition, Max Steps, Max Batch Size, Extra Fields; velocity advection either shares those settings or has its own.
- Pressure: MGPCG on the sparse tile hierarchy (each level coarsens tile coordinates), red-black Gauss-Seidel smoothing; inactive space is vacuum (pressure 0), bounds walls are closed or open per side, a solid-fraction field with solid velocity is a Neumann boundary; an optional goal-divergence field. A single-V-cycle mode skips CG.
- Advection-reflection: Disabled, Single-Project, Double-Project, with a reflection amount.
- Viscosity as an implicit diffusion through the same multigrid; hourglass filtering (amount, scale by divergence, relative divergence, divergence scale).
- Each kernel on the GPU (Slang) and the CPU (the same algorithm over the thread pool).

**Out:**
- Sourcing, forces and fields (M542 - PyroSolver Node And Sourcing, M543 - Pyro Fields And Combustion, M544 - Pyro Shaping Forces).
- Building collision masks (M545 - Pyro Collisions); this milestone only consumes a solid-fraction field.

**Edge cases:**
- Pressure not converging within the iteration limit: the last iterate is used and a warning set once per frame.
- A closed domain with net goal divergence: the mean is removed so the system stays solvable, as for any all-Neumann region.
- A solid filling a whole tile: the tile stays active with zero fluid fraction and is skipped by the smoother.
- CPU vs GPU: equal within tolerance over a few steps; never compared bit-exact.

**Assumptions:**
- Unconfirmed: one coarsening scheme serves both the sparse tiles and Dense mode without a separate dense multigrid.

## Phase 541.1: Advection

- [ ] M541.P1.T1 — Advection kernels on CPU and GPU
  - files: `Engine/Geo/Pyro/PyroAdvect.{h,cpp}`, `Engine/Gpu/Kernels/pyro_advect.slang`, `Engine/Gpu/Kernels/pyro_advect.cmake`, `Tests/Native/PyroAdvect_Test.cpp`, `Tests/Gpu/PyroAdvectGpu_Test.cpp`
  - approach: Back-trace through the staggered velocity with the chosen trace method, sub-stepped to the CFL condition up to Max Steps; MacCormack and BFECC with min/max clamping and a blend toward semi-Lagrangian; scalar and vector fields, batched by Max Batch Size on the GPU. Samplers from M540 - Sparse Tile Field Core. Bundled gtest is old: no `GTEST_SKIP`, no `ASSERT_TRUE(ptr)`.
  - verify: CPU test: a blob in a constant field moves by v·dt within half a voxel for each scheme; BFECC loses less peak than semi-Lagrangian over 50 steps; clamping keeps values inside the source range. GPU test (lavapipe) matches CPU within 1e-4 relative per scheme.
  - size: L

## Phase 541.2: Pressure

- [ ] M541.P2.T1 — Sparse multigrid hierarchy and MGPCG on the CPU
  - files: `Engine/Geo/Pyro/PyroPressure.{h,cpp}`, `Tests/Native/PyroPressure_Test.cpp`
  - approach: Build coarse levels by halving tile coordinates of the active set; cell types fluid / vacuum / solid from activation and the solid-fraction field; red-black Gauss-Seidel smoother, full-weighting restriction, trilinear prolongation; V-cycle as a CG preconditioner, or alone in single-V-cycle mode; goal divergence on the right-hand side; tolerance and max iterations.
  - verify: `PyroPressure_Test`: a known Poisson problem converges to tolerance; projection leaves max |divergence| below tolerance in fluid cells; a solid box with velocity v gives zero relative normal flux on its faces; vacuum cells end at pressure 0.
  - size: L
- [ ] M541.P2.T2 — The same pressure solve on the GPU
  - files: `Engine/Geo/Pyro/PyroPressureGpu.{h,cpp}`, `Engine/Gpu/Kernels/pyro_pressure.slang`, `Engine/Gpu/Kernels/pyro_pressure.cmake`, `Tests/Gpu/PyroPressureGpu_Test.cpp`
  - approach: Mirror P2.T1's hierarchy as device buffers built from the CPU-built level tables; smoother, residual, restriction, prolongation and the CG dot products (reduction kernels) in Slang; record through `GpuRecordContext`.
  - verify: `PyroPressureGpu_Test` on lavapipe: the problems from P2.T1 converge, and the projected velocity matches the CPU within 1e-4 relative.
  - size: L

## Phase 541.3: Projection extras

- [ ] M541.P3.T1 — Advection-reflection, viscosity and hourglass filtering
  - files: `Engine/Geo/Pyro/PyroProject.{h,cpp}`, `Engine/Gpu/Kernels/pyro_project.slang`, `Engine/Gpu/Kernels/pyro_project.cmake`, `Tests/Native/PyroProject_Test.cpp`, `Tests/Gpu/PyroProjectGpu_Test.cpp`
  - approach: A step function composing advect and project in Disabled, Single-Project (carry the removed component to the next step, scaled by reflection amount) and Double-Project orders; implicit viscosity solved with the P2 multigrid on each velocity component; hourglass filtering on the staggered velocity, scaled by absolute or relative divergence.
  - verify: CPU test: a vortex ring keeps more kinetic energy over 100 steps with Single-Project than Disabled, and the result stays bounded with reflection amount 1; viscosity damps a shear layer at the analytic rate within 10 %; GPU matches CPU within 1e-4 relative after 5 steps.
  - size: L

**Verification gate:** full ctest green including `PyroAdvect_Test`, `PyroAdvectGpu_Test`, `PyroPressure_Test`, `PyroPressureGpu_Test`, `PyroProject_Test` and `PyroProjectGpu_Test`.
