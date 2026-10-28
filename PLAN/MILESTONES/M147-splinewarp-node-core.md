# M147 - SplineWarp Node Core

A native `SplineWarp` node that warps its Source input according to the difference between linked source and destination curves:
- Curves can be open, closed or single-point.
- Closed curves can be marked as boundaries, which may be feathered.
- Correspondence points, which are animatable, refine the pairing.
- There is a per-link amount and a global amount.
- An invert toggle.
- A thin-plate-spline solver evaluated on a coordinate field.
- Upstream transforms concatenate into its single resample.

The node is complete and scriptable from Python, but **registered as internal (hidden)**. M148 - SplineWarp Viewer Tools adds the viewer tools and makes it visible.

Deps:
- M146 - Roto Hosting For Any Node (`RotoCurveContext`, the capability gate, the panel hook)
- M113 - Native Warp Nodes (`resampleRowFromCoords`, the Jacobian-from-coordinates helper)
- M120 - Native Node Port Kit (per-node registration fragments)
- M137 - Native Base Class Flexibility (the Custom kind)

Scouted facts (2026-10-20, `main` at `8fdb5741f`; re-check at promotion):
- **Curve evaluation** (`Engine/Bezier.h`):
  - `evaluateAtTime_DeCasteljau` (`:379/:390`) and `evaluateFeatherPointsAtTime_DeCasteljau` (`:430/:442`) return `ParametricPoint` lists.
  - The static `bezierEval`/`bezierPoint`/`bezierFullPoint` are at `:96-117`.
  - `isPointOnCurve(x,y,acceptance,&t,&feather)` is at `:~164`.
  - Open curves come from `Bezier(ctx,name,layer,isOpenBezier)` (`:83`), queried with `isOpenBezier()` (`:122`).
  - Single-CP curves are supported (`Bezier.cpp:406,1422`).
  - Per-CP animation lives in `BezierCP` X/Y/tangent `Curve`s (`BezierCP.h:76-81`).
- **No warp prior art in the tree:** no TPS, RBF, MVC or Delaunay code. Eigen is vendored at `libs/Eigen3`, but only `ceres` adds it as an include path (`libs/ceres/CMakeLists.txt:249`). No Engine source includes it yet.
- **Concatenation is upstream-only.**
  - `EffectInstance::tryConcatenateTransforms` (`EffectInstance.cpp:2208-2315`) multiplies upstream 3x3 matrices, and the consumer fetches with `getImage(..., &inputTransform)`.
  - Native Transform folds it in through `Resampler::concatenateInputTransform` (`Engine/Nodes/Image/Resampler.h:461`; `Transform.cpp:1134,1184`).
  - M113 - Native Warp Nodes's IDistort applies the concatenated matrix to its per-pixel sample coordinates. SplineWarp follows the same pattern.
- **Native RoD/RoI pattern:** `TransformNode::getRegionOfDefinition` (`Transform.cpp:962`) and `getRegionsOfInterest` (`:982-1030`). The mask input gets the full render window.
- **Resampler:** the `FilterEnum` options are at `Resampler.h:87/116`, and `interpolateSuper` (with Jacobian) at `:337`. M113.P1.T1 adds `resampleRowFromCoords` and the Jacobian helper.
- **Python:** `Engine/PyRoto.{h,cpp}` and `Engine/typesystem_engine.xml:1871-2120`. Shiboken can swallow exceptions on define-ownership bindings (see the project memory on Shiboken ownership), so test error paths explicitly.

## Scope

**In:**
- The `SplineWarp` v1 node: a Source input, host mask and mix, and a geometry curve context (M146 - Roto Hosting For Any Node). It uses plain ID `SplineWarp` at major 1 and is registered internal.
- **Curve roles:**
  - A curve is **unlinked** (ignored by the warp), **linked** (the source or destination of exactly one link), or a **boundary**.
  - Only closed curves may be boundaries.
  - A boundary is never linked.
- **Links:** any number per node, and one link per curve. A link has:
  - an alignment (a start-parameter offset and a reversed flag);
  - an animatable 0–1 amount;
  - any number of animatable correspondences, each a source-curve parameter paired with a destination-curve parameter.
- **Auto-alignment** for linking two existing curves: it picks the start offset and direction that minimise the centroid-relative pairing distance. Duplicates align at zero.
- **Warp semantics:**
  - Correspondences split each linked pair into spans. Each span is paired by arc length.
  - A single-point curve links point to point.
  - Each link's destination is lerped toward its source by `globalAmount × linkAmount`.
  - `invert` swaps every link's source and destination.
  - The solve maps destination positions to source positions directly, so it is the inverse map, with no iteration.
- **Boundaries:**
  - With at least one boundary, nothing outside the union of the boundaries' feather curves moves.
  - Zero-displacement constraints sit on each boundary's outer (feather) curve.
  - Between a boundary's curve and its feather, the displacement is also scaled by a ramp from 1 at the curve to 0 at the feather, shaped by `featherFallOff`.
  - With no feather, the curve is the edge.
- **Solver:** a TPS with a `smoothness` regularisation knob (0 means exact fit). It falls back to translation for one sample, a similarity for two, and least-squares affine for collinear samples. The solver choice knob has TPS as its only option in this milestone.
- **Coordinate field:** the solver is evaluated on a grid whose step is a `field resolution` knob in full-scale pixels, scaled by the render scale. The field is bilinear between grid points, and its central differences give the per-pixel Jacobian for filtering.
- **Knobs:** amount, invert, solver, smoothness, curve resolution, field resolution, filter (Resampler's 10), clamp, black outside, and bbox (Source / Source ∪ warped).
- Upstream transform concatenation into the single resample.
- Python bindings for roles, links, alignment, amounts and correspondences.

**Out:**
- Viewer tools, toolbar, overlay colours, panel columns, making the node visible, and docs → M148 - SplineWarp Viewer Tools.
- The mesh solver → M149 - SplineWarp Mesh Solver.
- The GPU path → M150 - SplineWarp GPU Kernel.
- Motion blur → M151 - SplineWarp Motion Blur.
- Track-linked control points and copying curves from Roto → M152 - Curve Track Links And Roto Copy.
- Downstream concatenation, and absorbing a SplineWarp into another warp → M153 - Distortion Concatenation.
- Morphing, dissolving and a second input → none (user: out of scope).
- Fold detection and warnings → none (user, grill).
- Unlinked curves acting as pins → none (user, grill).

**Edge cases:**
- No links, or a global amount of 0: identity on Source (`isIdentity`).
- Links but every link's amount is 0: identity.
- A boundary with no links inside it: the field is zero there, giving identity, but it still costs a solve. Short-circuit when no link sample lies inside any boundary.
- A link whose destination crosses itself or folds the map: rendered as solved. The source is sampled twice, and that never fails.
- A link between an open curve and a closed curve: paired by arc length over the open curve's span. The closed curve is treated as cut at its aligned start.
- Mismatched CP counts: arc-length pairing makes them irrelevant.
- Inserting or removing a CP on a linked curve: each correspondence's parameter on that curve is remapped, so it stays at the same position on the curve.
- Deleting a linked curve deletes its link. Deleting a curve that a correspondence references deletes the correspondence with the link.
- Marking an open curve as a boundary is refused, with an error from Python. Opening a boundary curve clears its boundary role.
- Coincident or near-duplicate samples from different links (conflicting constraints): with `smoothness` 0 the TPS system is singular there. Merge samples closer than 1e-3 px by averaging their targets.
- A huge sample count: `curve resolution` bounds it, and the solve is capped at a fixed sample budget by uniformly decimating spans. The budget is set in P2.T2 from a timing test.
- Render scale below 1: the curves and field step scale. The field is solved in canonical coordinates, so draft and full renders agree.
- Bbox Source ∪ warped: the RoD is the union of the source RoD and the field's forward image of the source RoD corners and the destination curve bounds.

**Assumptions:**
- `resampleRowFromCoords` from M113 - Native Warp Nodes takes a per-pixel coordinate row plus an optional Jacobian, as its brief says.
- M137 - Native Base Class Flexibility's Custom kind lets a node own `render`, RoD and RoI, as Keyer and Merge do.
- A dense TPS solve at the sample budget (about 1–2k samples) runs well under a second per frame on a modest machine. P2.T2 measures it, and lowers the budget or moves to a blocked solver if not.

## Phase 147.1: Warp curve model

- [ ] M147.P1.T1 — `SplineWarpContext`: roles, links, amounts and correspondences
  - files: `Engine/Nodes/Transform/SplineWarp/SplineWarpContext.{h,cpp}` (new), `Engine/CMakeLists.txt`, `Tests/Native/SplineWarpModel_Test.cpp` (new)
  - approach:
    - Subclass `RotoCurveContext`. Each curve has a role: unlinked or boundary; linked is derived from link membership.
    - `WarpLink` holds `{src, dst, startOffset, reversed, amount (Curve), correspondences: vector<{srcParam Curve, dstParam Curve}>}`.
    - A parameter is `u ∈ [0, segmentCount]` (segment index plus t), so it is stable under CP moves.
    - Enforce one link per curve, no boundary links, and closed-only boundaries.
    - Remap correspondence parameters on CP insert or remove through the base's item-change hooks. Delete the link with either curve.
    - Fold links, roles and correspondences into the content hash.
  - verify: the new test covers creating a link, a second link on the same curve refused, a boundary on an open curve refused, CP insertion keeping a correspondence at the same position (to 1e-6), deleting a curve removing its link, and the hash changing on each edit. It runs at the phase build.
  - size: L

- [ ] M147.P1.T2 — Serialize SplineWarp curve data
  - files: `Serialization/SplineWarpSerialization.h` (new), `Serialization/CMakeLists.txt` (if headers are listed), `Engine/Nodes/Transform/SplineWarp/SplineWarpContext.cpp`, `Tests/Native/SplineWarpModel_Test.cpp`
  - approach:
    - Add a struct carried alongside the base context serialization: roles, plus links by curve script-name with their alignment, amount curve and correspondence curves.
    - Load resolves the names after the curves, dropping any link whose curve is missing with a log line.
    - There is no compatibility path: this is a new node.
  - verify: a round-trip test saves and loads a project with two links, animated correspondences, a boundary and a single-point link, and compares the model field by field.
  - size: M

- [ ] M147.P1.T3 — Auto-alignment when linking existing curves
  - files: `Engine/Nodes/Transform/SplineWarp/SplineWarpAlign.{h,cpp}` (new), `Engine/Nodes/Transform/SplineWarp/SplineWarpContext.cpp`, `Tests/Native/SplineWarpAlign_Test.cpp` (new)
  - approach:
    - `computeAlignment(src, dst, time)`. For closed curves, try each start offset on a fine arc-length grid, both directions, and minimise the sum of squared centroid-relative distances under arc-length pairing. For open curves, choose reversed or not.
    - The link-creation API calls it unless told the pair is a duplicate, which uses offset 0, not reversed.
  - verify: the test covers a rotated, re-started copy of a closed curve recovering its offset and direction, a reversed open curve, and different CP counts on the same shape.
  - size: M

## Phase 147.2: Solver and coordinate field

- [ ] M147.P2.T1 — Sample links and boundaries into constraint pairs
  - files: `Engine/Nodes/Transform/SplineWarp/SplineWarpSampling.{h,cpp}` (new), `Tests/Native/SplineWarpSampling_Test.cpp` (new)
  - approach:
    - At a time, emit `(dstPt, srcPt)` pairs. Each link is split at its correspondences (plus its aligned start) into spans, and each span is sampled by arc length at `curve resolution` spacing. A single-point link gives one pair.
    - Lerp the destination toward the source by `globalAmount × linkAmount`, and swap the sides under `invert`.
    - Boundaries contribute zero-displacement pairs along their feather curve, or the curve itself with no feather.
    - Merge pairs closer than 1e-3 px, and enforce the sample budget by uniform decimation per span.
  - verify: the test checks the counts and positions for a straight-line link, correspondences shifting the pairing, amount 0.5 halving displacement at the samples, invert, boundary pairs having zero displacement, and the merge and budget rules.
  - size: M

- [ ] M147.P2.T2 — TPS solver with smoothness and degenerate fallbacks
  - files: `Engine/Nodes/Transform/SplineWarp/SplineWarpTps.{h,cpp}` (new), `Engine/CMakeLists.txt` (Eigen include for Engine), `Tests/Native/SplineWarpTps_Test.cpp` (new)
  - approach:
    - Solve the 2D TPS (the `r² log r` kernel plus an affine part) with Eigen. Use a dense LU or LDLT of `K + λI` with λ from `smoothness`.
    - Fall back to translation (1 sample), similarity (2), and least-squares affine (collinear).
    - Return an evaluator `(x,y) → (sx,sy)`, cached by the sample-set hash.
    - Time a solve at 500, 1,000 and 2,000 samples in the test, and set the budget so a solve takes under about 200 ms on the dev host.
  - verify: the test covers exact interpolation at the samples with smoothness 0 (1e-6), an affine-only set reproducing the affine exactly everywhere, each fallback, smoothness above 0 reducing the bending energy, and the timing log.
  - size: L

- [ ] M147.P2.T3 — Coordinate field with boundary masking and feather ramp
  - files: `Engine/Nodes/Transform/SplineWarp/SplineWarpField.{h,cpp}` (new), `Tests/Native/SplineWarpField_Test.cpp` (new)
  - approach:
    - Evaluate the solver on a grid over the render window plus a margin. The step is `field resolution × renderScale`.
    - Store the source coordinates, and expose bilinear lookup and a per-pixel Jacobian from the field's central differences.
    - With boundaries:
      - rasterize the union of the boundaries' feather polygons into a weight on the same grid;
      - weight 1 inside the curve, a `featherFallOff`-shaped ramp to 0 at the feather, and 0 outside;
      - the displacement is multiplied by the weight.
    - Cache the field by (sample hash, time, render scale, window).
  - verify: the test covers identity samples giving an identity field (1e-6), a translation link giving uniform displacement, a pixel outside the boundary having zero displacement, the ramp's monotone profile across a feather, and the Jacobian of a known affine map matching the analytic one (1e-5).
  - size: L

## Phase 147.3: The node

- [ ] M147.P3.T1 — `SplineWarp` node: knobs and CPU render
  - files: `Engine/Nodes/Transform/SplineWarp/SplineWarp.{h,cpp}` (new), `Engine/Nodes/Transform/SplineWarp.reg.inc` (new, per M120 - Native Node Port Kit), `Tests/Native/NativeSplineWarp_Test.cpp` (new)
  - approach:
    - A Custom-kind node on M137 - Native Base Class Flexibility's framework.
    - It returns `Geometry` from `getCurveContextKind`, with `SplineWarpContext` as its context. Inputs are Source plus a mask, with host mask and mix.
    - Knobs are as in `## Scope`.
    - Render: build the field, then per row call `resampleRowFromCoords` with the field coordinates and Jacobian, the filter, clamp and black outside.
    - Register it internal.
  - verify: the test renders a ramp through a translation link (matching a Transform of the same offset within 1e-4 away from the edges), a boundary keeping the outside bit-exact, amount 0 being bit-exact identity, and invert of a translation giving the opposite shift.
  - size: L

- [ ] M147.P3.T2 — RoD, RoI and identity
  - files: `Engine/Nodes/Transform/SplineWarp/SplineWarp.cpp`, `Tests/Native/NativeSplineWarp_Test.cpp`
  - approach:
    - RoD follows the bbox knob: the source RoD, or its union with the field's forward image of the source RoD corners and the destination curve bounds.
    - The source RoI is the bounding box of the field's source coordinates over the render window, plus the filter's support, clipped to the source RoD.
    - The mask gets the render window.
    - `isIdentity` covers no links, a global amount of 0, or every link amount being 0 at that time. Short-circuit boundaries that hold no link samples.
  - verify: tests for the RoD under both bbox settings, an RoI that covers the sampled region (by rendering with a source cropped to the RoI and comparing), and the identity cases.
  - size: M

- [ ] M147.P3.T3 — Upstream transform concatenation
  - files: `Engine/Nodes/Transform/SplineWarp/SplineWarp.cpp`, `Tests/Native/NativeSplineWarp_Test.cpp`
  - approach:
    - Fetch the source with `&inputTransform`, as M113 - Native Warp Nodes's IDistort does.
    - Map each field source coordinate through the concatenated matrix's inverse before sampling, and transform the Jacobian with it.
    - Map the RoI through it with `transformInputRois`.
    - SplineWarp doesn't declare `getCanTransform`: it is a consumer, not a matrix.
  - verify: Transform (scale 0.5, rotate 30°) → SplineWarp matches the reference "apply the matrix and the warp in one resample" within 1e-4, and differs measurably from the same graph with concatenation disabled (a double resample).
  - size: M

## Phase 147.4: Python

- [ ] M147.P4.T1 — Python bindings for SplineWarp curve data
  - files: `Engine/PySplineWarp.{h,cpp}` (new), `Engine/typesystem_engine.xml`, `Engine/CMakeLists.txt`, `Tests/Native/SplineWarpPython_Test.cpp` (new)
  - approach:
    - Expose `node.roto` (from M146 - Roto Hosting For Any Node) plus a `SplineWarp` accessor:
      - `link(src, dst, duplicate=False)`, `unlink(curve)`, `links()`
      - `setBoundary(curve, bool)`
      - per-link `setAmount(value, time)`, `addCorrespondence(srcParam, dstParam, time)`, `correspondences()` and `remove`
    - Errors (an open-curve boundary, a second link) raise Python exceptions. Return early in inject-code where Shiboken ownership would swallow them.
  - verify: the test runs a Python snippet through the app's interpreter that builds two links and a boundary, renders, matches the C++-built equivalent bit-exactly, and checks that each error raises.
  - size: M

**Verification gate:**
- The build and the full ctest pass, including every new `SplineWarp*` and `NativeSplineWarp*` test.
- A scripted headless render (`NatronRenderer` with a Python script) of a fixture under `build/` matches its golden EXR:
  - two links, one with correspondences;
  - a single-point link;
  - an open-curve link;
  - a feathered boundary;
  - Transform → SplineWarp concatenation.
- The solve-timing log is recorded in `## Decisions`.
- No GUI check: the node is hidden until M148 - SplineWarp Viewer Tools.

## Decisions

- 2026-10-28: Phases 1–2 implemented (builds pending). Model: a start offset is a parameter on the destination curve (open destination: offset 0, direction only); links deleted on item removal, not structure change (drag between layers removes/re-inserts); single-point curves linkable; amount = static value + Curve. Phase 2: sample budget 1000 (provisional, confirm with the timing test); collinear points fall back to a least-squares affine; smoothness measured on unit-scaled point spread; links at amount 0 still pin; boundary weights use the Roto feather fall-off (masking exact per pixel, weight interpolated on the grid); field grid anchored to canonical coordinates (cache key: warp hash, time, scale, resolution, window). Known gaps: undo of a point removal doesn't restore correspondences; Python Layer.removeItem may skip the item-removed hook.
- 2026-10-20 — **Reuse the Roto curve stack, generalised (user):** SplineWarp curves are Roto Beziers in a geometry-only context from M146 - Roto Hosting For Any Node, so animation, undo, the panel and serialization come for free.
- 2026-10-20 — **Both solvers, user-selectable (user):** TPS here; the mesh solver is M149 - SplineWarp Mesh Solver.
- 2026-10-20 — **Upstream concatenation only here (user):** downstream is the engine-wide M153 - Distortion Concatenation.
- 2026-10-20 — **Unlinked curves are ignored by the warp (user, grill):** they are not pins.
- 2026-10-20 — **Boundaries are fixed and zero-displacement, with an optional feather (user, grill):** constraints sit on the feather curve, and a falloff ramp eases displacement to zero between the curve and its feather. (Planner: putting the constraint on the outer curve is what lets a feather soften anything, since a zero constraint on the inner curve would leave the band nothing to ease.)
- 2026-10-20 — **Correspondences are animatable (user, grill).**
- 2026-10-20 — **Per-link amount multiplies the global amount (user, grill):** applied as a lerp of the destination toward the source before the solve, so curves at amount 0.5 land halfway rather than having their displacement scaled after the fact.
- 2026-10-20 — **One link per curve (user, grill):** any number of pairs per node.
- 2026-10-20 — **Bbox knob, defaulting to the source RoD (user, grill).**
- 2026-10-20 — **Python bindings in this milestone (user, grill).**
- 2026-10-20 — **No fold guard (user, grill):** the dst→src TPS is single-valued, so folds can't fail.
- 2026-10-20 — **Invert toggle (user, grill).**
- 2026-10-20 — **Smoothness knob (user, grill).**
- 2026-10-20 — **Hidden until M148 - SplineWarp Viewer Tools (user, grill).**
