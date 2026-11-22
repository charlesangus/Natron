# M545 - Pyro Collisions

`PyroSolver`'s Collision tab and Advanced collision controls: colliders from an SDF VDB or polygons (through M418 - VDB Conversion Ops' `GeoVDBConvert`), moving and deforming colliders with velocity, ground planes, a collision mask and collision correction, feeding M541 - Pyro Advection And Pressure's solid boundary.

Member of F35 - Pyro: branches off and merges into `feature/f35-pyro` (PLAN-FORMAT §9b); the Feature's UAT covers it. Epic: E22 - Simulation And Solvers. Shared design: `PLAN/DECISIONS/2026-11-22-vdb-pyro-solver.md`.

## Scope

**In:**
- Limit Collision Range: Limit Collision Range, Range Type, Frame, Collision Frame Range, Cycle Length.
- Source Collision: Collision Type (each reference option, including SDF + Volume Velocity), Collision Voxel Size, Collision Offset, Velocity Scale, Collision SDF, Velocity Volume, Use Deforming Geometry.
- Ground Planes: X, Y and Z Ground Plane, Use Deforming Geometry.
- Advanced Collision Mask: Build Collision Mask, Bandwidth, Use Point Velocity For Collisions, Use Volume Velocity For Collisions, Collide With Non-SDF. Collision Correction: Correct Collisions, Fields To Correct, Feedback Scale, IOP Iterations.
- The Collision field guide (M543 - Pyro Fields And Combustion) shows the mask.

**Out:**
- Height Field ground planes: native geometry has no heightfield primitive (see Decisions).
- Two-way coupling that moves the collider (none).

**Edge cases:**
- Polygon colliders: converted per frame (or once, with a static frame) at the collision voxel size; open meshes get an unsigned band, as M418 does.
- Deforming geometry: collider velocity from point `v`, or from point motion between frames when `v` is absent.
- Smoke starting inside a collider: corrected out by collision correction when on, otherwise zeroed in solid cells.

## Phase 545.1: Collision mask

- [ ] M545.P1.T1 — Collider SDF, velocity and ground planes into the solid field
  - files: `Engine/Geo/Pyro/PyroCollision.{h,cpp}`, `Engine/Gpu/Kernels/pyro_collision.slang`, `Engine/Gpu/Kernels/pyro_collision.cmake`, `Engine/Nodes/Pyro/PyroSolver.{h,cpp}`, `Tests/Native/PyroCollision_Test.cpp`
  - approach: From the collision input build an SDF (`GeoVDBConvert::polygonsToSdf`, or the input SDF directly), a velocity field (point `v` rasterised, a velocity volume, or both), and ground planes; convert into M540 tiles as solid fraction and solid velocity within the bandwidth; the range limit selects the frame. Bundled gtest is old: no `GTEST_SKIP`, no `ASSERT_TRUE(ptr)`.
  - verify: `PyroCollision_Test`: a static sphere collider keeps density out of its interior within one voxel over 30 steps; a sphere moving at v pushes smoke ahead with normal velocity ≈ v at its surface; a Y ground plane stops a falling blob.
  - size: L
- [ ] M545.P1.T2 — Collision correction and the remaining controls
  - files: `Engine/Geo/Pyro/PyroCollision.{h,cpp}`, `Engine/Nodes/Pyro/PyroSolver.{h,cpp}`, `Tests/Native/PyroCollision_Test.cpp`, `Engine/Nodes/Pyro/docs/PyroSolver.md`
  - approach: Correction pushes listed fields out of solids along the SDF gradient, scaled by feedback, iterated IOP Iterations times; Collide With Non-SDF treats a fog volume as solid above 0.5. Docs for the Collision tab written fresh.
  - verify: `PyroCollision_Test`: density seeded inside a collider ends outside it after correction with total mass kept within 5 %.
  - size: M

## Phase 545.2: Proof

- [ ] M545.P2.T1 — GUI check: smoke around a moving collider
  - files: `Tests/gui/m545_pyro_collision.py`
  - approach: M542's plume rising past an animated sphere and onto a ground plane; Collision field guide on.
  - verify: the script passes under Xvfb; screenshots sent with SendUserFile for the Feature's UAT.
  - size: M

**Verification gate:** full ctest green including `PyroCollision_Test`; `Tests/gui/m545_pyro_collision.py` passes under Xvfb.

## Decisions

- 2026-11-22 — **Height Field ground plane dropped (/cat-plan):** native geometry has no heightfield primitive; revisit if one is added.
