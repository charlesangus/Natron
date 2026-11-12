# M416 - Geometry Deformers And Time

SOP deformers and time operators for native geometry: `GeoBend` (bend, twist, taper, squash and stretch, as the Bend SOP), `GeoLattice`, `GeoPointDeform`, `GeoMountain`, `GeoTimeShift` and `GeoTrail`. Capture-based deformers compute weights once from their rest inputs and cache them by those inputs' hash, so scrubbing the deformed input only re-applies weights (user, 2026-11-12). `GeoTrail` has all three of the SOP's modes (user, 2026-11-12).

Member of F21 - Geometry Operators: branches off and merges into `feature/f21-geometry-operators` (PLAN-FORMAT §9b); the Feature's UAT covers it. Epic: E9 - USD And 3D.

## Scope

**In:**
- `GeoBend`: capture region (origin, direction, length), bend angle, twist, taper, squash/stretch, with a capture-origin handle in the viewer if the transform-widget framework allows it cheaply.
- `GeoLattice`: rest and deformed cage inputs (a box lattice of divisions), trilinear or Bernstein (FFD) weights.
- `GeoPointDeform`: rest and deformed point inputs, capture by k nearest points within a radius with distance falloff, deforming `P` and optionally `N`/`orient`.
- `GeoMountain`: fractal noise displacement along `N` or a direction (reuses `GeoAttribNoise`'s noise kernel from M415 - Geometry Attribute Ops, which this milestone depends on).
- `GeoTimeShift`: evaluate the input at another frame (frame, offset, clamp range), cached per source frame.
- `GeoTrail`: Compute Velocity (backward or central difference, matching points by `id` or number), Preserve Original (N trailing copies with `trailframe`), Connect as Polylines.

**Out:**
- Bone/skin capture (none).
- Wire deformer (none).

**Edge cases:**
- Rest and deformed inputs with different point counts: node error naming both counts.
- Capture cache invalidation: keyed by rest-input hash and capture knobs; deformed-input changes reuse it.
- `GeoTrail` with changing point counts and no `id`: velocity by point number is wrong, so the node warns and writes zero `v` for unmatched points.
- `GeoTimeShift` outside the clamp range: holds the boundary frame.

**Assumptions:**
- Unconfirmed: `GeoTrail`'s velocity feeds motion blur through M405 - Particle Solver Core's `v` → `velocities` mapping; if F21 lands before F19, this milestone adds that mapping instead.

## Phase 416.1: Deformers

- [ ] M416.P1.T1 — `GeoBend` and `GeoMountain`
  - files: `Engine/Nodes/Geometry/GeoBend.{h,cpp,reg.inc}`, `Engine/Nodes/Geometry/GeoMountain.{h,cpp,reg.inc}`, `Tests/GeoBend_Test.cpp`
  - approach: Bend: map points into the capture frame, apply bend/twist/taper/squash per the SOP's parametrisation along the capture length, map back. Mountain: fractal noise along `N` or a direction, amplitude, frequency, octaves, seed.
  - verify: `GeoBend_Test`: bending a unit-length line by 90° gives a quarter arc of radius 2/π; twist of 180° rotates the end by 180°; mountain is deterministic per seed.
  - size: M
- [ ] M416.P1.T2 — Capture-cached `GeoLattice` and `GeoPointDeform`
  - files: `Engine/Nodes/Geometry/GeoLattice.{h,cpp,reg.inc}`, `Engine/Nodes/Geometry/GeoPointDeform.{h,cpp,reg.inc}`, `Engine/Geo/GeoCaptureCache.{h,cpp}`, `Tests/GeoDeformCapture_Test.cpp`
  - approach: A small capture cache keyed by (rest-input hashes, capture knobs) holding per-point weight lists. Lattice: trilinear or Bernstein weights in the rest cage's parametric space. PointDeform: k nearest rest points within radius (point tree), inverse-distance weights, local frames for `N`/`orient`.
  - verify: `GeoDeformCapture_Test`: identical rest and deformed inputs leave geometry unchanged; translating the deformed cage translates the mesh; scrubbing the deformed input performs no recapture (counted).
  - size: L

## Phase 416.2: Time

- [ ] M416.P2.T1 — `GeoTimeShift` and `GeoTrail`
  - files: `Engine/Nodes/Geometry/GeoTimeShift.{h,cpp,reg.inc}`, `Engine/Nodes/Geometry/GeoTrail.{h,cpp,reg.inc}`, `Tests/GeoTrail_Test.cpp`
  - approach: Both declare their extra frames through `getFramesNeeded()` so `evaluateGeometry()` pulls them. TimeShift returns the input at the target frame. Trail computes `v` by differences matched by `id` (or number), keeps N trailing copies with `trailframe`, or connects them as polylines per point.
  - verify: `GeoTrail_Test`: a point moving +1 per frame at 24 fps gets `v = 24` (central); Preserve Original with 3 trails has 4× points; Connect makes one polyline per point; TimeShift −1 equals the previous frame.
  - size: M

## Phase 416.3: Proof

- [ ] M416.P3.T1 — GUI check and docs
  - files: `Tests/gui/m416_deformers_time.py`, `Engine/Nodes/Geometry/docs/*.md`
  - approach: A bent, lattice-deformed tube; a point-deformed mesh driven by animated points; trails on an animated point. Docs written fresh.
  - verify: the script passes under Xvfb; screenshots sent with SendUserFile for the Feature's UAT.
  - size: M

**Verification gate:** full ctest green including `GeoBend_Test`, `GeoDeformCapture_Test` and `GeoTrail_Test`; `Tests/gui/m416_deformers_time.py` passes under Xvfb.

## Decisions

- 2026-11-12 — **`GeoTrail` has all three SOP modes (user, /cat-plan grill).**
- 2026-11-12 — **Capture-based deformers cache rest-input weights (user):** scrubbing the deformed input only re-applies them.

Shared design: `PLAN/DECISIONS/2026-11-12-geometry-simulation-expansion.md`.
