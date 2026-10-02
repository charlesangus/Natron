# Milestone 37: Channel/layer management nodes

> **Draft (2026-09-26), answers recorded 2026-09-27, revised 2026-10-02 (see `## Decisions`).** Q3 and Q6 diverge from the recommended defaults the draft was written against, and both are applied throughout. Q6 renames Remove to RemoveLayers. Q3(c), as extended on 2026-10-02, means colour views are listed and matched, and RemoveLayers narrows or drops the colour plane. The §5a freshness check against M65's code was done on 2026-10-02: P1.T3 is cancelled, P1.T1, P1.T2, P2.T1, P2.T2, P3.T1, P3.T2, P4.T2, P4.T3, P5.T1, P6.T1 and P6.T2 were rewritten, and P2.T3 is new.

Two native nodes in the Channel group:
- **RemoveLayers** takes layers out of the stream, or keeps only the chosen ones.
- **AddLayers** brings project-registry layers into the stream, zero-filled, wherever the input lacks them.

Both choose layers with M38's channel set, including its Regex rows, resolved per frame at the render's (time, view) (`DECISIONS/2026-09-24-layers-vary-with-time.md`).

The colour views `rgba`/`rgb`/`alpha`/`xy` are ordinary rows on both nodes. They alias the one storage colour plane (`kNatronColorLayerID`, layout Alpha/XY/RGB/RGBA) by channel bit (`PLAN/DESIGN/2026-09-26-rgba-rgb-alpha-layers.md`). So a colour row narrows, widens or drops that one plane rather than naming a layer of its own.

Neither node moves data between layers or between colour bits, so the no-shuffle invariant (`PLAN/DESIGN/2026-09-19-layer-channel-widget.md` §5) stands:
- **RemoveLayers forwards non-colour layers from the input.** It uses the existing pass-through path (`EffectInstanceRenderRoI.cpp` ~444-485) and renders no pixels for them. It renders the colour plane only when it narrows it. That render copies each kept colour bit from the input's colour plane to the same bit, and writes zero to any bit the narrowed layout holds but the selection removed.
- **AddLayers writes zeros only where the input lacks the data:**
  - registry layers the input doesn't carry;
  - colour bits the input's colour plane lacks. The input's own colour bits are copied unchanged into the widened plane.

The engine gains one capability virtual: a per-node filter on pass-through layers. Today pass-through is "every input layer the node doesn't produce", with no per-layer opt-out (`EffectInstance.cpp` ~4613-4623). It also gains defined behaviour for an image stream that carries no colour plane, which no node could produce before RemoveLayers (P2.T3). Image data only; deep is M60.

## Design questions (awaiting the user)

- **Q1. Is Remove one node or two?** (a) One `Remove` node with an operation choice, remove or keep; the default is remove, with nothing selected. (b) Two nodes, Remove and Keep. (c) Remove only. *Recommend (a).* **Answered (a).**
- **Q2. Remove single channels, or whole layers only?** (a) Whole layers only: no channel buttons, and the no-shuffle rule is unchanged. (b) Single channels too, on non-colour layers; this amends the no-shuffle rule and replaces P1.T2 with an L task in P2. (c) Channel removal inside the colour plane too, through the rgba/rgb/alpha views (see Q3(b)); +2 L tasks. *Recommend (a); (b) can follow later.* **Answered (a).**
- **Q3. Can the colour views (rgba/rgb/alpha/xy) be removed?** (Reworded after M65.) (a) No: they are never listed or matched, and keep always keeps the colour plane. (b) Removing `alpha` narrows the colour plane to RGB, and removing `rgb` narrows it to Alpha. Since M65 makes missing colour channels read zero, this is close to zero-filling. (c) Removing `rgba` drops the colour plane entirely; +1 M task. *Recommend (a).* **Answered (c), extended on 2026-10-02 to include (b)'s narrowing in both remove and keep (see Decisions). Applied below; the +1 M task is P2.T3.**
- **Q4. Wildcard, regex, or both?** (a) Reuse M38's Regex rows. (b) Add a Wildcard mode to `KnobChannelSet` for every node; +1 M engine task, +1 M GUI task. (c) Wildcard on Remove and AddLayers only. *Recommend (a). The risk is people typing globs such as `spec*` as a regex.* **Answered (a).**
- **Q5. Is an Add node needed, and what does it do?** (a) `AddLayers`: zero-fills the chosen registry layers only where the input lacks them, leaves present layers untouched, and can add several at once. (b) As (a), plus a fill-colour knob. (c) No node: document the Shuffle and Constant recipes instead. *Recommend (a).* **Answered (a).**
- **Q6. Names?** (a) `fr.natron.Remove` "Remove" and `fr.natron.AddLayers` "AddLayers", both in Channel. (b) RemoveLayers and AddLayers. (c) Remove and AddChannels, as in Nuke. *Recommend (a).* **Answered (b) — renamed to RemoveLayers throughout below.**
- **Defaults taken, not asked:**
  - **Per-frame resolution, silent absent rows.** Both nodes resolve their selection per frame at the render's (time, view). A row naming a layer the input doesn't have is silent: it shows "(not in input)" and doesn't fail the render, unlike Shuffle's explicit rows.
  - **RemoveLayers' colour rule (from Q3(c) as extended).**
    - Definitions:
      - S = the colour bits of input 0's storage plane (empty when the input presents none).
      - C = the colour bits the rows select: a view row gives its whole view mask, because there are no channel buttons; a regex row gives the masks of the views it matches.
      - K = the kept bits: `S & ~C` in remove mode, `S & C` in keep mode.
    - Outcomes:
      - K = S: the plane passes through untouched.
      - K empty: the plane is dropped.
      - Otherwise the output plane is `narrowestColorStorageCovering(K)`. A bit of that layout outside K reads zero; this only happens for non-view-shaped removals such as `xy` from RGBA.
    - Consequences:
      - Keep mode keeps the colour plane only if a row selects it. Keeping `spec.*` gives `{specular}` alone, and keeping None empties the stream, as in Nuke.
      - Regex `.*` matches every view: remove `.*` empties the stream, and keep `.*` is identity.
  - **AddLayers' colour rule.** Adding a colour view the input's plane doesn't cover widens the plane to `narrowestColorStorageCovering(S ∪ view mask)`. The input's bits are copied and the added bits are zero, so this is "zero-fills only where the input lacks them" applied bit by bit. Examples:
    - `rgba` on RGB gives RGBA with A = 0;
    - `alpha` on RGB gives RGBA;
    - `rgb` on Alpha gives RGBA with RGB = 0;
    - `alpha` on a colourless or unconnected input gives an Alpha plane of zeros;
    - a view the input already covers is a no-op.
  - **Colour outcomes don't vary per frame.** The colour rows resolve against the input's colour layout, which comes from time-invariant metadata. So both nodes set their output colour layout in `getPreferredMetadata`, while non-colour layers still vary per frame.
  - **A colourless stream reads zero.** It still lists `rgba`/`rgb`/`alpha` (M65's present rule), and they read zero there. Write All writes no R/G/B/A for it. Details are in P2.T3.

Execution notes:
- **Stacking:** M37 stacks on M66. Branch `milestone/m37-channel-management-nodes` off `milestone/m66-plugin-alpha-only-moderate` and open the PR against it (`DECISIONS/2026-09-22-stacked-milestone-prs.md`). This is part of the 2026-10-02 parcel run (M66 → M37 → M50 → M60). The user approves P4.T3's shots and runs P6.T2's UAT together with the parcel, so the PR stays open until then.
- **M65 vocabulary is in force:**
  - Engine lists and assertions (`getPresentLayers`, `getAvailableLayers`, `listLayersForKnob`) stay at storage level, i.e. one `kNatronColorLayerID` entry whose `getNumComponents()` is the layout.
  - User-facing lists use the views, via `Node::listLayerViewsForKnob` / `ImageLayerDesc::expandColorViews`, e.g. `{rgba, rgb, alpha, diffuse, specular}`.
  - `appendInputStreamLayers` (`Node.cpp` ~6245-6268) adds an RGBA storage entry to input-bound lists when the stream has none. So resolve a node's own selection against the input's real `getPresentLayers`, not `listLayersForKnob`.
  - Colour helpers: `ImageLayerDesc::colorStorageBits`, `narrowestColorStorageCovering`, `colorViewMask`, `resolveColorView`, `getColorView`.
  - Colour copy precedent: `Shuffle.cpp` (`getInputColorStorage` ~462, `colorBitOfIndex` ~79, `findColorChannelInPlane` ~834, `render` ~911).
  - Shuffle's `getColorWriteBits` precedent applies only to nodes that widen through `checkMetadata`. Both M37 nodes set their layout in `getPreferredMetadata` instead and keep the default `getColorWriteBits`, which reads only the host layer knob (they have none, being multiplanar).
- **`NativeEffectBase::addAcceptedComponents` (`NativeEffectBase.cpp` ~156-162) accepts only RGB, RGBA and Alpha.** Both nodes override it to add XY so that an XY stream isn't clamped.
- **Fixtures** (all in `Tests/fixtures/`):
  - `flat-three-layers.exr`: RGBA (1,0,0,1), diffuse (0,1,0), specular (0,0,1).
  - `flat-rgb-only.exr` (1,0,0); `flat-alpha-only.exr` (A = 1); `flat-rgba-only.exr`.
  - `flat-seq-layers.####.exr`: frame 1 has RGBA + diffuse + specular, frame 2 has RGBA only.
  - `flat-no-color-layers.exr` is no colourless source: ReadOIIO duplicates its first layer into colour.
- **Container and builds.** The `natron-dev` container is single-tenant. Implementers edit in parallel and don't build. Each batch gets one detached build plus ctest (setsid+nohup, a fresh `.done` marker). Check `pgrep -x ninja` is 0 before relaunching, and never pgrep-wait on a build.
- **Tests.** Run them through `build/m61ctest.sh <regex>` (it sets `OFX_PLUGIN_PATH`) or `tools/ci/local/test.sh`. The debug build defines NDEBUG, so tests use EXPECT/ASSERT, never assert().
- **GUI checks** run under Xvfb with the recipe in `build/deeprepro/run-gui.sh` (see also `build/m61-gui/run-gui.sh`). Scripts and fixtures go under `build/m37-gui/`. Pre-seed `checkForUpdates=false`.
- **UAT packaging.** Package the release build for UAT. Launch-check it with the devshell `LD_LIBRARY_PATH` stripped (`build/appimages/run-launch-check.sh`).
- Batches:
  - B1: P1.T1, P1.T2
  - B2: P2.T1, P4.T1
  - B3: P2.T2, P4.T2
  - B4: P2.T3, P3.T1
  - B5: P3.T2, P5.T1
  - B6: P4.T3, P6.T1, P6.T2
- **Shared-file constraints:**
  - P2.T1, P2.T2, P3.T1 and P3.T2 each add a line to `Tests/CMakeLists.txt`, and P2.T1 and P3.T1 each add one to `Engine/AppManager.cpp`. No two of them share a batch.
  - P2.T3 adds its tests to P2.T2's `Tests/RemoveLayersRender_Test.cpp`, so it needs no CMake line.

## Phase 37.1: Engine foundations

- [x] M37.P1.T1 — Add a per-node filter on pass-through layers
  - files: `Engine/EffectInstance.h` (~1110-1129, next to `producesMetadataLayerImplicitly`/`isPassThroughForNonRenderedLayers`), `Engine/EffectInstance.cpp` (`getComponentsNeededDefault` ~4438-4517, its pass-through fill ~4453-4456; multiplanar branch of `getComponentsNeededAndProduced_public` ~4613-4623)
  - approach:
    - Add the virtual `filterPassThroughLayers(double time, ViewIdx view, std::list<ImageLayerDesc>* layers)`, a no-op by default. Document it next to `isPassThroughForNonRenderedLayers`: it may drop entries, including the colour storage entry, but never add or reshape them.
    - Call it with the pass-through (time, view) at two points:
      - in the multiplanar branch, after `removeFromLayersList(outputLayers, &upstreamAvailableLayers)`;
      - at the end of `getComponentsNeededDefault`'s pass-through fill.
    - Don't call it in the disabled branch (~4542-4568).
    - The filtered list is what `ActionsCache::setComponentsNeededResults` stores, so `getPresentLayers` and render forwarding see it with no further change.
    - `removeFromLayersList` matches with `findEquivalentLayer`, so a node that produces a narrowed colour plane already removes the input's colour plane from pass-through. The filter only matters when the colour plane is dropped, or for a non-colour layer.
  - verify: full debug ctest green. There's no behaviour change yet: P2.T1 is the first node to override it and carries the behaviour tests.
  - size: M

- [x] M37.P1.T2 — Let a channel set hide its channel buttons (Q2)
  - files: `Engine/KnobChannelSet.h` (class at ~131; `setChannels`/`setExcludedChannels`/`resolve` declarations ~220-255), `Engine/KnobChannelSet.cpp` (`resolve` ~568-650), `Engine/PyParameter.cpp` (`ChannelSetParam`), `Tests/KnobChannelSet_Test.cpp`
  - approach:
    - Add `setWithChannelButtons(bool)` / `getWithChannelButtons()` as a non-persistent member, default true (precedent: `KnobLayerSelect.h` ~115-122).
    - With the buttons off:
      - `setChannels`/`setExcludedChannels` throw `std::invalid_argument`, which Python's `ChannelSetParam` surfaces as `ValueError`. This includes `setChannels`' row-0 shim that writes `rgba`.
      - `resolve()` ignores the stored channels. A non-colour layer row yields every channel. A colour-view row calls `resolveColorView` with an empty channel list, so `channels` is the view mask ∩ storage and `zeroChannels` is the rest of the mask. A regex row ignores its exclusions.
    - The codec is unchanged.
  - verify: `ctest -R KnobChannelSet`:
    - with buttons off, a `diffuse` row storing `{R}` resolves to all three bits;
    - an `rgba` row storing `{R,G,B}` over RGBA storage resolves to channels `{0,1,2,3}`, and over RGB storage to channels `{0,1,2}` with zeroChannels `{3}`;
    - `setChannels` throws;
    - a regex row with an exclusion resolves to every channel;
    - the default (buttons on) is unchanged.
  - size: M

- [~] M37.P1.T3 — ~~Let a declared layer knob leave Color out of its listing (Q3)~~ (cancelled 2026-10-02)
  - files: none
  - approach: cancelled by Q3(c). RemoveLayers and AddLayers list and match the colour views like every other channel set does, so no `listsColor` opt-out is needed. The +1 M task Q3(c) calls for is P2.T3.
  - verify: n/a
  - size: S

## Phase 37.2: RemoveLayers

- [x] M37.P2.T1 — RemoveLayers node: keep/remove over a channel set, narrowing or dropping the colour plane (Q1–Q4, Q6)
  - files: `Engine/Nodes/Channel/RemoveLayers.h`, `Engine/Nodes/Channel/RemoveLayers.cpp` (new), `Engine/AppManager.cpp` (~1565, after ShuffleCopy's registration), `Engine/CMakeLists.txt` / sources list if Shuffle is listed there, `Tests/RemoveLayers_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach:
    - **Plugin.** `NativeEffectBase`, `fr.natron.RemoveLayers`, label "RemoveLayers", `PLUGIN_GROUP_CHANNEL`, one optional input "Source". Multiplanar; `producesMetadataLayerImplicitly` returns false (precedent: `Shuffle.h` ~96-104). All bit depths. `addAcceptedComponents` adds XY to the base list.
    - **Knobs:**
      - `operation`: `KnobChoice` {remove, keep}, default remove, not animated, metadata slave.
      - `channels`: `KnobChannelSet`, buttons off (P1.T2), default None, metadata slave, not animated, declared input-bound on input 0 via `declareLayerKnob`. It lists the colour views through the normal `listLayerViewsForKnob` path.
      - A secret sublabel built from `getSummary(present)`, e.g. "remove diffuse, alpha" or "keep spec.*" (precedent: `Shuffle.cpp` ~228-235).
    - **One helper, `computeColorOutcome(time, view)`, implements the header's colour rule** and is used everywhere below:
      - S comes from input 0's present colour storage (as `Shuffle::getInputColorStorage` does).
      - C comes from `channels->resolve(present)`, taking the colour entry's `channels | zeroChannels`, with present = input 0's `getPresentLayers`.
      - The outcome is one of: unchanged, dropped, or narrowed to `narrowestColorStorageCovering(K)`.
    - **`getComponentsNeededAndProduced`:**
      - When narrowed, produced = {narrowed colour desc} and input 0 needs {input colour storage}.
      - Otherwise produced and inputs are empty.
      - Pass-through is input 0 at (time, view).
    - **`filterPassThroughLayers`:**
      - Remove mode drops the non-colour layers the rows resolve to. Keep mode drops the non-colour layers they don't resolve to.
      - When the outcome is dropped, the colour storage entry is dropped too. When narrowed, the produced plane already displaced it.
    - **`getPreferredMetadata`:** when narrowed, `setNComps(-1, narrowed.getNumComponents())`. Otherwise it leaves the default, so a dropped plane keeps the input's layout in metadata: metadata can't express "no colour plane", and P2.T3 owns that case.
    - **`isIdentity`:** onto input 0 when the colour outcome is unchanged and no non-colour layer would be dropped at (time, view).
    - **`render`:** only the narrowed colour plane is ever requested. Write each output channel from the input colour image's channel on the same bit, or 0 when that bit isn't in K. Model it on Shuffle's bit-mapped copy, re-implemented locally since Shuffle's helpers are file-static.
    - Register the node next to Shuffle.
  - verify: `ctest -R RemoveLayers_` on Read(`flat-three-layers.exr`) → RemoveLayers, with storage-level assertions on `getPresentLayers(1,0,-1)` (colour entry checked by `isColorLayer()` + `getNumComponents()`):
    - the node is registered in Channel, and a new node is identity;
    - remove `diffuse` → {Color(4), specular};
    - remove `alpha` → {Color(3), diffuse, specular};
    - remove `rgb` → {Color(1), diffuse, specular};
    - remove `rgba`, and separately rows `rgb`+`alpha` → {diffuse, specular} with no colour entry;
    - remove regex `.*` → {} (empty);
    - keep `spec.*` → {specular};
    - keep `alpha` + `diffuse` → {Color(1), diffuse};
    - keep regex `.*` → identity;
    - keep None → {};
    - on Read(`flat-rgb-only.exr`), remove `alpha` → identity (K = S);
    - the output metadata's `getNComps(-1)` is 3 for remove `alpha` and 1 for remove `rgb`;
    - a downstream Blur's `listLayerViewsForKnob` follows knob changes and still lists `rgba, rgb, alpha` after remove `rgba` (M65's present rule);
    - a row naming an absent `depth` posts no error;
    - `removeLayer("diffuse")` is refused while RemoveLayers names it.
  - size: L

- [x] M37.P2.T2 — RemoveLayers renders, narrows the colour plane and varies per frame
  - files: `Tests/RemoveLayersRender_Test.cpp` (new; model it on `ShuffleRender_Test.cpp`: fixture writer helpers at the top, `createTimeVaryingReadSequence` ~243, `createTimeVaryingSwitch` ~255), `Tests/CMakeLists.txt`
  - approach: Read(`flat-three-layers.exr`) → RemoveLayers → Write (All, single-part 32f).
    - **Non-colour removal.** Removing `diffuse` writes exactly `R,G,B,A,specular.*` with the fixture values.
    - **Narrowing.**
      - Removing `alpha` writes exactly `R,G,B,diffuse.*,specular.*` with R,G,B = (1,0,0).
      - Removing `rgb` writes `A,diffuse.*,specular.*` with A = 1.
      - Keeping `alpha` + `spec.*` writes `A,specular.*`.
    - **Downstream of a narrowed plane.**
      - RemoveLayers(`alpha`) → Grade (All) renders cleanly, and its output colour layout stays RGB (M65's narrowest-covering rule).
      - RemoveLayers(`alpha`) → Grade(`rgba`) widens back to RGBA with A graded from 0.
    - **Per frame.** Reuse the sequence and Switch builders with remove regex `diff.*`:
      - frame 1 (RGBA + diffuse + specular) writes `R,G,B,A,specular.*`;
      - frame 2 (RGBA only) writes `R,G,B,A`.
      - Render each frame with the timeline parked on the other frame.
  - verify: `ctest -R RemoveLayersRender` green; full debug ctest green.
  - size: M

- [x] M37.P2.T3 — Carry a stream that has no colour plane: reads zero downstream, Write All omits it (Q3(c))
  - files: `Engine/EffectInstance.cpp` (`getComponentsNeededAndProduced_public`'s implicit metadata layer ~4594-4612; `getImage` ~724), `Engine/EffectInstanceRenderRoI.cpp` (~444-490), `Engine/ViewerInstance.cpp` if the viewer path needs it, `Tests/RemoveLayersRender_Test.cpp`
  - approach: RemoveLayers(remove `rgba`) on `flat-three-layers.exr` is the first source whose present layers have no colour storage while its metadata still says colour. Make that stream behave as M65's "missing colour channels read zero":
    - **(1) No forced colour plane.** `producesMetadataLayerImplicitly` must not add the metadata colour layer when the node's pass-through input presents no colour storage. This covers Write's embedded encoder, so Write All writes no R/G/B/A.
    - **(2) Zero-filled colour reads.** A downstream fetch of the colour plane from such an input gets a zero-filled image of the requested layout, not a null image or a failed render. A Blur or Grade whose rows name a colour view over the colourless stream then renders a zero colour plane. Do this in `getImage`, where the input's render returned no plane. Keep it out of `renderRoI`, so the cache never holds a fabricated plane.
    - **(3) Viewer.** On `rgba` it shows black with no error, and `diffuse` shows green. Its layer menu lists `rgba, rgb, alpha, diffuse, specular`.
    - **Scope.** Touch nothing else on the render path. If (1) changes any existing ctest expectation, stop and report the conflict rather than adapting the test.
  - verify: `ctest -R 'RemoveLayersRender|WriteAllLayers|ShuffleRender|ColorViews'` green, with new cases in `RemoveLayersRender_Test.cpp`:
    - RemoveLayers(`rgba`) → Write All writes exactly `diffuse.*,specular.*`;
    - → Blur(All) → Write All writes the same set with no persistent error;
    - → Grade(`rgba`) → Write All writes R,G,B,A = 0 plus the two layers;
    - a viewer render on `rgba` returns OK and black.
    - Then full debug ctest green.
  - size: M

## Phase 37.3: AddLayers

- [x] M37.P3.T1 — AddLayers node: zero-fill registry layers and colour bits the input lacks (Q5, Q6)
  - files: `Engine/Nodes/Channel/AddLayers.h`, `Engine/Nodes/Channel/AddLayers.cpp` (new), `Engine/AppManager.cpp` (~1566), `Tests/AddLayers_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach:
    - **Plugin.** `fr.natron.AddLayers`, label "AddLayers", Channel group, optional input "Source". Multiplanar; `producesMetadataLayerImplicitly` returns false; `addAcceptedComponents` adds XY.
    - **Knob `layers`:** `KnobChannelSet`, buttons off, default None, metadata slave, declared as a target. Target knobs list the registry, with colour folded to RGBA by `LayerRegistry::toStoragePlanes` and shown as views.
    - **Resolution.** Resolve the rows against the registry storage planes, which hold RGBA for colour, so a view row yields its whole mask A.
    - **Colour (the header's AddLayers rule):**
      - S = input 0's present colour storage bits, empty when the input has none or is unconnected.
      - If A ⊄ S, produce `narrowestColorStorageCovering(S | A)` and have input 0 need its colour storage. Otherwise colour passes through.
    - **Non-colour layers.** Produce the resolved registry layers input 0 lacks at (time, view). Pass-through is input 0.
    - **`getPreferredMetadata`:** when colour widens, `setNComps(-1, widened)`.
    - **`isIdentity`:** when nothing would be produced.
    - **`render`:**
      - `Image::fillZero(roi)` (`Engine/Image.h` ~700) on each non-colour plane.
      - The widened colour plane takes the input bits that are in S and zero for bits in A \ S (bit-mapped like P2.T1's render), or all zero with no input colour.
    - **Region of definition:** the Source's, or the project format when unconnected.
    - **Sublabel:** from `getSummary`, e.g. "diffuse, mask" or "alpha".
  - verify: `ctest -R AddLayers_` with `mask [A]` registered:
    - on Read(`flat-three-layers.exr`) with rows `mask` + `diffuse`: produced is `{mask}`, present is `{Color(4), diffuse, specular, mask}`;
    - on Read(`flat-rgb-only.exr`): row `rgba` gives produced and present colour of 4 comps and `getNComps(-1)` 4; row `rgb` is identity;
    - on Read(`flat-alpha-only.exr`), row `rgb` gives colour of 4 comps;
    - unconnected with `mask` presents `{mask}`; unconnected with `alpha` presents `{Color(1)}`;
    - `removeLayer("mask")` is refused while the node names it;
    - a new node is identity.
  - size: L

- [ ] M37.P3.T2 — AddLayers renders zeros, never overwrites, and varies per frame
  - files: `Tests/AddLayersRender_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach:
    - **Fixture graph.** Read(`flat-three-layers.exr`) → AddLayers(`mask`, `diffuse`) → Write(All):
      - `mask.A` is 0;
      - `diffuse` stays `(0,1,0)`, untouched;
      - RGBA stays (1,0,0,1).
    - **Widening.** Read(`flat-rgb-only.exr`) → AddLayers(`alpha`) → Write(All) writes R,G,B = (1,0,0) and A = 0.
    - **Colourless input.** RemoveLayers(`rgba`) → AddLayers(`rgb`) writes R,G,B = 0 plus the two layers (relies on P2.T3).
    - **Per frame.** On the sequence with row `diffuse`:
      - frame 1's diffuse is `(0,1,0)`;
      - frame 2's diffuse is zero.
      - Render each frame with the timeline parked on the other frame.
    - **Downstream.** A Grade can select `mask`.
  - verify: `ctest -R AddLayersRender` green; full debug ctest green.
  - size: M

## Phase 37.4: GUI

- [x] M37.P4.T1 — Channel-set rows without channel buttons (Q2)
  - files: `Gui/LayerChannelRow.h`, `Gui/LayerChannelRow.cpp` (button visibility ~812, `matches:` label ~68), `Gui/KnobGuiChannelSet.cpp`, `Tests/LayerChannelRow_Test.cpp`
  - approach:
    - `LayerChannelRow` set rows take `withChannelButtons`.
    - When it is false there is no button strip on layer rows, colour-view rows included, and no button line under regex rows. The `matches:` label stays.
    - `KnobGuiChannelSet` passes `knob->getWithChannelButtons()`.
  - verify: GuiTests (offscreen):
    - with buttons off, `diffuse`, `alpha` and matching regex rows have no buttons;
    - with buttons on, nothing changes.
  - size: M

- [x] M37.P4.T2 — "New layer…" on channel-set rows of a target knob (Q5)
  - files: `Gui/LayerChannelRow.cpp` (the "New layer..." entry is gated on `_mode == eModeLayerSelect` at ~656-661), `Gui/KnobGuiChannelSet.h`, `Gui/KnobGuiChannelSet.cpp` (~254, `setAvailableLayers(layers, false)`), `Tests/LayerChannelRow_Test.cpp`
  - approach:
    - Let set rows append "New layer..." when `listNewLayerEntry` is set, and pass `isTargetKnob()` at ~254.
    - `runNewLayerDialog` is already declared in `Gui/KnobGuiLayerSelect.h` (~49); reuse it.
    - `KnobGuiChannelSet` handles `newLayerRequested` the way `KnobGuiLayerSelect::onNewLayerRequested` does (~169-191): deferred via `QTimer::singleShot(0, …)`, in one undo step that registers the layer and sets the row to it.
  - verify: GuiTests:
    - a target set row lists "New layer..." last;
    - an input-bound set row doesn't;
    - the dialog flow is covered by P4.T3.
  - size: M

- [ ] M37.P4.T3 — Xvfb screenshots of both panels, the viewer menu and the node graph
  - files: `build/m37-gui/gui.py`, `build/m37-gui/run.sh`
  - approach: build Read(`flat-three-layers.exr`) → RemoveLayers → AddLayers → Viewer and screenshot:
    - **RemoveLayers panel:** operation, rows `diffuse` and `alpha`, a regex row with `matches:`, no channel buttons, and the colour views offered in the row menu.
    - **Viewer layer menu** before and after removing `diffuse`. The viewer on `alpha` after removing `alpha` shows black.
    - **AddLayers:** "New layer…" → `mask [A]`, its panel, and Project Settings → Layers showing `mask` used by 1.
    - **Node graph** with both sublabels.
  - verify: send the shots to the user. The task stays open until the user approves them; under the parcel run this happens at the parcel UAT.
  - size: M

## Phase 37.5: Round trip and scripting

- [ ] M37.P5.T1 — Save/load and PyPlug export for RemoveLayers and AddLayers
  - files: `Tests/RemoveLayers_Test.cpp`, `Tests/AddLayers_Test.cpp`, `Tests/PyPlugExport_Test.cpp`
  - approach:
    - **Save/load.** Save, reset and load a project containing RemoveLayers(keep, `spec.*` + `alpha`) and AddLayers(`mask`, `rgba`). Rows, operation and present layers survive, including the Alpha-only colour layout downstream of RemoveLayers.
    - **PyPlug export.** Export a group containing AddLayers on `mask [A]`:
      - the script has one `addProjectLayer("mask", ["A"])` before the node is created, and none for colour views;
      - reimporting it registers `mask`.
    - **Python.** `setChannels` on either knob raises `ValueError`.
  - verify: `ctest -R 'RemoveLayers_|AddLayers_|PyPlugExport'` green.
  - size: M

## Phase 37.6: Checkpoint

- [ ] M37.P6.T1 — Publish the M37 decision
  - files: `docs/decisions/<date>-remove-and-add-layers.md` (new), its `PLAN/DECISIONS/` mirror, `PLAN/DECISIONS/INDEX.md`
  - approach: record, in the style of `2026-09-26-rgba-rgb-alpha-xy-layers.md`:
    - the Q1–Q6 answers;
    - Q3(c) as extended: the remove/keep colour rule, keep-None empties, regex `.*` matches colour;
    - AddLayers' colour-widening default;
    - colourless-stream semantics (P2.T3);
    - the pass-through filter hook.
  - verify: the file exists and INDEX links to it.
  - size: S

- [ ] M37.P6.T2 — Packaged release AppImage and user checkpoint
  - files: `build/appimages/M37-<sha>.AppImage`, `build/appimages/M37-uat.md`
  - approach: build with the release `package.sh`. The UAT script walks through:
    - RemoveLayers `diffuse`: the viewer menu and a Write-All EXR no longer have it;
    - RemoveLayers `alpha`: the viewer's `alpha` is black, and a Write-All EXR has R,G,B but no A;
    - RemoveLayers `rgb`: the stream is alpha-only;
    - RemoveLayers `rgba`: Write All has no R/G/B/A, the viewer on `rgba` is black, and `diffuse` still shows;
    - keep `spec.*` leaves only specular (colour gone), then adding a keep row `rgba` brings colour back;
    - regex matches updating when the Read's file changes;
    - AddLayers "New layer…" `mask`, then a Grade selecting `mask`;
    - AddLayers `alpha` on an RGB JPEG gives an RGBA stream with A = 0 (viewer alpha black, written PNG transparent);
    - AddLayers on a `diffuse` that's already present leaves it untouched;
    - scrubbing the sequence 1↔2;
    - undo;
    - removing a layer from the Layers page is refused while a node names it.
  - verify: the AppImage launches under Xvfb via `run-launch-check.sh`. The user runs the script, with the parcel, and signs off.
  - size: S

**Verification gate:**
- `tools/ci/local/test.sh ctest debug` and `smoke debug` are green. That includes KnobChannelSet, RemoveLayers, RemoveLayersRender (with P2.T3's colourless cases), AddLayers, AddLayersRender, PyPlugExport, WriteAllLayers, ColorViewsRender, TimeVaryingLayers and ShuffleRender, plus GuiTests LayerChannelRow.
- The user has approved the P4.T3 screenshots.
- The user has signed off the P6.T2 UAT.
- The decision is published.

## Decisions

- 2026-09-27 — **Q1–Q6 answered** (user, via `/cat-discuss`): Q1 (a) one `Remove`-shaped node with a remove/keep operation choice; Q2 (a) whole layers only, no channel buttons; Q4 (a) reuse M38's Regex rows; Q5 (a) `AddLayers` zero-fills only, no fill-colour knob. Two answers diverge from the draft's recommendation: **Q3 (c)** — removing `rgba` drops the colour plane entirely (not "never listed or matched"), which the draft's task briefs (P1.T3, P2.T1, P3.T1) and P6.T2's UAT script were not written for; that rework is still needed before implementation starts. **Q6 (b)** — the nodes are named `RemoveLayers` and `AddLayers`; the rename is applied throughout this file already.
- 2026-10-02 — Q3(c) extended (user): in RemoveLayers, colour views narrow the colour plane. Removing `alpha` leaves RGB storage; removing `rgb` leaves Alpha-only; removing `rgba` (or both `rgb` and `alpha`) drops the colour plane. Keep mode mirrors it (keeping only `alpha` narrows to Alpha-only, etc.).
- 2026-10-02 — **§5a freshness check against M65's code; Q3(c) applied** (planning consultant). Colour views are listed and matched on both nodes.
  - **RemoveLayers** computes kept colour bits K (remove: S & ~C; keep: S & C). It passes the plane through, drops it, or produces `narrowestColorStorageCovering(K)` by a bit-for-bit copy. It sets the layout in `getPreferredMetadata`, because `checkMetadata`'s write-bit rule only widens.
  - **Consequences:** keep `spec.*` gives `{specular}` and keep None empties the stream.
  - **AddLayers** (default taken) widens the colour plane to the narrowest layout covering input ∪ view bits, zero-filling only the added bits.
  - **P1.T3 cancelled** (no `listsColor` opt-out needed).
  - **New P2.T3** (the +1 M): colourless streams read zero downstream, and Write All omits R/G/B/A.
  - **Rewritten** to M65 vocabulary and current line numbers: P1.T1, P1.T2, P2.T1, P2.T2 (sequence frames had been inverted), P3.T1, P3.T2, P4.T2 (`runNewLayerDialog` already declared), P4.T3, P5.T1, P6.T1, P6.T2.
  - **Stacking:** M37 now stacks on M66 (`milestone/m66-plugin-alpha-only-moderate`), and the batches were re-cut into B1–B6 so no two tasks in a batch edit `Tests/CMakeLists.txt` or `AppManager.cpp`.
- 2026-10-02 — **B1+B2 landed** (`5d4dfd0f4` P1.T1, `9c31a8dcc` P1.T2, `bf5a43d9e` P2.T1, `e61e3aa0e` P4.T1), with the full debug ctest at 689/689. Where the P1.T2 brief and existing code disagree, the existing convention wins: over RGB storage an `rgba` row resolves to channels 1111 with zeroChannels 1000, as `DefaultOverRGBStorageReportsAlphaAsReadingZero` expects. RemoveLayers accepts every bit depth, converting with `convertPixelDepth`. Its sublabel shows a regex row's matched layers, so it reads "keep specular" rather than "keep spec.*".
- 2026-10-02 — **B3+B4 landed** (`3c372022c` P2.T2, `7863f98f5` P4.T2, then the P2.T3 and P3.T1 commits), full debug ctest 713/713.
  - **P2.T3 went beyond its brief in two places (accepted).** (a) Over a colourless input, `getComponentsNeededDefault` drops colour from a non-target knob's selection when no row names a colour view. So All/regex rows never fabricate a colour plane, which matches the design rule that All never widens. An explicit `rgba` row still produces zeros. (b) The viewer's layer menu (`ViewerTabPrivate::getComponentsAvailabel`) adds RGBA when a connected input shows no colour, so the views stay listed.
  - **Known gaps, not fixed:**
    - A non-multiplanar node without a layer knob (a Dot) still reports colour over a colourless stream.
    - A Merge with All drops colour when only its pass-through B input is colourless.
    - A default (identity) Grade(`rgba`) over a colourless stream passes through and writes no RGBA.
    - AddLayers' render hash doesn't include the project registry, so registering or removing a layer may leave stale cached results until a knob changes.
