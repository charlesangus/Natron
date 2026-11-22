# M362 - Geometry Operators II

The second batch of SOP-style operators for native geometry, cut from F15 - Native Geometry during the M54 - SOPs-Style Geometry grill (2026-11-08): `GeoPolyExtrude`, `GeoBoolean`, `GeoFuse`, `GeoAttribCreate`, `GeoAttribPromote`, plus a dict attribute type that stores, survives operators and bridges to USD. Each node follows the matching SOP (`PLAN/DECISIONS/2026-11-08-native-geometry-follows-sops.md`) and the operator conventions M359 - Geometry Operators fixed: group patterns through `geoSelectElements` (`Engine/Geo/GeoOpUtils.h`), attribute promotion through `geoPromotedDesc`/`geoConformAttribute`, interpolation through `Engine/Nodes/Geometry/GeoInterpolate.h`, tombstoned deletes through `GeoDetailBuilder::deletePoints`/`deletePrimitives` (`Engine/GeoDetail.h`). M360 - Geometry Kernel Language's wrangle already creates attributes on write (`GeoKernelBinding.cpp:688`), but it has no promotion, no reducers and no dict type, so all five nodes are still needed.

Member of F21 - Geometry Operators: branches off and merges into `feature/f21-geometry-operators` (PLAN-FORMAT §9b); the Feature's UAT covers it. Epic: E9 - USD And 3D. All nodes are `NativeEffectBase` subclasses in `Engine/Nodes/Geometry/`, each with its own `.reg.inc` (`NATRON_NATIVE_NODE(...)`, as `GeoNormals.reg.inc`), pxr-free, picked up by the `Nodes/*` glob (`Engine/CMakeLists.txt:43`); tests are `Tests/Geo*_Test.cpp`, listed in `Tests/CMakeLists.txt`. Knob script names are camelCase and labels are the readable script name. Docs and tooltips are written fresh and never name the reference application.

## Scope

**In:**
- Dict attributes: `eGeoStorageDict` on every class, scalar and array, holding nested string-keyed values (int, float, string, int/float/string arrays, dict, dict array), with JSON in and out. They survive every F15 operator, pass through the kernel wrangle, show up in the Python wrangle and the spreadsheet, and bridge to USD.
- `GeoAttribCreate`: Group (""), Group Type (Guess from Group), Encode Invalid Attribute Names (off), Number of Attributes (1). Per entry: Enable (on), Name ("attribute1"), Existing Name (Use the better type, size and precision; also error, warning, replace), Class (Point; also Vertex, Primitive, Detail), Type (Float; also Integer, Vector, String, Float Array, Integer Array, String Array, Dictionary, Dictionary Array), Type Qualifier (Guess from name; also None, Position, Vector, Normal, Color, Quaternion, Transform Matrix, Texture Coordinate), Precision (32-bit; 64-bit), Size (1), Default (0 0 0 0), Write Values (on), Value (0 0 0 0), String (""), Dictionary (`{}`, JSON).
- `GeoAttribPromote`: Original Name (""), Original Class (Point), New Class (Primitive), Piece Attribute (off, "name"), Promotion Method (Maximum; also Minimum, Average, Mode, Median, Sum, Sum of Squares, Root Mean Square, First Match, Last Match, Array of All, Unique Values), Index Attribute (off, "index"), Change New Name (off), New Name (""), Delete Original (on).
- `GeoFuse` (inputs: geometry, optional target geometry):
  - Group (""), Target Group (off, ""), Modify Target (off), Position Attribute ("P"), Snap To (Near Points; also Grid, Specified Points).
  - Near points: Using (Least Target Point Number; also Closest Target Point), Snap Distance (0.001), Output Positions (Average Value; also Least/Greatest Point Number, Maximum, Minimum, Mode, Median, Sum, Sum of Squares, Root Mean Square, Weighted Average, Weighted Sum, Minimum Weight, Maximum Weight), Weight Attribute ("weight"), Radius Attribute (off, "pscale"), Match Attribute (off, "name"), Match Condition (Equal Attribute Values; also Unequal), Match Tolerance (0).
  - Grid: Grid Type (Grid Spacing; also Grid Lines, Power of 2 Grid Lines), Grid Spacing (0.1 0.1 0.1), Grid Lines (10 10 10), Grid Power 2 (3 3 3), Grid Offset (0 0 0), Grid Rounding (Nearest; also Down, Up), Grid Tolerance (10).
  - Specified points: Target Points Attribute ("snap_to"), Target Class (Point).
  - Fuse Snapped Points (on), Keep Fused Points (off), Remove Repeated Vertices and Degenerate Primitives (on), Remove Unused Points from Degenerate Primitives (on), Remove All Unused Points (off), Recompute Affected Normals (on), Snapped Points Group (off, "snapped_points"), Snapped Destination Attribute (off, "snapped_to").
  - Attributes to Snap (count 0; per entry: Output Values (Average Value; the Output Positions list plus Concatenate and Concatenate in Weight Order), Point Attributes (""), Weight Attribute ("weight")).
  - Groups to Snap (count 0; per entry: Group Propagation (Union; also Least/Greatest Point Number, Intersect, Most Common), Point Group Names ("")).
- `GeoPolyExtrude` (inputs: geometry, optional spine curve):
  - Main: Group (""), Divide Into (Connected Components; also Individual Elements), Split Group (off, ""), Extrusion Mode (Primitive/Edge Normal; also Point Normal), Distance (0), Inset (0), Twist (0), Divisions (1), Spine Shape (Straight; also Curved, Curve From Second Input).
  - Front transform: Transform Extruded Front (off), Transform Space (Local; also Global), Transform Order (Scale Rot Trans), Rotate Order (Rx Ry Rz), Translate (0 0 0), Rotate (0 0 0), Scale (1 1 1), Shear (0 0 0), Pivot Translate (0 0 0), Pivot Rotation (0 0 0), Pre-Transform Order (Scale Rot Trans), Pre-Rotate Order (Rx Ry Rz), Pre-Translate (0 0 0), Pre-Rotate (0 0 0), Pre-Scale (1 1 1), Pre-Shear (0 0 0).
  - Outputs and groups: Output Front (on), Front Group (off, "extrudeFront"), Output Back (off), Back Group (off, "extrudeBack"), Output Side (on), Side Group (off, "extrudeSide"), Front Boundary Group (off, "extrudeFrontSeam"), Back Boundary Group (off, "extrudeBackSeam"), Preserve Groups (on), Copy Edge Groups (off), Copy Point Groups (off), Copy Primitive Groups (off).
  - Insetting, normals, UVs: Limit Insetting (on), Use Common Limit (on), Add Vertex Normals (off), Cusp Angle (60), Cusp All Front Boundary Edges (on), Cusp All Back Boundary Edges (on), Generate Unwrapped Texture Coordinates for Sides (on), Style (Rectangular; also Unit Square, Proportional), Scaling (Match 3D Scale; also Fit Unit Square, Match Incident UV Scale).
  - Spine: Front Magnitude (1), Back Magnitude (1), Front Stiffness (0), Back Stiffness (0), Spacing (Uniform; also Curvature Sensitive), Reverse Direction (off), Axial Rotation (0), Front Blend (0.1), Back Blend (0.1).
  - Thickness and twist: Thickness Scale (1), Thickness Attribute (on, "thickness"), Thickness Ramp (on, flat 0.5), Range Half-Width (180), Twist Attribute (on, "twist"), Local Twist Ramp (on, flat 0.5).
  - Local attributes: Distance Scale (off, "zscale"), Inset Scale (off, "insetscale"), Twist Scale (off, "twistscale"), Divisions Scale (off, "divsscale"), X Direction (off, "localx"), Z Direction (off, "localz"), Center (off, "localctr").
- `GeoBoolean` (inputs: A, optional B):
  - Set A Group (""), Treat As (Solid; also Surface), Resolve Self-Intersections (on); the same three for Set B.
  - Operation (Union; also Intersect, Subtract, Shatter, Custom, Seam, Detect, Resolve), Subtract (A minus B; also B minus A, Both), Shatter (A Pieces; also B Pieces, Both).
  - Detriangulate (All Polygons; also Only Unchanged Polygons, No Polygons), Assume Seam Polygons Are Flat (on), Unique Points Along Seams (off), Collapse Tiny Seam-Adjacent Edges (on), Edge Length Threshold (1e-5).
  - Custom: A Depth Min/Max (1, 1000), B Depth Min/Max (1, 1000), Output Pieces Matching (Both; also A, B, Exactly One), Merge Adjacent Pieces (on).
  - Seam: Generate A-A Seams (on), Generate B-B Seams (on), Generate A-B Seams (on).
  - Detect: AxA Polygons (on, "axapolys"), AxB Polygons (on, "axbpolys"), AxA List (off, "axalist"), AxB List (off, "axblist").
  - Output groups: A Inside B, A Outside B, B Inside A, B Outside A (each off, named likewise); A-A, B-B and A-B seam edge groups (off, "aaseams"/"bbseams"/"abseams").
- A params→knob audit of all five nodes.

**Out:**
- Dict types in the kernel language (`d@`, dict functions): kernels refuse to read a dict, and dicts they don't touch pass through unchanged. Pending the user (Open questions).
- Attribute Create's per-element local variables (Local Variable, Allow Local Vars) and Save to Info Block: dropped with reasons, pending the user (Open questions); per-element values go through `GeoWrangle`.
- 8-bit and 16-bit precision: the detail stores int32/64 and float32/64 only (M54 decision, 2026-11-08).
- `.bgeo` dict interchange (M403 - Geometry File Cache refused `.bgeo`).

**Edge cases:**
- Tombstoned inputs: every node keeps holes and iterates in dense number order (M54 - Native Geometry Data Model's rule).
- Invalid JSON in a Dictionary knob: node error giving the character offset.
- Dict attributes at every storage switch site: a kernel snippet that reads one fails to compile naming the attribute; dict merged against a non-dict of the same name follows `GeoMerge`'s incompatible-type rule; interpolation (Subdivide, PolyExtrude, Boolean) takes the highest-weight source, as strings do.
- AttribPromote with New Class equal to Original Class and no piece attribute: warning, input unchanged. Vertex → point under the same name always deletes the original.
- AttribCreate on an existing attribute follows each Existing Name mode. Encoding uses the `xn__` scheme (`.local/docs/houdini/expressions/encode.md`).
- Fuse with Snap Distance 0 fuses only coincident points. With the second input connected, Modify Target is ignored. Results don't depend on thread count.
- PolyExtrude on non-planar n-gons uses the Newell normal. A missing Group errors with its name. Inset past the limit is clamped when Limit Insetting is on.
- Boolean with a non-closed or inconsistently wound solid: warning listing the problem primitives as a group pattern. Non-polygon primitives pass through with an info message. An empty B: union and subtract pass A, intersect gives an empty detail. One input with blank groups runs against itself.

**Assumptions:**
- Unconfirmed: the defaults above are recalled; the local reference pages list parameters but no defaults. M362.P6.T1 confirms them.
- Unconfirmed: Boolean's exact core is written in-house, extending M414 - Geometry Advanced Modeling's in-house choice (user, 2026-11-12). Pending the user (Open questions).
- Unconfirmed: native nodes can create and remove numbered knob blocks at runtime and restore them on project load before values deserialise. No repeated-knob support exists today, so M362.P2.T1 builds it.
- F21 stacks on F15 - Native Geometry (`feature/f15-native-geometry` merged into `feature/f21-geometry-operators`) until F15 lands on main.

## Phase 362.1: Dict attributes

- [ ] M362.P1.T1 — Dict storage, the `GeoDict` value and JSON
  - files: `Engine/Geo/GeoAttribute.{h,cpp}`, `Engine/Geo/GeoDict.{h,cpp}`, `Engine/Geo/GeoDictJson.{h,cpp}`, `Engine/GeoDetail.cpp` (content hash), `Tests/GeoDict_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: add `eGeoStorageDict` (`GeoAttribute.h:51`). Mirror `GeoStringTable` (`GeoAttribute.h:105`) with a per-attribute `GeoDictTable`: elements store an int32 index, index 0 is the empty dict, values interned by canonical hash. `GeoDict` is an immutable sorted map whose values are int64, double, string, int/float/string arrays, dict or dict array, shared by `shared_ptr<const>`. Dict arrays reuse the array pool, as string arrays do. A small hand-written RFC 8259 reader/writer (no Qt, no pxr) keeps ints and floats distinct. Fold dict content into `contentHash()`.
  - verify: `GeoDict_Test`: JSON round-trips nested values and keeps int vs float; equal dicts intern to one index; setting a dict detaches only its page; content hash changes with a dict value; a dict array round-trips.
  - size: L
- [ ] M362.P1.T2 — Dicts survive operators
  - files: `Engine/Geo/GeoOpUtils.cpp`, `Engine/Nodes/Geometry/GeoInterpolate.cpp`, `Engine/Nodes/Geometry/GeoMerge.cpp`, `Engine/Geo/GeoPacked.cpp`, `Engine/Geo/GeoKernelBinding.cpp`, the spreadsheet source from M337 - Geometry Spreadsheet, `Engine/Geo/GeoFileFormat.cpp` (only if M403 - Geometry File Cache has landed), `Tests/GeoDictOps_Test.cpp`
  - approach: add the dict case at every `eGeoStorageString` switch site (`git grep eGeoStorageString`). Copy and default-fill by index. Merge remaps one input's table into the other's. Interpolation takes the highest-weight source. Defragmenting keeps the tables. The kernel binding refuses dict reads with a named error and passes untouched dicts. The spreadsheet shows a compact JSON cell with the full value in the tooltip. `GeoFileFormat` gets a dict chunk if M403 has landed; otherwise record a follow-up in `## Decisions` for whichever lands second.
  - verify: `GeoDictOps_Test`: a primitive dict survives Merge (two tables), Delete plus defragment, Subdivide, Copy to Points and Transform unchanged; an untouching wrangle keeps it shared; a wrangle reading it errors with its name; the spreadsheet cell text is the JSON.
  - size: M
- [ ] M362.P1.T3 — Dict bridge to and from USD
  - files: `Engine/Nodes/Geometry/GeoUsdConvert.cpp`, `Engine/Nodes/Geometry/GeoSdfData.cpp`, `Engine/Nodes/Geometry/USDToGeo.cpp`, `Tests/Scene/GeoUsdConvert_Test.cpp`, `Tests/Scene/SceneToGeo_Test.cpp`
  - approach: a detail dict, or a primitive dict uniform over a `path` partition, becomes `customData[<name>]` (`VtDictionary`) on that prim. Varying primitive, point and vertex dicts become JSON string primvars tagged with primvar customData `natron:dict = true`. `USDToGeo` turns tagged primvars and the bridge's customData keys back into dict attributes. Untagged strings stay strings.
  - verify: `GeoUsdConvert_Test`: detail and uniform dicts land in customData, varying ones as tagged JSON primvars; `SceneToGeo_Test`: geo → USD → geo round-trips every dict attribute with equal content hashes.
  - size: M
- [ ] M362.P1.T4 — Dicts in the Python wrangle
  - files: `Engine/Nodes/Geometry/GeoPyBuffer.{h,cpp}`, `Engine/Nodes/Geometry/GeoPython.cpp`, `Tests/GeoPython_Test.cpp`
  - approach: dict attributes appear as Python lists of `dict` (lists of lists for arrays), converted on first access and scattered back only when assigned. Assigning a list of dicts to a new name creates a dict attribute. An unconvertible value is a node error with the traceback.
  - verify: `GeoPython_Test`: a script reads a primitive dict, adds a key and writes it back, and the result carries the key; an unwritten dict stays shared; assigning a set reports an error on the node.
  - size: M

## Phase 362.2: Attribute nodes

- [ ] M362.P2.T1 — Repeated knob blocks for native nodes
  - files: `Engine/Nodes/NativeKnobRepeat.{h,cpp}`, `Tests/NativeKnobRepeat_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: a helper owning a count knob and a block template that creates or removes numbered plugin-declared knobs (`name1`, `name2`, …) inside a group when the count changes. On project load it recreates the blocks from the serialised count before values deserialise. Undo restores removed blocks with their values. Not the user-knob path (the user can't delete these). If it needs `Engine/Knob*` edits, the gate's GUI bench rule applies.
  - verify: `NativeKnobRepeat_Test`: count 3 → 1 → 3 keeps block 1's values and defaults blocks 2–3; a project saved with 3 blocks and animated values in block 2 reloads with them; undo of a count change restores values.
  - size: L
- [ ] M362.P2.T2 — `GeoAttribCreate`
  - files: `Engine/Nodes/Geometry/GeoAttribCreate.{h,cpp,reg.inc}`, `Engine/Geo/GeoAttribute.h` (add `eGeoTypeInfoTextureCoord`), `Engine/Geo/GeoOpUtils.{h,cpp}` (`geoEncodeAttribName`/`geoDecodeAttribName`), `Engine/Nodes/Geometry/GeoUsdConvert.cpp` (texcoord role), `Tests/GeoAttribCreate_Test.cpp`
  - approach: every Scope control, entries on M362.P2.T1's helper. Each entry is evaluated against the input, not the previous entries. The four Existing Name modes go through `geoPromotedDesc`. Writing honours Group and Group Type through `geoSelectElements`. Dictionary types parse the JSON knob through `GeoDictJson`. Arrays get no values. Only written pages detach.
  - verify: `GeoAttribCreate_Test`: float vector `Cd` on a point group writes only the group; Guess from name gives `N` the Normal qualifier; each Existing Name mode on a clashing int attribute; a Dictionary entry writes parsed JSON; an encoded name decodes back; two entries both read input values.
  - size: M
- [ ] M362.P2.T3 — `GeoAttribPromote` and the shared reducer
  - files: `Engine/Nodes/Geometry/GeoAttribPromote.{h,cpp,reg.inc}`, `Engine/Geo/GeoAttribReduce.{h,cpp}`, `Tests/GeoAttribPromote_Test.cpp`
  - approach: put all Fuse/Promote reduce methods in `GeoAttribReduce`: the 12 promotion methods plus weighted average/sum, min/max weight, concatenate and concatenate-by-weight; dicts merge keys. Promote gathers source elements per destination through `reverseTopology()`, or per piece over the union of each piece's elements, deduplicated in index order. Glob renaming (`instance*` → `my*instance`). Index Attribute records the winning element. Delete Original applies, forced for vertex → point with the same name.
  - verify: `GeoAttribPromote_Test`: point → primitive Average on a grid matches hand values; every method on one crafted set; Array of All and Unique Values give arrays; a piece attribute averages per `name`; the index attribute names the max point; the glob rename; the forced delete.
  - size: M

## Phase 362.3: Fuse

- [ ] M362.P3.T1 — `GeoFuse` near-point snapping and consolidation
  - files: `Engine/Nodes/Geometry/GeoFuse.{h,cpp,reg.inc}`, `Engine/Geo/GeoPointTree.{h,cpp}` (radius queries if missing), `Tests/GeoFuse_Test.cpp`
  - approach: Near Points mode with every Scope control except Grid, Specified Points and the two count blocks. Query a target point tree (`GeoPointTree::within`). Least Target Point Number is a deterministic union over ascending numbers. Closest Target Point, radius and match filters included. Output positions through `GeoAttribReduce`. Consolidation rewires vertices to the surviving point; then degenerate removal and the unused-point options through `GeoDetailBuilder` deletes, `GeoNormals`' helper for affected normals, the snapped group and destination attribute.
  - verify: `GeoFuse_Test`: two coincident-edge grids fuse with shared points; distance 0 fuses only exact duplicates; Closest vs Least differ on a crafted case; a match attribute stops cross-piece fusing; degenerate triangles removed; a 1M-point jittered cloud gives identical output at 1 and 8 threads.
  - size: L
- [ ] M362.P3.T2 — `GeoFuse` grid and specified-point modes, attribute and group snapping
  - files: `Engine/Nodes/Geometry/GeoFuse.{h,cpp}`, `Tests/GeoFuse_Test.cpp`
  - approach: Grid mode with the three grid types, offset, rounding and tolerance. Specified Points uses the target attribute and class. Attributes to Snap and Groups to Snap blocks on M362.P2.T1's helper, values through `GeoAttribReduce`, Concatenate building arrays (and merged dicts). All five group propagation rules.
  - verify: `GeoFuse_Test`: grid spacing 0.5 snaps to multiples; power-of-2 lines match; rounding Down/Up; specified points follows `snap_to`; `Cd` averages on fused points; Concatenate makes a float array; group Intersect vs Union.
  - size: M

## Phase 362.4: PolyExtrude

- [ ] M362.P4.T1 — `GeoPolyExtrude` face extrusion core
  - files: `Engine/Nodes/Geometry/GeoPolyExtrude.{h,cpp,reg.inc}`, `Engine/Geo/GeoExtrudeFronts.{h,cpp}`, `Tests/GeoPolyExtrude_Test.cpp`
  - approach: fronts from the primitive group: connected components over shared edges (`geoForEachEdge`), the split group, or individual elements. Newell normals per face, averaged per front. Distance, Inset (Limit Insetting, Use Common Limit), Twist and Divisions on a straight spine. Front, back and side output with all eight groups plus Preserve and the three Copy options. Point and vertex attributes interpolated along the sides (`GeoInterpolate`), primitive attributes copied. Every Scope knob created now; later tasks wire the rest.
  - verify: `GeoPolyExtrude_Test`: one box face at distance 1 gives 10 faces and correct bounds; connected vs individual on a 2×2 grid; inset limit stops at a point; side and front groups hold the right prims; `uv` and a primitive group carry over.
  - size: L
- [ ] M362.P4.T2 — Edge extrusion, point-normal mode and the front transform
  - files: `Engine/Nodes/Geometry/GeoPolyExtrude.cpp`, `Engine/Geo/GeoExtrudeFronts.cpp`, `Tests/GeoPolyExtrude_Test.cpp`
  - approach: edge groups extrude along averaged adjacent face normals (interior) or the in-surface outward direction (boundary). Point Normal mode reads point `N`, computing it if absent. The front transform uses both TRS stacks with orders, pivots, Local/Global space and shear (`geoComposeTransform` extended with shear), applied on top of Distance and Twist.
  - verify: a grid's boundary edge extrudes in-plane; an interior edge extrudes along +Z; point-normal mode on a sphere patch; translate (0,0,1) with distance 0 equals distance 1; Global vs Local differ on two tilted faces.
  - size: L
- [ ] M362.P4.T3 — Curved and external spines, ramps and local attributes
  - files: `Engine/Nodes/Geometry/GeoPolyExtrude.cpp`, `Engine/Geo/GeoExtrudeSpine.{h,cpp}`, `Tests/GeoPolyExtrude_Test.cpp`
  - approach: Curved spine is a cubic Hermite between back and front frames with magnitude and stiffness. Curve From Second Input samples the input polyline, with reverse, axial rotation and the two blends. Curvature-sensitive spacing by curvature-weighted arc length. Thickness and twist ramps via `KnobParametric` (as `Engine/Nodes/Color/ColorCorrect.cpp`), plus the curve attributes and scales. The seven local attributes scale or override per front.
  - verify: a curved spine with stiffness 0 passes the expected midpoint; a quarter-arc spine input bends the extrusion onto it; a thickness ramp at 0.25 halves the mid-section; a `zscale` of 2 doubles one face's distance.
  - size: L
- [ ] M362.P4.T4 — Extrusion normals and side UVs
  - files: `Engine/Nodes/Geometry/GeoPolyExtrude.cpp`, `Tests/GeoPolyExtrude_Test.cpp`
  - approach: Add Vertex Normals with Cusp Angle and the boundary cusp toggles, through `GeoNormals`' helper. Side UVs only when `uv` exists on the input, in the three styles and three scalings.
  - verify: a box face extrusion with cusp 60 keeps sides flat-shaded; Unit Square side UVs span 0–1; Match 3D Scale UV lengths equal 3D edge lengths.
  - size: M

## Phase 362.5: Boolean

- [ ] M362.P5.T1 — Exact mesh arrangement core
  - files: `Engine/Geo/GeoMeshArrangement.{h,cpp}`, `libs/predicates/` (vendored exact orientation predicates, public domain), `libs/CMakeLists.txt`, `Tests/GeoMeshArrangement_Test.cpp`
  - approach: triangulate input polygons, keeping source primitive and corner weights per triangle. Triangle pairs through a BVH (brute force test-only). Intersections are implicit points evaluated with filtered exact predicates. Each intersected triangle is re-triangulated with constraints (`libs/CDT`, already vendored). Output is a conforming triangle soup with source tags, seam edges and cell adjacency; no floats rounded until output, where near-coincident points are snapped and recorded.
  - verify: `GeoMeshArrangement_Test`: two overlapping cubes give a watertight soup with a closed seam loop; coplanar overlapping faces resolve without slivers; a fixed-seed random-triangle fuzz never yields crossing output triangles (exact predicates).
  - size: L
- [ ] M362.P5.T2 — `GeoBoolean` solid and surface operations
  - files: `Engine/Nodes/Geometry/GeoBoolean.{h,cpp,reg.inc}`, `Engine/Geo/GeoWindingLabels.{h,cpp}`, `Tests/GeoBoolean_Test.cpp`
  - approach: label cells with A and B winding depths by propagation across the arrangement. Union, Intersect, Subtract (three), Shatter (three) and Custom (depth ranges, Output Pieces Matching, Merge Adjacent Pieces) select by depth. Surface sets are cut, not filled. Self-intersections resolved when toggled or required. Point and vertex attributes interpolated from source corner weights, primitive attributes copied. The four inside/outside groups, Detriangulate (three modes), the non-closed/winding warning with a group pattern, all Scope knobs.
  - verify: `GeoBoolean_Test`: union of two unit cubes offset 0.5 has volume 1.5 (divergence theorem) and is closed; intersect 0.5; A−B 0.5; shatter both gives 3 closed pieces; surface grid ∩ solid cube is cut along the cube; `uv` interpolated on cut faces; Custom on three nested boxes keeps only the innermost.
  - size: L
- [ ] M362.P5.T3 — Seam, Detect and Resolve modes and seam cleanup
  - files: `Engine/Nodes/Geometry/GeoBoolean.cpp`, `Tests/GeoBoolean_Test.cpp`
  - approach: Seam outputs polylines for A-A, B-B and A-B. Detect passes A through with the AxA/AxB groups and list attributes. Resolve outputs the raw arrangement. Cleanup: Assume Seam Polygons Are Flat drops inline points on detriangulated faces; Unique Points Along Seams splits points; Collapse Tiny Seam-Adjacent Edges fuses edges up to the threshold. The three seam edge groups.
  - verify: the cube-pair seam is one closed polyline of the expected length; Detect lists the right B primitives; Unique Points doubles the seam points; nearly aligned cubes (1e-7 offset) leave no edge shorter than the threshold.
  - size: M

## Phase 362.6: Audit and proof

- [ ] M362.P6.T1 — Params→knob audit
  - files: `Engine/Nodes/Geometry/Geo{PolyExtrude,Boolean,Fuse,AttribCreate,AttribPromote}.cpp` (read), `.local/docs/houdini/nodes/sop/{polyextrude,boolean,fuse,attribcreate,attribpromote}.md` (read)
  - approach: list every reference parameter per node and match it to a knob, label and default. Confirm defaults against the source the user names (Open questions). Report present / missing / dropped-with-reason in a table for the PR body only; never commit it or name the reference application in the repo.
  - verify: the table covers every reference parameter of the five nodes; each missing knob or wrong default is fixed here or listed as a follow-up task.
  - size: M
- [ ] M362.P6.T2 — Docs and GUI check
  - files: `Engine/Nodes/Geometry/docs/{GeoPolyExtrude,GeoBoolean,GeoFuse,GeoAttribCreate,GeoAttribPromote}.md`, `Tests/gui/m362_geometry_operators_ii.py`
  - approach: docs written fresh. The GUI script builds Box → PolyExtrude → Boolean (subtract a sphere) → Fuse → AttribCreate (a dict) → AttribPromote → `GeoToUSD`, checks the viewer shows the result and that the count blocks add and remove controls, and takes screenshots for the Feature's UAT.
  - verify: the script passes under Xvfb; screenshots sent one per file.
  - size: M

**Verification gate:** full ctest green including `GeoDict_Test`, `GeoDictOps_Test`, `NativeKnobRepeat_Test`, `GeoAttribCreate_Test`, `GeoAttribPromote_Test`, `GeoFuse_Test`, `GeoPolyExtrude_Test`, `GeoMeshArrangement_Test`, `GeoBoolean_Test` and the extended `GeoPython_Test`/Scene bridge tests; `Tests/gui/m362_geometry_operators_ii.py` passes under Xvfb; the M362.P6.T1 audit shows no missing control; every node is pxr-free; the GUI bench rule (Context) if `Engine/Knob*` was touched.

## Decisions

- 2026-11-22 — Elaborated from the stub (consultant). Code findings: no mesh-boolean or JSON library is vendored (`libs/`); native nodes have no repeated-knob support; the wrangle creates attributes on write (`GeoKernelBinding.cpp:688`) but has no promote or dict support, so all five nodes stay.
- 2026-11-22 — Dict storage mirrors string storage (planner): an interned per-attribute table with int32 indices keeps pages copy-on-write and hashing cheap.
- 2026-11-22 — One reducer for AttribPromote and Fuse (planner): their method lists overlap almost entirely, and Concatenate needs dicts, so Phase 362.1 comes first.

## Open questions

1. Boolean core: in-house (exact predicates + `libs/CDT` + winding labels, three L tasks, robustness risk, no dependency), libigl + CGAL (robust, every operation, but CGAL headers + GMP/MPFR in the image and AppImage, slow compiles), or Manifold (Apache-2.0, fast, solids only — no Surface/Custom/Seam/Detect/Resolve, so it fails "every control")? Plan assumes in-house.
2. Authoritative defaults: the local pages have no default values. Read them from an install (parm-template dump), or let recalled values stand?
3. Attribute Create's Local Variable, Allow Local Vars and Save to Info Block: drop (plan), or evaluate Value per element as kernel-language expressions and/or add a header block to M403's format?
4. Kernel `d@` dict support: Out (plan), or in this milestone?
5. Repeated knob blocks (M362.P2.T1): keep here, or file an E14 - Architecture stub?
