# M413 - Geometry Modeling Ops

A batch of SOP modeling operators for native geometry, each following the matching SOP (`PLAN/DECISIONS/2026-11-08-native-geometry-follows-sops.md`): `GeoClean`, `GeoDivide`, `GeoTriangulate` (constrained Delaunay, for N-gons with holes that `GeoDivide`'s ear clipping can't handle), `GeoSmooth`, `GeoPeak`, `GeoResample`, `GeoCarve`, `GeoSweep` and `GeoRevolve`. Curve operators work on polylines and on M420 - Spline Curve Primitive's Bezier curves. Built in-house on `GeoDetail` and `GeoReverseTopology`, keeping every attribute class and group as the SOPs do.

Member of F21 - Geometry Operators: branches off and merges into `feature/f21-geometry-operators` (PLAN-FORMAT §9b); the Feature's UAT covers it. Epic: E9 - USD And 3D.

## Scope

**In:**
- `GeoClean`: remove degenerate prims, unused points, duplicate prims, fix winding consistency, and make manifold (split non-manifold edges).
- `GeoDivide`: convex, triangulate (ear clipping for simple polygons), and brick/bricker; `GeoTriangulate` (constrained Delaunay via the vendored `libs/CDT`) for N-gons with holes and planar point sets.
- `GeoSmooth`: Laplacian/Taubin smoothing of `P` or any float attribute, with strength, iterations, boundary and group locks.
- `GeoPeak`: move along `N` by a distance.
- `GeoResample`: polylines and Beziers by segment length or count, with `curveu` and tangent output.
- `GeoCarve`: cut curves by U range (first/second U, extract/cut/break points).
- `GeoSweep`: sweep a cross-section (or a generated ribbon/tube/square) along curves, with scale and twist ramps, end caps, UVs.
- `GeoRevolve`: revolve curves about an axis, with divisions, start/end angle, caps.

**Out:**
- PolyBevel, PolyReduce, Remesh (M414 - Geometry Advanced Modeling).
- PolyExtrude, Boolean, Fuse (M362 - Geometry Operators II).
- NURBS (none).

**Edge cases:**
- Heavily tombstoned input: operators keep the holes and iterate in number order (M54 - Native Geometry Data Model's Feature-wide rule).
- Non-planar polygons in triangulate: projected onto the best-fit plane.
- Sweep along a curve with zero-length segments: tangents carried from the neighbour.
- Revolve of a curve touching the axis: points on the axis are fused to avoid degenerate faces.
- Smooth with groups: unselected points stay fixed and act as boundary.

**Assumptions:**
- Unconfirmed: `libs/CDT` (on the F15 branch) is MPL-2.0 and stays vendored.

## Phase 413.1: Cleanup and polygon ops

- [ ] M413.P1.T1 — `GeoClean` and `GeoDivide`
  - files: `Engine/Nodes/Geometry/GeoClean.{h,cpp,reg.inc}`, `Engine/Nodes/Geometry/GeoDivide.{h,cpp,reg.inc}`, `Tests/GeoClean_Test.cpp`, `Tests/GeoDivide_Test.cpp`
  - approach: Follow `GeoDelete`'s structure. Clean: per-option passes over prims and the reverse topology (consistent winding by BFS over shared edges). Divide: convex split and ear-clip triangulation preserving vertex attributes; bricker cuts by axis planes.
  - verify: tests cover each Clean option on crafted defects, and triangulation of a concave L-shape giving n−2 triangles with preserved `uv`.
  - size: M
- [ ] M413.P1.T2 — `GeoTriangulate`, `GeoSmooth`, `GeoPeak`
  - files: `Engine/Nodes/Geometry/GeoTriangulate.{h,cpp,reg.inc}`, `Engine/Nodes/Geometry/GeoSmooth.{h,cpp,reg.inc}`, `Engine/Nodes/Geometry/GeoPeak.{h,cpp,reg.inc}`, `Tests/GeoTriangulate_Test.cpp`, `Tests/GeoSmooth_Test.cpp`
  - approach: Triangulate through `libs/CDT` with constraints from polygon edges and holes. Smooth: Taubin (λ/μ) over point neighbours from the reverse topology, group and boundary locks. Peak: `P += N * d`, using point `N` or computing it.
  - verify: tests: a square with a square hole triangulates with no triangle inside the hole; Taubin smoothing of a noisy sphere reduces radius variance without shrinking the mean radius by more than 1 %.
  - size: M

## Phase 413.2: Curve ops

- [ ] M413.P2.T1 — `GeoResample` and `GeoCarve`
  - files: `Engine/Nodes/Geometry/GeoResample.{h,cpp,reg.inc}`, `Engine/Nodes/Geometry/GeoCarve.{h,cpp,reg.inc}`, `Tests/GeoResample_Test.cpp`
  - approach: Arc-length parameterise polylines and Beziers (M420 - Spline Curve Primitive's helpers); resample by length or count with attribute interpolation; write `curveu` and `tangentu`. Carve extracts or cuts by U ranges.
  - verify: `GeoResample_Test`: a 10-unit line at length 1 gives 11 points evenly spaced; a Bezier resample lies on the curve; carve 0.25–0.75 keeps the middle half.
  - size: M
- [ ] M413.P2.T2 — `GeoSweep` and `GeoRevolve`
  - files: `Engine/Nodes/Geometry/GeoSweep.{h,cpp,reg.inc}`, `Engine/Nodes/Geometry/GeoRevolve.{h,cpp,reg.inc}`, `Tests/GeoSweep_Test.cpp`
  - approach: Sweep: rotation-minimising frames along the backbone (double reflection), cross-section from input 2 or generated, scale/twist ramps over `curveu`, caps, `uv` from u and cross-section length. Revolve: rotate curve copies about the axis, stitch, cap, `uv`.
  - verify: `GeoSweep_Test`: a circle swept along a straight line is a cylinder of the right radius and length with consistent winding; revolving a line segment parallel to the axis gives a cylinder; a profile touching the axis has no degenerate faces.
  - size: L

## Phase 413.3: Proof

- [ ] M413.P3.T1 — GUI check and docs
  - files: `Tests/gui/m413_modeling_ops.py`, `Engine/Nodes/Geometry/docs/*.md`
  - approach: A short modeling chain using every node (curve → resample → sweep; profile → revolve; clean/divide/smooth on an imported OBJ) displayed in the viewer. Docs written fresh.
  - verify: the script passes under Xvfb; screenshots sent with SendUserFile for the Feature's UAT.
  - size: M

**Verification gate:** full ctest green including every new `Geo*_Test`; `Tests/gui/m413_modeling_ops.py` passes under Xvfb.

## Decisions

- 2026-11-12 — **Modeling list split on sizing (/cat-plan):** PolyBevel, PolyReduce and Remesh moved to M414 - Geometry Advanced Modeling.
- 2026-11-12 — **Curve ops handle a real Bezier primitive (user, /cat-plan grill),** added by M420 - Spline Curve Primitive, not polylines only.
- 2026-11-12 — **Ships in F21 - Geometry Operators with M362 - Geometry Operators II and the other operator batches (user).**

Shared design: `PLAN/DECISIONS/2026-11-12-geometry-simulation-expansion.md`.
