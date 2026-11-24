# M580 - Scene Load Masks

Gives the Scene Graph panel controls over what a viewer's stage composes. They are a population mask (the prims that exist on it), muted file layers, payload load rules as an editable list, and variant selection fallbacks. They are saved with the script, as the viewer's load rules already are. They apply to the viewer only, and renders, writes and scripts still compose everything. A Create USDConfigureStage From Load Masks action bakes them into a new `USDConfigureStage` node, which renders and writes honour. Today a viewer's payload state is a `SceneLoadSet` (`Engine/SceneLoadSet.h`, path rules mirroring `UsdStageLoadRules`). The viewer commits it through `ViewerInstance::setSceneLoadSet` from `SceneViewport`'s `LoadSetCommitted` callback (`Gui/ViewerTab.cpp` ~:1630), and nodes evaluated for that viewer see it through `SceneEvaluationContext::loadSet`, the deferred-matching rule of F4 - Heavy Scene Viewer. `SceneQueryPool::borrow(payload, loadSet, …)` gives them stages with the same payloads loaded. The rules are set only by twirling rows and the row menu (`Gui/SceneGraphPanel.cpp`, M556 - Scene Graph Bulk Commands adds scoped loads). They are saved per viewer in `Serialization::SceneViewSerialization::loadRules` (`Serialization/NodeSerialization.h`, `Serialization/Yaml/NodeYaml.cpp`, converted by `sceneViewToSerialization`/`sceneViewFromSerialization` in `Engine/Nodes/Scene/SceneDrawStates.cpp`). Nothing in the code sets a population mask, mutes a layer or sets variant fallbacks. A payload's layers are a stack of `SceneLayerEntry` (`Engine/Nodes/Scene/ScenePayloadImpl.h`), which USDMerge's `mergeStacks` combines. The design references are Solaris's viewport load masks (`.local/docs/houdini/hom/hou/LopViewportLoadMasks.md`, `ref/panes/scenegraphtree.md`) and its Configure Stage LOP (`.local/docs/houdini/nodes/lop/configurestage.md`).

## Scope

**In:**
- `SceneLoadSet` gains a population mask (a list of prim paths, empty meaning everything), a list of muted layer identifiers, and variant selection fallbacks (variant set name → ordered selections). All three are part of its hash and equality and are kept by `withPaths`. `unite` combines them: masks as a union, mutes only when both sides mute, and fallbacks only where both agree. SceneOps converts the first two to `UsdStagePopulationMask` and `MuteAndUnmuteLayers`.
- Variant fallbacks are applied per stage by authoring the first available fallback selection, in the stage's session layer, on each prim that has the variant set and no composed selection. USD's own fallbacks (`SetGlobalVariantFallbacks`) are process-wide and only reach stages opened later, so they cannot serve one viewer.
- The viewer's stage and the pool stages its nodes borrow apply all of it, so deferred matching sees only populated prims, unmuted layers and the fallback variants.
- `SceneViewSerialization` gains `populationMask`, `mutedLayers` and `variantFallbacks`, emitted and read in YAML.
- A Load Masks button on the Scene Graph panel toolbar opens a popup, for the viewer the panel follows, with four sections and two actions:
  - Population: a Populate All toggle, a list of populated paths (add the selected rows, remove), and a row menu entry "Populate Only Selected".
  - Payloads: a Load All Payloads toggle, and the load rules as rows (path, rule All/Only/None) that can be edited and removed.
  - Muted Layers: the file layers the stage uses, each with a mute checkbox.
  - Variant Fallbacks: rows of variant set name and fallback selections, each with an enable toggle.
  - Reset All Load Masks clears the four lists without touching either toggle, as Solaris's reset does.
  - Create USDConfigureStage From Load Masks.
- `USDConfigureStage`: a node whose output carries a stage configuration (population, load rules, mutes, variant fallbacks) that every consumer of its scene composes with. That covers renders (USDRender), USDWrite, nodes downstream and viewers. It has a `.reg.inc`, the USD Composition toolbar submenu (its `PLUGIN_GROUP_USD_*` macro from M520 - USD Toolbar Category And Submenus), an icon, `docs/USDConfigureStage.md` and tests.
- The bake action: it inserts a `USDConfigureStage` after the viewed node, on the viewer's branch, with its knobs in Set modes holding the viewer's four masks, as one undo step. It is enabled only when a mask is set. The viewer's own masks are left as they are.
- Audit of Solaris's viewport load mask controls (each In or Out):
  - Populate All Primitives: In.
  - Population paths, with add, remove and remove with children: In.
  - Load All Payloads: In.
  - Load paths, with add and remove: In. The existing rules are a superset, since they also hold Only and None.
  - Mute Layers: In.
  - Variant selection fallbacks: In.
  - Reset All Viewport Load Masks: In.
  - Create Configure Stage Node from Load Masks: In, as Create USDConfigureStage From Load Masks. Unlike Solaris, it bakes the payload rules too.
  - Named saved blocks of masks: Out, with no milestone. Each viewer saves its own with the script.
- Audit of Solaris's Configure Stage controls for `USDConfigureStage` (each In or Out; labels are the readable script names):
  - Populate Primitives mode (Don't Change, Add and Remove, Set, Populate All): In, as `populateMode`.
  - Populate Paths and Remove Populate Paths (explicit paths): In, as `populatePaths` and `removePopulatePaths`.
  - Populate Paths from Input and Remove Populate Paths from Input (patterns matched on the input): Out, with no milestone. They would match through the input's unloaded payloads; explicit paths only.
  - Load Primitives mode (Don't Change, Add and Remove, Set, Load All): In, as `loadMode`.
  - Load Paths and Remove Load Paths: In, as `loadPaths` and `removeLoadPaths`. Each set path also takes All, Only or None, held in a `loadRules` table.
  - Load Paths from Input and Remove Load Paths from Input: Out, for the same reason as the populate patterns.
  - Mute Layers mode (Don't Change, Add and Remove, Set, Unmute All): In, as `muteMode`.
  - Mute Paths and Unmute Paths: In, as `mutePaths` and `unmutePaths`, for file layers.
  - Variant Selection Fallback Rules (Enable, Variant Set Name, Fallback Selections): In, as a `variantFallbacks` table.
  - Resolver Context Asset Path and Resolver Context Strings (Enable, URL Prefix, Value): Out, with no milestone. The fork uses the default resolver, which reads neither.
  - Stage Variables: M565 - USDConfigureLayer Node, which authors them as layer metadata.

**Out:**
- The viewer's own masks applied to renders, USDWrite or scripts: none. Renders keep loading everything unless a `USDConfigureStage` says otherwise (user decision).
- Muting a node's own anonymous layer: none. Disabling the node does that, and anonymous identifiers change between sessions.
- Load masks in live sessions' personal viewers: M561 - Live Session Personal Viewers.
- Muting from the inspector's Layer Stack tab: none. The popup owns muting, and M571's tab shows the result as it reads the stage.
- Prim and layer configuration (kind, active, layer metadata): M564 - USDConfigurePrim Node and M565 - USDConfigureLayer Node. Neither carries stage-level load state, so a dedicated node is used.

**Edge cases:**
- Nothing is loaded or populated without the user choosing it. A new viewer starts as today (root rule None, Populate All on, no mutes, no fallbacks).
- A population path that names no prim: kept and shown greyed "not in scene", as saved paths may arrive later (a payload loading, an upstream edit).
- A load rule under an unpopulated path: kept, greyed, and has no effect until populated.
- Turning Populate All off with no paths: the stage has no prims. The tree shows only the root and the popup says so; it is not refused.
- "Populate Only Selected" on rows inside an unloaded payload: the payload stays unloaded, and the mask names the paths for when it loads.
- Unpopulated prims are not on the stage, so the tree cannot show them. Paths are added back from the popup's list (typed or pasted paths), or by Populate All.
- Muting the layer a USDRead reads: its prims leave the viewer. Muting a layer only some prims use changes only their opinions.
- A muted identifier is the asset path as the stage uses it. A file missing at load is kept in the list and greyed.
- Variant fallbacks: a prim with an authored selection anywhere in its composition keeps it. A fallback naming no variant of the set tries the next. With none available, nothing is authored. When a payload loads and brings in its own selection for a prim, the fallback selection on that prim is withdrawn and the prim is re-evaluated, so a fallback never beats an authored opinion.
- Viewer masks over a `USDConfigureStage` scene: as Solaris states it, mutes are the union of both, the population is the intersection, viewer load rules act only within the payloads the node loads, and a viewer fallback applies only to variant sets the node leaves without one.
- USDMerge of configured inputs: masks and mutes are the union of the inputs', load rules are united as `SceneLoadSet::unite` does, and fallbacks of the stronger input win per variant set.
- USDWrite of a configured scene: Flattened mode writes the configured, composed stage. Layered mode leaves muted file layers out of its sublayers and writes the fallback selections. It warns that a population mask or load rules cannot be stored in layered files and that the written layers hold the whole scene.
- USDConfigureStage in Don't Change modes passes the input's configuration through unchanged. A disabled node passes its input through.
- Nodes evaluated for the viewer author over unpopulated prims as before. Their overs compose when the prims are populated again.
- Draw states (M329 - Viewer Draw States) on unpopulated prims are kept and apply when they return.
- The panel showing a node no viewer shows (its own stage): the Load Masks button is disabled, with a tooltip saying masks belong to a viewer.
- Two viewers: each keeps its own masks.
- Changing a viewer mask is viewer state, not a graph edit: it is not on the undo stack, as payload loads are not today. The bake action is a graph edit and is undoable.
- A mask edit recomposes the viewer's stage on its worker (`SetPopulationMask`, `MuteAndUnmuteLayers` and the fallback authoring on the long-lived stage), never on the main thread, and cancels a superseded recompose, as load batches do.

**Assumptions:**
- `UsdStage::SetPopulationMask` and `MuteAndUnmuteLayers` on SceneStage's long-lived stage are cheaper than reopening it, and keep the loaded payloads within the mask.
- `SceneQueryPool`'s nearest-stage choice still works with masks in the key. A stage with another mask is reused by changing its mask.
- Authoring fallback selections in a stage's session layer, walking only the populated and loaded prims, is fast enough on the Island flat file to stay within the recompose budget.

## Phase 580.1: Engine

- [ ] M580.P1.T1 — Population mask, muted layers and variant fallbacks in `SceneLoadSet`
  - files: `Engine/SceneLoadSet.{h,cpp}`, `Engine/Nodes/Scene/SceneOps.{h,cpp}` (conversions), `Tests/Scene/SceneLoadSetCoverage_Test.cpp`
  - approach: add `populationPaths` (sorted, minimal: drop a path under another), `mutedLayers` (sorted) and `variantFallbacks` (ordered map of set name to selections) to `SceneLoadSet`, with constructor, accessors, hash, equality, `withPaths` (keeps all three), `unite` (per In), `minimal` and `getChangedPaths`. Audit every `includesEverything()`/`getEntries().empty()` caller so that a set with a mask, mutes or fallbacks never counts as everything. SceneOps gets `toPopulationMask` and `toMuteList`.
  - verify: gtest: hashes differ by mask, by mutes and by fallbacks; `unite` of /a and /b masks is /a /b, and of /a and everything is everything; mutes intersect; fallbacks keep only agreeing sets; `includesEverything()` is false with only a mask; the USD conversions give the expected `UsdStagePopulationMask` and lists.
  - size: L

- [ ] M580.P1.T2 — Stages apply the mask and mutes
  - files: `Engine/Nodes/Scene/SceneStage.{h,cpp}`, `Engine/Nodes/Scene/SceneQueryPool.cpp`, `Tests/Scene/SceneStage_Test.cpp`, `Tests/Scene/SceneQueryPool_Test.cpp`
  - approach: when a stage syncs to a load set whose mask or mutes differ, it applies `SetPopulationMask` and `MuteAndUnmuteLayers`, then its payload rules, under the write lock and counted as a load edit. The pool includes mask and mutes in its nearest-stage choice.
  - verify: gtest: a stage synced with mask /World/A lacks /World/B; changing the mask to everything brings B back with A's loaded payloads still loaded; muting a sublayer file drops its opinion and unmuting restores it; the pool reuses one stage across two masks.
  - size: M

- [ ] M580.P1.T3 — Stages apply variant fallbacks
  - files: `Engine/Nodes/Scene/SceneStage.{h,cpp}`, `Engine/Nodes/Scene/SceneOps.{h,cpp}` (`applyVariantFallbacks`), `Tests/Scene/SceneStage_Test.cpp`
  - approach: after a sync, walk the populated, loaded prims with variant sets and author, in the stage's session layer, the first available fallback for each set the prim has no composed selection for. Track what was authored, so that a later sync (new loads, new fallbacks) withdraws stale selections first and re-walks only the changed subtrees. Do it under the write lock in one change block.
  - verify: gtest: a prim with a `shadingVariant` set and no selection gets the fallback `red`; a prim with an authored `blue` keeps it; a fallback list `green red` on a set without `green` picks `red`; loading a payload that authors its own selection on the prim withdraws the fallback; clearing fallbacks restores the unselected state.
  - size: M

- [ ] M580.P1.T4 — Save masks with the script
  - files: `Serialization/NodeSerialization.h`, `Serialization/Yaml/NodeYaml.cpp`, `Engine/Nodes/Scene/SceneDrawStates.{h,cpp}` (`sceneViewTo/FromSerialization`), `Engine/ViewerInstance.cpp`, `Tests/Scene/SceneDrawStates_Output_Test.cpp` (or beside the existing scene view serialization test)
  - approach: add `populationMask` (list; absent = everything), `mutedLayers` (list) and `variantFallbacks` (map of list) to `SceneViewSerialization` and `isEmpty()`. Emit and read them in YAML beside `loadRules`, and carry them through `sceneViewToSerialization`/`sceneViewFromSerialization` and the viewer's restore into its first committed load set. No older-format reading.
  - verify: gtest round trip through YAML with a mask, mutes, fallbacks and rules; a non-absolute mask path is dropped on read, as load rule paths are.
  - size: M

## Phase 580.2: Panel controls

- [ ] M580.P2.T1 — Load Masks popup: population and payloads
  - files: `Gui/SceneLoadMasksPopup.{h,cpp}` (new), `Gui/SceneGraphPanel.{h,cpp}`, `Gui/SceneViewport.{h,cpp}` (commit a load set with mask), `Gui/CMakeLists.txt`, `Tests/gui/scene_load_masks.py` (new)
  - approach: a toolbar button (`sceneGraphLoadMasks`) opens the popup for the followed viewer. Population section: Populate All toggle, a path list with Add Selected, Remove and Remove With Children, and paste or typed entry. Payloads section: Load All Payloads toggle and an editable rule table. Reset All Load Masks clears the lists. Edits commit through the viewport's load set path (`LoadSetCommitted`), off the main thread. The row menu gains Populate Only Selected. Disabled and greyed states follow Edge cases. The popup mirrors its state into `natronSceneGraphLoadMask*` properties for scripts.
  - verify: Xvfb script at 1920x1080 on the Island flat file: Populate Only Selected on one branch leaves only it and its ancestors in the tree and the viewer; Populate All restores; editing a rule from All to None unloads that branch; saving and reopening the script restores mask and rules; the main-thread heartbeat stays within the M333 budget during each recompose; a screenshot of the popup goes to the user.
  - size: L

- [ ] M580.P2.T2 — Muted Layers section
  - files: `Gui/SceneLoadMasksPopup.cpp`, `Engine/Nodes/Scene/SceneOps.{h,cpp}` (list the stage's file layers), `Tests/gui/scene_load_masks.py`
  - approach: the section lists the stage's used non-anonymous layers (`GetUsedLayers`, read on the worker under the stage lock), each with a mute checkbox and its owning node (the USDRead or USDReference that brought it), muted ones included even when no longer used. Toggling commits the new mute list as above.
  - verify: Xvfb script: a USDRead of a file with a sublayer lists both files; muting the sublayer removes its prim from the tree and viewer, and unmuting restores it; a muted file survives save and reload.
  - size: M

- [ ] M580.P2.T3 — Variant Fallbacks section
  - files: `Gui/SceneLoadMasksPopup.cpp`, `Engine/Nodes/Scene/SceneOps.{h,cpp}` (list the stage's variant set names), `Tests/gui/scene_load_masks.py`
  - approach: a table of rows (Enable, Variant Set Name with a completer filled from the variant sets on the stage's loaded prims, read on the worker, and Fallback Selections as space- or comma-separated names). Edits commit the new fallbacks through the load set path.
  - verify: Xvfb script: a USDRead of a file with an unselected `modelingVariant` set; adding the fallback `proxy` switches the prim's children in the tree; disabling the row restores them; the row survives save and reload.
  - size: M

## Phase 580.3: USDConfigureStage

- [ ] M580.P3.T1 — Stage configuration carried by the payload
  - files: `Engine/Nodes/Scene/ScenePayloadImpl.{h,cpp}`, `Engine/ScenePayload.h`, `Engine/Nodes/Scene/SceneQueryPool.cpp`, `Engine/Nodes/Scene/SceneOps.{h,cpp}` (`compose`), `Engine/Nodes/Scene/USDMerge.cpp`, `Engine/Nodes/Scene/USDWrite.cpp`, `Engine/ViewerInstance.cpp`, `Tests/Scene/SceneStageConfig_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: a pxr-free `SceneStageConfig` (population, load rules, mutes, fallbacks, each with "set" or "inherit") on `ScenePayload`, included in its stack hash and passed through by every node that appends entries. `SceneQueryPool::borrow` and `SceneOps::compose` combine it with the borrower's load set by the Edge-cases rule (the viewer's set narrowed by the payload's, a render's everything replaced by the payload's), so renders, downstream nodes and viewers honour it. USDMerge unites it per Edge cases. USDWrite applies it in Flattened mode and follows the Layered-mode rule with its warning.
  - verify: gtest: a payload configured to populate /World/A composes without /World/B for a render borrow (`SceneLoadSet::everything()`) and for a viewer borrow; a viewer mask of /World intersected with it still lacks B; a merge of two configured inputs unites their masks; a Flattened USDWrite omits B; a Layered USDWrite warns and leaves the muted file out of its sublayers.
  - size: L

- [ ] M580.P3.T2 — `USDConfigureStage` node
  - files: `Engine/Nodes/Scene/USDConfigureStage.{h,cpp,reg.inc}` (new), `Engine/CMakeLists.txt`, `Gui/Resources/Images/` (icon), `Engine/Nodes/Scene/docs/USDConfigureStage.md` (new), `Tests/Scene/USDConfigureStage_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: one scene input. Knobs per the Configure Stage audit (`populateMode`, `populatePaths`, `removePopulatePaths`, `loadMode`, `loadRules`, `removeLoadPaths`, `muteMode`, `mutePaths`, `unmutePaths`, `variantFallbacks`), labelled with their readable script names. It applies its modes to the input's `SceneStageConfig` and emits no layer, except that the fallback selections are authored at evaluation in its own layer, on the prims of its everything-loaded input that lack a composed selection. It registers in the Composition submenu. The doc covers every knob and the combination rules from Edge cases, with no reference-app names.
  - verify: gtest: each mode of each section (Don't Change passes the input's through, Add and Remove edits it, Set replaces it, Populate All, Load All and Unmute All clear it); two chained nodes combine as expected; a fallback reaches a USDRender borrow; a disabled node passes through.
  - size: L

- [ ] M580.P3.T3 — Create USDConfigureStage From Load Masks
  - files: `Gui/SceneLoadMasksPopup.cpp`, `Gui/SceneSelectionActions.{h,cpp}` (node creation after the viewed node), `Tests/gui/scene_load_masks.py`
  - approach: the popup's action creates a `USDConfigureStage` after the viewed node on the viewer's branch, the way create-node-from-selection inserts nodes. It sets Set modes and fills paths, rules, mutes and fallbacks from the viewer's current load set, as one undo step. It is disabled when no mask is set.
  - verify: Xvfb script: with a branch populated, a file muted and a fallback set, the action creates the node with those values; a USDRender of the node's output omits the unpopulated branch; one undo removes the node; a screenshot of the created node's panel goes to the user.
  - size: M

- [ ] M580.P3.T4 — Params→knob audit
  - files: `Engine/Nodes/Scene/USDConfigureStage.cpp`, `Engine/Nodes/Scene/docs/USDConfigureStage.md`
  - approach: check every In and Out Configure Stage control in `## Scope` against the node's knobs and doc, and confirm the labels are readable script names. Fix any gap and record the table in `## Decisions`.
  - verify: the Decisions entry lists every Configure Stage control with its knob or reason, and nothing In is missing.
  - size: M

## Phase 580.4: Renders and viewer masks

- [ ] M580.P4.T1 — Renders and writes ignore viewer masks
  - files: `Tests/Scene/SceneIntegration_Test.cpp`, `Tests/gui/scene_load_masks.py`
  - approach: with a viewer masked to one branch, a layer muted and a fallback set, and no USDConfigureStage, check that USDRender and USDWrite evaluate with the default context (id 0, everything) and see the whole, unmuted, unselected scene. No code change is expected. If one fails, fix the caller that leaked the viewer's set.
  - verify: gtest: a USDWrite of the masked viewer's node writes every prim and the muted layer's opinions, with no fallback selection; Xvfb script: a USDRender of the same node renders the unpopulated branch.
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; `SceneLoadSetCoverage_Test`, `SceneStage_Test`, `SceneQueryPool_Test`, `SceneStageConfig_Test`, `USDConfigureStage_Test` and the serialization and integration tests pass; `Tests/gui/scene_load_masks.py` passes under Xvfb at 1920x1080 within the stall budget; the user OKs the popup and created-node screenshots.

## Decisions

- 2026-11-24 — **Milestone now, recommended shape (user):** Scene Graph panel controls for population mask, layer muting and payload load rules, saved with the script; viewer-only by default, with renders loading everything. The user chooses what loads (the deferred-matching rule).
- 2026-11-24 — **Create node from masks is In (user):** an action bakes the viewer's masks into a node that renders honour. The planner picked a dedicated `USDConfigureStage`, since M564 (prim configuration) and M565 (layer metadata) carry no stage-level load state.
- 2026-11-24 — **Variant selection fallbacks are In (user):** saved with the masks and baked by the same action.
- 2026-11-24 — Masks travel in `SceneLoadSet`, so the nodes evaluated for a viewer match against the same populated, unmuted stage the viewer draws (planner). Payload rules are already saved per viewer; this milestone adds the editor and the other masks.
