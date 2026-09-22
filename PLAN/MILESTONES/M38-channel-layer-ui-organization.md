# Milestone 38: Layer/channel selection widget: process-in-place, no implicit shuffling (absorbs M35, M43, M36)

Governing design docs: `PLAN/DESIGN/2026-09-19-layer-registry.md` (Phase 38.1, approved
2026-09-19 with the "present-only input lists" amendment) and
`PLAN/DESIGN/2026-09-19-layer-channel-widget.md` (Phases 38.2+, v3, approved 2026-09-19 — three knob
types: channel set / one layer / one channel). Later phases
are elaborated once the widget doc is approved; M43 (drop premult) and M36 ("New layer…")
become phases here.

## Phase 38.1: Project-level layer registry

- [x] M38.P1.T1 — Add `LayerRegistry` with validation, reserved aliases, union rule and channel grouping
  - files: `Engine/LayerRegistry.h`, `Engine/LayerRegistry.cpp`, `Engine/CMakeLists.txt`, `Tests/LayerRegistry_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: value class per design §1.1 (built-ins seeded in the `Project.cpp:1096-1101` order, `depth [Z]` as `eOriginUser`), `add/remove/find/contains/snapshot`, `validate(desc, fromFile, error)` with `kLayerMaxChannels = 4`, `reservedAlias`, union-on-file-conflict returning an `eAddResult {added, unchanged, grown, refused}`, `groupChannelNames` mirroring `ReadOIIO.cpp:1138-1172`. Pure data + `QMutex` + `shared_ptr<const vector>` snapshot; no Qt signal here.
  - verify: `ctest -R LayerRegistry`: EXPECT_* on built-in order, `rgba`→Color alias, `none/all/Backward` refused, dotted ID refused unless `fromFile`, 5 channels refused, duplicate identical → unchanged, file union RGB+RGBA → grown, user conflict → refused, remove built-in refused, `groupChannelNames({"R","G","B","A","Z","diffuse.R","diffuse.G"})` → Color, depth, diffuse; a snapshot taken before an `add` is unchanged after it.
  - size: M

- [x] M38.P1.T2 — Make `Project` own and serialize the registry; emit `projectLayersChanged`
  - files: `Engine/ProjectPrivate.h`, `Engine/Project.h`, `Engine/Project.cpp`, `Engine/ProjectSerialization.h`, `Tests/ProjectSerialization_Test.cpp`
  - approach: `ProjectPrivate::layers` replaces `defaultLayersList` as the source of truth (the knob stays for T6, marked non-persistent); `Project::addLayer/removeLayer/getLayerRegistry/getLayerUsers`; delete `getProjectDefaultLayers/addProjectDefaultLayer/getProjectDefaultLayerNames` (`Project.cpp:1478-1580`) and the `defaultLayersList` branch of `knobChanged` (`:1776-1781`); `ProjectSerialization::_layers` (`Layers`, non-built-ins only, with `Origin`), version 6→7, restored in `ProjectPrivate::restoreFromSerialization` after formats (`ProjectPrivate.cpp:141`) and before knobs; signal `projectLayersChanged()` (`Project.h:392` pattern), suppressed while `isLoadingProject` and emitted once after `ProjectPrivate.cpp:236`; on change call `refreshChannelSelectors()` on all nodes.
  - verify: extend `ProjectSerialization_Test`: add `diffuse` + `specular`, save, reset, load → registry has both in order, built-ins not written to the XML (grep the saved file), `projectLayersChanged` emitted exactly once on load (QSignalSpy). Independent of T3: uses only `Project` API.
  - size: M

- [x] M38.P1.T3 — Route availability and OFX publication through the registry; delete per-node user components
  - files: `Engine/EffectInstance.cpp`, `Engine/EffectInstance.h`, `Engine/Node.cpp`, `Engine/Node.h`, `Engine/NodePrivate.h`
  - approach: `getAvailableLayers` (`EffectInstance.cpp:4496-4517`) merges one registry snapshot for `inputNb == -1` only; new `getPresentLayers` (produced ∪ pass-through, `:4466-4493, 4511`); `getUserLayers` (`:182-196`) = registry snapshot; `getComponentsNeededDefault` (`:4288-4297`) uses the registry; delete `Node::addUserComponents/getUserCreatedComponents` (`Node.cpp:7714-7763`), `createdComponents` + mutex (`NodePrivate.h:455-456`), `Node.h:1329-1331`, `Node.cpp:1481-1484`; the choice-knob "New…" path (`Gui/KnobGuiChoice.cpp:312-340`) temporarily calls `Project::addLayer` (wired in T6).
  - verify: existing `ctest` (WriteAllLayers, TypedPassthrough, DataKind) green; new case in `Tests/WriteAllLayers_Test.cpp`: after loading `flat-three-layers.exr` through ReadOIIO, `getPresentLayers(-1)` on the Read = {Color, diffuse, specular} and `getAvailableLayers(-1)` ⊇ registry built-ins; a Blur downstream reports present = the same three. Builds without `NodeSerialization` changes (T4 does those).
  - size: M

- [x] M38.P1.T4 — Drop `UserComponents` from node serialization and the PyPlug exporter
  - files: `Engine/NodeSerialization.h`, `Engine/NodeSerialization.cpp`, `Engine/NodeGroup.cpp`
  - approach: remove `_userComponents` (`NodeSerialization.h:236-238, 267, 321, 430`, `.cpp:202`), bump `NODE_SERIALIZATION_CURRENT_VERSION` (`:73`); remove the `addUserLayer` emission loop (`NodeGroup.cpp:2733-2745`); no migration (clean break, design §1.2).
  - verify: `ctest -R ProjectSerialization` and `DataKindProjectLoad` green; a saved `.ntp` contains no `UserComponents` element (test greps the file); `Tests/fixtures/*.ntp` still load.
  - size: S

- [x] M38.P1.T5 — Auto-register produced layers on the main-thread refresh
  - files: `Engine/Node.cpp`, `Engine/Node.h`, `Engine/Project.cpp`, `Tests/LayerRegistry_Test.cpp`
  - approach: `Node::registerProducedLayers()` called at the end of `refreshAllInputRelatedData` (`Node.cpp:6614` region): produced non-Color planes from `getComponentsNeededAndProduced_public(comps[-1])` → `Project::addLayer(desc, isReader ? eOriginFile : eOriginPlugin)`; union results call `incrementKnobsAge()` on `getLayerUsers(id)`; batched under `isLoadingProject`; not gated on output data kind (M60 will call it from DeepRead). `Node::getReferencedLayerIDs()` virtual, implemented over `_imp->channelsSelectors`/`maskSelectors` choice values so `getLayerUsers` is truthful in 38.1.
  - verify: test: create ReadOIIO on `flat-three-layers.exr` → registry gains `diffuse`, `specular` with `eOriginFile`; change the file knob to a plain RGBA fixture → both stay registered; save/reset/load with the file unchanged → registry identical and `projectLayersChanged` count unchanged after load; select `diffuse` on a Blur's Output Layer choice → `removeLayer("diffuse")` returns false and `getLayerUsers` names the Blur.
  - size: M

- [x] M38.P1.T6 — Project Layers page as a registry view; dialog validation; viewer lists present layers only
  - files: `Engine/KnobTypes.h`, `Gui/KnobGuiTable.cpp`, `Gui/KnobGuiTable.h`, `Gui/NewLayerDialog.cpp`, `Gui/ViewerTabPrivate.cpp`
  - approach: `KnobLayers` → three read-only columns Layer/Channels/Used by, `setIsPersistent(false)`, rebuilt from `snapshot()` on `projectLayersChanged`; `KnobGuiLayers::addNewUserEntry` → `Project::addLayer`, remove → `Project::removeLayer` with a refusal dialog listing the users, "Remove unused" button; delete `editUserEntry`/`tableChanged` rewriting; `NewLayerDialog::getComponents` validates via `LayerRegistry::validate` and surfaces the message instead of silently sanitising; the viewer's layer combo (`ViewerTabPrivate.cpp:385`) switches to `getPresentLayers`. The old Output Layer choice's "New…" entry is left on its T3 wiring (that knob is deleted in the widget phase).
  - verify: Xvfb run via `build/m38scout/` (recipe in `build/deeprepro/run-gui.sh`): script opens Project Settings → Layers, screenshots the page with Color…Forward, depth, and the two file layers of a loaded `flat-three-layers.exr` (Used by = 0/1); adding `spec2` through the page's "New" shows it in the table; removing `diffuse` while a Blur's Output Layer selects it shows the refusal dialog (screenshot); the viewer combo on the Read shows only Color/diffuse/specular (no project defaults).
  - size: L

- [x] M38.P1.T7 — Python: project layer API on `App`; delete `Effect.addUserLayer`
  - files: `Engine/PyAppInstance.h`, `Engine/PyAppInstance.cpp`, `Engine/PyNode.h`, `Engine/PyNode.cpp`, `Engine/typesystem_engine.xml`
  - approach: `getProjectLayers/getProjectLayer/addProjectLayer(name, channels)/removeProjectLayer` per design §6, raising `ValueError` with the registry's message; remove `Effect::addUserLayer` (`PyNode.cpp:991-1009`, `PyNode.h:376`); `ImageLayer` unchanged.
  - verify: a Python script run through the built binary in background mode asserts: built-ins first, `addProjectLayer("diffuse", ["R","G","B"])` returns an `ImageLayer`, re-adding is idempotent, `addProjectLayer("rgba", …)` returns Color, `addProjectLayer("bad name", …)` raises, `removeProjectLayer("Color")` is False, `hasattr(node, "addUserLayer")` is False; `tools/ci/local/test.sh smoke debug` green.
  - size: S

- [x] M38.P1.T8 — Key an input's components-needed answer on the input's own hash in `getPresentLayers`/`getAvailableLayers`
  - files: `Engine/EffectInstance.cpp`, `Tests/LayerRegistry_Test.cpp`
  - approach: both helpers query `input->getComponentsNeededAndProduced_public(getRenderHash(), …)` with the *caller's* hash, so the input's `ActionsCache` fills with foreign-hash entries and can serve a stale plane list (the M58 decisions noted the same for `getAvailableLayers`; the 38.1 gate fix `3429d3640` only masks it via metadata invalidation). Pass `input->getRenderHash()` (or the input node's hash, matching what the input's own render path uses) instead; keep `-1`'s own-hash path unchanged.
  - verify: gtest: reader → Blur; call `blur->getPresentLayers(0)` then change the reader's file to `flat-rgba-only.exr` (metadata refresh) and call again → the second answer has no `diffuse`/`specular` without any change to the Blur's hash; full ctest and smoke green.
  - size: S

**Phase 38.1 gate:** `tools/ci/local/test.sh ctest debug` and `test.sh smoke debug` green; the T6 Xvfb screenshots exist; a project saved with file layers reloads with an identical registry.

## Phase 38.2: Premult removal (M43)

- [x] M38.P2.T1 — Extend the Xvfb harness and take the "before" shots
  - files: `build/m38scout/gui.py`, `build/m38scout/dump.py`, `build/m38scout/run.sh`
  - approach: `dump.py` covers Roto, RotoPaint, Tracker, Constant, Ramp, ReadOIIO, WriteOIIO, Grade, Premult, Unpremult in addition to the 12 ids at `dump.py:5`; `gui.py` gains `NODE=` cases that connect a Read of `Tests/fixtures/flat-three-layers.exr` (copy it under `build/` first — fixtures must live under `build/`) and a Grade with A checked and R unchecked so the premult warning is visible; screenshots `before-<tag>.png`; `run.sh` keeps the `Xvfb :79 +extension GLX` / `LIBGL_ALWAYS_SOFTWARE=1` recipe (`build/deeprepro/run-gui.sh:5-13`).
  - verify: `build/m38scout/run.sh` produces `before-grade-0.png` showing the warning icon and `dump.txt` sections for every listed id (grep the `##` headers).
  - size: S

- [x] M38.P2.T2 — Render path stops deciding anything from premult
  - files: `Engine/EffectInstanceRenderRoI.cpp`, `Engine/EffectInstance.cpp`, `Engine/EffectInstance.h`, `Engine/EffectInstanceRenderDeep.cpp`
  - approach: delete the `outputComponents.front()` decisions (`EffectInstanceRenderRoI.cpp:661-669, 1283-1287`), `thisEffectOutputPremult` (`:373`), `ImagePlanesToRender::inputPremult/outputPremult` (`EffectInstance.h:1613-1622`, filled `:1314-1327`); `convertLayersFormatsIfNeeded` always `requiresUnpremult = false` (`:165, 189-218`); `renderHandler` loses `originalImagePremultiplication` (`EffectInstance.cpp:2249-2263`) and `unPremultRequired` (`:2719-2726, 2756, 2830, 2862`); temporarily pass `eImagePremultiplicationPremultiplied` to `Image` ctors (`:1384, 1452`, `EffectInstanceRenderDeep.cpp:623`) — the enum dies in T6.
  - verify: `tools/ci/local/test.sh ctest debug` green (WriteAllLayers pixel assertions are an RGBA round trip); `Tests/Image_Test.cpp:261-312` still pass unchanged.
  - size: M

- [x] M38.P2.T3 — Metadata and OFX boundary: no tracking, constant answer
  - files: `Engine/NodeMetadata.h`, `Engine/NodeMetadata.cpp`, `Engine/EffectInstance.cpp`, `Engine/OfxClipInstance.cpp`, `Engine/OfxImageEffectInstance.cpp`
  - approach: remove `outputPremult` from `NodeMetadata` (`.cpp:52-53, 77, 95, 135, 187-195`) and `EffectInstance::getPremult` (`EffectInstance.cpp:5564-5569`, decl `.h:950-952`); delete the derivation/forcing in `getDefaultMetadata`/`checkMetadata` (`:5341-5369, 5459-5462, 5481-5485, 5747-5752`) and the `:5675` warning hook; `OfxClipInstance::getPremult` (`:270-290`) returns `kOfxImageUnPreMultiplied` always, per-image property `:1490` likewise; `OfxImageEffectInstance` stops reading the plugin's answer back (`:1304`) and `updatePreferences_safe` (`:1331-1347`) loses the argument; `OfxEffectInstance::ofxGetOutputPremultiplication` (`.cpp:2985-2987`) returns the constant. Callers (`Node.cpp:2095-2109` info tooltip, RenderStats) compile against the constant until T6.
  - verify: `ctest` green; a Python background script connects Read→Grade and asserts `grade.getParam("premult").get()` stays False after connection (the auto-toggle no longer fires); `hasattr(effect, "getPremult")` still True until T6.
  - size: M

- [x] M38.P2.T4 — Remove the warning, RotoPaint's premultiply knob, and hide plugin premult params
  - files: `Engine/Node.cpp`, `Engine/NodePrivate.h`, `Engine/Node.h`, `Engine/RotoPaint.cpp`, `Engine/OfxEffectInstance.cpp`
  - approach: delete `premultWarning` (`NodePrivate.h:404`, creation `Node.cpp:2691-2706`, `checkForPremultWarningAndCheckboxes` `:7547-7611`, `Node.h:1064`, trigger `:5545`); delete RotoPaint `premultiply` (`RotoPaint.cpp:217-227`, use `:1491-1511, 1650`) and the `premultImage` call; `OfxEffectInstance` after param creation sets `premult`, `premultChanged`, `premultChannel`, `filePremult`, `outputPremult`, `inputPremult` secret + non-persistent when present (Read/Write containers inherit this since embedded params are container knobs, `Engine/OfxParamInstance.cpp:305-330`); drop the `premultChannel` load filter (`KnobSerialization.cpp:619`) as redundant; `Tests/DeepPipeline_Test.cpp:262-267` loses its `inputPremult` line.
  - verify: `dump.py` (T1) shows no visible `premult*`/`filePremult`/`outputPremult`/`inputPremult`/`premultiply` param on Grade, Read, Write, RotoPaint; Xvfb `after-grade-0.png` shows no warning icon with A on / R off; `ctest -R DeepPipeline` green.
  - size: M

- [x] M38.P2.T5 — Viewer without premult state
  - files: `Engine/ViewerInstance.cpp`, `Engine/UpdateViewerParams.h`, `Engine/ViewerInstancePrivate.h`, `Gui/ViewerGL.cpp`, `Gui/ViewerGLPrivate.cpp`
  - approach: drop `srcPremult` (`ViewerInstance.cpp:915`, `UpdateViewerParams.h:72, 121`, `ViewerInstancePrivate.h:77, 90, 106`, `OpenGLViewerI.h:127`, `ViewerGL.h:184`, `ViewerGLPrivate.h:108, 128`); `scaleToTexture{8,32}bitsForPremult` (`:2444-2508, 2800-2882`) become `scaleToTexture{8,32}bits` and choose the alpha-is-one template iff the image has no alpha channel (2 or 3 components) — rename the `opaque` template parameter (`ViewerInstance.cpp:2150, 2380, 2391`) to `noAlphaChannel` so no premult-era vocabulary survives in the viewer; `BlendSetter` (`ViewerGL.cpp:230-250`, `ViewerGLPrivate.cpp:621-640`, uses `:381-460`) always premultiplied blending (`GL_ONE, GL_ONE_MINUS_SRC_ALPHA`) when a checkerboard/wipe blend is needed.
  - verify: Xvfb screenshots of the viewer over the checkerboard for (a) the RGBA fixture, (b) an RGB Constant, before (T1) and after — pixel-diff within 1/255 on both.
  - size: M

- [x] M38.P2.T6 — Delete `ImagePremultiplicationEnum` end to end; cache version 5→6
  - files: `Global/Enums.h`, `Engine/Image.h`, `Engine/Image.cpp`, `Engine/ImageParams.h`, `Engine/ImageParamsSerialization.h`, `Global/GlobalDefines.h` (plus the mechanical ctor-argument sweep in `ImageCopyChannels.cpp`, `ImageConvert.cpp`, `RotoContext.cpp`, `RotoSmear.cpp`, `RenderStats.*`, `OutputEffectInstance.cpp`, `Gui/RenderStatsDialog.cpp`, `Engine/PyNode.*`, `typesystem_engine.xml:222`, tests)
  - approach: remove `Image::_premult` and every ctor/`makeParams` argument, `premultImage/unpremultImage`, the `premult/originalPremult/ignorePremult` template parameters of `copyUnProcessedChannels*` and `requiresUnpremult` of `convertToFormat*`; `ImageParams::_premult` and its nvp (`ImageParamsSerialization.h:112`); `Effect::getPremult` and the enum from the typesystem; RenderStats "Output Premult" column; `#define NATRON_CACHE_VERSION 6`. One compile-driven sweep; no behaviour change expected after T2-T5.
  - verify: `grep -rn -i "ImagePremultiplicationEnum\|getPremult\|outputPremult\|inputPremult\|eImagePremultiplication" Engine Gui Global Tests` returns zero hits and `grep -rn -i opaque Engine Gui Global | grep -v -i 'setOpaque\|WA_Opaque\|OpaquePaintEvent'` returns zero hits (the OFX `kOfxImageOpaque` string may remain only inside `libs/OpenFX`, never answered by the host); full `ctest` green; a stale disk cache from a previous build is discarded on first launch (log line).
  - size: L

- [x] M38.P2.T7 — openfx-misc fork: Premult/Unpremult always do their math
  - files: `tools/ci/local/fetch-assets.sh` (+ fork `charlesangus/openfx-misc`: `Premult/Premult.cpp`)
  - approach: in the fork delete every read of the clip premult property in `Premult.cpp`: the `isIdentity` shortcuts (`:756-770`), the `changedClip` quad auto-toggle (`:870-880`) and the `eImageOpaque` alpha-as-1 branch (`:644`) — Premult always multiplies by alpha, Unpremult always divides, the user owns knowing which state an image is in; `OPENFX_MISC_REPO` → the fork, `OPENFX_MISC_REF` → the SHA, add "delta 1" to the comment block (`:227-235`) in the style of `:152-211`.
  - verify: a Python background render of Constant(RGBA, 0.5 alpha)→Premult→Write and →Unpremult→Write: pixels are `rgb*a` and `rgb/a` respectively (read back with `Tests/FlatExrReader.h`-style parsing or a gtest added to `Tests/`).
  - size: S

- [x] M38.P2.T8 — openfx-io fork: readers and writers never convert premultiplication
  - files: `tools/ci/local/fetch-assets.sh` (`OPENFX_IO_REF` + fork-delta comment), `Tests/DeepPipeline_Test.cpp` (+ fork `charlesangus/openfx-io`: `IOSupport/GenericWriter.cpp`, `IOSupport/GenericReader.cpp`)
  - approach: in the fork, `GenericWriter::changedClip` stops re-deriving `inputPremult` from `_inputClip->getPreMultiplication()`, and the write path never multiplies or divides by alpha (delete the `inputPremult`-driven conversion; the param can stay declared, hidden host-side); `GenericReader` likewise never converts between `filePremult` and `outputPremult` — pixels are decoded as stored. Bump `OPENFX_IO_REF` to the merged commit and extend the delta comment. Then drop the `inputPremult` override 38.2.T4 had to keep in `Tests/DeepPipeline_Test.cpp`.
  - verify: `ctest -R "DeepPipeline|WriteAllLayers"` green with the override removed; a Python background render Constant(RGBA, rgb=1, a=0.5) → WriteOIIO EXR → read back with `Tests/FlatExrReader.h`-style parsing gives rgb=1 (not 0.5); Read of that EXR → Write PNG/EXR round-trips rgb=1; `tools/ci/local/test.sh smoke debug` green.
  - size: M

## Phase 38.3: The three knob types (engine)

- [x] M38.P3.T1 — `KnobTable` groundwork: fixed tags, expression refusal, alias branch
  - files: `Engine/KnobTypes.h`, `Engine/KnobTypes.cpp`, `Engine/Knob.h`, `Engine/Knob.cpp`, `Tests/Knob_Test.cpp`
  - approach: `KnobTable::getColumnTag(col)` (ASCII, used by the codec) separate from `getColumnLabel` (display; `KnobLayers` keeps `tr()` for display only); `KnobI::supportsExpressions()` default true, false on `KnobTable`, checked in `KnobHelper::setExpressionInternal` (`Knob.cpp:2825-2889`); `createDuplicateOnHolder` (`Knob.cpp:4525-4667`) gains a `KnobTable` branch (dispatching on `typeName()` to the concrete `BuildKnob`).
  - verify: gtest: a `KnobLayers` round trip encodes `<Name>`/`<Channels>` regardless of `QLocale`; `setExpression` on a table knob throws `std::invalid_argument`; `createDuplicateOnHolder` of a `KnobLayers` returns non-null with the same `typeName()`.
  - size: S

- [x] M38.P3.T2 — `KnobChannelSet` with `resolve()` and summary
  - files: `Engine/KnobChannelSet.h`, `Engine/KnobChannelSet.cpp`, `Engine/CMakeLists.txt`, `Tests/KnobChannelSet_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: §1.1 exactly: rows/codec on `KnobTable` with tags `Mode/Layer/Channels`, the row-0-only `none/all` invariant enforced in the setters, per-row compiled `QRegularExpression` cache swapped under a `QMutex` on value change, pure `resolve(present)`, `getSummary()`, `getReferencedLayerIDs()` (feeds the registry's used-by).
  - verify: `ctest -R KnobChannelSet`, EXPECT_* on: default Color row; `none`→empty; `all`→every present layer/all bits; layer row absent→nothing; enabled names ∩ actual channels; `depth[Z]`→bit 3; regex `spec.*` anchored (`specular` yes, `xspecular` no, `Specular` no); invalid regex→nothing; duplicate IDs OR-ed; Color first; codec round trip with a pattern containing `<`, `&` and a space; `setNone` on row 1 throws.
  - size: M

- [x] M38.P3.T3 — `KnobLayerSelect` and `KnobChannelSelect`
  - files: `Engine/KnobLayerSelect.h`, `Engine/KnobLayerSelect.cpp`, `Engine/KnobChannelSelect.h`, `Engine/KnobChannelSelect.cpp`, `Tests/KnobLayerSelect_Test.cpp`
  - approach: §1.2/§1.3; `KnobLayerSelect(withChannelButtons)` with `setLayer` resetting channels to all; `KnobChannelSelect` value `layerID.C` or empty; both expose `resolve(list)` and `getReferencedLayerIDs()`; registered in `KnobFactory.cpp:79-96`, `KnobSerialization.cpp:143-185`.
  - verify: gtest: `setLayer("diffuse")` after `setChannels({"R"})` on Color yields all of diffuse's channels; a buttonless select serialises an empty `Channels` cell and resolves to all bits; channel select `Color.A` resolves to (Color, 3), `diffuse.G` to (diffuse, 1), unknown → none; both survive a `KnobSerialization` save/load round trip by `typeName()`.
  - size: M

- [x] M38.P3.T4 — Python wrappers and creation functions
  - files: `Engine/PyParameter.h`, `Engine/PyParameter.cpp`, `Engine/PyNode.h`, `Engine/PyNode.cpp`, `Engine/typesystem_engine.xml`
  - approach: `ChannelSetParam`, `LayerSelectParam`, `ChannelSelectParam` per §1.5 (list-out-param idiom from `PathParam`, `typesystem_engine.xml:1404ff`); branches in `createParamWrapperForKnob` (`PyNode.cpp:390-456`); `Effect.createChannelSetParam/createLayerSelectParam/createChannelSelectParam(name, label)`; include from `Engine/PySide6_Engine_Python.h`.
  - verify: a Python background script on a bare `KnobHolder`-backed node (any Blur, knobs from T5 not required — create the param with `createChannelSetParam` on a Group): `getRows()` shapes, `setLayer/setChannels/addRegex/removeRow`, `removeRow(0)` raises `ValueError`, `setAsAlias` between two groups works.
  - size: M

- [x] M38.P3.T5 — Nodes create the knobs: eligibility, placement, quad adoption, mask selects, listing
  - files: `Engine/EffectInstance.h`, `Engine/EffectInstance.cpp`, `Engine/Node.h`, `Engine/Node.cpp`, `Engine/NodePrivate.h`
  - approach: `getLayerKnobSpec()` (§3) replacing `getCreateChannelSelectorKnob` (`EffectInstance.cpp:4522-4526`); `initializeDefaultKnobs` inserts the knob at main-page index 0 where `findOrCreateChannelEnabled` was called (`Node.cpp:2794`); `adoptChannelQuad` replaces `findOrCreateChannelEnabled` (`:2633-2707`): a plugin quad `NatronOfxParamProcessR..A` seeds the Color row / layer-select buttons, goes secret + non-persistent + forced true, the host never creates its own bools; `createMaskSelectors` (`:2493-2561`) makes `KnobChannelSelect`s; `Node::listLayersForKnob` (§1.4, Color always listed for input-bound); `getReferencedLayerIDs` aggregates the three types; old choice/bool knobs remain alive but secret until 38.8 so the render still works through the old path.
  - verify: `dump.py` shows `[ChannelSet] channels` first on CImgBlur/Grade/Transform, `[LayerSelect] layer` first on Constant/Ramp, `[ChannelSelect] maskChannel_Mask` on Blur, none of the three on DeepMerge/Shuffle/Premult; Grade's default row is `Color R,G,B`; `getLayerUsers("diffuse")` names a Blur whose row 1 is `diffuse`.
  - size: L

## Phase 38.4: Render model

- [x] M38.P4.T1 — `comps` from the knobs, per-plane bitsets, `processAllRequested` deleted
  - files: `Engine/EffectInstance.cpp`, `Engine/EffectInstance.h`, `Engine/EffectInstancePrivate.h`, `Engine/EffectInstanceRenderRoI.cpp`, `Engine/OutputSchedulerThread.cpp`
  - approach: §5(1): `getComponentsNeededDefault` (`EffectInstance.cpp:4219-4372`) builds `comps[-1]`/`comps[i]` from `resolve(getPresentLayers(...))` / the layer select / channel selects; ActionsCache entry stores `map<ImageLayerDesc, bitset<4>>` (`EffectInstancePrivate.h:70, 130-133`); remove the `processAllRequested` out-param from `getComponentsNeededAndProduced_public` (`:4375-4450`) and the dead block `EffectInstanceRenderRoI.cpp:457-476`; update callers `:450`, `OutputSchedulerThread.cpp:2297`, `RotoSmear.cpp:243`, `EffectInstance.cpp:4476`.
  - verify: gtest on Read(`flat-three-layers.exr`)→Blur: with rows `Color` + `diffuse[R,G]`, `getComponentsNeededAndProduced_public` yields `comps[-1] = {Color, diffuse}` with bitsets `1111`/`0011`... (index order `R,G` → bits 0,1) and `comps[0]` equal; with `All` → all three planes; with a regex matching nothing → Color only from metadata; `TypedPassthrough`/`DataKind` tests green.
  - size: M

- [x] M38.P4.T2 — Per-plane input fetch and per-plane host masking
  - files: `Engine/OfxClipInstance.cpp`, `Engine/EffectInstance.cpp`, `Engine/Node.cpp`
  - approach: §5(2)(3): `clipGetImage` (`OfxClipInstance.cpp:853-874`) picks the entry equivalent to `outputLayerBeingRendered`; `renderHandler` chooses `originalInputImage` and the bitset per plane in the loop at `EffectInstance.cpp:2618` (today `:2250-2258`); `Node::getProcessChannel` (`:5773-5785`) is deleted with `hostChannelSelectorEnabled` — the host masks whenever the node has channel buttons.
  - verify: gtest: Read→Invert(`Color R only` + `diffuse[G]`)→Write All: Color = (0,0,0,1) inverted R only i.e. `(0,0,0,1)`, diffuse = `(0,0,0)` (G inverted from 1 to 0, R/B untouched), specular untouched `(0,0,1)`; Read→Grade with `A` off leaves alpha bit-identical.
  - size: L

- [x] M38.P4.T3 — Identity on empty; delete "choice B"
  - files: `Engine/Node.cpp`, `Engine/EffectInstanceRenderRoI.cpp`
  - approach: `hasAtLeastOneChannelToProcess` (`Node.cpp:5846-5865`) = resolved set non-empty with a set bit; delete the `getChannelSelectorKnob(inputNbIdentity)` branch (`EffectInstanceRenderRoI.cpp:630-680`), always choice A.
  - verify: gtest: Blur with row 0 `None` renders bit-identical to its input for all three planes and reports identity; Blur with `diffuse` only leaves Color bit-identical.
  - size: S

- [x] M38.P4.T4 — Regression guards for adopted plugins
  - files: `Tests/ChannelSetRender_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: for Grade, Invert, ColorCorrect, Multiply, Saturation, CImgBlur on `flat-three-layers.exr`: full-channel result equals the pre-38 plugin-masked result (golden values computed by the plugin's own math on the constant fixture), single-channel results touch only that channel, non-Color rows touch only that plane; audit the openfx-misc grep list (`grep -l NatronOfxParamProcessR build/assets/plugin-src/openfx-misc/*/*.cpp`) and record in the test file which plugins read `processX` outside masking (none expected).
  - verify: `ctest -R ChannelSetRender` green; the audit list is in the test's header comment.
  - size: M

- [x] M38.P4.T5 — Quad-adoption exception list: KeyMix, DenoiseSharpen, ClipTest keep their own R/G/B/A
  - files: `Engine/Node.cpp`, `Engine/Node.h`, `Tests/ChannelSetRender_Test.cpp`
  - approach: the 38.4.T4 audit found three openfx-misc plugins whose `NatronOfxParamProcess*` quad is not a mask: KeyMix (`net.sf.openfx.KeyMix` — the quad picks A vs B per channel), DenoiseSharpen (`net.sf.openfx.DenoiseSharpen` — collapses R/G/B into one flag), ClipTest (`net.sf.openfx.ClipTestPlugin` — zebra decision ORs across selected channels). `adoptChannelQuad` consults a small host-side set of plugin IDs (verify the exact IDs from the bundle) for which the quad is left visible and untouched, the channel set's row-0 buttons are hidden for that node (the plugin owns per-channel behaviour) but the set still drives which layers are rendered, and the host bitset for that node's planes is all-true (plugin masks). Document the list in one why-comment next to the set.
  - verify: gtest: KeyMix's `NatronOfxParamProcessR` is visible and not forced; render KeyMix (A=reader on `flat-three-layers.exr`, B=reader on `flat-rgba-only.exr`, quad R on / G,B,A off) → output R from A, G/B/A from B (whatever the plugin's semantics are — assert against the plugin's own math); Grade's quad remains adopted; `ctest -R ChannelSetRender` green; full ctest green.
  - size: S

## Phase 38.5: GUI

- [x] M38.P5.T1 — `LayerChannelRow` widget
  - files: `Gui/LayerChannelRow.h`, `Gui/LayerChannelRow.cpp`, `Gui/CMakeLists.txt`
  - approach: §2: combo (entries by mode), checkable coloured `Button`s by channel name (`KnobGuiBool.cpp:273-300` constants), regex `LineEdit` + matches label validated on `editingFinished`, `[−]`; signals `layerChosen(id)`, `channelToggled(name, on)`, `patternCommitted(p)`, `removeRequested()`, `newLayerRequested()`; `setAbsentMarker(text)` inserts/removes the single marker item; no knob dependency (unit-testable with a fake list).
  - verify: an offscreen `QApplication` gtest (`QT_QPA_PLATFORM=offscreen`): entries order per mode, buttons rebuilt on layer change with all checked, invalid pattern sets the `dirty` property and tooltip, marker item present only while set; `clang-format` gate passes.
  - size: M

- [x] M38.P5.T2 — `KnobGuiChannelSet` with add/remove and one undo step per action
  - files: `Gui/KnobGuiChannelSet.h`, `Gui/KnobGuiChannelSet.cpp`, `Gui/KnobGuiFactory.cpp`, `Gui/KnobUndoCommand.h`
  - approach: composite `QVBoxLayout` of rows + `[+ Add layer]` (precedent `KnobGuiTable.cpp:206-281`); every row signal → one `KnobUndoCommand<std::string>` with the new `setMergeable(false)`; row-0 `None/All` greys rows 1+; repopulate on `onChannelsSelectorRefreshed` and `projectLayersChanged`; no expression menu entry when `!supportsExpressions()`.
  - verify: Xvfb via `build/m38scout/`: Blur panel screenshot `panel-blur-set-0.png` matches the §2 mockup with rows Color / diffuse / regex; scripted Ctrl+Z after "add row → choose diffuse → toggle G" leaves exactly three undo entries and restores the panel state each step (assert via `getRows()` from `gui.py`).
  - size: L

- [x] M38.P5.T3 — `KnobGuiLayerSelect` and `KnobGuiChannelSelect`
  - files: `Gui/KnobGuiLayerSelect.h`, `Gui/KnobGuiLayerSelect.cpp`, `Gui/KnobGuiChannelSelect.h`, `Gui/KnobGuiChannelSelect.cpp`, `Gui/KnobGuiFactory.cpp`
  - approach: one `LayerChannelRow` each; same-line placement respected for the mask footer (`setAddNewLine(false)` chain, `KnobGuiContainerHelper.cpp:678-698`); target-role combos append the sentinel (wired in 38.7).
  - verify: Xvfb: Blur's mask footer screenshot shows `☑ [ Color.A ▾ ] □ Invert Mask` on one line; a Constant panel shows `Layer [ Color ▾ ] [R][G][B][A]` first; selecting `diffuse.R` on the mask and reconnecting to a plain RGBA Constant shows `diffuse.R (not in input)`.
  - size: M

- [x] M38.P5.T4 — NodeGui summary label
  - files: `Gui/NodeGui.cpp`, `Gui/NodeGui.h`, `Engine/Node.h`, `Engine/Node.cpp`
  - approach: `outputLayerChanged` → `layerSelectionChanged` (`Node.h:1456, 1512`), emitted on any of the three knobs' value change; `NodeGui::onOutputLayerChanged` (`NodeGui.cpp:3186-3228`) draws `getSummary()` (`(All)`, `(diffuse)`, `(Color.rgb, /spec.*/)`; nothing for the default Color-all row); multiplanar branch kept.
  - verify: Xvfb screen shot: a Blur set to `All` shows `(All)` under its name; back to default shows nothing.
  - size: S

## Phase 38.6: Node adoption

- [x] M38.P6.T1 — Write: channel set on the container drives the encoder's plane list
  - files: `Engine/WriteNode.cpp`, `Engine/WriteNode.h`, `Engine/EffectInstance.cpp`, `Engine/EffectInstance.h`, `Tests/WriteAllLayers_Test.cpp`
  - approach: §4 Write: `channels` (input-bound, input 0, default Color all) at main-page index 0 of the container; after `createWriteNode` set the encoder's `processAllLayers` true/secret/non-persistent, `outputChannels` and `outputComponents` secret; `filterLayersForEmbeddedInput` virtual called from `getAvailableLayers`/`getPresentLayers` when `getIOContainer()` is set, overridden to intersect with `resolve()` (whole layers in this task); Color subset drives the hidden `outputComponents` to the RGBA/RGB/Alpha superset; test switches `:158-160, :183, :193, :232-234` to `setAll()`/`setLayer(Color)` and adds a `Color + diffuse` case expecting 7 channels.
  - verify: `ctest -R WriteAllLayers` green with the three cases; `dump.py` shows `[ChannelSet] channels` first on WriteOIIO and no visible `processAllLayers`/`outputChannels`.
  - size: L

- [x] M38.P6.T2 — Write: exact channel subsets for non-Color layers
  - files: `Engine/Image.h`, `Engine/ImageCopyChannels.cpp`, `Engine/EffectInstance.cpp`, `Tests/WriteAllLayers_Test.cpp`
  - approach: `Image::extractChannels(indices)` (new, plain copy like `ImageCopyChannels.cpp:121-126`); in the embedded encoder's input fetch (`EffectInstance.cpp:976-987` region) a requested plane that matches an available plane by ID but with a channel subset renders the full plane and extracts; `WriteNode::filterLayersForEmbeddedInput` now emits subset descriptors.
  - verify: test case: rows `Color` + `diffuse[G]` → the EXR has channels `R,G,B,A,diffuse.G` and `diffuse.G == 1`; `specular[R,B]` → `specular.R, specular.B` only.
  - size: M

- [x] M38.P6.T3 — Read: no channel mechanics (openfx-io task)
  - files: `Engine/ReadNode.cpp`, `Engine/ReadNode.h`
  - approach: §4 Read: after `createReadNode` hide by name `outputComponents`, `outputLayer`, `outputLayerChoice` (pattern `refreshFileInfoVisibility`, `:806-823`); on every filename change re-pin `outputLayer` to the Color entry by ID; leave the plugin's guess of `outputComponents` in place; no `OPENFX_IO_REF` bump.
  - verify: `dump.py`: ReadOIIO shows no visible `outputComponents`/`outputLayer`; gtest: Read of `flat-three-layers.exr` reports present `{Color(RGBA), diffuse, specular}`, an RGB PNG fixture reports `Color(RGB)` only; a new fixture with NO R/G/B/A channels (`Tests/fixtures/flat-no-color-layers.exr`, diffuse+specular only, generated by extending `make-flat-layers-fixture.py`) reports present `{Color, diffuse, specular}` — the first layer is *duplicated* into Color, and `diffuse` is still present as its own plane with its own pixels (Write All → EXR has `R,G,B,diffuse.*,specular.*`); `ctest -R ReadTimeOffset`/existing reader tests green.
  - size: S

- [x] M38.P6.T3a — Write: a channel set without a Color row writes no Color
  - files: `Engine/EffectInstance.cpp`, `Engine/WriteNode.cpp`, `tools/ci/local/fetch-assets.sh` (`OPENFX_IO_REF` + fork-delta comment), `Tests/WriteAllLayers_Test.cpp` (+ fork `charlesangus/openfx-io`: `IOSupport/GenericWriter.cpp`)
  - approach: two halves. Host: `getComponentsNeededAndProduced_public` merges the metadata (Color) layer into every multiplanar effect's produced planes ("Ensure the plug-in made the metadata layer available"); skip that merge when the effect is a Write container's embedded encoder and the container's channel set resolves to no Color row, so the encoder's present-layer answer carries exactly the selected planes. Fork: with a single non-Color plane GenericWriter takes its `encode()` path, which names channels by count (1→A, 2→XY, 3→RGB, 4→RGBA) instead of `layer.channel`; make that path use the plane's layer name + channel names whenever the plane is not Color (the multi-plane `encodePlanes` path already does), open a PR on `charlesangus/openfx-io`, merge, bump `OPENFX_IO_REF` and extend the delta comment as 38.2.T8 did.
  - verify: `Tests/WriteAllLayers_Test.cpp`: rows `specular[R,B]` alone → the EXR has channels `specular.R, specular.B` only (no R/G/B/A); rows `diffuse` alone → `diffuse.R, diffuse.G, diffuse.B`; the existing `Color + …` cases unchanged; `ctest -R WriteAllLayers` green; full `ctest` green; `tools/ci/local/test.sh smoke release` green after the ref bump.
  - size: L

- [x] M38.P6.T4 — Tracker: layer select, no buttons
  - files: `Engine/TrackerContextPrivate.h`, `Engine/TrackerContextPrivate.cpp`, `Engine/TrackerFrameAccessor.cpp`, `Engine/TrackerNode.h`
  - approach: `KnobLayerSelect layer` (input-bound, input 0, no buttons) on the Tracking page where `trackRed/Green/Blue` were (`TrackerContextPrivate.cpp:216-243`, `.h:103-111`, reads `:1162-1165` deleted); `TrackerFrameAccessor.cpp:352-353` requests the resolved layer; `natronImageToLibMvFloatImage` (`:172-205`) averages R,G,B for Color and all channels otherwise; absent layer → no image → existing failure path.
  - verify: gtest (headless): tracking one marker over two frames of a synthetic sequence gives the same result on `Color` as before (golden from the current build) and identical on a `diffuse` copy of the same pixels; `dump.py` shows `[LayerSelect] layer` and no `trackRed`.
  - size: M

- [x] M38.P6.T5 — Roto / RotoPaint: target layer select with buttons
  - files: `Engine/RotoPaint.cpp`, `Engine/RotoPaint.h`, `Engine/RotoPaintInteract.h`, `Engine/RotoDrawableItem.cpp`, `Tests/RotoLayer_Test.cpp`
  - approach: §4 Roto: `layer` (target, buttons; defaults all-on / A-only) replaces the "Output" separator and four bools (`RotoPaint.cpp:197-214`); `getPreferredMetadata` (`:1349-1375`) advertises the selected layer; on layer change the internal tree's host knobs are set to the same layer (`RotoDrawableItem.cpp:184-208, 261-268`); `copyChannels` (`:1528-1530`) from the select's bits.
  - verify: gtest: RotoPaint (solid brush) over the fixture into `diffuse` with `[R]` only → diffuse.R painted, diffuse.G/B and Color bit-identical to input; into a new 2-channel registry layer `mask2 [A,B]` → 2-channel plane written; into `Color` with A-only (Roto) → alpha only.
  - size: L

- [x] M38.P6.T6 — Generators: target layer select with adopted quads
  - files: `Engine/OfxEffectInstance.cpp`, `Engine/Node.cpp`, `Tests/GeneratorLayer_Test.cpp`
  - approach: `getLayerKnobSpec` returns `{eLayerSelect, eTarget, true}` for `isGenerator()` (`OfxEffectInstance.cpp:794-815`); `adoptChannelQuad` seeds the buttons from Ramp/Rectangle/Radial's own quads and Constant's host quad; the generator writes its output into the target plane and every other plane passes through from Source untouched (user decision); unselected channels of the target plane are copied from Source's same plane when Source carries it and are zero otherwise.
  - verify: gtest: Constant with `layer = depth` (registered by default) and a downstream Write All → EXR has `depth.Z` with the constant's first value; Ramp over the fixture Source with `layer = diffuse [R]` → only diffuse.R changes; `dump.py` shows `[LayerSelect] layer` first on Constant/Ramp/Radial/Rectangle.
  - size: M

## Phase 38.7: "New layer…" (M36)

- [x] M38.P7.T1 — Sentinel entry on target knobs → `NewLayerDialog` → `Project::addLayer` → select
  - files: `Gui/LayerChannelRow.cpp`, `Gui/KnobGuiLayerSelect.cpp`, `Gui/NewLayerDialog.cpp`
  - approach: §7: `addItemNew` on target-role combos; on selection open the dialog, call `Project::addLayer(desc, eOriginUser)`, on success push one `KnobUndoCommand` setting the new ID, on cancel/refusal revert the combo without an undo entry and show the registry's message verbatim.
  - verify: Xvfb: Roto panel → "New layer…" → `mask [A]` → the combo shows `mask`, the project Layers page lists it with Used by = 1, Ctrl+Z restores the previous layer while `mask` stays registered; a Blur's channel-set combo has no "New layer…" entry.
  - size: M

- [x] M38.P7.T2 — PyPlug exporter carries referenced layers
  - files: `Engine/NodeGroup.cpp`, `Tests/PyPlugExport_Test.cpp`
  - approach: in the export path that previously emitted `addUserLayer` (`NodeGroup.cpp:2733-2745`, removed in 38.1.T4) emit `app.addProjectLayer(name, channels)` once per non-built-in ID referenced by any of the three knob types inside the group, before node creation.
  - verify: gtest: export a group containing a Roto targeting `mask [A]`; the script contains exactly one `addProjectLayer("mask", ["A"])` line before the Roto's creation; re-importing into a fresh project registers `mask`.
  - size: S

## Phase 38.8: Deletions, PyPlugs, docs

- [x] M38.P8.T1 — Delete the old selectors and bools from `Node`
  - files: `Engine/Node.cpp`, `Engine/Node.h`, `Engine/NodePrivate.h`, `Engine/NodeInputs.cpp`, `Engine/OfxClipInstance.cpp`
  - approach: §8 Engine list (`createChannelSelector`, `getSelectedLayer*`, `onLayerChanged`, `refreshEnabledKnobsLabel`, `refreshLayersChoiceSecretness`, `getChannelSelectorKnob`, `getProcessAllLayersKnob`, `getMaskChannel`, the choice loop of `refreshChannelSelectors`, `ChannelSelector`, `MaskSelector::compsAvailable`, knob dispatch); `getAvailableLayers` retained for output-clip `ComponentsPresent` and `kNatronOfxExtraCreatedPlanes`; `OfxClipInstance.cpp:876-888` fallback uses `listLayersForKnob`+`resolve`.
  - verify: `grep -n "channelsSelectors\|processAllLayersKnob\|enabledChan\|getSelectedLayer\b" Engine/*.cpp Engine/*.h` returns zero hits; full `ctest` green; `dump.py` shows no `channels` Choice, no `processAllLayers`, no `*_channels` on any node.
  - size: M

- [x] M38.P8.T2 — PyPlugs: rewrite the seven, delete ZRemap/ZMask
  - files: `Gui/Resources/PyPlugs/{Fill,AngleBlur,DropShadow,PIKColor,Glow,LightWrap,EdgeBlur}.py`, delete `Gui/Resources/PyPlugs/ZRemap.py`, `ZRemap.png`, `ZMask.py`
  - approach: §8 PyPlugs: `NatronOfxParamProcess*` writes → `getParam("channels").setChannels([...])` / `setLayer(...)`; `EdgeBlur.py:41-53, 471-479` aliases the group's `createChannelSetParam` to the inner Blur's `channels`; remove `premult`/`premultChanged` writes; delete the two implicit-shuffle PyPlugs.
  - verify: `tools/ci/local/test.sh smoke debug` green; a Python background script instantiates each of the seven PyPlugs and asserts the inner node's `getRows()` matches the intended set (e.g. Fill: `Color [A]`); `ZRemap`/`ZMask` absent from `app.getPluginIDs()`.
  - size: M

- [x] M38.P8.T2a — User layer-knob params round-trip through PyPlug export and reload
  - files: `Engine/NodeGroup.cpp`, `Engine/KnobLayerSelect.{h,cpp}`, `Engine/KnobSerialization.cpp` (if the flag needs a field), `Tests/PyPlugExport_Test.cpp`
  - approach: `exportGroupToPython` emits nothing for user-created `KnobChannelSet`/`KnobLayerSelect`/`KnobChannelSelect` params, so re-exporting EdgeBlur from the GUI would drop its `Blur1channels` master; add the `createChannelSetParam`/`createLayerSelectParam`/`createChannelSelectParam` emission (name, label, current value, alias links) next to the other user-param cases. `KnobLayerSelect::withChannelButtons` is a ctor flag not persisted by `KnobSerialization`, so a user-created layer select comes back buttonless after reload; persist it (serialization version bump if a new field is added) and emit it in the export.
  - verify: `Tests/PyPlugExport_Test.cpp`: a group with a user channel-set param aliased to an inner Blur's `channels` exports a `createChannelSetParam` line + the alias, and the round-trip (script executed, `createInstance`) rebuilds the alias so editing the master drives the Blur; a user layer select with buttons saved and reloaded reports `getWithChannelButtons()` true; `ctest -R "PyPlug|KnobLayerSelect"` green; full ctest green.
  - size: M

- [x] M38.P8.T3 — Knob hints, `ofxNatron.h` note, decision record
  - files: `libs/OpenFX/include/ofxNatron.h`, `Engine/KnobChannelSet.cpp`, `Engine/KnobLayerSelect.cpp`, `Engine/KnobChannelSelect.cpp`, `docs/decisions/2026-09-19-three-layer-knobs.md`
  - approach: `ofxNatron.h` comment at the `kNatronOfxParamProcess*` block (`:285-296`): a standard quad is adopted by the host, forced true, host-masked; knob tooltips state the no-shuffle meaning and the `(not in input)`/`(not in project)` marker; the decision file summarises §0-§6 decisions (`Documentation/` is empty on `main` since M14, so hints and decision files are the docs).
  - verify: `tools/ci/local/test.sh` doc/format gates green; `Node::makeDocumentation` output for a Blur contains the new hint text (grep on the generated HTML via a background script).
  - size: S

## Phase 38.10: UAT round-1 fixes (runs before the 38.9 re-take)

User acceptance test of the 38.9.T2 AppImage on 2026-09-21 — findings and the user's calls are in `## Decisions` (2026-09-21, "UAT round 1"). Steps 1 and 5 of the script passed; everything below is what did not, plus the design feedback. This phase sits before Phase 38.9 in the file on purpose: the PM executes it first, then re-takes the after-shots and re-runs the checkpoint.

### Widget layout and rules

- [x] M38.P10.T1 — `[−]` on the left of rows 1+, a bare `[+]` aligned under the `[−]` column
  - files: `Gui/LayerChannelRow.h`, `Gui/LayerChannelRow.cpp`, `Gui/KnobGuiChannelSet.cpp`
  - approach: in `LayerChannelRow`'s constructor (`LayerChannelRow.cpp:121-147`) move `_removeButton` from the end of the `QHBoxLayout` to index 0; when the row is not removable (row 0, `KnobGuiChannelSet.cpp:171`) put a fixed-width spacer of the same width there instead, so every combo starts at the same x. In `KnobGuiChannelSet::createWidget` (`:130-146`) the `+ Add layer` button becomes a `Button("+")` sized like the `[−]` button, left-aligned in its container, so it reads as the next cell of the `[−]` column. Tooltip on `[+]`: "Add a layer row".
  - verify: Xvfb via `build/m38scout/` (`NODE=blur`, three rows): screenshot shows `[−]` left of rows 1 and 2, no `[−]` on row 0, all three combos left-aligned, and a lone `[+]` directly under the `[−]` column; offscreen `LayerChannelRow` gtest still green; `clang-format` gate passes.
  - size: S

- [x] M38.P10.T2 — Divider under the channel section
  - files: `Engine/Node.cpp`, `Engine/Node.h`, `Engine/NodePrivate.h`
  - approach: `Node::createLayerKnob` (`Node.cpp:2659-2689`) also creates a `KnobSeparator` (`kNodeParamLayerSeparator`, non-persistent, no label) and inserts it at main-page index 1, right after the channel set / layer select; its secretness follows the layer knob's (set together wherever the layer knob is made secret, e.g. the 38.4.T5 exception list does not hide the knob, so nothing else needed today). Not created for mask footers (`createMaskSelectors`).
  - verify: Xvfb screenshots of Blur, Constant and Roto panels show a horizontal rule between the channel section and the first plugin knob; `dump.py` shows a `[Separator]` at main-page index 1 on Blur/Constant and none on Shuffle/DeepMerge; a saved `.ntp` contains no separator knob entry.
  - size: S

- [x] M38.P10.T3 — A layer can be chosen by only one layer row; regex rows do not consume layers
  - files: `Engine/KnobChannelSet.h`, `Engine/KnobChannelSet.cpp`, `Gui/KnobGuiChannelSet.cpp`, `Gui/LayerChannelRow.h`, `Gui/LayerChannelRow.cpp`, `Tests/KnobChannelSet_Test.cpp`
  - approach: model: `KnobChannelSet::setLayer(row, id, …)` (`KnobChannelSet.cpp:328-340`) throws `std::invalid_argument` when another `eModeLayer` row already holds `id` (the Python wrapper already maps that to `ValueError`, 38.3.T4); `setRows` applies the same check. GUI: `KnobGuiChannelSet::refreshWidgets` (`:167-189`) passes each row the set of layer IDs held by *other* layer rows via a new `LayerChannelRow::setExcludedLayers(set<string>)`; `rebuildCombo` (`LayerChannelRow.cpp:434-503`) skips those in `eModeSetRow0`/`eModeSetRowN`, always keeping the row's own current selection listed. Regex rows neither exclude nor are excluded. Duplicates already in a loaded value are shown as-is (resolve still ORs them).
  - verify: gtest: `setLayer(1, "diffuse")` after row 0 is `diffuse` throws; regex row `diff.*` alongside a `diffuse` row is accepted; Xvfb: with rows Color / diffuse, opening row 2's combo lists `Regex…`, `specular` and not `Color`/`diffuse`; row 1's combo still lists `diffuse` (its own value).
  - size: M

### Regex rows get channel buttons

- [x] M38.P10.T4 — Engine: regex rows carry an excluded-channel set; `resolve()` honours it
  - files: `Engine/KnobChannelSet.h`, `Engine/KnobChannelSet.cpp`, `Engine/PyParameter.h`, `Engine/PyParameter.cpp`, `Tests/KnobChannelSet_Test.cpp`
  - approach: for `eModeRegex` rows the `Channels` cell (`ChannelSetRow::channels`, `KnobChannelSet.h:48-76`) now lists the *excluded* channel names — a regex's channel universe is dynamic, so exclusion is the encoding that keeps a newly matched channel on by default and never requires a value write on refresh. `resolve()` (`KnobChannelSet.cpp:443-505`) accumulates `allChannelBits(layer)` minus bits whose names are excluded; `setRegex(row, pattern)` keeps an existing exclusion set when only the pattern changes; new `setExcludedChannels(row, names)`/`getExcludedChannels(row)` valid only on regex rows (`std::invalid_argument` otherwise, same shape as `setNone` on row 1); `ChannelSetParam` gains the two methods. Codec unchanged (same three tags); document the per-mode meaning of `Channels` in the header comment.
  - verify: `ctest -R KnobChannelSet`: regex `spec.*` with excluded `{G}` on present `specular[R,G,B]` → bits `101`; the same row when a `specularZ[X,Y,Z]` layer appears → all of its bits on (nothing excluded); `setExcludedChannels(0, …)` on a layer row throws; codec round trip of a regex row with excluded `{G,B}`; Python background script: `setExcludedChannels(2, ["G"])` then `getExcludedChannels(2) == ["G"]`.
  - size: M

- [x] M38.P10.T5 — GUI: indented `|-` button line under each regex row, union of matched channels
  - files: `Gui/LayerChannelRow.h`, `Gui/LayerChannelRow.cpp`, `Gui/KnobGuiChannelSet.h`, `Gui/KnobGuiChannelSet.cpp`
  - approach: `LayerChannelRow` becomes a two-line widget for regex rows: line 1 unchanged (`[−] [Regex… ▾] [pattern] matches: …`), line 2 indented to the combo's x with a small `⌊` / `|-` decoration label followed by the channel buttons (`rebuildChannelButtons`, `LayerChannelRow.cpp:557-592`, reused). `KnobGuiChannelSet` computes, for each regex row, the matched present layers (it already produces the `matches:` text) and hands the row the ordered union of their channel names (first-seen order) plus the excluded set; a toggle emits the existing `channelToggled` and `KnobGuiChannelSet` writes `setExcludedChannels` through one `KnobUndoCommand` (same path as layer-row toggles). Line 2 is hidden when the pattern matches nothing.
  - verify: Xvfb: Blur with rows Color / regex `.*` over `flat-three-layers.exr` plus a registered `depth[Z]`-producing upstream (or a Constant into `depth`) shows one button line `R G B A Z` under the regex row; unpressing `G` then reading `getExcludedChannels(1)` from `gui.py` gives `["G"]`; Ctrl+Z restores it in one step; pattern `nomatch` hides the button line.
  - size: M

- [x] M38.P10.T6 — Node summary names the matched layers, not the pattern
  - files: `Engine/KnobChannelSet.h`, `Engine/KnobChannelSet.cpp`, `Gui/NodeGui.cpp`, `Engine/Node.cpp`, `Tests/KnobChannelSet_Test.cpp`
  - approach: `getSummary()` (`KnobChannelSet.cpp:560-599`) gains an overload taking the present-layer list and renders regex rows as the layers they resolve to, in present order, with the lowercase channel initials when a subset is selected (`specular.rb`), and `(no match)` for a pattern matching nothing; `NodeGui::onLayerSelectionChanged` (`NodeGui.cpp:3187-3219`) calls it with `Node::listLayersForKnob`; because matches change when upstream changes, `Node::refreshChannelSelectors` (`Node.cpp:7405-7414`) also emits `layerSelectionChanged` so the label follows the graph. The no-argument overload stays for Python/tests.
  - verify: gtest: rows Color + regex `spec.*` with present {Color, diffuse, specular} → `Color, specular`; excluded `{G}` on the regex → `Color, specular.rb`; Xvfb: Blur label reads `(Color, specular)` after typing `spec.*`, and switches to `(Color, (no match))` when the Read's file is changed to `flat-rgba-only.exr`.
  - size: S

### Bugs from the script

- [x] M38.P10.T7 — Layer users: RotoPaint's internal tree does not count (UAT step 2.6)
  - files: `Engine/Node.cpp`, `Engine/NodePrivate.h`, `Engine/Node.h`, `Tests/RotoLayer_Test.cpp`
  - approach: `Node::retargetLayerKnob` (`Node.cpp:5615-5638`) is the only path that drives an internal node's layer knob from its RotoPaint container; record that in `LayerKnobSource` (a `drivenByContainer` flag next to the `eRoleTarget` it already sets) and make `Node::getReferencedLayerIDs` (`:5472-5488`) skip driven knobs, so `Project::getLayerUsers` (`Project.cpp:1509-1521`) reports the Roto node alone. Applies equally to the per-item `Effect`/`Merge` nodes (`RotoDrawableItem.cpp:154-341`) and the `globalMerge` (`RotoContext.cpp:4670`).
  - verify: gtest: Roto targeting a new `mask [A]` with one bezier → `getLayerUsers("mask")` has exactly one entry (the Roto node); `removeLayer("mask")` is refused with a message naming only `Roto1`; Xvfb: Project Settings → Layers shows `mask` Used by 1.
  - size: S

- [x] M38.P10.T8 — Write's channel combo lists the present layers without a click-select-click dance (UAT step 3)
  - files: `Engine/WriteNode.cpp`, `Engine/Node.cpp`, `Gui/KnobGuiLayerChannelBase.cpp`
  - approach: reproduce first (Xvfb: connect Roto → Write, open the panel, open the combo: today it is empty/stale until a value is picked). Likely cause: the container's `layerListRefreshed` (`Node.cpp:7413`) fires from `refreshAllInputRelatedData`/`refreshMetadata_recursive` before the embedded encoder exists or before `filterLayersForEmbeddedInput` (`WriteNode.cpp:1090-1128`) can answer, and nothing fires afterwards; `WriteNode` must call `getNode()->refreshChannelSelectors()` after `createWriteNode`/`recreateKnobs` (`:525`) and on its own `onInputChanged`, and the panel's `KnobGuiLayerChannelBase::onLayerListRefreshed` (`KnobGuiLayerChannelBase.cpp:87-103`) must also relist when the panel is first shown (a `showEvent`/`onKnobsInitialized` hook) so a panel opened after the refresh is current. Fix whatever the repro actually shows; if the cause is elsewhere, record it in `## Decisions`.
  - verify: Xvfb: Read(`flat-three-layers.exr`) → Roto(`mask`) → Write; open the Write panel for the first time and open row 0's combo → `None, All, Regex…, Color, diffuse, specular, mask` immediately; change the Read's file to `flat-rgba-only.exr` → the still-open combo lists Color only on next open; existing `ctest -R WriteAllLayers` green.
  - size: M

- [x] M38.P10.T9 — Write All: a single-channel user layer comes out as `mask.A`, never `subimage03` (UAT step 3, openfx-io fork)
  - files: `tools/ci/local/fetch-assets.sh` (`OPENFX_IO_REF` + fork-delta comment), `Tests/WriteAllLayers_Test.cpp` (+ fork `charlesangus/openfx-io`: `OIIO/WriteOIIO.cpp`, `OIIO/ReadOIIO.cpp`)
  - approach: reproduce headless: Read fixture → Roto into `mask [A]` → Write All to EXR, then `oiiotool --info -v` and a Read of the result. Expected root cause: in the per-part encode paths (`WriteOIIO.cpp:1210-1398`) a one-channel non-Color plane is named by count (`A`) rather than `mask.A`, and no `oiio:subimagename` is ever set, so OpenEXR/OIIO synthesise `subimage03` and `ReadOIIO.cpp:1126-1131` adopts that as the layer. Fix both sides in the fork: every non-Color channel is named `<layer>.<channel>` in all three part modes, and per-layer parts set `oiio:subimagename` to the layer label; the reader prefers the channel prefix and ignores a synthesised `subimageNN` name. Open a fork PR, merge, bump `OPENFX_IO_REF`, extend the delta comment as 38.6.T3a did.
  - verify: new `WriteAllLayers` case: rows All with a registered `mask [A]` upstream → the EXR's channel list is exactly `R,G,B,A,diffuse.R,diffuse.G,diffuse.B,specular.R,specular.G,specular.B,mask.A` and `oiiotool --info -v` shows no `subimage` anywhere; reading it back through ReadOIIO reports present layers {Color, diffuse, specular, mask}; `tools/ci/local/test.sh smoke release` green after the ref bump.
  - size: M

- [x] M38.P10.T10 — Restore `(Un)premult by` on the colour family as `unPremultBy` / `unPremultByChannel` (UAT step 4, openfx-misc fork)
  - files: `tools/ci/local/fetch-assets.sh` (`OPENFX_MISC_REF` + fork-delta comment), `Gui/Resources/PyPlugs/*.py`, `Tests/ChannelSetRender_Test.cpp` (+ fork `charlesangus/openfx-misc`: `SupportExt/ofxsMaskMix.h`)
  - approach: user decision — a node-level "unpremult by X, do the op, premult again" is a convenience, not the app-wide premult *concept* 38.2 removed, and comes back under new names so it cannot be confused with the hidden ones. In the fork: `kParamPremult "premult"` → `"unPremultBy"` (label `(Un)premult by`), `kParamPremultChannel "premultChannel"` → `"unPremultByChannel"` (label empty or `Channel`, same-line), `ofxsMaskMix.h:32-39, 64-77`; `premultChanged` stays as-is (it is still in the host hide list, and the `changedClip` auto-toggle is already inert because the host answers a constant). Host side: `hideDeprecatedPremultKnobs` (`OfxEffectInstance.cpp:615-646`) is unchanged — the renamed params are simply no longer matched. PyPlugs: re-add under the new names any `premult` value that 38.8.T2 removed (`git show fd33e5ab8 -- Gui/Resources/PyPlugs`, line 64 region; `premultChanged` writes stay removed). Bump `OPENFX_MISC_REF`, extend the delta comment (`fetch-assets.sh` "delta" block).
  - verify: `dump.py`: Grade, ColorCorrect, Multiply show `[Bool] unPremultBy` and `[Choice] unPremultByChannel` visible, no visible `premult`/`premultChannel`/`premultChanged`; a background render Constant(rgb=1, a=0.5) → Grade(gain 2, `unPremultBy` on) gives `rgb = min(1,1/0.5*2)*0.5` per the plugin's math vs. `2` with it off (assert against the plugin's own formula); `tools/ci/local/test.sh smoke release` green.
  - size: M

- [x] M38.P10.T11 — Mask channel absent from a connected Mask input → error state, render fails (UAT step 6.1)
  - files: `Engine/EffectInstanceRenderRoI.cpp`, `Engine/Node.cpp`, `Engine/Node.h`, `Tests/ChannelSetRender_Test.cpp`
  - approach: user decision: Nuke-like. New `Node::checkMaskChannelsPresent(std::string* message)` walks `_imp->maskSelectors` (`Node.cpp:2448-2507`): for each mask input that is connected and whose enabled bool is on and whose `KnobChannelSelect` value is not `None`, run `resolve` against `getPresentLayers(inputNb)` (`KnobChannelSelect.cpp:144-178`); the first miss produces `Mask channel <layer.channel> is not in the Mask input`. `renderRoIInternal` calls it before the plugin render (next to the OpenGL check at `EffectInstanceRenderRoI.cpp:706`) → `setPersistentMessage(eMessageTypeError, …)` and `eRenderRoIRetCodeFailed`; `refreshChannelSelectors` clears the message when the check passes. The `getImage` null-mask shortcut (`EffectInstance.cpp:808`) stays for the disconnected / `None` cases.
  - verify: gtest: Blur with Mask = Constant(RGB) and `maskChannel_Mask = diffuse.R` → render returns failed and `hasPersistentMessage()` names `diffuse.R`; reconnecting the Mask to the three-layer Read → render succeeds and the message is gone; Mask disconnected with the same value → render succeeds, no message; Xvfb: the node shows the red error frame and the mask footer still reads `diffuse.R (not in input)`.
  - size: M

- [x] M38.P10.T12 — Tracker: drop the layer select; always track Color (UAT step 6.2)
  - files: `Engine/TrackerNode.h`, `Engine/TrackerContextPrivate.cpp`, `Engine/TrackerFrameAccessor.cpp`, `Tests/` (the 38.6.T4 tracker gtest)
  - approach: user decision: omit rather than fix — non-RGB tracking waits for the native Shuffle (M34). `TrackerNode::getLayerKnobSpec` (`TrackerNode.h:120-123`) returns `eKindNone`; `resolveLayerKnob`/`getLayerKnob` uses at `TrackerContextPrivate.cpp:1127, 1433` and the `TrackerFrameAccessor` request go back to Color; `natronImageToLibMvFloatImage` keeps the R,G,B average (38.6.T4 already made it channel-count driven). Remove the `diffuse` case from the tracker gtest; keep the Color golden.
  - verify: `dump.py`: Tracker shows none of the three knob types and no `trackRed`; the tracker gtest's Color case is unchanged; full ctest green.
  - size: S

- [x] M38.P10.T13 — Generators: the layer select drives a hidden `outputComponents` (UAT step 6.3)
  - files: `Engine/Node.cpp`, `Engine/Node.h`, `Engine/OfxEffectInstance.cpp`, `Tests/GeneratorLayer_Test.cpp`
  - approach: the plugin's `outputComponents` (`ofxsGenerator.cpp:103, 219, 412`, read unconditionally — no secret gate, unlike GenericWriter) is redundant with the layer select. In `adoptChannelQuad`'s generator branch (38.6.T6): find the `outputComponents` choice by name, `setSecretLocked(true)` + non-persistent (the lock from 38.6.T2, `KnobI::setSecretLocked`), and on every layer-select change set it from the selection — target Color with buttons `A` only → `Alpha`, an R/G/B subset → `RGB`, otherwise `RGBA` (the same superset rule as Write's Color row); any non-Color target → `RGBA` (the produced plane comes from the layer select, `outputComponents` is irrelevant). Drive it from `Node::onEffectKnobValueChanged` where the layer select is already handled.
  - verify: `dump.py`: Constant/Ramp/Radial/Rectangle show no visible `outputComponents`; gtest: Constant `Color [A]` → downstream Write All EXR has `A` only in Color (or `R,G,B,A` with rgb zero if the encoder's superset rule says so — assert the actual rule once), `Color [R,G,B]` → `R,G,B`; `layer = depth` unchanged from 38.6.T6; a saved `.ntp` has no `outputComponents` entry for a Constant.
  - size: M

- [x] M38.P10.T14 — Read format: a Read's output format is its file's, whatever the project format (UAT incidental)
  - files: `Engine/ReadNode.cpp`, `Engine/EffectInstance.cpp`, `Tests/` (new or existing reader gtest)
  - approach: reproduce first in a background script: fresh project → Read an 8×8 fixture (autosets the project format once, `Project.cpp:2271-2304`, from `EffectInstanceRenderRoI.cpp:1727-1740`) → Read a larger file; compare the second Read's `getOutputFormat()`/RoD with the file's size. GenericReader does `clipPreferences.setOutputFormat(format)` (`GenericReader.cpp:2143`) only when `gotSequenceTimeDomain` and the first frame resolves, so check whether the container's metadata pass (`getDefaultMetadata`, `EffectInstance.cpp:5411-5576`, whose fallback is the project format) runs before the encoder has the frame range, or whether 38.2.T3's `checkMetadata` deletions dropped a re-check. If the Read's format is wrong, fix so it always equals the file's format after the filename settles (a metadata refresh on `onFileNameParameterChanged` if that is the gap). Project-format autoset stays once-per-project (stock behaviour); record that as the assumption in `## Decisions` so the user can overrule it.
  - verify: gtest: Read of `Tests/fixtures/flat-three-layers.exr` (small) then a Read of a larger fixture (generate a 256×128 EXR via `make-flat-layers-fixture.py` if none exists) in the same project → the second Read's output format is 256×128 and its RoD matches; the project format remains the first file's; Xvfb: the viewer on the second Read shows the whole image, not an 8×8 crop.
  - size: M

**Phase 38.10 gate:** full `tools/ci/local/test.sh ctest debug` and `smoke debug` green; `dump.py` shows no visible `premult`/`premultChannel`/`premultChanged`/`outputComponents` on Grade/Constant and `unPremultBy` visible on Grade; the T9 and T10 fork refs are merged and bumped; then Phase 38.9 is re-run.

## Phase 38.9: Evidence and checkpoint (re-run after Phase 38.10)

- [x] M38.P9.T1 — After-shots
  - files: `build/m38scout/gui.py`, `build/m38scout/run.sh`
  - approach: same tags as 38.2.T1 plus Blur (set with three rows), Roto (New layer…), Tracker, mask footer, Write (All), Constant; produce `after-<tag>.png` beside `before-<tag>.png`; `dump.py` → `dump-after.txt`.
  - verify: all after-shots exist; `diff <(grep ChannelSet dump-after.txt | wc -l) …` counts match §3's table (channel set on Blur/Grade/Transform/Write/Merge; layer select on Constant/Ramp/Roto/RotoPaint/Tracker; none on Read/Shuffle/Premult/DeepMerge).
  - size: S

- [ ] M38.P9.T1a — Re-take the after-shots after Phase 38.10
  - files: `build/m38scout/gui.py`, `build/m38scout/run.sh`
  - approach: re-run 38.9.T1 on the Phase 38.10 build; the Blur shot must show the new row layout (T1), divider (T2) and a regex row with its `|-` button line (T5); the Tracker shot has no layer knob (T12); `dump-after.txt` is regenerated and its counts re-matched against §3's table with Tracker moved to the "none" column.
  - verify: all `after-<tag>.png` regenerated (mtime newer than the 38.10 gate commit); `dump-after.txt` matches the amended table.
  - size: S

- [ ] M38.P9.T2 — Packaged release AppImage and user checkpoint (round 2)
  - files: none (uses `tools/ci/local/package.sh`)
  - approach: package release; hand the user the round-1 script again (Read `flat-three-layers.exr` → Blur → Roto into a new layer → Write All; Grade; viewer over checkerboard; mask footer / Tracker / Constant) with these additions: the `[−]`/`[+]` layout and divider; picking the same layer twice is impossible; a regex row shows channel buttons and the node label names the matched layers; Project Settings → Layers shows `mask` Used by 1; the Write combo is populated on first open; the EXR has `mask.A` and no `subimage*`; Grade shows `(Un)premult by`; a missing mask channel errors the node; Tracker has no layer knob; Constant has no Output Components; a second, larger Read keeps its own format. Round 1 (2026-09-21) is recorded in `## Decisions`.
  - verify: the user confirms the round-2 checks on the AppImage; the milestone PR links the before/after shots.
  - size: S

**Verification gate:** `tools/ci/local/test.sh ctest debug` and `test.sh smoke debug` green; `grep -rn -i "ImagePremultiplicationEnum\|premultWarning" Engine Gui` empty; `dump-after.txt` matches §3's table as amended by Phase 38.10 (Tracker: none); the 38.4.T4 regression guards and the 38.6 Write/Read/Roto/generator tests and the 38.10 tests pass; after-shots for Blur, Roto, Tracker, mask footer, Write, Constant exist; the round-2 user checkpoint on the packaged release AppImage passes.

## Decisions

- 2026-09-19 — **Rescoped (user): M38 is a new layer/channel selection widget, absorbing M35, M43 and M36**: Nuke-style but stronger (regex, add/remove rows); nodes process exactly the selected layers/channels in place; non-Shuffle nodes get no shuffle capability.
- 2026-09-19 — **Clean break for old projects** (user): no load-time mapping of `processAllLayers`/`channels`/R,G,B,A; old nodes come up on the default selection; one `NATRON_CACHE_VERSION` bump (5→6, in 38.2).
- 2026-09-19 — **Regex matches layer labels, whole-string anchored, case-sensitive, `QRegularExpression`** (user): users type what they see.
- 2026-09-19 — **ZRemap and ZMask are dropped until M34** (user): they depend on the implicit shuffle.
- 2026-09-19 — **One undo step per user action**; regex commits on `editingFinished`.
- 2026-09-19 — **Three knob types by value shape, not one knob with flags** (user, review round 1): channel set (multi-row, None/All on row 0 only, regex), layer select (one layer, optional buttons), channel select (one `layer.channel`); the shared part is the GUI row and the population source. Per-type Python classes with no "raises if variant" cases.
- 2026-09-19 — **All three derive from `KnobTable`, with untranslated column tags**: one codec, one alias branch, one serialization case; `KnobLayers`' `tr()`-tagged format is the anti-pattern (`KnobTypes.h:1174-1183`). Not `KnobChoice`: an absent selection must persist as an ID independent of the current list.
- 2026-09-19 — **Listing is a `Node` service keyed on the knob's role; Color is always listed on input-bound knobs**: input-bound → `getPresentLayers`, target → registry + "New layer…" (registry doc §3); an unconnected node still offers Color because every stream has a Color plane (`EffectInstance.cpp:4270-4273`). The absent marker decorates the selected value, never the list.
- 2026-09-19 — **Merge is out of M38** (user): native node in its own milestone; the OFX Merge is ordinary and may break; its non-standard quads are not adopted.
- 2026-09-19 — **Premult removal runs first (Phase 38.2)**: the per-plane masking/`renderHandler` rewrite touches exactly the functions carrying premult arguments; the warning lives inside the channel-bool machinery that is deleted later; the widget is never built premult-aware.
- 2026-09-19 — **OFX premult property answered as a constant `kOfxImageUnPreMultiplied`**: ABI must stay answerable; only `kOfxImagePreMultiplied` triggers the Grade family's auto-toggle (`Grade.cpp:1188-1210`) and `kOfxImageOpaque` makes Premult treat alpha as 1 (`Premult.cpp:644`).
- 2026-09-19 — **Premult/Unpremult nodes are kept; openfx-misc is fork-and-fixed for `Premult.cpp`'s identity shortcuts**: they are the user's tools under the new responsibility model; no constant answer keeps both working with the upstream plugin (`:756-770`); fork-and-fix is the project's standing pattern (`fetch-assets.sh:152-154`). A native pair is M37-era work.
- 2026-09-19 — **Plugin premult params (`premult`, `premultChanged`, `premultChannel`, `filePremult`, `outputPremult`, `inputPremult`) are hidden host-side by name and left off**: "no premult knobs anywhere" without touching 30 plugins; PyPlug writes to them are removed.
- 2026-09-19 — **Read: host-side hide of `outputComponents`, `outputLayer`, `outputLayerChoice`; no fork change; the plugin's guess stays**: the guess makes Color follow the file (`GenericReader.cpp:2069`), which is the wanted behaviour; `outputLayer` is the Read-side implicit shuffle and is pinned to Color.
- 2026-09-19 — **Write: the container's channel set feeds the encoder through a host filter on the embedded input's present list; `processAllLayers` forced true and hidden**: the encoder already enumerates "planes present" (`WriteOIIO.cpp:585-591`), so the host's answer *is* the selection; M58's hash fold (`Node.cpp:809-814`) carries the knob into the encoder's hash. Non-Color subsets are exact via extraction; Color subsets map to the RGBA/RGB/Alpha superset.
- 2026-09-19 — **Host masks for every node with host-owned channel buttons; adopted quads forced true**: a positional quad cannot hold per-layer sets; `copyUnProcessedChannels` is already a plain copy (`ImageCopyChannels.cpp:49`), so per-plane masking changes no arithmetic.
- 2026-09-19 — **Generators, Roto and RotoPaint are target-role layer selects with buttons; Tracker is an input-bound layer select without buttons** (user (d)/(e) plus the generator recommendation): a generator writes into one layer; Roto writes into any registry layer by advertising it in `getPreferredMetadata` and pushing the same layer to its internal tree; the Tracker averages the chosen layer's channels and `trackRed/Green/Blue` go.
- 2026-09-19 — **Multiplanar plugins (Premult, Unpremult, STMap, IDistort, Shuffle, LayerContactSheet) and deep nodes get none of the three knobs**: a multiplanar plugin chooses its own planes; deep is M60.
- 2026-09-19 — **No new OFX descriptor property**: the variant property of v2 belonged to the rejected model; adoption is by node kind and quad name, documented in `ofxNatron.h`.
- 2026-09-19 — **Expressions refused via a `KnobI::supportsExpressions()` hook**: no per-knob refusal exists today (`Knob.cpp:2825-2889`); a per-frame channel set has no render meaning we support.

---

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
- 2026-09-19 — **Widget design v3 approved** with three answers: (1) Read of a file with no R/G/B/A layer keeps OIIO's default (first layer into Color) for M38, provided the layer is *duplicated* into Color and still present as its own plane — verified by a no-Color fixture in 38.6.T3; (2) Write Color subsets map to the encoder's RGBA/RGB/Alpha superset for now; (3) a generator writes its output into the target layer and every other layer passes through untouched.
- 2026-09-19 — **Premult/Unpremult always do their math** (user confirmation): once the un/premultiplied concept is gone the user owns knowing an image's state; the openfx-misc fork deletes every read of the clip premult property in `Premult.cpp` (identity shortcuts, auto-toggle, and the opaque branch), and the 30 plugins' own (un)premult checkboxes are hidden and off.
- 2026-09-19 — **"Opaque" goes with the premult concept** (user): `kOfxImageOpaque`/`eImagePremultiplicationOpaque` is the third value of the same property and is never answered or tracked; the viewer's alpha-is-one path is renamed to a channel-count term (`noAlphaChannel`) because that is what it actually keys on.
- 2026-09-20 — **Phase 38.1 gate passed** after one regression fix (`3429d3640`): `Node::registerProducedLayers` queried a Read container's produced planes under its own hash before the bundled decoder had loaded the file, caching Color as RGBA for an RGB file; the fix invalidates cached components-needed results whenever a node's metadata changes, since produced planes derive from clip preferences the hash never tracked. A related pre-existing mis-keying (input answers cached under the caller's hash) is T8.
- 2026-09-20 — **Readers and writers must stop converting premultiplication themselves (38.2.T8, openfx-io fork)**: found by 38.2.T4 — `GenericWriter::changedClip` re-derives its `inputPremult` from the clip property on every connect (now always UnPreMultiplied) and then premultiplies before writing; `GenericReader` converts `filePremult`→`outputPremult` the same way. Hiding the knobs is not enough; under "the user owns premult", I/O writes and reads pixels as stored.
- 2026-09-20 — **Phase 38.2 (premult removal) complete**: `ImagePremultiplicationEnum` gone, `NATRON_CACHE_VERSION` 6, OFX answers a constant, no premult knob or warning anywhere, viewer pixel-identical before/after; Premult/Unpremult always do their math (`charlesangus/openfx-misc#1`), readers/writers never convert (`charlesangus/openfx-io#4`, which also sets `oiio:UnassociatedAlpha` on write so OIIO itself does not divide by alpha for PNG/TGA/WebP; TIFF's EXTRASAMPLES tag now says unassociated). Follow-up noted, out of scope: with the default ACES config a bare EXR→EXR round trip applies a Rec.709→AP1 transform because ReadOIIO's default input space for a WriteOIIO EXR resolves to `lin_rec709_scene` while the writer outputs `scene_linear` — a colourspace-default issue for M50.
- 2026-09-20 — **38.5.T3: the three knob GUIs share `KnobGuiLayerChannelBase`**: listing, refresh on `layerListRefreshed`/`projectLayersChanged`, the one-undo-step push and the absent-marker text moved out of `KnobGuiChannelSet` into a base the two selects derive from. Also found while verifying: `build/release` had four objects (`RotoItem`, `TrackerContextPrivate`, two tests) with empty ninja dependency records, so they never rebuilt after `Knob.h` grew a virtual (a `bad_alloc` vtable mismatch in `NodesOwningTheirPlanesGetNoLayerKnob`); deleting the objects fixed it. If a release-tree test fails in a way the code can't explain, check `ninja -t deps` for `#deps 0` before debugging.
- 2026-09-21 — **38.6.T1: Write's `outputComponents` is hidden inside a secret group, not made secret itself**: `GenericWriterPlugin::getClipPreferences` only applies `outputComponents` while it is non-secret, and with `processAllLayers` forced on a secret `outputComponents` leaves the Color clip empty. The Color row's channels drive it to Alpha/RGB/RGBA. Follow-up carried into 38.6.T2: the encoder's own R/G/B/A quad is still visible on the Write panel and must be adopted/hidden like every other standard quad.
- 2026-09-21 — **38.6.T2: the encoder's quad needs a secret lock, not a plain hide**: GenericWriter's `getClipPreferences` and the host's `refreshEnabledKnobsLabel` both re-show `NatronOfxParamProcessR..A` on every metadata pass, and the plugin packs a lone plane through whichever boxes it sees shown. `KnobI::setSecretLocked` makes `setSecret` a no-op and a `kOfxParamPropSecret` get-hook reports the knob's state to the plugin. `Node::adoptChannelQuad` could use the same lock for ordinary nodes; not changed. Found, not fixed: a Write whose rows exclude Color still writes R,G,B,A because `getComponentsNeededAndProduced_public` merges the metadata layer into every multiplanar effect's produced planes, and with a single plane GenericWriter's `encode()` path names channels by count rather than `layer.channel` — raised as an open question.
- 2026-09-21 — **A Write with no Color row writes no Color — fixed in M38 (user decision)**: the 38.6.T2 finding (host merges the metadata layer into every multiplanar effect's produced planes; GenericWriter's single-plane path names channels by count) is not deferred; new task 38.6.T3a covers the host skip and the openfx-io fork change.
- 2026-09-21 — **38.6.T3a landed** (`charlesangus/openfx-io#5`, `OPENFX_IO_REF` → `9b558a7`): a Write in mode None now produces no planes at all rather than a forced Color; no UI guard added — it follows from the user decision and the 38.9 checkpoint can surface it if it reads wrong.
- 2026-09-21 — **38.6.T5: Roto's `getPreferredMetadata` stays at Color/4**: `NodeMetadata` can only express Color/Disparity/Motion and the produced plane list for a non-Color target already comes from `getComponentsNeededDefault`; retargeting the internal tree is a `Node::retargetLayerKnob` + `RotoContext::retargetRotoPaintTree` pair driven from `Node::onEffectKnobValueChanged` because host-created knobs never reach `EffectInstance::knobChanged`. Known limit: an alpha-less 2/3-channel target cannot carry the shape's coverage into the internal Merge, so inside the shape the result is additive rather than an over.
- 2026-09-21 — **Phase 38.6 complete.** 38.6.T6: with Source unconnected a generator targeting a non-Color layer produces only that plane (no Color); a one-channel non-Color plane takes the plugin's first output channel, not its alpha. Left visible and out of scope: the generators' own `outputComponents` choice. Unverified note from the implementer: `LayerSelectParam.set("depth")` from a NatronRenderer script did not take — check the Python wrapper's setter name (`setLayer`) in 38.8.T2 when the PyPlugs are rewritten.
- 2026-09-21 — **38.8.T2: Glow's `alpha` drives its inner channel sets through an `onParamChanged` callback, not expressions** — table knobs refuse expressions by design, so an animated `alpha` no longer re-evaluates per frame (channel sets cannot animate either). Fill is unchanged: its quad writes land on Premult/Unpremult's own visible quads. Two gaps found and filed as 38.8.T2a: the exporter emits no `create*Param` for user layer knobs, and `withChannelButtons` is not persisted.
- 2026-09-21 — **Phase 38.8 complete.** 38.8.T3 found two pre-existing doc-pipeline limits, both out of scope: `Node::makeDocumentation` skips every host-injected knob (`NodeDocumentation.cpp` `isDeclaredByPlugin` gate), so the new hints never reach the exported docs; and `NatronRenderer --export-docs` segfaults on any reader plugin (Read container created with an empty filename). Hint text verified live via `Param.getHelp()` instead. `ofxNatron.h` note landed as `charlesangus/openfx-natron#2`.
- 2026-09-21 — **Gate automated checks green**: debug ctest 373/373 (first run had one `DeepReadWriteTest.DeepWriteRendersASequenceThroughTheRenderScheduler` abort at fixture teardown — `QThread: Destroyed while thread is still running` — under the parallel run; 6/6 clean in isolation and 373/373 on the rerun, so recorded as a scheduler-teardown flake in the M26 family, not an M38 regression), debug smoke green, premult grep empty, 11 after-shots, `dump-after.txt` matches §3. Release AppImage at `build/release/artifacts/Natron-2.6.0-x86_64.AppImage` for the 38.9.T2 checkpoint.
- 2026-09-21 — **UAT round 1 on the 38.9.T2 AppImage: not passed; fixes stay in M38 as Phase 38.10 (user decision, over shipping first)**. Script results: 1 (Blur set + undo) ok; 2.6 the Layers page reports `mask` Used by 4 — the Roto plus its internal `_Effect`/`_Merge`/`globalMerge` nodes — and the refusal dialog names all four; 3 the Write's channel combo is empty until a value is picked and re-opened, and Write All produced a `subimage03` layer where `mask.A` was expected; 4 the user wants the colour family's node-level `(Un)premult by` back; 5 (viewer over checkerboard) ok; 6.1 a mask channel absent from a connected Mask input should be an error state; 6.2 the Tracker's layer combo lists only Color; 6.3 Constant still shows `Output Components`. Incidental: a second, larger Read appeared "read in at" the 8×8 project format the first Read had autoset. Design feedback: `[−]` left of rows 1+, bare `[+]` aligned under the `[−]` column, no duplicate layer rows (regex rows exempt), regex rows get channel buttons for the union of matched channels on an indented `|-` line, a divider under the channel section, and the node label names the layers a regex matched rather than the pattern.
- 2026-09-21 — **Node-level `(Un)premult by` is not the premult concept** (user): 38.2 removed the app's *tracking* of premultiplication state; a colour node unpremultiplying by a channel, grading, and re-premultiplying is a per-node convenience the user asks for explicitly, so it returns — under the fork names `unPremultBy` / `unPremultByChannel` so the hidden `premult*` family stays hidden by name. `premultChanged`, Read/Write's `filePremult`/`outputPremult`/`inputPremult`, and RotoPaint's deleted `premultiply` are unchanged. Amends the 2026-09-19 "plugin premult params hidden host-side" decision.
- 2026-09-21 — **Tracker gets no layer knob** (user): fix-or-omit → omit; it tracks Color (R,G,B average) and non-RGB tracking is a Shuffle (M34) use case. Amends the 2026-09-19 "Tracker is an input-bound layer select without buttons" decision; §3's table moves Tracker to the "none" column.
- 2026-09-21 — **A selected mask channel missing from a connected Mask input fails the render with a persistent error** (user): Nuke-like; the badge-only alternative was offered and declined. Disconnected Mask input or `None` keep the silent no-mask path.
- 2026-09-21 — **Regex rows persist an *excluded* channel set; layer rows keep their *enabled* set**: a regex's channel universe changes with the graph, so exclusion keeps new channels on by default and never needs a value write from a GUI refresh; the `Channels` cell's meaning is per mode and documented in the header. Python exposes `setExcludedChannels`/`getExcludedChannels` on regex rows only.
- 2026-09-21 — **Layer-user counting skips knobs driven by a container**: the RotoPaint internal tree's layer knobs are set by `retargetLayerKnob`, never by the user, so they are marked driven and `getReferencedLayerIDs` ignores them — the Roto node is the one user.
- 2026-09-21 — **Project-format autoset stays once-per-project (assumption, 38.10.T14)**: stock Natron consumes `autoSetProjectFormat` on the first reader render; what must hold is that each Read's own output format equals its file's format. If the user wants later Reads to re-set the project format, that is a separate ask.
- 2026-09-22 — **Phase 38.10 executed with the build environment rebuilt from scratch**: the session started with no Docker image; `aswf/ci-vfxall:2027-clang21.1` was pulled through `mirror.gcr.io`, `natron-dev` rebuilt, and `.ccache` plus both build trees survived. Tasks were implemented in parallel batches and committed after each batch's gtests passed; the shared-file tasks landed as three grouped commits (channel set `6b36ec7e0`, Node `4c69dfd77`, Write All/Roto `67ed048cc`) rather than one per task.
- 2026-09-22 — **Generators lock `outputComponents` to `RGBA` (38.10.T13)**, not the Alpha/RGB/RGBA superset rule the brief proposed: driving it to `RGB` for a Color `[R,G]` selection dropped the stream's alpha and broke pass-through over a source (`ConstantOverSourceWritesTwoChannelsOfColor`). The layer select's buttons say which channels are *written*; the stream's channel count is not theirs to change.
- 2026-09-22 — **Write's combo went stale because `NodeGroup` containers never refresh their own channel selectors (38.10.T8)**: `Node::forceRefreshAllInputRelatedData`'s group branch only refreshes the nodes inside the group, so a Write's own connect never emitted `layerListRefreshed`; the container now refreshes its selectors when it owns a host layer knob. Not the encoder-timing cause the brief guessed. After switching the Read to `flat-rgba-only.exr` with Roto(`mask`) in between, the correct listing is `Color, mask`, not Color only.
- 2026-09-22 — **`subimage03` came from the missing part name, not channel naming (38.10.T9, `charlesangus/openfx-io#6`)**: the default "Split Views,Layers" mode never set `oiio:subimagename`, so OpenEXR synthesised one and the reader adopted it for the bare `R,G,B,A` part. Found on the way: Roto/RotoPaint targeting a **one-channel** user layer rendered nothing — `RotoPaint::render` applied the plane's channel bit (bit 3) to the RGBA temp image before the host's 4→1 conversion read channel 0; fixed in `RotoPaint.cpp`, covered by the new Write All case.
- 2026-09-22 — **Read format symptom not reproducible (38.10.T14)**: engine (debug renderer) and GUI (the UAT AppImage under Xvfb) both give a second Read its file's 128×128 format/RoD with the project at 8×8; a gtest now pins it. Most likely the viewer was still on Read1 (new nodes stack at the same position; `connectInput` on an occupied viewer input is a no-op) or kept its zoom. Round 2 asks the user to view Read2 explicitly.
- 2026-09-22 — **`(Un)premult by` rename lives in `charlesangus/openfx-supportext` (38.10.T10)**: `ofxsMaskMix.h` is a submodule, so the rename is supportext#2/#3 with openfx-misc#2/#3 repointing the submodule; supportext also carried its own `setIsSecret(true)` on the channel choice, which is removed (the choice is still non-functional in the plugin math — alpha is always used — and its hint says so). `unPremultByChannel` takes `(channels, row=0)` in Python like its siblings.
- 2026-09-20 — **Three plugins keep their own R/G/B/A quads (38.4.T5)**: the 38.4.T4 audit of the openfx-misc quads found 21 that only mask (Grade, Invert, ColorCorrect, Multiply, Saturation, Add, Clamp, ColorMatrix, CopyRectangle, Gamma, FrameBlend, Distortion, Log2Lin, Premult, PLogLin, Quantize, Radial, Roto, Rectangle, Ramp, Threshold, plus CImgFilter-based nodes) and three that use the quad to change behaviour — KeyMix, DenoiseSharpen, ClipTest. Adoption by name stays the rule; those three are a host-side exception list, quads visible, plugin masks.
