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

- [x] M381.P2.T1 — USDSetAttribute matches on the load set
  - files: `Engine/Nodes/Scene/USDSetAttribute.cpp` (~:338-350), `Tests/Scene/SceneLoadSetCoverage_Test.cpp`
  - approach: go through `matchPattern`; the schema-type check runs only on prims that exist.
  - verify: on the city with nothing loaded, an edit opens 0 buildings; parity at L={Block_0}; the unsearched note shows.
  - size: M

- [x] M381.P2.T2 — USDEditPrim on load sets
  - files: `Engine/Nodes/Scene/USDEditPrim.cpp` (~:460, ~:544), `Tests/Scene/SceneLoadSetCoverage_Test.cpp`
  - approach: Edit mode via `matchPattern`. Create mode's ancestor check borrows `withPaths(context.loadSet, {path})` and adds the path to the context's reach.
  - verify: both modes open 0 buildings with nothing loaded; parity at L.
  - size: M

- [x] M381.P2.T3 — USDReference borrows only its paths
  - files: `Engine/Nodes/Scene/USDReference.cpp` (~:93, ~:320, ~:360), `Tests/Scene/SceneLoadSetCoverage_Test.cpp`
  - approach: `borrowStage` takes `withPaths(current, {path, sourcePrim})`.
  - verify: probe plus parity with the target inside a payload.
  - size: M

- [x] M381.P2.T4 — USDVariant on the load set
  - files: `Engine/Nodes/Scene/USDVariant.cpp` (`renderScene` ~:294), `Tests/Scene/SceneLoadSetCoverage_Test.cpp`
  - approach: `matchPattern`; a literal match under an unloaded payload doesn't count as lacking the variant set.
  - verify: parity with a variant set authored inside the payload; probe reads 0.
  - size: M

- [x] M381.P2.T5 — USDProjectUV on the load set
  - files: `Engine/Nodes/Scene/USDProjectUV.cpp` (~:835, ~:857, ~:920), `Tests/Scene/SceneLoadSetCoverage_Test.cpp`
  - approach: a named camera through `withPaths`; with no camera named, the context set plus reach "/". Meshes via `matchPattern`; the result key gains the load-set hash.
  - verify: probe, UV parity at L, a camera inside a payload resolves.
  - size: L

## Phase 381.3: Borrowers inside an evaluation

- [x] M381.P3.T1 — Merge and graft on the evaluation context
  - files: `Engine/Nodes/Scene/SceneOps.cpp` (`readInput` ~:2145, `rootHoldsGprim` ~:2215, ~:2490), `Engine/Nodes/Scene/SceneLayerMemo.h` (`SceneGraftMemo` ~:158), `Tests/Scene/SceneLoadSetCoverage_Test.cpp`
  - approach: borrow with `SceneEvaluationContext::current()` and add reach for the link prims and the passed-through root. Read `getLinks(input)` on the context set with reach rather than through `withPaths`. `SceneGraftMemo` keeps one slot per context.
  - verify: USDMerge on the city opens 0 buildings; parity of grafts and unmapped links; a viewer graft and a render graft don't evict each other.
  - size: L

- [x] M381.P3.T6 — Inputs evaluate on their own context
  - files: `Engine/EffectInstance.h` (~:1759, ~:1815), `Engine/EffectInstanceRenderScene.cpp` (~:194-232), `Engine/SceneLoadSet.{h,cpp}`, `Tests/Scene/SceneQueryPool_Test.cpp`
  - approach: `getSceneInputContext`/`mapSceneInputReach` virtuals, identity by default; the input loop evaluates each input on its mapped context with a reach of its own when the set differs, maps that reach back after `renderScene` and adds it to the caller's; `SceneRenderActionArgs::getInputContext(i)`; a stable id per (parent id, input) for mapped contexts; `SceneLoadSet::unite(a, b)` (true union, self/below split; null wins).
  - verify: gtest — identity nodes pass the same pointers; `unite` is a superset of both on every prim of the city; a mapped input's reach comes back mapped; a diamond keeps two memo slots.
  - size: M

- [x] M381.P3.T7 — Path-changing nodes map load sets and reach
  - files: `Engine/Nodes/Scene/SceneOps.{h,cpp}` (`reparentRoots` ~:2569, `readInput` ~:2194, `rootHoldsGprim` ~:2289; `SceneReparentArgs` gains `inputContexts`, empty = current()), `Engine/Nodes/Scene/SceneReparentNode.cpp` (graft ~:474, `renderWrap` ~:364, beside `mapToInputPath` ~:282), `Engine/Nodes/Scene/USDAssignMaterial.cpp` (~:294), `Engine/Nodes/Scene/SceneNodeAuthoring.cpp` (`graftSharedMaterial` ~:840), `Engine/Nodes/Scene/USDReference.cpp` (source borrow ~:325), `Engine/Nodes/Scene/USDProjectUV.cpp` (~:758), `Engine/Nodes/Scene/USDProject3D.cpp` (~:353), `Tests/Scene/SceneLoadSetCoverage_Test.cpp`
  - approach: `SceneOps::mapGraftLoadSet`/`mapGraftReach` (destination, pass-through root, autoRename candidates united; unknown → null), wrap and reference mappings (check USD's ancestral-arc composition under load rules in the pxr headers before relying on the reference ancestor rule); `reparentRoots` reads each input on `inputContexts[i]` and records reach there; nodes override the P3.T6 virtuals. Camera inputs that aren't composed into the output (USDProjectUV, USDProject3D) get `withPaths(none, {camera})` as their input context.
  - verify: a USDPrune `/World//car*` above USDXformPrim `/Grp`, viewer L={/Grp/World/Block_0}: parity, only Block_0 opens, and loading `/Grp/World/Block_1` re-evaluates the prune; the same through wrap mode, an autoRename collision (superset parity), the AssignMaterial material input and the USDReference source; reach comes back in `/Grp` paths. `AGraftEvaluatesTheSameOnALoadSetAsOnEverything` is rewritten to load `/Grp/World/Block_0` with an upstream pattern node.
  - size: L

- [x] M381.P3.T2 — `graftMaterial` on the evaluation context
  - files: `Engine/Nodes/Scene/SceneNodeAuthoring.cpp` (~:765), `Tests/Scene/SceneLoadSetCoverage_Test.cpp`
  - approach: the current context with reach of the grafted roots. Also map grafts through `graftMaterialInput` (the geometry Material input) the way P3.T7 maps `graftSharedMaterial`.
  - verify: probe plus parity on a material input that has a payload.
  - size: M

- [x] M381.P3.T3 — Reparent wrap on the evaluation context
  - files: `Engine/Nodes/Scene/SceneReparentNode.cpp` (`renderWrap` ~:364, `gatherWrapRetargets` ~:113), `Tests/Scene/SceneLoadSetCoverage_Test.cpp`
  - approach: existence checks through `withPaths`; `gatherWrapRetargets` on the context set with reach "/".
  - verify: a relationship inside a payload is retargeted once that payload loads; probe reads 0.
  - size: M

- [x] M381.P3.T4 — USDProject3D camera through the closure
  - files: `Engine/Nodes/Scene/USDProject3D.cpp` (`renderScene` ~:459), `Tests/Scene/SceneLoadSetCoverage_Test.cpp`
  - approach: named camera through `withPaths`; with none named, context set plus reach "/".
  - verify: a camera inside a payload resolves while only its building opens.
  - size: M

- [x] M381.P3.T5 — USDRead metrics correction on the context
  - files: `Engine/Nodes/Scene/USDRead.cpp` (~:443), `Engine/Nodes/Scene/SceneOps.cpp` (~:1204), `Tests/Scene/SceneLoadSetCoverage_Test.cpp`
  - approach: the current context with reach "/".
  - verify: Match Up Axis on the city opens 0 buildings; parity at L, including a reset-stack prim inside a payload.
  - size: M

## Phase 381.4: Borrowers outside an evaluation

- [x] M381.P4.T1 — USDRender reads only its settings chain
  - files: `Engine/Nodes/Scene/USDRender.cpp` (~:273, `readActiveSettings` ~:334, `deriveOutputMetadata` ~:552), `Tests/Scene/SceneLoadSetCoverage_Test.cpp`
  - approach: `readActiveSettings` reads `renderSettingsPrimPath` with none loaded, then `withPaths` on the settings prim and its camera target. The settings-prim menu uses `eRuleAll` on `/Render`. `deriveOutputMetadata` stays everything (render output); check which thread runs it and report a main-thread stall in `## Decisions`.
  - verify: `getRegionOfDefinition` on the city opens 0 buildings with unchanged resolution.
  - size: M

- [x] M381.P4.T2 — Viewer evaluation context for transform handles
  - files: `Engine/ViewerInstance.h` (~:657), `Engine/ViewerInstancePrivate.h` (~:615), `Engine/Nodes/Scene/USDEditTransform.cpp` (`transformsPrim` ~:333, last-result fields ~:158-175), `Gui/SceneTransformTarget.cpp` (~:531), `Tests/Scene/SceneLoadSetCoverage_Test.cpp`
  - approach: `ViewerInstance::getSceneEvaluationContext()`; `transformsPrim(path, context)` is called with the asking viewer's context; matches cached per context.
  - verify: two contexts keep separate matches; the stale-match path opens 0 buildings.
  - size: M

- [x] M381.P4.T3 — Centre Pivot as a cancellable worker job
  - files: `Engine/Nodes/Scene/USDEditTransform.cpp` (`centrePivot` ~:379), `Tests/Scene/SceneLoadSetCoverage_Test.cpp`
  - approach: runs on a worker with the showing viewer's context. Bounds from loaded prims plus authored `extentsHint` for unloaded payloads (never measured, per the design doc).
  - verify: centre equals the fully loaded centre when L covers the matches; probe reads 0; a second press cancels the first.
  - size: M

- [x] M381.P4.T4 — Knob menus fill off the main thread
  - files: `Engine/Nodes/Scene/USDVariant.cpp` (~:237), `Engine/Nodes/Scene/USDProjectUV.cpp` (~:716), `Engine/Nodes/Scene/USDProject3D.cpp` (~:317), `Engine/Nodes/Scene/USDRenderSettings.cpp` (~:292), `Tests/Scene/SceneLoadSetCoverage_Test.cpp`
  - approach: today they run queued on the main thread and call `evaluateScene` with the default (everything) context. Evaluate on a worker with the showing viewer's context, add a disabled "N unloaded branches not searched" row, fill on the main thread.
  - verify: refresh returns before the worker ends; 0 buildings opened; the row count is right.
  - size: L

- [x] M381.P4.T6 — USDRender output metadata without a main-thread full load
  - files: `Engine/Nodes/Scene/USDRender.cpp` (`deriveOutputMetadata`), `Engine/Nodes/NativeEffectBase.cpp` (~:509 `getMetadata`), `Tests/Scene/SceneLoadSetCoverage_Test.cpp`
  - approach: `deriveOutputMetadata` borrows everything and is reached on the main thread (`refreshMetadataSideEffects` → `getMetadata`, the viewer tab and node graph), so the first derive after each change stalls the UI on a heavy scene. Find what it actually reads (settings, products, AOVs, camera) and read only those chains through `withPaths`; anything that truly needs the whole stage moves to the render thread, with the metadata refreshed when it arrives.
  - verify: test — deriving the output metadata of a USDRender over the city with nothing loaded opens at most the settings and camera chain and gives the same metadata as with everything loaded.
  - size: M

- [x] M381.P4.T7 — The showing viewer's context mapped down to the node
  - files: `Engine/Nodes/Scene/SceneNodeAuthoring.{h,cpp}` (`getShowingViewerContext`), `Tests/Scene/SceneLoadSetCoverage_Test.cpp`
  - approach: the viewer's load set is in the viewer's paths, but Centre Pivot (P4.T3) and the knob menus (P4.T4) use it on the node directly. Walk the input chain from the viewer down to the node and apply each node's `getSceneInputContext` (P3.T6/T7), so a graft between the node and the viewer maps the set as an evaluation would. With several routes, unite the results.
  - verify: test — a USDVariant above a USDXformPrim `/Grp`, viewer loading `/Grp/World/Block_0`: the variant menu searches `/World/Block_0` loaded and reports the other blocks unsearched; Centre Pivot through the same graft uses the mapped set.
  - size: M

- [x] M381.P4.T8 — Menus follow viewer loads, and the note row is disabled
  - files: `Engine/Nodes/Scene/SceneNodeAuthoring.{h,cpp}` (`SceneMenuRefresh`), `Engine/ChoiceOption.h` (or wherever `ChoiceOption` lives), `Gui/KnobGuiChoice.cpp`, `Gui/ComboBox.cpp`, the four menu nodes, `Tests/Scene/SceneLoadSetCoverage_Test.cpp`
  - approach: a load change in the showing viewer whose paths meet a menu's recorded reach re-runs that menu's refresh (use the reach the job recorded, mapped up per P3.T6). `ChoiceOption` gains a disabled flag that `KnobGuiChoice`/`ComboBox` honour, and the "N unloaded branches not searched" row uses it instead of being selectable-and-ignored.
  - verify: test — loading Block_0 in the viewer refills the variant menu with B_0's variant set and drops the note's count; the note row is disabled in the combo box (GUI check in an existing scene GUI script, or a gtest on `ChoiceOption` plus `ComboBox` if a widget test exists).
  - size: M

- [x] M381.P4.T5 — No implicit everything borrow
  - files: `Engine/Nodes/Scene/SceneQueryPool.h` (~:106) and every remaining caller
  - approach: delete `SceneQueryPool::borrow(payload, err)`; every caller names its set, with an explicit everything plus a reason for USDWrite, render metadata and Search Unloaded. Add an owner table to the `SceneQueryPool.h` doc.
  - verify: `rg` finds no implicit borrow; syntax-check on the touched files; the P1–P4 suites pass.
  - size: M

**Verification gate:** release gate green with `SceneLoadSetCoverage_Test` (a no-full-load test and a parity test for every converted node). An Xvfb check that opening USDVariant and USDRenderSettings properties on the city keeps the main thread responsive. The city check, each node's viewer result agreeing with USDRender, goes to F4 - Heavy Scene Viewer's UAT.

## Decisions

- 2026-11-13 — Elaborated after the claim (consultant). The stub's list was confirmed and extended: USDEditPrim Create mode, USDReference, `SceneReparentNode::renderWrap`, `graftMaterial`, and four main-thread knob-menu refreshes that evaluate fully loaded. `SceneGraftMemo` and USDEditTransform's last result hold one state each, so they become per-context.
- 2026-11-13 — Load-set owner (user): a borrower uses the load set of the evaluation it serves (viewer, a Scene Graph panel's own, or everything for renders, USDWrite and scripts). Outside an evaluation it uses the viewer that asked or that shows the node: the most recently active viewer whose scene input reaches the node, else no load set plus the "N unloaded branches not searched" note. Known-path readers load only the payloads on those paths. Rejected: a showing panel supplies the set (zero or several panels, results depend on which is open); everything for menus and buttons (main-thread stalls).
- 2026-11-13 — P1.T1 implemented (578518139). `expectParity` composes fully loaded, so `openedBuildings()` is meaningless after a parity check in the same test.
- 2026-11-13 — P3.T1 implemented (1094c6953). It exposed a pre-existing F4 bug from M331 - Deferred Pattern Matching: `evaluateScene` passes the caller's context unchanged to every input, but load sets and reach are in the viewer's (output) paths, so a pattern node upstream of a graft, a reference source or a material input matches nothing inside payloads and is never re-evaluated by a load (stale). Consultant design adopted: inputs evaluate on their own context through identity-by-default virtuals at the input-evaluation boundary, path-changing nodes map load sets down and reach back up, unknown mappings fall back to a superset (P3.T6, P3.T7, before P3.T2). P3.T1's parity test was vacuous (it loaded `/World/Block_0` on a `/Grp` stage) and is rewritten in P3.T7.
- 2026-11-13 — Camera inputs that aren't composed into the output read on `withPaths(none, {camera})` rather than the viewer's set (PM call): the viewer's set says nothing about a separate camera branch.
- 2026-11-13 — P3.T7 implemented (100056752). USD probe (pxr 26.08): a referencing prim's own load rule decides both the payloads above the referenced prim and its own payload, confirming "S's ancestors get T's self rule". The P3.T6 identity shortcut now also requires the same reach object, since a mapped input whose set equals the caller's would otherwise return unmapped reach. autoRename maps conservatively (a root picks up every `D/X_n` rule), over-loading on collisions; accepted as a correct superset (PM). USDReference with no Source Prim reads its source with everything loaded. `graftMaterialInput` is mapped in P3.T2.
- 2026-11-13 — P4.T1 implemented (94f26acf1). `deriveOutputMetadata` stays an everything borrow but is reached on the main thread (`refreshMetadataSideEffects` → `getMetadata`, the viewer tab, the node graph), a UI stall on heavy scenes; P4.T6 added to remove it (PM, per "UI never stalls"). `prepareRender` keeps everything (render thread; the camera fallback needs the whole stage).
- 2026-11-13 — P4.T3 implemented (034216bd8). The viewer is the active one if it is downstream, else the first downstream viewer (no recency tracker exists). If that viewer's context has no stored result, the job uses the latest result. A knob-driven button: the gate adds an Xvfb press of Centre Pivot on the city (ctest alone doesn't cover it).
- 2026-11-13 — P4.T4 implemented (9a40064f2). Gaps found and given tasks (PM): the showing viewer's context isn't mapped through path-changing nodes between the node and the viewer (affects P4.T3 too; P4.T7); menus don't refresh when the viewer loads a branch, and the note row is selectable-but-ignored because `ChoiceOption` has no disabled flag (P4.T8). Each refresh runs on its own detached thread, superseded jobs cancelled by their abort flag.
- 2026-11-13 — P4.T7 implemented (ff16fe245): `mapContextDownToNode` walks from the viewer's input to the node. The mapped context keeps the viewer's id (evaluations derive their own ids privately), so Centre Pivot may fall back to the node's latest result.
- 2026-11-13 — P4.T8 implemented (d626498f1): `ViewerInstance::setSceneLoadSet` queues `SceneMenuRefresh::followLoadChanges()`; camera menus reach the whole stage. `ChoiceOption` has a `disabled` flag honoured by `ComboBox` (new `ComboBox_Test`); `KnobChoice` itself doesn't refuse a scripted pick, the nodes ignore the note value.
- 2026-11-13 — P4.T6 implemented (6110fc68b). The output metadata is the primId table of every gprim on the fully loaded stage, so it can't come from a partial load (the verify line's "same metadata with nothing loaded" can't hold). It is never built on the main thread: readers off the main thread or `prepareRender` build it from a full stage, main-thread readers get an uncached provisional empty answer and are invalidated when it arrives (PM, accepted). Main-thread readers (Python, clip preferences) see no primId entries until a render or an off-thread reader builds the table for the current hash; a background full load just to fill it was rejected, as it would load the whole scene for a node merely being in the graph.
- 2026-11-13 — P4.T5 implemented (311c55e48): no unconverted borrower found; explicit `SceneLoadSet::everything()` only at USDRender's render output and off-thread primId metadata, and in tests. All tasks implemented; full release gate running (stale phase-3 build cancelled). The gate's Xvfb checks (properties responsiveness on the city, a Centre Pivot press) need a GUI script, written next and run with `--from-run`.
- 2026-11-13 — First full build (gate run 43459797124): 19 of the new tests fail in both legs (SceneLoadSetCoverage, `withPaths`, ComboBox); being diagnosed by cause.
- 2026-11-13 — First-build failures diagnosed (5d812aa1c). Product bugs: USD's `Minimize()` dropped nested load-only rules, so `withPaths` loaded nothing (now only the innermost load-only rule of a chain is kept); mapped load sets weren't minimal (new `SceneLoadSet::minimal`); `ComboBox::isItemEnabled` on a missing index. Test bugs: parity counted edits under unloaded payloads; the centre-pivot tests matched nothing. Three link tests assumed links that appear only once a payload loads, which USD can't produce. PM call: links authored in loaded layers are the same on every load set, and those tests are being rewritten.
- 2026-11-13 — Link tests rewritten (97575b2c3); re-gating with `m381_deferred_matching_gui.py`.
- 2026-11-13 — Gate fixes (test and script only) and F4 merged in (30a9be6bc); re-gating.
- 2026-11-13 — Gate green on 30a9be6bc (ctest; GUI script 57/57). F4 merged in (071720688). PR #278 into F4 opened; review running.
- 2026-11-13 — PR #278 review round 1: 18 findings, 4 high. USDRender's settings menu and `readActiveSettings` still compose upstream on the main thread with load-everything. `findOwner` regressed through grafts. `withPaths` loaded too little when the root rule was absent. Medium: the viewer choice didn't follow the 'reaches the node' rule; detached menu and pivot threads; context-id collisions; camera menus ignored the viewer's loads. Being fixed.
- 2026-11-13 — Round 1 fixes committed (3fe0dec1e). USDRender's menu and summary run off the main thread, with a provisional format until the worker result lands. A new `SceneWorker` gives each state one joinable worker. The round-2 review is checking the main-thread join on node destruction. Re-gating.
- 2026-11-13 — Review round 2 (final): of the 18 round-1 findings, 13 are fixed and 5 partly. The fix itself added two high findings: the main thread joined `SceneWorker` on node destruction (unbounded, and it can deadlock with the GIL), and stale summary jobs erased the newer provisional record. PM direction: cancel and detach on node destruction, join only at app shutdown with the GIL released; the main thread never evaluates upstream for USDRender's provisional summary. Being fixed; gate cancelled.
- 2026-11-13 — Round 2 fixes committed (984b75629). Re-gating.
- 2026-11-14 — PR CI (debug) on 984b75629 failed SceneLoadSetCoverage, SceneQueryPoolLoadSets and the USDRender* suites. Fixed in 180f36b44: `applyLoadSet` loads rule-free subtrees whole and only the prim itself where rules sit below (an All rule with rules under it no longer opens excluded payloads); scene work that outlives its node returns Aborted instead of asserting in `clearPersistentMessage` (jobs are detached on node destruction by design); USDRender's primId table is keyed on the input's hash, since repopulating the settings menu moves the node's hash. The Centre Pivot test now releases idle stages first. GraphScalingConnectTest's timeout is runner slowness (1.5x across the suite), not this diff. Superseded gate cancelled; re-gating.
