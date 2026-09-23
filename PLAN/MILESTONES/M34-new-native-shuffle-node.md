# Milestone 34: New native Shuffle node

Governing design doc: `PLAN/DESIGN/2026-09-22-native-shuffle.md` (v1, approved 2026-09-22; its "Answers" section amends §1). The design doc governs whenever a brief below is ambiguous. M38 built the machinery this milestone uses: the project LayerRegistry; the knobs `KnobChannelSet`, `KnobLayerSelect` and `KnobChannelSelect`; the per-plane render model; and the rule that nodes process in place and only Shuffle moves data between layers.

Execution notes carried over from M38:
- The `natron-dev` container is single-tenant, so one build runs at a time. Launch builds detached with a done-marker.
- The debug build defines NDEBUG, so tests use EXPECT/ASSERT, not assert().
- GUI checks run under Xvfb (recipe `build/deeprepro/run-gui.sh`, fixtures under `build/`).
- Objects with empty ninja deps records can go stale after header changes.

## Phase 34.1: Engine foundations

- [x] M34.P1.T1 — Let `KnobLayerSelect` hold an optional None
  - files: `Engine/KnobLayerSelect.h`, `Engine/KnobLayerSelect.cpp`, `Engine/Knob.cpp`, `Tests/KnobLayerSelect_Test.cpp`
  - approach: `setAllowNone(bool)` is a per-node-kind flag like `withChannelButtons` and is never persisted. An empty Layer cell means None. With None set, `resolve()` returns false, `getReferencedLayerIDs` adds nothing and `getSummary()` returns "None". `setLayer("")` is refused unless None is allowed. The alias branch (`Knob.cpp` ~4578) copies the flag.
  - verify: `ctest -R KnobLayerSelect` covers: None round-trips through encode/decode; `resolve` returns false; no referenced IDs; `setLayer("")` throws when None is not allowed.
  - size: S
- [x] M34.P1.T2 — Offer a "None" entry in the layer-select row when it is allowed
  - files: `Gui/LayerChannelRow.h`, `Gui/LayerChannelRow.cpp`, `Gui/KnobGuiLayerSelect.cpp`, `Tests/LayerChannelRow_Test.cpp`
  - approach: in `eModeLayer`, prepend an `eKindNone` entry when the knob allows None. Choosing it writes an empty layer as one undo step.
  - verify: `ctest -R LayerChannelRow`: the row lists `None` first only for an allow-None knob; choosing it writes `""`, and one undo restores the previous layer.
  - size: S
- [x] M34.P1.T3 — Give plugin-owned layer knobs a listing role and count them as registry references
  - files: `Engine/Node.h`, `Engine/Node.cpp`, `Engine/NodePrivate.h`, `Tests/LayerKnobs_Test.cpp`
  - approach: add `Node::declareLayerKnob(knob, inputNb, role)` and `Node::setLayerKnobInput(knob, inputNb)`, which add to or update `layerKnobSources`. `getReferencedLayerIDs` walks every declared, non-driven knob, not just the host `layerKnob`. `setLayerKnobInput` emits `layerListRefreshed`.
  - verify: gtest on a user `KnobLayerSelect` created on a NoOp:
    - Declared as target, it lists the registry.
    - Declared input-bound on input 0, fed by `flat-three-layers.exr`, it lists Color/diffuse/specular.
    - Set to `diffuse`, `removeLayer("diffuse")` is refused and names the NoOp.
    - After `setLayerKnobInput(knob, 1)` with input 1 unconnected, it lists Color only.
  - size: M
- [x] M34.P1.T4 — Let a multiplanar effect opt out of implicitly producing its metadata layer
  - files: `Engine/EffectInstance.h`, `Engine/EffectInstance.cpp`, `Tests/MultiplanarTestEffect.h`, `Tests/wmain.cpp`, `Tests/LayerKnobsRender_Test.cpp`
  - approach: a virtual `producesMetadataLayerImplicitly()`, defaulting to true, gates the metadata-layer merge in `getComponentsNeededAndProduced_public` (~`EffectInstance.cpp:4487-4507`). Add a multiplanar test effect that produces only `diffuse`, registered like the DataKind test plugins.
  - verify: gtest with Read(`flat-three-layers.exr`) → test effect:
    - Flag false: `comps[-1] == {diffuse}`, and Color and specular pass through.
    - Flag true (the default): Color is in `comps[-1]`.
    - Full ctest green.
  - size: M
- [x] M34.P1.T5 — Add the `KnobShuffleMap` knob type and its codec
  - files: `Engine/KnobShuffleMap.h`, `Engine/KnobShuffleMap.cpp`, `Tests/KnobShuffleMap_Test.cpp`, `Tests/CMakeLists.txt`
  - approach:
    - A `KnobTable` subclass with tags `Out` and `Src`, and typeName `ShuffleMap`.
    - Rows are `<Out>out1.3</Out><Src>in2.0</Src>`, where `Src` is `inJ.i`, `0` or `1`. "Keep" is the absence of a row.
    - API: `getSource/setSource/clear/reset/getRows`, with struct `ShuffleSource { enum {eKeep,eInput,eZero,eOne}; int slot; int index; }`.
    - Each setter is exactly one `setValue`. No expressions.
  - verify: `ctest -R KnobShuffleMap`:
    - encode/decode round-trip
    - keep means no row
    - `reset()` empties the table
    - malformed cells decode as keep
    - setting `out1.3` twice leaves one row
  - size: M
- [x] M34.P1.T6 — Register `KnobShuffleMap` for creation and project load
  - files: `Engine/KnobFactory.cpp`, `Engine/KnobSerialization.cpp`, `Tests/KnobShuffleMap_Test.cpp`
  - approach: one `knobFactoryEntry` and one `KnobSerialization::createKnob` case, the same touch points M38 used for its knob types.
  - verify: gtest: a user `KnobShuffleMap` on a NoOp comes back from a project save/reset/load with identical rows.
  - size: S

## Phase 34.2: The native Shuffle node

- [x] M34.P2.T1 — Add the Shuffle node skeleton: description, knobs, layer declarations, planes needed/produced, metadata, RoD
  - files: `Engine/Nodes/Channel/Shuffle.h`, `Engine/Nodes/Channel/Shuffle.cpp`, `Engine/AppManager.cpp`, `Tests/Shuffle_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: follow design §1. The node is `fr.natron.Shuffle` on `NativeEffectBase`, with inputs B (0) and A (1). Knobs:
    - `in1Input` and `in2Input`
    - `in1` and `in2`: allow None, declared input-bound via P1.T3
    - `out1` and `out2`: target; `out2` allows None; `out2 == out1` resolves to None
    - `mapping`

    Changing `inNInput` calls `setLayerKnobInput`. The node is multiplanar with `producesMetadataLayerImplicitly() == false`, passes non-rendered layers through from B, and overrides `getComponentsNeededAndProduced` per design §1. Format, PAR and frame range come from B, or from A when B is disconnected. `render()` is a stub.
  - verify: gtest on Read(`flat-three-layers.exr`) → Shuffle:
    - the host `getLayerKnob()` is null
    - `out1` lists the registry, and `in1` lists Color/diffuse/specular
    - with `out1=spec2` (registered), `comps[-1]=={spec2}` and Color/diffuse/specular pass through
    - with `in2Input=A` and A unconnected, `in2` lists Color only
    - `removeLayer("diffuse")` is refused while `in1=diffuse`
    - `isIdentity` is true on a new node
  - size: L
- [x] M34.P2.T2 — Render the mapping: input channels, constants and keep
  - files: `Engine/Nodes/Channel/Shuffle.cpp`, `Tests/ShuffleRender_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: for each plane in `args.outputLayers`, fetch the slot planes and B's same plane once with `getImage(inputNb, …, &layer, …)` (the RotoPaint pattern), then fill each channel.
    - Keep takes B's same channel, or 0 when B lacks it.
    - A disconnected input or a None slot behaves as keep.
    - Colour output is always RGBA.
  - verify: pixel gtests on `flat-three-layers.exr`, read through Write in All mode (the WriteAllLayers pattern):
    - (a) `in1=diffuse`, `out1=Color` → Color (0,1,0,1)
    - (b) two-layer swap in one node; Color stays (1,0,0,1)
    - (c) `out1.A←0` and `out1.R←1`
    - (d) A = Constant(0.5), `out1.A←in2.A` → alpha 0.5
    - (e) `out1=mask`[A]`←in1.R` → a new `mask` plane of 1; Color untouched
    - (f) `in2Input=A`, A disconnected, `out1.A←in2.A` → alpha kept from B
  - size: L
- [x] M34.P2.T3 — Fail the render when a wired source is missing upstream
  - files: `Engine/Nodes/Channel/Shuffle.cpp`, `Engine/Node.cpp`, `Engine/Node.h`, `Tests/ShuffleRender_Test.cpp`
  - approach: this is the user decision (design doc "Answers" 2). A mapping row fails the render with a persistent error naming the channel when its slot's input is connected but lacks that slot's layer, or when its channel index is beyond that layer. Reuse or extend `Node::checkSelectedChannelsPresent`, the M38 mask-channel error path, so the badge and the clearing behaviour match. A disconnected input, a None slot and unwired channels stay silent.
  - verify: gtests:
    - Read(`flat-rgba-only.exr`) → Shuffle with `in1=diffuse` and `out1.R←in1.R` fails the render with a persistent error naming `diffuse`.
    - Switching the Read to `flat-three-layers.exr` clears the error and the render succeeds.
    - `in1=diffuse` with no wired rows renders without error.
    - A None slot with a wired row renders as keep.
  - size: M
- [x] M34.P2.T4 — Expose `ShuffleMapParam` to Python
  - files: `Engine/PyParameter.h`, `Engine/PyParameter.cpp`, `Engine/PyNode.cpp`, `Engine/typesystem_engine.xml`, `Engine/PySide6_Engine_Python.h`
  - approach: `connect(src, dst)` (with `"0"` and `"1"` allowed as `src`), `disconnect`, `getSource` (returns `""` for keep), `getConnections` and `reset`. Names resolve against the node's current slot layers; unknown slots or channels raise `ValueError` with the reason.
  - verify: a background-mode script (`-b`, as `build/m38scout/gui.py` does) asserts each API call, a `ValueError` on `"in1.Q"`, and that `setExpression` on `mapping` raises.
  - size: M
- [x] M34.P2.T5 — PyPlug export emits Shuffle mappings and round-trips them
  - files: `Engine/NodeGroup.cpp`, `Tests/PyPlugExport_Test.cpp`
  - approach: after the layer selects, emit `getParam("mapping").connect(...)` lines. Referenced custom layers already go out through `addProjectLayer`.
  - verify: `ctest -R PyPlugExport`: a group containing a Shuffle (`in1=diffuse`, `out1=spec2`, `out1.A←1`) exports, re-imports, and has identical knob values.
  - size: M

## Phase 34.3: Matrix UI

- [ ] M34.P3.T1 — Add the `KnobGuiShuffleMap` toggle matrix
  - files: `Gui/KnobGuiShuffleMap.h`, `Gui/KnobGuiShuffleMap.cpp`, `Gui/KnobGuiFactory.cpp`, `Tests/ShuffleMatrix_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: design §2.
    - Derive from `KnobGuiLayerChannelBase` so it refreshes on `layerListRefreshed` and `projectLayersChanged`.
    - One row per output channel, each an exclusive `QButtonGroup`, with columns for the in1 channels, the in2 channels, keep, 0 and 1. Use `Button` with the R/G/B/A colours.
    - Each click pushes one non-mergeable undo command.
    - `out2` rows appear only when `out2` is set.
    - An absent slot layer greys out its columns and shows the "(not in input)" marker.
    - A Reset button.
  - verify: GUI gtest:
    - With `out1=Color` and `in1=diffuse`, there are 4 rows × (3 + keep + 0 + 1) buttons.
    - Clicking R←G changes one value, and undo restores it.
    - `out1=depth` gives 1 row.
    - `out2=None` hides the `out2` rows.
  - size: L
- [ ] M34.P3.T2 — Panel layout, knob hints and node-graph sub-label
  - files: `Engine/Nodes/Channel/Shuffle.cpp`, `build/m34scout/gui.py`, `build/m34scout/run.sh`
  - approach: put `inNInput` on the same line as `inN`, give every knob a hint, and keep a hidden sub-label knob (`kNatronOfxParamStringSublabelName`, the PrecompNode precedent) updated in `knobChanged`, e.g. `(diffuse → Color)`.
  - verify: Xvfb screenshots of the panel for the diffuse→Color example and of the node-graph label.
  - size: M

## Phase 34.4: Replace the OFX Shuffle

- [x] M34.P4.T1 — Port DropShadow, EdgeBlur, Fill and Glow to the native Shuffle
  - files: `Gui/Resources/PyPlugs/DropShadow.py`, `Gui/Resources/PyPlugs/EdgeBlur.py`, `Gui/Resources/PyPlugs/Fill.py`, `Gui/Resources/PyPlugs/Glow.py`
  - approach: translate each `outputX = "<in>.<plane>.<chan>"` into `in1`/`in2`/`out1` settings plus `connect()` calls. EdgeBlur's `outputComponents=Alpha` becomes `out1.R/G/B←0` with A kept.
  - verify:
    - `test.sh smoke debug` is green.
    - A background script instantiates each PyPlug, asserts the inner Shuffle's `getConnections()`, and renders one frame of a Constant through it with the expected pixel.
  - size: M
- [ ] M34.P4.T2 — Port LightWrap and PIKColor; replace PIKColor's `outputA` expression with a callback
  - files: `Gui/Resources/PyPlugs/LightWrap.py`, `Gui/Resources/PyPlugs/PIKColor.py`
  - approach: the same translation as P4.T1. PIKColor's `screenType` `onParamChanged` callback sets `connect("in2.G"|"in2.B","out1.A")`, following the Glow precedent from M38.8.T2.
  - verify: the P4.T1 script covers both PyPlugs, and toggling `screenType` flips the inner connection.
  - size: M
- [ ] M34.P4.T3 — Restore ZRemap and ZMask on the native Shuffle
  - files: `Gui/Resources/PyPlugs/ZRemap.py`, `Gui/Resources/PyPlugs/ZRemap.png`, `Gui/Resources/PyPlugs/ZMask.py`
  - approach: restore both from `fd33e5ab8^`. Replace the `Source_channels` choice with an inner Shuffle (`in1=depth`, `out1=Color←Z`) whose `in1` is aliased to a group-level layer select.
  - verify: both IDs are in `app.getPluginIDs()`, and Constant → Shuffle(`out1=depth←in1.R`) → ZRemap renders the expected remapped value.
  - size: M
- [x] M34.P4.T4 — Delete the host's OFX-Shuffle special cases
  - files: `Engine/NodeInputs.cpp`, `Engine/EffectInstance.cpp`, `Engine/EffectInstance.h`, `Engine/KnobSerialization.cpp`, `Tests/LayerKnobs_Test.cpp`
  - approach: delete the four `PLUGINID_OFX_SHUFFLE` "A is main" branches, the macro, and the `outputR..A` filter entries (~`KnobSerialization.cpp:640-656`). `LayerKnobs_Test.cpp:374` now asserts on the native Shuffle.
  - verify: `grep -rn PLUGINID_OFX_SHUFFLE Engine Gui` is empty; full ctest green.
  - size: M
- [ ] M34.P4.T5 — Drop Shuffle from the openfx-misc fork build and bump the pin
  - files: `charlesangus/openfx-misc` (a fork PR removing `Shuffle` from its build), `tools/ci/local/fetch-assets.sh` (`OPENFX_MISC_REF`)
  - approach: this is the user decision (design doc "Answers" 3). Graphs containing the OFX Shuffle no longer load it, which the clean-break decision accepts. Keep the fork change minimal: remove the plugin from the build list and its source directory. Leave a one-line pin comment pointing at the fork PR.
  - verify:
    - `tools/ci/local/fetch-assets.sh` rebuilds `Misc.ofx.bundle`.
    - A gtest asserts `net.sf.openfx.ShufflePlugin` is absent from the plugin list, and that the native Shuffle is the only "Shuffle" in the Channel group.
    - Full ctest green.
  - size: M

## Phase 34.5: Checkpoint

- [ ] M34.P5.T1 — Packaged release AppImage and user checkpoint
  - files: none (uses `tools/ci/local/package.sh`)
  - approach: package a release build and give the user a UAT script covering:
    - diffuse→Color
    - A's alpha into B
    - a two-layer swap in one node
    - "New layer…" on `out1`
    - a missing wired source failing with an error, and clearing when the source returns
    - Layers-page removal refused while a Shuffle uses the layer
    - the ported PyPlugs, ZRemap and ZMask
    - only one Shuffle in Tab and the menus
  - verify: the user confirms on the AppImage; the milestone PR links the after-shots.
  - size: S

**Verification gate:** all of the following:
- `tools/ci/local/test.sh ctest debug` and `test.sh smoke debug` are green, including the new KnobShuffleMap, Shuffle, ShuffleRender and ShuffleMatrix tests.
- `grep -rn "ShufflePlugin" Gui/Resources/PyPlugs` and `grep -rn PLUGINID_OFX_SHUFFLE Engine Gui` are both empty.
- The Xvfb panel and node-graph shots exist.
- The user checkpoint on the packaged release AppImage passes.

## Decisions

- 2026-09-22 — **Design approved with four user answers** (design doc "Answers"):
  - Two output slots.
  - An unwired channel keeps B's value. A wired source missing from a connected input fails the render, following the M38 mask rule; a disconnected input or a None slot stays silent.
  - The OFX Shuffle is deleted from the openfx-misc fork build rather than hidden.
  - The mapping UI is a toggle matrix.
  - The no-RGBA Read residual was not asked about; the default is taken and it is left alone.
- 2026-09-23 — **PM resumed after a crashed session**; the Docker daemon had lost the natron-dev image and it was rebuilt via `mirror.gcr.io`. P1.T2, P2.T2 and P2.T4 were found implemented but uncommitted, verified (targeted ctest 44/44 with `OFX_PLUGIN_PATH` set; `build/m34scout/shufflemap_py.py` OK under NatronRenderer -b) and committed. The ShuffleRender segfault reported earlier did not reproduce.
- 2026-09-23 — **User: share UI screenshots before signing off.** P3.T1 (matrix) and P3.T2 (panel/node-graph label) stay unchecked until Xvfb screenshots have been sent to the user and they approve them.
