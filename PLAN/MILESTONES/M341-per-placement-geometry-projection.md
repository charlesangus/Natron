# M341 - Per-Placement Geometry Projection

A USDProject3D in Geometry space holds one `geometryFrame` per material. Assigned through USDAssignMaterial to prims at different places, it follows the first and warns, and the others show it offset. When several assigns share one projection node, the later one sets the frame. This milestone gives each placed prim its own frame (user, 2026-11-05). Built on M234 - Camera Projection Nodes' assignment work.

## Scope

**In:**
- One Geometry projection assigned to any number of prims, each projecting from its own placement, held or animated.
- Several USDAssignMaterial nodes sharing one projection node, each keeping its own placement.
- USDWrite's bake honours per-prim placement.

**Out:**
- Projections that follow deforming points — M342 - Sticky Projections On Deforming Geometry.
- Binding by Material Path keeps taking the camera in the geometry's own coordinates.

**Edge cases:**
- The warning for prims at different places goes away, and its tests change with it.
- A prim added to the match by a later edit gets its frame through the node's normal re-evaluation.
- Instanced prims: each instance projects from its own placement, or the limit is stated in the node docs and warned.

## Phase 341.1: A frame per prim

- [ ] M341.P1.T1 — Decide how a prim carries its frame
  - files: `Engine/Nodes/Scene/shaders/natronProject3D.glslfx`, `## Decisions` here
  - approach: compare a constant matrix primvar on each matched prim, read by the shader, with one material copy per distinct placement. The primvar avoids a material per prim, so prefer it if Storm serves a constant matrix primvar to a glslfx surface, per prim and animated. Check the pxr headers and try it on `tools/spikes/storm-projection/`. Display state rule: USDWrite must not export it as Natron data.
  - verify: a spike renders two prims at different places with one material, each projection correct; the choice is recorded in `## Decisions`.
  - size: M

- [ ] M341.P1.T2 — Author per-prim placement
  - files: `Engine/Nodes/Scene/USDAssignMaterial.cpp`, `Engine/Nodes/Scene/SceneNodeAuthoring.{h,cpp}`, `Engine/Nodes/Scene/shaders/natronProject3D.glslfx`, `Engine/Nodes/Scene/docs/USDProject3D.md`, `Tests/Scene/USDProject3D_Test.cpp`
  - approach: USDAssignMaterial and the primitive nodes place each matched prim through the mechanism from M341.P1.T1, held or sampled when animated. Remove the warning and the one-placement text in the docs.
  - verify: gtests: two prims apart, and two assigns sharing one projection, each match their direct-connection references in Storm (`Tests/Scene/USDRenderStorm_Test.cpp`).
  - size: L

- [ ] M341.P1.T3 — USDWrite bakes per placement
  - files: `Engine/Nodes/Scene/USDWrite.cpp` (around line 824), `Tests/Scene/USDWrite_Test.cpp`
  - approach: the bake reads each mesh's own frame instead of the material's.
  - verify: a gtest exports two prims apart and checks each one's baked UVs against a direct projection; the export carries no Natron data.
  - size: M

**Verification gate:** release gate green; strict-GL projection tests and `Tests/gui/m234_projection.py` pass with a two-placement step added; a screenshot of two placed prims is shared with the user.
