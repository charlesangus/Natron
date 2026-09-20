# Milestone 38: Layer/channel selection widget: process-in-place, no implicit shuffling (absorbs M35, M43, M36)

Governing design docs: `PLAN/DESIGN/2026-09-19-layer-registry.md` (Phase 38.1, approved
2026-09-19 with the "present-only input lists" amendment) and
`PLAN/DESIGN/2026-09-19-layer-channel-widget.md` (Phases 38.2+; **being rewritten** around
three knob types — channel set / one layer / one channel — see `## Decisions`). Later phases
are elaborated once the widget doc is approved; M43 (drop premult) and M36 ("New layer…")
become phases here.

## Phase 38.1: Project-level layer registry

- [x] M38.P1.T1 — Add `LayerRegistry` with validation, reserved aliases, union rule and channel grouping
  - files: `Engine/LayerRegistry.h`, `Engine/LayerRegistry.cpp`, `Engine/CMakeLists.txt`, `Tests/LayerRegistry_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: value class per design §1.1 (built-ins seeded in the `Project.cpp:1096-1101` order, `depth [Z]` as `eOriginUser`), `add/remove/find/contains/snapshot`, `validate(desc, fromFile, error)` with `kLayerMaxChannels = 4`, `reservedAlias`, union-on-file-conflict returning an `eAddResult {added, unchanged, grown, refused}`, `groupChannelNames` mirroring `ReadOIIO.cpp:1138-1172`. Pure data + `QMutex` + `shared_ptr<const vector>` snapshot; no Qt signal here.
  - verify: `ctest -R LayerRegistry`: EXPECT_* on built-in order, `rgba`→Color alias, `none/all/Backward` refused, dotted ID refused unless `fromFile`, 5 channels refused, duplicate identical → unchanged, file union RGB+RGBA → grown, user conflict → refused, remove built-in refused, `groupChannelNames({"R","G","B","A","Z","diffuse.R","diffuse.G"})` → Color, depth, diffuse; a snapshot taken before an `add` is unchanged after it.
  - size: M

- [ ] M38.P1.T2 — Make `Project` own and serialize the registry; emit `projectLayersChanged`
  - files: `Engine/ProjectPrivate.h`, `Engine/Project.h`, `Engine/Project.cpp`, `Engine/ProjectSerialization.h`, `Tests/ProjectSerialization_Test.cpp`
  - approach: `ProjectPrivate::layers` replaces `defaultLayersList` as the source of truth (the knob stays for T6, marked non-persistent); `Project::addLayer/removeLayer/getLayerRegistry/getLayerUsers`; delete `getProjectDefaultLayers/addProjectDefaultLayer/getProjectDefaultLayerNames` (`Project.cpp:1478-1580`) and the `defaultLayersList` branch of `knobChanged` (`:1776-1781`); `ProjectSerialization::_layers` (`Layers`, non-built-ins only, with `Origin`), version 6→7, restored in `ProjectPrivate::restoreFromSerialization` after formats (`ProjectPrivate.cpp:141`) and before knobs; signal `projectLayersChanged()` (`Project.h:392` pattern), suppressed while `isLoadingProject` and emitted once after `ProjectPrivate.cpp:236`; on change call `refreshChannelSelectors()` on all nodes.
  - verify: extend `ProjectSerialization_Test`: add `diffuse` + `specular`, save, reset, load → registry has both in order, built-ins not written to the XML (grep the saved file), `projectLayersChanged` emitted exactly once on load (QSignalSpy). Independent of T3: uses only `Project` API.
  - size: M

- [ ] M38.P1.T3 — Route availability and OFX publication through the registry; delete per-node user components
  - files: `Engine/EffectInstance.cpp`, `Engine/EffectInstance.h`, `Engine/Node.cpp`, `Engine/Node.h`, `Engine/NodePrivate.h`
  - approach: `getAvailableLayers` (`EffectInstance.cpp:4496-4517`) merges one registry snapshot for `inputNb == -1` only; new `getPresentLayers` (produced ∪ pass-through, `:4466-4493, 4511`); `getUserLayers` (`:182-196`) = registry snapshot; `getComponentsNeededDefault` (`:4288-4297`) uses the registry; delete `Node::addUserComponents/getUserCreatedComponents` (`Node.cpp:7714-7763`), `createdComponents` + mutex (`NodePrivate.h:455-456`), `Node.h:1329-1331`, `Node.cpp:1481-1484`; the choice-knob "New…" path (`Gui/KnobGuiChoice.cpp:312-340`) temporarily calls `Project::addLayer` (wired in T6).
  - verify: existing `ctest` (WriteAllLayers, TypedPassthrough, DataKind) green; new case in `Tests/WriteAllLayers_Test.cpp`: after loading `flat-three-layers.exr` through ReadOIIO, `getPresentLayers(-1)` on the Read = {Color, diffuse, specular} and `getAvailableLayers(-1)` ⊇ registry built-ins; a Blur downstream reports present = the same three. Builds without `NodeSerialization` changes (T4 does those).
  - size: M

- [ ] M38.P1.T4 — Drop `UserComponents` from node serialization and the PyPlug exporter
  - files: `Engine/NodeSerialization.h`, `Engine/NodeSerialization.cpp`, `Engine/NodeGroup.cpp`
  - approach: remove `_userComponents` (`NodeSerialization.h:236-238, 267, 321, 430`, `.cpp:202`), bump `NODE_SERIALIZATION_CURRENT_VERSION` (`:73`); remove the `addUserLayer` emission loop (`NodeGroup.cpp:2733-2745`); no migration (clean break, design §1.2).
  - verify: `ctest -R ProjectSerialization` and `DataKindProjectLoad` green; a saved `.ntp` contains no `UserComponents` element (test greps the file); `Tests/fixtures/*.ntp` still load.
  - size: S

- [ ] M38.P1.T5 — Auto-register produced layers on the main-thread refresh
  - files: `Engine/Node.cpp`, `Engine/Node.h`, `Engine/Project.cpp`, `Tests/LayerRegistry_Test.cpp`
  - approach: `Node::registerProducedLayers()` called at the end of `refreshAllInputRelatedData` (`Node.cpp:6614` region): produced non-Color planes from `getComponentsNeededAndProduced_public(comps[-1])` → `Project::addLayer(desc, isReader ? eOriginFile : eOriginPlugin)`; union results call `incrementKnobsAge()` on `getLayerUsers(id)`; batched under `isLoadingProject`; not gated on output data kind (M60 will call it from DeepRead). `Node::getReferencedLayerIDs()` virtual, implemented over `_imp->channelsSelectors`/`maskSelectors` choice values so `getLayerUsers` is truthful in 38.1.
  - verify: test: create ReadOIIO on `flat-three-layers.exr` → registry gains `diffuse`, `specular` with `eOriginFile`; change the file knob to a plain RGBA fixture → both stay registered; save/reset/load with the file unchanged → registry identical and `projectLayersChanged` count unchanged after load; select `diffuse` on a Blur's Output Layer choice → `removeLayer("diffuse")` returns false and `getLayerUsers` names the Blur.
  - size: M

- [ ] M38.P1.T6 — Project Layers page as a registry view; dialog validation; viewer lists present layers only
  - files: `Engine/KnobTypes.h`, `Gui/KnobGuiTable.cpp`, `Gui/KnobGuiTable.h`, `Gui/NewLayerDialog.cpp`, `Gui/ViewerTabPrivate.cpp`
  - approach: `KnobLayers` → three read-only columns Layer/Channels/Used by, `setIsPersistent(false)`, rebuilt from `snapshot()` on `projectLayersChanged`; `KnobGuiLayers::addNewUserEntry` → `Project::addLayer`, remove → `Project::removeLayer` with a refusal dialog listing the users, "Remove unused" button; delete `editUserEntry`/`tableChanged` rewriting; `NewLayerDialog::getComponents` validates via `LayerRegistry::validate` and surfaces the message instead of silently sanitising; the viewer's layer combo (`ViewerTabPrivate.cpp:385`) switches to `getPresentLayers`. The old Output Layer choice's "New…" entry is left on its T3 wiring (that knob is deleted in the widget phase).
  - verify: Xvfb run via `build/m38scout/` (recipe in `build/deeprepro/run-gui.sh`): script opens Project Settings → Layers, screenshots the page with Color…Forward, depth, and the two file layers of a loaded `flat-three-layers.exr` (Used by = 0/1); adding `spec2` through the page's "New" shows it in the table; removing `diffuse` while a Blur's Output Layer selects it shows the refusal dialog (screenshot); the viewer combo on the Read shows only Color/diffuse/specular (no project defaults).
  - size: L

- [ ] M38.P1.T7 — Python: project layer API on `App`; delete `Effect.addUserLayer`
  - files: `Engine/PyAppInstance.h`, `Engine/PyAppInstance.cpp`, `Engine/PyNode.h`, `Engine/PyNode.cpp`, `Engine/typesystem_engine.xml`
  - approach: `getProjectLayers/getProjectLayer/addProjectLayer(name, channels)/removeProjectLayer` per design §6, raising `ValueError` with the registry's message; remove `Effect::addUserLayer` (`PyNode.cpp:991-1009`, `PyNode.h:376`); `ImageLayer` unchanged.
  - verify: a Python script run through the built binary in background mode asserts: built-ins first, `addProjectLayer("diffuse", ["R","G","B"])` returns an `ImageLayer`, re-adding is idempotent, `addProjectLayer("rgba", …)` returns Color, `addProjectLayer("bad name", …)` raises, `removeProjectLayer("Color")` is False, `hasattr(node, "addUserLayer")` is False; `tools/ci/local/test.sh smoke debug` green.
  - size: S

**Phase 38.1 gate:** `tools/ci/local/test.sh ctest debug` and `test.sh smoke debug` green; the T6 Xvfb screenshots exist; a project saved with file layers reloads with an identical registry.

*(Phases 38.2+ — the three knob types, render semantics, premult removal, "New layer…", deletions, Read/Write/Tracker/Roto, docs, evidence — are written once the widget design doc is approved.)*

## Decisions

- 2026-09-19 — **Rescoped (user decision): M38 is a new layer/channel selection widget, absorbing M35.** Nuke-style but more powerful: dropdown None / All / Regex / list of layers, dynamic channel buttons matching the chosen layer's actual channels, "Add layer" adds another instance (instances beyond the first have a remove button). Nodes process exactly the selected layers/channels in place; non-Shuffle nodes get no shuffle capability. Not the earlier "reorder the existing knobs" proposal.
- 2026-09-19 — **Clean break for old projects** (user decision): no load-time mapping of `processAllLayers` / `channels` / R,G,B,A values; old nodes come up on the default selection. `NATRON_CACHE_VERSION` bumps.
- 2026-09-19 — **Plugin-declared R/G/B/A rows are adopted into the widget, Merge included** (user decision): the 28 openfx-misc plugins' `NatronOfxParamProcess*` rows are hidden and driven by the widget; Merge uses a widget *variant* with no add-rows and no regex. Goal is one consistent channel-selection UI across essentially all nodes with small variations for specific purposes — treated as a foundational design, written up in `PLAN/DESIGN/2026-09-19-layer-channel-widget.md` for approval before implementation.
- 2026-09-19 — **Regex matches layer labels, whole-string anchored, case-sensitive, `QRegularExpression`** (user decision): users type what they see.
- 2026-09-19 — **ZRemap and ZMask PyPlugs are dropped until M34** (user decision): they depend on the implicit shuffle; the native Shuffle milestone re-adds them.
- 2026-09-19 — **One undo step per user action**; the regex editor commits on `editingFinished`, not per keystroke.
- 2026-09-19 — **Design review round 1 (user)**: (a) prefer *three knob types* by value shape — a channel **set** (multi-row, None/All on row 0 only, regex), a single **layer** select (optional channel buttons), a single **channel** select — over one knob with variant flags; the shared part is the GUI row and the population source, not the knob. (b) **Merge is out of M38**: it becomes a native node in its own milestone (A/B/output layer+channel selects, an "also merge" channel set, an alpha rule); the OFX Merge is left alone and may break. (c) **M43 (drop premult) and M36 ("New layer…") fold into M38** as phases. (d) Read gets *no* channel mechanics (its `outputComponents` param goes); Write gets the full channel set; Tracker selects exactly one layer; Roto/RotoPaint select exactly one layer with channels. (e) Open questions answered: full channel set even for Transform/Switch-style nodes; reset channels to all-on when the layer changes; Roto in M38. (f) Deep layers/channels are a hole (no layer grouping, processing nodes hardcode RGBA) → new milestone **M60** after M38, not a phase here.
- 2026-09-19 — **Blocking design gap: script-level layer knowledge.** Natron has no project-wide layer registry — user-created layers live on the node that created them and only appear downstream, so a Roto/generator/Shuffle cannot target a layer that is not yet upstream, and "New layer…" cannot mean what it means in Nuke. The user's call: a global layer cannot live on a node; design the registry first, then return to the widget's open questions.
- 2026-09-19 — **Registry design approved** (`PLAN/DESIGN/2026-09-19-layer-registry.md`) with one amendment: **input-bound knobs and the viewer list present layers only; target knobs (Roto output, generators, Shuffle output) list the registry and carry "New layer…"** — greyed registry entries in every input list are clutter. Removal refused while referenced + "Remove unused"; `depth [Z]` pre-registered; Python surface on `App`. Phase 38.1 elaborated from it.
