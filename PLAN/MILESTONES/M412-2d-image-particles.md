# M412 - 2D Image Particles

Screen-space particles for comp, on the same POP engine as 3D (user, 2026-11-12). `ParticleSolverEnd` gains a 2D mode that keeps motion in a plane in image coordinates. `PopSourceImage` emits from an image's alpha or luma, taking `Cd` from the pixels. `ParticleRender` is an image node that draws particles as splats or image sprites, with motion blur from `v` and additive or over blending. An optional Scene camera input renders 3D particles straight to an image without Hydra (user, 2026-11-12).

Member of F19 - Particles: branches off and merges into `feature/f19-particles` (PLAN-FORMAT §9b); the Feature's UAT covers it. Epic: E22 - Simulation And Solvers.

## Scope

**In:**
- 2D mode on the particle solver: `P.z` and `v.z` held at zero, forces' z ignored, positions in the image's canonical coordinates (pixels, honouring pixel aspect).
- `PopSourceImage`: emit where an image's alpha or luma exceeds a threshold, with density proportional to it; `Cd` (and optionally `Alpha`) sampled from the pixels; initial velocity from a chosen vector layer.
- `ParticleRender`: input Particles (geometry), optional Sprite (image), optional Camera (scene). Splat shapes (disc, gaussian), size from `pscale`, colour from `Cd`/`Alpha`, sprites rotated by `angle`, additive or over, depth-sorted when a camera is set; motion blur by sub-frame splatting along `v` with shutter controls; output format knob.
- GPU and CPU paths for rendering.

**Out:**
- Lit particle renders (use `GeoToUSD` → USDRender).
- Volume-style particle shading (none).

**Edge cases:**
- Huge particle counts: splatting is a parallel scatter with per-tile bins; no cap.
- Particles off-screen: culled; RoI respected (render is local, unlike the sim).
- Camera given but particles in 2D mode: camera ignored with an info message.
- Sprite with alpha: premultiplied over; additive ignores sprite alpha.
- Proxy: `ParticleRender` scales positions and sizes to the requested scale.

**Assumptions:**
- Unconfirmed: the camera projection reuses M234 - Camera Projection Nodes' camera-matrix helpers.

## Phase 412.1: 2D emission

- [ ] M412.P1.T1 — 2D solver mode and `PopSourceImage`
  - files: `Engine/Nodes/Particles/ParticleSolverEnd.{h,cpp}`, `Engine/Nodes/Particles/PopSourceImage.{h,cpp,reg.inc}`, `Tests/Native/PopSourceImage_Test.cpp`
  - approach: A Space knob (3D / 2D) on the solver zeroes z after each substep. `PopSourceImage` has an image input, renders it at the requested frame and substep time, samples birth positions proportional to the chosen channel above a threshold (CDF over pixels), sets `P` in canonical coordinates, `Cd` from RGB, `v` from an optional vector layer.
  - verify: `PopSourceImage_Test`: an image whose alpha is a left-half mask emits only in the left half; `Cd` matches the sampled pixels; 2D mode keeps z = 0 under 3D gravity.
  - size: M

## Phase 412.2: Rendering

- [ ] M412.P2.T1 — `ParticleRender` splats and sprites in 2D
  - files: `Engine/Nodes/Particles/ParticleRender.{h,cpp,reg.inc}`, `Engine/Gpu/Kernels/particlerender.slang`, `Engine/Gpu/Kernels/particlerender.cmake`, `Tests/Native/ParticleRender_Test.cpp`
  - approach: An image node with a geometry input; bins particles to tiles covering the RoI, splats disc or gaussian footprints of `pscale` pixels with `Cd`/`Alpha`, or a sprite image rotated by `angle`; additive or over (over sorted by `id` for stability). Motion blur splats N sub-samples along `v` over the shutter. CPU and GPU forms.
  - verify: `ParticleRender_Test`: one particle renders a disc of the right radius and colour; two overlapping particles in additive sum, in over composite; motion blur spreads energy along `v` with constant total; GPU matches CPU within tolerance.
  - size: L
- [ ] M412.P2.T2 — Camera projection for 3D particles
  - files: `Engine/Nodes/Particles/ParticleRender.{h,cpp}`, `Tests/Native/ParticleRender_Test.cpp`
  - approach: With a Scene camera connected, project `P` through the camera (from the USD camera prim via the existing helpers), scale `pscale` by perspective, depth-sort for over, and cull behind the camera.
  - verify: `ParticleRender_Test`: a particle at the camera's look-at point renders at frame centre; doubling distance halves its radius; particles behind the camera don't render.
  - size: M

## Phase 412.3: Proof

- [ ] M412.P3.T1 — GUI check and docs
  - files: `Tests/gui/m412_image_particles.py`, `Engine/Nodes/Particles/docs/PopSourceImage.md`, `Engine/Nodes/Particles/docs/ParticleRender.md`
  - approach: Sparks emitted from a Text node's alpha, blown by wind and curl noise, rendered with motion blur over a plate; and the same 3D fountain from M405 - Particle Solver Core rendered through a camera.
  - verify: the script passes under Xvfb; screenshots sent with SendUserFile for the Feature's UAT.
  - size: M

**Verification gate:** full ctest green including `PopSourceImage_Test` and `ParticleRender_Test`; `Tests/gui/m412_image_particles.py` passes under Xvfb.

## Decisions

- 2026-11-12 — **2D particles reuse the POP engine (user, /cat-plan grill):** a 2D solver mode, `PopSourceImage` and a `ParticleRender` image node, not a second solver.
- 2026-11-12 — **`ParticleRender` takes an optional camera (user):** 3D particles render straight to an image; Hydra stays for lit renders.

Shared design: `PLAN/DECISIONS/2026-11-12-geometry-simulation-expansion.md`.
