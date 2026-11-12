# M420 - Spline Curve Primitive

A Bezier curve primitive type for `GeoDetail`, so curve modeling (M413 - Geometry Modeling Ops' Sweep, Revolve, Resample, Carve) works on real splines, not only polylines (user, 2026-11-12: Bezier only). Today the detail has polygon, polyline and two packed primitive types (`Engine/Geo/GeoTopology.h:44`). The new type stores control points as ordinary vertices with an order per primitive. `GeoConvert` turns it into polylines, the bridges write and read `UsdGeomBasisCurves` (bezier basis), and the viewer draws it. Operators that don't understand splines convert them first, as Houdini's do.

Member of F21 - Geometry Operators: branches off and merges into `feature/f21-geometry-operators` (PLAN-FORMAT §9b); the Feature's UAT covers it. Epic: E9 - USD And 3D.

## Scope

**In:**
- `eGeoPrimitiveBezierCurve`: order (2–11, cubic default), open or closed, control points as vertices; vertex and primitive attributes as for polylines.
- Evaluation helpers: position, tangent and arc length at a parameter, used by curve ops.
- `GeoCurve` node: draw-free creation from a points string or the input's points, with order and close.
- `GeoConvert` node: Bezier → polyline (by level of detail or segment length) and polyline → Bezier (interpolating fit).
- Bridges: `GeoToUSD` writes `BasisCurves` with `type = cubic`, `basis = bezier` (other orders converted to polylines with an info message); `USDToGeo` reads bezier `BasisCurves` back as Bezier primitives.
- Display: the geometry display (M376 - Geometry Display) draws Bezier prims through the bridge; hulls drawn as guides when Points display is on.
- Existing operators: those that don't handle Bezier convert to polylines on the fly, with an info message.

**Out:**
- NURBS curves and surfaces (user, 2026-11-12: none).
- Interactive curve drawing tools in the viewer (none).

**Edge cases:**
- A Bezier prim whose vertex count isn't `order + k·(order−1)`: invalid, rejected at build with an error.
- Closed curve: the last segment wraps to the first control point.
- Merge of polylines and Beziers: kept as their own types.
- `GeoTransform` and other point operators move control points directly (correct for Bezier, which is affine-invariant).

**Assumptions:**
- M403 - Geometry File Cache's format must store the new type: whichever of the two lands second adds it (F18 and F21 are separate Features).
- Unconfirmed: Hydra Storm draws bezier `BasisCurves` acceptably at default refinement; otherwise the bridge raises `refinementLevel`.

## Phase 420.1: Primitive

- [ ] M420.P1.T1 — Bezier primitive type and evaluation
  - files: `Engine/Geo/GeoTopology.h`, `Engine/Geo/GeoBezier.{h,cpp}`, `Engine/GeoDetail.{h,cpp}`, `Tests/GeoBezier_Test.cpp`
  - approach: Add the enum value and a per-primitive order and closed flag (primitive-class intrinsic storage); validation in `GeoDetailBuilder`; de Casteljau evaluation, tangents, and arc length by adaptive Gauss quadrature. Include it in `contentHash()`.
  - verify: `GeoBezier_Test`: a cubic quarter-circle approximation evaluates within 3e-4 of the circle; arc length matches; invalid vertex counts are rejected; hash changes with order.
  - size: M

## Phase 420.2: Nodes and bridges

- [ ] M420.P2.T1 — `GeoCurve` and `GeoConvert`
  - files: `Engine/Nodes/Geometry/GeoCurve.{h,cpp,reg.inc}`, `Engine/Nodes/Geometry/GeoConvert.{h,cpp,reg.inc}`, `Engine/Geo/GeoOpUtils.{h,cpp}`, `Tests/GeoConvert_Test.cpp`
  - approach: `GeoCurve` builds one primitive from a coordinate string or input points; `GeoConvert` converts Bezier ↔ polyline, preserving and interpolating attributes. Add a shared `ensurePolylines()` helper in `GeoOpUtils` that existing curve-consuming operators call, with an info message.
  - verify: `GeoConvert_Test`: Bezier → polyline at LOD 10 gives 10 segments per span on the curve; polyline → Bezier → polyline round-trips within tolerance; attributes interpolate.
  - size: M
- [ ] M420.P2.T2 — Bridge and display support
  - files: `Engine/Nodes/Geometry/GeoUsdConvert.cpp`, `Tests/GeoToUSD_Test.cpp` (or M358's bridge test)
  - approach: In the partitioner, map Bezier prims to a `BasisCurves` partition with cubic/bezier, `curveVertexCounts`, `wrap` for closed; non-cubic orders become linear curves after conversion. `USDToGeo` maps bezier `BasisCurves` back.
  - verify: the bridge test round-trips a cubic Bezier through `GeoToUSD` and `USDToGeo` with identical control points; a GUI screenshot under Xvfb shows the curve drawn smoothly.
  - size: M

**Verification gate:** full ctest green including `GeoBezier_Test` and `GeoConvert_Test` and the bridge test; an Xvfb screenshot of a Bezier curve with hull guides sent with SendUserFile for the Feature's UAT.

## Decisions

- 2026-11-12 — **Bezier curves only (user, /cat-plan grill):** no NURBS curves or surfaces.

Shared design: `PLAN/DECISIONS/2026-11-12-geometry-simulation-expansion.md`.
