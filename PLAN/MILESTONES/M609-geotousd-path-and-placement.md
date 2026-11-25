# M609 - GeoToUSD Path And Placement

Make it explicit where GeoToUSD's prims go and how they combine with its Scene input. Today the rules are implicit:
- the detail is partitioned by `path`, or by `name` when there is no `path`;
- relative and empty paths go under `rootPath` (default `/geo`);
- a `def` silently becomes an `over` wherever the input already has the path (`ensurePrim`, `Engine/Nodes/Geometry/GeoUsdConvert.cpp` ~:1035);
- colliding non-USD geometry is moved under `rootPath`.

This milestone ports the path, placement and hierarchy controls of the reference app's SOP Import node, with **its defaults exactly** (user, 2026-11-25):
- Path Attributes (`path,name`), Path Prefix (`/<node name>`) and Prefix Absolute Paths;
- Other Primitives (Define, the default; Overlay; Overlay Transforms) and Define Only Leaf Primitives;
- Packed USD Primitives and Packed Primitives handling, Parent Primitive Type and Kind Authoring;
- Adjust Transforms For Input Hierarchy (on);
- Load As Reference with its reference type and prim, and the layer style (sublayer as is, copy into the node's layer — today's `copyIntoLayer`).

The full control audit lives here, and controls on the attribute and material side go to M610 - GeoToUSD Attribute And Material Controls.

Not a Feature member: it ships on its own under E9 - USD And 3D.

## Scope

**In:**
- Knobs, with labels matching script names:
  - `pathAttributes` (string, default `path,name`): the first non-empty attribute wins.
  - `pathPrefix` (default `/` + node name, replacing `rootPath`).
  - `prefixAbsolutePaths` (off).
  - `otherPrimitives` (Define / Overlay / Overlay Transforms, default Define).
  - `defineOnlyLeafPrimitives` (off).
  - `packedUsdPrimitives` (Ignore / Overlay Transforms / Overlay Transforms And Attributes; default Overlay Transforms).
  - `packedPrimitives` (Create Xforms / Native Instances / Point Instancer / Unpack; extends today's knob of that name).
  - `parentPrimitiveType` and `kindAuthoring`.
  - `adjustTransformsForInputHierarchy` (on).
  - `loadAsReference` with `referenceType` and `referencePrimitive`.
  - `layerStyle` (Sublayer / Copy Into Node Layer, replacing `copyIntoLayer`).
- Path rules from the reference app: invalid characters become `_`; incompatible prim types at one path get `_0`, `_1` suffixes; with no path attribute, automatic names (`mesh_0`).
- The node message reports each partition's path and whether it was defined or overlaid, so you can see where the geometry went.
- Docs: `Engine/Nodes/Geometry/docs/GeoToUSD.md` rewritten around the controls, with the round-trip recipe (USDRead → USDToGeo → … → GeoToUSD set to Overlay → USDMerge), without naming other applications.
- The full SOP Import control audit as a table in this file's `## Decisions` at P1.T1: every control In (M609 or M610 - GeoToUSD Attribute And Material Controls) or Out, with the reason.

**Out:**
- Attribute patterns, material binding, `usd*` special attributes, prims from points, time-sample and topology modes (M610 - GeoToUSD Attribute And Material Controls).
- Merge order in USDMerge (M560 - USDMerge Opinion Order).
- The one-node in-place workflow (M612 - USDGeoModify Node).
- The detail-attribute override mechanism the reference app has (`usdconfig*` detail attributes written by a configure node); no milestone, since knobs can be expression-driven.

**Edge cases:**
- Define on a path the input already has: authored as `def` in the stronger layer, so the prim is redefined over the original (type and authored properties replaced). Unauthored properties still compose from the input, as in the reference app.
- Overlay where the input has no prim at that path: an `over` with no `def` below is inactive in the composed stage; the node warns and lists the paths.
- Adjust Transforms For Input Hierarchy with flattened (world-space) geometry from USDToGeo (`usdWorldSpace=1`): ancestors' world transform is inverted so the world position is kept.
- Two partitions resolving to one path with the same type: merged into one prim, as today.

**Assumptions:**
- Unconfirmed: `pathPrefix` default `/` + node name is evaluated by an expression on the node's script name, so renaming the node moves the prims, as in the reference app.

## Phase 609.1: Controls

- [ ] M609.P1.T1 — Control audit and the path knobs
  - files: `Engine/Nodes/Geometry/GeoToUSD.{h,cpp}` (~:325-421), `Engine/Nodes/Geometry/GeoUsdConvert.{h,cpp}` (~:160-170, ~:791-860), `Tests/Geometry/GeoToUSDPaths_Test.cpp` (new), `Tests/CMakeLists.txt`, this milestone file's `## Decisions`
  - approach: First write the audit of every SOP Import control (from `.local/docs/houdini/nodes/lop/sopimport.md` and `.local/docs/houdini/solaris/sop_import.md`) into `## Decisions`, each In (M609 or M610) or Out with a reason. Then replace `rootPath` with `pathPrefix` and add `pathAttributes` and `prefixAbsolutePaths`. Partitioning takes the first non-empty listed attribute, sanitises names, suffixes incompatible types, and names unattributed geometry `mesh_0` and so on.
  - verify: gtest: `path=/a/b` with prefix `/pre` stays `/a/b`, and with Prefix Absolute it becomes `/pre/a/b`; `name=leg` gives `/pre/leg`; a mesh and curves at one path become `_0`/`_1` siblings; no attribute gives `mesh_0`.
  - size: M
- [ ] M609.P1.T2 — Define/Overlay modes, leaf-only definition and parent type/kind
  - files: `Engine/Nodes/Geometry/GeoUsdConvert.cpp` (`ensurePrim` ~:1035, ~:1445-1451), `Engine/Nodes/Geometry/GeoToUSD.cpp`, `Tests/Geometry/GeoToUSDPaths_Test.cpp`
  - approach: Replace the implicit over rule with `otherPrimitives`: Define authors `def`; Overlay authors `over` with the full data; Overlay Transforms authors `over` with only the xform. Remove the silent `eGeoUsdPlacedCollision` move (Define on an existing path redefines it). `defineOnlyLeafPrimitives` authors intermediate prims as overs. `parentPrimitiveType` and `kindAuthoring` apply to the intermediate prims created. Report Overlay paths with no prim below in the node message.
  - verify: gtest on an input stage containing `/a/m`: Define gives a `def` spec, Overlay an `over`, and Overlay Transforms an `over` holding only `xformOp:transform`; Overlay on a missing path warns.
  - size: M
- [ ] M609.P1.T3 — Packed handling and Adjust Transforms For Input Hierarchy
  - files: `Engine/Nodes/Geometry/GeoUsdConvert.{h,cpp}` (`eGeoUsdPrimOverride` ~:72), `Engine/Nodes/Geometry/GeoToUSD.{h,cpp}`, `Tests/Geometry/GeoToUSDPaths_Test.cpp`
  - approach: `packedUsdPrimitives` chooses Ignore, Overlay Transforms or Overlay Transforms And Attributes (constant primvars). `packedPrimitives` gains Create Xforms. `adjustTransformsForInputHierarchy` inverts the world transform of the input's existing ancestors of each prim and authors the result as the local transform, so world position is kept. That generalises today's "relative to the input's parent" rule for packed overrides to every partition.
  - verify: gtest: a world-space detail under an input ancestor translated by (1,0,0) lands at the same world position with the adjustment on, and is offset by (1,0,0) with it off.
  - size: M
- [ ] M609.P1.T4 — Load As Reference and layer style
  - files: `Engine/Nodes/Geometry/GeoToUSD.cpp` (~:989-1020, `emitSceneLayer`), `Tests/Geometry/GeoToUSDPaths_Test.cpp`
  - approach: `layerStyle` replaces `copyIntoLayer`: Sublayer (today's default, the `natrongeo` layer as a sublayer) or Copy Into Node Layer. `loadAsReference` instead references or payloads the geometry layer onto a prim, with `referenceType` (Weaker/Stronger Payload, Weaker/Stronger Reference) and `referencePrimitive` (Automatic/Default/Specific). The prim path is the prefix; it is created with `parentPrimitiveType` if missing.
  - verify: gtest: Load As Reference with a payload gives a prim with one payload arc to the geometry layer, and the composed stage has the mesh under it.
  - size: M
- [ ] M609.P1.T5 — Node message, docs and GUI check
  - files: `Engine/Nodes/Geometry/GeoToUSD.cpp`, `Engine/Nodes/Geometry/docs/GeoToUSD.md`, `Tests/gui/m609_geotousd_paths.py`
  - approach: The node message lists up to 20 partitions, each with its path and whether it was defined or overlaid. The docs explain each control and the round-trip recipe. A GUI script builds USDRead(fixture) → USDToGeo → GeoTransform → GeoToUSD(Overlay) → USDMerge and checks the edited prim is at its original path with its parent xform intact.
  - verify: the script passes under Xvfb; the docs build (the existing docs check, if there is one).
  - size: M

**Verification gate:** CI green; release gate green with `--gui` running `Tests/gui/m609_geotousd_paths.py`; `GeoToUSDPaths_Test` and the existing GeoToUSD/GeoUsdConvert tests pass; the control audit in `## Decisions` has no unassigned controls.

## Decisions
- 2026-11-25 — Full SOP Import control audit, In or Out each with a reason (user, grill).
- 2026-11-25 — The reference app's defaults exactly: Define, prefix `/<node name>`, adjust-for-hierarchy on; the round trip picks Overlay explicitly, or uses M612 - USDGeoModify Node (user, grill).
- 2026-11-25 — Split from M610 - GeoToUSD Attribute And Material Controls for size (planner).
