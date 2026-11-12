# M411 - 2D Fluid Fire And Controls

Fire, richer sourcing, moving colliders and shading for M410 - 2D Fluid Solver Core's `Fluid2D`. Fuel burns into flame and heat. Source and collider velocity can come from motion-vector layers (VectorGenerator from F8 - Motion Vectors, or any vector layer), so smoke follows plate motion and is pushed by moving objects (user, 2026-11-12). A separate `Fluid2DShade` node turns density and flame into colour without lighting (user, 2026-11-12), so look changes never re-sim.

Member of F20 - 2D Fluids: branches off and merges into `feature/f20-2d-fluids` (PLAN-FORMAT §9b); the Feature's UAT covers it. Epic: E22 - Simulation And Solvers.

## Scope

**In:**
- Combustion: a `fuel` field from the Source (chosen channel), ignition temperature, burn rate, flame lifetime, heat and smoke emitted per burned fuel, gas expansion (divergence source). Output layers gain `fuel` and `flame`.
- Source modes per field: Add, Max, Replace, with per-field scales; source velocity from a chosen vector layer of the Source input with a scale.
- Moving colliders: Collision alpha plus an optional collider velocity vector layer, enforced as a moving boundary.
- Forces: wind (direction, speed), an image-driven force layer, turbulence controls (already in M410) exposed per frequency band.
- `Fluid2DShade`: density → smoke colour by absorption and scatter colour; flame → emission by blackbody temperature or a ramp, with intensity; outputs premultiplied scene-linear RGBA.

**Out:**
- Lighting and self-shadowing in `Fluid2DShade` (user, 2026-11-12: omitted).
- 3D fire (none).

**Edge cases:**
- Vector layer in pixels per frame vs per second: converted using the project frame rate and the sim's substeps; documented in the tooltip.
- Vector layer missing on the Source: the velocity-source knob errors with the layer name.
- Collider velocity without collider alpha: ignored with an info message.
- Blackbody at very high temperature: normalised so intensity stays the knob's, not physical radiance, unless "Physical" is on.

**Assumptions:**
- Unconfirmed: VectorGenerator's output layer convention (forward vectors, pixels per frame) is the one `Fluid2D` reads by default.

## Phase 411.1: Combustion and sourcing

- [ ] M411.P1.T1 — Fuel, flame and expansion
  - files: `Engine/Gpu/Kernels/fluid2d.slang`, `Engine/Nodes/Fluid/Fluid2DKernels.{h,cpp}`, `Engine/Nodes/Fluid/Fluid2D.{h,cpp}`, `Tests/Native/Fluid2D_Test.cpp`, `Tests/Gpu/Fluid2DKernelsGpu_Test.cpp`
  - approach: Add `fuel` and `flame` fields with a burn kernel (fuel above ignition temperature burns at a rate, producing heat, smoke and flame, and a divergence source for expansion that the projection honours). CPU and GPU forms.
  - verify: `Fluid2D_Test`: a fuel source above ignition produces flame and rising heat, and total fuel decreases at the burn rate; GPU matches CPU within tolerance.
  - size: L
- [ ] M411.P1.T2 — Source modes, vector-driven velocity and moving colliders
  - files: `Engine/Nodes/Fluid/Fluid2D.{h,cpp}`, `Engine/Gpu/Kernels/fluid2d.slang`, `Engine/Nodes/Fluid/Fluid2DKernels.{h,cpp}`, `Tests/Native/Fluid2D_Test.cpp`
  - approach: Per-field Add/Max/Replace with scales; read a chosen vector layer from Source as source velocity and from Collision as collider velocity (converted to sim units), enforcing collider velocity as the boundary condition in projection. Wind and image-driven force layer.
  - verify: `Fluid2D_Test`: a source moving with a matching vector layer leaves a trail that follows it; a collider moving right pushes resting smoke right; Replace mode clamps density to the source inside it.
  - size: L

## Phase 411.2: Shading

- [ ] M411.P2.T1 — `Fluid2DShade` node
  - files: `Engine/Nodes/Fluid/Fluid2DShade.{h,cpp,reg.inc}`, `Engine/Gpu/Kernels/fluid2dshade.slang`, `Engine/Gpu/Kernels/fluid2dshade.cmake`, `Tests/Native/Fluid2DShade_Test.cpp`
  - approach: A point-op image node reading the fluid layers: smoke alpha = 1 − exp(−density·absorption), colour from a scatter colour; flame emission from blackbody (CIE-derived, in the working space via OCIO) or a colour ramp, times intensity; composite emission over smoke, premultiplied. GPU and CPU forms. No lighting.
  - verify: `Fluid2DShade_Test`: zero density gives zero alpha; blackbody at 1500 K is orange-red and at 6500 K near-white in linear sRGB; GPU matches CPU.
  - size: M

## Phase 411.3: Proof

- [ ] M411.P3.T1 — GUI check and docs
  - files: `Tests/gui/m411_fluid2d_fire.py`, `Engine/Nodes/Fluid/docs/Fluid2D.md`, `Engine/Nodes/Fluid/docs/Fluid2DShade.md`
  - approach: A torch: a moving source with a synthetic vector layer, fuel burning, a moving collider, shaded through `Fluid2DShade`; play 1–48.
  - verify: the script passes under Xvfb; frame-24 and frame-48 screenshots sent with SendUserFile for the Feature's UAT.
  - size: M

**Verification gate:** full ctest green including `Fluid2D_Test`, `Fluid2DKernelsGpu_Test` (lavapipe) and `Fluid2DShade_Test`; `Tests/gui/m411_fluid2d_fire.py` passes under Xvfb.

## Decisions

- 2026-11-12 — **Raw layers plus a separate `Fluid2DShade`, without lighting or self-shadowing (user, /cat-plan grill).**
- 2026-11-12 — **Source and collider velocity from motion-vector layers (user):** smoke follows plate motion and moving objects push it.

Shared design: `PLAN/DECISIONS/2026-11-12-geometry-simulation-expansion.md`.
