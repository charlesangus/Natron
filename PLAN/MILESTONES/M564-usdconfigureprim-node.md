# M564 - USDConfigurePrim Node

A `USDConfigurePrim` node edits prim **metadata** on the prims a pattern matches: kind, active, instanceable, hidden, specifier, asset info and custom data. Today no node does: USDEditPrim authors schema attributes, USDSetAttribute one raw attribute, and nothing reaches the spec-level fields (`kind`, `active`, `instanceable`, `hidden`, `assetInfo`, `customData`) that USD keeps outside attributes. It mirrors Solaris Configure Primitives (`.local/docs/houdini/nodes/lop/configureprimitive.md`) for its metadata controls, and its "only author what the user touched" rule: each field has an enable toggle (`AuthoredKnobSet`, `Engine/Nodes/Scene/AuthoredKnobSet.h`) and an untouched field authors nothing, so the node never stamps defaults over its input. It is the metadata half of the settled shape; attributes stay in USDEditPrim and USDSetAttribute, and its tooltips say where (purpose and draw mode are schema attributes reachable through USDEditPrim with the Imageable and GeomModelAPI schemas).

The node follows the USDVariant pattern (`Engine/Nodes/Scene/USDVariant.cpp`): resolve the pattern with `SceneNodeAuthoring::matchPattern`, then author `over` specs in its own layer through `emitSceneLayer`, with `SdfPrimSpec` setters for the metadata. Custom data and asset info use the table knob M449 adds (`Engine/KnobLayerCustomData.{h,cpp}`, `Gui/KnobGuiLayerCustomData.{h,cpp}`: rows of name, type, value for String, String Array, Token, Token Array, Float, Integer, Boolean), so this milestone depends on M449 - USD Layer Save Paths for the knob.

## Scope

**In** (every Solaris Configure Primitives control, In or Out with a reason; Out is below):
- `primPattern` (Prim Pattern), the prims to configure.
- Kind: a choice None | Assembly | Group | Component | Subcomponent behind a toggle. None authors an empty kind (`kind = ""`) to block a weaker kind; untouched authors nothing.
- Fix Kind Hierarchy (bool, "Fix Kind Hierarchy"): for each configured prim whose composed kind (after this node's Kind) is a model kind (`component`, `group` or `assembly`), every ancestor whose composed kind is not `group` or `assembly` is given kind `group`, so the model hierarchy is valid (Solaris: "all parents of component or group have a kind of assembly or group").
- Active (bool toggle): `active` metadata, authored true or false.
- Instanceable (bool toggle): `instanceable` metadata.
- Hide Prim In Scene Graph (bool toggle, label "Hidden"): the `hidden` metadata that hides the prim in scene-graph trees; the viewer still draws it.
- Specifier (choice Define | Over | Class, "Specifier"): the specifier of the node's own prim specs. Default Over; the choice only applies when set.
- Asset Info: the fixed fields Asset Identifier ("Identifier", an asset path), Asset Name, Asset Version (string), Asset Dependencies (asset-path array, one per line), plus a table of extra named asset-info entries, all authored into the `assetInfo` dictionary under their USD keys (`identifier`, `name`, `version`, `payloadAssetDependencies`). Each fixed field has its own toggle.
- Custom Data: a table of named, typed rows authored into `customData`; a row name containing colons nests (`a:b` goes to `customData["a"]["b"]`, USD's convention).
- Docs `USDConfigurePrim.md`, the toolbar registration in `PLUGIN_GROUP_USD_Edit`, tests, one Xvfb GUI script.

**Out** (control by control, from Solaris Configure Primitives):
- Sampling Behavior, Start/End/Inc, Subframe Sampling, Shutter, Samples, Always Include Frame Sample: metadata is not time-sampled; the node authors one value. No milestone.
- Purpose, Proxy Primitive: purpose is a schema attribute and proxyPrim a relationship; both belong to USDEditPrim (Imageable schema), the latter through M562 - USDEditPrim Relationship Knobs. The tooltips say so.
- Visibility: a schema attribute (`visibility`, animatable); USDEditPrim, Imageable.
- Set Extents and its modes: `extent` is a computed array attribute; no milestone.
- Draw mode group (Apply Draw Mode, Draw Mode, Draw Mode Color, Card Geometry, the six card textures): all schema attributes of GeomModelAPI; USDEditPrim with the GeomModelAPI schema. The tooltips say so.
- Type (retyping a prim) and Apply API Schemas: both change what the prim *is*, which is USDEditPrim's schema choice (Define applies the type, API schemas apply with their attributes). Not metadata of the kind this node edits.
- Asset Thumbnail: needs `UsdMediaAssetPreviewsAPI`, which Natron's USD build is not known to ship; no milestone. Free asset-info rows cover a path stored by hand.
- Icon, Catalog Parent Path, Allow LOPs to Edit This Primitive, Allow Viewport Selection: Houdini-specific attributes and custom data keys with no Natron consumer; no milestone.
- Metadata on properties (attributes, relationships): Solaris splits it to Configure Properties; no milestone.
- Layer-level metadata: M565 - USDConfigureLayer Node (and M449's region node).
- Clearing existing metadata: an untouched field authors nothing, and USD offers no "erase" in an overriding layer except the explicit empty values the controls above allow; no separate Clear control.

**Edge cases:**
- Pattern matches nothing: the scene passes through with a node warning via `describeNoMatch`; blank pattern passes through silently, like USDVariant.
- No field toggled: nothing authored, the output scene is the input, no warning (the toggles are the opt-in).
- Fix Kind Hierarchy never changes an ancestor already `group` or `assembly`, never touches the configured prim itself, and does nothing for a configured prim whose kind is none or `subcomponent`.
- `subcomponent` may sit under a component, so it is not a model kind for Fix Kind Hierarchy.
- Active false on a prim the input defines inside a reference or payload: authored as an override on the stage path; USD composes it. Prims below an inactive prim are not matched by later nodes (they vanish from traversal); the node warns when a matched prim is already below an inactive ancestor.
- Instanceable on a prim with no reference or payload: authored; it has no effect until one composes there, and the node does not warn (as USD does not).
- A configured prim that is inside an unloaded payload: the pattern never loads payloads; a plain-path pattern below an unloaded payload is authored on the path per `matchPattern`'s rule.
- Specifier Define over an input prim that already has a type: authors `def` with no type, which composes as the same prim with its existing type (USD keeps the stronger typeName); a node warning notes that Define does not retype. Specifier Class authors a class spec, which composes out of the traversed scene: the node warns that the prim disappears from traversal.
- Custom data row names that repeat, are empty or contain invalid characters: node error naming the row (as M449's `USDConfigureLayer` does); type parse failures name the row and type.
- Asset Dependencies parse: one asset path per line, blank lines ignored; relative paths are anchored as `SceneNodeAuthoring::getAnchoredFilePath` does for other nodes, because the layer is anonymous.
- No backward compatibility: new node, nothing imported.

**Assumptions:**
- `SdfPrimSpec::SetKind`, `SetActive`, `SetInstanceable`, `SetHidden`, `SetSpecifier`, `SetAssetInfo` and `SetCustomData` are enough to author every field in the node's anonymous layer; T1 proves each against composed `UsdPrim` metadata.
- `KnobLayerCustomData` from M449 can be reused as-is for prim custom data and asset info (it holds name, type, value rows with no layer-specific meaning). If it carries layer-specific code, T1 of the table task lifts it into a shared header.
- The scene-graph panel's hidden filter (M556 - Scene Graph Bulk Commands, M557) reads `hidden` metadata from the composed stage, so a Hidden edit shows there with no extra work. Unverified; T6 checks it in the GUI script.

## Phase 564.1: The node

- [ ] M564.P1.T1 — Node skeleton, pattern, simple metadata fields
  - files: `Engine/Nodes/Scene/USDConfigurePrim.{h,cpp}`, `Engine/Nodes/Scene/USDConfigurePrim.reg.inc`, `Engine/Nodes/Scene/CMakeLists.txt` (or the file lists the other Scene nodes use), `Gui/Resources/Images/NativeNodes/USDConfigurePrim.png`, `Tests/Scene/ConfigurePrim_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: a scene node on the USDVariant pattern: one Scene input, `PrimSelector` Prim Pattern, and knobs `kind`, `active`, `instanceable`, `hidden`, `specifier` each in an `AuthoredKnobSet` (companion toggles), with tooltips that say what is and is not here (purpose and draw mode live in USDEditPrim). `renderScene` resolves the pattern (`matchPattern`), and `emitSceneLayer` authors, for each match, `SdfCreatePrimInLayer(layer, path)` with the specifier set and `SetKind`/`SetActive`/`SetInstanceable`/`SetHidden` for the toggled fields only; the facts digest holds the matched paths and toggled values. Registered under `PLUGIN_GROUP_USD_Edit`. Knob labels are the readable script names.
  - verify: gtest on in-memory stages: each field alone authors only its metadata (composed `GetKind`, `IsActive`, `IsInstanceable`, `IsHidden`, `GetSpecifier` match; an untouched field leaves the input's value); no toggle authors nothing; an empty-match pattern warns and passes through; Kind None blocks a weaker `component`.
  - size: L

- [ ] M564.P1.T2 — Table knob reuse and Custom Data
  - files: `Engine/Nodes/Scene/USDConfigurePrim.cpp`, `Engine/Nodes/Scene/CustomDataRows.{h,cpp}` (shared parser, extracted from `USDConfigureLayer.cpp` if M449 left it inline), `Tests/Scene/ConfigurePrim_Test.cpp`
  - approach: add the `customData` table knob (`KnobLayerCustomData`); parse rows into a `VtDictionary` with the type rules M449 defined (arrays split on whitespace, String Array respects quotes, Token Array does not); names with colons nest; author with `SdfPrimSpec::SetCustomData` after merging with nothing (the node authors only its rows, USD dictionaries compose key-wise over the input). Row errors name the row.
  - verify: gtest: one row of each type lands as the expected `VtValue`; `a:b` nests and composes key-wise with an input `customData["a"]["c"]`; duplicate and empty names and an integer row holding text are errors naming the row.
  - size: M

- [ ] M564.P1.T3 — Asset Info
  - files: `Engine/Nodes/Scene/USDConfigurePrim.cpp`, `Tests/Scene/ConfigurePrim_Test.cpp`
  - approach: add the four fixed fields (identifier as an asset path knob, name, version, dependencies as a multi-line string), each with its toggle, and an `assetInfo` table knob of extra entries; author into `SetAssetInfo` under `identifier`, `name`, `version`, `payloadAssetDependencies` (an `SdfAssetPathArray`) and the extra keys. Anchor relative asset paths like other nodes.
  - verify: gtest: the composed `UsdModelAPI(prim).GetAssetIdentifier/GetAssetName/GetAssetVersion/GetPayloadAssetDependencies` return the values; an extra row lands in `assetInfo`; untouched fields leave the input's asset info key-wise.
  - size: M

- [ ] M564.P1.T4 — Fix Kind Hierarchy
  - files: `Engine/Nodes/Scene/USDConfigurePrim.cpp`, `Tests/Scene/ConfigurePrim_Test.cpp`
  - approach: implement the rule in Scope as a pure function over the composed stage and the matched paths (`UsdModelAPI::IsModel`/`GetKind`, `KindRegistry::IsA`): for each configured prim with a model kind, walk its ancestors and give `group` to those whose composed kind is not `group` or `assembly`; author the ancestors' `kind` as `over`s.
  - verify: gtest on `/A/B/C` with C `component`: A and B (no kind) become `group`; an ancestor already `assembly` is untouched; a C of kind `subcomponent` or none changes nothing; with the toggle off nothing extra is authored. The table of cases is written into the test first.
  - size: M

- [ ] M564.P1.T5 — Warnings for Define, Class, inactive ancestors
  - files: `Engine/Nodes/Scene/USDConfigurePrim.cpp`, `Tests/Scene/ConfigurePrim_Test.cpp`
  - approach: add the node warnings from Edge cases (Define does not retype, Class removes the prim from traversal, a matched prim below an inactive ancestor) through one `setPersistentMessage` composer so several apply together; clear when none.
  - verify: gtest on the node's message for each case and for a clean configuration (message cleared).
  - size: M

## Phase 564.2: Docs and checks

- [ ] M564.P2.T1 — Params-to-knob audit
  - files: `Tests/Scene/ConfigurePrim_Test.cpp`
  - approach: a test table of every Solaris Configure Primitives control listed in `## Scope` (In and Out) against the built node's knobs: each In control has a knob with the documented script name and a companion toggle; each Out control has no knob. Failure names the control.
  - verify: the audit test passes.
  - size: M

- [ ] M564.P2.T2 — Node doc page
  - files: `Engine/Nodes/Scene/docs/USDConfigurePrim.md`
  - approach: document the node: only toggled fields are authored; each field's USD meaning; the model-hierarchy rule behind Kind and Fix Kind Hierarchy; custom data and asset info rows; what is deliberately elsewhere (purpose, visibility, draw mode, API schemas, relationships: USDEditPrim). No mention of other applications.
  - verify: a reviewer checks names and defaults against the knobs.
  - size: S

- [ ] M564.P2.T3 — Xvfb GUI script
  - files: `Tests/gui/usd_configureprim.py`
  - approach: under Xvfb at 1920x1080 build Cube -> USDConfigurePrim from Python; set Kind and Hidden, one custom-data row; assert the toggles turn on by editing the value and off by restoring it, that the composed stage carries the metadata, that the Scene Graph panel hides the prim when Hidden is set, and that a project save and reload keeps the toggles and rows. Screenshot of the node's page.
  - verify: the script passes in the remote `--gui` gate; the user OKs the screenshot (one file).
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; `ConfigurePrim_Test` (including the params-to-knob audit) passes; `Tests/gui/usd_configureprim.py` passes under Xvfb at 1920x1080; the user OKs the node-page screenshot.

## Decisions

- 2026-11-24 — **Prim metadata gets its own node (user):** "Shape A plus Configure Layer": USDConfigurePrim = prim metadata only (kind, active, instanceable, hidden, specifier, assetInfo, customData, plus the other metadata Solaris Configure Primitives exposes). Each field has an enable toggle so unset fields author nothing. Attributes stay in USDSetAttribute and USDEditPrim: purpose and drawMode are schema attributes reachable through USDEditPrim via Imageable and GeomModelAPI, and ConfigurePrim's tooltips point there.
- 2026-11-24 — Audit of Solaris Configure Primitives: the controls that are metadata are In; Sampling, Purpose, Proxy Primitive, Visibility, Set Extents, the Draw Mode group, Type, Apply API Schemas, Thumbnail and the Houdini-specific interface flags are Out with reasons in Scope (PM draft; the user's list of fields is In unchanged).
- 2026-11-24 — Depends on M449 - USD Layer Save Paths for the shared custom-data table knob (PM draft).
