# M219 - Curves Follow Vectors

Move Roto and RotoPaint curves along motion vectors and bake the result into ordinary keyframes. The artist picks any node with motion layers as the vector source: an M217 - VectorGenerator Node, or a Read of a CG EXR with Forward/Backward layers. They also pick a motion model and a direction, then follow the selected shapes or points.

The artist's existing keys are anchors. Between two keys the follower tracks forward from the first and backward from the second, then crossfades the two so both ends land exactly. A low-confidence point is moved by the fitted motion of the shape's confident points, and the frames where that happened are reported. An optional keyframe-reduction pass thins the baked keys within a tolerance.

The work lives in M146 - Roto Hosting For Any Node's geometry base (`RotoCurveContext`, `RotoCurveInteract`), so M220 - SplineWarp Follow Vectors only has to expose it. See `DECISIONS/2026-10-25-motion-vectors-epic.md`.

Deps:
- M217 - VectorGenerator Node: the vectors, the `vectorConfidence` layer, and the synthetic fixtures.
- M146 - Roto Hosting For Any Node: `RotoCurveContext`, `RotoCurveInteract`, the toolbar builder and the panel hook.

Scouted facts (2026-10-25, `origin/main` at `3c8553ec`; re-check at promotion, after M146 merges):
- **Rendering another node's layers off the render path:** `Engine/TrackerFrameAccessor.cpp:298-353` calls `effect->renderRoI(args, &layers)` with its own per-frame cache. This is the pattern for sampling a vector source.
- **Bezier writes:**
  - `Bezier::setPointAtIndex(feather,index,time,x,y,lx,ly,rx,ry)` is at `Engine/Bezier.h:276`.
  - `setKeyframe(time)` is at `:338`, and `removeKeyframe(time)` at `:343`.
  - `getKeyframeTimes` (`:621`) and `getKeyframeTimesAndInterpolation` (`:623`) list a shape's keys. Bezier keys are per shape, shared by all of its points.
  - `BezierCP` has `setPositionAtTime`, `setLeftBezierPointAtTime` and `setRightBezierPointAtTime` (`BezierCP.h:90-94`).
- **Strokes** (`RotoStrokeItem`) store their points as `Curve`s keyed by sample index, not by frame (`RotoStrokeItem.h:82,136-137`). They can only move through the drawable transform:
  - `RotoDrawableItem::setTransform(time,tx,ty,sx,sy,cx,cy,rot,skewX,skewY)` (`RotoDrawableItem.h:171`)
  - `setExtraMatrix(setKeyframe,time,mat)` (`:173`), which carries perspective
  - `getTransformAtTime` (`:166`)
  - `setKeyframeOnAllTransformParameters` (`:158`)
- **Python:** `BezierCurve` in `Engine/PyRoto.h:118-136` (point setters and getters). `Roto` creators are at `:181-183`. Bindings are in `Engine/typesystem_engine.xml:1871-2120`. Shiboken can swallow exceptions on define-ownership bindings, so test error paths explicitly.
- **Background work with progress:** the tracker runs tracking on a scheduler with progress and abort (`TrackerContext::trackMarkers`, `TrackerContextPrivate.cpp:1101`). Mirror its threading and progress reporting.
- Eigen is vendored at `libs/Eigen3`. M217.P1.T1 puts it on the Engine include path.

## Scope

**In:**
- **Vector source:** any node whose output carries the Forward and Backward motion layers, picked by name. `vectorConfidence` is used when present. Without it, confidence is taken as 1 everywhere.
- **Direction and range:**
  - Track backward to the start of the range, back one frame, forward one frame, or forward to the end of the range.
  - The range is the project range, or an in/out pair.
- **Motion models**, chosen per follow:
  - **Per-point:** each CP, its tangents and its feather point sample their own vector, so the shape deforms.
  - **Translate, Affine or Perspective:** a robust fit (IRLS with a Huber weight, weighted by confidence) over vector samples on a grid inside the shape, plus its CPs. A fit keys the shape's transform, not its points.
  - Strokes always use a fit.
- **Anchors:** existing keys in the range on what is being followed. Those are point keys for per-point and transform keys for fits.
  - Between two anchors, the trajectory is forward-from-A blended with backward-from-B by a smoothstep weight over the gap.
  - Before the first anchor and after the last, tracking runs one way from the nearest one.
  - Following with no anchors starts from the current frame's state.
- **Occlusion:** a point whose confidence is below `confidenceThreshold` (default 0.5) moves by the shape's fit over its confident samples. A shape whose confident fraction falls below 25% stops following at that frame and keeps its last state.
- **Follow report:** per shape, the frame ranges that used inferred motion and the frame it stopped at, if any. It is shown in a node message and returned to Python.
- **Keyframe reduction,** optional, after baking: remove keys whose removal keeps every point within `tolerance` px (default 0.5) of the baked positions on every frame. Anchors are never removed.
- **One undo step per follow.**
- **UI:** a Follow Vectors toolbar group in `RotoCurveInteract` with:
  - a source picker;
  - a model choice;
  - the four direction buttons;
  - reduce and tolerance;
  - confidence threshold.
  - It runs in the background with a progress bar and abort. The panel's right-click menu has a Follow Vectors… entry.
- **Python:** `Roto.followVectors(items, source, first, last, direction, model, reduce=False, tolerance=0.5, confidenceThreshold=0.5)`, which returns the report.

**Out:**
- SplineWarp curves → M220 - SplineWarp Follow Vectors.
- Live (render-time) vector links → none (user: baked only). Track-based live links are M152 - Curve Track Links And Roto Copy's.
- Warping paint pixels by vectors (VectorDistort or SmartVector-style) → none.
- Moving individual stroke samples → none: the stroke data model has no per-frame points.
- Generating the vectors → M217 - VectorGenerator Node.

**Edge cases:**
- **The source node has no motion layers:** the picker doesn't list it. Python raises `ValueError` naming the node.
- **The source is deleted mid-follow:** the follow aborts and the undo step reverts it.
- **The source is the curve host itself, or downstream of it:** refused, because it creates a cycle. The picker excludes those nodes.
- **A point leaves the frame:** it keeps following by the vectors clamped at the border, with confidence 0, so it uses the inferred motion.
- **Mixed selection** of shapes and points from several shapes: each shape is followed independently. Selected points move per-point, and the rest of their shape stays put.
- **A fit on a Bezier that already has point animation:** the point keys are kept, and the fit only adds transform keys.
- **Render scale:** the source is sampled at full scale (scale 1), so the vectors match the curves' canonical coordinates.
- **Abort:** keys baked up to the last completed frame stay, as one undo step.
- **Inside a group, or a source in another group:** the source is resolved by its fully qualified name.

**Assumptions:**
- After M146, `RotoCurveInteract`'s toolbar builder accepts a host-independent knob group.
- Unconfirmed: an anchor-to-anchor gap longer than 200 frames is bridged the same way, with no extra re-anchoring.
- Unconfirmed: the affine and perspective fits on strokes go through `setExtraMatrix`, while translate uses `setTransform`'s translation.

## Phase 219.1: Engine

- [ ] M219.P1.T1 — Vector sampler
  - files: `Engine/VectorSampler.{h,cpp}` (new), `Engine/CMakeLists.txt`, `Tests/VectorSampler_Test.cpp` (new)
  - approach:
    - Given a node and a frame, render its Forward, Backward and (optional) `vectorConfidence` layers at scale 1 over its RoD through `renderRoI`, following `TrackerFrameAccessor.cpp:298-353`. Keep a small LRU of frames.
    - Provide bilinear `sample(frame, x, y) → (fwd, bwd, conf)` and `hasMotionLayers(node)`.
    - Refuse a node that is the host or downstream of it.
  - verify: `ctest -R VectorSampler` passes. On a VectorGenerator over a translating fixture, samples equal the expected vector within 0.1 px, a node without motion layers is rejected, and a downstream source is rejected.
  - size: M

- [ ] M219.P1.T2 — Trajectory integration and anchor bridging
  - files: `Engine/VectorFollow.{h,cpp}` (new), `Tests/VectorFollow_Test.cpp` (new)
  - approach:
    - Pure math over a sampler interface, so tests can feed an analytic field.
    - Integrate a set of points frame by frame, forward with Forward and backward with Backward.
    - Bridge between two anchors with a smoothstep crossfade, and track one-way outside them.
    - Carry per-point confidence through.
  - verify: `ctest -R VectorFollow` passes. With an analytic rotation field, a 50-frame forward integration stays within 0.25 px. A bridge with a deliberately biased field lands exactly on both anchors and halves the mid-gap error of one-way tracking.
  - size: M

- [ ] M219.P1.T3 — Motion models and occlusion inference
  - files: `Engine/VectorFollowModels.{h,cpp}` (new), `Engine/VectorFollow.cpp`, `Tests/VectorFollowModels_Test.cpp` (new)
  - approach:
    - Implement per-point, plus Eigen-based IRLS/Huber fits for translate, affine and perspective (normalised DLT followed by IRLS). Weight samples by confidence.
    - Sample the shape interior on a grid (spacing from its bounding box), plus its CPs.
    - Infer low-confidence points from the confident-sample fit, apply the 25% stop rule, and build the follow report.
  - verify: `ctest -R VectorFollowModels` passes:
    - known affine and homography fields are recovered within 1e-3 under 20% outliers;
    - an occluded half of a shape follows the visible half's motion;
    - the report lists the inferred frames;
    - the stop rule fires.
  - size: L

- [ ] M219.P1.T4 — Bake onto Beziers and strokes, with undo
  - files: `Engine/VectorFollow.cpp`, `Engine/RotoCurveContext.{h,cpp}`, `Engine/RotoUndoCommand.{h,cpp}`, `Tests/VectorFollowBake_Test.cpp` (new)
  - approach:
    - Add `RotoCurveContext::followVectors(params) → report`.
    - Per-point writes CP, tangent and feather keys through `setPointAtIndex`. Fits write transform keys: `setTransform` for translate, and `setExtraMatrix` for affine and perspective.
    - Strokes always take a fit.
    - Gather existing keys as anchors per `## Scope`.
    - Wrap the whole follow in one undo command, including the partial result after an abort.
  - verify: `ctest -R VectorFollowBake` passes:
    - a Bezier and a stroke follow a translating fixture through VectorGenerator, within 0.5 px at frame +20;
    - anchors are honoured exactly;
    - undo restores the original keys;
    - an aborted follow leaves only the completed frames.
  - size: L

- [ ] M219.P1.T5 — Keyframe reduction
  - files: `Engine/VectorFollowReduce.{h,cpp}` (new), `Engine/VectorFollow.cpp`, `Tests/VectorFollowReduce_Test.cpp` (new)
  - approach:
    - Greedy removal: repeatedly drop the key whose removal gives the smallest maximum deviation, measured over every frame and every point of the shape (point keys) or the transformed bounding-box corners (transform keys), while that deviation stays within `tolerance`.
    - Never remove an anchor.
    - Run it inside the follow's undo step.
  - verify: `ctest -R VectorFollowReduce` passes. A constant-velocity follow reduces to its two end keys, a sinusoid keeps its extrema within tolerance, and anchors survive.
  - size: M

## Phase 219.2: UI and Python

- [ ] M219.P2.T1 — Follow Vectors toolbar group
  - files: `Engine/RotoCurveInteract.{h,cpp}`, `Engine/VectorFollowKnobs.{h,cpp}` (new), `Engine/RotoPaint.cpp`
  - approach:
    - Add the group through M146's toolbar builder:
      - a source picker listing only nodes for which `hasMotionLayers` is true and that pass the cycle check;
      - the model choice;
      - the four direction buttons;
      - reduce, tolerance and confidence threshold.
    - The buttons act on the current selection.
    - The group is shown in Roto and RotoPaint.
  - verify: at the phase build, the Xvfb pass in the gate shows the group and runs a follow. `ctest -R VectorFollowBake` still passes.
  - size: M

- [ ] M219.P2.T2 — Background run, progress, abort and report
  - files: `Engine/VectorFollowTask.{h,cpp}` (new), `Engine/RotoCurveInteract.cpp`, `Gui/RotoPanel.cpp`
  - approach:
    - Run `followVectors` off the GUI thread, mirroring the tracker's scheduler, with viewer progress and abort.
    - Refresh the viewer as frames bake.
    - On completion, post the report as a node message. The right-click Follow Vectors… entry in `RotoPanel` uses the toolbar's current settings.
  - verify: at the phase build, the gate's Xvfb pass covers progress, an abort keeping partial keys, the report message and the panel entry.
  - size: M

- [ ] M219.P2.T3 — Python `Roto.followVectors`
  - files: `Engine/PyRoto.{h,cpp}`, `Engine/typesystem_engine.xml`, `Tests/Python/VectorFollow_Test.py` (new; follow the existing Python test registration)
  - approach:
    - Bind the call with the signature in `## Scope`. It runs synchronously and returns the report as a dict: per shape, the inferred ranges and the stop frame.
    - Raise `ValueError` for a source without motion layers or a cyclic source, using an early return in the inject-code so Shiboken can't swallow it.
  - verify: the Python test follows a Bezier, reduces it, reads the report, and checks that both error cases raise.
  - size: M

**Verification gate:**
- The build and the full ctest pass, including `VectorSampler`, `VectorFollow`, `VectorFollowModels`, `VectorFollowBake`, `VectorFollowReduce` and the Python test.
- Xvfb GUI check on a fixture plate under `build/`, with screenshots shared before sign-off:
  - a VectorGenerator feeds a Roto Bezier, which tracks forward per-point;
  - a RotoPaint stroke follows by an affine fit;
  - a two-anchor bridge lands on both keys;
  - reduction thins the keys;
  - an abort mid-follow keeps the partial keys, and undo reverts the whole follow.
- Run the GUI bench if the diff touches the bench rule's paths.
- An AppImage built with `package.sh` for UAT.

## Decisions

- 2026-10-25 — **The vectors come from any node with motion layers (user),** which covers both VectorGenerator and CG renders. There is no built-in flow and no new Roto input.
- 2026-10-25 — **Results are baked, with optional keyframe reduction (user).** There are no live links.
- 2026-10-25 — **Existing keys are anchors, and gaps are bridged forward/backward with a crossfade (user).**
- 2026-10-25 — **The motion model is chosen per follow (user):** per-point, or a robust Translate, Affine or Perspective fit that keys the shape's transform. Strokes always use a fit.
- 2026-10-25 — **Occluded points are inferred from their confident neighbours, and the inferred frames are reported (user).**
- 2026-10-25 — **The UI is a viewer toolbar group plus a panel entry, with a Python call (user).**
