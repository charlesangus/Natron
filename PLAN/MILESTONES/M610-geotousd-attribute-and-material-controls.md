# M610 - GeoToUSD Attribute And Material Controls

The attribute, material and special-attribute side of the SOP Import port, assigned by M609 - GeoToUSD Path And Placement's control audit. That covers four groups:
- **Import data:** Author Time Samples (If Time Dependent / If Not Specifically Excluded / Never), Topology Attributes (Animated / Static / None), and attribute patterns for which attributes become USD properties, with `^` exclusion and the special tokens.
- **Materials:** binding from a `usdmaterialpath` attribute, plus material references from `usdmaterialreffile`/`usdmaterialrefprim`.
- **Special attributes:** `usdvisibility`, `usdpurpose`, `usdkind`, `usdactive`, `usdinherits`, `usdspecializes`, `usdprimtype`, `usdapischemas`, `usdinstancerpath` and `usdprototypespath`.
- **Prims from points:** a point carrying `usdprimtype` becomes a prim.

Today GeoToUSD exports every attribute except `path`, `name` and `usdSource` (`Engine/Nodes/Geometry/GeoUsdConvert.cpp` ~:1215) and authors no material bindings.

Ships on its own under E9 - USD And 3D.

## Scope

**In:**
- `authorTimeSamples` becomes a choice with the three modes; `topologyAttributes` (Animated / Static / None).
- `attributes` (pattern, default `*`), `indexedAttributes`, `booleanAttributes`, `assetPathAttributes`, `relationshipAttributes` and `setDefaultValueAttributes`, each a pattern with `^` exclusion. Token support per the audit.
- `bindMaterials` (on) reading `usdmaterialpath` (a Material prim, or another prim to copy its binding from), with GeomSubset bindings from a primitive-group `materialBind` where groups are already exported as subsets.
- The special `usd*` primitive attributes listed above.
- Prims from points with `usdprimtype`, placed by the point's transform (`P` plus `orient`/`scale`/`transform` attributes, as the instancer path reads them).
- Docs updated in `Engine/Nodes/Geometry/docs/GeoToUSD.md`.

**Out:**
- Anything M609 - GeoToUSD Path And Placement's audit assigned Out.

**Edge cases:**
- Topology None with changed topology: the topology attributes are not authored and the node warns that the counts no longer match the input.
- `usdmaterialpath` naming a prim absent from the input: the binding is still authored and the node warns.
- A pattern excluding `P`: points are not authored, which is valid for Overlay attribute-only edits.

**Assumptions:**
- Unconfirmed: groups exported as GeomSubsets keep their names, so `materialBind` can target them.

## Phase 610.1: Controls

- [ ] M610.P1.T1 — Time-sample and topology modes
  - files: `Engine/Nodes/Geometry/GeoToUSD.{h,cpp}`, `Engine/Nodes/Geometry/GeoUsdConvert.{h,cpp}`, `Tests/Geometry/GeoToUSDAttributes_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: Turn `authorTimeSamples` into the three-mode choice (time dependency read from the detail's cook, as today). `topologyAttributes` controls whether `faceVertexCounts`/`faceVertexIndices` (and curve counts) are time-sampled, authored as defaults, or skipped. Keep the parallel, cancellable range cook from M376.P2.T3.
  - verify: gtest: Static topology on an animated-P chain authors counts once and points per frame; None authors no counts.
  - size: M
- [ ] M610.P1.T2 — Attribute patterns
  - files: `Engine/Nodes/Geometry/GeoUsdConvert.cpp` (~:1215, attribute mapping ~`GeoUsdConvert.h`:185-189), `Engine/Nodes/Geometry/GeoToUSD.cpp`, `Engine/Geo/GeoOpUtils.{h,cpp}` (`geoNameMatchesPattern` ~:496), `Tests/Geometry/GeoToUSDAttributes_Test.cpp`
  - approach: Filter exported attributes through `attributes`, plus the typed patterns that change how a match is authored (indexed primvar, bool, asset path, relationship, default value). Use `geoNameMatchesPattern` for globs and `^`. Honour the audit's special tokens.
  - verify: gtest: `* ^Cd` drops displayColor; `assetPathAttributes=tex` authors `tex` as an asset; `indexedAttributes=name` authors an indexed primvar.
  - size: M
- [ ] M610.P1.T3 — Material binding from attributes
  - files: `Engine/Nodes/Geometry/GeoUsdConvert.cpp`, `Engine/Nodes/Geometry/GeoToUSD.{h,cpp}`, `Tests/Geometry/GeoToUSDAttributes_Test.cpp`
  - approach: With `bindMaterials` on, a primitive string `usdmaterialpath` binds the partition (or a per-value GeomSubset when values differ within a partition) via `UsdShadeMaterialBindingAPI`. A non-Material target copies that prim's binding. `usdmaterialreffile`/`usdmaterialrefprim` author a referenced Material under the partition. Unbound partitions keep whatever the input composes.
  - verify: gtest: two primitives with different `usdmaterialpath` values in one partition give two GeomSubsets bound to the two materials.
  - size: M
- [ ] M610.P1.T4 — Special usd* attributes and prims from points
  - files: `Engine/Nodes/Geometry/GeoUsdConvert.cpp`, `Engine/Nodes/Geometry/GeoToUSD.cpp`, `Engine/Nodes/Geometry/docs/GeoToUSD.md`, `Tests/Geometry/GeoToUSDAttributes_Test.cpp`
  - approach: Map `usdvisibility`, `usdpurpose`, `usdkind`, `usdactive`, `usdinherits`, `usdspecializes`, `usdprimtype`, `usdapischemas`, `usdinstancerpath` and `usdprototypespath` to their USD metadata and properties, and never export them as primvars. A point with `usdprimtype` and a `path` becomes a prim of that type, with its xform from the point. Document every special attribute.
  - verify: gtest: `usdactive=0` deactivates the prim; `usdkind=component` sets kind; a point with `usdprimtype=Xform` and `path=/p` yields an Xform at the point's position.
  - size: M

**Verification gate:** CI green; release gate green; `GeoToUSDAttributes_Test`, `GeoToUSDPaths_Test` and the existing GeoToUSD tests pass; every control M609 - GeoToUSD Path And Placement's audit assigned to M610 is implemented.

## Decisions
- 2026-11-25 — Split from M609 - GeoToUSD Path And Placement for size; together they cover the full SOP Import audit (user chose full coverage, grill).
