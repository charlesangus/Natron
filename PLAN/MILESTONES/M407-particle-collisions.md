# M407 - Particle Collisions

Particles collide with geometry through signed-distance VDBs, as Houdini's static objects do. `PopCollide` takes collision geometry, converts it with M418 - VDB Conversion Ops' `VDBFromPolygons` path to an SDF (cached per frame for deforming colliders, with collider velocity), and applies bounce, friction, stick or kill responses, writing hit attributes. A ground plane needs no VDB.

Member of F19 - Particles: branches off and merges into `feature/f19-particles` (PLAN-FORMAT §9b); the Feature's UAT covers it. Epic: E22 - Simulation And Solvers. Depends on F22 - Native VDB landing, because M418 - VDB Conversion Ops is a member of that Feature.

## Scope

**In:**
- `PopCollide` with a Collision Geometry input (polygons or an SDF VDB primitive), a voxel-size knob, and a ground-plane option.
- Responses per collider: Bounce (restitution), Slide (friction), Stick, Kill.
- Hit attributes: `hitnum`, `hittime`, `hitpos`, `hitN`, `hitv`, written on the hit substep, as in POPs.
- Deforming colliders: one SDF per frame, interpolated at substep time, plus collider velocity from point motion so moving objects push particles.
- Continuous collision: substep-swept SDF test (march along the step) to avoid tunnelling through thin walls at the chosen voxel size.

**Out:**
- Mesh BVH ray-cast collisions (user chose VDB SDF).
- Particle-particle collisions beyond `PopInteract` (M406 - Particle Forces And Wrangle).
- GPU collisions (M409 - Particle GPU Path).

**Edge cases:**
- Open (non-watertight) collision meshes: SDF built as an unsigned narrow band with thickness; documented.
- Particles starting inside a collider: pushed out along the gradient on the first substep.
- Voxel size much larger than particle spacing: info message suggesting a smaller size.
- Collider with no polygons: no-op with an info message.
- Stick: particle follows the collider's surface velocity until released by a wrangle clearing `stuck`.

**Assumptions:**
- Unconfirmed: SDF lookups use OpenVDB's `BoxSampler` on the CPU; M409 - Particle GPU Path ports them through NanoVDB.

## Phase 407.1: Collisions

- [ ] M407.P1.T1 — `PopCollide` against an SDF and a ground plane
  - files: `Engine/Nodes/Particles/PopCollide.{h,cpp,reg.inc}`, `Engine/Geo/GeoSdfCollide.{h,cpp}`, `Tests/Native/PopCollide_Test.cpp`
  - approach: Build (or accept) an SDF grid from the collision input via the M418 - VDB Conversion Ops library function, cached by the collider's detail hash. Per substep, for each particle: march from old to new `P` in steps ≤ half the voxel size, on sign change locate the hit, reflect `v` by restitution and friction about `hitN`, or stick/kill; write hit attributes. Ground plane is analytic.
  - verify: `PopCollide_Test`: a falling particle bounces off a box SDF to the restitution-predicted height; a fast particle does not tunnel through a 1-voxel wall; kill removes it; hit attributes are set on the hit substep only.
  - size: L
- [ ] M407.P1.T2 — Deforming colliders with velocity
  - files: `Engine/Nodes/Particles/PopCollide.{h,cpp}`, `Engine/Geo/GeoSdfCollide.{h,cpp}`, `Tests/Native/PopCollide_Test.cpp`
  - approach: Build SDFs at the frame before and after, interpolate the distance at substep time, and rasterise collider point velocity (from `v` or finite differences of `P`) into a velocity grid so responses are relative to the collider's motion.
  - verify: `PopCollide_Test`: a moving plane pushes resting particles along at its speed; a rotating box flings particles tangentially.
  - size: L

## Phase 407.2: Proof

- [ ] M407.P2.T1 — GUI check and docs
  - files: `Tests/gui/m407_pop_collide.py`, `Engine/Nodes/Particles/docs/PopCollide.md`
  - approach: Particles pouring onto an animated sphere over a ground plane; check playback, and that a `PopWrangle` colouring particles by `hitnum` shows hits on the sphere and ground.
  - verify: the script passes under Xvfb; a screenshot sent with SendUserFile for the Feature's UAT.
  - size: M

**Verification gate:** full ctest green including `PopCollide_Test`; `Tests/gui/m407_pop_collide.py` passes under Xvfb.

## Decisions

- 2026-11-12 — **Collisions through VDB SDFs (user, /cat-plan grill),** not a mesh BVH: robust, fast, GPU-portable via NanoVDB, as Houdini's static objects. This makes F19 - Particles wait for F22 - Native VDB to land.
- 2026-11-12 — **Bounce, slide, stick and kill responses with hit attributes (user).**

Shared design: `PLAN/DECISIONS/2026-11-12-geometry-simulation-expansion.md`.
