# M52 - USD Layer-Stack Completion

Full title: USD layer-stack completion — schema-driven edits, path expressions, composition nodes

Stage 1 of the 3D roadmap (`PLAN/DESIGN/2026-09-18-3d-roadmap-lops-paint-sops.md`,
Question 1). M19/M20 built the scene substrate and a hand-curated node
set; this milestone adds the three pieces that make the rest of USD's
surface reachable without a node per attribute: `SdfPathExpression` prim
selection shared by every pattern knob, authored-only knob semantics, and
a schema-driven `USDEditPrim`. Then the small set of composition nodes
that are one Sdf call each. Requires M20's gate and M226 - Scene Hierarchy From Topology (node renames, re-parenting); part of the 3D MVP (user, 2026-10-26). All nodes are
`NativeEffectBase` subclasses in `Engine/Nodes/Scene/`, one anonymous
layer each via `SceneOps`, memoized per M19.P1.T3, registered in
`AppManager::loadBuiltinNodePlugins()` under `PLUGIN_GROUP_3D`.

## Phase 52.1: Selection and authored-only substrate

- [x] M52.P1.T1 — `PrimSelector`: one path-expression knob helper for every scene node
  - files: `Engine/Nodes/Scene/PrimSelector.h`/`.cpp`, `Engine/Nodes/Scene/SceneOps.h`/`.cpp`, `Tests/PrimSelector_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: a `KnobString` wrapper whose value is an `SdfPathExpression` (USD ≥ 23.11), plus `SceneOps::selectPrims(composedStage, expression, time) → vector<SdfPath>` evaluated over a stage traversal. Supports the expression language as shipped (`/World/geo//*`, `//*Light*`, set operators); an invalid expression sets a persistent node error naming the parse failure rather than matching nothing silently. Pxr types stay behind the pimpl.
  - verify: unit tests against the M19.P2.T4 reference `.usda` — absolute path, wildcard, descendant and union expressions each return the expected prim set; an invalid expression reports an error string.
  - size: M

- [x] M52.P1.T2 — Migrate the scene nodes' pattern knobs onto `PrimSelector`
  - files: `Engine/Nodes/Scene/USDEditTransform.cpp`, `Engine/Nodes/Scene/USDPrune.cpp`, `Engine/Nodes/Scene/USDMaterial.cpp`, `Engine/Nodes/Scene/USDProjectUV.cpp` and `USDProject3D.cpp` if M234 - Camera Projection Nodes has landed, `Tests/Scene/SceneIntegration_Test.cpp`
  - approach: replace each node's "path or wildcard" knob with `PrimSelector`, keeping the knob's script name and the prim-path hint that M231 - Scene Graph Panel's drag and M232 - Scene Selection To Nodes' picker use. Today's plain-path, `*` and space-separated forms are valid `SdfPathExpression`s and must behave identically. No compatibility with older projects (fork rule).
  - verify: the scene integration test still passes unchanged; a space-separated two-path pattern written by the picker selects both prims.
  - size: M

- [x] M52.P1.T3 — `AuthoredKnobSet`: only knobs the user touched write opinions
  - files: `Engine/Nodes/Scene/AuthoredKnobSet.h`/`.cpp`, `Tests/AuthoredKnobSet_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: helper owning a group of value knobs, each paired with a hidden-by-default `KnobBool` "author" companion (Solaris' per-parameter enable). The companion flips true on the first user edit of its value knob (hook `onKnobValueChanged` with a user-edit reason), is shown as a small toggle beside the knob, and is serialized like any knob. `forEachAuthored(fn)` visits only enabled pairs; resetting a value knob to default clears its companion. Animated knobs with the companion set author time samples per M19.P1.T3.
  - verify: unit test — a set with three knobs where one is edited reports exactly one authored knob; the state round-trips through project serialization; a reset-to-default clears it.
  - size: M

## Phase 52.2: Schema-driven edit node

- [x] M52.P2.T1 — `SchemaKnobMapper`: schema attribute definitions → knob descriptions
  - files: `Engine/Nodes/Scene/SchemaKnobMapper.h`/`.cpp`, `Tests/SchemaKnobMapper_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: for a typed or applied-API schema name, walk `UsdSchemaRegistry`'s prim definition and emit a list of `(attrName, knobType, dimension, default, allowedTokens, doc)`. Mapping table: bool→`KnobBool`; int/uint→`KnobInt`; float/double/half→`KnobDouble`; float2/3/4, double2/3/4→`KnobDouble` dims 2–4; color3f/color4f→`KnobColor`; token with `allowedTokens`→`KnobChoice`, token without→`KnobString`; string→`KnobString`; asset→`KnobFile`; matrix/quat/array-valued attributes and relationships are **skipped** and returned in a separate "unsupported" list (arrays are geometry data — M54). Also lists the schema's typed and applied-API names for a chooser.
  - verify: unit test — `UsdGeomCamera` maps to knobs for focalLength, horizontalAperture, clippingRange (2-dim) and projection (choice with `perspective`/`orthographic`); `UsdGeomMesh` reports `points`/`faceVertexIndices` in the unsupported list.
  - size: M

- [x] M52.P2.T2 — `USDEditPrim` node
  - files: `Engine/Nodes/Scene/USDEditPrim.h`/`.cpp`, `Engine/AppManager.cpp` (registration), `Tests/EditPrim3D_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: inputs: Scene. Knobs: `PrimSelector`, a schema chooser (`KnobChoice` from M52.P2.T1's list, typed schemas and applied APIs), then one `KnobPage` of knobs built by `SchemaKnobMapper` inside an `AuthoredKnobSet`, rebuilt when the schema knob changes (knobs for the previous schema are removed; serialized values for the current schema are restored). Render: for each selected prim, author exactly the authored knobs' values as attribute opinions in the node's layer; an applied-API schema also authors the `apiSchemas` listOp. Never creates prims (that is `USDPrim`). Skipped attributes are listed in the node's help text.
  - verify: unit test — over the reference camera prim, editing only focalLength yields an exported layer whose only opinion under that prim is `focalLength`; switching schema to `CollectionAPI` and setting one field authors the apiSchemas entry plus that field. Manual: property panel with `UsdGeomMesh` selected stays responsive.
  - size: L

- [ ] ~~M52.P2.T3 — `USDPrim` gains a schema type~~ — cancelled 2026-10-26: `USDPrim` is retired by M226 - Scene Hierarchy From Topology; typed prim creation moves to M52.P2.T4.

- [x] M52.P2.T4 — `USDEditPrim` can define the prims it targets
  - files: `Engine/Nodes/Scene/USDEditPrim.{h,cpp}`, `Tests/EditPrim3D_Test.cpp`
  - approach: add a Define mode (Solaris Primitive LOP). The node takes a prim path (instead of the selector) plus the chosen typed schema, defines the prim (missing ancestors as Xform) and authors the touched attributes through the same `AuthoredKnobSet` page. The Edit mode stays the default. This is the route for arbitrary typed prims now that the hand-made hierarchy nodes are only `USDXformPrim`/`USDScopePrim`.
  - verify: unit test — Define with type `SphereLight` at `/rig/key` authors a SphereLight with an Xform `/rig`, and only touched attributes carry opinions; Edit mode behaves as P2.T2's test expects.
  - size: M

## Phase 52.3: Composition nodes

- [x] M52.P3.T1 — `USDReference`
  - files: `Engine/Nodes/Scene/USDReference.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/Reference3D_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: inputs: Scene (target stack), optional Scene (source stack). Knobs: target prim path, mode `reference | payload`, source = file path *or* the source input's composed default prim (the input stack's strongest layer is referenced by its anonymous identifier, which is valid in-process), `instanceable` bool. Authors the reference/payload listOp on the target prim in the node's layer. Payloads are loaded by default at consumers.
  - verify: unit test — referencing the reference `.usda` under `/World/ref` composes its prims there; referencing a second `USDRead` input does the same with no file involved; `instanceable` shows on the composed prim.
  - size: M

- [x] M52.P3.T2 — `USDVariant`
  - files: `Engine/Nodes/Scene/USDVariant.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/Variant3D_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: inputs: Scene. Knobs: `PrimSelector`, variant set (`KnobChoice` populated from the composed input's variant sets on the first selected prim, plus free text), variant name (same, from that set's variants). Authors the variant selection on each selected prim. Populating the choices reads the composed input stage through `StageCache`; a selection naming a set the prim lacks is authored anyway (USD ignores it) but flagged in the node's info message.
  - verify: unit test — a hand-authored `.usda` with a `shadingVariant` set: selecting `red` vs `blue` changes the composed material binding; choice knobs list both variants.
  - size: M

- [x] M52.P3.T3 — `USDSetAttribute`
  - files: `Engine/Nodes/Scene/USDSetAttribute.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/SetAttribute3D_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: inputs: Scene. Knobs: `PrimSelector`, attribute name, value type (`KnobChoice` over the scalar/vector types `SchemaKnobMapper` supports, plus `token`), value knob rebuilt per type, `primvar` bool (authors `primvars:<name>` with `constant` interpolation), animated → time samples. The no-Python escape hatch for anything `USDEditPrim` can't reach: custom attributes, primvars, arbitrary metadata-free opinions.
  - verify: unit test — authoring `primvars:displayColor` as color3f on the reference mesh composes a constant primvar; a custom `float` attribute round-trips through `USDWrite`.
  - size: M

- [x] M52.P3.T4 — Extend the scene vocabulary integration test
  - files: `Tests/` (M20.P3.T3's test), a second small reference `.usda` with a variant set
  - approach: extend the headless graph with `USDReference` → `USDVariant` → `USDEditPrim` → `USDSetAttribute` → `USDWrite`, asserting the exported composition: referenced prims present, selected variant applied, only the authored attribute opinions present, custom attribute present.
  - verify: test green in `build-and-test`.
  - size: M

**Verification gate:** CI green including M52.P3.T4; `USDEditPrim` over a camera prim exports exactly the touched attributes and nothing else; `USDVariant` switches a variant visible in the Viewer's 3D view on real hardware; every scene pattern knob is a `PrimSelector`; `USDEditPrim` Define mode creates a typed prim; pre-existing ctest suite green.

## Decisions

- 2026-10-28: Phase 3 implemented (build pending). USDReference: Source Prim knob added (blank = default prim, else first root prim for the input); missing file/source passes through with a warning; input mode references a stack layer. USDVariant: saved text knobs for set/variant names with unsaved pick-lists that fill them (a choice can't hold an unlisted name). USDSetAttribute: 27 value types, primvars as constant one-element arrays, schema-defined attributes must use the schema's type (non-custom), others custom. Placeholder icons (copies).
- 2026-10-28: Phase 2 implemented (build pending). Expressions mark AuthoredKnobSet knobs authored (expressionChanged). SchemaKnobMapper lists typed and applied API schemas by plug-in; attributes map to knobs (arrays, matrices, quaternions, relationships listed, no knob). USDEditPrim: schema menu grouped by plug-in with None, saves the schema name; knob names = attribute name with ':'→'_' minus the multi-apply instance (clash → usd_ prefix); multi-apply needs Instance Name; API schemas always applied to matched prims; values written raw (no unit conversion). Define mode creates missing ancestors as Xforms, abstract schema is an error. Node::fromSerialization tolerates knobs created/deleted during load (isRestoringKnobs). Open: undoing a schema change loses the old schema's values; should Define mode also offer transform knobs?
- 2026-10-28: Phase 1 implemented (build pending). Prim selection uses USD's path-expression evaluator in stage order; inactive prims excluded; blank selects nothing; collection references (%) rejected; an invalid pattern is a persistent node error. AuthoredKnobSet: per-knob `<scriptName>Authored` toggle, off by default and visible as an unlabelled leading toggle (Solaris-style; reading of "hidden-by-default"); user/GUI/script edits turn it on; reset to default turns it off. PM call: setting an expression should also mark the knob authored (phase 2 follow-up).
- 2026-10-25 — **Long-lived USD scene (bosley-native-nodes, from the review in `PLAN/DESIGN/2026-10-25-usd-long-lived-scene-review.md`):** selectPrims and USDVariant read their upstream through M19 - USD/Hydra Foundation's query-stage pool, not StageCache. USDReference must not reference only the strongest anonymous layer (that drops the weaker ones): reference a stack layer that sublayers the whole input, and handle it in USDWrite.
- 2026-10-03 — **USD Python coexistence spike (user decision):** after M52's gate, run a time-boxed spike testing whether pxr Python bindings can load into Natron's Shiboken6/PySide6 interpreter (M19 builds USD with Python OFF). A Solaris-style Python-over-the-stage node depends on the outcome; `USDSetAttribute` (P3.T3) stays the no-Python substitute meanwhile. Add the spike as a trailing task when M52 is promoted.
- 2026-10-26 — **M52 joins the 3D MVP and follows M226 (user, M19/M20 UAT grill):** USDTransform is now USDEditTransform and USDPrim is retired, so P1.T2 migrates the renamed nodes and P2.T3 is replaced by a Define mode on USDEditPrim (P2.T4) — typed prims come through the schema-driven node rather than a generic USDAddPrim (user chose Xform + Scope only).
