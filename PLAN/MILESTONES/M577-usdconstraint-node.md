# M577 - USDConstraint Node

A new Scene node, USDConstraint, that keeps the prims a pattern names (any `UsdGeomXformable`: geometry, cameras, lights, xforms) attached to a target, with one Type choice: Parent (follow the target as if parented to it), Aim (turn to look at it) and Follow Path (ride along a `UsdGeomBasisCurves` prim). USD has no constraints, so the node reads the input stage's world transforms at every sample frame and authors the result as one matrix xform op, as USDEditTransform does (`Engine/Nodes/Scene/USDEditTransform.cpp`, `renderScene()` ~line 676: `SceneNodeAuthoring::matchPattern`, `getFreeOpName`, `SceneNodeAuthoring::authorValue` over `getSampleFrames(edit)`, `emitSceneLayer` keyed by `SceneLayerMemo::Key::eUpstreamReadFacts`). The constrained matrix is a world matrix, authored in Replace form (`!resetXformStack!` then the op), so a later USDEditTransform in Append mode offsets the prim in its constrained space. The target comes from the first input or an optional second Target input, read the way USDAssignMaterial reads its optional Material input (`args.getInputScene(1)`). The node registers in the USD Edit submenu (`PLUGIN_GROUP_USD_*`, M520 - USD Toolbar Category And Submenus).

## Scope

**In:**
- Inputs: Scene (0) and an optional Target scene (1).
- Common knobs: Type (`type`: Parent, Aim, Follow Path; default Parent), Prim Pattern (`primPattern`, the USD path expression every pattern-taking Scene node uses; every xformable match is constrained), Target Source (`targetSource`: First Input, Second Input, Position; Position only valid for Aim), Target Prim (`targetPrim`), Hide Target (`hideTarget`, default off: authors `visibility = invisible` on the target when it comes from the first input).
- Parent (Solaris Parent Constraint + Gaffer ParentConstraint): Position, Rotation, Scale, Shear toggles (`position`, `rotation`, `scale`, `shear`, all on); Maintain Offset (`maintainOffset`, default on) measured at Reference Frame (`referenceFrame`, default the project's first frame; Gaffer's referenceFrame, which makes Solaris's implicit offset frame explicit); Target Offset (`targetOffset`, a translate in the target's object space, Gaffer); Relative Offset (`relativeOffset`, off), Offset Method (`offsetMethod`: By Frame, By Time), Offset Frame (`offsetFrame`), Integer Frames (`integerFrames`), Offset Time (`offsetTime`, seconds): read the target's transform at a shifted time so the source lags or leads.
- Aim (Solaris Look At Constraint + Gaffer AimConstraint): Target Position (`targetPosition`, world space, when Target Source is Position); Target Offset (shared knob); Look At Axis (`lookAtAxis`: +X, -X, +Y, -Y, +Z, -Z; default -Z, the axis USD cameras and lights look down); Look Up Axis (`lookUpAxis`, same options, default +Y; must differ from Look At Axis on its line); Up Vector Mode (`upVectorMode`: X Axis, Y Axis, Z Axis, From Primitive, Custom; default Y Axis); Up Vector (`upVector`, Custom); Up Source (`upSource`: First Input, Second Input) and Up Prim (`upPrim`) for From Primitive; Twist (`twist`, degrees about the look-at direction).
- Follow Path (Solaris Follow Path Constraint): the target is a `UsdGeomBasisCurves` prim (its first curve), sampled in world space for every `type` (linear) and basis (bezier, bspline, catmullRom) with `wrap` nonperiodic, periodic or pinned; Parametrization (`parametrization`: Normalized Distance, Normalized Knots, Distance From Start, Distance From End, Distance Point Attribute; default Normalized Distance), Distance Attribute (`distanceAttribute`, a primvar name; the X of a float3 primvar or a float primvar), Position (`pathPosition`, animatable); Maintain Offset (shared knob); Look At Mode (`lookAtMode`: None, Along Path, Direction Attribute From Path, From Primitive; default Along Path), Direction Attribute (`directionAttribute`), Look At Source (`lookAtSource`) and Look At Prim (`lookAtPrim`); Look Up Mode (`lookUpMode`: Up Attribute From Path, Along Path, X Axis, Y Axis, Z Axis, From Primitive, Custom; default Y Axis), Up Attribute (`upAttribute`), plus the shared Up Vector, Up Source, Up Prim, Look At Axis and Look Up Axis; Roll (`roll`, degrees, animatable for banking).
- Knobs that do not apply to the chosen Type are hidden; a knob's visibility also follows its mode (Target Position only for Position, Up Prim only for From Primitive, and so on).
- Per-frame evaluation: the layer authors a default value when nothing varies; when any knob that applies is animated, or the source's, target's, up prim's or look-at prim's world transform (`UsdGeomXformCache::TransformMightBeTimeVarying` on the prim and its ancestors) or the path's points vary, it authors a time sample at every frame of `getSampleFrames(edit)`. Relative Offset reads the target at frame + offset, so it time-samples too.
- The layer is keyed on the computed matrices (a facts digest), so an upstream edit that moves the target re-authors it and one that does not leaves the memoized layer in place.
- `Engine/Nodes/Scene/docs/USDConstraint.md`, gtests for the maths, the path sampler and the node on in-memory stages, and an Xvfb GUI script with panel screenshots.

**Out:**
- Sampling Behavior, Start/End/Inc and Subframe Sampling (Shutter, Shutter Open/Close, Camera Prim, Samples, Always Include Frame Sample): every Scene node samples whole frames of the project range; renderers interpolate between them. No milestone.
- Source Type / Source Instances and Target Type / Target Instance (`/path[i]` point instances): instancer editing belongs to M576 - Point Instance Extract And Prune, and extracting an instance first gives a prim this node can use.
- Import Time: there is no embedded SOP network; the node reads the stage at each sample frame.
- Use Snippet (Follow Path position and roll): no wrangle language on USD nodes; knob expressions compute Position and Roll.
- Gaffer `targetMode` Bound, UV and Vertex (attach to a point on a surface): a surface constraint, not asked for. Origin is what the node uses.
- Gaffer `ignoreMissingTarget`: the node always warns and passes the scene through on a missing target (Edge cases), so there is no switch.
- Gaffer `relativeTransform`: Maintain Offset and Target Offset cover it, and a USDEditTransform in Append mode after the node offsets in constrained space.
- Gaffer free aim/up vectors: the six signed axes cover real rigs; a skewed aim axis is a USDEditTransform before the node.
- `UsdGeomNurbsCurves` and `UsdGeomHermiteCurves` paths: node error naming the type. No milestone.
- Viewport handles or guide lines for the constraint: none.
- Other Solaris constraints (Points, Surface, Blend) and Gaffer Point, Reflection and Framing constraints: not asked for.

**Edge cases:**
- Target prim missing, inactive, or not xformable: a persistent warning naming it ("may be inside an unloaded payload" when an unloaded ancestor could hold it, as `SceneNodeAuthoring::describeNoMatch` hints), and the input scene passes through unchanged. The node never loads payloads to find it.
- Target is a matched prim or a descendant of one (the constraint would move its own target): node error naming both; Up Prim and Look At Prim likewise.
- Second Input chosen with nothing connected: node error "Target Source is Second Input but the Target input is not connected". The two inputs' world spaces are taken to coincide.
- Target Source Position with Type Parent or Follow Path: node error; Look At Axis equal to (or the negation of) Look Up Axis: node error.
- Aim with the target at the source's position, or the up vector parallel to the look-at direction at a frame: that frame keeps the previous frame's orientation (the first frame: the source's own rotation) and a warning names the frames.
- Parent with only some of Position/Rotation/Scale/Shear on: the world matrix takes the on components from the constrained result and the off components from the source's own world transform, via one decomposition (translate, rotate, scale, shear) and recomposition.
- Maintain Offset: offset = inverse(target world at Reference Frame) x source world at Reference Frame; a Reference Frame outside the project range still reads the stage there.
- Nested matches (a matched prim under another matched prim): each reads its own world transform from the input stage; the authored reset makes each independent of the other's new op.
- The prim already has an op of the node's name: `getFreeOpName` picks a suffixed one, as USDEditTransform does; a rename re-authors under the new name.
- Path with zero or one point: node error; a periodic curve wraps Position; Normalized parametrizations clamp outside 0 to 1 on a nonperiodic curve.
- Distance Point Attribute values not increasing along the curve: a warning, and the first matching segment wins.
- A long project range with time-varying inputs reads the stage once per frame on the render thread; the loop checks the render's abort flag every frame, so a changed knob cancels it fast.

- Maintain Offset is one knob whose default depends on Type (on for Parent, off for Follow Path, off and hidden for Aim); a user edit of Type resets it to the new Type's default (user, 2026-11-24).
- The constrained transform is authored as a world matrix behind `!resetXformStack!`, not as a local matrix relative to the parent; the prim's existing ops are replaced (user, 2026-11-24).

**Assumptions:**
- Solaris's Look At default axes are -Z and +Y; unverified (the docs do not state defaults). -Z/+Y is right for USD cameras and lights either way.

## Phase 577.1: Constraint maths

- [ ] M577.P1.T1 — Parent and aim matrix maths
  - files: `Engine/Nodes/Scene/SceneConstraintMath.{h,cpp}` (new, free of node code), `Tests/Scene/SceneConstraintMath_Test.cpp`, `Tests/CMakeLists.txt`, `Engine/CMakeLists.txt`
  - approach: pure functions on `GfMatrix4d`/`GfVec3d`: `decompose`/`compose` (translate, rotate, scale, shear; `GfMatrix4d::Factor`); `parentWorld(sourceWorld, targetWorld, components, offset, targetOffset)` per the Parent edge cases; `aimWorld(sourceWorld, targetPoint, lookAtAxis, lookUpAxis, upVector, twistDegrees, previousRotation, *degenerate)` that keeps the source's translate and scale and builds the rotation from the signed axes; `axisVector(enum)`.
  - verify: gtest — parent with all components equals target x offset; position-only keeps the source's rotation and scale; maintain offset reproduces the source's world at the reference frame; aim with -Z/+Y at a target on +X gives a camera looking down +X; twist 90 rolls about the aim; a target at the source and an up parallel to aim report degenerate and return the previous rotation.
  - size: M

- [ ] M577.P1.T2 — BasisCurves path sampler
  - files: `Engine/Nodes/Scene/SceneCurvePath.{h,cpp}` (new), `Tests/Scene/SceneCurvePath_Test.cpp`, `Tests/CMakeLists.txt`, `Engine/CMakeLists.txt`
  - approach: build from a `UsdGeomBasisCurves` at a time (its first curve, points in world space through a `UsdGeomXformCache`): evaluate position and tangent for linear and cubic bezier/bspline/catmullRom with nonperiodic, periodic and pinned wrap (USD BasisCurves rules); an arc-length table for Normalized Distance and the distances; Normalized Knots by vertex index; Distance Point Attribute by searching a float or float3 (X) primvar; interpolate vertex `vector3f` primvars (direction, up) at a parameter. Errors for NurbsCurves/HermiteCurves, empty and one-point curves.
  - verify: gtest on in-memory stages — a straight linear curve maps Normalized Distance 0.5 to its midpoint; Normalized Knots on a five-point polyline hits each vertex at 0, 0.25, ..., 1; a bezier arc's tangent at 0 points along its first control leg; periodic wrap of 1.25 equals 0.25; Distance From End measures from the last point; a non-monotonic distance primvar reports the warning flag; a NurbsCurves prim returns the error.
  - size: M

## Phase 577.2: Node

- [ ] M577.P2.T1 — Share the free op name helper
  - files: `Engine/Nodes/Scene/USDEditTransform.cpp`, `Engine/Nodes/Scene/SceneNodeAuthoring.{h,cpp}`
  - approach: move `getFreeOpName(prim, base)` out of USDEditTransform's anonymous namespace into `SceneNodeAuthoring` unchanged, and call it from there.
  - verify: `USDEditTransform_Test` passes unchanged.
  - size: M

- [ ] M577.P2.T2 — USDConstraint node with the Parent type
  - files: `Engine/Nodes/Scene/USDConstraint.{h,cpp,reg.inc}` (new), the native registry includes beside the other Scene nodes, `Engine/CMakeLists.txt`, `Tests/Scene/USDConstraint_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: a `NativeEffectBase` with the two inputs, every knob listed in Scope (all Types, so the panel is final), visibility updated in `knobChanged` from Type and the modes, and a user edit of Type resetting `maintainOffset` to that Type's default (on for Parent, off otherwise) as one undoable edit (not on project load, so a saved value is kept). `renderScene` follows USDEditTransform: validate, `matchPattern` with the xformable filter, borrow the input (and Target input) stages through `SceneQueryPool`, decide time variance, compute each match's world matrix per sample frame with `parentWorld` (checking the render's abort flag each frame), hash the matrices into the facts digest, and author `xformOp:transform:<name>` with `[!resetXformStack!, op]` through `authorValue`. All the Edge cases for targets and Parent apply; Aim and Follow Path set "not implemented yet" errors until the next tasks. Register in the USD Edit submenu; the label "USDConstraint".
  - verify: gtest on in-memory stages — a cube parented to an animated xform follows it at frames 1, 10, 20 (`ComputeLocalToWorldTransform` of the output); position-only keeps the cube's rotation; maintain offset holds the reference-frame offset; Relative Offset -5 frames lags; a static target authors a default value and no time samples; a missing target passes through with the warning; a target under the source errors; Second Input reads the second stage.
  - size: L

- [ ] M577.P2.T3 — Aim type
  - files: `Engine/Nodes/Scene/USDConstraint.cpp`, `Tests/Scene/USDConstraint_Test.cpp`
  - approach: evaluate Aim per frame with `aimWorld`: target point from the target prim's world origin plus Target Offset, or Target Position; up from the world axis, Custom vector, or the Up Prim's world origin minus the source's position; carry the previous frame's rotation for degenerate frames and collect them into the warning; the axis-equal and Position-with-wrong-Type errors.
  - verify: gtest — a `UsdGeomCamera` aimed at an animated sphere has the sphere on its -Z axis at every frame; a light with From Primitive up keeps its +Y towards the up prim; Target Position animated by keyframes time-samples; a degenerate frame warns and keeps the prior orientation.
  - size: M

- [ ] M577.P2.T4 — Follow Path type
  - files: `Engine/Nodes/Scene/USDConstraint.cpp`, `Tests/Scene/USDConstraint_Test.cpp`
  - approach: build a `SceneCurvePath` per sample frame (once when the curve does not vary), place the source at Position under the chosen Parametrization, orient by Look At Mode and Look Up Mode with the Look At and Look Up axes and Roll through `aimWorld` (Along Path uses the tangent as the aim direction, the attribute modes the interpolated primvars, From Primitive the prim's world origin), and apply Maintain Offset at Reference Frame.
  - verify: gtest — a card on a linear path with Position keyed 0 to 1 over frames 1 to 25 sits at the curve's start, middle and end; Along Path points its -Z along the tangent; Roll 30 banks it; Look At Mode None keeps the card's rotation; maintain offset keeps the reference offset; a NurbsCurves target errors.
  - size: M

- [ ] M577.P2.T5 — Hide Target
  - files: `Engine/Nodes/Scene/USDConstraint.cpp`, `Tests/Scene/USDConstraint_Test.cpp`
  - approach: when Hide Target is on and the target comes from the first input, also author `visibility = invisible` over the target prim in the node's layer (and hash it); ignored with Second Input and Position.
  - verify: gtest — the target's computed visibility is invisible with the knob on and inherited with it off; with Second Input the first stage's prim of the same path stays visible.
  - size: M

- [ ] M577.P2.T6 — Node doc and params-to-knob audit
  - files: `Engine/Nodes/Scene/docs/USDConstraint.md` (new, in the style of `docs/USDEditTransform.md`), the milestone's PR body
  - approach: write the doc (Types, the knobs each uses, Replace-form authoring and how to offset after it, time sampling, warnings and errors, "Tolerance: none recorded."), naming no other application. Then audit every Solaris Parent, Look At and Follow Path Constraint parameter and every Gaffer ParentConstraint and AimConstraint plug against the knobs, recording each as a knob or as Out with its reason in the PR body; any gap not in Scope's Out goes back to the PM.
  - verify: the doc lists every knob in `initializeKnobs()` by its label; the audit table covers each parameter in `.local/docs/houdini/nodes/lop/{parentconstraint,lookatconstraint,followpathconstraint}.md` and the two Gaffer pages.
  - size: M

## Phase 577.3: GUI check

- [ ] M577.P3.T1 — Xvfb check and panel screenshots
  - files: `Tests/gui/usd_constraint.py` (new)
  - approach: under Xvfb at 1920x1080, build USDCamera + an animated USDSphere through USDMerge into USDConstraint (Aim, Prim Pattern the camera, Target Prim the sphere); step frames and assert the viewer-side stage's camera matrix changes and keeps the sphere on -Z; switch Type to Parent and Follow Path and assert the hidden/shown knob sets; take one panel screenshot per Type and one viewer shot through the aimed camera.
  - verify: the script passes; the four screenshots go to the user, one file each.
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; `SceneConstraintMath_Test`, `SceneCurvePath_Test`, `USDConstraint_Test` and `USDEditTransform_Test` pass; `Tests/gui/usd_constraint.py` passes under Xvfb at 1920x1080; the user OKs the panel and viewer screenshots.

## Decisions

- 2026-11-24 — **One node with a Type choice (user):** a single USDConstraint node whose Type picks Parent, Aim or Follow Path, evaluated per frame into xform ops, for prims and cameras; the controls are the union of the reference constraints, chosen per Type.
- 2026-11-24 — **Maintain Offset default per Type (user):** one `maintainOffset` knob, defaulting on for Parent and off for Follow Path, reset to the new Type's default when the user changes Type: it matches the reference defaults without a duplicate knob per Type.
- 2026-11-24 — **World matrix behind a reset (user):** the result is authored as `xformOp:transform:<name>` after `!resetXformStack!`, holding the world matrix, as USDEditTransform's Replace mode does: it is correct whatever the parent hierarchy does, and a USDEditTransform in Append mode downstream offsets in constrained space.
