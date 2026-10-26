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

- [ ] M226.P1.T1 — `SceneOps::reparentRoots`: graft an input stack's roots under a new prim
  - files: `Engine/Nodes/Scene/SceneOps.h`/`.cpp`, `Engine/Nodes/Scene/SceneLayerMemo.h` (keep the wrapper layer alive with the memo entry), `Tests/Scene/SceneReparent_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: given one or more input payloads, a destination path, an ancestor prim type and an auto-rename flag, build one anonymous wrapper layer per input that sublayers that input's stack. Author into the caller's layer: the ancestors, the destination prim, and one child per input root prim that internally references `<wrapper>:/<root>`. Return a fresh payload (wrapper-free stack: the node layer plus the wrappers it references), the way `USDMerge` builds from scratch, not an append onto the upstream. Root names come from the composed input via `SceneQueryPool`, and the layer key folds in those names (M19.P3.T5 read keys). Auto-rename picks the first free `_<n>` suffix in input order.
  - verify: gtest — two inputs with roots `/Card1` and `/Sphere1` under `/T1` compose to `/T1/Card1` and `/T1/Sphere1` with no root-level `/Card1`; an animated xformOp on `/Card1` still animates at `/T1/Card1`; a collision composes (later stronger) with rename off and yields `/T1/Card1_1` with it on; re-evaluating unchanged inputs returns the same layer handle; a SceneStage synced to the result then to an edited upstream resyncs only the edited subtree (count resynced paths as in `SceneStage_Test`).
  - size: L

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

- 2026-10-26 — **Hierarchy follows graph topology (user, M19/M20 UAT grill):** USDXformPrim/USDScopePrim re-parent their inputs (multi-input, optional auto-rename, collisions otherwise compose later-stronger); USDTransform is renamed USDEditTransform and keeps pattern-edit semantics; USDPrim is retired; create nodes (camera, light, primitives) have no scene input; USDRead keeps file roots. Project-wide record: `PLAN/DECISIONS/2026-10-26-scene-hierarchy-from-topology.md`.
- 2026-10-26 — **Up axis is a project setting authored by created nodes (user):** not a viewport preference; mismatching files warn, never convert.
- 2026-10-26 — **Re-parent by internal references (cat-plan, from the code survey):** wrapper layer sublayering the input stack + per-root internal references keeps per-node memoization and SceneStage's sublayer-list sync; Solaris Graft's flatten-copy would re-copy the subtree on every upstream edit. Proven first by M226.P1.T1.
