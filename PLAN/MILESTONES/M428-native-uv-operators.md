# M428 - Native UV Operators

UV authoring for native geometry, following `PLAN/DECISIONS/2026-11-08-native-geometry-follows-sops.md`. Member of F11 - Texture Painting.

Three nodes, each with every control and default of the matching SOP (`.local/docs/houdini/nodes/sop/uvproject.md`, `uvflatten.md`, `uvlayout.md`):
- complete `GeoUVProject`;
- add `GeoUVFlatten`;
- add `GeoUVLayout`.

Today `GeoUVProject` (M359.P1.T9) has only two modes: Planar (an axis plus TRS) and Camera. It also has non-SOP knobs: `axis`, `fitToBounds`, `invertU`/`invertV`, and a separate `attributeClass` (main `Engine/Nodes/Geometry/GeoUVProject.h:41-63`).

What exists to build on:
- edge groups (`Engine/Geo/GeoGroup.h:164`);
- `GeoTopology`;
- `KnobTable` (`Engine/KnobTypes.h:1621`);
- the bundled `libs/Eigen3`.

Algorithms:
- SCP: our own implementation of Mullen et al. 2008, on Eigen sparse.
- ABF++: ported from Blender's parametrizer (GPL-2.0-or-later, which is compatible).
- Packing: our own raster packer, as the SOP describes.

## Scope
**In:**
- **UV Project:**
  - Projections: Orthographic, Polar, Cylindrical, Toroidal, Plastic Wrap, plus Natron's Camera.
  - Transform/Rotate Order and Translate/Rotate/Scale/Pivot.
  - Inner Radius, Initialize Method and button, U/V Range, Angle.
  - Fix Boundary Seams, Fix Poles, Pole Radius.
  - Group/Attrib Type sets the attribute class.
- **UV Flatten:** every listed parameter, with pins, alignments and straightenings as `KnobTable`s.
- **UV Layout:** every listed parameter, including second-input targets and stacking.
- `uv` stays vertex class by default. Other attributes and groups are kept, and tombstones respected.

**Out:**
- The SOPs' interactive viewer states (UV view, pin drag, Repack): there is no UV viewport.
- Seam picking in the viewer: M608 - Geometry Group Picker.

**Edge cases:**
- Non-polygon primitives pass through untouched.
- Degenerate or zero-area faces are skipped, with a warning count.
- If ABF fails to solve an island, that island falls back to SCP with a warning.
- Non-manifold edges split islands.
- Islands that fail to pack at Fixed scale go to the nonpacked group, as in the SOP.
- Tiles above 1100 warn (M311).

**Assumption (unconfirmed):** Blender's ABF++ separates cleanly from BMesh.

## Phase 428.1: UV Project
- [x] M428.P1.T1 — GeoUVProject: SOP projections and transform
  - files: `Engine/Nodes/Geometry/GeoUVProject.{h,cpp}`, `Tests/GeoUVProject_Test.cpp`
  - approach:
    - Replace Planar/axis/fit/invert with the SOP set and transform knobs. Labels match script names; no backward compatibility.
    - Group/Attrib Type picks vertex or point `uv`.
    - Camera stays.
  - verify: gtest.
    - A grid, cylinder, sphere and torus give the closed-form uv for each projection.
    - Transform and Rotate Order change the result as expected.
  - size: L
- [x] M428.P1.T2 — GeoUVProject: Initialize, ranges, Angle, seam and pole fixes
  - files: `Engine/Nodes/Geometry/GeoUVProject.{h,cpp}`, `Tests/GeoUVProject_Test.cpp`
  - approach:
    - Initialize fits the TRS to the group bounds by Method (undoable).
    - U/V Range and Angle work about (0.5, 0.5).
    - Fix Boundary Seams unwraps vertex u across the seam.
    - Fix Poles takes the neighbours' u within Pole Radius.
  - verify: gtest.
    - On a cylinder, seam faces have no u jump.
    - On a sphere, pole vertices take the neighbours' u.
    - Initialize maps the bounds to 0-1.
  - size: M

## Phase 428.2: Island topology and packing
- [x] M428.P2.T1 — UV island and seam topology helper
  - files: `Engine/Geo/GeoUVIslands.{h,cpp}` (new), `Tests/GeoUVIslands_Test.cpp`, `Tests/CMakeLists.txt`
  - approach:
    - Islands come from uv connectivity (with tolerance), from seam or separator edge groups, or from an island attribute.
    - Also provides vertex edges and boundary loops.
  - verify: gtest.
    - A cube with seams gives the expected islands.
    - uv-split and seam-split agree.
  - size: M
- [x] M428.P2.T2 — Raster island packer
  - files: `Engine/Geo/GeoUVPack.{h,cpp}` (new), `Tests/GeoUVPack_Test.cpp`
  - approach:
    - Rasterize islands at Search Resolution, with padding in pixels.
    - Rotation steps and cavities.
    - Iterations with Random Seed.
    - Scale: Largest packable or Fixed, searched to Scale Search Tolerance.
    - Targets: rectangles, a UDIM grid (Tile size, Columns, Starting UDIM), or islands from a second input.
    - Stacking of identical and mirrored islands.
    - Runs in parallel across iterations; abortable.
  - verify: gtest.
    - No overlap within the padding.
    - A UDIM run fills tiles from Starting UDIM.
    - The same seed gives the same layout.
    - Cancel returns promptly.
  - size: L

## Phase 428.3: Flatten and Layout nodes
- [x] M428.P3.T1 — SCP and ABF++ solvers with pins, alignment and straightening
  - files: `Engine/Geo/GeoUVFlattenSolver.{h,cpp}` (new), `Tests/GeoUVFlattenSolver_Test.cpp`
  - approach:
    - One solve per island, in parallel.
    - SCP: a sparse generalized eigenproblem on Eigen.
    - ABF++: ported.
    - Constraints:
      - pins;
      - axis-aligned groups;
      - straightened loops (ABF only);
      - rectify group;
      - preserve boundary (SCP only).
  - verify: gtest.
    - A cube's six islands come out with low angular distortion (record the threshold).
    - Pins stay exactly in place.
    - Aligned groups share an axis.
    - Straightened loops are collinear.
  - size: L
- [x] M428.P3.T2 — GeoUVFlatten node, every UV Flatten control
  - files: `Engine/Nodes/Geometry/GeoUVFlatten.{h,cpp,reg.inc}`, `Engine/Nodes/Geometry/docs/GeoUVFlatten.md`, `Tests/GeoUVFlatten_Test.cpp`
  - approach:
    - Controls: Group, UV Attribute, Flattening Method, Existing UVs options, both seam fields with Enable Manual Layout semantics, constraint tables, output groups, Packing area Center/Size (via GeoUVPack).
    - Always procedural.
  - verify: gtest.
    - A cube with seams gives non-overlapping islands inside the packing area.
    - Preserve Layout keeps existing islands.
    - The output groups are present.
    - Other attributes and groups survive.
  - size: M
- [x] M428.P3.T3 — GeoUVLayout node, every UV Layout control
  - files: `Engine/Nodes/Geometry/GeoUVLayout.{h,cpp,reg.inc}`, `Engine/Nodes/Geometry/docs/GeoUVLayout.md`, `Tests/GeoUVLayout_Test.cpp`
  - approach:
    - Two inputs.
    - Connectivity, target, scale and offset overrides (attributes and `polygon:value` strings).
    - Scale Islands To Match Surface Area and Axis Alignment.
    - Every Packing, Targets and Stacking knob, plus the output groups and attributes, mapped onto GeoUVPack.
  - verify: gtest.
    - The string `35:1009` lands that island in tile 1009.
    - Area matching scales by 3D area.
    - Islands outside the group are avoided.
  - size: M
- [ ] M428.P3.T4 — Paint check through GeoToUSD
  - files: `Tests/gui/m428_uv_operators_paint.py`
  - approach:
    - Chain: GeoPrimitive box → GeoUVFlatten → GeoUVLayout (UDIM) → GeoToUSD → USDTexturePaint.
    - Paint a stroke across two faces.
  - verify: passes remotely; the tiles show paint on both islands; screenshots shared.
  - size: M

**Verification gate:**
- In ctest, the M428 gtests pass, plus the existing M359 GeoUVProject tests rewritten for the new knobs.
- `m428_uv_operators_paint.py` passes remotely, with screenshots shared.
- The UAT is collected at F11's.

## Decisions
- 2026-11-26 (PM, per the no-backward-compat and match-the-SOP rules):
  - GeoUVProject drops `axis`, `fitToBounds`, `invertU`/`invertV` and its separate class knob in favour of the SOP's controls.
  - UV Flatten runs procedurally, with no Repack, because there is no UV view yet. A UV viewport is a separate future stub.
