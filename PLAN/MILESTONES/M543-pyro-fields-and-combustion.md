# M543 - Pyro Fields And Combustion

`PyroSolver`'s Fields tab and fire: density and temperature with their control fields and flame emission, a flame field with lifespan, flame expansion into the pressure solve's goal divergence, colour and alpha fields, the speed field, viscosity, and the Field Guides viewer overlays. After this milestone a burning sphere gives fire and smoke.

Member of F35 - Pyro: branches off and merges into `feature/f35-pyro` (PLAN-FORMAT §9b); the Feature's UAT covers it. Epic: E22 - Simulation And Solvers. Shared design: `PLAN/DECISIONS/2026-11-22-vdb-pyro-solver.md`.

## Scope

**In:**
- Density: Dissipation (both controls in the reference), Clamp Below, Use Control Field with Control Field, Control Range, Compute Range, Remap Control Field, Control Ramp; Emit Density From Flame with Operation, Emission Scale, Flame Range, Compute Range, Remap Flame Range, Flame Ramp.
- Temperature: Temperature Diffusion, Cooling Rate, Emit Temperature From Flame with Operation, Strength, Emission Scale, Flame Range, Compute Range, Remap Flame Range, Flame Ramp.
- Flame: Create Flame Field, Flame Lifespan. Flame Expansion: Add Expansion, Expansion Rate, Flame Range, Remap Flame Range, Flame Ramp.
- Color: Create Cd And Alpha Fields, Default Color, Default Alpha, Dissipation, Only Dissipate Alpha, Remap Control Field, Control Field Ramp, Blur, Radius, Sharpening, Radius, Threshold. Speed: Calculate Speed Field. Viscosity.
- Field Guides: Field Guide (No Guide, Density, Temperature, Flame, Velocity, Speed, Divergence, Pressure, Collision, Voxel Size, Bounding Box, Active Region), Visualize As (3D, Plane), Plane Orientation, Plane Position, Guide Range, Compute Range, Divisions.
- A shared control-field remap helper (field, range, compute range, remap ramp) that M544 - Pyro Shaping Forces reuses.

**Out:**
- Disturbance, turbulence and shredding (M544 - Pyro Shaping Forces).
- Shading the fire for render (M547 - Pyro Look And Post-Process).

**Edge cases:**
- Compute Range on an empty field: keeps the previous range and sets an info message.
- Flame expansion in a closed domain: the pressure solve removes the mean (M541 - Pyro Advection And Pressure), with a warning that expansion needs an open side.
- A guide asked for a field that isn't simulated (Flame with Create Flame Field off): draws nothing, with a tooltip saying why.

## Phase 543.1: Fields

- [ ] M543.P1.T1 — Control-field helper, density and temperature
  - files: `Engine/Geo/Pyro/PyroControlField.{h,cpp}`, `Engine/Geo/Pyro/PyroFields.{h,cpp}`, `Engine/Gpu/Kernels/pyro_fields.slang`, `Engine/Gpu/Kernels/pyro_fields.cmake`, `Engine/Nodes/Pyro/PyroSolver.{h,cpp}`, `Tests/Native/PyroFields_Test.cpp`
  - approach: The helper maps a field through range and ramp into a 0–1 weight (compute range by a parallel min/max over active tiles). Density dissipation (optionally weighted), clamp below, emission from flame; temperature diffusion (M541's diffusion solve), cooling, emission from flame. CPU and Slang forms. Bundled gtest is old: no `GTEST_SKIP`, no `ASSERT_TRUE(ptr)`.
  - verify: `PyroFields_Test`: dissipation 0.1 for one second scales density by 0.9 within 1 %; a control ramp of zero disables dissipation where the control is low; cooling reduces temperature at the set rate; GPU matches CPU within 1e-4 relative.
  - size: M
- [ ] M543.P1.T2 — Flame field and flame expansion
  - files: `Engine/Geo/Pyro/PyroCombustion.{h,cpp}`, `Engine/Gpu/Kernels/pyro_fields.slang`, `Engine/Nodes/Pyro/PyroSolver.{h,cpp}`, `Tests/Native/PyroCombustion_Test.cpp`
  - approach: Flame decays over its lifespan and emits density and temperature through P1.T1's emission; expansion writes the goal-divergence field the pressure solve consumes, mapped through the flame range and ramp.
  - verify: `PyroCombustion_Test`: flame of lifespan 0.5 s reaches zero after 0.5 s without sourcing; positive expansion gives net outward flux equal to the integrated goal divergence within tolerance.
  - size: M
- [ ] M543.P1.T3 — Colour, speed and viscosity controls
  - files: `Engine/Geo/Pyro/PyroFields.{h,cpp}`, `Engine/Nodes/Pyro/PyroSolver.{h,cpp}`, `Tests/Native/PyroFields_Test.cpp`
  - approach: `Cd` and `alpha` fields with defaults, dissipation (alpha only when set), control-field remap, blur and unsharp sharpening with threshold; speed as |vel| into a `speed` field; the Viscosity knob drives M541's implicit viscosity.
  - verify: `PyroFields_Test`: Only Dissipate Alpha leaves `Cd` unchanged while alpha drops; speed equals |vel| at cell centres; sharpening raises local contrast only above the threshold.
  - size: M

## Phase 543.2: Guides

- [ ] M543.P2.T1 — Field Guides in the viewer
  - files: `Engine/Nodes/Pyro/PyroGuides.{h,cpp}`, `Engine/Nodes/Pyro/PyroSolver.{h,cpp}`, `Tests/Native/PyroGuides_Test.cpp`
  - approach: Build guide geometry as a side output the geometry display already draws (M376 - Geometry Display): a colour-ramped slice quad (Plane, with orientation, position, range and divisions), a sparse point/line cloud (3D, velocity as lines), and boxes for voxel size, bounding box and active tiles.
  - verify: `PyroGuides_Test`: the Active Region guide has one box per active tile; the Plane guide's texels equal the field sampled at the plane within ramp precision.
  - size: M

## Phase 543.3: Proof

- [ ] M543.P3.T1 — GUI check: fire
  - files: `Tests/gui/m543_pyro_fire.py`, `Engine/Nodes/Pyro/docs/PyroSolver.md`
  - approach: A burning sphere (flame and temperature sources) through `PyroSolver` with expansion, shown with the Temperature plane guide and the Active Region guide; docs for the Fields tab written fresh.
  - verify: the script passes under Xvfb; screenshots sent with SendUserFile for the Feature's UAT.
  - size: M

**Verification gate:** full ctest green including `PyroFields_Test`, `PyroCombustion_Test` and `PyroGuides_Test`; `Tests/gui/m543_pyro_fire.py` passes under Xvfb.
