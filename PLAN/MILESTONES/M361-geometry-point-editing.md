# M361 - Geometry Point Editing

Last code member of F15 - Native Geometry. Component selection in the Viewer's 3D view, in the
geometry context only: point, edge and primitive picking. Selections reach nodes as SOPs
do it, by Tab-creating a node from the selection or reselecting for an existing node, and can be
grown, shrunk and converted between classes. `GeoEdit`, Houdini's Edit SOP, drags the picked
points or packed primitives with the translate gizmo, keyed by global point number. On the
engine side, operators gain a Group Type knob with "Guess from Group", edge groups where they make
sense, `GeoDelete` edge removal, and `GeoGroup` Expand and Promote modes. Also the Feature's
headless integration test and the scale bench.
Shared design: M54 - Native Geometry Data Model's intro and Scope, `PLAN/DECISIONS/2026-11-08-native-geometry-follows-sops.md`, `PLAN/DECISIONS/2026-10-27-geometry-bridges-follow-solaris.md`.

Member of F15 - Native Geometry: branches off and merges into `feature/f15-native-geometry` (PLAN-FORMAT §9b); the Feature's UAT covers it.

## Scope

**In:**
- Point, edge and primitive pick modes with click and marquee, highlighted through `HdSelection` plus guide prims, only when the Viewer's active input is geometry.
- A live viewer selection (class plus element set). Switching pick mode converts it. Viewer shortcuts grow, shrink and convert it.
- Selection → node, both of Houdini's paths: Tab-create a geometry node from the viewer with its group and group type filled in from the selection, and "Reselect for current node" (backtick, then Enter) to rewrite an existing node's group.
- A pattern encoder from an element set to compact Houdini group syntax (`0-9 14`, edge paths `p3-4-5`) that `geoSelectElements` reads back exactly.
- Group-class conversion (Promote) and topological grow/shrink (Expand) helpers, used by the viewer and by new `GeoGroup` Expand and Promote modes.
- A Group Type knob defaulting to "Guess from Group" on every operator that accepts more than one class. Edges are added to `GeoTransform`, `GeoDisplace` and `GeoDelete`. `GeoDelete` removes edges as Blast SOP does.
- `GeoEdit` keyed by global point number, moving packed primitives by their transforms, with one undo entry per drag. An edge selection drags the edges' points.
- The geometry pipeline integration test and the 10M-point GPU-host bench.

**Out:**
- Vertex (face-corner) pick mode and vertex selections from the viewer (none; a later follow-up).
- Edge group types on operators other than `GeoTransform`, `GeoDisplace` and `GeoDelete` (none). Those operators convert an edge selection to their accepted class instead.
- An edge run-over for `GeoWrangle` (none).
- Component selection or editing in the scene context (M232 - Scene Selection To Nodes).
- Topology-changing interactive tools, apart from `GeoDelete`'s edge removal (none).
- Bench timing or memory thresholds (none).

**Edge cases:**
- Upstream topology changes under a written group pattern: it keeps its numbers and may select different elements, as in SOPs. The live viewer selection drops numbers past the new count. Only `GeoEdit` warns: when the upstream point or primitive count changes, it skips every edit and sets a warning.
- An edge selection fed to a node that can't take edges is converted to the node's accepted class: points become the edges' endpoints, and primitives become those containing a selected edge.
- Pick-mode switch: points → primitives keeps only primitives whose points are all selected; primitives → points keeps all their points; edges convert as above; points → edges keeps edges with both ends selected.
- "Guess from Group" with a pattern that mixes classes (an edge path beside a primitive group name) sets a node error naming the conflicting terms.
- An empty selection Tab-creates the node with an empty group (everything), as in SOPs.
- Packed primitives pick and edit as whole primitives. In edge mode they don't pick.
- Reselect with no geometry node selected, or with one that has no group knob: the shortcut does nothing and the viewer status line says why.

**Assumptions:**
- Storm's `HdxPickTokens->pickEdges` hit reports an `edgeIndex` that maps to a point pair through the mesh's face-vertex topology, so a hit can be turned into a `GeoEdge` via the partition table. Verify at P1.T1.
- Blast SOP's Edges type opens each polygon at a deleted edge (a closed polygon becomes an open polyline, and a polygon whose edges are all deleted is removed). Confirm against Houdini at claim.
- Group Promote SOP's "Include only elements entirely contained" defaults to off. `GeoGroup` Promote copies that default.

Requires M358 - Geometry Bridges And Display, M359 - Geometry Operators and M360 - Geometry Kernel Language (the integration test uses their nodes, and P3 extends M359 - Geometry Operators' nodes), and M376 - Geometry Display (the geometry display and the viewer-private session layer, which in turn builds on F4 - Heavy Scene Viewer's viewer stage). Gizmo from M20.P2.T1. All nodes are `NativeEffectBase` subclasses in `Engine/Nodes/Geometry/`, each registered through its own `.reg.inc` (the pattern in `Engine/Nodes/Scene/USDCard.reg.inc`), under `PLUGIN_GROUP_3D`. Native geometry uses SOP attribute names (`P`, `N`, `uv`, `Cd`, `pscale`, `orient`, `path`, `name`); only `GeoUsdConvert` maps them to USD names. Group patterns are parsed by `geoSelectElements` (`Engine/Geo/GeoOpUtils.h`); edge groups are `GeoEdgeGroup` (`Engine/Geo/GeoGroup.h`); point → vertex → primitive adjacency is `GeoReverseTopology` (`Engine/Geo/GeoTopology.h`). GUI checks are `Tests/gui` scripts, run remotely with an explicit `--gui`.

## Phase 361.3: Group classes and conversions (engine)

- [ ] M361.P3.T1 — Group Promote and Expand helpers
  - files: `Engine/Geo/GeoGroupOps.h`/`.cpp` (new), `Tests/GeoGroupOps_Test.cpp`, `Tests/CMakeLists.txt`, `Engine/CMakeLists.txt`
  - approach: free functions on `GeoDetail` over element sets: point, edge and primitive bitsets, plus `GeoEdge` lists for edges. **Promote** converts between points, edges and primitives with an "entirely contained" flag. Points → primitives requires all of a primitive's points with the flag, any with it off. Primitives → points takes all their points. Edges → points takes the endpoints. Edges → primitives takes primitives using an edge. Points → edges takes edges with both ends with the flag, either end without. Primitives → edges takes all their edges. **Expand** grows (positive steps) or shrinks (negative) by topological neighbours through `GeoReverseTopology`: points through shared edges, primitives through shared edges, and edges through shared points. Shrinking removes elements on the set's boundary. Tombstoned elements are never returned.
  - verify: unit tests on a 4×4 grid — promoting a 2×2 point block to primitives gives one primitive with the flag and nine without; edges → points → edges round-trips a closed edge loop; expanding one centre point by 1 gives it plus its four edge neighbours; expand +1 then −1 of a 2×2 primitive block returns the block; deleted elements never appear.
  - size: M

- [ ] M361.P3.T2 — Selection pattern encoder
  - files: `Engine/Geo/GeoOpUtils.h`/`.cpp`, `Tests/GeoOpUtils_Test.cpp`
  - approach: `geoEncodePattern` turns a point, primitive or vertex element set into the shortest pattern in Houdini group syntax that `geoSelectElements` reads back. It coalesces runs into `A-B` and regular strides into `A-B:step`, and emits ascending space-separated terms. It turns a `GeoEdge` list into edge point paths (`p3-4-5`), chaining edges that share endpoints into one path. An empty set encodes as an empty pattern only when the caller asks for "everything". Otherwise it encodes as a pattern that selects nothing, which needs a documented token: add `^*` support to `geoSelectElements` if no such token exists.
  - verify: unit tests — for random sets over a 10k-element detail, `geoSelectElements(geoEncodePattern(s)) == s` for points, primitives and edges; `{0..9, 14}` encodes as `0-9 14`; `{0,2,4,6}` as `0-6:2`; a closed edge loop encodes as one path; the empty set round-trips as empty.
  - size: M

- [ ] M361.P3.T3 — Group Type knob with "Guess from Group" and edges on Transform and Displace
  - files: `Engine/Geo/GeoOpUtils.h`/`.cpp` (`GeoGroupTypeEnum`, `geoGuessGroupType`), `Engine/Nodes/Geometry/GeoTransform.h`/`.cpp`, `Engine/Nodes/Geometry/GeoDisplace.h`/`.cpp`, `Engine/Nodes/Geometry/GeoCopyToPoints.h`/`.cpp`, `Tests/GeoTransform_Test.cpp`, `Tests/GeoDisplace_Test.cpp`, `Tests/GeoOpUtils_Test.cpp`
  - approach: extend `GeoGroupTypeEnum` with Guess and Edges. `geoGuessGroupType(detail, pattern, allowed, fallback)` resolves the class: an edge path term means edges; a group name or glob means the class whose group names it matches (primitive groups first, then point, then edge, as Houdini does); `@attr` means the attribute's class; plain numbers mean the node's fallback. Mixed classes set an error naming the terms. Each operator that accepts more than one class gets a "Group Type" choice knob (script name `groupType`, label "Group Type"), default Guess: `GeoTransform` (points, edges, primitives), `GeoDisplace` (points, edges) and `GeoCopyToPoints` (its current classes). Edges on Transform and Displace resolve to the edges' endpoints through M361.P3.T1's Promote and then run the point path. The other single-class operators are unchanged.
  - verify: unit tests — Guess picks primitives for a primitive group name, edges for `p0-1-2` and the fallback for `0-5`; mixed `p0-1 myPrimGroup` errors; `GeoTransform` with an edge group moves exactly the edges' points; `GeoDisplace` with Edges displaces only those points; Guess with an empty group transforms everything.
  - size: M

- [ ] M361.P3.T4 — `GeoDelete` edges and Group Type
  - files: `Engine/Nodes/Geometry/GeoDelete.h`/`.cpp`, `Tests/GeoDelete_Test.cpp`
  - approach: give `GeoDelete` the Group Type knob from M361.P3.T3 (Guess, points, edges, primitives). Edges follow Blast SOP's Edges type: each polygon using a deleted edge is opened at it, so a closed polygon becomes an open polyline starting after the cut and a polygon cut twice splits into polylines. A polygon whose edges are all deleted is removed, and open polylines lose the deleted segment. Attributes carry to the resulting primitives and vertices, and tombstoning follows M54.P1.T5. "Delete non-selected" applies to edges too. Confirm the Blast behaviour against Houdini first (Scope assumption), and record any difference in `## Decisions`.
  - verify: unit tests — deleting one edge of a closed quad gives one open 4-point polyline; deleting two opposite edges gives two 2-point polylines; deleting all four removes the primitive; primitive and vertex attributes carry over; point count is unchanged.
  - size: M

- [ ] M361.P3.T5 — `GeoGroup` Expand and Promote modes
  - files: `Engine/Nodes/Geometry/GeoGroup.h`/`.cpp`, `Engine/Nodes/Geometry/docs/GeoGroup.md`, `Tests/GeoGroup_Test.cpp`
  - approach: add two sections to `GeoGroup` after its existing tests. **Expand** (toggle `expand`, `expandSteps` integer, negative shrinks) applies M361.P3.T1's Expand to the group the node produces. **Promote** (toggle `promote`, `promoteTo` choice of points/edges/primitives, `entirelyContained` default off, `keepOriginal` default off) converts the produced group into a group of the target class with the same name, as Group Promote SOP does. Labels match the script names. Only group storage detaches.
  - verify: unit tests — a one-point group with Expand 2 on a grid gives the expected diamond; Promote of a 2×2 point block to primitives with and without `entirelyContained` gives one and nine primitives; `keepOriginal` leaves the point group in place; all other attribute pages stay shared.
  - size: M

## Phase 361.1: Picking and editing

- [ ] M361.P1.T1 — 3D-view point, edge and primitive picking
  - files: `Gui/SceneViewport.cpp`/`.h`, `Gui/ActionShortcuts.h`, `Tests/gui/m361_geo_pick.py`
  - approach: point, edge and primitive pick modes using `UsdImagingGLEngine::TestIntersection` with the pick target set to `pickPoints`, `pickEdges` or `pickFaces` (Storm renders the id AOVs for this), with click and marquee select (`resolveUnique` for the marquee). Selection is stored as global point numbers, primitive numbers or `GeoEdge` point pairs, mapped from (prim path, index) through the partition table; an edge hit's `edgeIndex` is resolved to its two points through the mesh's face-vertex topology (Scope assumption). Highlighting uses Storm's own selection: an `HdSelection` with point, edge or element-index highlighting through the engine's selection tracker, plus guide prims (purpose `guide`) in the viewer-private session layer for point markers and the marquee. Nothing is drawn by a second GL pass outside Hydra. Mode buttons sit in the Viewer's 3D toolbar, and their shortcuts register under `kShortcutGroupViewer3D` with defaults that avoid existing Viewer keys. Component display and picking exist **only in the geometry context**, when the Viewer's active input is geometry. In the scene context, picking stays at prim level (M19.P2.T3, M232 - Scene Selection To Nodes), as in Solaris. Packed primitives pick as whole primitives and don't pick in edge mode.
  - verify: GUI script — clicking a corner of a `GeoPrimitive` cube selects that point and highlights it; clicking an edge selects that point pair; marquee selects the expected set in each mode; a packed `USDToGeo` output picks whole packed primitives; viewing a `GeoToUSD` downstream of the same cube offers prim picking only, with no component modes.
  - size: L

- [ ] M361.P1.T2 — `GeoEdit` node with viewport drag
  - files: `Engine/Nodes/Geometry/GeoEdit.h`/`.cpp`/`.reg.inc`, `Gui/SceneViewport.cpp` (drag → knob writeback), `Tests/GeoEdit_Test.cpp`
  - approach: the Edit SOP. It stores a sparse map from global point number to offset, and from packed primitive number to transform delta, in a serialized knob (a `KnobString` holding a compact text encoding is acceptable; a table knob if one fits). It also records the input's point and primitive counts at edit time. Render applies the offsets to `P` (detaching only the touched pages) and multiplies the packed primitives' transforms. It then recomputes `N` if present. In the viewport, with the node selected and points, edges or packed primitives picked (M361.P1.T1), M20.P2.T1's translate gizmo drags them, and release writes one knob change (one undo entry). An edge selection drags the edges' endpoints, and a non-packed primitive selection drags its points. Topology guard: if the input's point or primitive count differs from the recorded count, skip all edits and set a node warning.
  - verify: unit test — an edit map round-trips through serialization and moves exactly the listed points; a packed primitive's edit moves its transform only; changing the upstream point count triggers the warning and leaves points unmodified. Manual — drag creates one undo entry; dragging an edge moves its two points.
  - size: L

## Phase 361.4: Selection workflow in the Viewer

- [ ] M361.P4.T1 — Live selection: mode-switch conversion, grow, shrink and convert
  - files: `Gui/SceneViewport.cpp`/`.h`, `Gui/ActionShortcuts.h`, `Tests/gui/m361_geo_selection.py`
  - approach: the Viewer holds one live geometry selection (class plus set) for the displayed detail. Switching pick mode converts it with M361.P3.T1's Promote, entirely-contained on. Grow, Shrink and "Convert to Points/Edges/Primitives" actions apply Expand ±1 and Promote, and register under `kShortcutGroupViewer3D` with defaults that avoid existing Viewer keys. When the detail changes, numbers past the new count are dropped and nothing else is remapped (SOP behaviour). The selection is exposed through one accessor so M337 - Geometry Spreadsheet can sync with it later.
  - verify: GUI script — select one grid point, Grow twice → the expected diamond is highlighted; switch to primitive mode → only fully covered primitives remain; Shrink returns the previous set; deleting points upstream drops out-of-range numbers without error.
  - size: M

- [ ] M361.P4.T2 — Tab-create a geometry node from the selection
  - files: `Gui/SceneViewport.cpp`, `Gui/TabMenuBridge.cpp`/`.h`, `Tests/gui/m361_geo_tab_create.py`
  - approach: Tab in the Viewer's 3D view in the geometry context opens the node creation menu (through `TabMenuBridge`, as the node graph does). Choosing a geometry node inserts it after the node feeding the Viewer's active input, rewires that input's downstream links through it, and points the Viewer at it. If the node has a `group` knob, the live selection is converted to the node's accepted class (edges kept where the node's Group Type allows them, else converted per M361.P3.T1), encoded with M361.P3.T2, and written into `group`, with `groupType` set to the selection's class. The whole insertion is one undo entry. An empty selection leaves `group` empty.
  - verify: GUI script — select edges of a grid, Tab-create `GeoTransform` → it sits between the source and the Viewer with `group` = the edge path and Group Type = Edges; Tab-create `GeoSubdivide` from a point selection → its group holds the promoted primitives; one undo removes the node and restores the links.
  - size: L

- [ ] M361.P4.T3 — Reselect for the current node
  - files: `Gui/SceneViewport.cpp`, `Gui/ActionShortcuts.h`, `Tests/gui/m361_geo_reselect.py`
  - approach: "Reselect for Current Node" (default backtick, `kShortcutGroupViewer3D`) applies when exactly one geometry node with a `group` knob is selected. It loads that node's current group into the live selection, decoded with `geoSelectElements` on the node's input detail and shown in the matching pick mode. Enter then writes the new pattern and `groupType` back into that node (one undo entry), the same encoding as M361.P4.T2. Escape cancels. Enter without reselect keeps the Tab-create path. In any other state the shortcut does nothing and the status line says why.
  - verify: GUI script — with a `GeoDelete` selected, backtick shows its primitive group highlighted; adding two primitives and Enter writes the extended pattern; undo restores the old one; with only a `GeoMerge` selected (no group knob) the shortcut does nothing and the status line explains.
  - size: M

## Phase 361.2: Integration and scale

- [ ] M361.P2.T1 — Geometry pipeline integration test in CI
  - files: `Tests/GeoPipeline_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: headless graph — `GeoPrimitive`(grid) → `GeoTransform` → `GeoGroup`(bbox, then Expand 1 and Promote to primitives) → `GeoDelete` → `GeoDisplace`(constant) → `GeoNormals` → `GeoWrangle`(`@P.y += 0.1`) → `GeoEdit`(one point) → `GeoToUSD` → `USDMerge`(with `USDRead`) → `USDWrite`. Assert the exported mesh's point count, one edited point's position, and `normals`. Also `USDToGeo`(unpacked) of the same export equals the pre-bridge detail after defragment. A second chain is the SOP Modify round trip: `USDRead` → `USDToGeo`(packed) → `GeoTransform` → `GeoToUSD`(Scene input connected) → `USDWrite`. Assert the source prims moved at their original paths and the export has no extra prims. A third is scatter-instancing: `GeoScatter` → `GeoCopyToPoints`(Pack and Instance) → `GeoToUSD` → `USDWrite`, asserting one `PointInstancer` with the scattered count. A fourth deletes an edge group with `GeoDelete` and asserts the exported `UsdGeomBasisCurves` count.
  - verify: test green in `build-and-test`.
  - size: M

- [ ] M361.P2.T2 — Scale bench on the GPU host
  - files: `tools/bench/geo_scale.py` (or the existing bench harness's equivalent), `Tests/GeoBench_Test.cpp` (bench-only, not in ctest)
  - approach: build a 10M-point grid and time `GeoTransform` → `GeoEdit`(5 points) → `GeoDelete`(half) → `GeoToUSD`, plus `geoEncodePattern` and Expand over a 1M-element selection, before and after, in one command on the GPU host (benches run together there). Record wall time and peak memory, with no thresholds.
  - verify: the bench runs to completion on the GPU host and its numbers are recorded in the member's PR body.
  - size: M

**Verification gate:** CI green including M361.P2.T1 and the P3 unit tests; the remote GUI run (explicit `--gui`) passes `m361_geo_pick.py`, `m361_geo_selection.py`, `m361_geo_tab_create.py` and `m361_geo_reselect.py`; on the Feature branch build under Xvfb, point, edge and primitive picking, Tab-create, reselect and `GeoEdit` drag work with one undo entry each, in the geometry context only, and a downstream `GeoToUSD` offers prim picking only; the M361.P2.T2 bench numbers are recorded in the PR body; the pre-existing ctest suite is green.

## Decisions

- 2026-11-08 — Split out of M54 - SOPs-Style Geometry (user, /cat-plan); tasks renumbered from M54.P4.*, M54.P5.* and M54.P1.T8. Waits on F4 - Heavy Scene Viewer landing (user, M54 grill). GeoEdit keys on global point number (user, M54 grill).
- 2026-11-13 — Pick modes are points, edges and primitives (user, grill): every mode Storm picks natively (`pickPoints`, `pickEdges`, `pickFaces`); vertex picking needs a face hit plus nearest corner and is left for later.
- 2026-11-13 — Selection reaches nodes the SOP way, both paths (user, grill): select then Tab-create a node with its group filled, and "Reselect for Current Tool" (backtick, Enter) to rewrite an existing node's group. Picking alone writes nothing. Houdini behaviour checked against SideFX forum threads, since the official SOP docs were unreachable and aren't in `.local/docs`.
- 2026-11-13 — Class mismatch (user, grill): operators where edges make sense gain them (`GeoTransform`, `GeoDisplace`, `GeoDelete`), with a Houdini-style Group Type knob defaulting to "Guess from Group"; the rest convert an edge selection to their accepted class. `GeoWrangle` gets no edge run-over (no Houdini equivalent).
- 2026-11-13 — Selections and patterns stay element numbers across upstream topology changes, as in SOPs (user, grill); only `GeoEdit` keeps its count-change warning.
- 2026-11-13 — Grow/shrink/convert live in the Viewer and as `GeoGroup` Expand and Promote modes (user, grill), so procedural graphs can do it too.
- 2026-11-13 — Switching pick mode converts the live selection, points → primitives keeping only fully covered ones (user, grill).
- 2026-11-13 — Stays one milestone rather than splitting the engine work into a separate F15 - Native Geometry member (user, grill).
- 2026-11-13 — Confirmed (user, grill): Tab-create inserts the node after the one feeding the Viewer's active input and points the Viewer at it; pick-mode buttons sit in the 3D toolbar with shortcut-editor defaults that avoid existing Viewer keys; `GeoEdit` with an edge selection drags the edges' points.
