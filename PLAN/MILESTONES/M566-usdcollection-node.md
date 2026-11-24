# M566 - USDCollection Node

A scene node that authors one named `UsdCollectionAPI` collection on a chosen prim, from prim patterns, so lights, materials, render settings and later nodes can name a set of prims once. It follows the Solaris Collection LOP. The authoring half already exists in part: `SceneNodeAuthoring::authorCollectionBinding()` (`Engine/Nodes/Scene/SceneNodeAuthoring.cpp` ~line 1687) applies a collection with `expandPrims` and sets its includes and excludes for a material binding; this milestone lifts that into a general `authorCollection()` and puts a node on it. The include and exclude knobs are the prim-pattern relationship knobs M562 - USDEditPrim Relationship Knobs adds through `SchemaKnobMapper` (`Engine/Nodes/Scene/SchemaKnobMapper.{h,cpp}`); the node reuses that machinery and does not build a second pattern-to-targets path. Prim patterns already evaluate collections in the Scene Graph (F37 work: `UsdGetCollectionPredicateLibrary()` in `Gui/SceneGraphModel.cpp` ~line 2973), so a collection this node authors is already usable there.

## Scope

**Solaris Collection LOP controls, each In or Out:**
- Create Primitive If Required: In, as `Create Prim If Missing`. Off, a missing target prim is an error.
- Default Primitive Path: Out. It only feeds the per-collection Primitive Path of a multi-collection node; this node has one collection and one `Prim Path`.
- Number of Collections: Out. One collection per node; chain nodes for several. The stage is the same either way, and a repeating row of collections is not part of this milestone.
- Enable (per collection): Out. The node's own disable does the same job for a single collection.
- Collection Name: In. A namespaced name such as `karma:traceset:roundedge` is allowed (the colons are the instance namespace), since trace-set collections are named that way. A blank name authors nothing and sets a node warning.
- Primitive Path: In (`Prim Path`).
- Add to Last Modified Primitives: Out. Natron has no last-modified-prims set for later nodes to read.
- Icon: Out. It feeds a collections panel the Scene Graph does not have.
- Expansion Rule: In, as a menu: No Expansion (`explicitOnly`), Expand Primitives (`expandPrims`, the default), Expand Primitives and Properties (`expandPrimsAndProperties`).
- Allow Instance Proxies in Collection: In, as `Include Instance Proxies`: pattern matching also visits prims inside native instances, so instance-proxy paths can become targets.
- Primitives: In (`Includes`, a prim pattern knob from M562).
- Pattern Is USD Path Expression: In. The Includes text is authored as the collection's `membershipExpression` instead of resolving it to targets; Excludes is then disabled, as in the reference node.
- Add Exclusions: In. Shows `Excludes`, a second prim pattern. Off, the collection's excludes are not written (an existing excludes relationship upstream is left alone).
- Exclude Primitives: In (`Excludes`).
- `%self` in the patterns: Out. Inverting a collection through its own members is not supported; use Include Root with Excludes instead.
- Trace sets: In through the namespaced name; the Karma trace-set semantics themselves are none of this node's business.

**In (beyond the controls):**
- Collection references in every node's prim patterns (user, 2026-11-24): `%name` and `<path>.collection:<name>` resolve against the stage through a stage evaluator in the selector path (`SceneOps::parsePrimExpression()`, `SceneOps::selectPrims()`, `PrimSelector`), so a collection this node authors can be named in USDSetAttribute, USDPrune, the M562 relationship knobs and every other node with a prim pattern.
- `Include Root` (`includeRoot`), a USD attribute the reference node lacks, so `includes = none, excludes = a few` ("everything except") is expressible.
- Replace semantics: if the prim already has a collection of this name on the upstream stage, this node's includes, excludes, expansion rule, root flag and expression replace the old ones.
- `SceneNodeAuthoring::authorCollection()` shared with `authorCollectionBinding()` (the binding keeps its behaviour) and with M567 - USD Light Linking.
- Docs `Engine/Nodes/Scene/docs/USDCollection.md`, toolbar entry in the USD Edit submenu (M520 - USD Toolbar Category And Submenus), icon, tests.

**Out:**
- Collections on non-prims (properties as members): none. `expandPrimsAndProperties` is exposed but no property picker is.
- A collections panel or tree view in the Scene Graph: none.
- Applying a material, light link or any other use of the collection: M567 - USD Light Linking and the material nodes do that.

**Edge cases:**
- Includes pattern matches nothing: the collection is authored with no includes (it is empty, which USD allows) and the node warns "<pattern> matched no prims".
- Excludes pattern matches prims the includes did not: harmless, authored as given.
- Target prim missing and Create Prim If Missing on: created as a typeless `def` (with its missing ancestors), as `USDScopePrim` creates ancestors.
- Name not a valid namespaced identifier: error naming it, nothing authored.
- Patterns resolve against the loaded stage only; prims inside unloaded payloads are not matched and no payload is loaded for it (the viewer rule).
- A pattern naming a collection that does not exist on the stage: matches nothing, with a warning naming it; a collection that refers to itself, directly or through others: matches nothing, with USD's error text.
- A collection reference inside an unloaded payload: not seen, no payload is loaded.
- Path-expression mode with text `SdfPathExpression` rejects: error with USD's message.
- A pattern resolved to targets goes stale if the upstream scene changes; the node re-resolves at every evaluation, as `USDSetAttribute` does.

**Assumptions:**
- Unverified: the bundled USD is new enough for `UsdCollectionAPI::GetMembershipExpressionAttr()` (the Scene Graph already uses `UsdObjectCollectionExpressionEvaluator`, which arrived with it). Task M566.P1.T1 checks it first.
- Unverified: `SchemaKnobMapper` can describe a multi-apply schema instance; if not, the Expansion Rule and Include Root knobs are built by hand and only Includes and Excludes come from M562's relationship machinery.

## Phase 566.1: Authoring core

- [ ] M566.P1.T1 — Lift collection authoring into `authorCollection()`
  - files: `Engine/Nodes/Scene/SceneNodeAuthoring.{h,cpp}`, `Tests/Scene/SceneNodeAuthoringCollection_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: a `SceneCollectionSpec { name, includes, excludes, expansionRule, includeRoot, membershipExpression (optional) }` and `authorCollection(UsdPrim, spec, std::string* error)` that applies `UsdCollectionAPI` with the (namespaced) instance name, replaces includes/excludes/expansion rule/`includeRoot`, and in expression mode authors `membershipExpression` and clears the relationships. `authorCollectionBinding()` calls it for its collection half and keeps its output byte for byte. First confirm `GetMembershipExpressionAttr()` exists in the bundled USD; if it does not, expression mode is dropped and the knob, doc line and Scope item go with it (record in Decisions).
  - verify: gtest on an in-memory stage — a namespaced name `karma:traceset:a`, `explicitOnly` and `expandPrims`, `includeRoot` on with excludes only, expression mode, replacing an existing collection, an invalid name returning false; the existing `authorCollectionBinding` tests still pass unchanged.
  - size: M

- [ ] M566.P1.T2 — Resolve patterns for a collection, instance proxies optional
  - files: `Engine/Nodes/Scene/SceneNodeAuthoring.{h,cpp}`, `Engine/Nodes/Scene/SceneOps.{h,cpp}` (only if `selectPrims` needs the proxy flag threaded), `Tests/Scene/SceneNodeAuthoringCollection_Test.cpp`
  - approach: a helper over `SceneOps::selectPrims(..., traverseInstanceProxies)` returning the targets for the Includes and Excludes patterns, plus a count for the "matched no prims" warning. If M562's relationship-knob path already resolves patterns to `SdfPath` targets, call that instead and add only the instance-proxy pass-through.
  - verify: gtest — a pattern matching two prims, one matching nothing (empty, warning count 0 matches), and a stage with a native instance where the proxy flag adds the proxy prim only when on.
  - size: M

## Phase 566.2: The node

- [ ] M566.P2.T1 — USDCollection class and knobs
  - files: `Engine/Nodes/Scene/USDCollection.{h,cpp}`, `Engine/Nodes/Scene/USDCollection.reg.inc`, registration beside the other `.reg.inc` files
  - approach: follow `USDPrune`/`USDEditPrim`: `NativeEffectBase` scene node. Knobs (labels equal the readable script names): Collection Name (`collectionName`), Prim Path (`primPath`, a prim picker as the other nodes), Create Prim If Missing (`createPrimIfMissing`), Expansion Rule (`expansionRule`), Include Root (`includeRoot`), Includes (`includes`, M562 pattern knob), Add Exclusions (`addExclusions`), Excludes (`excludes`, shown by Add Exclusions), Pattern Is Path Expression (`patternIsPathExpression`, which disables Excludes), Include Instance Proxies (`includeInstanceProxies`). Scene evaluation applies `authorCollection()` in the node's layer edit; warnings through the node's warning channel; patterns never load payloads. Includes and Excludes share M562's knob helper.
  - verify: builds; a gtest builds Sphere-prim scene → USDCollection and checks the composed `collection:<name>:includes` targets, the expansion rule and `includeRoot`.
  - size: L

- [ ] M566.P2.T2 — Docs, toolbar entry and icon
  - files: `Engine/Nodes/Scene/docs/USDCollection.md`, `Gui/Resources/Images/NativeNodes/USDCollection.png`, the node's `kNodeSpec.grouping`
  - approach: docs in the style of `USDScopePrim.md` (what it authors, every knob, the edge cases above, and that other nodes' patterns can name the collection); no reference-app names. Grouping is the USD Edit submenu `PLUGIN_GROUP_USD_Edit` (M520); if M520 has not landed, `PLUGIN_GROUP_3D_USD`.
  - verify: the node appears in the toolbar submenu; the docs build step passes (`tools/ci` docs check, as other nodes); `docs/USDCollection.md` names every knob.
  - size: M

- [ ] M566.P2.T3 — Params-to-knob audit
  - files: `Engine/Nodes/Scene/USDCollection.cpp`, the milestone file (a checked table)
  - approach: walk the Solaris Collection LOP parameter list in Scope against the built knobs and the docs; every In control has a knob and a doc line, every Out control has a reason in the docs only if it would surprise a user. Fix gaps found.
  - verify: the table in the PR body shows each control mapped; no gap left.
  - size: S

## Phase 566.3: Collection references in prim patterns

- [ ] M566.P3.T1 — Parse and evaluate collection references
  - files: `Engine/Nodes/Scene/SceneOps.{h,cpp}`, `Tests/Scene/SceneOpsCollectionRef_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: `parsePrimExpression()` stops rejecting references: it accepts `%name` and `<path>.collection:<name>` (the second is rewritten to USD's expression reference `%<path>.collection:<name>`), and says in its header comment that references need a stage. `selectPrims()` evaluates with a stage evaluator (`UsdObjectCollectionExpressionEvaluator` with the collection predicate library, as `Gui/SceneGraphModel.cpp` ~line 2973 does) so references resolve through `UsdCollectionAPI` membership, expression-mode collections included; the cancellable overload polls as before. First check what the bundled USD's `SdfPathExpression` grammar accepts for a reference; if bare `%name` is not USD grammar, `%name` is taken as the collection of that name on the first prim of the stage (stage order) that has it, and docs say so. A selector with no stage (`parsePrimExpression` alone) still reports that a reference needs a stage.
  - verify: gtest — a collection by path form and by `%name`, one authored by `authorCollection()` in expression mode, a nested reference, a missing collection (no match, warning text), a self-referencing pair (no match, error text), set operators mixing a reference with plain paths; the existing selector tests are unchanged.
  - size: L

- [ ] M566.P3.T2 — Selector consumers and caches
  - files: `Engine/Nodes/Scene/PrimSelector.{h,cpp}`, `Engine/Nodes/Scene/SceneOps.cpp` (`matchDrawRules`, `drawRulesNeedStage`), the nodes that call `parsePrimExpression()` directly
  - approach: audit every caller of `parsePrimExpression()` and `selectPrims()`: pattern validation that ran without a stage (knob error display, `drawRulesNeedStage()`) treats a reference as needing a stage; any per-node memo keyed on pattern text alone is also keyed on the stage's content hash when the text holds a reference, so editing a collection re-selects. Fix each caller found.
  - verify: gtest — a USDPrune pattern naming a collection prunes the right prims, and editing the collection (upstream USDCollection knob) changes the result without touching the pattern; `drawRulesNeedStage()` is true for a rule with a reference.
  - size: M

- [ ] M566.P3.T3 — Docs for patterns and a GUI check
  - files: `Engine/Nodes/Scene/docs/` (the page or pages that describe prim patterns; USDCollection.md), `Tests/gui/usd_collection_refs.py`
  - approach: document both reference forms, their requirement of a stage, the missing and self-referencing cases. The Xvfb script builds Sphere/Cube → USDCollection → USDPrune with the pattern `%name`, asserts the pruned prims, edits the collection's Includes knob and asserts the viewer and Scene Graph update (a knob-driven refresh); screenshot to the user.
  - verify: the script passes under Xvfb at 1920x1080; the user OKs the screenshot.
  - size: M

## Phase 566.4: Tests

- [ ] M566.P4.T1 — Node tests and an Xvfb GUI check
  - files: `Tests/Scene/USDCollection_Test.cpp`, `Tests/gui/usd_collection.py`, `Tests/CMakeLists.txt`
  - approach: gtests for every Edge case (no match, missing prim with and without Create, bad name, expression mode, replace, instance proxies, unloaded payload not matched). The GUI script builds Sphere/Cube → USDCollection, types a pattern, asserts the composed collection, then types `<prim>.collection:<name>` in the Scene Graph filter and asserts the same prims are listed; edits the Includes text and asserts the collection updates (a knob-driven refresh, so ctest alone is not enough).
  - verify: gtest passes; the script passes under Xvfb at 1920x1080; a screenshot of the node's panel and the Scene Graph filter result goes to the user.
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; `SceneNodeAuthoringCollection_Test`, `SceneOpsCollectionRef_Test` and `USDCollection_Test` pass and the existing `authorCollectionBinding` tests are unchanged; `Tests/gui/usd_collection.py` and `Tests/gui/usd_collection_refs.py` pass under Xvfb at 1920x1080; the user OKs the node-panel screenshot.

## Decisions

- 2026-11-24 — Collection node is its own milestone ahead of light linking: M567 - USD Light Linking depends on it for the authoring helper and on M562 for the pattern knobs. (planner)
- 2026-11-24 — **Collection references fold in (user):** other nodes' prim patterns resolve `%name` and `<path>.collection:<name>` through a stage evaluator in the selector path (`parsePrimExpression` and `selectPrims`); Phase 566.3 does it.
