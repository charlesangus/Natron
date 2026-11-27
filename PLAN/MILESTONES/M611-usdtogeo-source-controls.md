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

- [x] M611.P1.T1 — Control audit, naming and traversal knobs
  - files: `Engine/Nodes/Geometry/USDToGeo.{h,cpp}`, `Engine/Nodes/Geometry/GeoUsdConvert.{h,cpp}` (`geoUsdReadPrim` ~:2640-2690, `geoAppendPackedUSD`), `Tests/Geometry/USDToGeoControls_Test.cpp` (new), `Tests/CMakeLists.txt`, this file's `## Decisions`
  - approach: Write the audit from `.local/docs/houdini/nodes/sop/lopimport.md` and `.local/docs/houdini/nodes/sop/unpackusd.md` (and `nodes/lop/sopmodify.md` for the export controls) into `## Decisions`. Add `pathAttribute`, `nameAttribute`, `traversal`, `purpose`, `excludeInactivePrimitives` and `importPointInstances` (one packed primitive per instance). Apply them in the prim walk before reading.
  - verify: gtest on an in-memory stage: Components traversal yields one packed primitive per component; `nameAttribute=name` writes leaf names; an inactive prim is skipped by default.
  - size: M
- [x] M611.P1.T2 — Time sample and pivot location
  - files: `Engine/Nodes/Geometry/USDToGeo.cpp`, `Tests/Geometry/USDToGeoControls_Test.cpp`
  - approach: `timeSample` Static reads at `importFrame` and makes the cook time-invariant (the hash ignores time). `pivotLocation` puts the packed primitive's point at the bounds centroid or at the prim origin.
  - verify: gtest: Static at frame 10 returns the same P at frames 1 and 20; Origin puts the packed point at the prim's translate.
  - size: M
- [x] M611.P1.T3 — Primvar and attribute import, ST to UV, usdxform and material path
  - files: `Engine/Nodes/Geometry/GeoUsdConvert.cpp`, `Engine/Nodes/Geometry/USDToGeo.cpp`, `Tests/Geometry/USDToGeoControls_Test.cpp`
  - approach: Filter primvars with `importPrimvars` (plus inherited primvars from ancestors when on) and custom attributes with `importAttributes`. `nonTransformingPrimvars` marks attributes the GeoTransform family must not transform. Use the existing attribute-type metadata if there is one, and record it otherwise. `translateStToUv` renames `st` to `uv`. Unpacked prims get `usdxform`; with `applyInverseUsdTransform` the inverse is applied, so points sit in the prim's local space. `importMaterialPath` writes `usdmaterialpath` from the resolved binding, per subset.
  - verify: gtest: `importPrimvars=^displayColor` drops Cd; `usdxform` equals the prim's world matrix; a prim bound to `/mtl/a` gets `usdmaterialpath=/mtl/a`; with inverse applied, P equals the prim's local points.
  - size: M
- [x] M611.P1.T4 — Round-trip test and docs
  - files: `Tests/Geometry/GeoRoundTrip_Test.cpp` (new), `Engine/Nodes/Geometry/docs/USDToGeo.md`, `Tests/CMakeLists.txt`
  - approach: A gtest that runs a stage through USDToGeo (unpacked) and GeoToUSD (Overlay, M609's controls) with no edit, and checks that the composed result equals the input for points, primvars, xforms and material bindings. Document every control and the round trip.
  - verify: the round-trip gtest passes for a nested, transformed, material-bound mesh.
  - size: M

**Verification gate:** CI green; release gate green; `USDToGeoControls_Test` and `GeoRoundTrip_Test` pass; the control audit in `## Decisions` has no unassigned controls.

## Decisions
- 2026-11-25 — Full LOP Import + Unpack USD audit, following the user's full-audit choice for M609 - GeoToUSD Path And Placement (planner, from the grill).
- 2026-11-26 — P1.T1 control audit (implementer). Reference: sop/lopimport.md, sop/unpackusd.md, lop/sopmodify.md. Every control is In, Folded or Out:
  - LOP Path — Out: the node's scene input is the source; a path to another network node is a side channel.
  - Primitives — In as the existing `primPattern` ("Prim Pattern").
  - Exclude Inactive Primitives — In, `excludeInactivePrimitives`, default on. Off reads inactive prims through an additive `SceneOps::selectPrimsWithPredicate` (defined, non-abstract; an inactive prim is never loaded, so the loaded predicate would hide it) and writes `usdactive` (1/0) on their elements, only when an inactive prim was read.
  - Import Point Instances — In, `importPointInstances`, default off, packed only. One packed USD primitive per instance on its prototype; ids/velocities/angularVelocities/accelerations become point `id`/`v`/`w`/`accel`; per-instance primvars follow `importPrimvars`; `invisibleIds` give `usdvisibility=invisible`; masked instances are dropped unless inactive prims are read (then `usdactive=0`). Per-instance intrinsics (source instancer path, instance index) are Out: no packed-intrinsic store for them yet, and the instancer path is not needed to place an instance.
  - Purpose — In, `purpose`, a space-separated token string (no multi-select knob exists). Default `default render proxy` (unconfirmed: the reference default is not stated in the docs). Unknown tokens are an error. A non-default purpose that is not read prunes its subtree; the purpose list also drives the bounds cache.
  - Traversal (No Traversal / Components / Gprims / Groups) — In, `traversal`, default No Traversal. Outermost match below each matched prim; Gprims also stops at point instancers; Groups means kind group (assembly IsA group).
  - Strip Layers Above Layer Breaks — Out: belongs to F27 - USD Layer Authoring (layer-break authoring does not exist yet).
  - Time Sample — In, `timeSample` (Animated / Static), default Animated.
  - Import Frame (animated) — Folded into Animated: it reads the frame being rendered; retiming is a TimeOffset node's job, not a per-node expression.
  - Static Import Frame — In as `importFrame` ("Import Frame"), default 1, enabled only for Static. Static evaluates the input and the time code at that frame and reports the output as not frame varying (cache key ignores time).
  - Path Attribute / Name Attribute — In, `pathAttribute` (default `path`) and `nameAttribute` (default blank, not written) as attribute-name strings; blank writes none. Points prims carry them on points.
  - Display As (viewport LOD) — Out: viewer drawing of packed USD primitives is not a geometry property here; packed primitives draw from their bounds.
  - Pivot Location (Centroid / Origin) — In, `pivotLocation`, default Centroid (reference default), packed only. Empty bounds fall back to the origin.
  - Unpack USD Group — Folded into `primPattern` + `traversal` (the node reads prims, not packed primitives).
  - Delete Original Prims, Output, Iterations, Transfer Attributes, Transfer Groups — Out: those describe unpacking existing packed primitives in place; here each read is fresh, and attributes of the source packed primitives do not exist.
  - File Path Attribute / Instance Level Attribute — Out: no file-backed packed primitives or nested-instance levels in the fork's packed USD primitive.
  - Detail Attributes promotion (Promote to Primitive if Different / Primitive / Point / Vertex) — Out for now: constant primvars stay detail attributes, as before. Possible gap; a follow-up could add it to GeoUsdReadOptions.
  - Import Primvars — In, `importPrimvars`, default `*`, globs over USD names with `^` removal (geoNameMatchesPattern).
  - Import Inherited Primvars — In, `importInheritedPrimvars`, default off (FindPrimvarsWithInheritance; constant ones only, own primvar wins).
  - Import Computed Visibility — Out: visibility is left on the scene (GeoToUSD trims unchanged values), and `usdvisibility` is only written for instancer invisibleIds.
  - Import Attributes — In, `importAttributes`, default blank; custom non-primvar, non-xformOp attributes only; class guessed from the array size (point, vertex, primitive, else detail).
  - Non-transforming Primvars — In, `nonTransformingPrimvars`, default `rest`. Recorded with the existing attribute metadata: typeInfo None, which GeoTransform and family already go by, so no new metadata.
  - Translate ST primvar to UV — In, `translateStToUv`, default on.
  - Import Relationships — Out: no string-array relationship attributes in the geometry model; material bindings are covered by `importMaterialPath`.
  - usdxform / Apply Inverse USD Transform / usdmaterialpath (SOP Modify and SOP Import side) — In on the import side as `writeUsdXform` (default on; Float64 x16, typeInfo None, local-to-world, a point attribute on points prims), `applyInverseUsdTransform` (default off) and `importMaterialPath` (default on; subset bindings per face, written only when a material is bound). GeoToUSD skips `usdxform` and `usdmaterialpath` when planning primvars.
  - Apply Inverse USD Transform is redundant with Flatten Transforms off: its result is the local points. Implemented literally (it overrides flatten, and `usdWorldSpace` is then not written). Kept (user, 2026-11-26).
  - The remaining SOP Modify (export) controls — Out of this milestone: M609, M610 and M612.
  - Tests live in `Tests/Scene/` (globbed by CMake), not the nonexistent `Tests/Geometry/`, so `Tests/CMakeLists.txt` is unchanged.
- 2026-11-26 — **Apply Inverse USD Transform kept (user):** a port carries every control, and M612's round trip uses it explicitly. Test fixes for the inactive-prim predicate and invisible point instances in 35fee8ed66.
- 2026-11-27 (PM) — T1–T4 landed in f240d92423 and 35fee8ed66 (milestone/m611-usdtogeo-source-controls); m611-gate3 release (full ctest) green at 4fb0ed2741 after merging F42 (main with the #397 cancel-latency fix).
