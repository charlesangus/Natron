# M405 - Particle Solver Core

A Houdini-POP-style particle system on native geometry. Particles are points in a `GeoDetail` with `id`, `age`, `life`, `dead`, `v`, `P`, `pscale` and any user attributes. A `ParticleSolverBegin`/`ParticleSolverEnd` block, built on M404 - Geometry Solver, holds `Pop*` nodes that each act on the particle detail per substep; the end node integrates. Any `Geo*` node also works inside the body. This milestone delivers the block, integration, emission and kill; forces come in M406 - Particle Forces And Wrangle and collisions in M407 - Particle Collisions.

Member of F19 - Particles: branches off and merges into `feature/f19-particles` (PLAN-FORMAT §9b); the Feature's UAT covers it. Epic: E22 - Simulation And Solvers.

## Scope

**In:**
- `ParticleSolverBegin`/`ParticleSolverEnd` over `GeoSolverBegin`/`End`'s core: start frame, substeps, reset, cache, and the integrator (symplectic Euler by default, with a force accumulator attribute `force` cleared each substep and `mass` defaulting to 1).
- `PopSource`: emit from the source detail's points, scattered over its polygons (reusing `GeoScatter`'s area-weighted sampler) or along its polylines; constant or per-frame birth rate, an optional impulse count; birth density modulated by a chosen source attribute (user, 2026-11-12); inherit attributes (`v`, `Cd`, any listed) from the source, with an initial velocity, variance and an inherit-velocity scale; unique, stable `id` per particle.
- Ageing and `PopKill`: `age` advances by dt, particles past `life` die; `PopKill` also kills by group, by a wrangle-style expression or a bounding region.
- Output: the particle points (no primitives) through `GeoToUSD` as `UsdGeomPoints`, `pscale` → `widths` (already mapped in `GeoUsdConvert.cpp:1240`) and `v` → `velocities` for motion blur.

**Out:**
- Forces and `PopWrangle` (M406 - Particle Forces And Wrangle).
- Collisions (M407 - Particle Collisions).
- The GPU path (M409 - Particle GPU Path).
- Emission from volumes (none until after M418 - VDB Conversion Ops) and from images (M412 - 2D Image Particles).
- Rigid bodies, fluids (FLIP), cloth (none).

**Edge cases:**
- Fractional birth counts per substep: carried over in a detail attribute so the long-run rate is exact and deterministic.
- Deforming emitter: scatter positions sample the source at substep time by interpolating its frames, so emission doesn't step at frame boundaries.
- Source with no polygons in surface mode: no emission and an info message.
- Modulating attribute missing or negative: missing errors with the name; negatives clamp to zero.
- Deaths: points are removed after the substep (tombstoned), and `id` is never reused.
- Determinism: random streams seed from (seed, frame, substep, emitter), so results don't depend on thread count.

**Assumptions:**
- Unconfirmed: the integrator choice is a knob (Euler, symplectic Euler, Verlet) with symplectic Euler default.
- Unconfirmed: `v` → `velocities` mapping is missing from `GeoUsdConvert` today (a grep found none) and is added here.

## Phase 405.1: Solver and integration

- [ ] M405.P1.T1 — `ParticleSolverBegin`/`End` with integration and ageing
  - files: `Engine/Nodes/Particles/ParticleSolverBegin.{h,cpp,reg.inc}`, `Engine/Nodes/Particles/ParticleSolverEnd.{h,cpp,reg.inc}`, `Engine/Geo/GeoParticles.{h,cpp}`, `Engine/CMakeLists.txt`, `Tests/Native/ParticleSolver_Test.cpp`
  - approach: Subclass or compose `GeoSolverBegin`/`End` (M404 - Geometry Solver). End runs, per substep: body (which adds to `force`), integrate `v += force/mass*dt; P += v*dt`, advance `age`, mark and remove dead points, clear `force`. Put the per-element math in `GeoParticles` as page-batched loops so M409 - Particle GPU Path can mirror it. Register under a new `PLUGIN_GROUP_3D` "Particles" sub-menu.
  - verify: `ParticleSolver_Test`: a single particle with `force = (0,-9.8,0)` set by a test node matches the closed-form trajectory within the integrator's error at 1 and 4 substeps; particles with `life = 1` die after 1 s; results identical at 1 and 8 threads.
  - size: L

## Phase 405.2: Emission and kill

- [ ] M405.P2.T1 — `PopSource` from points, surfaces and curves
  - files: `Engine/Nodes/Particles/PopSource.{h,cpp,reg.inc}`, `Engine/Nodes/Geometry/GeoScatter.{h,cpp}` (expose the sampler), `Tests/Native/PopSource_Test.cpp`
  - approach: Emit mode Points / Surface / Curves. Factor `GeoScatter`'s area-weighted sampler into a reusable function and add length-weighted polyline sampling. Birth rate (per second) or impulse count, with fractional carry-over in a detail attribute; density weighted by a chosen source attribute. Interpolate a deforming source between frames at substep time. Inherit listed attributes; set `id`, `age = 0`, `life` ± variance, initial `v` and inherited `v` × scale.
  - verify: `PopSource_Test`: 100/s for 1 s at 24 fps with 3 substeps emits exactly 100; a ramp attribute modulating density puts ≥ 90 % of particles in its high half; ids are unique and stable across a reset and re-sim; curves mode places every particle on the polyline.
  - size: L
- [ ] M405.P2.T2 — `PopKill`, and particle velocities through `GeoToUSD`
  - files: `Engine/Nodes/Particles/PopKill.{h,cpp,reg.inc}`, `Engine/Nodes/Geometry/GeoUsdConvert.cpp`, `Tests/Native/PopKill_Test.cpp`, `Tests/GeoToUSD_Test.cpp` (or the bridge test M358 added)
  - approach: `PopKill` marks `dead` by group pattern, by bounding box/sphere region, or by a simple `age`/attribute comparison. In `GeoUsdConvert`, map point `v` (float3, vector type) to `UsdGeomPointBased` `velocities` for Points, Mesh and Curves partitions.
  - verify: `PopKill_Test`: a box region kills exactly the particles inside it; the bridge test checks `velocities` authored on a Points prim from `v`.
  - size: M

## Phase 405.3: Proof

- [ ] M405.P3.T1 — GUI check: a fountain
  - files: `Tests/gui/m405_particles.py`, `Engine/Nodes/Particles/docs/ParticleSolver.md`, `Engine/Nodes/Particles/docs/PopSource.md`
  - approach: Build a grid emitter → `ParticleSolver` block with `PopSource` and a test gravity via `GeoWrangle` → `GeoToUSD` → viewer, play 1–48, check points draw with `widths` and motion blur in a USDRender. Docs written fresh.
  - verify: the script passes under Xvfb; frame 48 screenshot sent with SendUserFile for the Feature's UAT.
  - size: M

**Verification gate:** full ctest green including `ParticleSolver_Test`, `PopSource_Test` and `PopKill_Test`; `Tests/gui/m405_particles.py` passes under Xvfb; a 1M-particle, 100-frame sim saved headless through `GeoFileCache` with time per frame recorded in `## Decisions` (no threshold).

## Decisions

- 2026-11-12 — **POP nodes inside a solver block (user, /cat-plan grill),** on M404 - Geometry Solver, not a monolithic node: the Houdini model, and any `Geo*` node works in the body.
- 2026-11-12 — **`PopSource` emits from points, surfaces and curves, with density modulated by a chosen source attribute (user).**
- 2026-11-12 — **Particles ship as F19 - Particles (user):** core, forces, collisions, GPU path and 2D image particles share one UAT.

Shared design: `PLAN/DECISIONS/2026-11-12-geometry-simulation-expansion.md`.
