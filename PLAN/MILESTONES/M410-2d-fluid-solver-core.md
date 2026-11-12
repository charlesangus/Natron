# M410 - 2D Fluid Solver Core

A grid-based 2D smoke solver in the image graph. One `Fluid2D` node takes Source, Velocity and Collision image inputs and outputs scene-linear data layers (density, temperature, velocity), simulated frame to frame through M190 - Cross-Frame Feedback's frame store. The solver is Pyro's core recipe in 2D: semi-Lagrangian advection (MacCormack/BFECC option), multigrid pressure projection, buoyancy, dissipation, vorticity confinement and turbulence. It runs as Slang GPU kernels with a multithreaded CPU path of the same kernels (user, 2026-11-12). Fire, richer sources, motion-vector-driven velocity and shading are M411 - 2D Fluid Fire And Controls.

Member of F20 - 2D Fluids: branches off and merges into `feature/f20-2d-fluids` (PLAN-FORMAT §9b); the Feature's UAT covers it. Epic: E22 - Simulation And Solvers.

## Scope

**In:**
- `Fluid2D` image node: inputs Source (density from alpha or a chosen channel, temperature from another), Velocity (a 2-channel layer added as a force or set), Collision (alpha mask, static in this milestone).
- Domain: a chosen format times a resolution-scale knob; boundary Open, Closed or Padded. The sim always runs at its own resolution, whatever the viewer proxy or RoI; output is resampled to the requested scale (user, 2026-11-12).
- Solver: advection (semi-Lagrangian, MacCormack or BFECC), multigrid pressure solve with a tolerance and max V-cycles, buoyancy from temperature and density, dissipation and cooling, vorticity confinement, turbulence (curl noise).
- Time: start frame, substeps, reset; frames stored through M190's cross-frame store and baked through `LoopFrameBake`'s path.
- Output layers: `density`, `temperature`, `vel.x/vel.y` (as a motion-vector-compatible layer), and an RGBA preview of density.
- GPU kernels with a CPU path; placement by weight.

**Out:**
- Fuel/flame combustion, motion-vector sources, moving colliders, `Fluid2DShade` (M411 - 2D Fluid Fire And Controls).
- 3D smoke (none; VDB work in M417 - VDB Volume Primitive is the base for later).
- Step nodes for custom solvers (user chose one node).

**Edge cases:**
- Viewer proxy and RoI: ignored by the sim; requests at any scale resample the full-res result, so proxy never changes the look.
- Scrubbing backward and edits: as M190 - Cross-Frame Feedback (cached frames replay; an edit resets frames after start).
- Very large domains (8K+): memory estimate shown in the node's info; no cap (memory: no hand-holding size caps).
- Source format differs from the domain: sources are resampled into the domain's pixel space.
- Pressure solver not converging within max cycles: uses the last iterate and sets a warning once.
- CPU vs GPU results: equal within tolerance on a few frames; never compared bit-exact.

**Assumptions:**
- Unconfirmed: the M190 store's keying (loop hash, context, view, mipmap, frame) fits a single-node sim without a Loop block; if not, P1.T2 adds a small adapter.
- Unconfirmed: MAC-staggered velocity grid on the GPU, collocated for output.

## Phase 410.1: Kernels

- [ ] M410.P1.T1 — Fluid step kernels on CPU and GPU
  - files: `Engine/Gpu/Kernels/fluid2d.slang`, `Engine/Gpu/Kernels/fluid2d.cmake`, `Engine/Nodes/Fluid/Fluid2DKernels.{h,cpp}`, `Tests/Native/Fluid2DKernels_Test.cpp`, `Tests/Gpu/Fluid2DKernelsGpu_Test.cpp`
  - approach: Advection (semi-Lagrangian with bilinear and MacCormack/BFECC with limiter), buoyancy, dissipation, vorticity confinement, divergence, multigrid Poisson solve (red-black Gauss-Seidel smoother, full-weighting restriction), and projection, on a MAC grid. Mirror each kernel in C++ over a thread pool, following the CPU/GPU pairs native filters use (for example Blur).
  - verify: CPU tests: advecting a blob by a constant field moves it by the expected offset; projection leaves max |divergence| below tolerance; the multigrid solve converges on a known Poisson problem. GPU tests (lavapipe) match CPU within 1e-4 relative for each kernel.
  - size: L

## Phase 410.2: Node

- [ ] M410.P2.T1 — `Fluid2D` node with domain, inputs and frame store
  - files: `Engine/Nodes/Fluid/Fluid2D.{h,cpp,reg.inc}`, `Engine/CMakeLists.txt`, `Tests/Native/Fluid2D_Test.cpp`
  - approach: A `NativeEffectBase` image node whose render resolves its domain from format × scale regardless of RoI and mipmap, pulls inputs at substep times, steps the sim from the last stored frame using M190's cross-frame store (start frame, substeps, reset), then resamples to the requested scale and RoI. Output layers via the layer registry (E4 - Layers And Channels). Mark frame-ordered so writers serialise. Labels match script names.
  - verify: `Fluid2D_Test`: a static source with buoyancy rises over 24 frames; rendering at proxy 1/2 equals a downscale of the full render; scrubbing back to frame 10 does no new steps; an edit resets; closed boundaries conserve density within dissipation.
  - size: L

## Phase 410.3: Proof

- [ ] M410.P3.T1 — GUI check and docs
  - files: `Tests/gui/m410_fluid2d.py`, `Engine/Nodes/Fluid/docs/Fluid2D.md`
  - approach: A Radial source into `Fluid2D` at 2K, play 1–48 in the viewer, toggle proxy and check the look is stable; cancel mid-play and check the UI stays responsive. Docs written fresh.
  - verify: the script passes under Xvfb; frame-48 screenshots sent with SendUserFile for the Feature's UAT.
  - size: M

**Verification gate:** full ctest green including `Fluid2DKernels_Test`, `Fluid2DKernelsGpu_Test` (lavapipe) and `Fluid2D_Test`; `Tests/gui/m410_fluid2d.py` passes under Xvfb; a 2K, 100-frame sim timed CPU vs GPU on the GPU host and recorded in `## Decisions` (report, no threshold).

## Decisions

- 2026-11-12 — **One `Fluid2D` node (user, /cat-plan grill),** not step nodes in a loop.
- 2026-11-12 — **Stable fluids with multigrid pressure (user):** Pyro's recipe, GPU-friendly.
- 2026-11-12 — **Domain is format × scale; the sim ignores proxy and RoI (user):** output is resampled, so proxy never changes the look.
- 2026-11-12 — **CPU path alongside the GPU kernels (user).**
- 2026-11-12 — **Ships as F20 - 2D Fluids with M411 - 2D Fluid Fire And Controls (user).**

Shared design: `PLAN/DECISIONS/2026-11-12-geometry-simulation-expansion.md`.
