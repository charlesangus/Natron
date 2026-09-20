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

- [ ] M38.P4.T2 — Per-plane input fetch and per-plane host masking
  - files: `Engine/OfxClipInstance.cpp`, `Engine/EffectInstance.cpp`, `Engine/Node.cpp`
  - approach: §5(2)(3): `clipGetImage` (`OfxClipInstance.cpp:853-874`) picks the entry equivalent to `outputLayerBeingRendered`; `renderHandler` chooses `originalInputImage` and the bitset per plane in the loop at `EffectInstance.cpp:2618` (today `:2250-2258`); `Node::getProcessChannel` (`:5773-5785`) is deleted with `hostChannelSelectorEnabled` — the host masks whenever the node has channel buttons.
  - verify: gtest: Read→Invert(`Color R only` + `diffuse[G]`)→Write All: Color = (0,0,0,1) inverted R only i.e. `(0,0,0,1)`, diffuse = `(0,0,0)` (G inverted from 1 to 0, R/B untouched), specular untouched `(0,0,1)`; Read→Grade with `A` off leaves alpha bit-identical.
  - size: L

- [ ] M38.P4.T3 — Identity on empty; delete "choice B"
  - files: `Engine/Node.cpp`, `Engine/EffectInstanceRenderRoI.cpp`
  - approach: `hasAtLeastOneChannelToProcess` (`Node.cpp:5846-5865`) = resolved set non-empty with a set bit; delete the `getChannelSelectorKnob(inputNbIdentity)` branch (`EffectInstanceRenderRoI.cpp:630-680`), always choice A.
  - verify: gtest: Blur with row 0 `None` renders bit-identical to its input for all three planes and reports identity; Blur with `diffuse` only leaves Color bit-identical.
  - size: S

- [ ] M38.P4.T4 — Regression guards for adopted plugins
  - files: `Tests/ChannelSetRender_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: for Grade, Invert, ColorCorrect, Multiply, Saturation, CImgBlur on `flat-three-layers.exr`: full-channel result equals the pre-38 plugin-masked result (golden values computed by the plugin's own math on the constant fixture), single-channel results touch only that channel, non-Color rows touch only that plane; audit the openfx-misc grep list (`grep -l NatronOfxParamProcessR build/assets/plugin-src/openfx-misc/*/*.cpp`) and record in the test file which plugins read `processX` outside masking (none expected).
  - verify: `ctest -R ChannelSetRender` green; the audit list is in the test's header comment.
  - size: M

## Phase 38.5: GUI

- [ ] M38.P5.T1 — `LayerChannelRow` widget
  - files: `Gui/LayerChannelRow.h`, `Gui/LayerChannelRow.cpp`, `Gui/CMakeLists.txt`
  - approach: §2: combo (entries by mode), checkable coloured `Button`s by channel name (`KnobGuiBool.cpp:273-300` constants), regex `LineEdit` + matches label validated on `editingFinished`, `[−]`; signals `layerChosen(id)`, `channelToggled(name, on)`, `patternCommitted(p)`, `removeRequested()`, `newLayerRequested()`; `setAbsentMarker(text)` inserts/removes the single marker item; no knob dependency (unit-testable with a fake list).
  - verify: an offscreen `QApplication` gtest (`QT_QPA_PLATFORM=offscreen`): entries order per mode, buttons rebuilt on layer change with all checked, invalid pattern sets the `dirty` property and tooltip, marker item present only while set; `clang-format` gate passes.
  - size: M

- [ ] M38.P5.T2 — `KnobGuiChannelSet` with add/remove and one undo step per action
  - files: `Gui/KnobGuiChannelSet.h`, `Gui/KnobGuiChannelSet.cpp`, `Gui/KnobGuiFactory.cpp`, `Gui/KnobUndoCommand.h`
  - approach: composite `QVBoxLayout` of rows + `[+ Add layer]` (precedent `KnobGuiTable.cpp:206-281`); every row signal → one `KnobUndoCommand<std::string>` with the new `setMergeable(false)`; row-0 `None/All` greys rows 1+; repopulate on `onChannelsSelectorRefreshed` and `projectLayersChanged`; no expression menu entry when `!supportsExpressions()`.
  - verify: Xvfb via `build/m38scout/`: Blur panel screenshot `panel-blur-set-0.png` matches the §2 mockup with rows Color / diffuse / regex; scripted Ctrl+Z after "add row → choose diffuse → toggle G" leaves exactly three undo entries and restores the panel state each step (assert via `getRows()` from `gui.py`).
  - size: L

- [ ] M38.P5.T3 — `KnobGuiLayerSelect` and `KnobGuiChannelSelect`
  - files: `Gui/KnobGuiLayerSelect.h`, `Gui/KnobGuiLayerSelect.cpp`, `Gui/KnobGuiChannelSelect.h`, `Gui/KnobGuiChannelSelect.cpp`, `Gui/KnobGuiFactory.cpp`
  - approach: one `LayerChannelRow` each; same-line placement respected for the mask footer (`setAddNewLine(false)` chain, `KnobGuiContainerHelper.cpp:678-698`); target-role combos append the sentinel (wired in 38.7).
  - verify: Xvfb: Blur's mask footer screenshot shows `☑ [ Color.A ▾ ] □ Invert Mask` on one line; a Constant panel shows `Layer [ Color ▾ ] [R][G][B][A]` first; selecting `diffuse.R` on the mask and reconnecting to a plain RGBA Constant shows `diffuse.R (not in input)`.
  - size: M

- [ ] M38.P5.T4 — NodeGui summary label
  - files: `Gui/NodeGui.cpp`, `Gui/NodeGui.h`, `Engine/Node.h`, `Engine/Node.cpp`
  - approach: `outputLayerChanged` → `layerSelectionChanged` (`Node.h:1456, 1512`), emitted on any of the three knobs' value change; `NodeGui::onOutputLayerChanged` (`NodeGui.cpp:3186-3228`) draws `getSummary()` (`(All)`, `(diffuse)`, `(Color.rgb, /spec.*/)`; nothing for the default Color-all row); multiplanar branch kept.
  - verify: Xvfb screen shot: a Blur set to `All` shows `(All)` under its name; back to default shows nothing.
  - size: S

## Phase 38.6: Node adoption

- [ ] M38.P6.T1 — Write: channel set on the container drives the encoder's plane list
  - files: `Engine/WriteNode.cpp`, `Engine/WriteNode.h`, `Engine/EffectInstance.cpp`, `Engine/EffectInstance.h`, `Tests/WriteAllLayers_Test.cpp`
  - approach: §4 Write: `channels` (input-bound, input 0, default Color all) at main-page index 0 of the container; after `createWriteNode` set the encoder's `processAllLayers` true/secret/non-persistent, `outputChannels` and `outputComponents` secret; `filterLayersForEmbeddedInput` virtual called from `getAvailableLayers`/`getPresentLayers` when `getIOContainer()` is set, overridden to intersect with `resolve()` (whole layers in this task); Color subset drives the hidden `outputComponents` to the RGBA/RGB/Alpha superset; test switches `:158-160, :183, :193, :232-234` to `setAll()`/`setLayer(Color)` and adds a `Color + diffuse` case expecting 7 channels.
  - verify: `ctest -R WriteAllLayers` green with the three cases; `dump.py` shows `[ChannelSet] channels` first on WriteOIIO and no visible `processAllLayers`/`outputChannels`.
  - size: L

- [ ] M38.P6.T2 — Write: exact channel subsets for non-Color layers
  - files: `Engine/Image.h`, `Engine/ImageCopyChannels.cpp`, `Engine/EffectInstance.cpp`, `Tests/WriteAllLayers_Test.cpp`
  - approach: `Image::extractChannels(indices)` (new, plain copy like `ImageCopyChannels.cpp:121-126`); in the embedded encoder's input fetch (`EffectInstance.cpp:976-987` region) a requested plane that matches an available plane by ID but with a channel subset renders the full plane and extracts; `WriteNode::filterLayersForEmbeddedInput` now emits subset descriptors.
  - verify: test case: rows `Color` + `diffuse[G]` → the EXR has channels `R,G,B,A,diffuse.G` and `diffuse.G == 1`; `specular[R,B]` → `specular.R, specular.B` only.
  - size: M

- [ ] M38.P6.T3 — Read: no channel mechanics (openfx-io task)
  - files: `Engine/ReadNode.cpp`, `Engine/ReadNode.h`
  - approach: §4 Read: after `createReadNode` hide by name `outputComponents`, `outputLayer`, `outputLayerChoice` (pattern `refreshFileInfoVisibility`, `:806-823`); on every filename change re-pin `outputLayer` to the Color entry by ID; leave the plugin's guess of `outputComponents` in place; no `OPENFX_IO_REF` bump.
  - verify: `dump.py`: ReadOIIO shows no visible `outputComponents`/`outputLayer`; gtest: Read of `flat-three-layers.exr` reports present `{Color(RGBA), diffuse, specular}`, an RGB PNG fixture reports `Color(RGB)` only; a new fixture with NO R/G/B/A channels (`Tests/fixtures/flat-no-color-layers.exr`, diffuse+specular only, generated by extending `make-flat-layers-fixture.py`) reports present `{Color, diffuse, specular}` — the first layer is *duplicated* into Color, and `diffuse` is still present as its own plane with its own pixels (Write All → EXR has `R,G,B,diffuse.*,specular.*`); `ctest -R ReadTimeOffset`/existing reader tests green.
  - size: S

- [ ] M38.P6.T4 — Tracker: layer select, no buttons
  - files: `Engine/TrackerContextPrivate.h`, `Engine/TrackerContextPrivate.cpp`, `Engine/TrackerFrameAccessor.cpp`, `Engine/TrackerNode.h`
  - approach: `KnobLayerSelect layer` (input-bound, input 0, no buttons) on the Tracking page where `trackRed/Green/Blue` were (`TrackerContextPrivate.cpp:216-243`, `.h:103-111`, reads `:1162-1165` deleted); `TrackerFrameAccessor.cpp:352-353` requests the resolved layer; `natronImageToLibMvFloatImage` (`:172-205`) averages R,G,B for Color and all channels otherwise; absent layer → no image → existing failure path.
  - verify: gtest (headless): tracking one marker over two frames of a synthetic sequence gives the same result on `Color` as before (golden from the current build) and identical on a `diffuse` copy of the same pixels; `dump.py` shows `[LayerSelect] layer` and no `trackRed`.
  - size: M

- [ ] M38.P6.T5 — Roto / RotoPaint: target layer select with buttons
  - files: `Engine/RotoPaint.cpp`, `Engine/RotoPaint.h`, `Engine/RotoPaintInteract.h`, `Engine/RotoDrawableItem.cpp`, `Tests/RotoLayer_Test.cpp`
  - approach: §4 Roto: `layer` (target, buttons; defaults all-on / A-only) replaces the "Output" separator and four bools (`RotoPaint.cpp:197-214`); `getPreferredMetadata` (`:1349-1375`) advertises the selected layer; on layer change the internal tree's host knobs are set to the same layer (`RotoDrawableItem.cpp:184-208, 261-268`); `copyChannels` (`:1528-1530`) from the select's bits.
  - verify: gtest: RotoPaint (solid brush) over the fixture into `diffuse` with `[R]` only → diffuse.R painted, diffuse.G/B and Color bit-identical to input; into a new 2-channel registry layer `mask2 [A,B]` → 2-channel plane written; into `Color` with A-only (Roto) → alpha only.
  - size: L

- [ ] M38.P6.T6 — Generators: target layer select with adopted quads
  - files: `Engine/OfxEffectInstance.cpp`, `Engine/Node.cpp`, `Tests/GeneratorLayer_Test.cpp`
  - approach: `getLayerKnobSpec` returns `{eLayerSelect, eTarget, true}` for `isGenerator()` (`OfxEffectInstance.cpp:794-815`); `adoptChannelQuad` seeds the buttons from Ramp/Rectangle/Radial's own quads and Constant's host quad; the generator writes its output into the target plane and every other plane passes through from Source untouched (user decision); unselected channels of the target plane are copied from Source's same plane when Source carries it and are zero otherwise.
  - verify: gtest: Constant with `layer = depth` (registered by default) and a downstream Write All → EXR has `depth.Z` with the constant's first value; Ramp over the fixture Source with `layer = diffuse [R]` → only diffuse.R changes; `dump.py` shows `[LayerSelect] layer` first on Constant/Ramp/Radial/Rectangle.
  - size: M

## Phase 38.7: "New layer…" (M36)

- [ ] M38.P7.T1 — Sentinel entry on target knobs → `NewLayerDialog` → `Project::addLayer` → select
  - files: `Gui/LayerChannelRow.cpp`, `Gui/KnobGuiLayerSelect.cpp`, `Gui/NewLayerDialog.cpp`
  - approach: §7: `addItemNew` on target-role combos; on selection open the dialog, call `Project::addLayer(desc, eOriginUser)`, on success push one `KnobUndoCommand` setting the new ID, on cancel/refusal revert the combo without an undo entry and show the registry's message verbatim.
  - verify: Xvfb: Roto panel → "New layer…" → `mask [A]` → the combo shows `mask`, the project Layers page lists it with Used by = 1, Ctrl+Z restores the previous layer while `mask` stays registered; a Blur's channel-set combo has no "New layer…" entry.
  - size: M

- [ ] M38.P7.T2 — PyPlug exporter carries referenced layers
  - files: `Engine/NodeGroup.cpp`, `Tests/PyPlugExport_Test.cpp`
  - approach: in the export path that previously emitted `addUserLayer` (`NodeGroup.cpp:2733-2745`, removed in 38.1.T4) emit `app.addProjectLayer(name, channels)` once per non-built-in ID referenced by any of the three knob types inside the group, before node creation.
  - verify: gtest: export a group containing a Roto targeting `mask [A]`; the script contains exactly one `addProjectLayer("mask", ["A"])` line before the Roto's creation; re-importing into a fresh project registers `mask`.
  - size: S

## Phase 38.8: Deletions, PyPlugs, docs

- [ ] M38.P8.T1 — Delete the old selectors and bools from `Node`
  - files: `Engine/Node.cpp`, `Engine/Node.h`, `Engine/NodePrivate.h`, `Engine/NodeInputs.cpp`, `Engine/OfxClipInstance.cpp`
  - approach: §8 Engine list (`createChannelSelector`, `getSelectedLayer*`, `onLayerChanged`, `refreshEnabledKnobsLabel`, `refreshLayersChoiceSecretness`, `getChannelSelectorKnob`, `getProcessAllLayersKnob`, `getMaskChannel`, the choice loop of `refreshChannelSelectors`, `ChannelSelector`, `MaskSelector::compsAvailable`, knob dispatch); `getAvailableLayers` retained for output-clip `ComponentsPresent` and `kNatronOfxExtraCreatedPlanes`; `OfxClipInstance.cpp:876-888` fallback uses `listLayersForKnob`+`resolve`.
  - verify: `grep -n "channelsSelectors\|processAllLayersKnob\|enabledChan\|getSelectedLayer\b" Engine/*.cpp Engine/*.h` returns zero hits; full `ctest` green; `dump.py` shows no `channels` Choice, no `processAllLayers`, no `*_channels` on any node.
  - size: M

- [ ] M38.P8.T2 — PyPlugs: rewrite the seven, delete ZRemap/ZMask
  - files: `Gui/Resources/PyPlugs/{Fill,AngleBlur,DropShadow,PIKColor,Glow,LightWrap,EdgeBlur}.py`, delete `Gui/Resources/PyPlugs/ZRemap.py`, `ZRemap.png`, `ZMask.py`
  - approach: §8 PyPlugs: `NatronOfxParamProcess*` writes → `getParam("channels").setChannels([...])` / `setLayer(...)`; `EdgeBlur.py:41-53, 471-479` aliases the group's `createChannelSetParam` to the inner Blur's `channels`; remove `premult`/`premultChanged` writes; delete the two implicit-shuffle PyPlugs.
  - verify: `tools/ci/local/test.sh smoke debug` green; a Python background script instantiates each of the seven PyPlugs and asserts the inner node's `getRows()` matches the intended set (e.g. Fill: `Color [A]`); `ZRemap`/`ZMask` absent from `app.getPluginIDs()`.
  - size: M

- [ ] M38.P8.T3 — Knob hints, `ofxNatron.h` note, decision record
  - files: `libs/OpenFX/include/ofxNatron.h`, `Engine/KnobChannelSet.cpp`, `Engine/KnobLayerSelect.cpp`, `Engine/KnobChannelSelect.cpp`, `docs/decisions/2026-09-19-three-layer-knobs.md`
  - approach: `ofxNatron.h` comment at the `kNatronOfxParamProcess*` block (`:285-296`): a standard quad is adopted by the host, forced true, host-masked; knob tooltips state the no-shuffle meaning and the `(not in input)`/`(not in project)` marker; the decision file summarises §0-§6 decisions (`Documentation/` is empty on `main` since M14, so hints and decision files are the docs).
  - verify: `tools/ci/local/test.sh` doc/format gates green; `Node::makeDocumentation` output for a Blur contains the new hint text (grep on the generated HTML via a background script).
  - size: S

## Phase 38.9: Evidence and checkpoint

- [ ] M38.P9.T1 — After-shots
  - files: `build/m38scout/gui.py`, `build/m38scout/run.sh`
  - approach: same tags as 38.2.T1 plus Blur (set with three rows), Roto (New layer…), Tracker, mask footer, Write (All), Constant; produce `after-<tag>.png` beside `before-<tag>.png`; `dump.py` → `dump-after.txt`.
  - verify: all after-shots exist; `diff <(grep ChannelSet dump-after.txt | wc -l) …` counts match §3's table (channel set on Blur/Grade/Transform/Write/Merge; layer select on Constant/Ramp/Roto/RotoPaint/Tracker; none on Read/Shuffle/Premult/DeepMerge).
  - size: S

- [ ] M38.P9.T2 — Packaged release AppImage and user checkpoint
  - files: none (uses `tools/ci/local/package.sh`)
  - approach: package release; hand the user a script: Read `flat-three-layers.exr` → Blur (rows Color + diffuse, then regex `spec.*`) → Roto into a new layer → Write All; confirm no premult knob/warning on Grade/Read/Write/RotoPaint; viewer over checkerboard.
  - verify: the user confirms the six checks on the AppImage; the milestone PR links the before/after shots.
  - size: S

**Verification gate:** `tools/ci/local/test.sh ctest debug` and `test.sh smoke debug` green; `grep -rn -i "ImagePremultiplicationEnum\|premultWarning" Engine Gui` empty; `dump-after.txt` matches §3's table; the 38.4.T4 regression guards and the 38.6 Write/Read/Tracker/Roto/generator tests pass; after-shots for Blur, Roto, Tracker, mask footer, Write, Constant exist; the user checkpoint on the packaged release AppImage passes.

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
