# M236 - Shared Materials And Assignment

Asset-style material workflow, beside M226 - Scene Hierarchy From Topology's comp-first one (each geometry carries its own material). A project setting names a shared materials scope, a root prim (default `/materials`). Re-parenting nodes leave that root at the top instead of grafting it, and the M226 link repair re-points bindings into it. `USDAssignMaterial` binds a material to the prims a pattern matches. On each matched root it authors one collection plus a `material:binding:collection` relationship, so a later graft repairs one relationship per root (probe 2026-10-26: ~5 ms, against ~900 ms for 20k direct bindings). Research: UsdShade white paper (`.local/docs/usd/wp_usdshade.html`), pxr 26.08 `usdShade/materialBindingAPI.h`, Solaris Assign Material / Material Library LOPs, Katana UsdMaterialAssign.

## Scope

**In:**
- Project knob Materials Scope (`materialsScope`, default `/materials`; empty turns the feature off).
- `SceneOps::reparentRoots` leaves the scope root at its own path (never renamed); `USDXformPrim`/`USDScopePrim` pass it through and remap links into it as identity.
- `USDAssignMaterial`: a Scene input plus an optional Material input. Knobs Prim Pattern (USDEditTransform's glob syntax), Material Path, Purpose (All/Full/Preview) and Strength (Stronger Than Descendants by default, or Weaker Than Descendants). With the Material input connected, its roots are grafted under the scope and the first material, depth first, is bound. Otherwise Material Path names a material already in the scene.
- USDWrite round-trips assigned scenes: the scope is written once and repair overrides are flattened.
- A strict-GL Storm render, a GUI script with screenshots, and node docs.

**Out:**
- Direct-binding mode, an Unassign node, GeomSubset assignment: none (a later node if asked).
- Path-expression patterns and collection references in patterns: M52 - USD Layer-Stack Completion.
- Picking prims or materials from the Viewer or scene graph: M231 - Scene Graph Panel / M232 - Scene Selection To Nodes.
- Changes to `USDMaterial`, the primitives' Material input graft, kind/purpose/visibility: none (primitives stay simple).

**Edge cases:**
- Matches under several roots: one collection plus one binding per root. Matches inside the scope are skipped, and a warning names them. Zero matches give USDEditTransform's zero-match warning.
- Two assigns on the same root: each one writes the root's `propertyOrder` with its own binding first, so the later node in the graph wins. A later assign downstream of a re-parent sits on a higher ancestor, and as stronger-than-descendants it also wins.
- Comp-first geometry (`/Card1` bound to `/Card1/Material1`): an assign that matches it overrides that binding, because collection beats direct on the same prim.
- A Material input whose root is the scope itself (a `USDScopePrim` named `materials`) passes through rather than nesting at `/materials/materials`.
- A Material Path that does not resolve: the binding is still authored, with a node warning.
- A non-scope root named like the scope, or a file whose own `/materials` root holds gprims: passed through, with a warning on the re-parent node. Two `/materials` from different inputs compose later-stronger, as in USDMerge.
- Materials Scope empty, or not a single-name absolute path: re-parent nodes graft everything (M226 behaviour) and USDAssignMaterial with a Material input errors.
- An instanceable prim: matching the instance root works; matching inside it needs instance-proxy traversal and is not supported.

**Assumptions:**
- Storm's material-binding resolution honours collection bindings, `bindMaterialAs` and `propertyOrder` as `ComputeBoundMaterial` does (pxr 26.08). M236.P3.T2 proves it.
- Binding names derived from the node's fully qualified script name are unique on a root.

## Phase 236.1: Scope substrate

- [ ] M236.P1.T1 — Project Materials Scope setting, in the scene layer key
  - files: `Engine/Project.{h,cpp}`, `Engine/ProjectPrivate.h`, `Engine/Nodes/Scene/SceneLayerMemo.h`, `Engine/Nodes/NativeEffectBase.cpp` (`getSceneLayerKey`), `Tests/Scene/SceneUpAxis_Test.cpp`
  - approach: a string knob beside Up Axis/Meters Per Unit (Project.cpp ~1842). `getProjectMaterialsScope()` returns the validated root name, or empty. It joins the project digest (~3555) and the up-axis invalidation branch (~2920). `SceneLayerMemo::Key` gets `materialsScope`, filled where `upAxis` is (NativeEffectBase.cpp:938).
  - verify: gtest — the default is `/materials`; changing it re-keys a scene node's layer; `/a/b` and `materials` validate to empty.
  - size: M

- [ ] M236.P1.T2 — `reparentRoots` passes the scope root through
  - files: `Engine/Nodes/Scene/SceneOps.{h,cpp}`, `Engine/Nodes/Scene/SceneReparentNode.cpp`, `Tests/Scene/SceneReparent_Test.cpp`
  - approach: `SceneReparentArgs::passThroughRoot`. In each graft layer that root is defined at its own path, referencing `<wrapper>:/<root>`, and is never auto-renamed. `graftedPathOf` maps it to itself, so the existing link repair re-points relationships into it. The name joins the graft hash. SceneReparentNode fills it from the project and warns when the passed-through root holds a `UsdGeomGprim`.
  - verify: gtest — an input with `/Asset` bound to `/materials/M` grafts to `/T1/Asset`, with `/materials/M` still at the root and `ComputeBoundMaterial` resolving it. Two inputs with `/materials` compose. With the setting empty, M226 behaviour is unchanged.
  - size: L

## Phase 236.2: Assignment

- [ ] M236.P2.T1 — `SceneNodeAuthoring::authorCollectionBinding` on one root
  - files: `Engine/Nodes/Scene/SceneNodeAuthoring.{h,cpp}`, `Tests/Scene/SceneMaterialBinding_Test.cpp`
  - approach: given a root prim, matched paths, material path, binding name, purpose, strength and the root's existing collection-binding names: apply `CollectionAPI(<name>)` (includes, `expandPrims`) and `MaterialBindingAPI`; author `material:binding:collection[:<purpose>]:<name>` = [collection, material] with `bindMaterialAs`; write `propertyOrder` with the new binding first.
  - verify: pure-USD gtest — `ComputeBoundMaterial` resolves per purpose. A second binding written later wins on overlap. Stronger beats a descendant's direct binding and Weaker does not. After an internal-reference graft plus a repaired material target, the binding still resolves.
  - size: M

- [ ] M236.P2.T2 — `USDAssignMaterial` node with Material Path
  - files: `Engine/Nodes/Scene/USDAssignMaterial.{h,cpp,reg.inc}`, `Tests/Scene/USDAssignMaterial_Test.cpp`
  - approach: Scene input. Match with `matchPrims` through `SceneQueryPool`, group matches by root, skip the scope, and call P2.T1 per root inside `emitSceneLayer` with `eUpstreamReadFacts` (the digest covers matches, material path and existing binding names). Warnings follow the Edge cases. Knob labels follow script names.
  - verify: gtest — `/Asset/geo/*` binds `/materials/M`. The node then goes under a USDXformPrim and `/T1/Asset/geo/a` still resolves to it. Two chained assigns: the later wins. A Card's own material is overridden. A miss warns.
  - size: L

- [ ] M236.P2.T3 — Material input grafts into the scope
  - files: `Engine/Nodes/Scene/USDAssignMaterial.{h,cpp}`, `Tests/Scene/USDAssignMaterial_Test.cpp`
  - approach: when connected, `reparentRoots` the input under the scope (Scope type, scope root passed through) with its own graft memo, stacks it on the scene input via `SceneOps::appendEntry`, and binds the first material depth first. Material Path greys out while the input is connected.
  - verify: gtest — USDMaterial1 → `/materials/Material1`, bound. Two assigns sharing one material node give one `/materials/Material1`. A `USDScopePrim` named `materials` does not nest.
  - size: M

## Phase 236.3: Export, render, docs

- [ ] M236.P3.T1 — USDWrite round-trip of shared materials
  - files: `Engine/Nodes/Scene/USDWrite.cpp` (only if needed), `Tests/Scene/USDWrite_Test.cpp`
  - approach: a USDRead asset → USDAssignMaterial (Material input) → USDXformPrim → USDWrite in both modes, then read back. Fix the export only if the check fails.
  - verify: round-trip gtest — exactly one `/materials/Material1`; `ComputeBoundMaterial` is identical before and after; the repaired binding is written plainly into the output (no anonymous layer identifiers); in layered mode the asset file is still referenced by a relative path.
  - size: M

- [ ] M236.P3.T2 — Strict-GL Storm render of a pattern-bound asset
  - files: `Tests/Scene/USDRenderStorm_Test.cpp`, one committed reference crop (≤256 px, half-float, zip)
  - approach: render the P3.T1 graph with a red shared material over a grey asset material, Stronger strength, then again with a second assign over it.
  - verify: passes under `build/m63-gui/run-gl-tests.sh` with `NATRON_TEST_REQUIRE_GL=1`; switching Strength to Weaker makes it fail.
  - size: M

- [ ] M236.P3.T3 — GUI script and screenshots
  - files: `Tests/gui/scene_assign_material.py` (fixtures under `build/`)
  - approach: build the graph under Xvfb at 1920x1080 and screenshot the Viewer's 3D view before and after inserting the USDXformPrim, plus the USDAssignMaterial panel.
  - verify: the script passes and writes three screenshots; they are shared with the user (SendUserFile) before sign-off.
  - size: M

- [ ] M236.P3.T4 — Node docs
  - files: `Engine/Nodes/Scene/docs/USDAssignMaterial.md` (new), `docs/USDXformPrim.md`, `docs/USDScopePrim.md`
  - approach: describe the knobs, the collection-per-root model, strength/purpose and scope pass-through, following the existing docs' layout.
  - verify: the files render and name every knob.
  - size: S

**Verification gate:** release gate green (per `.local/build-mode`); the strict-GL test passes; `scene_assign_material.py`, `scene_viewer_live_edit.py` and `m226_scene_hierarchy.py` pass under Xvfb; the user OKs the screenshots.

## Decisions

- 2026-10-27 — **User:** USDAssignMaterial has both a Material input (grafted into the scope and bound) and a Material Path knob (greyed while the input is connected). Default strength Stronger Than Descendants.
- 2026-10-27 — **Collection binding per matched root, Stronger Than Descendants by default (cat-plan):** with bindings only on roots, graph order equals binding strength. A later assign is either on the same root (where `propertyOrder` decides) or on a higher ancestor after a re-parent (where stronger-than-descendants decides). Weaker would let an asset's internal bindings, or an earlier assign deeper down, beat a later node. Rejected: direct bindings on each prim (~900 ms repair per graft at 20k prims).
