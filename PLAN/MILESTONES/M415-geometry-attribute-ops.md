# M415 - Geometry Attribute Ops

The SOP attribute and query operators: `GeoAttribTransfer`, `GeoAttribNoise`, `GeoAttribBlur`, `GeoAttribRandomize`, `GeoMeasure`, `GeoConnectivity`, `GeoSort` and `GeoRay`. Spatial queries go through a cached triangle BVH next to the wrangle's existing `nearpoint` point tree (user, 2026-11-12). With the BVH in place, the kernel language gains the surface-query builtins `intersect`, `minpos`, `xyzdist` and `primuv` for `GeoWrangle`, and `PopWrangle` and `VolumeWrangle` get them through the shared binding (user, 2026-11-12).

Member of F21 - Geometry Operators: branches off and merges into `feature/f21-geometry-operators` (PLAN-FORMAT §9b); the Feature's UAT covers it. Epic: E9 - USD And 3D.

## Scope

**In:**
- `GeoBVH`: a cached triangle BVH over polygon prims (triangulated on the fly), keyed by topology and `P` hash, with ray, closest-point and primitive-UV queries.
- `GeoAttribTransfer`: transfer point/prim/vertex attributes from a source by distance threshold, blend width and kernel, using the point tree or the BVH.
- `GeoAttribNoise`: add noise to any float attribute (Perlin, simplex, worley, curl), with amplitude, frequency, offset, animation, fractal octaves.
- `GeoAttribBlur`: diffuse an attribute over point connectivity or proximity, iterations and mode.
- `GeoAttribRandomize`: random values by distribution (uniform, gaussian, direction, orientation), seeded per id or element.
- `GeoMeasure`: area, perimeter, volume, curvature, gradient and Laplacian per element or per piece.
- `GeoConnectivity`: `class` attribute per connected component (points or prims), reusing M402 - Geometry Loops' `GeoConnectivity` helper.
- `GeoSort`: sort points or prims by attribute, axis, proximity, random or reverse.
- `GeoRay`: project points onto a surface along a direction or `N`, or to the closest point, with transform-points and hit attributes.
- Kernel builtins `intersect`, `minpos`, `xyzdist`, `primuv` on the CPU.

**Out:**
- GPU forms of the new builtins (none; they mark a program CPU-only under M408 - Kernel Language GPU Backend).
- Volume-based closest-point queries (none).

**Edge cases:**
- Source with no polygons for BVH-based modes: falls back to the point tree, with an info message.
- `GeoAttribTransfer` attribute type mismatch: destination keeps its type; incompatible types error naming the attribute.
- `GeoRay` misses: points stay, with `hitprim = -1`, matching the SOP.
- Noise on integer attributes: rounds; on string attributes: error.

**Assumptions:**
- Unconfirmed: noise kernels come from `libs/noise` / M129 - Clean-Sheet Noise Node.

## Phase 415.1: Spatial core

- [ ] M415.P1.T1 — `GeoBVH` and the surface-query builtins
  - files: `Engine/Geo/GeoBVH.{h,cpp}`, `Engine/Geo/GeoKernelBinding.{h,cpp}`, `Tests/GeoBVH_Test.cpp`, `Tests/GeoWrangle_Test.cpp`
  - approach: SAH-binned BVH over triangles of polygon prims, built in parallel, cached in the detail's derived-data cache. Queries: ray (first hit, all hits), closest point with prim and parametric UV. Add `intersect`, `minpos`, `xyzdist`, `primuv` to the binding with VEX-compatible signatures on input indices; reads from the second input build its BVH once per run.
  - verify: `GeoBVH_Test` checks ray hits and closest points against brute force on random triangles; `GeoWrangle_Test` cases for each builtin on a unit sphere give the analytic results.
  - size: L

## Phase 415.2: Nodes

- [ ] M415.P2.T1 — `GeoAttribTransfer` and `GeoRay`
  - files: `Engine/Nodes/Geometry/GeoAttribTransfer.{h,cpp,reg.inc}`, `Engine/Nodes/Geometry/GeoRay.{h,cpp,reg.inc}`, `Tests/GeoAttribTransfer_Test.cpp`, `Tests/GeoRay_Test.cpp`
  - approach: Follow the SOPs' knob sets. Transfer by point tree for points, BVH for prims/vertices, with threshold, blend width and kernel falloff. Ray by direction, `N` or minimum distance, writing `hitprim`, `hitu/v`, `dist`.
  - verify: tests: `Cd` transferred from a coloured plane onto a grid above it within distance; ray-projecting a grid onto a sphere puts every point on its surface.
  - size: M
- [ ] M415.P2.T2 — `GeoAttribNoise`, `GeoAttribBlur`, `GeoAttribRandomize`
  - files: `Engine/Nodes/Geometry/GeoAttribNoise.{h,cpp,reg.inc}`, `Engine/Nodes/Geometry/GeoAttribBlur.{h,cpp,reg.inc}`, `Engine/Nodes/Geometry/GeoAttribRandomize.{h,cpp,reg.inc}`, `Tests/GeoAttribOps_Test.cpp`
  - approach: Page-batched loops over the chosen attribute class; blur over connectivity (reverse topology) or proximity (point tree); randomize seeded from `id` when present for stability across topology changes.
  - verify: `GeoAttribOps_Test`: noise is deterministic per seed and changes with offset; blur of a delta spreads symmetrically and conserves the sum in connectivity mode; randomized directions are unit length.
  - size: M
- [ ] M415.P2.T3 — `GeoMeasure`, `GeoConnectivity`, `GeoSort`
  - files: `Engine/Nodes/Geometry/GeoMeasure.{h,cpp,reg.inc}`, `Engine/Nodes/Geometry/GeoConnectivity.{h,cpp,reg.inc}`, `Engine/Nodes/Geometry/GeoSort.{h,cpp,reg.inc}`, `Tests/GeoMeasure_Test.cpp`
  - approach: Measure area/perimeter/volume (divergence theorem), curvature (cotangent Laplacian), gradient; Connectivity wraps the shared helper; Sort reorders by building a permuted detail through `GeoDetailBuilder`.
  - verify: `GeoMeasure_Test`: unit-cube area 6 and volume 1; sphere curvature ≈ 1/r; three merged boxes get classes 0–2; sorting by `P.x` orders points ascending.
  - size: M

## Phase 415.3: Proof

- [ ] M415.P3.T1 — GUI check and docs
  - files: `Tests/gui/m415_attribute_ops.py`, `Engine/Nodes/Geometry/docs/*.md`
  - approach: A chain colouring a mesh by curvature, transferring it to scattered points, noising and blurring it, displayed in the viewer. Docs written fresh.
  - verify: the script passes under Xvfb; screenshots sent with SendUserFile for the Feature's UAT.
  - size: M

**Verification gate:** full ctest green including `GeoBVH_Test`, `GeoWrangle_Test` and the new node tests; `Tests/gui/m415_attribute_ops.py` passes under Xvfb.

## Decisions

- 2026-11-12 — **Shared triangle BVH plus the existing point tree (user, /cat-plan grill),** not VDB-based closest-point.
- 2026-11-12 — **Adds `intersect`, `minpos`, `xyzdist`, `primuv` wrangle builtins (user).**

Shared design: `PLAN/DECISIONS/2026-11-12-geometry-simulation-expansion.md`.
