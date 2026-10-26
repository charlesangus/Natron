# M226 - Scene Hierarchy From Topology

The node graph's shape becomes the scene's shape. Card1 → USDXformPrim "Transform1" and
Sphere1 → "Transform2", merged, gives `/Transform1/Card1` and `/Transform2/Sphere1`. Today
the same graph gives a flat `/Card1`, `/Sphere1`, because USDTransform only edits existing
prims by pattern. That is the Nuke behaviour the user rejected in M19/M20 UAT
(2026-10-26). This milestone adds the re-parenting hierarchy nodes and renames the edit
node. It turns the create nodes into pure sources and makes the project's up axis and units
authored data. Survey and rationale: `PLAN/DESIGN/2026-10-26-3d-mvp-survey.md`. Decision:
`PLAN/DECISIONS/2026-10-26-scene-hierarchy-from-topology.md`.

Code lives in `Engine/Nodes/Scene/` on top of M19 - USD/Hydra Foundation and M20 - 3D Node
Vocabulary. Nodes are `NativeEffectBase` subclasses that author one memoized layer each
(`SceneNodeAuthoring.cpp`, `emitSceneLayer`).

## Scope

**In:**
- `USDXformPrim` and `USDScopePrim`, which replace `USDPrim`. Each takes any number of scene inputs and re-parents every input's root prims under its prim path, creating intermediate prims for a nested path. `USDXformPrim` has TRS + pivot knobs.
- An optional auto-rename knob (default off) on both. Same-named roots from different inputs otherwise compose, with later inputs stronger, as in `USDMerge`.
- `USDTransform` renamed to `USDEditTransform`, with its behaviour unchanged: a pattern edit of existing prims.
- `USDCamera` and `USDLight` lose their scene input and become pure sources at `/<NodeName>`.
- Project settings Up Axis (Y default, or Z) and Meters Per Unit (default 1.0 — unconfirmed, see Assumptions). Every created node authors them as layer metadata, and a USDRead whose file disagrees gets a node warning.
- USDWrite exports the new reference structure correctly in both modes.

**Out:**
- Converting up axis or units of scene data — none (a future node).
- Primitive geometry nodes — M227 - USD Primitive Nodes.
- Transform gizmos on `USDXformPrim` and the gizmo target policy — M230 - 3D Transform Widgets.
- Path-expression patterns and the schema-driven create/edit node — M52 - USD Layer-Stack Completion.
- A generic `USDAddPrim` — none (user chose Xform + Scope only).
- Loading projects saved with `USDPrim`/`USDTransform` — none (no backward compatibility in this fork).

**Edge cases:**
- A nested prim path (`/set/props/Transform1`): the missing ancestors are defined as `Xform` on `USDXformPrim` and as `Scope` on `USDScopePrim`.
- Two inputs both carry root `/Card1`. Auto-rename off: they compose, later input stronger. Auto-rename on: the later one becomes `/Card1_1` (the first free suffix).
- An input with no root prims contributes nothing, and the node still defines its own prim.
- An input's root prim path equals the node's own path (`/Transform1` fed into Transform1): node error naming the cycle-like collision; the scene is not authored.
- A USDRead under a `USDXformPrim`: the file's roots (`/World`) land at `/Transform1/World`. USDRead itself still keeps file roots (user).
- An upstream USDEditTransform pattern written against `/Card1` downstream of a re-parent must now say `/Transform1/Card1`. That is intended. The pattern simply matches nothing, and the existing zero-match warning tells the user.
- Up axis mismatch in a merge (a Z-up file in a Y-up project): the project setting wins on created layers, and USDRead warns. Composed `upAxis` metadata follows the strongest layer as today.

**Assumptions:**
- Re-parenting by internal references works: wrap the upstream stack in an anonymous layer that sublayers it, then author `/<Name>/<root>` children referencing `<wrapper>:/<root>`. It keeps memoization and SceneStage's sublayer-diff sync. M226.P1.T1 proves it before the nodes build on it.
- USDWrite's layered mode can flatten an anonymous wrapper layer reached through a reference.
- Unconfirmed: the Meters Per Unit default is 1.0 (metres, Nuke/Houdini convention), not USD's 0.01.

## Phase 226.1: Re-parenting substrate

- [ ] M226.P1.T1 — `SceneOps::reparentRoots`: graft input roots under a new prim, incrementally
  - files: `Engine/Nodes/Scene/SceneOps.h`/`.cpp`, `Engine/Nodes/Scene/ScenePayloadImpl.h` (the payload owns its wrapper layers and the input stacks they sublayer), `Engine/Nodes/Scene/SceneLayerMemo.h`, `Tests/Scene/SceneReparent_Test.cpp`
  - approach: split layout, per the 2026-10-26 probe: a structure layer (`def` of the destination and its ancestors, keyed on path and type), an xform layer authoring only `over` opinions (keyed on the transform knobs), and one reference layer per input (keyed on that input's wrapper) whose root children internally reference `<wrapper>:/<root>`. Each wrapper sublayers its input's stack. Return a fresh payload of 2 + inputs layers that keeps every wrapper and input stack alive itself, because a payload held by a render can outlive the memo entry and a dead wrapper composes an empty prim silently. Root names come from the composed input via `SceneQueryPool`; auto-rename picks the first free `_<n>` suffix in input order.
  - verify: gtest — `/T1/Card1` and `/T1/Sphere1` with no root `/Card1`; an animated xformOp survives; collision later-stronger (rename off) and `/T1/Card1_1` (rename on); unchanged inputs give the same layer handles; an edit to one input resyncs only that input's grafted root, and an xform knob edit only `/T1`'s xformOp properties (count resynced paths as in `SceneStage_Test`); a payload held past its memo entry still composes.
  - size: L

- [ ] M226.P1.T2 — Repair cross-root relationships on graft
  - files: `Engine/Nodes/Scene/SceneOps.*`, `Engine/Nodes/Scene/ScenePayloadImpl.h`, `Engine/Nodes/Scene/USDRead.cpp`, `Tests/Scene/SceneReparent_Test.cpp`
  - approach: the payload carries an index of relationships whose targets lie outside their own root: USDRead fills it with one Sdf-level scan of the file at load, Natron nodes record any they author, USDMerge unions indexes, and re-parenting remaps them. A grafted relationship in the index gets an override at its new path, authored in the node's own layer (allowed there, unlike inside a reference), pointing at the remapped target. Targets that can't be remapped raise a node warning naming them.
  - verify: gtest — a two-root file (`/geo` bound to `/looks/M`) grafted under `/T1` still resolves its material (`ComputeBoundMaterial`); an unremappable target warns.
  - size: M

## Phase 226.2: Nodes

- [ ] M226.P2.T1 — `USDXformPrim` and `USDScopePrim` replace `USDPrim`
  - files: `Engine/Nodes/Scene/USDXformPrim.{h,cpp,reg.inc}`, `Engine/Nodes/Scene/USDScopePrim.{h,cpp,reg.inc}`, `Engine/Nodes/Scene/USDPrim.*` (deleted), `Engine/Nodes/Scene/SceneNodeAuthoring.cpp`, `Engine/AppManager.cpp` (registration), CMake source list, `Tests/Scene/USDXformPrim_Test.cpp`
  - approach: share a small base for the multi-input/re-parent logic over M226.P1.T1. Both have a Prim Path knob (default `/<scriptName>`, following renames as today), Kind, and an Auto Rename bool. `USDXformPrim` also has translate/rotate/scale/pivot knobs authored as one `xformOp:transform` on its prim, animated as time samples per M19's policy. Ancestors are Xform for XformPrim and Scope for ScopePrim. Multi-input like `USDMerge` (the input count grows as inputs are connected). Labels follow script names.
  - verify: gtest — the user's example graph (Card stand-in → Xform1, Sphere stand-in → Xform2, both → USDMerge) composes `/Xform1/Card1` and `/Xform2/Sphere1` with each xform's matrix on its prim; a nested path defines Xform ancestors (Scope for ScopePrim); a node with no inputs defines just its prim; zero `USDPrim` references remain (`grep`).
  - size: M

- [ ] M226.P2.T2 — Rename `USDTransform` to `USDEditTransform`
  - files: `Engine/Nodes/Scene/USDTransform.*` → `USDEditTransform.*`, `Engine/AppManager.cpp`, `Gui/SceneViewport.cpp`, `Gui/ViewerTab.cpp` (gizmo lookup), CMake source list, `Tests/Scene/USDTransform_Test.cpp` → `USDEditTransform_Test.cpp`, `Tests/gui/scene_viewer_live_edit.py`, `Engine/Nodes/Scene/docs/`
  - approach: mechanical rename of class, plugin ID, label, files and tests. The authored op name stays `xformOp:transform:<nodename>`. No compatibility alias.
  - verify: the renamed gtest and `scene_viewer_live_edit.py` pass; `grep -rn "USDTransform\b"` finds nothing outside the plan.
  - size: M

- [ ] M226.P2.T3 — `USDCamera` and `USDLight` become pure sources
  - files: `Engine/Nodes/Scene/USDCamera.cpp`, `Engine/Nodes/Scene/USDLight.cpp`, `Engine/Nodes/Scene/SceneNodeAuthoring.{h,cpp}`, `Tests/Scene/USDCamera_Test.cpp`, `Tests/Scene/USDLight_Test.cpp`, `Tests/Scene/SceneIntegration_Test.cpp`
  - approach: remove the optional scene input. Each node authors only its own prim at its Prim Path into a fresh payload. Delete the "author on top of input / replace input transform" code paths. Update the integration graph to combine through `USDMerge` or a `USDXformPrim`.
  - verify: gtests — each node has zero inputs and composes only its own prim; the integration test passes with the new wiring.
  - size: M

- [ ] M226.P2.T4 — Materials live under their geometry
  - files: `Engine/Nodes/Scene/USDMaterial.*`, the M20 geometry nodes that bind materials, `Engine/Nodes/Scene/SceneNodeAuthoring.*`, `Tests/Scene/USDMaterial_Test.cpp`, `Tests/Scene/SceneIntegration_Test.cpp`
  - approach: comp-first, as classic Nuke. USDMaterial becomes a material source with no scene input. Geometry nodes take an optional Material input: the geometry node grafts the incoming material under its own prim (`/Card1/Material1`) and binds itself to it in its own layer, so re-parenting carries both and USD remaps the binding. Pattern binding moves to the later shared-materials milestone.
  - verify: gtest — geometry with a material input binds `/Card1/Material1`; under `/Transform1` it resolves `/Transform1/Card1/Material1`; one material node feeding two geometry nodes gives each its own copy.
  - size: M

## Phase 226.3: Up axis and units

- [ ] M226.P3.T1 — Project Up Axis and Meters Per Unit, authored by created nodes
  - files: `Engine/Project.cpp`/`ProjectPrivate.*` (two project knobs on the existing settings page), `Engine/Nodes/Scene/SceneNodeAuthoring.cpp` (`openLayerStage` authors `upAxis`/`metersPerUnit`), `Engine/Nodes/Scene/USDRead.cpp` (mismatch warning), `Engine/Nodes/Scene/USDCamera.cpp` (lens mm conversion reads the project's units), `Tests/Scene/SceneUpAxis_Test.cpp`
  - approach: Up Axis is a choice Y/Z (default Y) and Meters Per Unit a double (default 1.0). Every scene node that opens a layer authors both as layer metadata, and changing either knob invalidates scene nodes (it goes into their layer key). USDRead compares the file's `upAxis`/`metersPerUnit` with the project's and sets a persistent node warning naming both values when they differ. It never converts. The viewport keeps following the composed stage (M19), so a Z-up project now tumbles around Z with no file loaded.
  - verify: gtest — a USDCamera layer carries the project's upAxis/metersPerUnit; switching the project to Z re-authors it; a Z-up file read into a Y-up project warns, and a matching one doesn't; USDCamera focal length in mm converts with the project's metersPerUnit.
  - size: M

## Phase 226.4: Export

- [ ] M226.P4.T1 — USDWrite exports re-parented scenes
  - files: `Engine/Nodes/Scene/USDWrite.cpp`, `Tests/Scene/USDWrite_Test.cpp`
  - approach: layered mode flattens the anonymous wrapper layers that internal references point at into the output. Either flatten the reference arc away for anonymous targets, or write the wrapper as a sidecar and retarget the reference — pick the one that keeps file roots (USDRead layers) referenced, as M20's layered mode does, and record it in `## Decisions`. Flattened mode needs no change; verify it.
  - verify: round-trip gtest — the user's example graph with one USDRead under an Xform → USDWrite (both modes) → USDRead composes the identical prim tree (`ExportToString` diff); in layered mode the external file is still referenced by a relative path.
  - size: M

**Verification gate:** release gate green; the user's Card/Sphere example (stand-in prims until M227 - USD Primitive Nodes) builds `/Transform1/Card1` + `/Transform2/Sphere1` in the Viewer's 3D view and round-trips through USDWrite; the Up Axis setting flips the viewport grid with no file loaded; `scene_viewer_flip.py` and `scene_viewer_live_edit.py` pass under Xvfb; screenshots shared before sign-off.

## Decisions

- 2026-10-26 — **Units on edit layers (bosley-native-nodes):** every layer Natron authors, edit nodes included (USDEditTransform, USDPrune, the re-parent nodes' layers), carries the project's Up Axis and Meters Per Unit; graft layers copy only what their input authors. Downstream of any Natron node the composed scene therefore always reports the project's units, and USDRead's warning is the signal that a file's data disagrees. Never converted.
- 2026-10-26 — **Re-parenting layout (probe, bosley-native-nodes):** one layer holding all the references resyncs the whole `/T1` subtree on any upstream edit; the split layout (structure, xform `over`, one reference layer per input) resyncs only the edited input's root, and an xform knob edit only `/T1`'s xform properties. The payload owns its wrappers and input stacks. Probes in `build/m226-probe/`.
- 2026-10-26 — **Materials (user):** comp-first — each geometry node carries its own material through a Material input, authored under the geometry's prim, so re-parenting needs no repair; a shared `/materials` scope with pattern assignment is a later asset-workflow milestone (M236). No `/natron` umbrella root. Cross-root links in multi-root USDRead files are repaired on graft. Rejected after probing: a shared scope re-pointed by every Transform (one override per bound prim, ~0.7 s per edit at 20k prims) and relocates (every edit resyncs `/`).
- 2026-10-26 — **Hierarchy follows graph topology (user, M19/M20 UAT grill):** USDXformPrim/USDScopePrim re-parent their inputs (multi-input, optional auto-rename, collisions otherwise compose later-stronger); USDTransform is renamed USDEditTransform and keeps pattern-edit semantics; USDPrim is retired; create nodes (camera, light, primitives) have no scene input; USDRead keeps file roots. Project-wide record: `PLAN/DECISIONS/2026-10-26-scene-hierarchy-from-topology.md`.
- 2026-10-26 — **Up axis is a project setting authored by created nodes (user):** not a viewport preference; mismatching files warn, never convert.
- 2026-10-26 — **Re-parent by internal references (cat-plan, from the code survey):** wrapper layer sublayering the input stack + per-root internal references keeps per-node memoization and SceneStage's sublayer-list sync; Solaris Graft's flatten-copy would re-copy the subtree on every upstream edit. Proven first by M226.P1.T1.
