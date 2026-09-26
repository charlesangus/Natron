# Milestone 65: rgba, rgb, alpha and xy replace the Color layer

M65 replaces the one user-visible **Color** layer with four built-in registry layers, each with its own ID: `rgba` {R,G,B,A}, `rgb` {R,G,B}, `alpha` {A} and `xy` {X,Y}. All four are **colour views** of the one stored colour plane. A stream still carries at most one colour plane. `rgb.R` *is* `rgba.R`, so writing into `rgb` changes `rgba`. Selecting `rgb` or `alpha` on a node means the colour plane masked to those channels.

**`rgba`, `rgb` and `alpha` are always present.** A colour channel the stream doesn't carry reads as zero. For example, an RGB-only JPEG still lists `alpha`, and its A reads black. `xy` is present only when the colour plane has the 2-channel XY layout; it maps to channel bits 0-1.

**Design: one storage plane, many views.** The storage plane keeps its internal storage/OFX ID `kNatronColorLayerID` (= `kFnOfxImagePlaneColour`), which users never see. A view turns into (storage plane, channel mask) at four boundaries:
- selection resolution: `KnobChannelSet`, `KnobLayerSelect` and `KnobChannelSelect` `resolve()`;
- present-layer listing: GUI rows, the viewer and Python;
- registry consumers;
- native Shuffle.

Everything that works at storage level stays as it is: the render path, OFX clip mapping, WriteNode, cache keys, `findEquivalentLayer` and `groupChannelNames`. Giving the storage plane several IDs was rejected. Every one of those sites would have to fold the IDs back into one plane: 21 accessor sites in `EffectInstance.cpp` and 8 in `EffectInstanceRenderRoI.cpp`. It would also let `rgb` and `rgba` be cached as separate images that drift apart. The channel bits already line up: `ResolvedLayer::channelBit` (`KnobChannelSet.h:95-99`) puts a single-channel plane on bit 3. The masks are rgba = {0-3}, rgb = {0-2}, alpha = {3} and xy = {0,1}.

**Scale (scouted 2026-09-26):**
- 24 lines in 11 files name `kNatronColorLayerID`/`kNatronColorLayerLabel`. 13 of them are legacy filters in `KnobSerialization.cpp:576-726`, which this milestone deletes.
- About 150 test hits mention Color, across 16 files. Most assert storage-level present layers and stay as they are.

**Latent bugs found while scouting:**
- The bundled PyPlugs `Glow.py:39`, `PIKColor.py:383/484/526/641` and `LightWrap.py:1172` hard-code `uk.co.thefoundry.OfxImagePlaneColour`.
- `Gui/LayerChannelRow.cpp:54` defines `kColorLayerID = "Color"`, so the row-0 colour hoist at `:534` never fires.

Execution notes:
- **Branching:** stacked on M61. Branch `milestone/m65-rgba-rgb-alpha-layers` off `milestone/m61-layers-that-vary-with-time` and open the PR against it (`DECISIONS/2026-09-22-stacked-milestone-prs.md`). M37 stacks on M65.
- **Builds:** the `natron-dev` container is single-tenant. Implementers edit in parallel and don't build. Each batch gets one detached build plus ctest (setsid+nohup with a fresh `.done` marker). Check that `pgrep -x ninja` is 0 before relaunching, and never pgrep-wait on a build. Run tests through `build/m61ctest.sh <regex>` (it sets `OFX_PLUGIN_PATH`) or `tools/ci/local/test.sh`.
- **Tests:** the debug build defines NDEBUG, so tests use EXPECT/ASSERT, never assert().
- **GUI checks:** run under Xvfb with the recipe in `build/deeprepro/run-gui.sh`. Scripts and fixtures go under `build/m65-gui/`.
- **UAT:** package the release AppImage and launch-check it with the devshell `LD_LIBRARY_PATH` stripped (`build/appimages/run-launch-check.sh`).
- **Batches.** Tasks within a batch touch disjoint files:
  - B1: P1.T1, P2.T1
  - B2: P3.T1, P3.T2, P3.T3, P3.T4
  - B3: P4.T1, P5.T1, P7.T2, P7.T4
  - B4: P4.T2, P5.T2, P6.T1, P7.T1
  - B5: P5.T3, P6.T2, P7.T3
  - B6: P6.T3, P8.T1, P8.T2
  - B7: P8.T3
- **Shared files:** `Engine/EffectInstance.cpp` and `Engine/Node.cpp` are touched by P3.T1, P4.T1 and P7.T3, all in different batches. `Shuffle.cpp` is touched by P3.T3, P5.T1 and P5.T2, also in different batches. Only P2.T1 and P4.T1 edit `Tests/CMakeLists.txt`.

## Phase 65.1: Design

- [ ] M65.P1.T1 — Write the colour-views design doc
  - files: `.plan/PLAN/DESIGN/2026-09-26-rgba-rgb-alpha-layers.md` (new, in the plan worktree; commit it there)
  - approach: the doc fixes the following points, and every later task cites it:
    - **Model:** one storage colour plane with the internal ID `kNatronColorLayerID`, and four views with masks rgba {0-3}, rgb {0-2}, alpha {3}, xy {0,1}.
    - **Present rule:** `rgba`, `rgb` and `alpha` are always listed on every image stream. A colour channel the stream lacks reads as zero. `xy` is listed only for XY storage.
    - **Writing a missing channel:** when a node writes a colour channel the input lacks (e.g. Grade on `rgba` over an RGB stream), the output colour plane widens to RGBA. The doc must confirm this, or record the alternative and why.
    - **Boundaries:** the four boundary points, plus the list of storage-level sites that stay untouched.
    - **Registry:** the order is rgba, rgb, alpha, xy, DisparityLeft, DisparityRight, Backward, Forward, depth. The refused names are the existing list plus `Color`, `A` and non-exact case variants of the four view IDs (e.g. `RGBA`, `XY`).
    - **Shuffle:** channels outside the output views pass through from the main input. Output views that don't overlap merge; if they overlap, Out 2 counts as None. Implicit wiring between colour views goes by channel name. A colour channel that is read but missing reads zero; a missing *non-colour* channel still fails, as M61 P3.T2 has it.
    - **Clean break:** old values reset to the default with a warning, and the Python setters reject the retired ID.
    - **Viewer:** choosing `alpha` switches the display to A.
    - **Pointers:** add a note pointing to this doc in `2026-09-19-layer-registry.md` and `2026-09-19-layer-channel-widget.md`.
  - verify: PM review. The doc's Decisions section settles the widen-on-write rule and the Shuffle missing-colour-channel rule.
  - size: M

## Phase 65.2: Colour-view foundation

- [ ] M65.P2.T1 — Add the colour-view API to ImageLayerDesc
  - files: `Engine/ImageLayerDesc.h`, `Engine/ImageLayerDesc.cpp`, `Tests/ColorViews_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach:
    - Add the ID constants `kNatronColorViewRGBA "rgba"`, `…RGB "rgb"`, `…Alpha "alpha"` and `…XY "xy"`. Each label equals its ID.
    - Add these statics:
      - `getColorView(id)` and `isColorViewID(id)`.
      - `colorViewMask(id)`.
      - `colorStorageBits(storage)`, using the `channelBit` rule, with XY on {0,1}.
      - `presentColorViews(storage, list*)`: always rgba, rgb and alpha, plus xy for XY storage.
      - `resolveColorView(viewID, storage, rowChannels, bitset*)`. Its result is the view mask ∩ the row's channels. A result bit the storage lacks means "reads zero"; report those bits through a second out-param.
      - `colorViewForNComps(n)`: 4 → rgba, 3 → rgb, 1 → alpha, 2 → xy.
      - `expandColorViews(storageList)`: replaces the storage colour entry, in place and first, with the views it presents.
    - Widen static and member `isColorLayer` (`ImageLayerDesc.cpp:120-130`) so it is also true for view IDs. That way a view that leaks into an internal list still folds into the colour plane in `findEquivalentLayer` and `mapLayerToOFXPlaneString` (`:473-476`).
  - verify: `ctest -R ColorViews` passes these cases:
    - RGBA, RGB and Alpha storage each present {rgba, rgb, alpha}; XY presents {rgba, rgb, alpha, xy}.
    - `resolveColorView("rgb", RGBA)` gives 0b0111 with no zero bits.
    - `("alpha", RGB)` gives 0b1000 with bit 3 reading zero.
    - `("xy", XY)` gives 0b0011.

    The full debug ctest stays green, since behaviour doesn't change yet.
  - size: M

## Phase 65.3: Registry and knob model

- [ ] M65.P3.T1 — Make rgba, rgb, alpha and xy the built-in registry layers
  - files: `Engine/LayerRegistry.h`, `Engine/LayerRegistry.cpp`, `Engine/EffectInstance.cpp`, `Engine/Node.cpp`, `Engine/PyAppInstance.cpp`, `Tests/LayerRegistry_Test.cpp`
  - approach:
    - In the constructor (`LayerRegistry.cpp:173-201`), replace the `getRGBAComponents()` built-in with the four views, in the design-doc order.
    - Delete `reservedAlias` (`:75-85`) and its call in `add` (`:241-243`).
    - Extend `isRefusedReservedName` (`:88-95`) with the names from the design doc.
    - Add `LayerRegistry::toStoragePlanes(snapshot, list*)`, which folds the views into one `getRGBAComponents()`.
    - Switch the internal consumers to `toStoragePlanes`:
      - `getRegisteredProjectLayersList` (`EffectInstance.cpp:170-181`);
      - `getUserLayers` (`:198-210`), so plugins still get `kFnOfxImagePlaneColour` exactly once;
      - the target branch of `Node::listLayersForKnob` (`Node.cpp:5809-5812`).
    - Drop the alias branch in `App::addProjectLayer` (`PyAppInstance.cpp:547-554`).
  - verify: `ctest -R LayerRegistry` passes these cases:
    - The built-in order is correct and the count is 9 (it was 6, `:65`).
    - `add("rgba",{X})`, `add("Color",…)`, `add("RGBA",…)` and `add("XY",…)` are refused.
    - Removing `rgb` is refused because it is a built-in.
    - The Layers-knob rows start rgba/rgb/alpha/xy (`:439`, `:473`).
    - `toStoragePlanes` gives one colour entry.

    `ctest -R 'LayerKnobs|WriteAllLayers|TimeVaryingLayers'` stays green.
  - size: M

- [ ] M65.P3.T2 — Resolve colour views in KnobChannelSet
  - files: `Engine/KnobChannelSet.cpp`, `Engine/KnobChannelSet.h`, `Gui/KnobGuiChannelSet.cpp`, `Tests/KnobChannelSet_Test.cpp`, `Tests/ChannelSetRender_Test.cpp`
  - approach:
    - In `resolve()` (`:493-559`):
      - A row that names a view matches the storage colour plane, with its bits from `resolveColorView`. This works even when a bit is missing from storage, because missing bits read zero.
      - A row that names the retired storage ID matches nothing.
      - Regex rows match the labels from `presentColorViews` and every other layer's label. Results still accumulate by storage ID, so `All`, or a regex that matches several views, yields a single colour entry with the union of their bits.
    - `defaultRows()` (`:101-108`) becomes `rgba`.
    - `layerLabelForID` (`:599-611`) returns the view ID.
    - The regex summary lists only the widest view it matched.
    - In the GUI, the default for a new row (`KnobGuiChannelSet.cpp:460`) becomes `rgba`.
  - verify: `ctest -R 'KnobChannelSet|ChannelSetRender'` passes these cases:
    - The default resolves to the full colour plane.
    - `rgb` resolves to bits 0-2 and `alpha` to bit 3.
    - `rgb` and `alpha` rows together resolve to bits 0-3, once.
    - Regex `^rgb` matches rgba and rgb.
    - A row with the storage ID resolves to nothing.
    - Grade on `rgb` leaves A untouched, and Grade on `alpha` grades only A.
    - Grade on `rgba` over an RGB input processes A as zero and outputs RGBA.
  - size: L

- [ ] M65.P3.T3 — Resolve colour views in KnobLayerSelect and label Shuffle with the view
  - files: `Engine/KnobLayerSelect.cpp`, `Engine/Nodes/Channel/Shuffle.cpp` (label only, `:414-415`), `Tests/KnobLayerSelect_Test.cpp`, `Tests/Shuffle_Test.cpp`
  - approach:
    - The empty-table default (`KnobLayerSelect.cpp:144`) becomes `rgba`.
    - `resolve()` (`:253-279`) uses `resolveColorView` for view IDs; `ResolvedLayer.desc` stays the storage plane.
    - Replace `layerLabelForID` (`:282-294`) with the shared helper.
    - `Shuffle::resolveLayerLabel` returns the view ID.

    Shuffle keeps working without further changes because P2.T1 widened `isColorLayer`. Its view semantics come in P5.
  - verify: `ctest -R 'KnobLayerSelect|Shuffle_'` passes these cases:
    - The default is `rgba`.
    - `alpha` resolves to bit 3.
    - Shuffle's label and sublabel expectations read `rgba`.
  - size: M

- [ ] M65.P3.T4 — Resolve colour views in KnobChannelSelect (mask and premult channels)
  - files: `Engine/KnobChannelSelect.cpp`, `Tests/LayerKnobs_Test.cpp`
  - approach:
    - The default (`:86`) becomes `rgba.A`.
    - `resolve()` (`:139-172`) maps `rgba.X`, `rgb.X`, `alpha.A` and `xy.X` to the storage plane and a channel index. A channel missing from storage reads zero rather than failing.
    - `getSummary` shows the view ID.
    - The GUI lists only `rgba`'s channels, plus `xy`'s for XY storage. Any view is still accepted as a value.
  - verify: `ctest -R LayerKnobs` passes these cases:
    - `rgba.A` resolves on RGBA and on Alpha storage.
    - `alpha.A` is equivalent to `rgba.A`.
    - `rgb.A` is rejected.
    - `rgba.A` on RGB storage resolves and reads zero.
    - The retired `…OfxImagePlaneColour.A` resolves to nothing.
  - size: M

## Phase 65.4: Render path and OFX mapping

- [ ] M65.P4.T1 — Keep engine plane lists storage-only and prove masked rendering
  - files: `Engine/EffectInstance.cpp` (`getAvailableLayers` `:4714-4743`), `Engine/Node.cpp` (`listLayersForKnob` `:5783-5838`), `Tests/ColorViewsRender_Test.cpp` (new), `Tests/CMakeLists.txt`, `Tests/TimeVaryingLayers_Test.cpp`
  - approach:
    - `getAvailableLayers` and `listLayersForKnob` stay at storage level. Simplify the registry colour de-dup at `:4719-4741`.
    - Add `Node::listLayerViewsForKnob` (= `expandColorViews(listLayersForKnob)`) for the GUI and Python.
    - Implement the widen-on-write rule from the design doc where the output colour plane is chosen, and read missing colour channels as zero. Pass-through (`EffectInstanceRenderRoI.cpp:461-491`) and `appendSelectedPlanes` should otherwise need no change; prove that with tests.
  - verify: `ctest -R 'ColorViewsRender|TimeVaryingLayers|WriteAllLayers'` passes these cases:
    - `getPresentLayers` never contains a view ID.
    - On `flat-three-layers.exr`, Grade `rgb` gain 2 doubles RGB and keeps A.
    - Grade `alpha` changes only A.
    - Write All writes R, G, B and A once.
    - Write {rgb} writes RGB only.
    - `listLayerViewsForKnob` on `flat-rgb-only.exr` returns {rgba, rgb, alpha}.
  - size: L

- [ ] M65.P4.T2 — Map OFX component counts to views and cover RGB, Alpha and XY streams
  - files: `Tests/fixtures/make-flat-alpha-fixture.py` (new), `Tests/fixtures/flat-alpha-only.exr` (new), `Tests/ColorViewsRender_Test.cpp`, `Engine/ImageLayerDesc.cpp` (only if a gap appears)
  - approach:
    - OFX clips keep mapping through storage (`OfxEffectInstance.cpp:1372`, `:2962`; `EffectInstance.cpp:5763`). The user-facing mapping is `colorViewForNComps`.
    - Test with real openfx-misc and openfx-io plugins:
      - An RGBA-clip Grade with `rgb` selected gets an RGBA plane masked to RGB.
      - An Alpha-only ReadOIIO file presents {rgba, rgb, alpha}; its `rgb` rows resolve with RGB reading zero.
      - A 2-channel XY clip presents `xy` as well.
  - verify: `ctest -R ColorViewsRender` passes. `oiiotool --info` on the new fixture lists only `A`.
  - size: M

## Phase 65.5: Native Shuffle

- [ ] M65.P5.T1 — Shuffle resolves colour views in its slots and outputs
  - files: `Engine/Nodes/Channel/Shuffle.cpp`, `Engine/Nodes/Channel/Shuffle.h`, `Tests/Shuffle_Test.cpp`
  - approach: change these functions: `layerChannelCount` (`:269-297`), `resolveOutputLayerDesc` (`:382-403`), `getOutputLayer` (`:252-267`), `planeChannelName` (`:700-712`), `getComponentsNeededAndProduced` (`:520-562`), `isIdentity` (`:565-600`) and `checkExtraChannelsPresent` (`:875-948`). The new behaviour:
    - A view has 4, 3, 1 or 2 channels, each named by the view.
    - Every colour output produces the storage colour plane once.
    - When the two output views overlap, Out 2 counts as None; when they don't overlap, they merge.
    - The implicit source between colour views goes by channel name.
    - Reading a colour channel the input lacks reads zero, per the design doc. This amends M61 P3.T2 for colour channels only; a missing non-colour layer or channel still fails the render.
  - verify: `ctest -R 'Shuffle_|KnobShuffleMap'` passes these cases:
    - The produced lists are right for out `rgb`, `alpha`, rgba+alpha (overlap) and rgb+alpha (merge).
    - Implicit alpha←rgba reads A.
    - rgba→rgba is an identity.
    - `rgba.A` on RGB input reads zero and doesn't fail.
    - A missing `diffuse.R` still fails with M61's error text.
  - size: L

- [ ] M65.P5.T2 — Shuffle render passes untouched colour channels through and merges disjoint views
  - files: `Engine/Nodes/Channel/Shuffle.cpp` (`render` `:770-872`), `Tests/ShuffleRender_Test.cpp`
  - approach:
    - Match output planes to slots by storage, not by view ID (`:776-782`).
    - Copy colour channels outside the output views from the main input's colour plane, or zero when there is none.
    - When the rgb and alpha outputs merge, fill all four channels.
    - Fetch before locking, as today.
  - verify: `ctest -R ShuffleRender` passes these cases:
    - Out `rgb` with R←B keeps the source A.
    - Out `alpha` ← 1 keeps RGB.
    - ShuffleCopy rgb(from 2) + alpha(from 1) matches today's default ShuffleCopy output pixel for pixel.
    - The timed M61 cases stay green, except the colour-channel failure cases, which P5.T1 re-baselined.
  - size: L

- [ ] M65.P5.T3 — Shuffle matrix GUI shows view channels
  - files: `Gui/KnobGuiShuffleMap.cpp` (`:219`, `:268`, `:323`, `:497`), `Tests/KnobShuffleMap_Test.cpp`
  - approach:
    - Build rows and columns from `listLayerViewsForKnob`, and pass the registry snapshot at `:219` through `toStoragePlanes`/`expandColorViews`.
    - An `alpha` output has one row, `rgb` has three and `xy` has two.
    - When Out 2 overlaps Out 1, its column is disabled with a tooltip.
  - verify: GuiTests (offscreen) pass: the row and column counts per view are right, and the overlap column is disabled.
  - size: M

## Phase 65.6: GUI

- [ ] M65.P6.T1 — Layer rows, summaries and choice fallbacks list the views
  - files: `Gui/KnobGuiLayerChannelBase.cpp` (`:66-88`), `Gui/LayerChannelRow.cpp` (`:54`, `:534`), `Gui/NodeGui.cpp` (`:3252-3260`), `Gui/KnobGuiChoice.cpp` (`:375-376`), `Tests/LayerChannelRow_Test.cpp`
  - approach:
    - `listLayerEntriesForKnob` uses `listLayerViewsForKnob`, with the views first in the order rgba, rgb, alpha, xy.
    - Fix `kColorLayerID = "Color"` so that row 0 hoists `rgba`.
    - The node-graph summary hides the default `rgba`.
    - `ensureUnknownChoiceIsNotInternalLayerID` shows `rgba` for the storage ID.
  - verify: GuiTests `LayerChannelRow` passes these cases:
    - The 36 "Color" expectations now expect view IDs.
    - The row-0 order is None, All, Regex, rgba, rgb, alpha, ….
    - An RGB-only input still lists rgba, rgb and alpha.
  - size: M

- [ ] M65.P6.T2 — The viewer's layer and alpha menus use the views
  - files: `Gui/ViewerTab40.cpp` (`:856-1010`), `Gui/ViewerTabPrivate.cpp` (`:385`), `Engine/ViewerInstance.cpp`, `Gui/ProjectGuiSerialization.h` (`:119`)
  - approach:
    - The menu entries are `expandColorViews(present)`, labelled by view ID.
    - Choosing any view calls `setActiveLayer(storage)`.
    - `alpha` switches the display to A through the existing auto-switch (`:955-966`); rgba and rgb switch it back.
    - The alpha menu lists `rgba.A` once.
    - A saved `layerName` that is no longer in the menu falls back to `rgba`.
  - verify: covered by the P6.T3 Xvfb run; smoke debug is green.
  - size: M

- [ ] M65.P6.T3 — Xvfb screenshots of every place a user sees a layer
  - files: `build/m65-gui/gui.py`, `build/m65-gui/run.sh`
  - approach: build the graph Read(`flat-three-layers.exr`) → Grade → Shuffle → Viewer, then screenshot:
    - Grade's row-0 menu;
    - a regex `^rgb` row;
    - Grade on `flat-rgb-only.exr`, which still lists alpha;
    - the Shuffle menus and its matrix for out `alpha`;
    - the viewer with `alpha` chosen;
    - Project Settings → Layers.
  - verify: the screenshots are sent to the user, and the task stays open until the user approves them.
  - size: M

## Phase 65.7: Python, serialization and the clean break

- [ ] M65.P7.T1 — The Python API speaks views
  - files: `Engine/PyNode.h`, `Engine/PyNode.cpp` (`:94-160`, `:1064-1080`), `Engine/PyParameter.cpp` (`:2439-2450`, `:2605-2612`), `Tests/PyPlugExport_Test.cpp`
  - approach:
    - `ImageLayer.getRGBAComponents()`, `getRGBComponents()` and `getAlphaComponents()` return the view descs.
    - `Effect.getAvailableLayers` returns the expanded views.
    - The layer setters raise `ValueError` on the retired storage ID; the message names the views.
    - PyPlug export writes `setLayer("rgba")` and never emits `addProjectLayer` for a view.
  - verify: `ctest -R PyPlugExport` passes these cases:
    - A Grade on `rgb` round-trips.
    - Calling a setter with the storage ID raises.
    - `getAvailableLayers()` starts with rgba, rgb, alpha.
  - size: M

- [ ] M65.P7.T2 — Move the bundled PyPlugs off the storage ID
  - files: `Gui/Resources/PyPlugs/Glow.py` (`:39-46`), `Gui/Resources/PyPlugs/PIKColor.py` (`:383`, `:484`, `:526`, `:641`), `Gui/Resources/PyPlugs/LightWrap.py` (`:1172`)
  - approach: replace the literal with `"rgba"`. In Glow, `setLayer(colorLayer, ["A"])` becomes `setLayer("alpha")`. Make no other edits.
  - verify: `grep -rn OfxImagePlaneColour Gui/Resources` finds nothing, and smoke debug is green.
  - size: S

- [ ] M65.P7.T3 — Old projects load without crashing, with colour knobs reset and a warning
  - files: `Engine/KnobSerialization.cpp` (`:576-589`, `:640-726`), `Engine/Node.cpp` (`loadKnobs` `:1452-1481`), `Tests/fixtures/m65-legacy-color.ntp` (new), `Tests/ProjectSerialization_Test.cpp`
  - approach:
    - Delete the colour `KnobChoiceOptionFilter` entries. Keep the motion, disparity and `frameRange` filters.
    - At the end of `Node::loadKnobs`, reset any layer knob whose value names `kNatronColorLayerID`. Post one persistent warning per node: "Colour layer from an older project was reset to rgba".
    - Build the fixture by hand, following the `ocio-old-config.ntp` precedent. It needs:
      - Grade `channels` = Color;
      - a Shuffle with Color in and out;
      - a mask set to `Color.A`;
      - an OFX node with the Natron 2.2 value `outputChannels` = "RGBA".
  - verify: `ctest -R ProjectSerialization` passes these cases:
    - The project loads without crashing.
    - The reset knobs are at `rgba`/`rgba.A`.
    - The warning is present.
    - The OFX choice falls back to its default without throwing.
  - size: M

- [ ] M65.P7.T4 — Bump the image cache version
  - files: `Global/GlobalDefines.h` (`:97`)
  - approach: bump `NATRON_CACHE_VERSION` from 6 to 7. Correctness doesn't need it; it clears disk-cache entries left unreachable by the new default hashes, as M38 and M39 did.
  - verify: the build is green.
  - size: S

## Phase 65.8: Checkpoint

- [ ] M65.P8.T1 — Publish the M65 decision
  - files: `PLAN/DECISIONS/2026-09-26-rgba-rgb-alpha-xy-layers.md` (already seeded at plan time; extend it)
  - approach: extend the seeded decision with the design doc's rulings (widen-on-write, missing colour channels read zero) and the consequences of the clean break. The PM publishes the decision to `docs/decisions/` at the gate (PLAN-FORMAT §3a).
  - verify: the file carries the design-doc rulings; INDEX already links to it.
  - size: S

- [ ] M65.P8.T2 — Seal the storage plane and add a guard test against user-facing "Color"
  - files: `Engine/ImageLayerDesc.h` (`:55-56`), `Engine/ImageLayerDesc.cpp` (`:184-214`), `Engine/LayerRegistry.cpp` (`:431`), `Tests/ColorViews_Test.cpp`
  - approach:
    - Rename `kNatronColorLayerLabel` to `kNatronColorStorageLabel`, used on the storage descs only.
    - Add `ColorViews.NoUserFacingColor`. It asserts that none of these return "Color" or the storage ID: the registry snapshot, `listLayerViewsForKnob`, the three knob summaries, Shuffle's sublabel, and Python's `getAvailableLayers`.
  - verify: `ctest -R ColorViews` passes; the full debug ctest is green.
  - size: M

- [ ] M65.P8.T3 — Package the release AppImage for the user checkpoint
  - files: `build/appimages/M65-<sha>.AppImage`, `build/appimages/M65-uat.md`
  - approach: build with the release `package.sh`. The UAT script walks through:
    - Grade's menu showing rgba, rgb and alpha.
    - Grade on `rgb` keeping alpha, and Grade on `alpha`.
    - A JPEG Read still listing alpha, which reads black; the default Grade still works.
    - Shuffle out `alpha` ← 1 keeping the colour.
    - ShuffleCopy's default.
    - The viewer with `alpha` chosen.
    - Write All.
    - Regex `^rgb`.
    - An M61-era project loading with the reset warning.
    - The Glow, PIKColor and LightWrap PyPlugs.
  - verify: the AppImage launches under Xvfb via `run-launch-check.sh`, and the user signs off the UAT.
  - size: S

**Verification gate:** all of the following hold:
- `tools/ci/local/test.sh ctest debug` and `smoke debug` are green. That covers ColorViews, ColorViewsRender, LayerRegistry, KnobChannelSet, ChannelSetRender, KnobLayerSelect, LayerKnobs, Shuffle_, ShuffleRender, KnobShuffleMap, WriteAllLayers, TimeVaryingLayers, ProjectSerialization, PyPlugExport and GuiTests LayerChannelRow.
- `grep -rn OfxImagePlaneColour Gui/Resources` finds nothing.
- The user has approved the P6.T3 screenshots and signed off the P8.T3 UAT.
- The decision is published.

## Decisions

- 2026-09-26 — User-facing colour layers: `rgba` {R,G,B,A}, `rgb` {R,G,B}, `alpha` {A} and `xy` {X,Y} are real registry layers with their own IDs, and they replace "Color" everywhere a user sees or names a layer. That covers the channel-set widget (including regex labels), KnobLayerSelect/Shuffle, the viewer menu, Python and project serialization. Rationale: the user wants Nuke-style convenience layers instead of a single Color layer.
- 2026-09-26 — Shared channels: the views alias the one stored colour plane, so `rgb.R` is `rgba.R`. Rationale: a stream keeps at most one colour plane, and cached variants can't drift apart.
- 2026-09-26 — Always present: `rgba`, `rgb` and `alpha` are always listed, and a missing colour channel reads as zero. For example, an RGB JPEG's alpha is black. `xy` is listed only for XY-layout storage. Consequence: reading a missing colour channel no longer fails Shuffle, which amends M61 P3.T2 for colour channels only. Rationale: the user's call, and defaults such as Grade `rgba` never silently process nothing.
- 2026-09-26 — OFX mapping by component count: RGBA ⇄ rgba, RGB ⇄ rgb, Alpha ⇄ alpha, XY ⇄ xy. openfx-io and openfx-misc are unchanged.
- 2026-09-26 — Clean break with old projects, no alias: the legacy colour choice filters are deleted. Layer knobs in old projects that named Color reset to `rgba`/`rgba.A` with a per-node warning. Natron ≤2.2 OFX colour choices fall back to the plugin default. Python setters raise on the retired ID. Loading must never crash.
- 2026-09-26 — Sequencing: M65 is stacked on M61, and M37 builds on M65.

## Risks

- **Plugin-owned menus:** openfx-io's hidden `outputLayer` and similar plugin menus still say "Color", because the forks are unchanged.
- **Third-party scripts:** PyPlugs and scripts that hard-code the storage ID now raise. Warn about this in the release notes.
- **Widening `isColorLayer`:** this masks views leaking into engine lists. P4.T1's "no view ID in `getPresentLayers`" test is the guard, so keep it strict.
- **Shuffle wiring:** implicit wiring by name, the overlap rule and zero-reads change behaviour from M34/M61. Some tests will need re-baselining, and the change should be called out in UAT.
- **M60 (deep):** deep present lists pick up the views only if M60 goes through `expandColorViews`. Note this in the M60 draft.
