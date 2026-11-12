# M406 - Particle Forces And Wrangle

The POP forces and per-substep wrangle for M405 - Particle Solver Core's block. Each force node adds to the `force` accumulator (or edits `v` directly where the POP does) inside the solver body, scaled by an optional attribute and restricted by an optional group, following the matching POP node's behaviour.

Member of F19 - Particles: branches off and merges into `feature/f19-particles` (PLAN-FORMAT §9b); the Feature's UAT covers it. Epic: E22 - Simulation And Solvers.

## Scope

**In:**
- `PopForce` (gravity and a constant force), `PopWind` (wind velocity with air resistance), `PopDrag`, `PopCurlNoise` (divergence-free curl noise, animated), `PopAttract` (to points or a goal position, by point id or nearest), `PopAxisForce` (vortex/orbit about an axis), `PopSpeedLimit`, `PopInteract` (particle-to-particle separation and attraction with a radius), and `PopWrangle`.
- Every force node: group pattern, a scale attribute, and an on/off.
- `PopWrangle` runs the M360 - Geometry Kernel Language snippet per particle per substep, with `@TimeInc`, `@Frame`, `@Time`, solver inputs readable as extra inputs, and `ch()` knobs.

**Out:**
- Collisions (M407 - Particle Collisions).
- GPU execution of these nodes (M409 - Particle GPU Path).
- Fluid-style pressure (none).

**Edge cases:**
- `PopInteract` neighbour search at 1M particles: a uniform hash grid rebuilt per substep; cost recorded in the gate.
- Zero or negative mass: forces skip that particle with a one-time warning.
- Curl noise continuity: sampled at substep time so animated noise doesn't step at frames.
- Attract with no target points: no-op with an info message.
- A wrangle that writes `P` directly: allowed, as in POPs; integration still adds `v*dt` afterwards.
- A wrangle that adds or removes points: applied after the run as M360 queues topology edits; new points get fresh ids.

**Assumptions:**
- Unconfirmed: curl noise reuses the noise library `libs/noise` / M129 - Clean-Sheet Noise Node's kernels for its potential field.
- Unconfirmed: `PopWrangle` is `GeoWrangle`'s binding with solver variables added, not a second runner.

## Phase 406.1: Field forces

- [ ] M406.P1.T1 — `PopForce`, `PopWind`, `PopDrag`, `PopSpeedLimit`
  - files: `Engine/Nodes/Particles/PopForce.{h,cpp,reg.inc}`, `Engine/Nodes/Particles/PopWind.{h,cpp,reg.inc}`, `Engine/Nodes/Particles/PopDrag.{h,cpp,reg.inc}`, `Engine/Nodes/Particles/PopSpeedLimit.{h,cpp,reg.inc}`, `Engine/Nodes/Particles/PopForceCommon.{h,cpp}`, `Tests/Native/PopForces_Test.cpp`
  - approach: A shared base handles group pattern, scale attribute and the page-batched loop that adds to `force` (or limits `v`). Wind: `force += airresist * (wind − v)`. Drag: `force −= drag * v`. Speed limit clamps |v| to min/max after forces, as the POP does.
  - verify: `PopForces_Test` checks each node against hand-computed one-step results, group restriction and scale attribute; wind with air resistance converges `v` to the wind velocity.
  - size: M
- [ ] M406.P1.T2 — `PopCurlNoise`, `PopAttract`, `PopAxisForce`
  - files: `Engine/Nodes/Particles/PopCurlNoise.{h,cpp,reg.inc}`, `Engine/Nodes/Particles/PopAttract.{h,cpp,reg.inc}`, `Engine/Nodes/Particles/PopAxisForce.{h,cpp,reg.inc}`, `Tests/Native/PopForces_Test.cpp`
  - approach: Curl noise as the curl of a 3D vector noise potential (frequency, offset, roughness, animation speed), as a force or a velocity blend. Attract toward a goal point or the second input's points (match by `id` or nearest via the cached point tree). Axis force: orbit, suction and lift about an axis with radial falloff.
  - verify: `PopForces_Test`: curl-noise field divergence is ≈ 0 by finite differences; attract converges particles onto targets; axis force produces circular motion of the expected radius.
  - size: M

## Phase 406.2: Interaction and wrangle

- [ ] M406.P2.T1 — `PopInteract`
  - files: `Engine/Nodes/Particles/PopInteract.{h,cpp,reg.inc}`, `Engine/Geo/GeoHashGrid.{h,cpp}`, `Tests/Native/PopInteract_Test.cpp`
  - approach: Per substep, build a uniform hash grid over `P` with cell = interaction radius; separation force (repulsion inside `pscale`-scaled radius) and optional attraction band, symmetric so momentum is conserved. Parallel over pages, gather-only (each particle sums its neighbours) for determinism.
  - verify: `PopInteract_Test`: two particles closer than the radius separate symmetrically; a 100k cloud's total momentum is conserved within float tolerance; results identical at 1 and 8 threads.
  - size: L
- [ ] M406.P2.T2 — `PopWrangle`
  - files: `Engine/Nodes/Particles/PopWrangle.{h,cpp,reg.inc}`, `Engine/Geo/GeoKernelBinding.{h,cpp}`, `Tests/Native/PopWrangle_Test.cpp`
  - approach: Reuse `GeoWrangle`'s compile and run path over points, with the solver's substep time exposed as `@TimeInc` and Time/Frame at substep time; second and later inputs are the solver block's extra inputs. Same `ch()` and Create Parameters button.
  - verify: `PopWrangle_Test`: `v@force += {0,-9.8,0};` reproduces `PopForce` gravity bit-for-bit; `@TimeInc` equals 1/(fps·substeps).
  - size: M

## Phase 406.3: Proof

- [ ] M406.P3.T1 — GUI check and docs
  - files: `Tests/gui/m406_pop_forces.py`, `Engine/Nodes/Particles/docs/*.md`
  - approach: A fountain with gravity, wind, curl noise and interaction; check playback and that toggling each force changes the result. Docs written fresh for every node.
  - verify: the script passes under Xvfb; a frame-48 screenshot sent with SendUserFile for the Feature's UAT.
  - size: M

**Verification gate:** full ctest green including `PopForces_Test`, `PopInteract_Test` and `PopWrangle_Test`; `Tests/gui/m406_pop_forces.py` passes under Xvfb; `PopInteract` at 1M particles timed per substep in `## Decisions` (no threshold).

## Decisions

- 2026-11-12 — **Core and field forces plus `PopInteract` (user, /cat-plan grill).**

Shared design: `PLAN/DECISIONS/2026-11-12-geometry-simulation-expansion.md`.
