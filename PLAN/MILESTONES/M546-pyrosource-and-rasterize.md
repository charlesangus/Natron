# M546 - PyroSource And Rasterize

The two source-building geometry nodes: `PyroSource` scatters points over or inside geometry and gives them pyro attributes (density, temperature, flame, `v`, …), and `VolumeRasterizeAttributes` turns point attributes into named VDB grids for `PyroSolver`'s Sourcing tab. Independent of the solver, so it can run beside M540–M545.

Member of F35 - Pyro: branches off and merges into `feature/f35-pyro` (PLAN-FORMAT §9b); the Feature's UAT covers it. Epic: E22 - Simulation And Solvers. Shared design: `PLAN/DECISIONS/2026-11-22-vdb-pyro-solver.md`.

## Scope

**In:**
- `PyroSource`: Group, Group Type, Initialize (each reference preset: source smoke, fire, fuel, colour …), Mode (each reference mode: surface scatter, volume scatter, surface attributes, …), Particle Separation, Particle Scale, Density Attribute, Point / Vertex / Primitive / Detail Attributes, Deform Radius, Minimum Points, Maximum Points, and the Attributes list (Attribute, Vector Attribute, Name, Default Value, Scale).
- `VolumeRasterizeAttributes`: General (Group, Attributes, Source Attribute, Visible Volumes), Filter (Filter, Voxel Size, Particle Scale, Minimum Filter Size), Velocity Blur (Velocity Blur, Shutter, Shutter Offset, Blur Samples), Coverage (Coverage Attribute, Coverage Scale, Normalize By Clamped Coverage, Attribute Rules list with Attributes and Method).
- Output: VDB primitives (M417 - VDB Volume Primitive) named after the attributes.

**Out:**
- Burst, trail, spread, thruster and instancing source tools (none planned).
- The solver side of sourcing (M542 - PyroSolver Node And Sourcing).

**Edge cases:**
- Maximum Points reached: scattering stops and the info message says so (a user limit, not a safety cap).
- Points with no `pscale`: Particle Scale × Particle Separation is the radius.
- Vector attributes: rasterised to `Vec3fGrid`; `v` gets the velocity grid class.

**Assumptions:**
- Unconfirmed: the image's OpenVDB includes `openvdb/points` rasterisation tools; if not, rasterisation is written in-house over `GeoDetail`.

## Phase 546.1: Rasterize

- [ ] M546.P1.T1 — `VolumeRasterizeAttributes`
  - files: `Engine/Nodes/Pyro/VolumeRasterizeAttributes.{h,cpp,reg.inc}`, `Engine/Geo/Pyro/PyroRasterize.{h,cpp}`, `Tests/Native/VolumeRasterize_Test.cpp`
  - approach: Splat each point's attributes with a spherical filter of radius pscale × particle scale (at least the minimum filter size) into grids at the voxel size, in parallel per leaf; velocity blur splats Blur Samples copies over the shutter interval; coverage accumulates and optionally normalises; attribute rules pick per-attribute methods (maximum, average, density-weighted, …). Bundled gtest is old: no `GTEST_SKIP`, no `ASSERT_TRUE(ptr)`.
  - verify: `VolumeRasterize_Test`: one point of density 1 and radius r integrates to the filter's volume within 2 %; velocity blur stretches the splat along `v`; Maximum and Average rules give their values where two points overlap.
  - size: L

## Phase 546.2: Source

- [ ] M546.P2.T1 — `PyroSource`
  - files: `Engine/Nodes/Pyro/PyroSourceNode.{h,cpp,reg.inc}`, `Tests/Native/PyroSourceNode_Test.cpp`
  - approach: Scatter over surfaces with `GeoScatter`'s area-weighted sampler (M359 - Geometry Operators) or inside closed meshes at the particle separation; carry the listed attributes over; the Initialize presets fill the Attributes list; Deform Radius scales `pscale` with local stretch.
  - verify: `PyroSourceNode_Test`: surface scatter at separation s gives a point count within 10 % of area/s²; the Source Fire preset creates `flame` and `temperature` with their defaults.
  - size: M

## Phase 546.3: Proof

- [ ] M546.P3.T1 — GUI check and docs
  - files: `Tests/gui/m546_pyro_source.py`, `Engine/Nodes/Pyro/docs/PyroSource.md`, `Engine/Nodes/Pyro/docs/VolumeRasterizeAttributes.md`
  - approach: A torus → `PyroSource` (Source Fire) → `VolumeRasterizeAttributes` → `GeoToUSD` in the viewer; docs written fresh, naming no other application.
  - verify: the script passes under Xvfb; screenshots sent with SendUserFile for the Feature's UAT.
  - size: M

**Verification gate:** full ctest green including `VolumeRasterize_Test` and `PyroSourceNode_Test`; `Tests/gui/m546_pyro_source.py` passes under Xvfb.
