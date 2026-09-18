# Milestone 52: USD layer-stack completion — schema-driven edits, path expressions, composition nodes

Stage 1 of the 3D roadmap (`PLAN/DESIGN/2026-09-18-3d-roadmap-lops-paint-sops.md`,
Question 1). M19/M20 built the scene substrate and a hand-curated node
set; this milestone adds the three pieces that make the rest of USD's
surface reachable without a node per attribute: `SdfPathExpression` prim
selection shared by every pattern knob, authored-only knob semantics, and
a schema-driven `EditPrim3D`. Then the small set of composition nodes
that are one Sdf call each. Requires M20's gate. All nodes are
`NativeEffectBase` subclasses in `Engine/Nodes/Scene/`, one anonymous
layer each via `SceneOps`, memoized per M19.P1.T3, registered in
`AppManager::loadBuiltinNodePlugins()` under `PLUGIN_GROUP_3D`.

## Phase 52.1: Selection and authored-only substrate

- [ ] M52.P1.T1 — `PrimSelector`: one path-expression knob helper for every scene node
  - files: `Engine/Nodes/Scene/PrimSelector.h`/`.cpp`, `Engine/Nodes/Scene/SceneOps.h`/`.cpp`, `Tests/PrimSelector_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: a `KnobString` wrapper whose value is an `SdfPathExpression` (USD ≥ 23.11), plus `SceneOps::selectPrims(composedStage, expression, time) → vector<SdfPath>` evaluated over a stage traversal. Supports the expression language as shipped (`/World/geo//*`, `//*Light*`, set operators); an invalid expression sets a persistent node error naming the parse failure rather than matching nothing silently. Pxr types stay behind the pimpl.
  - verify: unit tests against the M19.P2.T4 reference `.usda` — absolute path, wildcard, descendant and union expressions each return the expected prim set; an invalid expression reports an error string.
  - size: M

- [ ] M52.P1.T2 — Migrate M20's pattern knobs onto `PrimSelector`
  - files: `Engine/Nodes/Scene/Transform3D.cpp`, `Engine/Nodes/Scene/Prune3D.cpp`, `Engine/Nodes/Scene/Material3D.cpp`, `Tests/` (the M20.P3.T3 scene vocabulary test)
  - approach: replace each node's "path or wildcard" knob with `PrimSelector`; knob script-name unchanged so M20-era projects load. Behaviour for the old plain-path and `*` forms must be identical (they are valid `SdfPathExpression`s).
  - verify: M20.P3.T3 integration test still green unchanged; a project saved with M20's Transform3D loads and composes the same override.
  - size: S

- [ ] M52.P1.T3 — `AuthoredKnobSet`: only knobs the user touched write opinions
  - files: `Engine/Nodes/Scene/AuthoredKnobSet.h`/`.cpp`, `Tests/AuthoredKnobSet_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: helper owning a group of value knobs, each paired with a hidden-by-default `KnobBool` "author" companion (Solaris' per-parameter enable). The companion flips true on the first user edit of its value knob (hook `onKnobValueChanged` with a user-edit reason), is shown as a small toggle beside the knob, and is serialized like any knob. `forEachAuthored(fn)` visits only enabled pairs; resetting a value knob to default clears its companion. Animated knobs with the companion set author time samples per M19.P1.T3.
  - verify: unit test — a set with three knobs where one is edited reports exactly one authored knob; the state round-trips through project serialization; a reset-to-default clears it.
  - size: M

## Phase 52.2: Schema-driven edit node

- [ ] M52.P2.T1 — `SchemaKnobMapper`: schema attribute definitions → knob descriptions
  - files: `Engine/Nodes/Scene/SchemaKnobMapper.h`/`.cpp`, `Tests/SchemaKnobMapper_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: for a typed or applied-API schema name, walk `UsdSchemaRegistry`'s prim definition and emit a list of `(attrName, knobType, dimension, default, allowedTokens, doc)`. Mapping table: bool→`KnobBool`; int/uint→`KnobInt`; float/double/half→`KnobDouble`; float2/3/4, double2/3/4→`KnobDouble` dims 2–4; color3f/color4f→`KnobColor`; token with `allowedTokens`→`KnobChoice`, token without→`KnobString`; string→`KnobString`; asset→`KnobFile`; matrix/quat/array-valued attributes and relationships are **skipped** and returned in a separate "unsupported" list (arrays are geometry data — M54). Also lists the schema's typed and applied-API names for a chooser.
  - verify: unit test — `UsdGeomCamera` maps to knobs for focalLength, horizontalAperture, clippingRange (2-dim) and projection (choice with `perspective`/`orthographic`); `UsdGeomMesh` reports `points`/`faceVertexIndices` in the unsupported list.
  - size: M

- [ ] M52.P2.T2 — `EditPrim3D` node
  - files: `Engine/Nodes/Scene/EditPrim3D.h`/`.cpp`, `Engine/AppManager.cpp` (registration), `Tests/EditPrim3D_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: inputs: Scene. Knobs: `PrimSelector`, a schema chooser (`KnobChoice` from M52.P2.T1's list, typed schemas and applied APIs), then one `KnobPage` of knobs built by `SchemaKnobMapper` inside an `AuthoredKnobSet`, rebuilt when the schema knob changes (knobs for the previous schema are removed; serialized values for the current schema are restored). Render: for each selected prim, author exactly the authored knobs' values as attribute opinions in the node's layer; an applied-API schema also authors the `apiSchemas` listOp. Never creates prims (that is `Prim3D`). Skipped attributes are listed in the node's help text.
  - verify: unit test — over the reference camera prim, editing only focalLength yields an exported layer whose only opinion under that prim is `focalLength`; switching schema to `CollectionAPI` and setting one field authors the apiSchemas entry plus that field. Manual: property panel with `UsdGeomMesh` selected stays responsive.
  - size: L

- [ ] M52.P2.T3 — `Prim3D` gains a schema type
  - files: `Engine/Nodes/Scene/Prim3D.cpp`, `Tests/` (M20's Prim3D test)
  - approach: replace M20's "empty xform / simple prim" choice with the `SchemaKnobMapper` typed-schema list; the created prim gets that type, and its attributes are exposed through an `AuthoredKnobSet` exactly as in `EditPrim3D` (share the page-building code — factor it into `SchemaKnobMapper` or a small `SchemaKnobPage` helper if T2 didn't already).
  - verify: unit test — `Prim3D` with type `SphereLight` authors a prim of that type; with `Xform` (the default) behaves exactly as M20's test expects.
  - size: S

## Phase 52.3: Composition nodes

- [ ] M52.P3.T1 — `Reference3D`
  - files: `Engine/Nodes/Scene/Reference3D.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/Reference3D_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: inputs: Scene (target stack), optional Scene (source stack). Knobs: target prim path, mode `reference | payload`, source = file path *or* the source input's composed default prim (the input stack's strongest layer is referenced by its anonymous identifier, which is valid in-process), `instanceable` bool. Authors the reference/payload listOp on the target prim in the node's layer. Payloads are loaded by default at consumers.
  - verify: unit test — referencing the reference `.usda` under `/World/ref` composes its prims there; referencing a second `ReadScene` input does the same with no file involved; `instanceable` shows on the composed prim.
  - size: M

- [ ] M52.P3.T2 — `Variant3D`
  - files: `Engine/Nodes/Scene/Variant3D.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/Variant3D_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: inputs: Scene. Knobs: `PrimSelector`, variant set (`KnobChoice` populated from the composed input's variant sets on the first selected prim, plus free text), variant name (same, from that set's variants). Authors the variant selection on each selected prim. Populating the choices reads the composed input stage through `StageCache`; a selection naming a set the prim lacks is authored anyway (USD ignores it) but flagged in the node's info message.
  - verify: unit test — a hand-authored `.usda` with a `shadingVariant` set: selecting `red` vs `blue` changes the composed material binding; choice knobs list both variants.
  - size: M

- [ ] M52.P3.T3 — `SetAttribute3D`
  - files: `Engine/Nodes/Scene/SetAttribute3D.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/SetAttribute3D_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: inputs: Scene. Knobs: `PrimSelector`, attribute name, value type (`KnobChoice` over the scalar/vector types `SchemaKnobMapper` supports, plus `token`), value knob rebuilt per type, `primvar` bool (authors `primvars:<name>` with `constant` interpolation), animated → time samples. The no-Python escape hatch for anything `EditPrim3D` can't reach: custom attributes, primvars, arbitrary metadata-free opinions.
  - verify: unit test — authoring `primvars:displayColor` as color3f on the reference mesh composes a constant primvar; a custom `float` attribute round-trips through `WriteScene`.
  - size: S

- [ ] M52.P3.T4 — Extend the scene vocabulary integration test
  - files: `Tests/` (M20.P3.T3's test), a second small reference `.usda` with a variant set
  - approach: extend the headless graph with `Reference3D` → `Variant3D` → `EditPrim3D` → `SetAttribute3D` → `WriteScene`, asserting the exported composition: referenced prims present, selected variant applied, only the authored attribute opinions present, custom attribute present.
  - verify: test green in `build-and-test`.
  - size: S

**Verification gate:** CI green including M52.P3.T4; `EditPrim3D` over a camera prim exports exactly the touched attributes and nothing else; `Variant3D` switches a variant visible in `Viewport3D` on real hardware; every M20 pattern knob is a `PrimSelector` and M20-era projects still load; pre-existing ctest suite green.
