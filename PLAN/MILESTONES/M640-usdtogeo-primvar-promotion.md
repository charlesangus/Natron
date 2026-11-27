# M640 - USDToGeo Primvar Promotion

M611 - USDToGeo Source Controls keeps constant primvars as detail attributes (its audit's "Detail Attributes promotion — Out for now"). The review of PR #406 found that constant instancer primvars then don't reach instance points. Add a promotion control to USDToGeo, with the options Promote to Primitive if Different, Primitive, Point and Vertex. It applies to unpacked prims and point instances alike, so the two agree. The scout found no promote control on M611's branch. Primvar reads are at `USDToGeo.cpp:523-529` (`GetPrimvarsWithValues`, `ComputeFlattened`), and point-instancer unpacking is `appendPointInstances` (:447, transforms :458, proto indices :464, called from :1026-1028). Constant/uniform interpolation maps to `eGeoAttribClassDetail` in `GeoUsdConvert.cpp:2734-2760`. Ships in F42 - Geometry USD Bridge Controls (user, 2026-11-27).

## Scope

**In:**
- The promotion knob on USDToGeo and its effect on constant (and uniform, where the option asks) primvars, for unpacked prims and point instances.

**Out:**
- GeoToUSD's reverse direction (M610 - GeoToUSD Attribute And Material Controls).

**Edge cases:**
- **Promote to Primitive if Different:** a constant primvar whose value is the same on every imported prim stays a detail attribute; otherwise it becomes a primitive attribute. This is the default, matching the reference.
- **Point instances:** an instancer's constant primvar becomes a point attribute on every instance point it produced when the option is Point, or a primitive attribute on the instance's packed prim.
- **Same name, different types across prims:** warn and skip, using the existing warning path (`GeoUsdConvert.cpp:2818-2887`).
- **Instance proxies:** main filters them out (`USDToGeo.cpp:322`, :353); check what M611 reads before relying on their primvars.

## Phase 640.1: Promotion control

- [ ] M640.P1.T1 — Add the Promote knob and apply it to unpacked prims
  - files: `Engine/Nodes/Geo/USDToGeo.{h,cpp}` (`initializeKnobs` next to `_primPattern`, primvar read :523-529), `Engine/Geo/GeoUsdConvert.cpp` (:2734-2760 class selection); `Tests/Native/` USDToGeo test (the one M611 extends)
  - approach: a choice knob `promoteConstants` (label "Promote Constants") with Primitive if Different (default), Primitive, Point, Vertex, plus a None/Detail option that keeps today's behaviour. Pass it into the class selection so constant primvars resolve to the chosen class, expanding the value across elements. For "if Different", gather every imported prim's value first, then decide.
  - verify: tests per option on a two-mesh stage with an equal and a differing constant primvar; `ctest -R USDToGeo` green
  - size: M
- [ ] M640.P1.T2 — Apply the same promotion to point instances
  - files: `Engine/Nodes/Geo/USDToGeo.cpp` (`appendPointInstances` :447-:470, caller :1026-1028); the USDToGeo test
  - approach: route the instancer's constant primvars through T1's promotion so Point gives each instance point the value, and Primitive puts it on each packed prim, matching what unpacked prims get.
  - verify: a test where a constant instancer primvar reaches every instance point under Point, and matches the unpacked-prim result under each option
  - size: M

**Verification gate:** full build and ctest green on F42's branch; the M611 review case (constant instancer primvar on instance points) is covered by a test.
