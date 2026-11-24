# M575 - USD Restructure And Graft

Two new Edit nodes for the scene's namespace. **USDRestructure** moves, renames and duplicates the prims its Prim Pattern matches, and removes composition arcs from them. **USDGraft** grafts chosen prims of its Source input under chosen paths of its Main input. Both build on the link-following code that USDXformPrim's Wrap mode already uses (`Engine/Nodes/Scene/SceneReparentNode.cpp`). `renderWrap()` defines the new path with an internal reference to the old one, deactivates the old prim, and calls `gatherWrapRetargets()` (anonymous namespace, ~:116) to author explicit target lists for the relationships and connections that point into or out of the moved prim. Graft mode goes through `SceneOps::reparentRoots()` (`Engine/Nodes/Scene/SceneOps.h` ~:249), which references every input root through `SceneOps::createStackLayer()`, moves cross-root links (`SceneRootGraft`, `SceneUnmappedLink`), and maps load sets and reach with `mapGraftLoadSet`/`mapGraftReach`/`mapWrapLoadSet`/`mapWrapReach`. Today all of this handles exactly one wrapped prim or whole input roots. This milestone generalizes it to lists of (source, target) moves and grafts of any prim. Where a move is valid as a USD relocate, it is authored as one (`SdfLayer::SetRelocates`; OpenUSD ≥ 24.11 per `cmake/NatronUsd.cmake`). The viewer's upstream lookups (`findCameraNode` in `Gui/ViewerTab.cpp` ~:367, and the transform-target walk in `Gui/SceneTransformTarget.cpp` ~:540) `dynamic_cast` to `SceneReparentNode` to follow moved prims. They move to an interface the new nodes also implement.

## Scope

**In:**
- Shared SceneOps core: retargeting and authoring for a list of moves, load-set and reach mapping for a list of moves, grafting arbitrary source prims (not just roots) with link following, and relocate validity checks and authoring. Wrap mode and `reparentRoots` are rewritten on top of it, with no behaviour change.
- A path-mapping interface (output path → input and input path) that SceneReparentNode, USDRestructure and USDGraft implement. The viewer's camera lookup and transform-target walk use it, so a renamed or moved camera or prim is still found upstream.
- **USDRestructure** (Edit submenu). Operations Reparent, Rename, Duplicate, Remove Arcs. Method Auto/Relocate/Reference for Reparent and Rename. Creating the node from a scene selection fills Prim Pattern.
- **USDGraft** (Edit submenu). Inputs Main (0) and Source (1). Shared placement knobs and per-row Enable / Source Path / Destination Path / Keep Position / Keep Material, with row add/remove through the reusable knob row list (`Engine/Nodes/KnobRowList.{h,cpp}`) that M567 - USD Light Linking ships. Also Frame Offset Mode and Frame Offset.
- `docs/USDRestructure.md` and `docs/USDGraft.md`, gtests, an Xvfb GUI script, and a params→knob audit.
- Control audit for **Solaris Restructure Scene Graph**. Every control is listed; knob script names are in parentheses.
  - Flatten Node Input Layers: **Out**. Natron never flattens. Every method here authors over the composed input, so it works across layers and nodes without it.
  - Primitives → Prim Pattern (`primPattern`, a `PrimSelector`): **In**.
  - Operation (`operation`): **In**. Reparent Primitives → Reparent, and Rename Primitives → Rename. Remove Primitives: **Out**, because USDPrune's Deactivate does this. Rename Variant Set and Remove Variant Set: **Out**. A variant set's specs live in upstream layers and other files. A stronger layer can't rename them, so this would be destructive editing of source layers, which Natron's non-destructive node layers don't do. No milestone (user, 2026-11-24). Remove References → Remove Arcs: **In**. Duplicate (from the user's "rename/move/duplicate"; the Duplicate LOP audit is below): **In**.
  - New Parent Prim (`newParent`): **In**.
  - Create Parent Prims (`createParents`), Primitive Type (`parentType`), Only Define Missing Prims (`onlyDefineMissing`): **In**.
  - Primitive Specifier (`parentSpecifier`): **In** for Define and Over. Class is **Out**: an abstract parent would make every moved prim abstract and remove it from the stage. No milestone.
  - Old Name / New Name (`oldName`, `newName`) with `@name`, `@index` and `*` rules: **In**.
  - Remove Empty Variants: **Out**. It only applies to Remove Primitives, which is Out.
  - Old/New Variant Set Name and Variant Sets: **Out**, along with their operations.
  - References / Payloads / Inherits / Specializes toggles (`removeReferences`, `removePayloads`, `removeInherits`, `removeSpecializes`): **In**.
- Control audit for **Solaris Relocate**, which feeds the Method knob:
  - Handle Invalid Relocate (`invalidRelocate`: Do Nothing / Message / Warning / Error): **In**. It applies when Method is Relocate. Under Auto, an invalid relocate falls back to Reference and reports nothing.
  - Number of Relocates / Enable / Primitives / Target Path: **In**, as Prim Pattern plus New Parent or New Name. A single USDRestructure handles one pattern. For several relocations, chain several nodes; multi-row relocates are **Out**, no milestone (user, 2026-11-24).
  - Locals `@primname`, `@primpath`, `@primdir`, `@prim`, `@numprim`: `@name` and `@index` are **In** (Rename). The others are **Out**: a Natron string knob can take a Python expression instead.
  - Empty target path deletes: **Out** (USDPrune).
- Control audit for **Solaris Duplicate** (Duplicate operation):
  - Source Primitives = Prim Pattern: **In**.
  - Modify Source Primitive (`sourceMode`): all six options **In**: Deactivate, Hide, First Duplicate, Classes, Deactivated, Hidden.
  - Total Number (`totalNumber`), Separate Source and Destination Paths (`separateDestination`), Destination Primitives (`destination`): **In**.
  - Duplicate Name (`duplicateName`) with tokens `@copy`, `@numcopy`, `@src`, `@numsrc`, `@srcname`, `@srcpath`: **In**. Default `@srcname_@copy`, counting from 0.
  - Reference Type (`referenceType`): Reference, Inherits and Specializes are **In**. Copy is **Out**: it flattens the source into the node's layer, while arc-based copies already take per-copy overrides. No milestone.
  - Make Instanceable (`instanceable`), Parent Primitive Kind (`parentKind`, including Automatic), Parent Primitive Type (shared `parentType`): **In**.
  - Add As customData to Source Prims (`recordCustomData`), customData Name (`customDataName`): **In**.
  - Collection Primitive (`collectionPrim`), Collection Name (`collectionName`), Promote Collections (`promoteCollections`), Promote Source Collections (`promoteSourceCollections`): **In**.
  - Transform Cumulative (`cumulative`), Translate, Rotate, Scale, Uniform Scale and rotate order (`SceneTransformKnobs`), Pivot Translate (`pivot`): **In**. Shear, Pivot Rotate, Transform Order (TRS order) and Use XformCommonAPI are **Out**: they're not in Natron's transform knob set (USDXformPrim has none of them). No milestone.
- Control audit for **Solaris Graft Branches** (USDGraft):
  - Primitive Path (`primPath`), Primitive Kind (`kind`), Parent Primitive Type (`parentType`), Primitive Specifier (`parentSpecifier`: Define and Over; Class Out as above): **In**.
  - Make Destination Primitive Paths Unique (`makeUnique`, Auto Rename's `_<n>` scheme), Treat Destination Paths as Parents (`destinationAsParent`, default on): **In**.
  - Frame Offset Mode (`frameOffsetMode`: Match Start Times / Offset / Match And Offset, default Offset), Frame Offset (`frameOffset`, in frames): **In**.
  - Number of Source Prims (`sourceCount`), Enable (`enable<i>`), Source Prim Path (`sourcePath<i>`), Destination Prim Path (`destinationPath<i>`), Keep Position (`keepPosition<i>`), Keep Material (`keepMaterial<i>`): **In**.
- Control audit for **Solaris Graft Stages**: **Out**, because it is already covered. USDXformPrim and USDScopePrim in Graft mode graft every input's roots under Prim Path. Primitive Path maps to Prim Path, Primitive Kind to Kind, and Make Unique to Auto Rename. Parent Primitive Type is fixed by the node (Xform or Scope). Primitive Specifier, the per-input Destination Path table and Destination Path Default (`@input`) are not offered there. No milestone.
- Reference only, no port: Gaffer Rename's deletePrefix, deleteSuffix, find/replace and regular expressions are covered by `*` patterns plus a Python expression on New Name. Gaffer Duplicate's iterative transform is Cumulative. Gaffer Parent's filter-driven multi-parent is **Out** (no milestone).

**Out:**
- Scene Graph panel commands (Rename, drag to reparent) that create or edit a USDRestructure: M589 (user, 2026-11-24).
- Editing PointInstancer instances (M576 - Point Instance Extract And Prune).
- Undo beyond Natron's ordinary knob undo: none.

**Edge cases:**
- **Method Auto**: a move is authored as a relocate when it is valid. That means the source is neither `/` nor a root prim, no layer of the input payload (`SceneOps::getLayers`) holds a spec at or below the source, so the prim comes only through a reference or payload arc (a USDReference'd asset, for example), and the prim is neither an instance proxy nor a prototype. Otherwise the move uses the reference method: define at the target, internally reference the source, author `active = false` on the source and `active = true` on the copy, and retarget links. USDRead content is sublayered, so it always takes the reference method. The node docs say so.
- **Relocate method and links**: USD remaps targets authored across the arc. Properties in the local stack that point into a relocated prim still get explicit target lists from the shared retargeting.
- Method Relocate with an invalid move: the move is not authored, and `invalidRelocate` decides what happens (nothing, an info message, a warning, or an error that fails the action).
- Nested matches (a match under another match): moves apply outermost first. A nested match's source is its path after the outer move. For Reparent, a nested match under a reparented match moves with it and is not moved again.
- **Target collision**: a target path that already holds a prim (Reparent or Rename), or two matches mapping to one target, is a node error naming both paths. For Duplicate, names must be unique among siblings; a collision is an error.
- A New Parent under a moved source (a move into itself) is a node error.
- A missing New Parent with Create Parents off is a node error. With it on, the missing ancestors are defined (or overridden) as Parent Type with Parent Specifier. Only Define Missing authors `over` on parents that exist.
- Rename `*` patterns: Old Name `char_*_left` → New Name `char_*_right`. A name that doesn't match Old Name is left alone. An invalid resulting prim name is a node error naming it.
- Remove Arcs: an explicit empty list op in the node's layer clears arcs authored anywhere in the local stack. Arcs that come from across another reference or payload can't be removed from here, so a node warning names the prim and arc (from `SceneOps` `ScenePrimArc`, `introducingLayer` not in the local stack).
- Matches inside unloaded payloads aren't seen. The viewer never loads payloads for this, so the reach maps report the paths, and a later load re-evaluates the node.
- **USDGraft**: a Source Path with no prim in Source is a node warning, and that row contributes nothing. An empty Source input contributes no layer. An empty or disabled row contributes nothing.
- **USDGraft destination**: with Destination As Parent on, `/geo/sphere` → `<primPath>/<destinationPath>/sphere`; off, → `<primPath>/<destinationPath>`. When a destination already holds a Main prim and Make Unique is off, the two compose. Main's local opinions are stronger than the referenced graft (a reference is weaker than local opinions). The docs say so.
- **USDGraft links**: a link from a grafted prim to a prim another row of the same Source grafts follows the graft. A link into the project's Materials Scope carries that material to Main's Materials Scope, as `passThroughRoot` does. Any other outside link is dropped and named in a node warning (`SceneUnmappedLink`).
- **Keep Position**: authors `xformOp:transform:graftKeep` first in the grafted prim's op order, equal to inverse(destination parent world) × (source parent world). If either side might vary in time (`TransformMightBeTimeVarying`), it is sampled at each frame of the project range. A non-Xformable grafted prim gets a node warning, and its position is not kept.
- **Keep Material**: when the source prim's bound material (`ComputeBoundMaterial`) comes from an ancestor outside the graft, the grafted prim gets a direct binding. The material follows the link rules above.
- **Frame offsets**: they go on the graft reference's `SdfLayerOffset`, with frames converted through Main's time codes per second and the project fps. Match Start Times shifts Source's `startTimeCode` onto Main's. When Source has none authored, the earliest time sample on the grafted prims is used; when Main has none, the project's first frame. No samples at all means no shift.
- Prim Pattern or Prim Path that is not a valid pattern or absolute path: a node error, and the scene action fails.

**Assumptions:**
- Relocates in any layer of Natron's stack apply to the whole composed stack. A later graft (`createStackLayer` sublayers the stack) keeps them. P1.T4 tests both.
- USDWrite writes the relocates layer metadata and the internal references unchanged. When it flattens, the composed result is what ends up in the file.
- `Engine/Nodes/KnobRowList.{h,cpp}` (count knob, per-row column spec, Add After and Remove per row) has shipped with M567 - USD Light Linking, a dependency; USDGraft uses it unchanged.
- M520 - USD Toolbar Category And Submenus provides `PLUGIN_GROUP_USD_EDIT` (it is in progress). If it hasn't merged, the nodes register under `PLUGIN_GROUP_3D_USD`, and M520's sweep moves them.

## Phase 575.1: Shared move and graft core

- [ ] M575.P1.T1 — Retarget and author a list of moves in SceneOps
  - files: `Engine/Nodes/Scene/SceneOps.{h,cpp}`, `Engine/Nodes/Scene/SceneReparentNode.cpp`, `Tests/Scene/SceneMove_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: Move `WrapRetarget` and `gatherWrapRetargets()` out of the anonymous namespace into SceneOps, as `SceneMoveRetarget` and `gatherMoveRetargets(stage, const std::vector<std::pair<SdfPath,SdfPath>>& moves, retargets)`. A target under any move's source maps to that move's target (the longest prefix wins). A property inside a moved prim is authored at its moved path. Add `authorMoves(layer, moves, retargets, reactivateTargets, error)` for the reference method (the spec authoring in `renderWrap`). `renderWrap` calls both with one move. Nested moves are given outermost first, with each nested source already rewritten.
  - verify: the existing `SceneReparent_Test` and `USDXformPrim_Test` pass unchanged. A new gtest on an in-memory stage checks two sibling moves with a relationship between them, a nested move (`/a` → `/x/a`, then `/x/a/b` → `/y/b`), a connection from outside into a moved prim, and a binding from a moved prim to an unmoved material.
  - size: L

- [ ] M575.P1.T2 — Load sets and reach for a list of moves
  - files: `Engine/Nodes/Scene/SceneOps.{h,cpp}`, `Engine/Nodes/Scene/SceneReparentNode.cpp`, `Tests/Scene/SceneMove_Test.cpp`
  - approach: Generalize `mapWrapLoadSet` and `mapWrapReach` to `mapMovesLoadSet` and `mapMovesReach`, which take the move list. Rules at or under a target map back under its source, and an ancestor of a source stays and adds the target. Keep one-move wrappers for Wrap mode, or switch Wrap over. Add `mapMovedPathToInput(moves, outputPath)`, which returns empty for a source path a move vacated.
  - verify: gtest cases cover two moves, nested moves, a null load set staying null, and a vacated source mapping to empty. `SceneReparent_Test` still passes.
  - size: M

- [ ] M575.P1.T3 — A path-mapping interface for upstream lookups
  - files: `Engine/Nodes/Scene/ScenePathMapping.h` (new, free of USD headers, as `SceneReparentNode.h` notes), `Engine/Nodes/Scene/SceneReparentNode.{h,cpp}`, `Gui/ViewerTab.cpp`, `Gui/SceneTransformTarget.cpp`, `Tests/Scene/SceneReparent_Test.cpp`
  - approach: Add an abstract `ScenePathMapping` with `getDefinedPrimPath()` and `mapToInputPath(int inputNb, const std::string& outputPath)`. It returns empty when that input doesn't contribute the path, which replaces the `isWrapping()` input clamp. SceneReparentNode implements it from the existing `mapToInputPath()`. `findCameraNode` and the transform-target walk `dynamic_cast` to the interface instead of `SceneReparentNode`.
  - verify: a gtest calls the interface on USDXformPrim in Graft mode (every input) and Wrap mode (input 0 only, others empty). The existing Xvfb scripts covering the viewer camera through a USDXformPrim still pass.
  - size: M

- [ ] M575.P1.T4 — Graft arbitrary prims and author relocates
  - files: `Engine/Nodes/Scene/SceneOps.{h,cpp}`, `Tests/Scene/SceneMove_Test.cpp`, `Tests/Scene/SceneReparent_Test.cpp`
  - approach: (a) Add `graftPrims(const SceneGraftPrimsArgs&, memo, output, grafts, unmappedLinks, error)`, generalizing `reparentRoots`. The args are a list of `{inputIndex, sourcePath, graftedPath, SdfLayerOffset}` and an optional base payload to stack on. Each graft defines `graftedPath` with an internal reference to `sourcePath` in the input's stack layer. Links are carried when the target lies under a prim the same input grafts, `passThroughRoot` is handled as today, and the rest are unmapped. `reparentRoots` builds its list from the roots and calls it. (b) Add `canRelocate(payload, stage, source, target, reason)` with the Edge-cases rules, and `authorRelocates(layer, pairs)` through `SdfLayer::SetRelocates`.
  - verify: all `SceneReparent_Test` cases pass unchanged. New cases cover grafting a non-root prim with a layer offset (samples shift), and two grafted prims linking to each other (the link follows). Relocate cases: a prim under a referenced file asset relocates, and its descendants and an arc-side relationship follow. A USDRead-style sublayered prim is rejected with a reason. A relocate still composes after a later `reparentRoots` graft of its root.
  - size: L

## Phase 575.2: USDRestructure

- [ ] M575.P2.T1 — USDRestructure node with Reparent
  - files: `Engine/Nodes/Scene/USDRestructure.{h,cpp,reg.inc}`, the native registry include list beside `USDXformPrim.reg.inc`, `Engine/Nodes/Scene/CMakeLists` or the source list the Scene nodes use, `Tests/Scene/USDRestructure_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: A scene node with one input. Knobs: `primPattern` (`PrimSelector`), `operation` (Reparent/Rename/Duplicate/Remove Arcs; the other operations' knobs stay secret until chosen), `newParent`, `createParents`, `parentType`, `parentSpecifier`, `onlyDefineMissing`, `method`, and `invalidRelocate`. `renderScene` matches against the borrowed upstream stage (`SceneQueryPool`, input context load set) and builds the move list. It splits each move by `canRelocate` and Method, then authors relocates plus `authorMoves` plus retargets into the node's own memoized layer stacked on input 0. `getSceneInputContext`/`mapSceneInputReach` use `mapMovesLoadSet`/`mapMovesReach`, and the node implements `ScenePathMapping`. Creating the node from a scene selection fills `primPattern` (as USDXformPrim sets Wrap). The Edge-cases errors and warnings go through `setPersistentMessage`.
  - verify: gtests cover reparenting two prims under a new `/grp` (created as Xform with Define), Only Define Missing on an existing parent (an `over`), a collision error, a move into itself (an error), Auto relocating a USDReference'd asset's child and falling back on a local prim, and Method Relocate with Error on a local prim (the action fails).
  - size: L

- [ ] M575.P2.T2 — Rename operation
  - files: `Engine/Nodes/Scene/USDRestructure.{h,cpp}`, `Tests/Scene/USDRestructure_Test.cpp`
  - approach: Add `oldName` and `newName`, plus a pure `computeRenamedName(oldPattern, newPattern, name, index)` implementing `@name`, `@index` (match order in stage traversal) and the `*` capture. Matches whose names don't fit Old Name are skipped. Renames become moves to `parent/newName` through the P2.T1 path, nested renames applied outermost first.
  - verify: gtest of `computeRenamedName` on every row of the reference table (new_name@index, prefix, suffix, replace prefix, `char_*_left` → `char_*_right`), and a stage test renaming a camera and a bound mesh. The binding still resolves and the old paths are gone.
  - size: M

- [ ] M575.P2.T3 — Duplicate operation
  - files: `Engine/Nodes/Scene/USDRestructure.{h,cpp}`, `Tests/Scene/USDRestructure_Test.cpp`
  - approach: Add `sourceMode`, `totalNumber`, `separateDestination`, `destination`, `duplicateName` (the tokens in Scope), `referenceType` (Reference/Inherits/Specializes), `instanceable` and `parentKind` (Automatic picks group, or assembly when the children are models). Each copy is defined at its name, with the chosen arc to the source and `active` or `visibility` authored per Source Mode as the reference's Duplicate describes. Links inside the source subtree follow through the arc. Links from a copy to outside the subtree keep their targets.
  - verify: gtests cover each of the six Source Modes (counts and active/visibility per prim), Separate Destination under a missing parent, Inherits arcs, Instanceable copies sharing one prototype, and a duplicate name collision error.
  - size: L

- [ ] M575.P2.T4 — Duplicate transforms
  - files: `Engine/Nodes/Scene/USDRestructure.{h,cpp}`, `Tests/Scene/USDRestructure_Test.cpp`
  - approach: Add `SceneTransformKnobs` plus `pivot` and `cumulative` on the Duplicate section. Each copy gets `xformOp:transform:duplicate` (copy k gets the transform composed k+1 times when Cumulative is on, otherwise once), put first in an `xformOpOrder` that keeps the source's ops after it. Animated knobs author samples over the project range, as USDXformPrim does.
  - verify: gtest with three copies at translate (1,0,0): world positions are 1,2,3 with Cumulative and 1,1,1 without, and an animated rotate authors samples.
  - size: M

- [ ] M575.P2.T5 — Record duplicate info
  - files: `Engine/Nodes/Scene/USDRestructure.{h,cpp}`, `Tests/Scene/USDRestructure_Test.cpp`
  - approach: Add `recordCustomData` and `customDataName` (a string array of copy paths in the source's customData). Add `collectionPrim` and `collectionName`, which author a `UsdCollectionAPI` including the copies. Add `promoteCollections` (a pattern) and `promoteSourceCollections`, which copy matching collections found under the sources onto Collection Prim, rewritten per copy and merged by name, with the original contents added when promoting sources.
  - verify: gtest of the reference's `/Geometry/orig/lamp` example with Promote on and off and Promote Source on, plus the customData array contents.
  - size: M

- [ ] M575.P2.T6 — Remove Arcs operation
  - files: `Engine/Nodes/Scene/USDRestructure.{h,cpp}`, `Tests/Scene/USDRestructure_Test.cpp`
  - approach: Add `removeReferences`, `removePayloads`, `removeInherits` and `removeSpecializes`, each authoring an explicit empty list op on the matches in the node's layer. For each match, read the composition (the SceneOps `ScenePrimArc` walk) and warn for arcs of a chosen type that are introduced from outside the local stack.
  - verify: gtests check that a local reference and an inherits are cleared (the prim composes without them), a payload is cleared without loading it, and an arc inside a referenced asset stays, with the warning naming it.
  - size: M

- [ ] M575.P2.T7 — USDRestructure docs
  - files: `Engine/Nodes/Scene/docs/USDRestructure.md`
  - approach: In the style of `docs/USDXformPrim.md`, cover each operation and knob, the Auto/Relocate/Reference rules (USDRead content always uses Reference), link following, nested matches, Remove Arcs' limits, and errors. Add "Tolerance: none recorded." and no reference-app names.
  - verify: a reviewer reads the doc against the knob list in `USDRestructure.cpp`; every knob is described, and a grep for Houdini, Solaris, Gaffer, Katana and Nuke finds nothing.
  - size: M

## Phase 575.3: USDGraft

- [ ] M575.P3.T1 — USDGraft node with rows
  - files: `Engine/Nodes/Scene/USDGraft.{h,cpp,reg.inc}`, the registry include list, `Tests/Scene/USDGraft_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: Inputs Main (0) and Source (1). Knobs: `primPath`, `kind`, `parentType`, `parentSpecifier`, `makeUnique` and `destinationAsParent`. A `sourceCount` row count (at most 1024) shows rows of `enable<i>`, `sourcePath<i>` and `destinationPath<i>`, with Add After and Remove per row, declared through M567's `KnobRowList` (`Engine/Nodes/KnobRowList.{h,cpp}`) with one column per row knob; no change to the helper. `renderScene` resolves destinations (Destination As Parent, Make Unique with `_<n>`, missing ancestors per Parent Type, Specifier and Kind) and calls `SceneOps::graftPrims` on Main's payload. Unmapped links and missing sources become warnings. Input contexts map load sets per row (the destination's rule maps back to the source path; Main passes through), and the node implements `ScenePathMapping` for both inputs.
  - verify: gtests cover grafting `/geo/sphere` to `/new/ball` with Destination As Parent on and off, two rows on one destination with Make Unique on (`_1`) and off (composed), a missing source (a warning), a link between two grafted rows following, and a binding into `/materials` carried over.
  - size: L

- [ ] M575.P3.T2 — Frame offsets
  - files: `Engine/Nodes/Scene/USDGraft.{h,cpp}`, `Tests/Scene/USDGraft_Test.cpp`
  - approach: Add `frameOffsetMode` and `frameOffset`, which compute one `SdfLayerOffset` for all rows per the Edge-cases rules, passed in the `graftPrims` args.
  - verify: gtest grafting a 100–200 source into a 200–300 Main. Match Start Times puts the first sample at 200, Offset −10 puts it at 90, and Match And Offset 5 puts it at 205. Different time codes per second convert correctly.
  - size: M

- [ ] M575.P3.T3 — Keep Position
  - files: `Engine/Nodes/Scene/USDGraft.{h,cpp}`, `Tests/Scene/USDGraft_Test.cpp`
  - approach: Add `keepPosition<i>`, which authors the compensating `xformOp:transform:graftKeep` per the Edge-cases rule using `UsdGeomXformCache` on both stages, sampled over the project range when time-varying.
  - verify: gtest grafting a prim from under a translated, rotated parent to under a scaled parent. Its world matrix matches the source within 1e-9, and with an animated source parent it matches at three frames.
  - size: M

- [ ] M575.P3.T4 — Keep Material
  - files: `Engine/Nodes/Scene/USDGraft.{h,cpp}`, `Tests/Scene/USDGraft_Test.cpp`
  - approach: Add `keepMaterial<i>`. When the grafted prim's bound material comes from an ancestor outside the graft, author a direct `material:binding` on the grafted prim, carried by the link rules (Materials Scope pass-through, a graft of another row, or a warning).
  - verify: gtests cover an inherited binding under `/materials` being kept, the same with Keep Material off (falls back to Main's ancestor binding), and a material outside both giving the warning.
  - size: M

- [ ] M575.P3.T5 — USDGraft docs
  - files: `Engine/Nodes/Scene/docs/USDGraft.md`
  - approach: In the style of `docs/USDXformPrim.md`, cover the inputs, destination resolution with examples, rows, Make Unique, frame offsets, Keep Position and Keep Material, link rules, composing with Main's opinions, and errors. Add "Tolerance: none recorded." and no reference-app names.
  - verify: a reviewer checks every knob in `USDGraft.cpp` is described, and the reference-app-name grep finds nothing.
  - size: M

## Phase 575.4: GUI check and audit

- [ ] M575.P4.T1 — Xvfb GUI script and screenshots
  - files: `Tests/gui/usd_restructure_graft.py`
  - approach: Under Xvfb at 1920x1080, build USDCamera + Card → USDXformPrim → USDRestructure renaming the camera, and check the viewer can look through the renamed camera (`findCameraNode` through the interface). Reparent the card and check the transform handle edits USDXformPrim's knobs. Build USDGraft from a second USDRead-free scene, add three rows with Add After, remove the middle one, and check the remaining rows' knobs. Take screenshots of both node panels with every operation section shown.
  - verify: the script passes under Xvfb, and the screenshots go to the user, one file each.
  - size: M

- [ ] M575.P4.T2 — Params→knob audit
  - files: none (report)
  - approach: For each reference control listed in `## Scope`, give the knob that implements it (script name, label = readable script name, default) or its Out reason. Diff that against the knobs the two nodes actually create.
  - verify: the table is in the PR body; no In control is left without a knob, and no knob is left unlisted.
  - size: S

**Verification gate:** release gate green per `.local/build-mode`; `SceneMove_Test`, `SceneReparent_Test`, `USDXformPrim_Test`, `USDRestructure_Test` and `USDGraft_Test` pass; `Tests/gui/usd_restructure_graft.py` passes under Xvfb at 1920x1080; the user OKs the panel screenshots; the params→knob audit has no gaps.

## Decisions

- 2026-11-24 — **Scene Graph panel commands are elsewhere (user):** Rename and drag-to-reparent in the Scene Graph panel are Out here; M589 owns them.
- 2026-11-24 — **No variant-set operations (user):** rename and remove variant set stay Out.
- 2026-11-24 — **One relocate per node (user):** several relocates per node are Out; chain nodes instead.
- 2026-11-24 — **Row list ships in M567 (planner):** the reusable knob row list (`Engine/Nodes/KnobRowList.{h,cpp}`) ships in M567 - USD Light Linking, which lands earlier; USDGraft reuses it, so M575 depends on M567.

- 2026-11-24 — **Milestone now (user):** USDRestructure (rename, move and duplicate prims) and USDGraft (graft a second input's prims under a chosen path), authoring USD relocates where they fit and sharing the link-following code of USDXformPrim's Wrap mode.
- 2026-11-24 — Grafts compose through internal references to the Source's stack layer, as `reparentRoots` does, rather than copying flattened specs: the result follows upstream edits and reuses the existing link and load-set mapping (planner).
- 2026-11-24 — Method Auto relocates only prims with no spec in the input's layers, and falls back to reference-and-deactivate otherwise, since USD relocates cannot move prims of the local layer stack (planner).
