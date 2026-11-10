# M342 - Sticky Projections On Deforming Geometry

Make a projection stick to animated geometry (user, 2026-11-05). M234 - Camera Projection Nodes' Geometry space with Use Frame Hold sticks through transforms, because it holds one matrix. It can't follow points that move on their own, such as a skinned or simulated mesh or an animated cache: the surface slides through the image. This milestone projects from the geometry's points as they were at the reference frame, which is Frame Hold.

## Scope

**In:**
- A USDProject3D in Geometry space with Use Frame Hold stays stuck to a prim whose points move, in the viewer and in USDRender: time-sampled meshes (USD and Alembic), skinned UsdSkel meshes, and curves and points.
- USDAssignMaterial authors the reference points through its Material input and through Material Path alike.
- Each deforming prim keeps its own placement: its reference points are in world space as USDAssignMaterial sees them at Frame Hold.
- Project On, Clip Near Far and behind-the-camera are all judged on the surface at the reference frame.
- USDWrite bakes the same result, curves and points included.

**Out:**
- Placement of prims that don't deform, including several at different places — M341 - Per-Placement Geometry Projection.
- Occlusion: the projection still falls on every face in the frustum.
- A choice of rest source (a knob for a `rest` or `Pref` primvar): none; the reference frame is the only source.
- De-instancing matched prims: none.
- Live projection in renderers other than Storm: none; USDWrite's bake remains the route to them.
- Bindings authored outside USDAssignMaterial, such as in a file or by a script: no node places them, so they slide, and the node docs say so; none.
- Geometry space without Use Frame Hold: it is placed anew each frame, so there is no reference frame to stick to; none.

**Edge cases:**
- Topology that changes over time: the rest points can't apply, so the prim falls back to rigid behaviour with a node warning naming it.
- Topology is compared by value against the reference frame, not read from `ValueMightBeTimeVarying`, so a cache that re-samples unchanged indices still sticks.
- UsdSkel meshes: reference points are the skinned result at Frame Hold, computed with UsdSkel.
- A prim inside a native instance: opinions on instance proxies are ignored, so it falls back to rigid with a warning naming it; nothing is de-instanced.
- A mesh already carrying a rest-position primvar: ignored. The plate lines up with the camera at the hold, not with a modelled rest pose.
- One mesh under two projections at different Frame Holds, on GeomSubsets or binding purposes: one primvar per hold frame, so both stay correct; two projections at the same hold share one.
- Frame Hold outside the cache's range: USD's nearest sample, as for any read.
- Project On with Front or Back on curves and points: they have no faces, so they show as Both, and the node docs say so.
- Points that don't vary and aren't skinned keep today's path, with no primvar and no copy of the points.

**Assumptions:**
- The user means deforming points as well as transforms. If rigid motion turns out to be misbehaving, that is a bug in M234 - Camera Projection Nodes' work to fix here first, through M342.P1.T1.
- Storm can read a vertex primvar named by a material's primvar reader in a glslfx surface, on meshes, curves and points, and fall back to the rigid path per prim where it is missing. M342.P1.T1 proves it.
- usdAbc may report unchanged indices as time-varying for some Alembic caches. The by-value comparison covers it either way; its cost goes in M342.P1.T1's record.

## Phase 342.1: Project from reference points

- [x] M342.P1.T1 — Confirm rigid stickiness and try reference points
  - files: `Tests/Scene/USDProject3D_Test.cpp`, `tools/spikes/storm-projection/`, `## Decisions` here
  - approach: test that today's Geometry space with Use Frame Hold sticks on an animated transform. Then try a vertex primvar holding each point's world position at the reference frame, named by a primvar reader on the material and read by the glslfx surface in place of the current position, on a generated deforming mesh, curves and points. A prim without the primvar in the same material must take the rigid path. Check the pxr headers, and how much authoring, the by-value topology check and each frame cost on a heavy mesh.
  - verify: a rigid test passes or the bug is fixed; a spike renders a deforming mesh, curves and points with the image stuck, and a rigid prim beside them; the cost is recorded in `## Decisions`.
  - size: M

- [x] M342.P1.T2 — Read reference points in the surface
  - files: `Engine/Nodes/Scene/USDProject3D.{h,cpp}`, `Engine/Nodes/Scene/shaders/natronProject3D.glslfx`, `Engine/Nodes/Scene/docs/USDProject3D.md`, `Tests/Scene/USDRenderStorm_Test.cpp`
  - approach: USDProject3D, held in Geometry space, authors a primvar reader named for its Frame Hold. Where the prim has that primvar, the surface projects from it, skipping `geometryFrame`, and derives Project On's facing from it too. Otherwise it takes today's path. Project On is Both on prims with no faces. Document the behaviour and its limits.
  - verify: a gtest authors the primvar by hand on a deforming mesh; its Storm render stays stuck across frames, Front keeps the same faces as the mesh turns, and a prim without the primvar matches today's reference.
  - size: L

- [x] M342.P1.T4 — Author reference points through the Material input
  - files: `Engine/Nodes/Scene/USDAssignMaterial.cpp`, `Engine/Nodes/Scene/SceneNodeAuthoring.{h,cpp}`, `Tests/Scene/USDProject3D_Test.cpp`
  - approach: for a held Geometry projection, author each matched mesh's world-space points at Frame Hold as a vertex primvar named for that hold, over meshes whose points are time-sampled. Points that don't vary keep today's path. Changing Frame Hold re-authors them. Compare topology by value with the reference frame; where it differs, or the prim is an instance proxy, leave it rigid and warn naming the prim.
  - verify: gtests over generated scenes: time-sampled USD and an Alembic export both get the primvar at the hold's values; two meshes apart keep their own; two holds on one mesh give two primvars; unchanged indices re-sampled over time still stick; changing topology and an instance proxy warn.
  - size: L

- [x] M342.P1.T5 — Author reference points through Material Path
  - files: `Engine/Nodes/Scene/USDAssignMaterial.cpp`, `Engine/Nodes/Scene/SceneNodeAuthoring.{h,cpp}`, `Engine/Nodes/Scene/docs/USDProject3D.md`, `Tests/Scene/USDProject3D_Test.cpp`
  - approach: with the Material input unconnected, read the material at Material Path from the input stage, and when it is a held Geometry projection, author as the Material input does. Share the shader check with the graft's so that M341 - Per-Placement Geometry Projection can use it, or reuse it if M341 landed first. The docs say only bindings outside USDAssignMaterial slide.
  - verify: a gtest binds a held projection by Material Path to a deforming mesh; its primvar matches the Material input's, and its Storm render matches that reference.
  - size: M

- [x] M342.P1.T6 — Reference points for skinned meshes
  - files: `cmake/NatronUsd.cmake`, `Engine/Nodes/Scene/SceneNodeAuthoring.cpp`, `Tests/Scene/USDProject3D_Test.cpp`
  - approach: link `usdSkel`. Detect meshes bound to a skeleton and compute their skinned points at Frame Hold with `UsdSkelCache` and `UsdSkelSkinningQuery` (check the pxr headers), check which space that result is in, carry the points to world space at the hold, and author them as any others. A mesh the query can't skin warns and stays rigid.
  - verify: a gtest generates a small skinned mesh in code; its primvar matches the skinned points at the hold, and its Storm render stays stuck across frames.
  - size: M

- [x] M342.P1.T7 — Reference points on curves and points
  - files: `Engine/Nodes/Scene/SceneNodeAuthoring.cpp`, `Engine/Nodes/Scene/USDAssignMaterial.cpp`, `Tests/Scene/USDRenderStorm_Test.cpp`
  - approach: extend authoring from meshes to `UsdGeomPointBased`, with the same by-value topology check using each type's own counts. Curve width and point size stay live.
  - verify: gtests render deforming curves and points in Storm, stuck across frames, and show them with Project On set to Front.
  - size: M

- [x] M342.P1.T3 — USDWrite bakes sticky projections
  - files: `Engine/Nodes/Scene/USDWrite.cpp`, `Tests/Scene/USDWrite_Test.cpp`
  - approach: on a mesh carrying its material's reference primvar, bake UVs from that primvar with no `geometryFrame`, so the exported UVs hold across frames, and don't export the authoring primvar as Natron data.
  - verify: a gtest exports a deforming mesh, checks its UVs don't vary over time and match the reference-frame projection, and finds no Natron data; a skinned mesh likewise.
  - size: M

- [x] M342.P1.T8 — USDWrite bakes curves and points
  - files: `Engine/Nodes/Scene/USDWrite.cpp`, `Tests/Scene/USDWrite_Test.cpp`
  - approach: the bake assumes faces and leaves other gprims out. Give prims with reference points a path that writes vertex-interpolated `st` from them. Non-deforming curves and points stay as they are today.
  - verify: a gtest exports deforming curves and points; each has time-constant vertex `st` matching the reference-frame projection, and no Natron data.
  - size: M

**Verification gate:** release gate green; strict-GL projection tests pass; `Tests/gui/m234_projection.py` gains a deforming step; a production cache of the user's choosing is accepted by the user from an AppImage (gtest scenes are generated in code, with no new LFS fixtures).

## Decisions

- 2026-11-07 — Shimmer (user): leave the rounding noise at checker edges on moving projections unless it shows on real projections.

- 2026-11-07 — Clash warning (user): held projections only. Unheld projections share one name and stay silent, even when values clash.

- 2026-11-07 — Projections at one hold keep sharing a primvar, with a warning on a real clash (user, after Codex raised the overwrite): a prim whose values differ between two assigns at one hold, such as `Assign → Transform → Assign`, stays wrong on screen but warns. Per-projection primvars were rejected: about 12 MB per million points per projection and a Storm shader variant per name, on every multi-projection scene, for a rare graph shape.

- 2026-11-05 — UsdSkel meshes stick, skinned at Frame Hold (user): rigged USD assets are a likely input, and they have no time-sampled points, so a points-only check would let them slide with no warning.
- 2026-11-05 — Reference points are world space at the hold (user): deforming prims at different places each project correctly without waiting on M341 - Per-Placement Geometry Projection, and transforms after USDAssignMaterial carry the image along.
- 2026-11-05 — No rest-source choice (user): the plate lines up with the camera at the hold, not with a modelled rest pose, so a rest primvar would usually misalign.
- 2026-11-05 — Project On is judged at the reference frame (user): faces don't pop in and out as the surface rotates, consistent with Clip Near Far.
- 2026-11-05 — Material Path also sticks (user): the path route shouldn't behave differently from the Material input. M341 - Per-Placement Geometry Projection's matching limitation was removed at the user's request.
- 2026-11-05 — Instanced prims fall back to rigid with a warning (user): de-instancing behind the user's back could multiply memory on broad scenes.
- 2026-11-05 — A primvar per hold frame (user): two projections at different holds on one mesh both stay correct, rather than depending on node order, and memory only grows when holds really differ.
- 2026-11-05 — Every point-based prim sticks and bakes (user): curves and points are in, both live and in USDWrite's bake, so the export matches what's on screen.
- 2026-11-05 — Project On is Both on curves and points, documented (user): they have no faces, and a zero derived normal would make Front hide the whole prim.
- 2026-11-05 — Acceptance is generated gtests plus a production cache of the user's choosing from an AppImage (user): real-file quirks get seen before merge, and nothing new goes into LFS.
- 2026-11-07 — Reference points reach the shader through a `UsdPrimvarReader_float3` with a 1e30 sentinel fallback: Storm always defines `HD_HAS_*` for a declared primvar, so its absence is only detectable by value. The primvar is authored per prim per hold, so a rigid prim in the same material takes the frame path.
- 2026-11-07 — The rigid path reads `HdGet_points()`, and the shader has its own copy of `IsFlipped`: Storm defines `IsFlipped` and `ApplyInstanceTransformInverse` for meshes only, which failed the link on curves and points in every projection space. Faces are detected at run time with `fwidth(patchCoord.x) > 0.0`, which leaves Project On at Both on curves and points.
- 2026-11-07 — Facing is the cross of the reference position's derivatives, oriented by `gl_FrontFacing != IsFlipped`: it stays the face of the hold when the surface turns.
- 2026-11-07 — Unchanged topology is checked by value over the project's range, from `GetTimeSamplesInInterval`: caches re-sample unchanged indices, so `ValueMightBeTimeVarying` would reject them. Meshes compare counts and indices, curves `curveVertexCounts`, points their size and ids.
- 2026-11-07 — Skinned points are computed in skeleton space by `UsdSkelSkinningQuery`, where the mesh's own transform plays no part, then taken to the world by the skeleton's local-to-world, which agrees with `UsdSkel.BakeSkinning`. `UsdSkelCache::Populate` runs once per skel root per read.
- 2026-11-07 — Cost, mesh of 1,002,001 points, 24 samples, usdc, measured from Python: reading points at the hold 3.1 ms; world transform 85.8 ms in numpy, a C++ loop at most this order; authoring the primvar 0.3 ms, the array shared rather than copied; topology by value 17.7 ms per sample, 426 ms over 24; 12 MB per primvar per hold. Once memoized, a frame authors and rereads nothing but hashes the key. Storm on llvmpipe at 320 px, 4 frames including load: 12.6 and 14.4 s with the reader against 10.9 s without, on a shared and noisy host, so no clear per-frame difference beyond noise. No real-GPU number measured.
- 2026-11-10 — PR #214 review round 1 (Claude; Codex quota out): 6 findings fixed in eca42e58d — mirrored-geometry Front/Back via `natronProject3DReferenceFlipped()` (test `USDRenderStormTest.StormProject3DProjectOnFrontSurvivesAMirrorAfterTheAssign`), mesh point-count check, prototype set cached per input scene (a full walk per upstream edit remains: no type index exists), docs and comments. Waiting on PR CI.
