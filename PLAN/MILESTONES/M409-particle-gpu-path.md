# M409 - Particle GPU Path

Run particle simulation on the GPU for millions of particles. Integration, ageing, emission placement, the M406 - Particle Forces And Wrangle forces, `PopWrangle` (through M408 - Kernel Language GPU Backend) and M407 - Particle Collisions' SDF lookups (through NanoVDB) run as Slang kernels on particles that stay resident on the device across substeps. Read back once per frame. The CPU path from M405 - Particle Solver Core stays and is chosen by the weight-based placement or on any node in the body without a GPU form (user, 2026-11-12: CPU path kept for GPU-less hosts and farms).

Member of F19 - Particles: branches off and merges into `feature/f19-particles` (PLAN-FORMAT §9b); the Feature's UAT covers it. Epic: E22 - Simulation And Solvers.

## Scope

**In:**
- A resident particle buffer for the solver block and a per-substep kernel sequence: body nodes with GPU forms, then integrate/age/kill.
- GPU forms for `PopForce`, `PopWind`, `PopDrag`, `PopSpeedLimit`, `PopCurlNoise`, `PopAttract`, `PopAxisForce`, `PopInteract` (GPU hash grid), `PopKill`, `PopCollide` (NanoVDB SDF), and `PopWrangle` via M408.
- Emission: births computed on the CPU (counts, ids, sampled positions) and appended to the device buffer.
- Whole-block fallback to the CPU when any body node has no GPU form, with an info message naming it.

**Out:**
- GPU forms of arbitrary `Geo*` nodes inside the body (they force CPU fallback).
- Multi-GPU (none).

**Edge cases:**
- CPU and GPU sims diverge by ulps over many frames (chaotic): parity tests compare a few substeps with a tolerance; long-run looks are judged by eye at UAT.
- Device memory exhausted: the solver falls back to the CPU from the current frame, with a warning.
- Deaths on the GPU: stream compaction per substep keeps the buffer dense; ids stay stable.
- Cancel: abort checked between substeps; the partial frame is dropped.

**Assumptions:**
- Unconfirmed: NanoVDB headers ship with the OpenVDB in the image; if not, they are vendored (MPL-2.0).

## Phase 409.1: Resident solver

- [ ] M409.P1.T1 — Resident particle buffer, integrate/age/kill kernels and placement
  - files: `Engine/Nodes/Particles/ParticleSolverEnd.{h,cpp}`, `Engine/Gpu/Kernels/particles.slang`, `Engine/Gpu/Kernels/particles.cmake`, `Engine/Gpu/ParticleGpuState.{h,cpp}`, `Tests/Gpu/ParticleGpu_Test.cpp`
  - approach: Hold particles in device buffers across substeps; mirror `GeoParticles`' integrate/age/kill and add stream compaction for deaths. Birth appends from CPU-computed `PopSource` output. Read back to a `GeoDetail` once per frame. Placement asks the device scheduler; fall back on any body node lacking a GPU form.
  - verify: `ParticleGpu_Test` (lavapipe): a gravity-only sim of 10k particles over 24 frames matches the CPU path within 1e-4 relative per frame; deaths keep ids stable; a body with a `GeoTransform` falls back to the CPU with the info message.
  - size: L
- [ ] M409.P1.T2 — GPU forms of the field forces, kill and wrangle
  - files: `Engine/Gpu/Kernels/popforces.slang`, `Engine/Gpu/Kernels/popforces.cmake`, `Engine/Nodes/Particles/Pop*.cpp` (GPU hooks), `Tests/Gpu/ParticleGpu_Test.cpp`
  - approach: One kernel per force node, sharing the CPU node's parameter structs; `PopWrangle` dispatches its M408 program on the resident buffer.
  - verify: `ParticleGpu_Test`: each force node's one-substep GPU result matches CPU within tolerance; a `PopWrangle` gravity matches `PopForce` on the GPU.
  - size: L
- [ ] M409.P1.T3 — GPU `PopInteract` and `PopCollide`
  - files: `Engine/Gpu/Kernels/popinteract.slang`, `Engine/Gpu/Kernels/popcollide.slang`, matching `.cmake`, `Engine/Gpu/NanoVdbUpload.{h,cpp}`, `Tests/Gpu/ParticleGpu_Test.cpp`
  - approach: Counting-sort hash grid on the GPU for interaction (gather-only for determinism); upload collider SDFs as NanoVDB grids, cached per collider hash, and run the same swept test as the CPU.
  - verify: `ParticleGpu_Test`: interaction and collision single-substep results match CPU within tolerance; no tunnelling through a 1-voxel wall.
  - size: L

**Verification gate:** full ctest green including `ParticleGpu_Test` under lavapipe; on the GPU host, the same 1M-particle, 100-frame sim as M405 - Particle Solver Core's gate run CPU vs GPU, times recorded in `## Decisions` (report, no threshold).

## Decisions

- 2026-11-12 — **CPU path kept (user, /cat-plan grill):** GPU is an acceleration chosen by placement; GPU-less hosts and farms still sim.
- 2026-11-12 — **Wrangles on the GPU via M408 - Kernel Language GPU Backend (user).**

Shared design: `PLAN/DECISIONS/2026-11-12-geometry-simulation-expansion.md`.
