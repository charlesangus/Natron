# M381 - Deferred Matching Coverage

M331 - Deferred Pattern Matching moved some pattern nodes onto load sets. The rest still match against fully loaded stages. This milestone converts every remaining borrower so none builds a fully loaded stage on edit, and gives each a documented load-set owner.

## Scope

**In:**
- Every borrower listed in the phases below, a `SceneLoadSet::withPaths` closure for known-path reads, per-context graft and transform caches, a viewer-context accessor, and removal of the implicit everything `borrow()`.

**Out:**
- Search Unloaded UI: M334 - Knob Prim Tools.
- Bounds edits inside unloaded payloads (M331 - Deferred Pattern Matching known limit).
- The pattern language.

**Edge cases:**
- A camera or settings prim inside a payload: the closure loads one chain.
- A variant set defined inside a payload.
- A wrap target referenced from unloaded content: reach "/".
- No viewer showing the node: no load set plus the "unsearched" note.
- Two viewers with different sets: separate cache slots.

**Assumptions:**
- Every converted borrower adds its reach, or a load won't re-evaluate it (`ViewerInstance.cpp` ~:3655).
- Parity means "the same once opened with L".
- Load-set owner rule (user): a borrower uses the load set of the evaluation it serves; outside an evaluation, the viewer that asked or that shows the node; known-path readers load only the payloads on those paths.

## Phase 381.1: Probe and closure

- [x] M381.P1.T1 — Heavy-scene probe and parity helper
  - files: `Tests/Scene/SceneTestAuthoring.cpp`, new `Tests/Scene/SceneLoadSetCoverage_Test.cpp` (CarCity moved out of `Tests/Scene/ScenePatternContext_Test.cpp` ~:67), test CMake
  - approach: `openedBuildings()` counts the building files `SdfLayer::Find` returns. `expectParity(node, L)` evaluates once in a viewer context with L and once in the default context, opens both with `SceneOps::getLoadRules(L)`, and compares composed prims and properties.
  - verify: the probe reads 0 after `borrow(p, none)` and all buildings after `borrow(p)`; parity passes on USDPrune.
  - size: M

- [x] M381.P1.T2 — Load set for known-path reads
  - files: `Engine/SceneLoadSet.{h,cpp}`, `Tests/Scene/SceneQueryPool_Test.cpp`
  - approach: `SceneLoadSet::withPaths(base, paths)` = base plus `eRuleOnly` on each ancestor of each path; a null base stays null.
  - verify: `withPaths(none, {/World/Block_0/B_1/car_0})` opens one building, and that prim composes the same as with everything loaded.
  - size: M

## Phase 381.2: Pattern nodes

- [ ] M381.P2.T1 — USDSetAttribute matches on the load set
  - files: `Engine/Nodes/Scene/USDSetAttribute.cpp` (~:338-350), `Tests/Scene/SceneLoadSetCoverage_Test.cpp`
  - approach: go through `matchPattern`; the schema-type check runs only on prims that exist.
  - verify: on the city with nothing loaded, an edit opens 0 buildings; parity at L={Block_0}; the unsearched note shows.
  - size: M

- [ ] M381.P2.T2 — USDEditPrim on load sets
  - files: `Engine/Nodes/Scene/USDEditPrim.cpp` (~:460, ~:544), `Tests/Scene/SceneLoadSetCoverage_Test.cpp`
  - approach: Edit mode via `matchPattern`. Create mode's ancestor check borrows `withPaths(context.loadSet, {path})` and adds the path to the context's reach.
  - verify: both modes open 0 buildings with nothing loaded; parity at L.
  - size: M

- [ ] M381.P2.T3 — USDReference borrows only its paths
  - files: `Engine/Nodes/Scene/USDReference.cpp` (~:93, ~:320, ~:360), `Tests/Scene/SceneLoadSetCoverage_Test.cpp`
  - approach: `borrowStage` takes `withPaths(current, {path, sourcePrim})`.
  - verify: probe plus parity with the target inside a payload.
  - size: M

- [ ] M381.P2.T4 — USDVariant on the load set
  - files: `Engine/Nodes/Scene/USDVariant.cpp` (`renderScene` ~:294), `Tests/Scene/SceneLoadSetCoverage_Test.cpp`
  - approach: `matchPattern`; a literal match under an unloaded payload doesn't count as lacking the variant set.
  - verify: parity with a variant set authored inside the payload; probe reads 0.
  - size: M

- [ ] M381.P2.T5 — USDProjectUV on the load set
  - files: `Engine/Nodes/Scene/USDProjectUV.cpp` (~:835, ~:857, ~:920), `Tests/Scene/SceneLoadSetCoverage_Test.cpp`
  - approach: a named camera through `withPaths`; with no camera named, the context set plus reach "/". Meshes via `matchPattern`; the result key gains the load-set hash.
  - verify: probe, UV parity at L, a camera inside a payload resolves.
  - size: L

## Phase 381.3: Borrowers inside an evaluation

- [ ] M381.P3.T1 — Merge and graft on the evaluation context
  - files: `Engine/Nodes/Scene/SceneOps.cpp` (`readInput` ~:2145, `rootHoldsGprim` ~:2215, ~:2490), `Engine/Nodes/Scene/SceneLayerMemo.h` (`SceneGraftMemo` ~:158), `Tests/Scene/SceneLoadSetCoverage_Test.cpp`
  - approach: borrow with `SceneEvaluationContext::current()` and add reach for the link prims and the passed-through root. Read `getLinks(input)` on the context set with reach rather than through `withPaths`. `SceneGraftMemo` keeps one slot per context.
  - verify: USDMerge on the city opens 0 buildings; parity of grafts and unmapped links; a viewer graft and a render graft don't evict each other.
  - size: L

- [ ] M381.P3.T2 — `graftMaterial` on the evaluation context
  - files: `Engine/Nodes/Scene/SceneNodeAuthoring.cpp` (~:765), `Tests/Scene/SceneLoadSetCoverage_Test.cpp`
  - approach: the current context with reach of the grafted roots.
  - verify: probe plus parity on a material input that has a payload.
  - size: M

- [ ] M381.P3.T3 — Reparent wrap on the evaluation context
  - files: `Engine/Nodes/Scene/SceneReparentNode.cpp` (`renderWrap` ~:364, `gatherWrapRetargets` ~:113), `Tests/Scene/SceneLoadSetCoverage_Test.cpp`
  - approach: existence checks through `withPaths`; `gatherWrapRetargets` on the context set with reach "/".
  - verify: a relationship inside a payload is retargeted once that payload loads; probe reads 0.
  - size: M

- [ ] M381.P3.T4 — USDProject3D camera through the closure
  - files: `Engine/Nodes/Scene/USDProject3D.cpp` (`renderScene` ~:459), `Tests/Scene/SceneLoadSetCoverage_Test.cpp`
  - approach: named camera through `withPaths`; with none named, context set plus reach "/".
  - verify: a camera inside a payload resolves while only its building opens.
  - size: M

- [ ] M381.P3.T5 — USDRead metrics correction on the context
  - files: `Engine/Nodes/Scene/USDRead.cpp` (~:443), `Engine/Nodes/Scene/SceneOps.cpp` (~:1204), `Tests/Scene/SceneLoadSetCoverage_Test.cpp`
  - approach: the current context with reach "/".
  - verify: Match Up Axis on the city opens 0 buildings; parity at L, including a reset-stack prim inside a payload.
  - size: M

## Phase 381.4: Borrowers outside an evaluation

- [ ] M381.P4.T1 — USDRender reads only its settings chain
  - files: `Engine/Nodes/Scene/USDRender.cpp` (~:273, `readActiveSettings` ~:334, `deriveOutputMetadata` ~:552), `Tests/Scene/SceneLoadSetCoverage_Test.cpp`
  - approach: `readActiveSettings` reads `renderSettingsPrimPath` with none loaded, then `withPaths` on the settings prim and its camera target. The settings-prim menu uses `eRuleAll` on `/Render`. `deriveOutputMetadata` stays everything (render output); check which thread runs it and report a main-thread stall in `## Decisions`.
  - verify: `getRegionOfDefinition` on the city opens 0 buildings with unchanged resolution.
  - size: M

- [ ] M381.P4.T2 — Viewer evaluation context for transform handles
  - files: `Engine/ViewerInstance.h` (~:657), `Engine/ViewerInstancePrivate.h` (~:615), `Engine/Nodes/Scene/USDEditTransform.cpp` (`transformsPrim` ~:333, last-result fields ~:158-175), `Gui/SceneTransformTarget.cpp` (~:531), `Tests/Scene/SceneLoadSetCoverage_Test.cpp`
  - approach: `ViewerInstance::getSceneEvaluationContext()`; `transformsPrim(path, context)` is called with the asking viewer's context; matches cached per context.
  - verify: two contexts keep separate matches; the stale-match path opens 0 buildings.
  - size: M

- [ ] M381.P4.T3 — Centre Pivot as a cancellable worker job
  - files: `Engine/Nodes/Scene/USDEditTransform.cpp` (`centrePivot` ~:379), `Tests/Scene/SceneLoadSetCoverage_Test.cpp`
  - approach: runs on a worker with the showing viewer's context. Bounds from loaded prims plus authored `extentsHint` for unloaded payloads (never measured, per the design doc).
  - verify: centre equals the fully loaded centre when L covers the matches; probe reads 0; a second press cancels the first.
  - size: M

- [ ] M381.P4.T4 — Knob menus fill off the main thread
  - files: `Engine/Nodes/Scene/USDVariant.cpp` (~:237), `Engine/Nodes/Scene/USDProjectUV.cpp` (~:716), `Engine/Nodes/Scene/USDProject3D.cpp` (~:317), `Engine/Nodes/Scene/USDRenderSettings.cpp` (~:292), `Tests/Scene/SceneLoadSetCoverage_Test.cpp`
  - approach: today they run queued on the main thread and call `evaluateScene` with the default (everything) context. Evaluate on a worker with the showing viewer's context, add a disabled "N unloaded branches not searched" row, fill on the main thread.
  - verify: refresh returns before the worker ends; 0 buildings opened; the row count is right.
  - size: L

- [ ] M381.P4.T5 — No implicit everything borrow
  - files: `Engine/Nodes/Scene/SceneQueryPool.h` (~:106) and every remaining caller
  - approach: delete `SceneQueryPool::borrow(payload, err)`; every caller names its set, with an explicit everything plus a reason for USDWrite, render metadata and Search Unloaded. Add an owner table to the `SceneQueryPool.h` doc.
  - verify: `rg` finds no implicit borrow; syntax-check on the touched files; the P1–P4 suites pass.
  - size: M

**Verification gate:** release gate green with `SceneLoadSetCoverage_Test` (a no-full-load test and a parity test for every converted node). An Xvfb check that opening USDVariant and USDRenderSettings properties on the city keeps the main thread responsive. The city check, each node's viewer result agreeing with USDRender, goes to F4 - Heavy Scene Viewer's UAT.

## Decisions

- 2026-11-13 — Elaborated after the claim (consultant). The stub's list was confirmed and extended: USDEditPrim Create mode, USDReference, `SceneReparentNode::renderWrap`, `graftMaterial`, and four main-thread knob-menu refreshes that evaluate fully loaded. `SceneGraftMemo` and USDEditTransform's last result hold one state each, so they become per-context.
- 2026-11-13 — Load-set owner (user): a borrower uses the load set of the evaluation it serves (viewer, a Scene Graph panel's own, or everything for renders, USDWrite and scripts). Outside an evaluation it uses the viewer that asked or that shows the node: the most recently active viewer whose scene input reaches the node, else no load set plus the "N unloaded branches not searched" note. Known-path readers load only the payloads on those paths. Rejected: a showing panel supplies the set (zero or several panels, results depend on which is open); everything for menus and buttons (main-thread stalls).
- 2026-11-13 — P1.T1 implemented (578518139). `expectParity` composes fully loaded, so `openedBuildings()` is meaningless after a parity check in the same test.
