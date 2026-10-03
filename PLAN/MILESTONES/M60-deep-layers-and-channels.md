# Milestone 60: Deep images get layers/channels like flat images

> **Elaborated 2026-10-02** (planning consultant) from the 2026-09-19 stub. The user answered the three design questions on 2026-10-02: Q1 and Q2 as defaulted, Q3 against the default (deep alpha is structural). See `## Design questions`.

A deep stream already carries named channels (`R`, `G`, `B`, `A`, `Z`, `ZBack` and any AOV such as `diffuse.R`), but the layer model never sees them. Every deep node reports one metadata colour plane as its layers, the viewer always flattens RGBA, `DeepFromImage` only ever makes RGBA, and `DeepToImage` only ever flattens RGBA. This milestone gives a deep stream real layers:
- Its channels are grouped into storage-level `ImageLayerDesc`s: one colour storage plane whose layout covers the colour channels present, plus one layer per AOV.
- Those layers are reported through the same `getPresentLayers` path that flat images use. That way M38's layer/channel knobs, M65's colour views (`rgba`/`rgb`/`alpha`), the viewer's layer menu, the project layer registry and Python all work on deep streams without a parallel UI path.
- On top of that, `DeepFromImage`, `DeepToImage`, `DeepRecolor` and `DeepExpression` choose layers with those knobs, the viewer flattens the chosen deep layer, and two new deep nodes, DeepRemoveLayers and DeepAddLayers, bring M37's layer management to deep streams.

## Scout notes (2026-10-02, on `milestone/m66-plugin-alpha-only-moderate`, which contains M65)

**The data model already holds named channels.**
- `DeepImage` (`Engine/DeepImage.h:171-308`) keeps `std::map<std::string, DeepChannelBuffer> _channels` (`:306`) over one copy-on-write `SampleTable`. It has `hasChannel`/`getChannel`/`getChannelForWriting`/`setChannel`/`removeChannel`/`aliasContentsOf` (`:238-277`). `getChannelForWriting` allocates a zero-filled buffer for a channel name that is new (`Engine/DeepImage.cpp:83-95`).
- Names are flat strings. There is no layer grouping, and `Z`/`ZBack` are ordinary map entries.

**The stub's citations check out, but its claim that "extra channels are dropped through any deep processing node" is wrong.**
- **Already preserved.** Every processing node already carries AOVs through:
  - `DeepMerge` builds the union of A's and B's channel names (`Engine/Nodes/Deep/DeepMerge.cpp:465-472`); a channel one side lacks reads zero there (`:132-150`). Holdout takes A's channels (`:515-536`).
  - `DeepCrop` (`DeepCrop.cpp:232-241`) and `DeepReformat` (`DeepReformat.cpp:291-300`) copy every channel.
  - `DeepRecolor` and `DeepExpression` go through `NativeEffectBase::renderDeepFromInput` (`Engine/Nodes/NativeEffectBase.cpp:309-446`). It aliases the input's whole channel map and detaches only the written channels, creating any that are missing.
  - `DeepRead` reads every file channel by name (`DeepRead.cpp:214-242`, Z and ZBack/Zback split out at `:221-224`). `DeepWrite` writes every channel by name (`DeepWrite.cpp:141-155`).
- **Hardcoded to RGBA.** These are the real holes:
  - `DeepFromImage` renders its Source as RGBA only (`DeepFromImage.cpp:107-110`), takes Z from channel 0 of the Z input's metadata layer (`:111-113`), refuses anything but 4 components (`:166-168`), and writes the four RGBA channels (`:180-196`).
  - `DeepToImage` accepts RGBA only (`DeepToImage.cpp:69-74`) and flattens the four RGBA channels (`:129-142`).
  - `DeepRecolor` renders the Color input as RGBA (`DeepRecolor.cpp:150-151`), requires 4 components (`:204-206`), and writes RGB, or RGBA with Target Input Alpha (`:210-211`).
  - `DeepExpression` has fixed `{R,G,B,A,Z,ZBack}` expression knobs (`DeepExpression.cpp:42-43, 84-95`). It already *reads* any input channel by name, dotted names included (`DeepExpressionEvaluator.h:46, 64-66`; input names built at `DeepExpression.cpp:174-180`).
  - The viewer flattens deep input to RGBA whatever layer is selected (`Engine/ViewerInstance.cpp:1450-1457`, flatten call `:1545-1566`), and so does `renderDeepRoIFlattened` (`Engine/EffectInstanceRenderDeep.cpp:543-544, 657`).
  - `DeepFlatten::flattenToImage` fails if any named channel is missing (`Engine/DeepFlatten.h:48-68`), so with today's API M65's "missing colour channel reads zero" cannot hold for deep.

**How layer listing works today, and why deep needs its own source.**
- `Node::listLayersForKnob` (`Engine/Node.cpp:6291-6346`) → `appendInputStreamLayers` (`:6265-6288`) → `EffectInstance::getPresentLayers(time, view, inputNb)` (`Engine/EffectInstance.cpp:4799-4851`). That calls the *input's* `getComponentsNeededAndProduced_public` (`:4647-4770`), cached in `ActionsCache` keyed on (hash, time, view) (`:4665-4672`), and merges output (`comps[-1]`) with pass-through layers. `mergeLayersList`/`removeFromLayersList` (`:151-195`) match layers through `findEquivalentLayer`, which treats every colour layout as the same layer.
- `listLayerViewsForKnob` (`Node.cpp:6349-6378`) runs `ImageLayerDesc::expandColorViews` on that list. The viewer menu uses `getPresentLayers(-1)` on its active input (`Gui/ViewerTabPrivate.cpp:380-408`), and Python `Effect.getAvailableLayers` uses `getAvailableLayers` (`Engine/PyNode.cpp:1074-1084`).
- Deep nodes are not multiplanar, so they fall into `getComponentsNeededDefault` (`EffectInstance.cpp:4538-4644`; since M37 it also runs `filterPassThroughLayers` and the colourless-stream suppression). Today they report only the **metadata colour plane** plus their input's pass-through, never the channels in the deep data.
- Hooking in at `getComponentsNeededAndProduced_public` (one branch for deep-producing effects, `producesDeepData()` at `EffectInstanceRenderDeep.cpp:164`) makes every consumer above, including the actions cache, work unchanged.
- **The deep layers must be computable without rendering.**
  - `DeepRead` can read the OIIO header, as its `getPreferredMetadata` already does (`DeepRead.cpp:147-172`, which uses `getCurrentTime()`, not the query time).
  - The processing nodes derive their layers from their inputs and knobs.
  - The test stub `DeepSyntheticSource` (`Tests/DeepRenderTestEffect.h:329-440`) holds its `DeepImage`, so it can report its layers directly.
- **Layer knobs today.** `EffectInstance::getLayerKnobSpec` (`EffectInstance.cpp:4901-4908`) gives no host layer knob to non-image outputs, so deep-output nodes have none. `DeepToImage` (image output, not multiplanar) appears to get the host channel set, but its render ignores it. P2.T3 makes it multiplanar with its own knob.
- **Declaring knobs.** `Node::declareLayerKnob` (`Node.cpp:6492-6500`) is used from `initializeKnobs`, with Shuffle (`Engine/Nodes/Channel/Shuffle.cpp:239-242`) and M37's RemoveLayers/AddLayers (`RemoveLayers.cpp:136`, `AddLayers.cpp:121`) as precedents.
- **Registering produced layers.** `Node::registerProducedLayers` (`Node.cpp:6518-6570`) registers every non-colour layer in `comps[-1]`. M38 left it "not gated on output data kind (M60 will call it from DeepRead)". So deep layers reported as *produced* by DeepRead reach the registry for free, and a downstream deep node should report its input's layers as *pass-through*, not produced.

**Grouping helper.** `LayerRegistry::groupChannelNames` (`Engine/LayerRegistry.cpp:362-445`) groups flat names into dotted layers. It is unused by the engine (only `Tests/LayerRegistry_Test.cpp:271` calls it). It isn't right for deep as-is:
- it treats `Z` as a `depth` layer;
- it folds bare `I`/`Y` into colour;
- it emits a colour desc with whatever subset of channels it found, e.g. `{R, A}`, instead of one of the four storage layouts.

The deep grouping wraps it with those three fixes.

**Fixtures** (`Tests/fixtures/`, generated by `Tests/fixtures/make-deep-fixtures.py`):
- `deep-scanline.exr` and `deep-tiled.exr` hold `R,G,B,A,Z,ZBack,AOV` (`make-deep-fixtures.py:29`; the bare `AOV` is a one-channel layer);
- `deep-noncanonical.exr` holds `Z,ZBack,A,AOV` (no RGB: Alpha storage + `AOV`);
- `deep-nozback.exr` has no ZBack;
- `deep-interleaved.exr` holds `R,G,B,A,Z,ZBack`.
- **No fixture has dotted AOVs** (`diffuse.R/G/B`), and none varies its layers over a sequence.
- `build/make_big_deep.py` is reference only: a 640×360 RGBAZZBack two-disc file written into `build/deeprepro/`.
- Flat fixtures that M37 relies on are reused here: `flat-three-layers.exr`, `flat-rgb-only.exr`, `flat-alpha-only.exr` and `flat-seq-layers.000{1,2}.exr`.

**Existing deep tests** (all in `Tests/CMakeLists.txt:34-42`):
- `DeepImage_Test` (COW model);
- `DeepPixelOps_Test`;
- `DeepFlatten_Test` (flatten + probe);
- `DeepImageCache_Test`;
- `DeepRenderPipeline_Test` (cache, bounds growth, abort, flatten cache);
- `DeepReadWrite_Test` (exact read/write, including the non-canonical channel order);
- `DeepNodes_Test` (2344 lines: Merge, FromImage, Recolor, Crop, Reformat, Expression);
- `DeepPipeline_Test` (Read→Merge→Recolor→ToImage→Write);
- `DeepExpressionEvaluator_Test`.
- None of them asserts anything about layers.

**Registration and build.** Deep nodes are registered at `Engine/AppManager.cpp:1556-1564`, followed by Shuffle, ShuffleCopy and M37's RemoveLayers/AddLayers (`:1565-1568`). `Engine/CMakeLists.txt:36-44` globs `*.cpp` and `Nodes/**/*.cpp`, so new engine files need no CMake edit. New test files need a line in `Tests/CMakeLists.txt`.

## Design questions (answered by the user, 2026-10-02)

- **Q1. RemoveLayers/AddLayers on deep streams.** **Answered (a), the default:** they are in M60, as two new native nodes, `DeepRemoveLayers` and `DeepAddLayers`, in `Engine/Nodes/Deep/`. Their knobs and remove/keep semantics follow M37's nodes, except that alpha is structural (Q3). The rejected options were:
  - (b) making M37's nodes polymorphic. They would become the first polymorphic nodes that render both kinds, and every M37 graph's typed-edge resolution would change.
  - (c) a follow-up milestone.
- **Q2. DeepExpression's knob shape.** **Answered (a), the default:**
  - one `layer` layer select, input-bound on Source, default `rgba`;
  - four expression fields, relabelled with that layer's channel names;
  - fixed `Z`/`ZBack` fields.
- **Q3. Removing alpha from a deep stream.** **Answered (b), against the default:** deep `A` is structural and is never removed. The rule, applied everywhere below:
  - **Invariant.** Every deep stream with samples carries an `A` channel. Every deep source creates one:
    - DeepRead refuses a file without `A`, with a persistent error, as it does for a file with no channels;
    - DeepFromImage always writes `A`;
    - DeepAddLayers never needs to add it.
  - **No other node can drop it:**
    - DeepRecolor and DeepExpression may rewrite `A` but never remove it;
    - DeepMerge, DeepCrop and DeepReformat carry it through;
    - DeepRemoveLayers never drops it.
    - `renderDeepRoI` enforces this. A deep render whose output has samples but no `A` fails with a persistent error naming the node (P2.T6), so a future node can't break it silently.
  - **Colour storage is always Alpha or RGBA.** It is `narrowestColorStorageCovering(R/G/B bits present ∪ {A})`. A deep stream therefore never presents RGB storage. The views still list `rgba, rgb, alpha` (M65's present rule), and R/G/B missing from an Alpha storage read zero.
  - **Colour rows on deep can only take away R, G and B.** Let S = the stream's colour bits and C = the bits the rows select. Then:
    - remove mode keeps `(S \ C) ∪ {A}`;
    - keep mode keeps `(S ∩ C) ∪ {A}`.
    - Consequences:
      - removing `rgba` and removing `rgb` are the same: both drop R, G and B and leave Alpha storage;
      - removing `alpha` changes nothing in the colour plane (identity, if no other row applies);
      - keep None or keep `spec.*` still carries `A`;
      - regex `.*` in remove mode leaves `A` alone.
    - The DeepRemoveLayers tooltip and sublabel say "A is always kept".
  - **Flat outputs may still leave A out.** DeepToImage's channel set selects which *flat* planes it outputs. Leaving `A` out of its rows omits A from the image, but the flatten always composites with the deep `A`. DeepToImage outputs an image, so it can't produce a deep stream at all.

**Defaults taken, not asked:**
- **Z and ZBack are not layers.**
  - They are each sample's depths. They are never listed, grouped, selected or removed.
  - DeepExpression keeps dedicated Z/ZBack fields.
  - DeepToImage emits no depth layer (a front-depth `depth.Z` output is a follow-up).
  - A deep channel literally named `depth.Z` is an ordinary AOV.
- **The deep colour storage plane (Q3).** It is `narrowestColorStorageCovering(R/G/B bits present ∪ {A})`, so it is always Alpha or RGBA. For example:
  - `{R,G,B,A}` gives RGBA;
  - `{R,A}` gives RGBA, with G and B reading zero;
  - `{A}` gives Alpha.
  - A deep stream always has a colour plane.
- **Other layers.**
  - Bare non-colour names (`AOV`, `Y`, `I`) are one-channel layers named after the channel.
  - Dotted names group on the last dot, as `groupChannelNames` does.
  - The channel name of layer L, channel c, is `c` for the colour plane and for a bare layer whose only channel equals L; otherwise it is `L.c`. One helper owns this mapping in both directions.
- **Per frame.** Deep layers are resolved at the query's (time, view) (`DECISIONS/2026-09-24-layers-vary-with-time.md`). DeepRead reads the header of the file *at that time*.
- **DeepFromImage.**
  - It gets a `channels` channel set, input-bound on Source, default **All**.
  - `A` is always written (Q3), taken from the Source colour plane fetched as RGBA, which is today's conversion: an RGB source gives A = 1. Rows can't leave it out. An AOV-only selection still carries `A`, and a selection without `alpha` still writes it.
  - Values are copied verbatim (treated as premultiplied), as colour is today.
  - Z comes from a `zChannel` channel select, input-bound on the Z input, default `rgba.R` (today's "first channel"). An absent channel reads zero.
- **DeepToImage.**
  - It gets a `channels` channel set, input-bound on the deep input, default **All**.
  - Each selected layer is flattened front-to-back, always with the deep `A` as coverage, whether or not the rows select `alpha` (Q3). A missing AOV or colour channel reads zero.
  - Output colour layout: All/regex rows give the deep colour storage. View rows give `narrowestColorStorageCovering(union of the rows' view bits)`, so `rgb` gives RGB, `alpha` gives Alpha, and `rgba` over Alpha storage gives RGBA with RGB = 0. This is M37 RemoveLayers' "keep" narrowing, and it is what Q3's "leaving `A` out of its rows omits A" requires. (Corrected 2026-10-03: the earlier "storage bits ∪ view mask" would have kept A on an `rgb` row.)
  - With no colour row selected (e.g. rows `diffuse` alone), the flat output has no colour plane (M37 P2.T3 semantics).
- **DeepRecolor.**
  - It gets a `channels` channel set, input-bound on the Color input, default **`rgb`**.
  - Each selected Color-image channel is written to the deep channel of the same name, scaled by sample alpha over colour alpha as today. A non-colour layer the deep input lacks is created.
  - A row's `A` bit is ignored, because alpha stays under **Target Input Alpha**. The knob tooltip says so.
  - A row naming a layer absent from the Color image is skipped silently, leaving the deep channel untouched. A colour-view channel missing from Color's storage reads zero (M65).
- **DeepWrite gets no channel knob.** It keeps writing every channel; put DeepRemoveLayers upstream instead.
- **DeepRemoveLayers / DeepAddLayers mirror M37's rules,** with Q3's structural alpha. On deep there is no storage layout to narrow; colour bits are simply which of `R/G/B` exist, beside the ever-present `A`.
  - Remove/keep computes the kept channel names with Q3's colour rule, and drops the rest from the channel map. `A`, `Z` and `ZBack` are always kept.
  - AddLayers adds zero-filled buffers for target-registry channels the input lacks: a colour view adds only its missing R/G/B. An `alpha` row is a no-op, because A is always present. Present channels are never touched.
  - Both use M37's buttonless channel set (M37 P1.T2) and Regex rows, and resolve per frame.
- **The viewer on a deep input** lists the deep layers (as views) and flattens the selected layer. Alpha and matte display read the deep `A`. The flattened image is cached per (node hash, layer).
- **Produced vs pass-through.**
  - A deep node reports, as produced (`comps[-1]`), only the layers its own input doesn't carry. Its input's layers are reported as pass-through.
  - So `registerProducedLayers` registers each AOV once, at its origin: DeepRead, a DeepFromImage of an unregistered layer, or DeepAddLayers.

## Phase 60.1: Deep layer model

- [x] M60.P1.T1 — Group deep channel names into storage-level layers
  - files: `Engine/DeepLayers.h`, `Engine/DeepLayers.cpp` (new), `Tests/DeepLayers_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: Add a namespace `DeepLayers` with three functions.
    - `groupDeepChannels(const std::vector<std::string>& names, std::list<ImageLayerDesc>* layers)`:
      - strip `Z`, `ZBack` and `Zback`;
      - collect the R/G/B bits and emit `narrowestColorStorageCovering(bits ∪ {A})` first, *always* (Q3: Alpha or RGBA, even if the name list lacks `A`; P2.T6 makes such a stream an error at render);
      - group the rest by reusing `LayerRegistry::groupChannelNames`, after removing the colour names and remapping its bare `I`/`Y`/`Z` special cases to plain one-channel layers;
      - keep the input order otherwise.
    - `channelName(const ImageLayerDesc& layer, int index)` and its inverse `findChannel(name, layers, &layer, &index)`. The colour storage gives bare `R/G/B/A` by bit (`ResolvedLayer::channelBit` rule), a bare one-channel layer gives its own name, and anything else gives `L.c`.
    - `colorBits(const DeepImage&)` / `colorBits(names)`, where bit 3 (A) is always set.
    - Plain functions, with no EffectInstance dependency (as `DeepFlatten` is).
  - verify: `build/m61ctest.sh DeepLayers` covers:
    - `{R,G,B,A,Z,ZBack,diffuse.R,diffuse.G,diffuse.B}` → `{Color(4), diffuse(R,G,B)}`;
    - `{Z,ZBack,A,AOV}` → `{Color(1), AOV(AOV)}`;
    - `{R,A}` → `Color(4)`;
    - `{Z}` → `{Color(1)}`;
    - `{R,G,B,Z}` → `{Color(4)}` (never RGB storage);
    - `{Y, depth.Z}` → `{Y(Y), depth(Z)}`;
    - `channelName`/`findChannel` round-trip for each;
    - `expandColorViews` on the result lists `rgba, rgb, alpha` first.
  - size: M

- [x] M60.P1.T2 — Report a deep stream's layers through `getComponentsNeededAndProduced_public`
  - files: `Engine/EffectInstance.h`, `Engine/EffectInstance.cpp`, `Tests/DeepRenderTestEffect.h`, `Tests/DeepLayers_Test.cpp`
  - approach:
    - **New virtual** `EffectInstance::getDeepLayers(double time, ViewIdx view, std::list<ImageLayerDesc>* layers)`, storage level, Z/ZBack never included. The default is the layers-pass-through input's `getPresentLayers(-1)` at its pass-through time and view (`getLayersPassThroughInput`), or empty when there is none. Dot, Switch, TypedPassthrough and DeepCrop/DeepReformat need nothing more.
    - **New branch** in `getComponentsNeededAndProduced_public` (`EffectInstance.cpp:4647-4770`), after the disabled-node branch (`:4676-4699`) and before the `isMultiPlanar()` split (`:4701`): `if (producesDeepData())`, fill the result from `getDeepLayers` and return. Deep nodes then no longer reach `getComponentsNeededDefault`, so M37's colourless-stream suppression and identity-route handling there never apply to them; deep streams always carry colour (Q3).
      - Let D = `getDeepLayers(time, view)` and P = the pass-through input's present layers (`getLayersPassThroughInput`, then `getAvailableLayers(ptTime, ptView, ptInput)`).
      - `passThroughLayers` = the entries of P that appear in D with the **identical** desc (colour compared by layout, so `Color(4)` ≠ `Color(1)`). Then call `filterPassThroughLayers(ptTime, ptView, &passThroughLayers)`, keeping M37's contract (it may only drop entries).
      - `comps[-1]` = the entries of D not kept as pass-through.
      - **Do not use `removeFromLayersList`/plain `mergeLayersList` for this split.** They match through `findEquivalentLayer`, which treats every colour layout as one layer (`:151-195`). A node that narrows colour (DeepRemoveLayers `rgba`: Color(4)→Color(1)) or widens it (DeepRecolor writing R on an A-only stream) would otherwise present its input's layout. A layer the node dropped would also still be presented as pass-through. `getPresentLayers` (`:4799-4851`) then does `mergeLayersList(comps[-1], &passThrough)`, which is correct once the split is exact.
      - input entries empty, `processChannels` all set, `processChannelsPerPlane` empty;
      - cached through the same `ActionsCache` call, honouring `cacheResults`.
    - **Nothing else changes.** `getPresentLayers`, `listLayersForKnob`, `listLayerViewsForKnob`, the viewer menu, Python and `registerProducedLayers` are untouched and now see deep layers.
    - **Test stub.** `DeepSyntheticSource` (`Tests/DeepRenderTestEffect.h:329`) overrides `getDeepLayers` with `DeepLayers::groupDeepChannels` over its image's channel names. Add a small test deep effect (same header) whose `getDeepLayers` returns a list the test sets, to exercise the split.
  - verify: `build/m61ctest.sh 'DeepLayers|DeepNodes|DeepRenderPipeline|DataKind'` green, with new cases:
    - a synthetic source with `R,G,B,A,diffuse.R,G,B` presents `{Color(4), diffuse}` at storage level;
    - a Dot and a TypedPassthrough downstream present the same;
    - a disabled deep node presents its input's;
    - a downstream test deep effect reporting `{Color(1)}` over that source presents exactly `{Color(1)}`: no `diffuse`, and not the input's Color(4). Its `comps[-1]` is `{Color(1)}`;
    - one reporting `{Color(4), diffuse}` over an A-only source presents `Color(4)` and has `diffuse` and `Color(4)` in `comps[-1]`;
    - `Node::listLayerViewsForKnob` on a knob declared input-bound on a test node fed by the source lists `rgba, rgb, alpha, diffuse`;
    - an A-only source presents `{Color(1)}` and lists `rgba, rgb, alpha`;
    - `registerProducedLayers` on the source registers `diffuse`.
    - Then full debug ctest green.
  - size: L

- [x] M60.P1.T3 — DeepRead reports its file's layers per frame; add dotted-AOV fixtures
  - files: `Engine/Nodes/Deep/DeepRead.h`, `Engine/Nodes/Deep/DeepRead.cpp`, `Tests/fixtures/make-deep-fixtures.py` (+ the new `.exr` outputs), `Tests/DeepReadWrite_Test.cpp`
  - approach:
    - **`DeepRead::getDeepLayers`:**
      - opens `getFilenameAtTime(time)` with `OIIO::ImageInput::open`;
      - takes `spec().channelnames` and runs `DeepLayers::groupDeepChannels`;
      - keeps a one-entry (filename → layers) memo under a mutex so scrubbing doesn't reopen the header on every uncached query;
      - returns empty when there's no file.
    - **Structural alpha (Q3).** `renderDeep` posts a persistent error "<file> has no alpha (A) channel; deep data needs one" and fails when the file has no `A`, next to the existing no-channels check (`DeepRead.cpp:231-236`).
    - **Fixtures.** Extend the generator so that it writes, leaving existing outputs byte-identical:
      - `deep-layers.exr`: the PIXELS structure with `R,G,B,A,Z,ZBack,diffuse.R,diffuse.G,diffuse.B,specular.R,specular.G,specular.B`, where diffuse = (0,1,0)·A and specular = (0,0,1)·A per sample;
      - `deep-seq-layers.0001.exr` (as `deep-layers.exr`) and `deep-seq-layers.0002.exr` (RGBAZZBack only).
  - verify: `build/m61ctest.sh DeepReadWrite`, with new cases:
    - `deep-layers.exr` presents `{Color(4), diffuse, specular}`;
    - `deep-noncanonical.exr` presents `{Color(1), AOV}`;
    - `deep-scanline.exr` presents `{Color(4), AOV}`;
    - on the sequence, frame 1 presents diffuse and frame 2 doesn't, each queried with the timeline parked on the other frame;
    - after `refreshAllInputRelatedData`, the project registry contains `diffuse` and `specular`;
    - a temp file written by the test with `R,G,B,Z` and no `A` fails with that error;
    - DeepRead→DeepWrite of `deep-layers.exr` writes all 12 channels back exactly.
  - size: M

- [x] M60.P1.T4 — DeepMerge reports the union of its inputs' layers; Crop and Reformat pass theirs through
  - files: `Engine/Nodes/Deep/DeepMerge.h`, `Engine/Nodes/Deep/DeepMerge.cpp`, `Tests/DeepNodes_Test.cpp`
  - approach: `DeepMerge::getDeepLayers`:
    - in combine mode, the merge (`mergeLayersList`) of A's and B's present layers at (time, view), with the colour storage being `narrowestColorStorageCovering` of the union of both colour bit sets. This matches the render's channel union (`DeepMerge.cpp:465-472`).
    - in holdout mode, A's layers.
    - DeepCrop and DeepReformat rely on the default.
  - verify: `build/m61ctest.sh DeepNodes` green, with new cases on synthetic sources:
    - combine of `{R,G,B,A,diffuse.*}` and `{R,G,B,A,specular.*}` presents `{Color(4), diffuse, specular}`, and the rendered image has exactly those channels;
    - combine of an A-only and an RGBA source presents `Color(4)`, and combine of two A-only sources presents `Color(1)`;
    - holdout presents A's layers;
    - DeepCrop and DeepReformat present their input's.
  - size: M

## Phase 60.2: Deep nodes choose layers

- [x] M60.P2.T1 — DeepFromImage converts the layers its channel set selects
  - files: `Engine/Nodes/Deep/DeepFromImage.h`, `Engine/Nodes/Deep/DeepFromImage.cpp`, `Tests/DeepLayers_Test.cpp`
  - approach:
    - **Knobs.**
      - `channels`: `KnobChannelSet`, default All, not animated, declared input-bound on input 0 in `initializeKnobs` (Shuffle precedent).
      - `zChannel`: `KnobChannelSelect`, input-bound on input 1, default `rgba.R`.
    - **Resolution.** At (time, view), resolve against Source's `getPresentLayers` (not `listLayersForKnob`, which adds a fake RGBA).
    - **`renderInputImage`** (`DeepFromImage.cpp:96-146`).
      - Fetch the resolved planes, each as the Source's present storage desc, since `renderRoI` only matches planes with equal channel counts.
      - Fetch the RGBA colour plane (for A) only when the Source presents colour storage. Since M37 a stream can be colourless (RemoveLayers `rgba`), and `renderRoI` builds no zero plane for one; only `getImage` does (M37's zero-fill). A colourless Source therefore gives A = 0 everywhere, hence no samples and no error.
      - Fetch the Z input's chosen plane and read the selected channel; a missing one gives 0.
    - **`renderDeep`.**
      - Sample existence is still `A > 0`.
      - Always write `A` (Q3), plus every other resolved channel under `DeepLayers::channelName`. Colour bits the Source storage lacks are not written, and an `alpha` bit in the rows changes nothing.
      - Drop the 4-component check.
    - **`getDeepLayers`** = `groupDeepChannels` of exactly those names (colour bits ∪ {A}).
    - **Description.** Update the plugin description text.
  - verify: `build/m61ctest.sh 'DeepLayers|DeepNodes'`:
    - Read(`flat-three-layers.exr`)→DeepFromImage (All) presents `{Color(4), diffuse, specular}`, and its deep render has `R,G,B,A,diffuse.R/G/B,specular.R/G/B` with the fixture values on every covered pixel;
    - rows `diffuse` alone give `{Color(1), diffuse}` with channels `A, diffuse.*`;
    - rows `rgb` alone give `Color(4)` with channels `R,G,B,A`, so A is still written;
    - Read(`flat-rgb-only.exr`) gives `A = 1` samples;
    - `zChannel` = `diffuse.G` places samples at depth 1;
    - on `flat-seq-layers` frame 2 (no diffuse) with a `diffuse` row, there's no diffuse channel and no error;
    - Read(`flat-three-layers.exr`)→RemoveLayers(`rgba`) (a colourless stream, M37)→DeepFromImage renders an empty deep image with no error;
    - the existing DeepFromImage tests are unchanged and green.
  - size: L

- [x] M60.P2.T2 — Flatten several layers in one pass, reading missing channels as zero
  - files: `Engine/DeepFlatten.h`, `Engine/DeepFlatten.cpp`, `Tests/DeepFlatten_Test.cpp`
  - approach:
    - **New `DeepFlatten::flattenLayersToImages`.** It takes `src`, `roi`, an `alphaChannelName` (`"A"`) and a list of `{std::vector<std::string> channelNames, ImagePtr dst}` targets.
    - **One pass.** It tidies each pixel once (into the existing scratch and workspace) over the union of the named channels plus alpha. Then it composites front-to-back with that alpha and writes each target's components.
    - **Missing channels.** A name absent from `src`, `A` included, reads 0. This is no longer a failure.
    - **Old API.** `flattenToImage` becomes a wrapper over it, so DeepToImage and the viewer keep compiling until P2.T3 and P3.T1.
  - verify: `build/m61ctest.sh 'DeepFlatten|DeepRenderPipeline|DeepPipeline'`, with new cases:
    - two targets (RGBA and `diffuse.R/G/B`) match two single-layer reference flattens bit for bit;
    - a target naming an absent channel gets zeros and returns OK;
    - a `src` without `A` flattens to the plain sum of samples;
    - `FlattenToImageMatchesSerialReference` stays green.
  - size: M

- [x] M60.P2.T3 — DeepToImage flattens the layers its channel set selects
  - files: `Engine/Nodes/Deep/DeepToImage.h`, `Engine/Nodes/Deep/DeepToImage.cpp`, `Tests/DeepPipeline_Test.cpp`
  - approach:
    - **Multiplanar, the RemoveLayers way.** Copy M37's RemoveLayers overrides (`RemoveLayers.h:110-118`):
      - `isMultiPlanar()` true, so `getLayerKnobSpec` (`EffectInstance.cpp:4901-4908`) gives no host knob;
      - `producesMetadataLayerImplicitly()` false. Otherwise the multiplanar branch of `getComponentsNeededAndProduced_public` (`:4721-4746`) force-merges the metadata colour plane, and rows `diffuse` alone would still produce colour.
      - Also override `isPassThroughForNonRenderedLayers()` to `ePassThroughBlockNonRenderedLayers`. Its input is deep, so its layers must not be reported as flat pass-through (`:4749-4761`).
      - Accept RGB/RGBA/Alpha/XY (`addAcceptedComponents`, `DeepToImage.cpp:70-74`).
    - **Knob.** `channels`: `KnobChannelSet`, default All, declared input-bound on input 0 (the deep input; it lists through P1.T2's path).
    - **`getComponentsNeededAndProduced`.** Produced = the resolved storage layers at (time, view), with the colour layout from the Defaults rule (All/regex: the deep storage; view rows: narrowest covering their bits). No colour row means no colour plane. Use `ChannelCopy::findColorStorage` (`Engine/Nodes/Channel/ChannelCopy.h`) on the present list.
    - **`getPreferredMetadata`.** Sets `nComps(-1)` to the colour layout, resolved against a fixed RGBA storage, never against the input's layers at the current frame. This is M37 review round 1's rule (see `RemoveLayers::getPreferredMetadata`). If no colour row is selected, leave `nComps` alone: M37 tells colourless streams apart by present layers only, and `getImage`'s zero-fill covers downstream reads. Metadata can then disagree with an Alpha-storage frame under All. That is M37's accepted "Write follows metadata" gap.
    - **`render`** (`DeepToImage.cpp:103-148`). One `renderDeepRoI`, then one `flattenLayersToImages` over `args.outputLayers`, mapping each output plane's channels through `DeepLayers::channelName`. Coverage is always the deep `A`, whatever the rows say (Q3).
  - verify: `build/m61ctest.sh 'DeepPipeline|DeepNodes|WriteAllLayers'`:
    - DeepRead(`deep-layers.exr`)→DeepToImage→Write (All, 32f EXR) writes exactly `R,G,B,A,diffuse.*,specular.*`, with diffuse = flatten of the diffuse samples;
    - rows `diffuse` alone write `diffuse.*` only, with values equal to the All render's `diffuse.*` (still alpha-composited). `getPresentLayers(-1)` on DeepToImage is exactly `{diffuse}`: no colour plane and no `specular` passed through;
    - rows `rgb` write `R,G,B` only, equal to the All render's;
    - `deep-noncanonical.exr` with All writes `A,AOV`;
    - with row `rgba` it writes `R,G,B,A` with RGB = 0;
    - on the sequence, frame 2 writes no diffuse;
    - the existing DeepFromImage→DeepToImage round-trip test is green.
  - size: L

- [x] M60.P2.T4 — DeepRecolor recolours the channels its channel set selects
  - files: `Engine/Nodes/Deep/DeepRecolor.h`, `Engine/Nodes/Deep/DeepRecolor.cpp`, `Tests/DeepLayers_Test.cpp`
  - approach:
    - **Knob.** `channels`: `KnobChannelSet`, default `rgb`, input-bound on input 1 (Color). Its tooltip says alpha follows Target Input Alpha.
    - **`renderColorImage`.** Fetch the resolved planes plus RGBA, for the colour alpha.
    - **Channels to write.** Every resolved channel (colour `A` bit dropped) under `DeepLayers::channelName`, plus `A` when Target Input Alpha is on.
    - **The rewrite** scales each by `sampleAlpha / colorAlpha` as today. A colour-view channel missing from Color's storage writes 0, and absent rows are skipped.
    - **Checks.** Drop the 4-component check.
    - **`getDeepLayers`** = A's layers merged with the groups of the written names.
  - verify: `build/m61ctest.sh 'DeepLayers|DeepNodes'`:
    - synthetic deep `{R,G,B,A}` + Color = Read(`flat-three-layers.exr`) with rows `rgb` + `diffuse` writes R,G,B = (1,0,0)·α and creates `diffuse.*` = (0,1,0)·α;
    - it presents `{Color(4), diffuse}`;
    - unwritten channels still share storage with A;
    - a `specular` row with the Color input lacking specular leaves deep specular untouched;
    - the existing DeepRecolor tests are green unchanged.
  - size: M

- [x] M60.P2.T5 — DeepExpression rewrites the channels of a chosen layer
  - files: `Engine/Nodes/Deep/DeepExpression.h`, `Engine/Nodes/Deep/DeepExpression.cpp`, `Tests/DeepNodes_Test.cpp`
  - approach: Q2(a).
    - **Knobs.**
      - `layer`: `KnobLayerSelect`, input-bound on Source, default `rgba`, no channel buttons.
      - `expression0..3`: `KnobString`.
      - `expressionZ` and `expressionZBack` (renamed from today's six).
    - **Labels.** On a layer change and on `layerListRefreshed`, relabel `expression<i>` with the resolved view's channel names (`rgba` gives R/G/B/A, `diffuse` gives R/G/B), and hide the slots beyond its channel count.
    - **`getExpressions(time)`.** Map slot i to `DeepLayers::channelName` of the layer resolved at (time, view). A colour view writes its channels by bit, so `alpha`'s slot 0 is `A`.
    - **Unresolved layer.** If the layer is absent at that frame, post M61's "Layer X is not in the Source input" persistent error. Exception: colour views always resolve, and writing a missing colour channel creates it, as today.
    - **`getDeepLayers`** = input layers merged with the written names.
    - **Tests.** Update the description and the test helper `expressionsRGBAZZBack` to the new knob names. Re-base `CreatesAChannelTheInputLacks` on an A-only synthetic source that gains `R` through layer `rgba`, so it no longer builds an A-less deep stream (Q3; P2.T6 makes those an error).
    - **Structural alpha (Q3).** Rewriting `A` is allowed; nothing in DeepExpression removes it.
  - verify: `build/m61ctest.sh 'DeepNodes|DeepExpression'`:
    - all existing DeepExpression tests are green against the renamed knobs, including the re-based `CreatesAChannelTheInputLacks`;
    - new cases:
      - layer `diffuse` with `expression1 = "diffuse.G * 2"` doubles diffuse.G and shares every other channel;
      - layer `alpha` slot 0 rewrites A;
      - a `diffuse` layer on a frame where the input lacks it posts the error, and the next valid frame clears it;
      - Z and ZBack fields behave as before.
  - size: L

- [x] M60.P2.T6 — Enforce structural deep alpha in the render pipeline (Q3)
  - files: `Engine/EffectInstanceRenderDeep.cpp`, `Tests/DeepNodes_Test.cpp`, `Tests/DeepRenderPipeline_Test.cpp`
  - approach:
    - **Check.** In `renderDeepRoI`, right after a successful `renderDeep` and before the result is cached, fail the render if `getSampleTable().getTotalSampleCount() > 0` and `!hasChannel("A")`. Post the persistent error "<node> produced deep data without an alpha (A) channel", and insert nothing into the deep cache.
    - **Exempt.** Empty images (no samples) are exempt.
    - **Old checks.** DeepMerge's and DeepRecolor's "needs an alpha channel" checks stay as defensive code.
    - **Test rewrite.** `DeepRecolorRendersNothingWithoutAAndFailsWithAMessageWithoutAlphaOnA` (`DeepNodes_Test.cpp:1530`) uses an A-less synthetic source. It now expects the source's own render to fail with the new error, and DeepRecolor to fail downstream.
  - verify: `build/m61ctest.sh 'DeepNodes|DeepRenderPipeline|DeepReadWrite|DeepPipeline'` green, with new cases:
    - a synthetic `{R,G,B}` source with samples fails with the error and leaves no deep cache entry;
    - a synthetic empty source without `A` renders OK;
    - every DeepRemoveLayers/DeepAddLayers/DeepFromImage case from earlier batches still renders.
    - Then full debug ctest green.
  - size: M

## Phase 60.3: Viewer

- [x] M60.P3.T1 — The viewer flattens the selected deep layer
  - files: `Engine/EffectInstance.h`, `Engine/EffectInstanceRenderDeep.cpp`, `Engine/ViewerInstance.cpp`, `Tests/DeepRenderPipeline_Test.cpp`
  - approach:
    - **`renderDeepRoIFlattened`** (`EffectInstance.h:776`) gains `const std::list<ImageLayerDesc>& layers`, default `{RGBA}`.
      - It looks up, and stores, one cached image per layer. `ImageParams` components already distinguish them under the same node-hash key, so the existing hash purge stays exact.
      - It flattens missing ones in one `flattenLayersToImages` pass.
    - **Viewer.** M50 rewrote much of `ViewerInstance.cpp`, but the deep path is intact. In the `inArgs.deepUpstream` branch that forces RGBA and `alphaChannelIndex = 3` (`ViewerInstance.cpp:1450-1457`), and in the `renderDeepRoIFlattened` call that sets `alphaImage = colorImage` for matte (`:1545-1566`), replace the forced RGBA:
      - request `params->layer` (a view maps to its storage through `isColorLayer`, as M65 §8 does);
      - for the A or matte display, request `params->alphaLayer` and pick `alphaChannelName`'s index. Absent means black, as for flat.
      - Nothing in `Gui/` changes, because the menu already comes from `getPresentLayers` (P1.T2).
  - verify: `build/m61ctest.sh 'DeepRenderPipeline|DeepFlatten'`, with new cases on a synthetic `{R,G,B,A,diffuse.*}` source:
    - a viewer render with layer `diffuse` returns the diffuse flatten;
    - re-rendering `diffuse` hits the cache;
    - `rgba` after `diffuse` leaves both entries cached;
    - a hash change purges both;
    - the A display on an A-only source shows its alpha.
    - `SecondFlattenOfTheSameFrameIsACacheHit` and `FlattenCacheInvalidatesWhenTheDeepNodesHashChanges` stay green.
    - The Xvfb check is in P5.T1.
  - size: L

## Phase 60.4: DeepRemoveLayers and DeepAddLayers (Q1(a))

- [x] M60.P4.T1 — Add a deep render helper that drops and zero-adds channels without touching the rest
  - files: `Engine/Nodes/NativeEffectBase.h`, `Engine/Nodes/NativeEffectBase.cpp`, `Tests/DeepRenderPipeline_Test.cpp`
  - approach: New `NativeEffectBase::renderDeepReshapingChannels(args, input, const std::vector<std::string>& drop, const std::vector<std::string>& addZero)`.
    - It aliases the input (`aliasContentsOf`), or copies it over the output bounds the way `renderDeepFromInput` does (`NativeEffectBase.cpp:345-381`). Factor that copy into a shared private step rather than duplicating it.
    - Then it calls `removeChannel` for each name in `drop` and `getChannelForWriting` (zero-filled) for each name in `addZero`.
    - The sample table and every untouched channel stay shared.
  - verify: `build/m61ctest.sh 'DeepRenderPipeline|DeepNodes'`, with new cases using a local test effect that calls the helper:
    - on the aliasing path, a dropped channel is gone, an added channel is zero with the right length, and the table and other channels share storage with the input;
    - on the copy path (wider cached input), the values equal the aliasing render;
    - the existing renderDeepFromInput tests stay green.
  - size: M

- [x] M60.P4.T2 — DeepRemoveLayers node: remove or keep layers of a deep stream
  - files: `Engine/Nodes/Deep/DeepRemoveLayers.h`, `Engine/Nodes/Deep/DeepRemoveLayers.cpp` (new), `Engine/AppManager.cpp`, `Tests/DeepChannelNodes_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach:
    - **Render.** Built on P4.T1's helper.
    - **Plugin.** `fr.natron.DeepRemoveLayers`, label "DeepRemoveLayers", Deep group, deep in and out.
    - **Knobs.**
      - `operation` {remove, keep}, default remove;
      - `channels`: `KnobChannelSet` with `setWithChannelButtons(false)`, default a single None row (`encodeRows`), not animated, declared `eRoleInputBound` on input 0. Mirror `RemoveLayers::initializeKnobs` (`Engine/Nodes/Channel/RemoveLayers.cpp:92-140`), including its tooltip text plus "A is always kept".
      - a secret `kNatronOfxParamStringSublabelName` sublabel, built like `RemoveLayers::buildSubLabel` (`:413-440`): the verb on its own line, then `getShortSummary(present, KnobChannelSet::kSubLabelSummaryLength)`. Refresh it from `knobChanged`, `onKnobsLoaded` and `onChannelsSelectorRefreshed`.
    - **Kept names.** Follow M37's selection logic, `RemoveLayers::colorOutcomeFor`/`droppedLayerIDs` (`:166-237`). Those are private members over flat storage, so re-implement them here; don't share them.
      - Resolve with `KnobChannelSet::resolve` against input 0's `getPresentLayers` at (time, view). Find the colour storage with `ChannelCopy::findColorStorage` (`Engine/Nodes/Channel/ChannelCopy.h`).
      - C = OR of `channels | zeroChannels` over colour rows (M37 B1+B2: a view's bits the storage lacks come back in `zeroChannels`), minus A. Non-colour layers are matched by `getLayerID()`.
      - Remove mode keeps the input's channels outside C; keep mode keeps those inside C (Q3's colour rule).
      - `A`, `Z` and `ZBack` are always kept.
      - **Not reused from M37:** `ChannelCopy::mapColorChannels`/`copyChannels` (flat pixel copy) and `metadataColorStorage` (deep has no metadata colour layout). Deep keeps buffers by name through P4.T1.
    - **Node actions.**
      - `isIdentity` when nothing would be dropped;
      - `getDeepLayers` = the groups of the kept names.
      - No `filterPassThroughLayers` override. P1.T2's exact split already turns `getDeepLayers` into the presented list, so a dropped layer or a narrowed colour plane is never passed through. M37's flat RemoveLayers needs that override only because its colour path is the default one.
    - **Registration.** Register after DeepReformat (`AppManager.cpp:1564`), before Shuffle.
  - verify: `build/m61ctest.sh DeepChannelNodes`, on a synthetic source `{R,G,B,A,diffuse.*,specular.*}`:
    - a new node is identity;
    - remove `diffuse` presents `{Color(4), specular}`, and the render lacks `diffuse.*` while sharing every other buffer and the sample table with the input;
    - remove `alpha` alone is identity (Q3);
    - remove `rgba`, and separately remove `rgb`, gives `{Color(1), diffuse, specular}`, with channels `A, diffuse.*, specular.*`, and A shares storage with the input;
    - keep `spec.*` (regex) gives `{Color(1), specular}`;
    - keep `alpha` gives `{Color(1)}`;
    - keep None gives only `A, Z, ZBack`;
    - remove regex `.*` gives the same;
    - the output of every case above has an `A` channel;
    - a downstream DeepToImage (All) of remove `diffuse` writes no diffuse.
  - size: L

- [x] M60.P4.T3 — DeepAddLayers node: zero-fill registry layers a deep stream lacks
  - files: `Engine/Nodes/Deep/DeepAddLayers.h`, `Engine/Nodes/Deep/DeepAddLayers.cpp` (new), `Engine/AppManager.cpp`, `Tests/DeepChannelNodes_Test.cpp`
  - approach:
    - **Plugin.** `fr.natron.DeepAddLayers`, Deep group.
    - **Knob.** `layers`: `KnobChannelSet` with `setWithChannelButtons(false)`, default None, declared `eRoleTarget` on input 0, mirroring `AddLayers::initializeKnobs` (`Engine/Nodes/Channel/AddLayers.cpp:90-125`). It lists the registry as views, and M37 P4.T2's "New layer…" works here. A secret sublabel uses `getShortSummary` over the registry planes, as `AddLayers::buildSubLabel` (`:323-343`) does.
    - **Registry changes re-render (M37 B5).** Copy `AddLayers::refreshForRegistryChange` (`:379-406`), called from `onChannelsSelectorRefreshed`. It builds a signature of each resolved row's layer ID and bits, and on a change calls `node->incrementKnobsAge()`. Otherwise registering a layer a row names leaves a stale deep-cache entry, because the render hash doesn't include the registry. The `refreshMetadata_public` call isn't needed, since deep metadata has no colour layout to widen.
    - **Added names.** Resolve with `KnobChannelSet::resolve` against the registry storage planes from `node->listLayersForKnob(layers, …)` (`AddLayers::listRegistryPlanes`, `:127-138`). addZero = the resolved channel names the input lacks at (time, view): a colour view adds only its missing R/G/B/A, and registry layers use `DeepLayers::channelName`.
    - **Render.** `renderDeepReshapingChannels(args, input, {}, addZero)`.
    - **Node actions.**
      - `isIdentity` when addZero is empty;
      - `getDeepLayers` = input layers ∪ added.
    - **Unconnected input.** An unconnected deep input renders an empty DeepImage (no samples).
  - verify: `build/m61ctest.sh DeepChannelNodes`, with `mask [A]` registered:
    - on `{R,G,B,A,diffuse.*}`, rows `mask` + `diffuse` add `mask.A` = 0 on every sample and leave diffuse sharing the input's storage;
    - on an A-only source, `rgb` adds R,G,B = 0 and presents `Color(4)`;
    - a row `alpha` alone is identity (A is always present);
    - on DeepRead(`deep-seq-layers`) with row `diffuse`, frame 1 is identity and frame 2 adds zeros;
    - DeepAddLayers(`mask`)→DeepExpression(layer `mask`, `expression0 = "A"`) writes A into `mask.A`;
    - `removeLayer("mask")` is refused while the node names it;
    - registering `mask` after a row already names it bumps the node's age, and the next render adds `mask.A`.
  - size: L

## Phase 60.5: GUI and round trip

- [x] M60.P5.T1 — Xvfb screenshots of the deep nodes' panels and the viewer's deep layer menu
  - files: `build/m60-gui/run-gui.sh` (copy of `build/m61-gui/run-gui.sh`), `build/m60-gui/deep_panels.py`, `build/m60-gui/*.png`, fixture copies under `build/m60-gui/`
  - approach: Pre-seed `checkForUpdates=false` (fixtures must live under `build/`). Build DeepRead(`deep-layers.exr`) → DeepRemoveLayers → DeepAddLayers → DeepExpression → DeepRecolor (Color = Read `flat-three-layers.exr`) → DeepToImage → Viewer, plus a DeepFromImage on the flat Read. Take one shot each of:
    - every node's panel, with `channels` rows showing `rgba, rgb, alpha, diffuse, specular`;
    - DeepExpression's slots relabelled R/G/B when the layer is `diffuse`;
    - the viewer layer menu on DeepRead listing `rgba, rgb, alpha, diffuse, specular`;
    - the viewer showing `diffuse` (green) straight off DeepRead.
  - verify: the PNGs exist and the script exits 0. The shots are shared with the user, who approves them at the parcel UAT.
  - size: M

- [x] M60.P5.T2 — Save/load and Python round trip of the deep layer knobs
  - files: `Tests/DeepLayers_Test.cpp`
  - approach: Build a project with non-default values on each new knob, then save, close, reload and compare:
    - DeepFromImage `channels`/`zChannel`;
    - DeepToImage `channels`;
    - DeepRecolor `channels`;
    - DeepExpression `layer` + `expression1`;
    - DeepRemoveLayers keep `spec.*`;
    - DeepAddLayers `mask`.
    - Also check Python `Effect.getAvailableLayers(-1)` on DeepRead(`deep-layers.exr`) and on DeepToImage.
    - No legacy fixtures (clean break).
  - verify: `build/m61ctest.sh DeepLayers` green: the knob values are identical after reload, the rendered DeepToImage output is identical before and after, and Python lists `rgba, rgb, alpha, diffuse, specular` plus the registry layers.
  - size: M

## Phase 60.6: Checkpoint

- [x] M60.P6.T1 — Publish the M60 decision
  - files: `docs/decisions/2026-10-<dd>-deep-layers-and-channels.md` (new), its `PLAN/DECISIONS/` mirror, `PLAN/DECISIONS/INDEX.md`
  - approach: Record, in the style of `2026-09-26-rgba-rgb-alpha-xy-layers.md`:
    - the deep grouping rule (colour storage by bits, Z/ZBack excluded, the channel-name mapping);
    - the `getDeepLayers` hook and the produced/pass-through split;
    - each node's knob and default;
    - the viewer's per-layer flatten;
    - the Q1–Q3 answers, with Q3's structural-alpha rule and its enforcement point.
  - verify: the file exists and INDEX links to it.
  - size: S

- [ ] M60.P6.T2 — Package the release AppImage for the user checkpoint
  - files: `build/appimages/M60-<sha>.AppImage`, `build/appimages/M60-uat.md`
  - approach: Build with the release `package.sh`. The UAT script walks through:
    - DeepRead `deep-layers.exr`: the viewer menu lists `diffuse`/`specular` and shows each;
    - DeepToImage All → Write EXR has all layers;
    - DeepToImage `diffuse` only;
    - DeepFromImage on a three-layer EXR, then DeepToImage, round-trips the layers;
    - DeepRecolor with a `diffuse` row;
    - DeepExpression on `diffuse`;
    - DeepRemoveLayers `diffuse`, then `rgba` (Q3): R, G and B are gone, A is kept, and the viewer's `alpha` still shows coverage;
    - DeepRemoveLayers `alpha` changes nothing;
    - keep None leaves an alpha-only stream that DeepMerge still composites;
    - DeepToImage with rows `rgb` gives an RGB image composited with deep alpha;
    - DeepAddLayers "New layer…" `mask` → DeepExpression on `mask`;
    - scrubbing `deep-seq-layers` 1↔2;
    - undo.
    - The UAT runs with the parcel UAT.
  - verify: the AppImage launches under Xvfb via `build/appimages/run-launch-check.sh` (devshell `LD_LIBRARY_PATH` stripped). The UAT itself is deferred to the parcel UAT.
  - size: M

**Verification gate:**
- `tools/ci/local/test.sh ctest debug` and `smoke debug` are green, including DeepLayers, DeepNodes, DeepReadWrite, DeepPipeline, DeepFlatten, DeepRenderPipeline, DeepChannelNodes, DataKind, WriteAllLayers and GuiTests LayerChannelRow.
- The user has approved the P5.T1 screenshots and signed off the P6.T2 UAT at the parcel UAT.
- The decision is published.

Execution notes:
- **Stacking.** M60 stacks on M50. Branch `milestone/m60-deep-layers-and-channels` off `milestone/m50-proper-ocio-support`, which already contains M65, M66, M37 and M50. The M60 PR targets `milestone/m50-proper-ocio-support`, not `main` (`DECISIONS/2026-09-22-stacked-milestone-prs.md`). It is the last link of the 2026-10-02 parcel (M66 → M37 → M50 → M60), and the PR stays open until the parcel UAT.
- **It relies on M37's code:**
  - the buttonless channel set (P1.T2);
  - "New layer…" on target rows (P4.T2);
  - the colourless-stream semantics (P2.T3);
  - M37's AppManager registrations.
- **Line numbers were re-verified on 2026-10-03** against `milestone/m50-proper-ocio-support` at `9247f995a`, after M37 and M50 landed. Deep files, `DeepImage`, `DeepFlatten`, `NativeEffectBase` and `LayerRegistry` are unchanged since M66; `EffectInstance.cpp`, `Node.cpp`, `AppManager.cpp`, `ViewerInstance.cpp` and `Tests/CMakeLists.txt` moved. Still re-find by function name if a later batch shifts them.
- **M65 vocabulary is in force.** Engine lists stay storage-level; users see views via `listLayerViewsForKnob`/`expandColorViews`. Resolve a node's own selection against the input's real `getPresentLayers`, not `listLayersForKnob`, which adds an RGBA entry to colourless streams.
- **Container and builds.** The `natron-dev` container is single-tenant. Implementers edit in parallel and don't build. Each batch gets one detached build plus ctest (setsid+nohup, a fresh `.done` marker). Check `pgrep -x ninja` is 0 before relaunching, and never pgrep-wait on a build. New engine `.cpp` files are globbed, but they need a CMake re-configure, which the batch build does.
- **Tests.** Run them through `build/m61ctest.sh <regex>` or `tools/ci/local/test.sh ctest debug`. The debug build defines NDEBUG, so tests use EXPECT/ASSERT, never assert().
- **GUI.** The recipe is `build/m61-gui/run-gui.sh`; scripts and fixture copies go under `build/m60-gui/`.
- **Batches.** No two tasks in a batch edit the same file.
  - B1: P1.T1, P2.T2
  - B2: P1.T2, P4.T1
  - B3: P1.T3, P1.T4, P2.T1, P4.T2
  - B4: P2.T3, P2.T4, P2.T5, P3.T1, P4.T3
  - B5: P2.T6, P5.T1, P5.T2, P6.T1
  - B6: P6.T2
- **Shared-file constraints:**
  - `Tests/DeepLayers_Test.cpp`: P1.T1 (B1), P1.T2 (B2), P2.T1 (B3), P2.T4 (B4), P5.T2 (B5).
  - `Tests/DeepNodes_Test.cpp`: P1.T4 (B3), P2.T5 (B4), P2.T6 (B5).
  - `Tests/CMakeLists.txt`: P1.T1 (B1), P4.T2 (B3).
  - `Engine/AppManager.cpp` and `Tests/DeepChannelNodes_Test.cpp`: P4.T2 (B3), P4.T3 (B4).
  - `Tests/DeepRenderPipeline_Test.cpp`: P4.T1 (B2), P3.T1 (B4), P2.T6 (B5).
  - `Engine/EffectInstanceRenderDeep.cpp`: P3.T1 (B4), P2.T6 (B5).
  - `Engine/EffectInstance.h`: P1.T2 (B2), P3.T1 (B4).
  - `Engine/DeepFlatten.*`: P2.T2 only. P2.T3 and P3.T1 call its new API from B4.
- **Verify dependencies:**
  - P4.T2 tests on synthetic sources, so it doesn't need P1.T3's fixtures from the same batch.
  - P2.T3 and P4.T3 use `deep-layers.exr`/`deep-seq-layers` from P1.T3 (B3).
  - P2.T6 runs last among the engine tasks, so that every earlier test that might build an A-less stream has already been re-based (P2.T5).

## Decisions

- 2026-10-02 — **Elaborated** (planning consultant). Scouting showed that deep processing nodes already preserve AOVs, contrary to the stub. The holes are DeepFromImage, DeepToImage, DeepRecolor, DeepExpression, the viewer flatten, and the absence of any deep layer reporting.
  - **Design:**
    - group deep channels into storage-level layers (colour storage = narrowest layout covering R/G/B/A bits; Z/ZBack excluded; superseded by Q3 below: R/G/B bits ∪ A);
    - report them through a new `EffectInstance::getDeepLayers` virtual, inside `getComponentsNeededAndProduced_public`, so `getPresentLayers`, M38's knobs, M65's views, the viewer menu, the registry and Python need no deep-specific path.
  - **Defaults taken:**
    - DeepFromImage/DeepToImage channel sets default to All;
    - DeepRecolor defaults to `rgb`, with alpha left under Target Input Alpha;
    - DeepExpression gets one layer select with four relabelled slots plus Z/ZBack (Q2(a));
    - the viewer flattens the selected layer;
    - DeepWrite gets no knob.
  - **RemoveLayers/AddLayers on deep are in M60** as separate `DeepRemoveLayers`/`DeepAddLayers` nodes (Q1(a)), not as polymorphic M37 nodes. Removing deep alpha is allowed (Q3(a); reversed by the user below).
- 2026-10-02 — **Q1–Q3 answered by the user** (via the coordinator). Q1(a) and Q2(a) confirm the defaults.
  - **Q3(b): deep alpha is structural.**
    - Every deep stream with samples carries `A`. DeepRead refuses files without it (P1.T3), DeepFromImage always writes it (P2.T1), and `renderDeepRoI` rejects any deep output with samples and no `A` (new P2.T6, B5).
    - Deep colour storage is `narrowestColorStorageCovering(R/G/B ∪ A)`, so always Alpha or RGBA (P1.T1).
    - DeepRemoveLayers keeps `(S \ C) ∪ {A}` in remove mode and `(S ∩ C) ∪ {A}` in keep mode. So removing `rgba` drops R/G/B and keeps A, removing `alpha` is a no-op, and keep None leaves A + Z/ZBack (P4.T2).
    - DeepAddLayers treats `alpha` as a no-op (P4.T3).
    - DeepToImage's rows may omit A from the *flat* output, but it always composites with the deep `A` (P2.T3).
  - **Revised:** P1.T1, P1.T3, P1.T4, P2.T1, P2.T3, P2.T5 (`CreatesAChannelTheInputLacks` re-based on an A-only source), P4.T2, P4.T3, P6.T1, the P6.T2 UAT, and the batches (P2.T6 joins B5).
- 2026-10-03 — **§5a freshness check against M37 and M50** (planning consultant, tip `9247f995a`).
  - **Unchanged:** every cited file and symbol still exists, and deep code is untouched since M66. M50's `convertToFormat`/`Lut.h` changes touch no M60 task.
  - **Rewritten:**
    - **P1.T2:** the deep branch splits pass-through from produced by *exact* desc, then applies `filterPassThroughLayers`. `removeFromLayersList`/`findEquivalentLayer` treat all colour layouts as one, which would make a narrowing or dropping deep node present its input's layers. New verify cases cover this.
    - **P2.T1:** a colourless Source (M37) gives no samples rather than a failed `renderRoI`.
    - **P2.T3:** DeepToImage copies RemoveLayers' `producesMetadataLayerImplicitly()=false`, blocks pass-through of the deep input's layers, and keeps metadata independent of the frame (M37 review round 1). Its view-row colour layout is corrected to the rows' bits alone, so `rgb` gives RGB, matching Q3 and its own verify; the Defaults bullet is fixed to match.
    - **P3.T1:** viewer line numbers (M50 rewrote `ViewerInstance.cpp`).
    - **P4.T2/P4.T3:** name M37's APIs (`setWithChannelButtons(false)`, the `getShortSummary` sublabel, `channels|zeroChannels` colour bits, `ChannelCopy::findColorStorage`). DeepAddLayers copies AddLayers' registry-signature age bump (M37 B5), or its deep cache goes stale. M37's flat `ChannelCopy` pixel copy and `filterPassThroughLayers` override are deliberately not reused.
  - Scout-note line numbers updated. Batches unchanged, since no file overlaps changed. Stacking note restated: the PR targets `milestone/m50-proper-ocio-support`.
- 2026-10-03 — **B1+B2 landed:** P2.T2, then P1.T1+P1.T2+P4.T1 in one commit, because they share the test effect header and `DeepLayers_Test`. Full debug ctest 823/823.
  - **Note for P1.T3 and later:** P1.T2's `containsIdenticalLayer` compares layer IDs and ordered channel lists, so a node that reports its input's layers with channels in a different order counts them as produced, not passed through.
  - **Order source:** `DeepSyntheticSource` gets `diffuse` as B,G,R from a sorted map. `groupDeepChannels` must give a canonical channel order, or the comparison must ignore order.
  - **P1.T3's implementer decides which,** keeping the colour storage's bit order.
- 2026-10-03 — **B3 landed** (`ec44b3f1a` P1.T3, `dd4641a41` P1.T4, `b60f57365` P2.T1, `3e9df7ba5` P4.T2). Full debug ctest 850/850.
  - **Channel order.** `groupDeepChannels` now orders non-colour channels canonically: R, G, B, A, then the rest alphabetically. A deep image's channel map is sorted, so input order isn't usable.
  - **DeepFromImage choices (accepted).**
    - It clears pass-through, so every deep layer it reports counts as produced.
    - With Z connected, a missing Z value reads 0.
    - An alpha-less Source gives A = 1 samples, fixed in the B3 build round.
  - **DeepRemoveLayers sublabel.** It adds "A is always kept" only when the rows read as if they'd drop A.
  - **Open for review.** DeepRead's layer memo is keyed on filename only, so a file rewritten on disk keeps its old layers.
- 2026-10-03 — **B4 landed** (`c650f13fe` P2.T4, `a3aa0107b` P2.T5, `c1228c937` P3.T1, `05746e5d9` P2.T3, `62630f6f4` P4.T3). Full debug ctest 877/877.
  - **Lost edit recovered.** Parallel edits to `DeepChannelNodes_Test.cpp` dropped P4.T3's tests. The build round rewrote them from the brief.
  - **P3.T1 cache lookup.** The deep flatten cache matches exact components and mip level, because the generic lookup treats colour layouts as interchangeable. As a result, a deep view at a new zoom level re-renders rather than downscaling a cached flatten.
  - **P2.T5 known gaps.** The DeepExpression field labels don't follow the timeline, and changing the layer doesn't clear an existing error until the next render.
- 2026-10-03 — **B5 landed** (`033298539` P2.T6, `41b0035d2` P5.T2, `742f67578` decision published). Full debug ctest 881/881, smoke green, release builds. The P5.T1 Xvfb pass is 159/159 across 20 shots in `build/m60-gui/shots/`.
  - **Defects found by the GUI pass, outside M60:**
    - The viewer toolbar's OCIO row is about 1375 px wide since M50.P4.T7 fitted the combos to their contents. It squeezes the Properties pane at 1600 px. Fixed on M50's branch and merged up.
    - Deep nodes draw black label text on dark navy in the node graph. This predates M60 and is filed for UAT.
