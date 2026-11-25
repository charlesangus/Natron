# M611 - USDToGeo Source Controls

Give USDToGeo the source-side controls the round trip needs: the reference app's LOP Import plus Unpack USD control set, audited in full. Today it has three knobs (`primPattern`, `unpack`, `flattenTransforms`, `Engine/Nodes/Geometry/USDToGeo.cpp` ~:190-220). It always writes `path` (and `usdSource` when unpacked), reads only mesh, points and basis curves, and carries no material bindings. The milestone adds:

- **Naming:** Path Attribute and Name Attribute names.
- **What gets read:** Traversal (No Traversal / Components / Gprims / Groups), Purpose, Exclude Inactive Primitives, and Import Point Instances.
- **Time:** Time Sample (Animated / Static with an Import Frame).
- **Packed prims:** Pivot Location (Centroid / Origin).
- **Unpacked data:** Import Primvars (with inherited primvars), Import Attributes and Non-Transforming Primvars; the `usdxform` attribute (the prim's local-to-world matrix) with Apply Inverse USD Transform; Translate ST To UV; and `usdmaterialpath` written from the prim's bound material.

With those, GeoToUSD can put the geometry back exactly.

Ships on its own under E9 - USD And 3D.

## Scope

**In:**
- Knobs, with labels matching script names:
  - `pathAttribute` (default `path`) and `nameAttribute` (default empty = not written);
  - `traversal`, `purpose` (multi: default, render, proxy, guide), `excludeInactivePrimitives`, `importPointInstances`;
  - `timeSample` with `importFrame`;
  - `pivotLocation`;
  - `importPrimvars` (pattern, default `*`) with `importInheritedPrimvars`, `importAttributes` (pattern, default empty), `nonTransformingPrimvars` (pattern, default `rest`);
  - `writeUsdXform` (on when unpacked) with `applyInverseUsdTransform`;
  - `translateStToUv` (on);
  - `importMaterialPath` (on), which writes `usdmaterialpath`.
- `flattenTransforms` is kept as is; the audit records how it relates to `applyInverseUsdTransform`.
- The full LOP Import + Unpack USD control audit in this file's `## Decisions` at P1.T1.
- Docs in `Engine/Nodes/Geometry/docs/USDToGeo.md`.

**Out:**
- Subdivision and instancing reads beyond Import Point Instances; no milestone.
- Strip Layers Above Layer Breaks: Natron has no layer breaks yet (F27 - USD Layer Authoring owns layer breaks); no milestone here.

**Edge cases:**
- `nameAttribute` set and `pathAttribute` empty: GeoToUSD's default `pathAttributes` (`path,name`) still resolves the name under its prefix.
- Static time sample on an animated prim: every frame reads `importFrame`.
- Excluded inactive prims are not read; with exclusion off they are read and marked `usdactive=0`, so M610 - GeoToUSD Attribute And Material Controls writes them back inactive.
- Prims with a material bound per GeomSubset: `usdmaterialpath` is written per primitive from the subset it belongs to.

**Assumptions:**
- Unconfirmed: Components and Groups traversal follow USD `kind` (component, group/assembly), as the reference does.

## Phase 611.1: Controls

- [ ] M611.P1.T1 — Control audit, naming and traversal knobs
  - files: `Engine/Nodes/Geometry/USDToGeo.{h,cpp}`, `Engine/Nodes/Geometry/GeoUsdConvert.{h,cpp}` (`geoUsdReadPrim` ~:2640-2690, `geoAppendPackedUSD`), `Tests/Geometry/USDToGeoControls_Test.cpp` (new), `Tests/CMakeLists.txt`, this file's `## Decisions`
  - approach: Write the audit from `.local/docs/houdini/nodes/sop/lopimport.md` and `.local/docs/houdini/nodes/sop/unpackusd.md` (and `nodes/lop/sopmodify.md` for the export controls) into `## Decisions`. Add `pathAttribute`, `nameAttribute`, `traversal`, `purpose`, `excludeInactivePrimitives` and `importPointInstances` (one packed primitive per instance). Apply them in the prim walk before reading.
  - verify: gtest on an in-memory stage: Components traversal yields one packed primitive per component; `nameAttribute=name` writes leaf names; an inactive prim is skipped by default.
  - size: M
- [ ] M611.P1.T2 — Time sample and pivot location
  - files: `Engine/Nodes/Geometry/USDToGeo.cpp`, `Tests/Geometry/USDToGeoControls_Test.cpp`
  - approach: `timeSample` Static reads at `importFrame` and makes the cook time-invariant (the hash ignores time). `pivotLocation` puts the packed primitive's point at the bounds centroid or at the prim origin.
  - verify: gtest: Static at frame 10 returns the same P at frames 1 and 20; Origin puts the packed point at the prim's translate.
  - size: M
- [ ] M611.P1.T3 — Primvar and attribute import, ST to UV, usdxform and material path
  - files: `Engine/Nodes/Geometry/GeoUsdConvert.cpp`, `Engine/Nodes/Geometry/USDToGeo.cpp`, `Tests/Geometry/USDToGeoControls_Test.cpp`
  - approach: Filter primvars with `importPrimvars` (plus inherited primvars from ancestors when on) and custom attributes with `importAttributes`. `nonTransformingPrimvars` marks attributes the GeoTransform family must not transform. Use the existing attribute-type metadata if there is one, and record it otherwise. `translateStToUv` renames `st` to `uv`. Unpacked prims get `usdxform`; with `applyInverseUsdTransform` the inverse is applied, so points sit in the prim's local space. `importMaterialPath` writes `usdmaterialpath` from the resolved binding, per subset.
  - verify: gtest: `importPrimvars=^displayColor` drops Cd; `usdxform` equals the prim's world matrix; a prim bound to `/mtl/a` gets `usdmaterialpath=/mtl/a`; with inverse applied, P equals the prim's local points.
  - size: M
- [ ] M611.P1.T4 — Round-trip test and docs
  - files: `Tests/Geometry/GeoRoundTrip_Test.cpp` (new), `Engine/Nodes/Geometry/docs/USDToGeo.md`, `Tests/CMakeLists.txt`
  - approach: A gtest that runs a stage through USDToGeo (unpacked) and GeoToUSD (Overlay, M609's controls) with no edit, and checks that the composed result equals the input for points, primvars, xforms and material bindings. Document every control and the round trip.
  - verify: the round-trip gtest passes for a nested, transformed, material-bound mesh.
  - size: M

**Verification gate:** CI green; release gate green; `USDToGeoControls_Test` and `GeoRoundTrip_Test` pass; the control audit in `## Decisions` has no unassigned controls.

## Decisions
- 2026-11-25 — Full LOP Import + Unpack USD audit, following the user's full-audit choice for M609 - GeoToUSD Path And Placement (planner, from the grill).
