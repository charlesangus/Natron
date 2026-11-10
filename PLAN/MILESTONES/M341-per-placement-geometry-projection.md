# M341 - Per-Placement Geometry Projection

A USDProject3D in Geometry space holds one `geometryFrame` per material. Assigned through USDAssignMaterial to prims at different places, it follows the first and warns, and the others show it offset. When several assigns share one projection node, the later one sets the frame. This milestone gives each placed prim its own frame (user, 2026-11-05). Built on M234 - Camera Projection Nodes' assignment work.

## Scope

**In:**
- One Geometry projection assigned to any number of prims, each projecting from its own placement, held or animated.
- Several USDAssignMaterial nodes sharing one projection node, each keeping its own placement.
- USDAssignMaterial's Material Path places a projection as its Material input does.
- USDWrite's bake honours per-prim placement.

**Out:**
- Projections that follow deforming points, which carry their own placement in their reference points — M342 - Sticky Projections On Deforming Geometry.
- Bindings authored outside USDAssignMaterial, such as in a file or by a script: no node places them, so they keep taking the camera in the geometry's own coordinates; none.

**Edge cases:**
- The warning for prims at different places goes away, and its tests change with it.
- A prim added to the match by a later edit gets its frame through the node's normal re-evaluation.
- Instanced prims: each instance projects from its own placement, or the limit is stated in the node docs and warned.

## Phase 341.1: A frame per prim

- [x] M341.P1.T1 — Decide how a prim carries its frame
  - files: `Engine/Nodes/Scene/shaders/natronProject3D.glslfx`, `## Decisions` here
  - approach: compare a constant matrix primvar on each matched prim, read by the shader, with one material copy per distinct placement. The primvar avoids a material per prim, so prefer it if Storm serves a constant matrix primvar to a glslfx surface, per prim and animated. Check the pxr headers and try it on `tools/spikes/storm-projection/`. Display state rule: USDWrite must not export it as Natron data.
  - verify: a spike renders two prims at different places with one material, each projection correct; the choice is recorded in `## Decisions`.
  - size: M

- [x] M341.P1.T2 — Author per-prim placement
  - files: `Engine/Nodes/Scene/USDAssignMaterial.cpp`, `Engine/Nodes/Scene/SceneNodeAuthoring.{h,cpp}`, `Engine/Nodes/Scene/shaders/natronProject3D.glslfx`, `Engine/Nodes/Scene/docs/USDProject3D.md`, `Tests/Scene/USDProject3D_Test.cpp`
  - approach: USDAssignMaterial and the primitive nodes place each matched prim through the mechanism from M341.P1.T1, held or sampled when animated. With Material Path set, USDAssignMaterial reads the material from the input stage and places it the same way, sharing the shader check with M342 - Sticky Projections On Deforming Geometry (M342.P1.T5), or reusing it if that landed first. Remove the warning and the one-placement text in the docs.
  - verify: gtests: two prims apart, and two assigns sharing one projection, and a projection bound by Material Path, each match their direct-connection references in Storm (`Tests/Scene/USDRenderStorm_Test.cpp`).
  - size: L

- [x] M341.P1.T3 — USDWrite bakes per placement
  - files: `Engine/Nodes/Scene/USDWrite.cpp` (around line 824), `Tests/Scene/USDWrite_Test.cpp`
  - approach: the bake reads each mesh's own frame instead of the material's.
  - verify: a gtest exports two prims apart and checks each one's baked UVs against a direct projection; the export carries no Natron data.
  - size: M

**Verification gate:** release gate green; strict-GL projection tests and `Tests/gui/m234_projection.py` pass with a two-placement step added; a screenshot of two placed prims is shared with the user.

## Decisions

- 2026-11-05 — Material Path places a projection too (user, from M342 - Sticky Projections On Deforming Geometry's interview): the path route shouldn't behave differently from the Material input, so the limitation that was under Out is gone.
- 2026-11-07 — A prim carries its frame in a constant matrix primvar, read through `UsdPrimvarReader_matrix` connected to `inputs:geometryFrame`: `tools/spikes/storm-projection/run-placement.sh` renders two cards apart through one material pixel-identical to one material per card, held and animated, with no shader code change and Storm's identity fallback for an unplaced prim. The primvar is `natronProjectionFrame` unheld and `natronProjectionFrameHold<frame>` held, so projections held at different frames do not overwrite each other on a prim; nodes read the name from the reader's `inputs:varname`. USDWrite strips the primvars, the reader and the `geometryFrame` connection of what it baked.
- 2026-11-07 — Instances are the stated limit, warned on USDAssignMaterial and in the node docs: a collection binding from an ancestor, the form USDAssignMaterial authors, does not reach native instances in Storm for any material (lit grey in the spike), so instanced matches get no primvar; the instances of a point instancer share their prototype's one placement.
- 2026-11-09 — Two-placement screenshots approved (user): m234_two_placements.png, _viewer.png, _held.png from gate run on 593e8dfb5. M341 ships with M342 in PR #214.
