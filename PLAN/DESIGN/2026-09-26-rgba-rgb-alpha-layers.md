# Design: rgba, rgb, alpha and xy colour views (M65, Phase 65.1)

2026-09-28. Implements `DECISIONS/2026-09-26-rgba-rgb-alpha-xy-layers.md`. Every M65 task cites this doc. It supersedes the Color-layer parts of `2026-09-19-layer-registry.md` (§1.1 order, reserved aliases) and `2026-09-19-layer-channel-widget.md` (§1.1 default row, §1.3 `Color.A`, §2 combo order), and amends `2026-09-22-native-shuffle.md` (Color handling, missing-channel error). Scouted on `milestone/m65-rgba-rgb-alpha-layers`; cites are by function, not line.

## 0. What the code does today (the facts the rulings rest on)

- **One colour storage ID, four layouts.** `ImageLayerDesc::getRGBAComponents/getRGBComponents/getAlphaComponents/getXYComponents` (`Engine/ImageLayerDesc.cpp`) all carry `kNatronColorLayerID` (= `kFnOfxImagePlaneColour`) with label `kNatronColorLayerLabel` "Color". `mapNCompsToColorLayer(n)` picks one by count. `isColorLayer()` is an ID test. `findEquivalentLayer` folds all four into one.
- **Bit rule.** `ResolvedLayer::channelBit` (`Engine/KnobChannelSet.h`) maps channel c to bit c, except a 1-channel plane, which maps to bit 3. So RGBA = {0-3}, RGB = {0-2}, Alpha = {3}, XY = {0,1}.
- **Output colour count is chosen by metadata, not by the layer knob.** `EffectInstance::getDefaultMetadata` sets `nComps(-1)` to the *most components* over the connected inputs (after `findClosestSupportedComponents`), and each non-optional input clip to its upstream count. OFX plugins then answer `getClipPreferences` (`OfxEffectInstance::getPreferredMetadata`; HostSupport's default is `findMostChromaticComponents`). `Implementation::checkMetadata` clamps each clip to a supported layout. `getMetadataComponents` turns `nComps` back into a colour desc, and `getMetadataPlanes` → `appendSelectedPlanes` (`getComponentsNeededDefault`) makes that desc the colour plane that is produced. **So today a Grade over an RGB input outputs RGB**: Grade declares RGBA and RGB on Source and Output (`openfx-misc/Grade/Grade.cpp` describe), and nothing widens it. Grade also throws `kOfxStatErrImageFormat` when its src and dst components differ, so widening the output alone would break it.
- **Always-RGBA precedents.** `RotoPaint::getPreferredMetadata` forces `nComps(-1) = 4`. Native Shuffle's `resolveOutputLayerDesc` produces Color as `getRGBAComponents()`.
- **Implicit layout conversion does not read zero.** `getImage` converts an input to the clip's metadata layout through `convertLayersFormatsIfNeeded`. RGB→RGBA fills A with **1** unless `Node::usesAlpha0ToConvertFromRGBToRGBA()` is true, and nothing ever sets that flag. Alpha→RGBA **replicates** A into R, G and B (`ImageConvert.cpp`, `srcNComps == 1` branch). XY→RGBA zeroes B and sets A to 1.
- **The layer knob already refreshes metadata.** `Node::createLayerKnob` sets `setIsMetadataSlave(true)`, so any edit re-runs `refreshMetadata_recursive`.
- **Viewer.** `ViewerInstance` returns black for display channel A when `alphaChannelIndex >= componentsCount` (e.g. RGB). That is already "reads zero".
- **Shuffle today** (`Engine/Nodes/Channel/Shuffle.cpp`). `getEffectiveSource` returns 0 only for a None slot. `checkExtraChannelsPresent` skips None slots and disconnected inputs, and fails on anything else missing ("Channel X.c is not in the N input"). `planeChannelName` names colour indices R, G, B, A regardless of layout.
- **M61 legacy gate.** `Node::adoptChannelQuad` encodes `KnobChannelSet::defaultRows()` (+ the quad's default channels) into `_imp->legacyChannelSetDefault`. **The same string is also `setDefaultValue`'d on non-All nodes.** `Node::loadKnobs` applies it as the *default* when `version < NODE_SERIALIZATION_ALWAYS_SAVES_CHANNEL_SET` (17), `defaultProcessesAllLayers()` is true and no channel set was saved.

## 1. Model

- **One storage colour plane.** Its ID stays `kNatronColorLayerID`, and no user ever sees it. Its layout is RGBA, RGB, Alpha or XY. A stream carries at most one.
- **Four views,** each with its own ID (label = ID): `rgba` {R,G,B,A} mask {0,1,2,3}; `rgb` {R,G,B} {0,1,2}; `alpha` {A} {3}; `xy` {X,Y} {0,1}. View descs are new `ImageLayerDesc`s with the view ID and the same channel names as the matching storage desc. `colorViewForNComps(n)` wraps `mapNCompsToColorLayer(n)`: it returns the view whose channels equal that storage desc's (4 → rgba, 3 → rgb, 1 → alpha, 2 → xy).
- **Aliasing is by bit.** A view channel is the storage bit under it: `rgb.R` = `rgba.R` = bit 0; `xy.X` = bit 0 = `rgba.R`. Writing through one view changes every view.
- **Resolution** = (storage plane, bits). `resolveColorView(view, storage, rowChannels)` returns the view mask ∩ the row's channels, plus a second bitset, "zero bits": the result bits that the storage layout lacks.
- `isColorLayer()` (static and member) is widened to accept the view IDs, as a safety net only. P4.T1's "no view ID in `getPresentLayers`" test stays the guard.

## 2. Present rule

- Every image stream lists `rgba`, `rgb` and `alpha`, whatever its layout (including none: an unconnected input still lists them, as Color is listed today). `xy` is listed only when the storage layout is XY. `presentColorViews(storage)` / `expandColorViews(list)` implement this. Views replace the storage entry in place and come first.
- A colour channel the storage lacks **reads zero** wherever a view resolves it: channel sets, layer selects, channel selects (masks included: `rgba.A` on an RGB mask input is a zero mask), Shuffle and the viewer.
- Engine lists stay storage-only: `getAvailableLayers`, `getPresentLayers` and `listLayersForKnob`. Views appear only through `Node::listLayerViewsForKnob` (= `expandColorViews(listLayersForKnob)`).

## 3. Writing a missing channel (widen-on-write)

**Explicit writes widen; implicit selections do not.**

- **Write bits** = the colour bits a node writes on purpose:
  - a channel set's `layer` rows that name a view (the mask ∩ the row's channels);
  - a layer select that names a view (target or input-bound with buttons);
  - Shuffle: the union of the out1 and out2 view masks (§6).
- `All` rows and `regex` rows resolve colour to **storage bits only**: they select "every channel the stream has" and never widen. Otherwise every default-All node (Blur, Transform, … since M61) would turn an RGB JPEG into RGBA with A = 0, and Write would then emit a transparent PNG.
- **Rule:** if the write bits are not a subset of the storage bits of `nComps(-1)`, the output colour layout becomes RGBA. So does every non-mask input clip whose components type is colour, because of the no-shuffle invariant and Grade's src == dst check. The widen target is always RGBA, never RGB or XY, which matches the RotoPaint and Shuffle precedents.
- **Guard:** widen only if `findClosestSupportedComponents(clip, RGBA)` returns RGBA on the output and on every widened input. Otherwise leave the metadata alone: the plugin cannot hold the channel, so the missing write bits are dropped, as today.
- **Zero fill:** a widened input whose upstream layout is narrower is converted with zero fill: missing R/G/B/A = 0, and Alpha→RGBA gives RGB = 0, not replicated. Conversions not caused by widening (plain OFX clip remapping, e.g. an RGB A-input into Merge) keep today's fill of A = 1 and replicated alpha. That is storage-level behaviour and stays untouched (§4).
- **Where:**
  - A new step at the end of `EffectInstance::Implementation::checkMetadata` (`Engine/EffectInstance.cpp`), after the per-clip clamp. It needs no present list and no time, because layer knobs cannot animate. It sets `nComps` and a new per-clip `NodeMetadata` flag, `colorZeroFill(i)`.
  - `EffectInstance::getColorWriteBits(std::bitset<4>*)` is virtual. The default reads the layer knob rows; Shuffle overrides it.
  - `getImage` passes `colorZeroFill(inputNb)` where it passes `usesAlpha0ToConvertFromRGBToRGBA()` today. That flag is dead and is replaced.
  - `convertLayersFormatsIfNeeded` / `ImageConvert.cpp` get the 1→N zero-fill branch.
  - `appendSelectedPlanes`, `getMetadataPlanes` and the pass-through in `EffectInstanceRenderRoI.cpp` need no change: they already follow metadata.
- **Examples:**
  - Grade `rgba` {R,G,B,A} over RGB gives RGBA out, and A is graded from 0.
  - Grade `rgba` {R,G,B} (Grade's default: its quad has A off) over RGB gives RGB out, unchanged from today.
  - Grade `alpha` over RGB gives RGBA; RGB are copied from the input, A is graded from 0.
  - Blur `All` over RGB gives RGB.
  - Grade `rgb` over Alpha gives RGBA, with RGB graded from 0 and A passed through.

## 4. Boundaries

A view becomes (storage, bits) at exactly four places:
1. **Selection resolution:** `KnobChannelSet::resolve`, `KnobLayerSelect::resolve` and `KnobChannelSelect::resolve`. `ResolvedLayer.desc` is always the storage desc. A row naming the retired storage ID resolves to nothing.
2. **Present-layer listing:** GUI rows (`listLayerEntriesForKnob`, `LayerChannelRow`), the viewer's layer and alpha menus, Python `Effect.getAvailableLayers`, and the knob summaries and Shuffle sublabel.
3. **Registry consumers:** `LayerRegistry::toStoragePlanes` folds views to one `getRGBAComponents()` for `getRegisteredProjectLayersList`, `getUserLayers` (plugins see `kFnOfxImagePlaneColour` exactly once) and the target branch of `Node::listLayersForKnob`.
4. **Native Shuffle** (§6).

**Untouched (storage level):**
- the render path (`renderRoI`, `renderHandler`, pass-through, `appendSelectedPlanes`, `copyUnProcessedChannels`);
- metadata components type (`kNatronColorLayerID`);
- OFX clip mapping (`OfxClipInstance` ComponentsPresent, `OfxEffectInstance::getComponentsNeededAndProduced`, `mapLayerToOFXPlaneString` / `mapOFXPlaneStringToLayer`);
- WriteNode's `filterLayersForEmbeddedInput`;
- cache keys (`ImageParams::_components`);
- `findEquivalentLayer`, `LayerRegistry::groupChannelNames`, `Node::registerProducedLayers` (skips colour);
- non-widening layout conversions;
- deep (M60 must call `expandColorViews` for its lists).

## 5. Registry

- **Built-ins, in order:** `rgba`, `rgb`, `alpha`, `xy`, DisparityLeft, DisparityRight, Backward, Forward, then `depth` (removable, `eOriginUser`). That is 9 entries. `xy` is always registered, so target knobs list it; as a target it writes bits 0-1 and widens per §3. Views are immutable built-ins.
- **Delete `reservedAlias`.** Nothing aliases to colour any more.
- **Refused names** (`isRefusedReservedName`, case-insensitive match unless noted):
  - the existing list: none, all, Backward, Forward, DisparityLeft, DisparityRight, Motion, Disparity;
  - `Color`, `A`, and the storage ID `uk.co.thefoundry.OfxImagePlaneColour`;
  - every case variant of rgba, rgb, alpha and xy other than the exact built-in (e.g. `RGBA`, `Rgb`, `Alpha`, `XY`).
- The exact view IDs hit the built-in-conflict path: re-adding with identical channels is a no-op; with any other channels (`add("rgba",{X})`) it is refused.

## 6. Shuffle

- **Slots and outputs take views.** A view has 4, 3, 1 or 2 channels, named as the view names them. Mapping rows stay stored by slot and view-relative index. A row whose index is out of range for the current view is dormant: kept, not applied.
- **Produced plane:** every colour output produces the one storage plane. Its layout is the main input's colour layout if that covers the union of the output view masks, else RGBA; with no main input it is RGBA. This is the §3 rule via `Shuffle::getColorWriteBits`. The plan's "always RGBA" (M34) is dropped, so Shuffle out `rgb` on a JPEG stays RGB.
- **Overlap:** if out1 and out2 are both colour views and their masks intersect, out2 counts as None (marked "(overlaps out1)"). If the masks are disjoint (rgb + alpha, alpha + xy), they merge into the one plane.
- **Pass-through:** colour bits outside the output masks are copied from the main input's colour plane; bits it lacks are zero. Non-output layers pass through, as before.
- **Implicit wiring between colour views is by bit** (out bit b ← the in view's channel on bit b, if its mask has b; else 0). Within rgba/rgb/alpha this is the same as by name (alpha ← rgba reads A); for xy it gives X↔R and Y↔G, which by-name wiring could not. The plan says "by channel name"; read it as by bit.
- **Missing sources** (the `getEffectiveSource` / `checkExtraChannelsPresent` / `render` rule):
  - slot None → 0, silent (unchanged);
  - input disconnected → 0, silent (unchanged);
  - slot is a colour view on a connected input, and the wired bit is missing from storage → **0, silent**;
  - slot is a non-colour layer absent from the connected input, or the wired channel is beyond that layer → **fails**, with M61's message and persistent error (unchanged).
- `rgba → rgba` with no overrides and no out2 is an identity.

## 7. Clean break

- **Load.** The colour `KnobChoiceOptionFilter`s in `KnobSerialization.cpp` are deleted, so Natron ≤ 2.2 OFX colour choice values fall back to the plugin default without throwing. At the end of `Node::loadKnobs`, after `restoreUserKnobs` so that PyPlug user knobs are covered, one scan rewrites every layer-knob value that names the storage ID:
  - channel-set `layer` rows: storage ID → `rgba`, **channels kept** (pixel-identical, since rgba's mask is the old Color's);
  - layer selects: → `rgba`, channels kept. Shuffle's in and out slots included; mapping indices are unchanged because rgba index = old Color index;
  - channel selects: `<storageID>.C` → `rgba.C`;
  - the viewer's saved `layerName` "Color", or anything no longer in its menu → `rgba` (P6.T2, no warning).
- **Warning.** Any rewrite posts **one persistent warning per node**: "Colour layer from an older project was reset to rgba". Post it with `setPersistentMessage`, not `setChannelSelectorMessage`, so M61's clear-on-completed-render does not erase it.
- "Reset to the default" in the plan means the layer ID resets to `rgba`, *not* the value to the node's default. For default-All nodes the default is All, which would change which layers are processed.
- **Python.** Layer setters raise `ValueError` on the storage ID, naming the views. `KnobChannelSet::setChannels`'s row-0 shim writes `rgba`. The PyPlug exporter writes view IDs.
- **v17 gate (settled in Decisions §3).** Pre-v17 projects land on literal Color first; the scan then rewrites them with the warning.

## 8. Viewer

- The layer menu is `expandColorViews(present)`, labelled by view ID. Choosing any view calls `setActiveLayer(storage)`: map the entry back through `isColorLayer()` before it reaches `setActiveLayer` / `setAlphaChannel` in `refreshLayerAndAlphaChannelComboBoxAtTime`. Keep M61's `keepAbsentSelection` behaviour.
- `alpha` switches the display channels to A through the existing auto-switch. `rgba` and `rgb` switch it back to RGB. On RGB storage the A display is already black (`ViewerInstance`), which agrees with "reads zero".
- The alpha menu lists `rgba.A` once.

## 9. Risks specific to these rulings

- **Zero fill vs. A = 1.** An RGB stream's A is 0 through views but still 1 through plain OFX clip remapping (e.g. a JPEG as Merge's A input). The two paths disagree by design, to keep Merge-over-JPEG unchanged. The user should confirm this at UAT; if they want A = 0 everywhere, that is a one-flag change at the `getImage` site plus a `NATRON_CACHE_VERSION` bump.
- **Mask zero reads.** A mask on `rgba.A` from an RGB input now masks everything out, where it used to be treated as absent. It is Nuke-consistent; call it out in UAT.
- **Widen makes layouts depend on knobs.** Metadata already refreshes on layer-knob edits (it is a metadata slave), and the node hash covers the knob, so caches stay consistent. P7.T4's cache-version bump still clears stale disk entries.

## Decisions

1. **Widen-on-write: confirmed, narrowed.**
   - **Grounding:** today output colour count = the most components over inputs (`getDefaultMetadata`, the OFX default clip preferences). Grade over RGB outputs RGB, and Grade requires src == dst.
   - **Ruling:** when a node's *explicit* colour write bits (view rows, a view layer select, Shuffle's output views) exceed the storage bits, `checkMetadata` widens the output **and** every colour non-mask input clip to RGBA, if the plugin supports RGBA on those clips. Widened inputs are converted with zero fill (a new `NodeMetadata::colorZeroFill` flag, consumed in `getImage` in place of the dead `usesAlpha0ToConvertFromRGBToRGBA()`, plus a 1→N zero branch in `ImageConvert.cpp`). All and regex rows never widen.
   - **Rationale:** metadata is where layouts are decided, and the render path already follows it, so no render-path change is needed; widening only the output would break Grade's src/dst check. Letting All widen would silently add alpha = 0 to every RGB stream through default-All nodes.
2. **Shuffle missing colour channel reads zero: confirmed.** This amends M61 P3.T2 for colour views only.
   - Exact rule: a None slot → 0, silent; a disconnected input → 0, silent; a colour-view slot on a connected input whose wired bit is missing → 0, silent; a missing non-colour layer, or a channel beyond that layer, on a connected input → the render fails with M61's message.
   - Colour output layout follows §3: the main input's layout if it covers the written bits, else RGBA.
3. **v17 gate × clean break.**
   - `adoptChannelQuad` computes **two** encodings from the same quad channels:
     - `_imp->legacyChannelSetDefault` = `KnobChannelSet::legacyColorDefaultRows()`, which names the storage ID;
     - the live non-All default = `defaultRows()` (`rgba`).
   - The plan's "point `adoptChannelQuad()` at `legacyColorDefaultRows()`" must change only the legacy capture. Today the same `_imp->legacyChannelSetDefault` string is also `setDefaultValue`'d on non-All nodes, which would make every new Grade default to the retired ID.
   - On load, the gate installs the literal-Color default as today. The §7 scan then sees the storage ID and rewrites the *value* to `rgba` with the same channels. It also restores the knob's *default* to the node's live default (All or rgba), so "Reset to default" never returns the retired ID. Then it posts the warning.
   - A pre-v17 Blur with no saved channel set therefore ends at `rgba` with the warning. A pre-v17 Grade with no saved set (a non-All node, where the gate does not fire) ends at its live default `rgba` with no warning: nothing named Color.
