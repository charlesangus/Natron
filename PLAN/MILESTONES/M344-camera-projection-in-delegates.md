# M344 - Camera Projection In Delegates

Make M234 - Camera Projection Nodes' `USDProject3D` render in Cycles and MoonRay, matching Storm. The user called this "a core part of comp workflows" (2026-11-05). Today the node authors a Storm-only glslfx surface (`Engine/Nodes/Scene/shaders/natronProject3D.glslfx`), so both delegates fall back. The node will author each renderer's native network beside the glslfx one, on `outputs:cycles:surface` and `outputs:moonray:surface`, driven by the same stored parameters. The plate arrives through M343 - Live Textures In Delegates.

## Scope

**In:**
- All `USDProject3D` behaviour on both delegates: the projection through `worldToProjector` and `plateTransform`, World and Geometry projection space, Project On, Crop, near and far clip, and the camera and plate following the frame or frame hold.
- The same output as Storm: unlit, emissive, with the plate's alpha showing what is behind it.
- Agreement with Storm within tolerance on M234's test scenes, on interior pixels.

**Out:**
- Occlusion, and combining a projection with an existing material: as in M234 - Camera Projection Nodes.
- OSL: none (user, 2026-11-05).
- Natron Utility: M281 - Utility Materials And Lights evaluates the projection natively.
- MaterialX projection: none, MaterialX is set aside (user, 2026-11-05).
- Per-placement and deforming-geometry projection: M341 - Per-Placement Geometry Projection and M342 - Sticky Projections On Deforming Geometry extend all three renderers when they land.

**Edge cases:**
- Points behind the camera or outside the crop: the outside mode, as in Storm.
- Motion blur: the projection is evaluated at the shutter's frame time for v1, so a moving camera doesn't smear the plate differently per sample. T1 records whether a delegate forces otherwise.
- USDWrite: its export bake still replaces the material with a `UsdPreviewSurface`, so the native networks don't reach exported files.
- Plate missing: M343's fallback and node warning.

**Assumptions:**
- Cycles nodes can express the projection. `texture_coordinate` takes only an affine `ob_tfm`, so the perspective divide needs `geometry` Position, vector maths and `image_texture`, or a dedicated node in the charlesangus fork. T1 decides.
- MoonRay's `ProjectCameraMap`, reached through hdMoonray's coordSys binding (`Material.cc:105`), or a small map DSO can match Storm's parameters. T1 decides.
- M325 - MoonRay Fork carries the `GetMaterialRenderContexts` override that lets hdMoonray read `outputs:moonray:surface`.

## Phase 344.1: Route

- [x] M344.P1.T1 — Hand-author both projection networks
  - files: scratch only; findings recorded in this file's `## Decisions`
  - approach: hand-author `.usda` materials for Cycles and MoonRay that reproduce `natronProject3D.glslfx`'s parameters on M234's test scene, with a file plate. Read `ProjectCameraMap`'s attributes first. Compare each against Storm. If a stock network can't match, specify the smallest fork node or DSO that does.
  - verify: each renderer's frame matches Storm within tolerance on interior pixels; the chosen network, or the required new node, is recorded with source citations.
  - size: L

## Phase 344.2: Authoring

- [x] M344.P2.T1 — `USDProject3D` authors the Cycles network
  - files: `Engine/Nodes/Scene/USDProject3D.cpp`, `Engine/Nodes/Scene/SceneProjectionCameras.{h,cpp}`, `Tests/Scene/USDRenderCycles_Test.cpp`, one reference crop (≤256 px, half-float, zip)
  - approach: author T1's network on `outputs:cycles:surface` from the values already stored for glslfx, per frame or held, so the two can't drift. Any fork node goes in first, with `NATRON_CYCLES_COMMIT` bumped.
  - verify: gtest — a plate projected onto a card from the render camera reproduces the plate; Crop and Project On change the expected pixels; an animated camera differs at two frames.
  - size: L

- [x] M344.P2.T2 — `USDProject3D` authors the MoonRay network
  - files: `Engine/Nodes/Scene/USDProject3D.cpp`, `Tests/Scene/USDRenderMoonRay_Test.cpp`, one reference crop (≤256 px, half-float, zip)
  - approach: as M344.P2.T1, on `outputs:moonray:surface`, using T1's map or DSO. A new DSO ships through M325's fork and passes `build-moonray.sh`'s count checks.
  - verify: gtest — the same three cases as M344.P2.T1 pass through MoonRay.
  - size: L

- [x] M344.P2.T4 — MoonRay-readable projection frame
  - files: `Engine/Nodes/Scene/SceneNodeAuthoring.{h,cpp}` (`authorProjectionFrame`), `Engine/Nodes/Scene/USDProject3D.cpp`, `Engine/Nodes/Scene/USDWrite.cpp`, `Tests/Scene/USDRenderMoonRay_Test.cpp`
  - approach: hdMoonray drops float4 arrays and matrices (`lib/hydramoonray/Primvars.cc` ~246-320), so the Cycles column primvars never reach MoonRay. Also author float3 column and translation primvars of the projection frame (missing reads as identity via `AttributeMap` defaults), read them in the MoonRay network with `AttributeMap`, and strip them in USDWrite's bake alongside the Cycles ones.
  - verify: the MoonRay geometry-space test passes for a held non-mesh prim without reference points and for a prim whose frame isn't its current transform (both cases that fail today), matching Storm.
  - size: M

- [x] M344.P2.T3 — Export and progressive viewer check
  - files: `Engine/Nodes/Scene/USDWrite.cpp` (only if needed), `Tests/Scene/USDWrite_Test.cpp`, `Tests/gui/scene_projection_delegates.py`
  - approach: confirm the export bake drops the native outputs, and fix it if not. A GUI script selects each delegate, projects a Constant-driven plate onto a card, moves the camera and edits the plate.
  - verify: gtest — an exported file has no `cycles:` or `moonray:` outputs and renders the same in `usdview`; the Xvfb script at 1920x1080 passes and its screenshots go to the user, one file each.
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; the delegate render tests pass; `scene_projection_delegates.py` passes under Xvfb; the user OKs the screenshots and tests a UAT AppImage projecting a plate sequence in both delegates.

## Decisions

- 2026-11-05 — **Projection is in scope for delegates (user):** "absolutely, this is a core part of comp workflows".
- 2026-11-05 — **Author native networks in the node, not a translation filter (cat-plan):** the parameters are already known at authoring time, and the result is plain USD a user can inspect.
- 2026-11-13 — **P1.T1 spike result: stock nodes suffice in both delegates; no fork node or DSO.** Rendered in Storm, Cycles and MoonRay on four cases (crop on/off, Project On front/back, outside black/discard); interior pixels agree with Storm to mean ≤0.0002, p99 0.0005, max 0.01 for Cycles and 0.04 for MoonRay (≤0.03% of pixels over 0.01). Scenes and scripts in `.worktrees/m344/build/m344-spike/`.
  - **Cycles:** `cycles_texture_coordinate` (use_transform, `ob_tfm` = inverse of the projector's u/v/w columns with plateTransform folded in) → Object → `separate_xyz` → two `math` divides → `combine_xyz` → `image_texture` (extension black, unassociated alpha, data colorspace); crop via `vector_math` multiply_add/floor/length + `math` less_than; w>0 via `math` greater_than; Project On via `geometry` True Normal and Backfacing; `emission` + `transparent_bsdf` → `mix_closure` → `outputs:cycles:surface` (terminal output named `Closure`/`Emission`). hdCycles takes `cycles_*` ids natively (src/hydra/material.cpp:619-621), sockets case-insensitive (:404, :467, :520).
  - **MoonRay:** `ProjectCameraMap` is unusable: hdMoonray's coordsys binding is a stub (lib/hydramoonray/CoordSys.cc:63) and it can't express plateTransform. Stock maps instead: `AttributeMap`(position) → `TransformSpaceMap`(render→world) → `OpMap` dots/divides → `FloatToRgbMap` → `UsdUVTexture` (wrap black, raw); crop, w>0 and Project On via further `OpMap`s; `DwaEmissiveMaterial` → `outputs:moonray:surface`.
  - **Motion blur:** neither delegate reads time samples on material networks (cycles hydra/material.cpp:282-307; hdMoonray Material.cc:203), so the projection is evaluated at the frame time.
  - **For P2 (PM):** build Cycles from `geometry` Position + `vector_math` dots, as MoonRay does, rather than the `ob_tfm` trick. That handles orthographic projectors (G singular) and gives the depth for near/far clip. Geometry projection space and frame hold need the per-prim reference: read the world-space `natronReferencePoints*` primvar (or float3 primvar rows) through `cycles_attribute` / a MoonRay primitive-attribute map; P2.T1 settles which and records it.
- 2026-11-13 — P2.T1 implemented (72225731e). `projectorValues()`/`plateValues()` are the single source for the Storm and Cycles inputs. Geometry space reaches Cycles through three float4 column primvars (frame minus identity; a missing primvar reads as zero, i.e. identity), and frame hold through `natronReferencePointsHold*` via `cycles_attribute`. Known approximations: Project On tests the frame normal, not the deformed one, with reference points; a reference point exactly at the origin reads as missing. USDWrite's bake strips the new primvars. The reference crop `CyclesProject3DReference` is recorded at the gate.
- 2026-11-13 — P2.T2 implemented (c77c78647). hdMoonray passes no float4 or matrix primvars, so geometry space on MoonRay uses the world position, and held meshes use their reference points. Held non-meshes without reference points, and prims whose frame isn't their current transform, don't match Storm yet; P2.T4 added (float3 frame primvars, PM call per Scope "all behaviour on both delegates"). Also fixed: Cycles authoring failed for Project On = Both. The gate must record `MoonRayProject3DReference` with `NATRON_TEST_REQUIRE_MOONRAY=1`.
- 2026-11-13 — P2.T4 and P2.T3 implemented (ae7c6e416). MoonRay reads four float3 frame primvars, so in geometry space a prim without a frame primvar projects in object space, as Storm and Cycles do. USDWrite's bake did keep the Cycles and MoonRay outputs; it now drops them, their shaders and the projection primvars. New GUI script `scene_projection_delegates.py`. Full gate running with `--record-goldens` (Cycles and MoonRay references).
- 2026-11-13 — First gate (ae7c6e416): six projection test failures, now fixed (e2fa2cc79, F9 merged in): the plate references were never recorded, because the golden-recording step's regex left out the projection suites (the workflow now includes them); test unit errors (orthographic aperture is in mm, and USDEditTransform appends after the card's scale); and a real USDWrite bug, where the bake left unread Cycles and MoonRay nodes (it now strips every material child the Storm surface doesn't read). Re-gating with `--record-goldens`.
- 2026-11-13 — Gate green on e2fa2cc79 with `--record-goldens`. The Cycles and MoonRay references are committed (2b1122c5b, about 5 KB each). F9 merged in (1a1167531). PR #277 into F9 is open and under review; re-gating against the committed references.
- 2026-11-13 — PR #277 review round 1: no blockers; the projection maths matches Storm. Medium findings: the bake stripped material children it doesn't own; Project On applied to curves and points (Storm skips them); the Cycles frame-column path was untested; nothing compared against Storm. PM calls: materials that keep their Storm shader also drop the networks on export (Scope: the networks never reach exported files); topology and values are authored in separate layers so scrubs stay cheap. Being fixed; re-gate cancelled.
- 2026-11-13 — Round 1 fixes committed (4baf6fdd9).
  - Network shaders now carry a `natronProjectionNetwork` mark, and the bake strips only marked shaders.
  - Cycles skips Project On on curves and points.
  - Kept-Storm exports drop the networks.
  - Topology is memoized separately from values, and digest compares replace layer-text compares.
  - Tests now compare against Storm's references.
  - Cycles colourspace is explicit (data, unassociated alpha), so the Cycles reference must be re-recorded.
  PM decision for MoonRay: USDProject3D authors a constant `natronProjectionNoFaces` primvar on the non-mesh geometry it projects onto, the MoonRay network reads it, and export strips it. Being implemented; a `--record-goldens` gate follows.
- 2026-11-13 — The MoonRay marker is not feasible. USDProject3D never sees the geometry, and its frame primvars are written only in Geometry space, by USDAssignMaterial. A marker there would also miss bindings authored outside Natron. PM call: accept the limitation. In MoonRay, Project On applies to curves and points by their shading normal, while Storm and Cycles show them as Both. Documented in USDProject3D.md (ba0e23b6b). Gating with `--record-goldens` (the Cycles reference changes).
- 2026-11-14 — Layered-bake failure fixed (089c8391d): the bake's stage now sees the stack's sublayers. The test's user shader now sits under the real material path, /Card1/Projection. The Cycles reference is re-recorded (explicit colour space). Merging F9 (with main and M364).
