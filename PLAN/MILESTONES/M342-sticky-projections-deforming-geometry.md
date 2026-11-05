# M342 - Sticky Projections On Deforming Geometry

Make a projection stick to animated geometry (user, 2026-11-05). M234 - Camera Projection Nodes' Geometry space with Use Frame Hold sticks through transforms, because it holds one matrix. It can't follow points that move on their own, such as a skinned or simulated mesh or an animated cache: the surface slides through the image. This milestone projects from the geometry's points as they were at the reference frame.

## Scope

**In:**
- A USDProject3D in Geometry space with Use Frame Hold stays stuck to a mesh whose points are time-sampled, in the viewer and in USDRender.
- USDWrite bakes the same result.

**Out:**
- Per-prim placement — M341 - Per-Placement Geometry Projection.
- Occlusion: the projection still falls on every face in the frustum.

**Edge cases:**
- Topology that changes over time: the rest points can't apply, so the prim falls back to rigid behaviour with a node warning.
- UsdSkel meshes: reference points come from the skinned result at the reference frame, or the limit is stated and warned.
- A mesh already carrying a rest-position primvar: use it only if the node offers that choice; the default is the reference frame.

**Assumptions:**
- The user means deforming points as well as transforms. If rigid motion turns out to be misbehaving, that is a bug in M234 - Camera Projection Nodes' work to fix here first, through M342.P1.T1.

## Phase 342.1: Project from reference points

- [ ] M342.P1.T1 — Confirm rigid stickiness and try reference points
  - files: `Tests/Scene/USDProject3D_Test.cpp`, `tools/spikes/storm-projection/`, `## Decisions` here
  - approach: test that today's Geometry space with Use Frame Hold sticks on an animated transform. Then try a vertex primvar holding each point's position at the reference frame, read by the glslfx surface in place of the current position, on a generated deforming mesh. Check the pxr headers, and how much it costs per frame on a heavy mesh.
  - verify: a rigid test passes or the bug is fixed; a spike renders a deforming mesh with the image stuck; the cost is recorded in `## Decisions`.
  - size: M

- [ ] M342.P1.T2 — Author reference points and read them
  - files: `Engine/Nodes/Scene/USDProject3D.{h,cpp}`, `Engine/Nodes/Scene/USDAssignMaterial.cpp`, `Engine/Nodes/Scene/SceneNodeAuthoring.{h,cpp}`, `Engine/Nodes/Scene/shaders/natronProject3D.glslfx`, `Engine/Nodes/Scene/docs/USDProject3D.md`
  - approach: on placement, author the reference-frame points as a vertex primvar over matched meshes whose points are time-sampled; the surface projects from them. Points that don't vary keep today's path. Changing Frame Hold re-authors them. Warn where topology varies.
  - verify: gtests: a deforming mesh's Storm render stays stuck across frames (`Tests/Scene/USDRenderStorm_Test.cpp`), a changing-topology mesh warns.
  - size: L

- [ ] M342.P1.T3 — USDWrite bakes sticky projections
  - files: `Engine/Nodes/Scene/USDWrite.cpp`, `Tests/Scene/USDWrite_Test.cpp`
  - approach: bake UVs from the reference points, so the exported UVs hold across frames, and don't export the authoring primvar as Natron data.
  - verify: a gtest exports a deforming mesh, checks its UVs don't vary over time and match the reference-frame projection, and finds no Natron data.
  - size: M

**Verification gate:** release gate green; strict-GL projection tests pass; `Tests/gui/m234_projection.py` gains a deforming step; a real animated cache is accepted by the user from an AppImage.
