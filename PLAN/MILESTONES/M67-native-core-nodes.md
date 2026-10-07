# Milestone 67: Rewrite core nodes as native nodes

> **Elaborated 2026-10-06** (planning consultant, read-only, on `origin/milestone/m63-task-graph-render-scheduler` at `ce575b4d2`). The branch stacks on M63's PR #41. M64 is `blocked` after its strip-pull spike missed kill gate 1, and is parked until native kernels exist. Order: a benchmark harness and a native Grade, then a **go/no-go gate (P1.T6)**, then four family phases (colour → merge + generators → spatial → keying/misc), then wrap-up and packaging. Each family's native nodes take over the OFX plugin IDs at a higher major version. That family's openfx-misc plugins are retired from the bundle through one fork PR, `charlesangus/openfx-misc` `m67/retire-native-core`, which is merged with the milestone.

Natron's everyday nodes (Grade, Merge, Blur, Transform, ColorCorrect, Constant, CheckerBoard, and the colour and keying tools) are OpenFX plugins from openfx-misc, hosted through the OFX ABI. M62.P5.T1 measured ~0.65 MB per idle node, 75–80% of it OpenFX-specific: property-set copies and OFX→knob glue. The HD profiles show plugin pixel code and host/plugin copying dominating per-node time. The native-node framework from M17 (`Engine/Nodes/NativeEffectBase`) already hosts the deep and channel nodes, with typed edges, direct knob declaration and no ABI boundary. This milestone rewrites the core 2D node set as native nodes, one family at a time, and retires the corresponding openfx-misc plugins from the default bundle. Script names, knob names and defaults stay the same as the OFX versions, so Python scripts, PyPlugs and muscle memory carry over. **Clean break:** projects saved with the OFX versions are not migrated. No legacy fixtures and no migration code are written.

## Scout notes (2026-10-06, at `ce575b4d2`)

Read code at the rev with `git show ce575b4d2:<path>`, since the shared checkout moves between branches. Line numbers are at `ce575b4d2`.

**Native framework**
- `Engine/Nodes/NativeEffectBase.h`:
  - `NativeInputDescription` `:47-60` has label, optional and kind. It has **no `isMask`**.
  - `NativePluginDescription` `:68-91` has a single `grouping` string.
  - The class is at `:173`. `renderThreadSafety()` returns `eRenderSafetyFullySafeFrame` at `:270`. The `createKnob<>` helpers are at `:396-408`.
  - `.cpp` defaults: `addAcceptedComponents` (RGB, RGBA, Alpha) at `:157` and `addSupportedBitDepth` (byte, short, float) at `:166`.
  - The deep helpers (`renderDeepTwoPass`, `forEachDeepChunk` on `parallelForOnGlobalPool`) are at `:196-360`.
- `Engine/Nodes/README.md`:
  - Registration is one `registerBuiltInPlugin<T>(icon, deprecated, internal)` line in `AppManager::loadBuiltinNodePlugins()` (`Engine/AppManager.cpp:1579-1614`). The template is at `:1540-1576`.
  - Tests copy `Tests/TypedPassthrough_Test.cpp`.
  - `Engine/CMakeLists.txt:43-46` GLOB_RECURSEs `Engine/Nodes/**`, so new node files need no CMake edit.
- Existing native **image** nodes (Shuffle, ShuffleCopy, RemoveLayers, AddLayers in `Engine/Nodes/Channel/`) are all `isMultiPlanar()==true`. **No native node yet takes the host's process-in-place path** (non-multiplanar, host layer knob, unprocessed-channel copy). `RemoveLayers.cpp:91-140` shows the knob idioms: `setName`, `setIsMetadataSlave`, `declareLayerKnob`, and a sublabel knob named `kNatronOfxParamStringSublabelName`. `RemoveLayers.cpp:339-411` shows the render idiom: fetch every input plane before locking any image, then `Image::ReadAccess`.
- `Engine/Nodes/Channel/ChannelCopy.h` has the colour-plane helpers. `ImageLayerDesc.h:259-322` has the M65 colour-view helpers (`colorViewChannelBit`, `colorViewChannelIndex`, `colorStorageBits`, `colorViewForNComps`). An alpha-only image's single channel is the A bit.

**Plugin identity and versions**
- `AppManager::registerPlugin` (`AppManager.cpp:2068-2105`) keeps a **version set per ID**.
- `getPluginBinary` (`:2265-2335`) resolves a request as follows:
  - Major `-1` gives the highest version.
  - An exact major gives that version.
  - Otherwise it gives the closest major above, else the highest.
- `BaseTest::createNode(id, major, minor)` (`Tests/BaseTest.h`) and Python `app.createNode(id, major, group)` both select by major. So a native node registered under the OFX ID at **OFX major + 1** shadows the OFX one for every unversioned or older request, while the OFX plugin stays reachable by its exact major until it is retired.

**Host machinery the native nodes must plug into (M38/M65)**
- Layer knob:
  - `LayerKnobSpec` is at `EffectInstance.h:114-145`. The default `getLayerKnobSpec()` (`EffectInstance.cpp:5184-5192`) gives a ChannelSet, InputBound, with channel buttons, for any non-multiplanar image node.
  - `OfxEffectInstance::getLayerKnobSpec` (`OfxEffectInstance.cpp:1310-1320`) gives generators a LayerSelect, Target, with channel buttons.
  - `defaultProcessesAllLayers` (`:1323-1437`) is a list keyed by plugin ID. Transform, Crop, Position, Reformat, CImgBlur, CImgErode, CImgDilate and Dissolve process all layers. EdgeDetect and ChromaBlur keep colour.
  - `Tests/DefaultChannelSet_Test.cpp:56-146` pins the expected defaults per plugin ID.
- `Node::createLayerKnob` (`Node.cpp:3028-3065`) puts the knob at row 0 with a separator under it. `Node::initializeDefaultKnobs` (`:~3067-3170`) then:
  - creates mask selectors for every `isInputMask` input (`createMaskSelectors` `:2753-2815`, knobs `enableMask_<label>` and `maskChannel_<label>`);
  - calls `createUnPremultSelector` (`:2665-2705`), which **only triggers when the plug-in has knobs `unPremultBy`/`unPremultByChannel`** (`Node.h:69-70`) and creates `hostUnPremultBy` (`Node.h:67`);
  - re-orders `maskInvert`/`mix` (`Node.h:79-80`) to the end of the page.
- `adoptChannelQuad` (`:~2890-2990`) takes the default channel row from `isHostChannelSelectorSupported(&r,&g,&b,&a)` (`EffectInstance.h:2102-2113`) when there is no `NatronOfxParamProcess*` quad.
- Input-layer resolution for the plane being rendered (the no-shuffle invariant) is OFX-only code at `OfxClipInstance.cpp:850-915`. It uses `getThreadLocalNeededComponents`, `getThreadLocalOutputLayerBeingRendered`, `findEquivalentLayer`, and `getMaskChannel` for masks.
- The host "(Un)premult by" divide is OFX-only at `OfxClipInstance.cpp:1031-1076`, recorded through `setThreadLocalUnPremultDivisor`.
- After render, `EffectInstance.cpp:3111-3240` runs the NaN check, `premultiplyByChannel` (only if a divisor was recorded), `copyUnProcessedChannels` from the preferred input, and `applyMaskMix` (only when `isHostMaskingEnabled()`/`isHostMixingEnabled()`, which default to false at `EffectInstance.h:2122-2133`).
- Image API: `ReadAccess`/`WriteAccess` (`Image.h:359/419`), `copyUnProcessedChannels` `:767`, `unPremultiplyByChannel` `:786`, `premultiplyByChannel` `:797`, `applyMaskMix` `:806`.
- Rendering:
  - `RenderActionArgs` (`EffectInstance.h:1302-1318`) carries `outputLayers`, `roi`, `mappedScale` and `processChannels`.
  - `supportsTiles()` defaults to false (`:1684`). `supportsMultiResolution()` defaults to true (`:1696`).
  - Render scale is probed when it is `Maybe` (`Node.cpp:7380-7390`).
  - `getCanTransform`/`getTransform` are at `:1171/:1394` and `isIdentity` at `:1427`.
  - Host frame threading slices the RoI for `FullySafeFrame` + tiles + budget > 1 (`EffectInstanceRenderRoI.cpp:1158-1170`) through `parallelForOnGlobalPool` (`:1811-1854`).

**M63 scheduler constraints**
- Tasks pull inputs with the request pass's RoI and components. A `getImage` outside the planned RoI or layer is an **unplanned pull**, which `SchedulerEquivalence_Test` asserts is 0. So `getRegionsOfInterest` and `getComponentsNeededAndProduced` must declare everything `render()` fetches: the mask layer and the unpremult divisor layer included.
- `Tests/RenderBothWays.h:46-89` has `renderBothWays` (writer EXRs) and `renderBothWaysDirect` (renderRoI vs scheduler, bit-exact). The `renderDirect` and `readWindow` helpers are in an anonymous namespace (`RenderBothWays.cpp:337/431`).
- M63's investigation: at HD, kernels are 60–80% of the time and engine passes 10–14%. "Native nodes matter for fused kernels and host-controlled threading."
- Graph build cost: 64 s for 2000 OFX Grades, 109 s for 4000 native pass-throughs.

**M64 (parked)**
- Segments need tile-capable, uncached, `FullySafe`/`FullySafeFrame` point ops (RoI == output window).
- Kernel fusion across nodes "needs native nodes (M67)" (M64 Design §7).
- The spike measured 0.60–1.21x on OFX Grade chains with a per-call cost fit of a≈0.27 ms, b≈68 ns/px.

**Engine and GUI code that names the replaced plugins (it keeps working through ID takeover)**
- Roto's internal tree:
  - `RotoDrawableItem.cpp:186-268` creates Merge, CImgBlur, Constant and Transform by ID.
  - `RotoContext.cpp:4674` creates a Merge.
  - Knob names are hard-coded in `Bezier.cpp:59-74`, `RotoContext.cpp:71-88` and `RotoDrawableItem.cpp:65-67`: `operation`, `maskInvert`, `size`, `translate`, `rotate`, `scale`, `uniform`, `skewX`, `skewY`, `skewOrder`, `center`, `filter`, `resetCenter`, `black_outside`.
- Tracker creates and links a Transform (`TrackerContext.cpp:826`, `TrackerContextPrivate.cpp:163`).
- GUI:
  - Default shortcuts are keyed by ID (`Gui/GuiApplicationManager.cpp:1010-1023`): T Transform, M Merge, G Grade, C ColorCorrect, B CImgBlur.
  - Merge's operator icon comes from its sublabel (`Gui/NodeGui.cpp:334`, `:3309`).
  - Merge is created by drag hint (`Gui/NodeGraph15.cpp:278`, A input found by label).
  - Parametric control-point interpolation is chosen by plugin ID (`Engine/OfxParamInstance.cpp:4869-4873`): ColorCorrect horizontal, ColorLookup cubic.
- Host overlays exist for position and transform only:
  - `Node::addPositionInteract` `Engine/NodeOverlay.cpp:295`, `Node::addTransformInteract` `:318`, `KnobDouble::setHasHostOverlayHandle` `Engine/KnobTypes.cpp:230-255`.
  - `Gui/HostOverlay.cpp` has Position `:257`, Transform `:349` and CornerPin `:635`.
  - **There is no rectangle host overlay**, which Crop and the generators need.
- KnobParametric can be created natively (`KnobTypes.h:968-1014`). A curve-widget background interact is OFX-only (`Gui/CurveWidget.cpp:666-676`), which ColorLookup's ramp/histogram background uses.

**Tests and PyPlugs**
- The replaced IDs appear in ~45 files. The test-file references are concentrated in:
  - `Tests/ChannelSetRender_Test.cpp`: process-in-place and unpremult on Grade, Invert, ColorCorrect, Multiply, Saturation and CImgBlur. Several cases assert OFX-specific hidden knobs, e.g. `:701`, `:1293`.
  - `DefaultChannelSet_Test`, `LayerKnobs_Test`, `Metadata_Test`, `GeneratorLayer_Test`, `ColorViewsRender_Test`.
  - The scheduler suites (Grade and CheckerBoard chains).
  - Two legacy `.ntp` fixtures (`Tests/fixtures/channel-set-legacy-defaults.ntp`, `m65-legacy-color.ntp`).
- PyPlugs that use the replaced nodes are Glow, PIKColor, LightWrap, ZMask, DropShadow, EdgeBlur, AngleBlur, ZRemap and Fill. They call `createNode` with older majors, e.g. `DropShadow.py:211` TransformPlugin 1, `:257` MultiplyPlugin 2, `:287` Solid 1, `:333` CImgBlur 3, `:350` MergePlugin 1. `Tests/PyPlugInstantiate_Test.cpp` instantiates them all.
- `Tests/ShufflePluginList_Test.cpp` is the precedent for asserting that a retired OFX plugin is absent.
- `Tests/CMakeLists.txt:20-104` is an explicit list, a shared hotspot. Test effects are registered in `Tests/wmain.cpp:~112` (`registerTestBuiltInPlugin<T>()`).

**Bundle**
- `tools/ci/local/fetch-assets.sh:126-149` builds openfx-misc from our fork at `OPENFX_MISC_REF=3060fe33…`. Its comment records the precedent: "this revision excludes the OFX Shuffle plugin".
- In the fork, `CMakeLists.txt:111-204` (`MISC_SOURCES` GLOB, one line per directory), `:206+` (`MISC_RESOURCES`) and `:374-402` (`CIMG_SOURCES`) decide what is built.
- Multi-factory files need line-level edits rather than directory removal:
  - `Merge/Merge.cpp:1962-2008`: 11 IDs at 2.0, plus compat `q*` at 1.0.
  - `Constant/Constant.cpp:452-455`: Constant and Solid.
  - `Transform/Transform.cpp:557-562`: Transform, TransformMasked and DirBlur.
  - `CImg/Blur/CImgBlur.cpp:3259-3285`: `oldp1-4` at major 3, and `p1..p9` at major 4 (Blur is `p1`, EdgeDetect is `p9`).
  - `Reformat/Reformat.cpp:1103-1105`: 2.0, plus compat 1.1.
- `tools/ci/smoke_test.py:295-330` checks that Misc and CImg plugin IDs enumerate, using Constant, Grade, Merge and CImgBlur as representatives. `:598-670` renders Constant→Grade through Misc.
- A pinned openfx-misc source checkout is at `build/assets/plugin-src/openfx-misc`. The knob tables used below come from it.

**Bench**
- `tools/bench/graph_bench.py`:
  - `make()` `:138` calls `createNode(id, -1, …)`.
  - The `source()` CheckerBoard is at `:209`, `grade()` at `:223`, `merge()` at `:251` (input 0=B, 1=A, Mask by label), `build_chain` at `:297`.
  - The result record (`:~585-620`) has `build_s`, `rss_before_mb` and `rss_peak_mb`.
- `lib.sh:6-25` `bench_cooldown` reads `/proc/loadavg` only, with **no PSI**. `run_matrix.sh` hard-codes `build/release/Renderer/NatronRenderer`. `compare.py --pair-settings` pairs rows by settings only.
- `BASELINE.md` "After thread budget" (`2db83845a`, quiet N100): HD chain 30 took 1.0125 s and chain 100 took 3.0829 s, so ~29.6 ms marginal per OFX Grade.
- The GUI recipe is `build/m63-gui/run-gui.sh`: Xvfb `:79` with GLX, `checkForUpdates=false` pre-seeded, and screenshots by `QGuiApplication.primaryScreen().grabWindow(0)` in `viewer_check.py`.

**openfx-misc reference (knob script names, defaults, maths)**
The briefs quote what they need from these files: `Grade/Grade.cpp`, `ColorCorrect/ColorCorrect.cpp`, `Saturation/Saturation.cpp`, `Clamp/Clamp.cpp`, `Invert/Invert.cpp`, `Add/Add.cpp`, `Multiply/Multiply.cpp`, `Gamma/Gamma.cpp`, `Merge/Merge.cpp` plus `SupportExt/ofxsMerging.h`, `Dissolve/Dissolve.cpp`, `Constant/Constant.cpp`, `CheckerBoard/CheckerBoard.cpp`, `SupportExt/ofxsGenerator.{h,cpp}`, `Transform/Transform.cpp` plus `SupportExt/ofxsTransform3x3.cpp`/`ofxsFilter.h`, `Crop/Crop.cpp`, `Reformat/Reformat.cpp`, `Position/Position.cpp`, `Keyer/Keyer.cpp`, `ChromaKeyer/ChromaKeyer.cpp`, `ColorLookup/ColorLookup.cpp`, `CImg/Blur/CImgBlur.cpp`, `CImg/Erode/CImg{Erode,Dilate}.cpp`, `CImg/CImgFilter.h`, `SupportExt/ofxsMaskMix.h` and `SupportExt/ofxsLut.h:955-1105` (luminance coefficients).

Facts that shape the design:
- Under Natron, `ofxsMaskMix` declares no `mask` bool, because masking is on whenever the Mask clip is connected. It does declare `maskInvert` (false) and `mix` (1, range 0..1).
- This fork renamed the premult pair to `unPremultBy`/`unPremultByChannel`. The host hides it and replaces it with `hostUnPremultBy`.
- Merge, Transform, Dissolve, Crop, Position, Keyer and ChromaKeyer have no premult pair. Dissolve has no `mix`.

## Design (binding for the briefs below)

1. **Identity: take over the OFX IDs at OFX major + 1.**
   - Every native node registers under the ID of the OFX plugin it replaces, with `majorVersion` = the highest OFX major registered for that ID + 1, and minor 0. The table:
     - Grade, ColorCorrect, Saturation, Clamp, Invert, Add, Multiply, Gamma: 3.
     - Merge and its siblings: 3.
     - Dissolve, Constant, Solid, CheckerBoard, Transform, TransformMasked, Crop, Position, Keyer, ChromaKeyer, ColorLookup: 2.
     - Reformat: 3. CImgErode, CImgDilate: 3. CImgBlur, eu.cimg.EdgeDetect: 5.
   - Rationale: every unversioned or older-major request then resolves to the native node. That covers Python `app.createNode(id)`, the PyPlugs' versioned calls, Roto's and Tracker's internal nodes, the default shortcuts, the node-graph Merge hint, `smoke_test.py` and the bench.
   - The OFX plugin stays loadable by its exact major until its family retires. That is how the parity harness renders both side by side: the same ID at different majors. It gives the user's "different IDs" requirement through versions, without touching the call sites.
   - Labels drop the `OFX` suffix ("Grade", "Plus", …), grouping matches the OFX menus (`Color/Math`, `Merge/Merges`), and icons are the openfx-misc PNGs.
   - Siblings:
     - The nine Merge presets (Plus, Matte, Multiply, In, Out, Screen, Max, Min, Difference) are one native class registered under each ID, with its default `operation`.
     - Solid is a Constant preset. TransformMasked is Transform with a Mask input. Add, Multiply and Gamma are one `ColorMath` class with three IDs. Erode and Dilate are one class with two IDs.
   - **Dropped, not ported:**
     - `net.sf.openfx.MergeRoto`: no engine, PyPlug or test uses it.
     - Every OFX compat factory (Merge `q*` 1.0, CImgBlur `oldp1` 3.0, Reformat 1.1). Clean break.
   - **Kept as OFX:** DirBlur, Laplacian, ChromaBlur, Bloom, ErodeBlur, Sharpen, Soften, EdgeExtend. Their factories stay in the fork, and only the replaced registrations are removed.
2. **One base for flat 2D native nodes:** `Engine/Nodes/Image/NativeImageEffect` on `NativeEffectBase`. It is a convenience layer, not a second hierarchy.
   - Flags:
     - `supportsTiles` true, multi-resolution true, render scale Yes (set in the constructor; spatial params scale by `mappedScale`).
     - Thread safety `FullySafeFrame`, so host frame threading slices the RoI on `parallelForOnGlobalPool` under M63's per-task budget, and point ops never spawn threads of their own.
     - **Float only** (`addSupportedBitDepth`). The engine already works in float, which removes three template instantiations per node.
     - Accepted components follow the OFX clip lists.
   - Per-instance state is knobs only: no caches, no per-instance LUTs, and kernels and LUTs are built per render call.
   - `NativeInputDescription` gains `isMask`, and the base implements `isInputMask` from it, so the host creates `enableMask_*`/`maskChannel_*` exactly as for OFX.
   - Grouping becomes a `/`-separated path.
3. **Process in place, unpremult, mask and mix in one pass, in the node.**
   - The host still owns *which* layers and channels (M38 layer knob row 0, the `channels` script name, defaults from `isHostChannelSelectorSupported` and `defaultProcessesAllLayers`, and `hostUnPremultBy`).
   - The native base does the pixel work per row, in one pass:
     1. Divide the processed colour channels by the `hostUnPremultBy` channel. It may be another layer; the skip rule is `Node::getUnPremultSkipChannel`.
     2. Run the node's op.
     3. Multiply back.
     4. Apply mask × `mix` against the undivided source.
     5. Pass unprocessed channels through.
   - A new `EffectInstance::rendersUnprocessedChannels()` virtual (default false) tells `tiledRenderingFunctor` to skip its `copyUnProcessedChannels` pass for these nodes.
   - The divide and multiply maths are shared with `Image::unPremultiplyByChannel`/`premultiplyByChannel` through inline per-pixel helpers, so both paths compute identical values.
   - The `hostUnPremultBy` selector is created for native colour ops through a new `EffectInstance::wantsHostUnPremultSelector()` virtual. The OFX knob-name detection stays the OFX implementation.
   - Input-plane resolution (which input layer feeds the plane being rendered, and which layer/channel a mask reads) moves out of `OfxClipInstance` into one `EffectInstance` helper used by both.
   - Knob script names stay the OFX ones: `mix`, `maskInvert`, `channels`, `hostUnPremultBy`, `enableMask_Mask`, `maskChannel_Mask`. The OFX-internal `NatronOfxParamProcess*`, `unPremultBy*` and `premultChanged` are not declared.
4. **Scheduler-correct by declaration.**
   - Every node declares, through `getRegionsOfInterest`, `getComponentsNeededAndProduced` and `getFramesNeeded`, everything its `render()` fetches: mask and divisor layers included. So M63 task-graph renders show **0 unplanned pulls**, and Legacy and Task graph are bit-identical at pool sizes 1 and 4.
   - `isIdentity` matches the OFX conditions, so identities become identity tasks.
   - Transform (and Reformat with `preserveBB`) implement `getCanTransform`/`getTransform`, and consume a concatenated input transform the way the OFX Transform does.
   - No deep data and no GL in this milestone.
5. **Fusion-ready, not fused (M64 later).**
   - A point op implements `PixelKernel` (`Engine/Nodes/Image/PixelKernel.h`): an immutable object built from knob values at (time, view, scale). It is pure on (input rows, mask row, divisor row) → output row, with no knob access, allocation or I/O inside `processRow`.
   - `NativeImageEffect::isPointOp()` declares it.
   - The base's own render path is "build kernel once, run rows over the host's tile". A later fusing pass can chain kernels over a strip with no node changes. No fusion is built here.
6. **Parity.**
   - `Tests/Native/NativeParity.{h,cpp}` renders the OFX version (exact old major) and the native version (new major) of one ID from the same deterministic test source, through `renderRoI` at mipmap 0 and 1. It compares every channel of every rendered plane under a per-node tolerance.
   - It also asserts **knob parity**: each user-visible OFX knob exists on the native node with the same script name, type, dimension and default.
   - While the OFX plugin is loadable the comparison is live. With `NATRON_PARITY_RECORD_DIR` set, it also writes the OFX output of the cases marked `record` as `.f32` files (`NPAR` header, w, h, nComps, float32 LE).
   - After retirement, the harness compares against `Tests/fixtures/native-parity/<id>/<case>.f32`. These are small images, at most 64×48, and ~3 cases per node.
   - If neither the OFX plugin nor a reference exists, that is a test **failure**. The repo's gtest has no `GTEST_SKIP`, and tests use `EXPECT_*`/`ASSERT_*`, never `assert()`.
   - Tolerance classes, each written as a named constant in the node's test and tabulated in `Engine/Nodes/README.md` (P6.T3):

| Class | Bound |
|---|---|
| Exact (generators, Position, Clamp, Invert, Erode/Dilate, Merge Porter-Duff) | 0 |
| Arithmetic | 1e-6·max(1,\|ref\|) |
| `pow`/trig/LUT (Grade gamma, Gamma, ColorCorrect, Saturation, Keyer, ChromaKeyer, ColorLookup, Merge blend modes) | 1e-5·max(1,\|ref\|) |
| Resampling (Transform, Reformat, Crop softness) | 1e-5 abs |
| IIR blur, EdgeDetect, motion blur | 1e-4 abs |

   - A node that cannot meet its class gets its measured bound and the reason recorded in its test and in `## Decisions`.
   - Deliberate fixes of OFX bugs are listed as divergences and excluded from parity by case. Saturation's ACES AP1 fall-through is fixed.
7. **UI.**
   - Panels follow M38 (`PLAN/DESIGN/2026-09-19-layer-channel-widget.md`): the layer knob at row 0 with its separator, then the node's own knobs in OFX order, then the mask selectors, `hostUnPremultBy`, `maskInvert` and `mix` last (re-ordered by the host by name).
   - Overlays use host overlays: `addTransformInteract` for Transform, `addPositionInteract` for Position, and a new rectangle host overlay for Crop and the generators' Size extent.
   - Every family ends with an Xvfb GUI check that screenshots OFX and native panels side by side before retirement, for the user's approval at the parcel UAT.
8. **Retirement per family.**
   - One fork PR, `charlesangus/openfx-misc` branch `m67/retire-native-core`, branched from the current pin `3060fe33`, with one commit per family. It removes:
     - the family's directories from `MISC_SOURCES`/`MISC_RESOURCES`;
     - the CImg files from `CIMG_SOURCES`;
     - the replaced registrations, **including all compat versions**, from multi-factory files.
   - `fetch-assets.sh` is re-pinned to the branch head each time, and to the merge commit when the fork PR merges with the milestone.
   - `smoke_test.py`'s Misc and CImg representatives move to plugins that stay OFX.
   - `OfxEffectInstance.cpp`'s ID lists and the per-ID special cases in `OfxParamInstance.cpp` lose the dead IDs.
   - A plugin-list test asserts that each retired ID has exactly one registered version and that it creates a `NativeEffectBase`.
9. **Workflow.**
   - Implementers edit in parallel and do not build. The PM runs one detached debug build + ctest per batch in the single-tenant `natron-dev` container.
   - Shared hotspots have one owner per batch (see the batch plan): `Engine/AppManager.cpp` registration, `Tests/CMakeLists.txt`, `Tests/wmain.cpp`, `Gui/GuiResources.qrc`.
   - P1.T3 adds a `Tests/Native/*_Test.cpp` GLOB, so per-node tests never touch `Tests/CMakeLists.txt`.
   - Benchmarks compare **ratios within one run only**: native and OFX (or base and head) rounds are interleaved ABAB. Every config records `/proc/loadavg` and `/proc/pressure/{cpu,io,memory}`. An absolute number goes into `BASELINE.md` only when the run was quiet (load1 < 0.5 and cpu/io `some avg10` < 5% at every config start); otherwise it is labelled "contended, ratios only". The host has recently run at load ~20 with IO pressure ~95% from outside the sandbox.
   - Hardware range: nothing may assume 4 cores or the N100's caches. Threading comes from the host's slicing under the M63 budget, and spatial nodes parallelise internally by whole rows or columns, never by the host's tile split (see P4.T7).

## Phase 67.1: Benchmark harness, native Grade, go/no-go gate

- [x] M67.P1.T1 — Extend the bench harness for native-vs-OFX runs with load and pressure recorded
  - files: `tools/bench/graph_bench.py`, `tools/bench/lib.sh`, `tools/bench/run_matrix.sh`, `tools/bench/compare.py`, `tools/bench/native_vs_ofx.sh` (new)
  - approach:
    - `graph_bench.py`:
      - `BENCH_IMPL=ofx|native` (default `native`, which is today's behaviour after takeover). `make()` passes an explicit major for the IDs in a table `{"net.sf.openfx.GradePlugin": (2, 3)}`: ofx→2, native→3. Later families extend the table with one line each.
      - The `chain:0` topology (source + writer only) must work, so per-node deltas can be computed.
      - Record `impl`, plus `load1_start`, `psi_cpu_some10`, `psi_io_some10` and `psi_mem_some10` read from `/proc/pressure/*` at the start and end of the timed phase (`null` when unreadable).
    - `lib.sh`: add `bench_pressure` (prints loadavg and the three PSI `some avg10`), and log it from `bench_cooldown` on the host.
    - `run_matrix.sh`: forward `BENCH_IMPL`, and take `BENCH_RENDERER_DIR` (default `build/release`) and `BENCH_PLUGIN_PATH` (default `build/assets/Plugins`), so a second build and plugin set can be benched later (P6.T1).
    - `compare.py`: `--pair-field <field> <a> <b>`, the generalisation of `--pair-settings`, pairs rows by topo/n/res and that field and prints b/a ratios.
    - `native_vs_ofx.sh <tag> <rounds>` runs, per round, for impl in ofx then native: `tiny 5 0 chain:0,1000` and `hd 3 0 chain:30,100`. It then prints the four gate ratios of P1.T6 per round and their median. It refuses to start if `build/release` is older than `Engine/Nodes/Image/`.
  - verify:
    - `BENCH_DRY_RUN=1 BENCH_IMPL=ofx BENCH_TOPO=chain BENCH_N=3 python3 tools/bench/graph_bench.py` on the host prints major 2 for the Grades.
    - `chain:0` dry-runs.
    - `compare.py --pair-field impl ofx native` on two hand-written jsonl lines prints the expected ratio.
    - `shellcheck` is clean on the shell files.
  - size: M

- [x] M67.P1.T2 — Engine hooks for native flat nodes: shared input-plane resolution, host unpremult selector, unprocessed-channel ownership
  - files: `Engine/EffectInstance.h`, `Engine/EffectInstance.cpp`, `Engine/OfxClipInstance.cpp`, `Engine/Node.cpp`, `Tests/Native/EngineHooks_Test.cpp` (new; built by P1.T3's glob)
  - approach:
    - (a) Move the plane-selection logic at `OfxClipInstance.cpp:850-915` into a public helper: `bool EffectInstance::resolveInputPlaneForRender(int inputNb, double time, ViewIdx view, ImageLayerDesc* layer, int* maskChannel) const`. It chooses the needed-components entry equivalent to the output layer being rendered, then the mask channel through `getMaskChannel`, then the layer knob, then the clip components. `maskChannel` is -1 for non-masks. `OfxClipInstance` calls the helper with no behaviour change.
    - (b) Add `virtual bool wantsHostUnPremultSelector() const` (default false). `Node::createUnPremultSelector` (`Node.cpp:2665-2705`) creates the `hostUnPremultBy` selector when the effect returns true, even without the plug-in `unPremultBy`/`unPremultByChannel` knobs. The OFX path keeps detecting those knobs.
    - (c) Add `virtual bool rendersUnprocessedChannels() const` (default false). When true, `tiledRenderingFunctor` (`EffectInstance.cpp:~3150-3240`) skips `copyUnProcessedChannels` and the re-multiply, because the effect writes every channel itself. The NaN check stays.
    - Expose the per-pixel divide/multiply used by `Image::unPremultiplyByChannel`/`premultiplyByChannel` as inline helpers in `Engine/Image.h`, so P1.T4 computes the same values. This is a sixth file, `Engine/Image.h`. It is allowed because it is a mechanical extraction.
  - verify:
    - Full ctest is unchanged in both scheduler modes. `ChannelSetRender_Test`, `ChannelSetRenderUnPremultByTest` and `LayerKnobsRender_Test` prove the OFX path still selects the same planes and divides the same way.
    - `EngineHooks_Test` checks:
      - a test effect overriding `wantsHostUnPremultSelector()` gets a `hostUnPremultBy` knob;
      - `resolveInputPlaneForRender` returns the rendered-plane-equivalent layer for an OFX Grade on a three-layer input, and the selected mask channel when `maskChannel_Mask` names `spec.G`.
  - size: L

- [x] M67.P1.T3 — Parity harness: deterministic test source, OFX-vs-native renderer, knob parity, reference record and replay
  - files: `Tests/NativeParity.h` (new), `Tests/NativeParity.cpp` (new), `Tests/RenderBothWays.h`, `Tests/RenderBothWays.cpp`, `Tests/wmain.cpp`, `Tests/CMakeLists.txt`, `Tests/Native/NativeParitySelf_Test.cpp` (new)
  - approach:
    - `Tests/CMakeLists.txt`: `file(GLOB Tests_NATIVE_SOURCES Native/*_Test.cpp)` appended to `Tests_SOURCES`, plus `NativeParity.cpp`. This is the last hand-edit of the list this milestone, outside P6.
    - Promote `renderDirect`/`readWindow` from `RenderBothWays.cpp`'s anonymous namespace into a declared `renderNodePlanesDirect(node, time, view, mipmap, roi, layers, &out)` that returns the float pixels of every requested plane.
    - In `NativeParity.h`, define `ParitySourceTestEffect`. It is a test-only native generator, registered in `wmain.cpp`, whose `components` knob selects RGBA, RGB or Alpha and whose `extraLayer` knob adds an `spec` RGB plane. It is multiplanar like `MultiplanarTestEffect.h`, which may be extended instead. The RoD is 64×48 at an `origin` knob.
      - Values: r/g/b ramps over [-0.25, 1.75] with a sin term. Alpha holds rows of exact 0, exact 1, and a [0,1] ramp. Everything is offset by `0.01·time`.
    - API:
      - `ParityPair makeParityPair(BaseTest&, id, ofxMajor, nativeMajor)` creates both nodes on one source, plus an optional shared mask source.
      - `setKnobOnBoth(pair, name, values…)` fails if either node lacks the knob.
      - `ParityResult compareParity(pair, caseName, roi, mipmap, Tolerance, bool record)`.
      - `expectKnobParity(ofxNode, nativeNode, ignoredNames)`. It skips secret and OFX-internal names: `NatronOfxParamProcess*`, `unPremultBy*`, `premultChanged`, `aChannelsChanged`, `bChannelsChanged`. It compares script name, knob type class, dimension and default value per dimension (choices by option ID).
    - Live mode: when `getPluginBinary(id, ofxMajor)` has exactly that major, render both. If `NATRON_PARITY_RECORD_DIR` is set and `record`, write the OFX planes as `<dir>/<id>/<case>.f32`.
    - Replay mode: compare the native render against `NATRON_TESTS_FIXTURES_DIR/native-parity/<id>/<case>.f32`.
    - Neither available: `ADD_FAILURE()` naming what is missing.
    - Mismatches report the first differing (x, y, channel, ofx, native) and the max abs diff.
  - verify: `NativeParitySelf_Test` runs:
    - OFX Grade v2 against itself (as both "ofx" and "native") with a non-default `multiply`: diff 0, and knob parity clean;
    - the same pair with `gamma` changed on one node only: reports a mismatch (the test expects `!result.ok`);
    - a record into a temp dir then replay from it: diff 0;
    - an `.f32` round trip.

    Full ctest green.
  - size: L

- [x] M67.P1.T4 — `NativeImageEffect` base: point-op kernel, in-kernel unpremult/mask/mix/pass-through, host flags
  - files: `Engine/Nodes/Image/NativeImageEffect.h` (new), `Engine/Nodes/Image/NativeImageEffect.cpp` (new), `Engine/Nodes/Image/PixelKernel.h` (new), `Engine/Nodes/Image/ColorMath.h` (new), `Engine/Nodes/NativeEffectBase.h`, `Engine/Nodes/NativeEffectBase.cpp`, `Tests/Native/NativeImageEffect_Test.cpp` (new; it defines its test point op and registers it through a test-local fixture `SetUp` helper if `wmain.cpp` registration is unavoidable. If so, `wmain.cpp` is this task's, and P1.T3 has finished with it in the previous batch)
  - approach:
    - `NativeEffectBase`:
      - Add `bool isMask` to `NativeInputDescription` and implement `isInputMask()` from it.
      - Make `grouping` a `/`-path that `getPluginGrouping` splits.
    - `NativeImageEffect : NativeEffectBase` gets the flags of Design §2: tiles, multi-resolution, render scale Yes, float only, `FullySafeFrame`, `rendersUnprocessedChannels()` true, and `wantsHostUnPremultSelector()` from a constructor flag. It also has:
      - `virtual bool isPointOp() const`.
      - `virtual PixelKernelPtr makeKernel(const KernelContext&)`. `KernelContext` holds time, view, mappedScale and the processChannels bitset.
      - `getLayerKnobSpec` (a generator flag gives LayerSelect, Target, with buttons).
      - `isHostChannelSelectorSupported` from a protected default quad.
      - `defaultProcessesAllLayers` from a description flag.
    - `PixelKernel` is described in Design §5: `processRow(const RowIO&)`. `RowIO` holds the source row pointers per input, the mask row (or null), the divisor row and its channel (or null), dst, x0, y, width, nComps and the `std::bitset<4>` channels in colour-bit space.
    - The point-op `render()`:
      - For each `(layer, image)` in `args.outputLayers`, resolve the input plane with `resolveInputPlaneForRender` (P1.T2).
      - Fetch the source, the mask (when the mask input is connected and enabled) and the `hostUnPremultBy` divisor plane **before** locking any image (`RemoveLayers.cpp:353` rule).
      - Run, per row of `args.roi`: divide → kernel op → multiply → `maskInvert`/`mix` against the undivided source → copy unprocessed channels. Use the P1.T2 inline divide/multiply helpers. Map nComps 1/3/4 to colour bits with the M65 helpers.
      - Check `aborted()` per row band.
    - `getComponentsNeededAndProduced` and `getRegionsOfInterest` add the mask and divisor layers and windows, so the M63 request pass plans them.
    - `getPreferredMetadata` keeps the input's components.
    - The mask/mix stage reads knobs named `maskInvert`/`mix`, which a node declares through `addMaskMixKnobs(page)` (OFX defaults, range 0..1).
    - The base's `isIdentity` tail implements the OFX rules: `mix == 0` → input 0; a non-inverted connected mask whose RoD misses the window → input 0.
    - `ColorMath.h`: `enum LuminanceMath {Rec709, Rec2020, AcesAP0, AcesAP1, Ccir601, Average, Max}`, with the coefficients from `ofxsLut.h:955-1105` (correct AP1). It also has `addLuminanceMathKnob(page)` with the OFX option IDs, labels and default 0, and the Rec.709/sRGB delinearise helpers that Constant/CheckerBoard/ChromaKeyer need.
  - verify: `NativeImageEffect_Test` with a test op `out = 2·in` on the P1.T3 source covers:
    - process-in-place on `rgb` leaves A bit-identical;
    - an alpha-only input processes A;
    - `hostUnPremultBy` = `rgba.A` divides and multiplies back, and equals `Image::unPremultiplyByChannel` + op + `premultiplyByChannel` on the same pixels;
    - mask + `mix` 0.5 + `maskInvert`;
    - the `mix == 0` identity;
    - `renderBothWaysDirect` at pool sizes 1 and 4 is bit-exact with 0 unplanned pulls (mask and divisor connected);
    - a three-layer input with `channels` = all renders every plane.

    Full ctest green.
  - size: L

- [x] M67.P1.T5 — Native Grade (`net.sf.openfx.GradePlugin` 3.0)
  - files: `Engine/Nodes/Color/Grade.h` (new), `Engine/Nodes/Color/Grade.cpp` (new), `Engine/AppManager.cpp`, `Tests/Native/NativeGrade_Test.cpp` (new)
  - approach:
    - Inputs: `Source`, then `Mask` (optional, `isMask`). Components RGBA, RGB, Alpha. Grouping `Color`.
    - Host defaults: quad R, G, B on and A off. `hostUnPremultBy` on.
    - Knobs (page Controls, OFX order, ranges and display ranges from `Grade.cpp`):
      - `blackPoint` 0, `whitePoint` 1, `black` (label Lift) 0, `white` (Gain) 1, `multiply` 1, `offset` 0, `gamma` 1. All are 4-D colour, with the same default on all four channels.
      - `normalize` button. On press, fetch the source at the current frame and view, set `blackPoint`/`whitePoint` to the per-channel min/max inside one undo block, with the RGB/Alpha special cases of `Grade.cpp`.
      - `reverse` false, `clampBlack` **true**, `clampWhite` false.
      - Then `addMaskMixKnobs`.
    - Kernel, in double per channel, from `Grade.cpp:453-509`:
      - `A = d≠0 ? multiply·(white−black)/d : 0`, with `d = whitePoint−blackPoint`. `B = offset + black − A·blackPoint`.
      - Forward: `v = A·v + B`, then gamma, with the OFX special cases for gamma ≤ 0, gamma 1 and v ≤ 0.
      - Reverse: the `invgrade` order.
      - Clamp after the grade.
    - `isIdentity` follows `Grade.cpp:1127-1206`.
    - Register it in `AppManager.cpp` with an empty icon path. P2.T6, B5's registration owner, sets it to the Grade icon that P2.T1 adds.
  - verify: `NativeGrade_Test`:
    - knob parity with OFX Grade 2;
    - parity at mipmap 0 and 1 with the `pow` class tolerance on these cases: default (identity), multiply/offset, gamma 0.45 on negatives, reverse, clampWhite, an alpha-only source with A processed, `channels` = all on the three-layer source, mask + mix 0.5, and `hostUnPremultBy`. Cases 2, 4 and 8 are marked `record`.
    - `app.createNode("net.sf.openfx.GradePlugin")` gives the native node, and `createNode(id, 2)` gives the OFX node.
    - `renderBothWays` on CheckerBoard → 30 native Grades → writer is bit-exact.
    - `ChannelSetRender_Test`'s Grade cases now exercise the native Grade. Record which fail for P2.T7; do not fix them here.
  - size: M

- [x] M67.P1.T6 — Run the native-vs-OFX Grade chain benchmark and apply the go/no-go gate
  - files: `tools/bench/BASELINE.md`, this file's `## Decisions`
  - approach:
    - Build release at the batch tip, with the container held exclusively and no other build running (`pgrep -x ninja` is 0).
    - Run `tools/bench/native_vs_ofx.sh m67gate 3`: three ABAB rounds, each with tiny `chain:0,1000` (5 frames) and HD `chain:30,100` (3 frames).
    - Per round compute:
      - **mem** = (rss_before(native,1000) − rss_before(native,0)) / (rss_before(ofx,1000) − rss_before(ofx,0));
      - **build** = build_s(native,1000) / build_s(ofx,1000);
      - **tiny** = frame_wall_med(native,1000) / frame_wall_med(ofx,1000);
      - **hd** = [f(native,100) − f(native,30)] / [f(ofx,100) − f(ofx,30)], with f the median frame wall.
    - The gate value is the median over rounds. If any ratio's spread across rounds exceeds 0.15, run two more rounds and take the median of five.
    - Record the per-round tables, the medians, load and PSI per config, and the KB/node and ms/node. Give absolute values only if the run was quiet (Design §9), and label the section "M67 gate (contended)" otherwise.
  - verify: **Gate.**
    - **GO** if all of these hold:
      - mem ≤ **0.50** (the decision predicts ~0.2);
      - hd ≤ **1.05** (native must not be slower per HD node);
      - build ≤ **1.15** and tiny ≤ **1.15**.
    - **NO-GO** if mem > 0.50 or hd > 1.05: stop the milestone, set M67 to `blocked`, report the table and stacks to the user, and start no family phase.
    - If only build or tiny exceed 1.15, record it and add a task in Phase 67.2 to locate the per-node cost before P2's nodes land.
    - The outcome goes into `## Decisions`.
  - size: M

## Phase 67.2: Colour family (ColorCorrect, Saturation, Clamp, Invert, Add/Multiply/Gamma)

Shared rules for every node task in this phase:
- Each node is an `Engine/Nodes/Color/<Name>.{h,cpp}` on `NativeImageEffect` with `isPointOp()` true.
- Inputs: `Source`, then `Mask` (`isMask`).
- `hostUnPremultBy` on. Knobs come from the openfx-misc source named in the brief, in OFX order, with OFX defaults, ranges and display ranges, then `addMaskMixKnobs`.
- The quad default is the OFX `NatronOfxParamProcess*` defaults.
- Tests: `Tests/Native/Native<Name>_Test.cpp`, with knob parity, parity at mipmap 0 and 1 (≥5 cases, 2–3 marked `record`), a `renderBothWaysDirect` check that is bit-exact with 0 unplanned pulls, and the identity conditions.
- Registration is done only by the batch's registration owner (see the batch plan).

- [x] M67.P2.T1 — Copy the replaced plugins' icons into Natron's resources
  - files: `Gui/Resources/Images/NativeNodes/` (new PNGs), `Gui/GuiResources.qrc`
  - approach:
    - Copy, from `build/assets/plugin-src/openfx-misc`, the `net.sf.openfx.*.png` icons of Grade, ColorCorrect, Saturation, Clamp, Invert, Add, Multiply, Gamma, Merge and its nine sibling PNGs that exist, Dissolve, Constant, Solid, CheckerBoard, Transform, TransformMasked, Crop, Reformat, Position, Keyer, ChromaKeyer and ColorLookup. Keep their file names.
    - CImg plugins ship no PNG. Leave those three empty.
    - Add one `<file>` line per PNG to `GuiResources.qrc` under the `Resources/Images/NativeNodes/` prefix path. The icon path a node passes is `NATRON_IMAGES_PATH "NativeNodes/<file>.png"`.
  - verify: `ls Gui/Resources/Images/NativeNodes | wc -l` matches the qrc lines, and the batch build compiles the qrc.
  - size: S

- [x] M67.P2.T2 — Native ColorCorrect (`net.sf.openfx.ColorCorrectPlugin` 3.0)
  - files: `Engine/Nodes/Color/ColorCorrect.h`, `Engine/Nodes/Color/ColorCorrect.cpp`, `Tests/Native/NativeColorCorrect_Test.cpp`
  - approach:
    - Components RGBA, RGB, Alpha. Quad RGB on, A off.
    - Groups `Master`, `Shadows`, `Midtones`, `Highlights`. Script names are group + suffix: `Saturation`, `Contrast`, `Gamma`, `Gain`, `Offset`, all 4-D with defaults 1, 1, 1, 1, 0. The non-master groups also have `<Group>Enable` true.
    - Page `Ranges`:
      - `range` 2-D (0, 1), swapped on user edit if max < min.
      - `toneRanges`: a native `KnobParametric` with 2 curves ("Shadow", "Highlight"), range 0..1 and the OFX UI colours. Default points (0,1),(0.09,0) and (0.5,0),(1,1), added with `eKeyframeTypeHorizontal`. That is what `OfxParamInstance.cpp:4869` gives the OFX version, so the shapes match.
    - `luminanceMath` (`ColorMath.h`), `clampBlack` true, `clampWhite` false.
    - Kernel per `ColorCorrect.cpp:222-378,681-765`:
      - The luminance of the unpremultiplied input selects the weights s, h and m = 1−s−h through a 1024+1-entry tone LUT over `range`. The LUT is built in `makeKernel`. Outside the range, the curve is evaluated directly and clamped.
      - `out = s·S + m·M + h·H` of `applyGroup` per group, then `applyGroup(master)`.
      - Each group applies saturation (RGB only, l recomputed), then contrast (`pow(v/0.18, c)·0.18` for v>0, all four channels), then gamma, gain and offset.
      - Then the else-if clamp.
    - `isIdentity` follows the OFX rule.
  - verify: As in the phase rules. Cases include each group alone, a disabled group, a non-default `range`, every `luminanceMath`, and an edited `toneRanges` point (set on both nodes through `KnobParametric` API). Knob parity includes the parametric knob's default control points.
  - size: L

- [x] M67.P2.T3 — Native Saturation (`net.sf.openfx.SaturationPlugin` 3.0)
  - files: `Engine/Nodes/Color/Saturation.h`, `Engine/Nodes/Color/Saturation.cpp`, `Tests/Native/NativeSaturation_Test.cpp`
  - approach:
    - Components RGBA and RGB only, as in OFX. Quad RGB on, A off.
    - Knobs: `saturation` 1 (range 0..DBL_MAX, display 0..4), `luminanceMath`, `clampBlack` true, `clampWhite` false.
    - Kernel: `c' = (1−sat)·l + sat·c` on processed R, G and B, in double. Alpha is only clamped.
    - **Divergence:** ACES AP1 uses the correct AP1 coefficients. The OFX case falls through to CCIR 601 at `Saturation.cpp:222-225`. Exclude the AP1 case from parity, and add a native-only expected-value case.
  - verify: As in the phase rules, including every other `luminanceMath`, saturation 0 and 2.5, and the clamps.
  - size: M

- [x] M67.P2.T4 — Native Clamp (`net.sf.openfx.Clamp` 3.0)
  - files: `Engine/Nodes/Color/Clamp.h`, `Engine/Nodes/Color/Clamp.cpp`, `Tests/Native/NativeClamp_Test.cpp`
  - approach:
    - Components RGBA, RGB, Alpha, XY. Quad all four on.
    - Knobs: `minimum` (0,0,0,0), `minimumEnable` true, `maximum` (1,1,1,1), `maximumEnable` true, `minClampTo` (0,…), `minClampToEnable` false, `maxClampTo` (1,…), `maxClampToEnable` false. The colour knobs have no range set.
    - Kernel per `Clamp.cpp:416-430` on unpremultiplied values.
    - Identity: no channel processed, or both enables off.
  - verify: As in the phase rules, at the exact tolerance class.
  - size: M

- [x] M67.P2.T5 — Native Invert (`net.sf.openfx.Invert` 3.0)
  - files: `Engine/Nodes/Color/Invert.h`, `Engine/Nodes/Color/Invert.cpp`, `Tests/Native/NativeInvert_Test.cpp`
  - approach:
    - Components RGBA, RGB, Alpha, XY. Quad all on.
    - No value knob: `out = 1 − in` on processed unpremultiplied channels, unclamped.
    - Identity when no channel is processed.
  - verify: As in the phase rules, at the exact class, including values outside 0..1 and `hostUnPremultBy`.
  - size: M

- [x] M67.P2.T6 — Native ColorMath: Add, Multiply, Gamma (three IDs, one class)
  - files: `Engine/Nodes/Color/ColorMathNode.h`, `Engine/Nodes/Color/ColorMathNode.cpp`, `Tests/Native/NativeColorMath_Test.cpp`, `Engine/AppManager.cpp` (B5's registration owner). The node files are not named `ColorMath.*`, which would clash with `Engine/Nodes/Image/ColorMath.h`.
  - approach:
    - One class `ColorMathNode` templated or parameterised by operation, registered as `net.sf.openfx.AddPlugin`, `MultiplyPlugin` and `GammaPlugin`, all 3.0. Grouping `Color/Math`, labels Add, Multiply, Gamma. Quad RGB on, A off.
    - Components: Add has RGBA, RGB and Alpha (no XY). Multiply and Gamma also have XY.
    - Knobs:
      - `value`: 4-D colour. Add defaults to (0,0,0,0) with range ±DBL_MAX. Multiply defaults to (1,1,1,1) with range ±DBL_MAX. Gamma defaults to (1,1,1,1) with range 0..DBL_MAX. Display 0..4 on all three.
      - Gamma also has `invert` (false, not animated).
    - Kernels:
      - Add: `v + value`. Multiply: `v · value`.
      - Gamma: `e = invert ? value : 1/max(1e-8, value)`; if v ≤ 0 pass through, else `pow(v, e)`.
    - Identity: every processed channel's value is exactly neutral. Gamma ignores `invert`, as OFX does.
  - verify: As in the phase rules, for all three IDs. Knob parity is run per ID.
  - size: M

- [x] M67.P2.T7 — Colour-family test triage: make the suite pass on the native nodes without OFX-specific assumptions
  - files: the tests that the P2 batch build fails, expected to be `Tests/ChannelSetRender_Test.cpp`, `Tests/DefaultChannelSet_Test.cpp`, `Tests/LayerKnobs_Test.cpp`, `Tests/Metadata_Test.cpp` and `Tests/LayerKnobsRender_Test.cpp`. Five files at most; split the task if more fail.
  - approach:
    - The batch build's ctest list is the input.
    - For each failure, decide which of these it is:
      - (a) a native defect. Fix it in the node and note it, keeping this task's file budget by handing larger fixes back to the PM as a new task;
      - (b) an assertion about OFX internals, such as the hidden `NatronOfxParamProcess*` quad (`ChannelSetRender_Test.cpp:1293`) or the hidden plug-in `unPremultBy` (`:701`). Re-point it at a plugin that stays OFX: KeyMix, HSVTool or ColorMatrix, whichever exercises the same host path;
      - (c) a legacy-fixture test whose `.ntp` names a now-native ID. Under the clean-break rule, re-point it at a still-OFX plugin, or delete it if it only tested OFX→host migration. Never add compatibility code.
    - Expected-value assertions that hold for the native node stay unchanged. They are free acceptance tests.
  - verify: Full ctest green in both scheduler modes. Each changed test has a one-line reason in the commit message.
  - size: M

- [x] M67.P2.T8 — Colour-family GUI check under Xvfb, OFX and native side by side
  - files: `build/m67-gui/run-gui.sh` (new, untracked; copy of `build/m63-gui/run-gui.sh`), `build/m67-gui/panels_check.py` (new, untracked)
  - approach:
    - Release build, container exclusive, `checkForUpdates=false` pre-seeded (`run-gui.sh` checks it). `NATRON_M67_FAMILY=colour`.
    - For each ID of the family, create `app.createNode(id, ofxMajor)` and `app.createNode(id)` on a CheckerBoard. Open both panels and set one non-default knob on each.
    - Save `grabWindow(0)` screenshots to `build/m67-gui/colour-<label>-{ofx,native}.png`: the panels, the viewer on the native node, the ColorCorrect `Ranges` page with the curve widget, and a masked Grade with `maskChannel_Mask` visible.
    - Write `build/m67-gui/results-colour.txt` listing each panel's knob names in display order for both versions.
    - Check against M38: layer knob row 0 with separator, own knobs in OFX order, mask selectors, `hostUnPremultBy`, `maskInvert`/`mix` last.
  - verify: Natron exits 0. The results file shows identical knob order for each pair, apart from the documented hidden OFX knobs. The screenshots are listed in this file's `## Decisions` and in `build/appimages/M67-uat.md` (P6.T4) for the user's approval at the parcel UAT.
  - size: M

- [x] M67.P2.T9 — Record the colour family's parity references from the OFX plugins
  - files: `Tests/fixtures/native-parity/<id>/*.f32` (new, generated)
  - approach:
    - PM-run, container exclusive, on the debug build of the P2.T7 batch **before** P2.T10's pin bump. Run `NATRON_PARITY_RECORD_DIR=$PWD/Tests/fixtures/native-parity tools/ci/local/test.sh debug -R 'Native(Grade|ColorCorrect|Saturation|Clamp|Invert|ColorMath)'`, adapting the flag to `test.sh`'s filter syntax.
    - Commit only the files written, with the size total in the message. The total for the family must stay under 600 KB.
  - verify: `git status` shows only `.f32` files under the eight IDs, and the same ctest filter passes again.
  - size: S

- [x] M67.P2.T10 — Retire the colour family's OFX plugins (fork PR, pin, smoke test, host lists)
  - files: `tools/ci/local/fetch-assets.sh`, `tools/ci/smoke_test.py`, `Engine/OfxEffectInstance.cpp`, `Tests/Native/NativePluginList_Test.cpp` (new), plus in the fork `charlesangus/openfx-misc`: `CMakeLists.txt`
  - approach:
    - Fork:
      - Create branch `m67/retire-native-core` from the current pin `3060fe33`.
      - Remove `Grade/`, `ColorCorrect/`, `Saturation/`, `Clamp/`, `Invert/`, `Add/`, `Multiply/` and `Gamma/` from `MISC_SOURCES` and their PNG/SVG from `MISC_RESOURCES`.
      - Commit "M67: retire the colour family (now native in Natron)" and open the PR, or append to it if it exists. State in the PR body that it merges with Natron's M67 PR.
    - `fetch-assets.sh`:
      - Pin `OPENFX_MISC_REF` to the branch head.
      - Add a comment line naming the retired plugins and that the native ones take their IDs, in the style of the Shuffle comment.
    - `smoke_test.py`:
      - Replace all three Misc representatives with IDs that stay OFX to the end of the milestone: `net.sf.openfx.Premult`, `net.sf.openfx.HSVToolPlugin`, `net.sf.openfx.switchPlugin`. Native Constant and Merge still enumerate under their IDs, but they no longer prove the Misc bundle loads.
      - Make `check_misc_effect_render` render through one still-OFX Misc colour plugin with a closed-form result (e.g. ColorMatrix scaling R), keeping the native Constant→Grade check as a second case.
    - `OfxEffectInstance.cpp`: drop the dead IDs from the hard-coded lists. For this family that is only `defaultProcessesAllLayers`'s comments and sets, if any are present.
    - `NativePluginList_Test`: for each retired ID, `getPluginsList()[id]` has exactly one version, its major is the native one, and a created node's effect is a `NativeEffectBase`. Later families append IDs to its table.
  - verify:
    - After `fetch-assets.sh` rebuilds the plugins (PM, container), `verify_plugin_loads` passes on Misc.
    - Full ctest is green, with the parity tests for the colour family running in replay mode.
    - `tools/ci/local/test.sh smoke debug` passes.
    - `NativePluginList_Test` passes.
  - size: M

## Phase 67.3: Merge and generators (Merge, Dissolve, Constant, CheckerBoard)

- [x] M67.P3.T1 — Rectangle host overlay for Crop and generator extents
  - files: `Engine/HostOverlaySupport.h`, `Engine/HostOverlaySupport.cpp`, `Engine/NodeOverlay.cpp`, `Engine/Node.h`, `Gui/HostOverlay.cpp`
  - approach:
    - Add `HostOverlayKnobsRectangle` with knobs `bottomLeft` and `size`, an optional `interactive` bool and an optional enable bool. Add `Node::addRectangleInteract(bottomLeft, size, interactive, enable)`, mirroring `addPositionInteract` (`NodeOverlay.cpp:295`), and a `RectangleInteract` in `Gui/HostOverlay.cpp` next to `PositionInteract` (`:257`).
    - Port the drawing and pen behaviour of openfx-misc `SupportExt/ofxsRectangleInteract.cpp`: corner, edge and centre handles, the drag-in-canonical-coordinates rule, and the hidden or disabled state when the enable knob is off.
  - verify:
    - A gtest in `Tests/Native/RectangleOverlay_Test.cpp` checks that a test effect calling `addRectangleInteract` in GUI-less mode stores the overlay knobs (the `nativeOverlays` path).
    - The interaction is checked in P3.T7's GUI run: a drag on the generator rectangle changes `size`.
  - size: L

- [x] M67.P3.T2 — Native generator base and Constant/Solid (`net.sf.openfx.ConstantPlugin` 2.0, `net.sf.openfx.Solid` 2.0)
  - files: `Engine/Nodes/Image/NativeGenerator.h` (new), `Engine/Nodes/Image/NativeGenerator.cpp` (new), `Engine/Nodes/Generator/Constant.h` (new), `Engine/Nodes/Generator/Constant.cpp` (new), `Tests/Native/NativeConstant_Test.cpp` (new)
  - approach:
    - `NativeGenerator : NativeImageEffect` uses the generator flag: LayerSelect, Target, with channel buttons, and no hidden `outputComponents`. The layer knob owns that, as `adoptChannelQuad` does for OFX generators.
    - It reproduces the `ofxsGenerator` knobs exactly:
      - `extent`: Format/Size/Project/Default, IDs `format`/`size`/`project`/`default`.
      - `recenter`, `reformat`.
      - `NatronParamFormatChoice`: the 20 formats in OFX order.
      - `NatronParamFormatSize`/`NatronParamFormatPar`: secret.
      - `bottomLeft`/`size`: normalised defaults, i.e. the project extent.
      - `interactive`, `hidpi`, `frameRange` (1,1).
    - Visibility rules, RoD per extent, clip preferences (PAR, output format) and the time domain all follow `ofxsGenerator.cpp`.
    - Size extent calls `addRectangleInteract` (P3.T1).
    - The source is optional, and there is no input in the description.
    - Constant:
      - `color` 4-D (0,0,0,0), range ±DBL_MAX, display 0..1, animated.
      - Solid: `color` 3-D (0,0,0) and alpha 1.
      - Writes the colour into the target layer's selected channels. Float output is unclamped.
      - The frame-varying flag is set only when `color` is animated.
  - verify:
    - Knob parity with Constant 1 and Solid 1.
    - Parity at the exact class for every extent, at mipmap 0 and 1, and for a non-zero `bottomLeft`.
    - RoD equality for every extent and for a project format change.
    - `GeneratorLayer_Test` cases with Constant pass unchanged.
    - `renderBothWaysDirect` bit-exact.
  - size: L

- [x] M67.P3.T3 — Native CheckerBoard (`net.sf.openfx.CheckerBoardPlugin` 2.0)
  - files: `Engine/Nodes/Generator/CheckerBoard.h`, `Engine/Nodes/Generator/CheckerBoard.cpp`, `Tests/Native/NativeCheckerBoard_Test.cpp`
  - approach:
    - On `NativeGenerator`. Components RGBA, RGB, Alpha.
    - Knobs, all animated:
      - `boxSize` (64,64), range 1..DBL_MAX.
      - `color0` (0.1,0.1,0.1,1), `color1` (0.5,0.5,0.5,1), `color2` (0.1,0.1,0.1,1), `color3` (0.5,0.5,0.5,1).
      - `lineColor` (1,1,1,1), `lineWidth` 0.
      - `centerlineColor` (1,1,0,1), `centerlineWidth` 1.
    - Pixel maths exactly per `CheckerBoard.cpp:150-341`: pixel-space box, centre from the RoD (or the project for Default), the ±0.25 line thresholds, precedence centreline > line > box parity.
    - `isPointOp()` false: the kernel depends on (x, y), but it still uses the row kernel interface with x0/y.
  - verify:
    - Knob parity.
    - Parity at the exact class at mipmap 0, 1 and 2, with lineWidth 0 and 2, a PAR-2 project format, and an animated `color0`.
    - `graph_bench.py`'s chain source now runs native. Re-run `BENCH_DRY_RUN` and add the CheckerBoard row to the `BENCH_IMPL` table so P6.T1 can bench OFX-source chains.
  - size: M

- [x] M67.P3.T4 — Merge operator library (pure functions, all 39 operators)
  - files: `Engine/Nodes/Merge/MergeOperators.h` (new), `Engine/Nodes/Merge/MergeOperators.cpp` (new), `Tests/Native/MergeOperators_Test.cpp` (new)
  - approach:
    - Port `SupportExt/ofxsMerging.h` as plain float functions over premultiplied A, B, a and b:
      - the 39 operators in the OFX enum order (ATop … XOR), with the formulas and branch conditions from `ofxsMerging.h:719-1125`;
      - the non-separable Hue, Saturation, Color and Luminosity modes from `:1296-1425,1532-1618`;
      - `isMaskable(op)` (`:105-156`) and `isIdentityForBOnly(op)` (`:160-210`);
      - the operator option IDs and labels exactly as the OFX choice: lower-case, hyphenated.
    - Also `mergePixel(op, alphaMasking, A[4], B[4], nComps, out[4])` with the alpha-masking rule (`:1519-1741`).
  - verify:
    - `MergeOperators_Test` checks every operator on a grid of A, B, a and b values, including 0, 1, negatives and >1, against an independent table computed from the formulas in this plan's scout notes. Each case has its formula in a comment.
    - The four HSL modes are checked against values from the OFX plugin, rendered once by P3.T5's parity cases.
  - size: M

- [x] M67.P3.T5 — Native Merge and its nine presets (`net.sf.openfx.MergePlugin` 3.0 and `net.sf.openfx.Merge{Plus,Matte,Multiply,In,Out,Screen,Max,Min,Difference}` 3.0)
  - files: `Engine/Nodes/Merge/Merge.h` (new), `Engine/Nodes/Merge/Merge.cpp` (new), `Tests/Native/NativeMerge_Test.cpp` (new)
  - approach:
    - Inputs, keeping the OFX index order because scripts connect by index (`graph_bench.py:251`: 0=B, 1=A): `B`, `A`, `Mask` (`isMask`), `A2`..`A64`, all optional. Components RGBA, RGB, XY, Alpha. RGB is promoted to RGBA for output, as in OFX.
    - Show the extra A inputs the way the OFX Merge does today. Inspect `OfxEffectInstance.cpp:~1718` `setInputVisible`.
    - Knobs:
      - `Natron_sublabel` (`kNatronOfxParamStringSublabelName`, secret, non-persistent), holding the operator label. `NodeGui.cpp:334/3309` draws the operator icon from it.
      - `operation`: the per-preset default, Over for Merge.
      - `bbox`: `union`/`intersection`/`a`/`b`, default union.
      - `screenAlpha` (label "Alpha masking") false, enabled by `isMaskable`.
      - `AChannelsR/G/B/A`, `BChannelsR..A`, `OutputChannelsR..A`, all true, under label separators.
      - `maskInvert`, `mix`.
    - No `hostUnPremultBy`. The quad defaults to `rgba`, matching `DefaultChannelSet_Test.cpp:144`'s colour default.
    - Render per `Merge.cpp`:
      - Merge the first connected A over B (black if B is absent).
      - Fold each later A over the running result.
      - Without A: copy B when `isIdentityForBOnly(op)`, else run one pass with A=0.
      - A/B channel toggles zero those channels.
      - `OutputChannels` off → B's value.
      - Then `mix` and mask against B.
    - RoD per `bbox` (A means input `A` only). `isIdentity` follows the OFX order of rules.
    - `getRegionsOfInterest`: the output window on every connected input. Unconnected A inputs are not planned.
    - Register the nine presets as subclasses differing only in ID, label (Plus, Matte, …), grouping `Merge/Merges` and default operation.
    - The Roto internal tree (`RotoDrawableItem.cpp:261`, `RotoContext.cpp:4674`) now builds a native Merge. Its knobs `operation` and `maskInvert` keep working.
  - verify:
    - Knob parity for MergePlugin and each preset.
    - Parity for every operator at the matching tolerance class, with alpha masking on and off, A/B channel toggles, output channels off, three A inputs, B missing, mask + mix, each `bbox` (RoD equality), and mipmap 1.
    - `renderBothWays` on the `wide:16` and `comp:20` bench graphs built in-test: bit-exact, 0 unplanned pulls.
    - The Roto render tests (`ctest -R Roto`) pass.
  - size: L

- [x] M67.P3.T6 — Native Dissolve (`net.sf.openfx.DissolvePlugin` 2.0)
  - files: `Engine/Nodes/Merge/Dissolve.h`, `Engine/Nodes/Merge/Dissolve.cpp`, `Tests/Native/NativeDissolve_Test.cpp`
  - approach:
    - Inputs `0`..`63` (optional), then `Mask` (`isMask`). Components RGBA, RGB, XY, Alpha. General context only: there is no transition context in Natron.
    - Knobs: `which` 0. Its range is [0, 63], and its display range follows the highest connected input on connect, as `updateRange` does. `maskInvert`. **No `mix`.**
    - Maths: `prev = floor(which)`, `next = ceil(which)`, a linear blend, mask × blend.
    - RoI is empty for every input but prev and next, so only those two are planned. RoD and identity follow `Dissolve.cpp`.
    - `defaultProcessesAllLayers` true.
  - verify:
    - Knob parity, apart from `which`'s display range, which depends on connections.
    - Parity: which = 0, 0.3, 1, 2.5 with three inputs; a masked case; a missing input.
    - `renderBothWaysDirect` with 0 unplanned pulls at which = 0.3. The two inputs not chosen are never rendered (counting test effect).
  - size: M

- [x] M67.P3.T7 — Merge/generator test triage and GUI check
  - files: the failing tests from the P3 batch (≤5 files, as in P2.T7, expected `Tests/GeneratorLayer_Test.cpp`, `Tests/Metadata_Test.cpp` and scheduler suites using CheckerBoard), `build/m67-gui/panels_check.py` (untracked)
  - approach:
    - Triage by P2.T7's rules (a/b/c).
    - Then run P2.T8's GUI script with `NATRON_M67_FAMILY=merge` for Merge, the presets, Dissolve, Constant, Solid and CheckerBoard, OFX vs native, with these extra screenshots:
      - the Merge operator icon in the node graph after switching operation to `multiply`;
      - the A2/A3 inputs appearing;
      - the generator rectangle overlay in Size extent, before and after a scripted drag (`QTest` mouse events on the viewer).
  - verify: Full ctest green in both modes. The GUI results file shows matching knob order, and the rectangle drag changed `size`. Screenshots are listed for UAT.
  - size: M

- [x] M67.P3.T8 — Record parity references and retire the merge/generator OFX plugins
  - files: `Tests/fixtures/native-parity/…` (generated), `tools/ci/local/fetch-assets.sh`, `Engine/OfxEffectInstance.cpp`, `Tests/Native/NativePluginList_Test.cpp`, plus in the fork: `CMakeLists.txt`
  - approach:
    - Record first, PM-run on the pre-bump bundle, as in P2.T9: `-R 'Native(Merge|Dissolve|Constant|CheckerBoard)'`.
    - Fork commit "M67: retire merge and generators":
      - Remove `Merge/`, `Dissolve/`, `Constant/` and `CheckerBoard/` from `MISC_SOURCES` and `MISC_RESOURCES`. This drops MergeRoto and the Merge 1.0 compat factories along with them, by design.
    - Re-pin `OPENFX_MISC_REF`.
    - `OfxEffectInstance.cpp`: drop `net.sf.openfx.DissolvePlugin` from `processAllLayers`.
    - Append the 14 retired IDs (MergePlugin, the nine presets, Dissolve, Constant, Solid, CheckerBoard) to `NativePluginList_Test`.
  - verify: As in P2.T10: plugins rebuilt, full ctest green with replay parity, smoke passes, `NativePluginList_Test` passes.
  - size: M

## Phase 67.4: Spatial (Transform, Crop, Reformat, Position, Blur)

- [x] M67.P4.T1 — Shared resampler: the ten OFX filters, clamp, black outside, supersampling, motion-blur sampling
  - files: `Engine/Nodes/Image/Resampler.h` (new), `Engine/Nodes/Image/Resampler.cpp` (new), `Engine/Nodes/Image/TransformMath.h` (new), `Tests/Native/Resampler_Test.cpp` (new)
  - approach:
    - Port `SupportExt/ofxsFilter.h:53-340,661-826` as float functions:
      - Impulse, Box, Bilinear and Cubic (2-tap, never clamped).
      - Keys, Simon, Rifman and Mitchell (4-tap, clamped to [min, max] of the centre pair when `clamp` is on).
      - Parzen and Notch (4-tap, no clamp).
      - `black_outside` semantics: out-of-bounds taps give 0, otherwise coordinates clamp to the bounds.
      - `ofxsFilterInterpolate2DSuper`, which supersamples only when minifying, with bilinear supersamples up to 3^4 per axis.
    - Port the `ofxsTransform3x3.cpp` processor loop:
      - the pixel-centre mapping;
      - z ≤ 0 → 0;
      - motion blur: 1000 matrices over the shutter range, `maxIt = int(motionblur·40)`, `minsamples = max(13, maxIt/3)`, stratified sampling with van der Corput jitter, and an adaptive stop at `maxErr = motionblur/1000`.
    - Port the RoI and RoD helpers: the inverse-transformed window expanded by 0 / 0.5 / 1.5 px per filter class; the forward-transformed RoD, +1 px for `black_outside`; the infinite-RoD rules.
    - `TransformMath.h` holds the forward and inverse matrices (`ofxsMatrix2D` `:906-950`: center, translate, rotate, skew order, scale, amount with geometric scale) and the canonical↔pixel conversion with PAR and render scale.
    - Row-oriented so a later fusing pass can use it. No threading inside.
  - verify:
    - `Resampler_Test` checks each filter on a 1-D step and a 2-D impulse against values written from the OFX formulas: weights sum to 1, and the clamp behaviour.
    - A minification case triggers supersampling.
    - The RoI expansion amounts.
    - The motion-blur sample count for given `motionBlur`.
    - Matrix composition against hand-computed matrices for each skew order.
  - size: L

- [x] M67.P4.T2 — Native Transform and TransformMasked (`net.sf.openfx.TransformPlugin` 2.0, `net.sf.openfx.TransformMaskedPlugin` 2.0)
  - files: `Engine/Nodes/Transform/Transform.h` (new), `Engine/Nodes/Transform/Transform.cpp` (new), `Tests/Native/NativeTransform_Test.cpp` (new)
  - approach:
    - Inputs: `Source`, plus `Mask` (`isMask`) for the masked ID. Components RGBA, RGB, XY, Alpha. `defaultProcessesAllLayers` true. No `hostUnPremultBy`.
    - Knobs, script names as `Bezier.cpp:64-74` and `Transform.cpp` declare them:
      - `translate` (normalised (0,0)), `rotate` 0, `scale` (1,1), `uniform` false, `skewX` 0, `skewY` 0, `skewOrder` XY/YX, `transformAmount` 1, `center` (normalised (0.5,0.5)), `resetCenter`.
      - Secret: `transformCenterChanged`, `transformInteractOpen`, `srcClipChanged`.
      - `interactive` true, `hidpi`, `invert` false, `filter` (10 options, default cubic), `clamp` false, `black_outside` true.
      - `motionBlur` 0, `directionalBlur` false, `shutter` 0.5, `shutterOffset` (centered/start/end/custom, default start), `shutterCustomOffset` 0.
      - Masked ID only: `maskInvert`, `mix`.
    - Overlay through `Node::addTransformInteract` with these knobs.
    - Behaviour:
      - `changedClip`-equivalent: on the first connection, the centre resets to the source RoD centre unless `transformCenterChanged` is set.
      - Render through `Resampler`.
      - RoD and RoI through its helpers.
      - `isIdentity` per `Transform.cpp`: not identity with `clamp` on or motion blur active; `transformAmount == 0` or neutral parameters is identity.
    - Concatenation:
      - `getCanTransform()` true for the plain ID only.
      - `getTransform` returns the pixel matrix unless directional blur or motion blur is active and render quality is not draft.
      - `getInputsHoldingTransform` gives {0}. Render applies a concatenated input matrix handed back by `getImage(…, &transform)`, as the OFX Transform does through its `canTransform` clip.
    - Tracker (`TrackerContextPrivate.cpp:163`) and Roto (`RotoDrawableItem.cpp:196`) now create the native Transform. Their linked knobs must keep working.
    - Render scale and PAR handling follow `TransformMath`.
  - verify:
    - Knob parity for both IDs.
    - Parity at the resampling class for every filter, rotate 30 + scale 0.5 (minify), skew both orders, invert, `black_outside` off, `clamp` with Keys, mipmap 1, a PAR-2 format, and the masked mix 0.5.
    - Motion blur at the IIR/motion class, with shutter centered and start, and directional blur.
    - A Transform→Transform chain renders once through concatenation (counting the first Transform's renders = 0), and its output equals OFX Transform→Transform.
    - `Tracker_Test` and the Roto tests pass.
    - `renderBothWaysDirect` bit-exact.
  - size: L

- [x] M67.P4.T3 — Native Crop (`net.sf.openfx.CropPlugin` 2.0)
  - files: `Engine/Nodes/Transform/Crop.h`, `Engine/Nodes/Transform/Crop.cpp`, `Tests/Native/NativeCrop_Test.cpp`
  - approach:
    - Input `Source` (required). Components RGBA, RGB, XY, Alpha. `defaultProcessesAllLayers` true.
    - Knobs:
      - `rectangleInteractEnable` (secret, true).
      - The generator subset: `extent` default **Size**, `recenter`, the format knobs, `bottomLeft`, `size`, `interactive`, `hidpi`, `frameRange`. Reuse `NativeGenerator`'s knob builder from P3.T2 through a static helper, not inheritance.
      - `softness` 0 (range 0..1000), `reformat` false, `intersect` false, `blackOutside` false.
      - A user edit of `reformat` sets `blackOutside = !reformat`.
    - Rectangle overlay (P3.T1), disabled while `reformat` is on.
    - Crop rectangle, render with `rampSmooth` softness in canonical units (not scaled by render scale), RoD, RoI and clip preferences exactly per `Crop.cpp:361-472`. There is no isIdentity.
  - verify:
    - Knob parity.
    - Parity at the exact class (softness 0) and the resampling class (softness 20), with `blackOutside`, `reformat`, `intersect`, each extent and mipmap 1.
    - RoD equality.
    - `renderBothWaysDirect` bit-exact.
  - size: M

- [x] M67.P4.T4 — Native Reformat (`net.sf.openfx.Reformat` 3.0)
  - files: `Engine/Nodes/Transform/Reformat.h`, `Engine/Nodes/Transform/Reformat.cpp`, `Tests/Native/NativeReformat_Test.cpp`
  - approach:
    - Input `Source`. Components RGBA, RGB, XY, Alpha. `supportsMultipleClipPARs` true.
    - Knobs per `Reformat.cpp` (2.0 only, no compat):
      - `useRoD` false.
      - `reformatType` (`format`/`box`/`scale`/`project`, default project).
      - `NatronParamFormatChoice` (20 formats), with `NatronParamFormatSize` (200,200) and `NatronParamFormatPar` 1 secret.
      - `boxSize` (200,200), `boxFixed` false, `boxPar` 1.
      - `reformatScale` (1,1), `reformatScaleUniform` false.
      - `resize` (none/width/height/fit/fill/distort, default width), `reformatCentered` true.
      - `flip`, `flop`, `turn`, `preserveBB`: all false.
      - `filter` (cubic), `clamp` false, `black_outside` **false**.
    - The visibility table and `getBoxValues` write-back follow the OFX.
    - Output format, box per type, inverse mapping and RoD/RoI follow `Reformat.cpp:352-649`. Resampling goes through `Resampler`.
    - `getCanTransform` is true only while `preserveBB` is on.
    - Identity: no centre, flip, flop or turn, and resize None.
  - verify:
    - Knob parity.
    - Parity for each type and each resize mode, turn, flip and flop, and HD→PAL with PAR.
    - Output format and PAR equality via metadata.
    - `renderBothWaysDirect` bit-exact.
  - size: L

- [x] M67.P4.T5 — Native Position (`net.sf.openfx.Position` 2.0)
  - files: `Engine/Nodes/Transform/Position.h`, `Engine/Nodes/Transform/Position.cpp`, `Tests/Native/NativePosition_Test.cpp`
  - approach:
    - Input `Source`. Components RGBA, RGB, Alpha, XY. `defaultProcessesAllLayers` true.
    - Knobs: `translate` (normalised (0,0)) with a host position overlay (`setHasHostOverlayHandle`), and `interactive` false.
    - Integer shift `t = floor(translate·rs/par + 0.5)`, with y rounded down to even for field-both. Outside the shifted source is zero.
    - RoD and RoI are shifted. Identity when t = 0. No concatenation, as in OFX.
  - verify: Knob parity. Parity at the exact class for translate 0, (10.4, −3.6), mipmap 1 and PAR 2.
  - size: M

- [x] M67.P4.T6 — Separable IIR/FIR blur kernels ported from CImg (Van Vliet, Deriche, box, triangle, quadratic)
  - files: `Engine/Nodes/Filter/BlurKernels.h` (new), `Engine/Nodes/Filter/BlurKernels.cpp` (new), `Tests/Native/BlurKernels_Test.cpp` (new)
  - approach:
    - Port, as 1-D line filters over a float stride:
      - `CImg::vanvliet(sigma, order, boundary)`, the Gaussian default;
      - `CImg::deriche(sigma, order, boundary)`, the quasi-Gaussian;
      - `CImg::boxfilter(size, order, boundary, iter)` with iter 1/2/3 for box/triangle/quadratic.
    - Use the CImg version the openfx-misc pin bundles (`CImg/CImg.h`). CImg is CeCILL-C, which is GPL-compatible; credit it in the file header.
    - Boundary: black or nearest, passed as CImg passes a bool.
    - A pure line API, so `Blur` and `EdgeDetect` both use it.
  - verify: `BlurKernels_Test` compares each filter at sigma 0.5, 3 and 25, order 0 and 1, both boundaries, against the CImg reference called directly from the bundled `CImg.h` in the test. Test-only include of `build/assets/plugin-src/openfx-misc/CImg/CImg.h`, behind a `NATRON_TESTS_CIMG_DIR` define. Max abs diff ≤ 1e-6.
  - size: L

- [x] M67.P4.T7 — Native Blur (`net.sf.cimg.CImgBlur` 5.0)
  - files: `Engine/Nodes/Filter/Blur.h` (new), `Engine/Nodes/Filter/Blur.cpp` (new), `Tests/Native/NativeBlur_Test.cpp` (new)
  - approach:
    - Inputs `Source`, `Mask` (`isMask`). Components RGBA, RGB, XY, Alpha. `defaultProcessesAllLayers` true. Quad all on (v4 blurs alpha). `hostUnPremultBy` on.
    - Knobs per `CImgBlur.cpp`:
      - `size` 2-D (0,0), canonical XY, range 0..1000, display 0..100.
      - `uniform` false.
      - `orderX` 0, `orderY` 0.
      - `boundary` (`black`/`nearest`, default black).
      - `filter` (`quasigaussian`/`gaussian`/`box`/`triangle`/`quadratic`, default gaussian).
      - `expandRoD` true, `cropToFormat` true, `alphaThreshold` 0.
      - `maskInvert`, `mix`.
    - sigma = size·scale/2.4 (x divided by PAR). The RoI halo is `max(3, ceil(1.5·s)) + order` for the Gaussian family and `iter·(floor((s−1)/2)+1) + (order>0)` for the FIR filters. The RoD expansion and `cropToFormat` follow `CImgBlur.cpp:1637-1686`.
    - Identity per OFX (sigma < 0.1 or FIR size ≤ 1, order 0).
    - Derivative scaling by `renderScale^order`, and `alphaThreshold` after the blur.
    - **Determinism across hardware:**
      - `renderThreadSafety` is `eRenderSafetyFullySafe`, not `Frame`, so the host never slices the window. The IIR result depends on the window it runs over, as CImg's does on the RoI.
      - Inside `render`, run the horizontal pass with `parallelForOnGlobalPool` over whole rows and the vertical pass over whole column bands, so the result is identical at any pool size or core count.
      - Memory: one float buffer of window + halo per call.
    - Unpremult/mask/mix as in the base: the base helpers are called on the blurred buffer. `isPointOp()` is false.
  - verify:
    - Knob parity.
    - Parity at the IIR class against CImgBlur 4 at full-frame RoI: sizes 0.2 (identity), 3, 25 and (40, 5); each filter; order 1; nearest boundary; mipmap 1; `expandRoD` off; mask + mix.
    - `renderBothWays` at pool sizes 1, 4 and 16 is bit-exact.
    - The `ChannelSetRenderBlurTest` cases pass on the native Blur.
    - Roto feather/blur paths that create `PLUGINID_OFX_BLURCIMG` (`RotoDrawableItem.cpp:186`) pass `ctest -R Roto`.
  - size: L

- [x] M67.P4.T8 — Spatial test triage and GUI check
  - files: failing tests (≤5, per P2.T7's rules), `build/m67-gui/panels_check.py` (untracked)
  - approach:
    - Triage. Expect the scheduler suites' `mixed` graphs, `SchedulerEquivalence_Test`'s Transform→Transform case, and the Tracker and Roto suites.
    - GUI check with `NATRON_M67_FAMILY=spatial`, with these screenshots:
      - the Transform jack overlay, before and after a scripted drag of `translate`;
      - the Crop rectangle;
      - the Position handle;
      - the Reformat panel in each type showing the right knobs;
      - the Blur panel.
      - Each OFX vs native.
  - verify: Full ctest green in both modes. Overlay drags change the knobs. Screenshots are listed for UAT.
  - size: M

- [x] M67.P4.T9 — Record parity references and retire the spatial OFX plugins
  - files: `Tests/fixtures/native-parity/…` (generated), `tools/ci/local/fetch-assets.sh`, `tools/ci/smoke_test.py`, `Engine/OfxEffectInstance.cpp`, `Tests/Native/NativePluginList_Test.cpp`, plus in the fork: `CMakeLists.txt`, `Transform/Transform.cpp`, `CImg/Blur/CImgBlur.cpp`, `Reformat/Reformat.cpp`
  - approach:
    - Record first, PM-run, `-R 'Native(Transform|Crop|Reformat|Position|Blur)'`.
    - Fork commit "M67: retire the spatial nodes":
      - Remove `Crop/` and `Position/` from `MISC_SOURCES`/`RESOURCES`.
      - Remove the `Reformat/` directory. Its 1.1 compat factory goes with it.
      - In `Transform.cpp` delete the `p1`/`p2` statics and their `mRegisterPluginFactoryInstance` lines, keeping DirBlur `p3`, and drop the Transform/TransformMasked PNGs from resources.
      - In `CImgBlur.cpp` delete `oldp1` (CImgBlur 3.0) and `p1` (CImgBlur 4.0) and their registrations, keeping Laplacian, ChromaBlur, Bloom, ErodeBlur, Sharpen, Soften, EdgeExtend and EdgeDetect (EdgeDetect retires in P5).
    - Re-pin.
    - `smoke_test.py`: CImg representatives become `net.sf.cimg.CImgSharpen` and `CImgPlasma`.
    - `OfxEffectInstance.cpp`: drop the retired IDs from `processAllLayers`.
    - Append to `NativePluginList_Test`.
  - verify: As in P2.T10. Also `verify_plugin_loads` on CImg, DirBlur still created as OFX, and `PyPlugInstantiate_Test` green.
  - size: M

## Phase 67.5: Keying and misc (Keyer, ChromaKeyer, Erode/Dilate, EdgeDetect, ColorLookup)

- [x] M67.P5.T1 — Native Keyer (`net.sf.openfx.KeyerPlugin` 2.0)
  - files: `Engine/Nodes/Keyer/Keyer.h` (new), `Engine/Nodes/Keyer/Keyer.cpp` (new), `Tests/Native/NativeKeyer_Test.cpp` (new)
  - approach:
    - Inputs `Source` (RGBA, RGB), `InM` and `OutM` (`isMask`, optional), `Bg` (RGBA, RGB, optional). Output RGBA. Grouping `Keyer`. Colour default quad.
    - Knobs per `Keyer.cpp`:
      - `Natron_sublabel` "Luminance" (secret), set from `mode`.
      - `keyColor` RGB (0,0,0).
      - `mode` (`luminance`/`color`/`screen`/`none`).
      - `luminanceMath`.
      - `softnessLower` −0.5, `toleranceLower` 0, `center` 1, `toleranceUpper` 0, `softnessUpper` 0.5 (digits 5).
      - `despill` 1, `despillAngle` 120.
      - `show` (Intermediate/Premultiplied/Unpremultiplied/Composite), `sourceAlphaHandling` (`ignore`/`inside`/`normal`).
    - A user edit of `keyColor` or `mode` sets the thresholds as `changedParam` does.
    - Kernel per `Keyer.cpp:399-578`: Kfg by mode, the `key_bg` trapezoid, the in/out mask mixing, despill, premult by mode, the output by `show`, and composite over `Bg`.
    - The kernel reads four inputs, so a row kernel with several input pointers.
  - verify:
    - Knob parity.
    - Parity at the `pow` class for each mode × each `show`, with in/out masks, source-alpha handling, and Bg present and absent.
    - Picking `keyColor` through the knob updates the thresholds as OFX does: set on both, then compare the threshold values.
  - size: M

- [x] M67.P5.T2 — Native ChromaKeyer (`net.sf.openfx.ChromaKeyerPlugin` 2.0)
  - files: `Engine/Nodes/Keyer/ChromaKeyer.h`, `Engine/Nodes/Keyer/ChromaKeyer.cpp`, `Tests/Native/NativeChromaKeyer_Test.cpp`
  - approach:
    - Inputs as Keyer.
    - Knobs per `ChromaKeyer.cpp`:
      - `keyColor` (0,0,0).
      - `colorspace` (`ccir601`/`rec709`/`rec2020`, default rec709).
      - `linearProcessing` false.
      - `acceptanceAngle` 120, `suppressionAngle` 40.
      - `keyLift` 0, `keyGain` 1.
      - `show` default **Composite**.
      - `sourceAlphaHandling` (`ignore`/`insidemask`/`normal`).
    - Algorithm per `ChromaKeyer.cpp:250-615`: the Rec.709 OETF delinearisation (`ColorMath.h`), YPbPr with (Kr, Kb) per colourspace, the acceptance and suppression angles, the key processor and the output modes.
    - The OFX Composite path on an RGB source reads `srcPix[3]` out of bounds (unverified). Native reads alpha = 1 there. If parity differs on RGB+Composite, record it as a divergence.
  - verify: Knob parity. Parity at the `pow` class across colourspaces, linear on and off, the angle extremes (0, 180), keyGain 0, keyLift 1, every `show`, and masks.
  - size: M

- [x] M67.P5.T3 — Native Erode and Dilate (`net.sf.cimg.CImgErode` 3.0, `net.sf.cimg.CImgDilate` 3.0; one class)
  - files: `Engine/Nodes/Filter/ErodeDilate.h`, `Engine/Nodes/Filter/ErodeDilate.cpp`, `Tests/Native/NativeErodeDilate_Test.cpp`
  - approach:
    - Inputs `Source`, `Mask`. Components RGBA, RGB, XY, Alpha. Quad all on (A on). `defaultProcessesAllLayers` true. `hostUnPremultBy` on, defaulting to on for premultiplied sources as `defaultUnpremult` does. Check what the host selector supports; if it has no "auto" default, keep the selector at None and note it.
    - Knobs: `size` Int2D (1,1), range ±1000, display ±100. `expandRoD` true. `maskInvert`, `mix`.
    - Separable rectangular min/max (van Herk/Gil-Werman, O(1) per pixel) with width `floor(|s|·rs)·2+1` per axis. Positive size runs the node's own op and negative size runs the opposite, per `CImgErode.cpp:160-173`.
    - The boundary must match CImg's `erode`/`dilate`: Neumann (nearest) inside CImg. Verify against the bundled `CImg.h` in the test.
    - RoI `±ceil(|s|·rs)`. RoD expands only on the axes the OFX does.
    - Identity per OFX, including its negative-size quirk. Keep the quirk: it only affects identity, not pixels.
    - Threading as in P4.T7: whole rows, then whole column bands.
  - verify: Knob parity for both IDs. Parity at the exact class for sizes (1,1), (5,0), (−3,−3), mipmap 1, an alpha-only input, and the image edges. `renderBothWays` at pool sizes 1 and 4 is bit-exact.
  - size: M

- [x] M67.P5.T4 — Native EdgeDetect (`eu.cimg.EdgeDetect` 5.0)
  - files: `Engine/Nodes/Filter/EdgeDetect.h`, `Engine/Nodes/Filter/EdgeDetect.cpp`, `Tests/Native/NativeEdgeDetect_Test.cpp`
  - approach:
    - Inputs `Source`, `Mask`. Quad RGB on, A off. Keeps the colour layer (it is in `keepColorLayer`).
    - Knobs per `CImgBlur.cpp`'s EdgeDetect:
      - `filter` (`simple`/`sobel`/`rotinvariant`/`quasigaussian`/`gaussian`/`box`/`triangle`/`quadratic`, default gaussian).
      - `multiChannel` (`separate`/`rms`/`max`/`tensor`, default tensor).
      - `blurSize` 0, `erodeSize` 0, `nms` false, `expandRoD` true, `cropToFormat` true.
      - `maskInvert`, `mix`.
    - Algorithm per `CImgBlur.cpp:2110-2338`:
      - Gradients come from `BlurKernels` (P4.T6) as first-order derivative blurs with the unit-step scaling, or from the simple/Sobel/rotation-invariant finite differences after a Van Vliet pre-blur.
      - Then the channel combination (Di Zenzo tensor), the erode/dilate by `erodeSize` (reusing P5.T3's min/max), and NMS with the parabolic peak.
      - Keep the RMS-direction quirk only if parity requires it, and note it.
    - Boundary is nearest. Threading as in P4.T7.
    - The OFX RoI omits the erode size, which this node includes, so it is tile-correct. Parity is compared at full-frame RoI.
  - verify: Knob parity. Parity at the IIR class for each filter × each multiChannel mode, blurSize 3, erodeSize ±2 and nms on. `renderBothWays` at pool sizes 1 and 4 is bit-exact.
  - size: L

- [x] M67.P5.T5 — Native ColorLookup (`net.sf.openfx.ColorLookupPlugin` 2.0)
  - files: `Engine/Nodes/Color/ColorLookup.h`, `Engine/Nodes/Color/ColorLookup.cpp`, `Tests/Native/NativeColorLookup_Test.cpp`
  - approach:
    - Inputs `Source` (RGBA, RGB, XY, Alpha), `Mask`. Quad RGBA. `hostUnPremultBy` on.
    - Knobs per `ColorLookup.cpp`:
      - `hasBackgroundInteract` (secret) and `range` (0,1).
      - `lookupTable`: a native `KnobParametric` with 5 curves (master, red, green, blue, alpha), the OFX UI colours, and default points (0,0),(1,1) added with `eKeyframeTypeCubic`, as `OfxParamInstance.cpp:4871` gives the OFX version.
      - `showRamp` (secret), `backgroundDisplay` (`none`/`colorramp`/`histogram`, default colorramp), `updateHistogram`.
      - `source`/`target` (non-persistent), `setMaster`/`setRGB`/`setRGBA`/`setA` buttons that add control points as OFX does.
      - `masterCurveMode` (`standard`/`weightedstandard`/`filmlike`/`luminance`), `luminanceMath`, `clampBlack` false, `clampWhite` false.
      - `maskInvert`, `mix`.
    - Kernel:
      - 1024+1-entry tables per curve over `range`, built in `makeKernel`. Outside the range, evaluate the curve directly.
      - The four master modes as `ColorLookup.cpp:559-615`.
      - Alpha uses only its own curve.
    - Identity is only the mask-RoD rule, as in OFX.
  - verify: Knob parity, including the parametric defaults. Parity at the `pow` class for each master mode, an edited curve (same points set on both), a non-default `range`, values outside the range, the clamps, and an alpha-only input.
  - size: M

- [x] M67.P5.T6 — Curve background for native parametric knobs (ColorLookup's colour ramp and histogram)
  - files: `Engine/KnobTypes.h`, `Engine/KnobTypes.cpp`, `Gui/CurveWidget.cpp`, `Engine/Nodes/Color/ColorLookup.cpp`, `Tests/Native/ParametricBackground_Test.cpp` (new)
  - approach:
    - Add `KnobParametric::setBackgroundPainter(std::function<void(const ParametricBackgroundContext&)>)`. It is engine-side and GL-free, giving a list of coloured quads or polylines in curve space. `CurveWidget` draws it where it draws `getCustomInteract()` today (`CurveWidget.cpp:666-676`), when there is no OFX custom interact.
    - ColorLookup supplies:
      - the colour ramp (`colorramp`);
      - an RGB histogram of the source at the current frame, computed on `updateHistogram` press and on the GUI thread. It is stored as a non-persistent per-instance vector of 3×256 counts, freed when `backgroundDisplay` leaves `histogram`, so idle nodes stay small.
  - verify: `ParametricBackground_Test` checks that the painter returns the ramp quads and a histogram whose counts sum to the source pixel count. The visual result is checked in P5.T7's GUI run (ramp and histogram screenshots).
  - size: M

- [x] M67.P5.T7 — Keying/misc test triage and GUI check
  - files: failing tests (≤5, per P2.T7's rules), `build/m67-gui/panels_check.py` (untracked)
  - approach:
    - Triage.
    - GUI check with `NATRON_M67_FAMILY=keying`: Keyer and ChromaKeyer panels with `keyColor` picked from the viewer by ctrl-click (scripted); Erode, Dilate and EdgeDetect panels; ColorLookup with the colour ramp and the histogram. Each OFX vs native.
  - verify: Full ctest green in both modes. Screenshots are listed for UAT.
  - size: M

- [x] M67.P5.T8 — Record parity references and retire the keying/misc OFX plugins
  - files: `Tests/fixtures/native-parity/…` (generated), `tools/ci/local/fetch-assets.sh`, `Engine/OfxEffectInstance.cpp`, `Engine/OfxParamInstance.cpp`, `Tests/Native/NativePluginList_Test.cpp`, plus in the fork: `CMakeLists.txt`, `CImg/Blur/CImgBlur.cpp`
  - approach:
    - Record first, PM-run, `-R 'Native(Keyer|ChromaKeyer|ErodeDilate|EdgeDetect|ColorLookup)'`.
    - Fork commit "M67: retire keying and misc":
      - Remove `Keyer/`, `ChromaKeyer/` and `ColorLookup/` from `MISC_*`.
      - Remove `CImg/Erode/CImgErode.cpp` and `CImgDilate.cpp` from `CIMG_SOURCES`.
      - Delete EdgeDetect `p9` from `CImgBlur.cpp`.
    - Re-pin.
    - `OfxParamInstance.cpp:4869-4873`: drop the ColorCorrect and ColorLookup ID cases (dead now); keep TimeDissolve and Retime.
    - `OfxEffectInstance.cpp`: drop CImgErode, CImgDilate and EdgeDetect from the lists.
    - Append to `NativePluginList_Test`.
  - verify: As in P2.T10.
  - size: M

## Phase 67.6: Wrap-up: measurement, PyPlug audit, docs, AppImage

- [x] M67.P6.T1 — Bench the M67 tip against the M63 tip, interleaved in one run
  - files: `tools/bench/BASELINE.md`
  - approach:
    - Build the release of `ce575b4d2` in a worktree, `build/wt/m67-base`, inside the container mount. Its plugin set is `build/assets/Plugins.pre-m67`: a copy of the bundle at pin `3060fe33`, kept by the PM before P2.T10's first re-pin. Then build the M67 tip release.
    - Using `run_matrix.sh` with `BENCH_RENDERER_DIR`/`BENCH_PLUGIN_PATH` (P1.T1), interleave base and head ABAB for 2 rounds:
      - tiny `chain:1000 wide:1000 comp:300`;
      - HD `chain:30,100 mixed:100 wide:100 comp:100`;
      - `readchain:30 footagecomp:32` (plates from `make_plates.py`).
    - `compare.py --pair-field build base head` gives the ratios.
    - Record:
      - per-node idle RSS (chain:1000 minus chain:0);
      - HD marginal ms/node;
      - `mixed` and `footagecomp` (all-native now) frame-time ratios;
      - rss_peak;
      - load and PSI.
    - Write an "After M67" section: ratios always, absolute numbers only if quiet.
    - Add an "Input for M64" paragraph with the native HD per-node cost against the `stream_bench` bandwidth floor (~5 ms/node at HD on this host), restating whether M64's kill-gate premise has changed.
  - verify:
    - Every config exited 0.
    - mem and hd ratios are no worse than the P1.T6 gate medians by more than 0.1. If they are, add a finding to `## Decisions`.
    - `compare.py` output is pasted into the section.
  - size: M

- [x] M67.P6.T2 — PyPlug audit: every param the bundled PyPlugs set on a now-native node exists
  - files: `Tests/Native/PyPlugNativeParams_Test.cpp` (new)
  - approach:
    - Parse each bundled PyPlug in `NATRON_TESTS_PYPLUGS_DIR` (Glow, PIKColor, LightWrap, ZMask, DropShadow, EdgeBlur, AngleBlur, ZRemap, Fill, and any other that creates a retired ID). For each `createNode("<retired id>", …)` block, collect the `getParam("<name>")` names. Plain text parsing in C++ with `QRegularExpression`.
    - Instantiate the PyPlug, as `PyPlugInstantiate_Test` does.
    - Assert that each named param exists on the corresponding inner node, except names in a documented allow-list of OFX-internal names that the PyPlug guards with `if param is not None`: `unPremultBy`, `unPremultByChannel`, `premultChanged`, `NatronOfxParamProcess*`.
  - verify: The test passes. A missing name is listed with PyPlug, node and param. Each finding is fixed in the node (if it is a real knob) or in the allow-list (if it is OFX-internal), with the reason.
  - size: M

- [x] M67.P6.T3 — Document flat native nodes and the parity tolerances
  - files: `Engine/Nodes/README.md`
  - approach: Add a section "Flat image nodes (`NativeImageEffect`)" covering:
    - the point-op kernel contract and the fusion-readiness rules (Design §5);
    - the in-kernel unpremult/mask/mix order;
    - the host knobs a node gets and which script names it must keep;
    - the scheduler declaration rules (0 unplanned pulls);
    - the threading rule: host slicing for point ops, whole rows/columns for spatial filters;
    - the ID takeover with major + 1;
    - how to write a parity test;
    - the tolerance table (Design §6) with each node's class and any recorded divergence.
  - verify: Every node of this milestone appears in the table with the tolerance its test uses (cross-check by grepping the test constants). The comment policy checker passes on the file.
  - size: M

- [x] M67.P6.T4 — Package the release AppImage and write the UAT script
  - files: `build/appimages/M67-<sha>.AppImage` (artefact), `build/appimages/M67-uat.md` (untracked)
  - approach:
    - Build release at the milestone tip and run `tools/ci/local/package.sh release`. A debug AppImage cannot be packaged in the container. Copy the result to `build/appimages/M67-<sha>.AppImage`.
    - Run the launch check (`build/appimages/run-launch-check.sh`).
    - Write `M67-uat.md` with:
      - for each family: create each node from the Tab menu and its shortcut (G, C, M, T, B); check panel layout against the listed screenshots; check the M38 layer knob, mask and mix; the Transform jack, Crop and generator rectangles, and the Position handle;
      - load a PyPlug (DropShadow, Glow) and check that it renders;
      - Roto with feather and Tracker → Transform;
      - the expected numbers from P6.T1;
      - the list of deliberate divergences;
      - a note that projects saved with the OFX versions are not migrated (clean break).
    - The user's sign-off is deferred to the parcel UAT, together with M63 and M64.
  - verify: The AppImage launches under the launch check. The UAT file lists every screenshot from P2.T8, P3.T7, P4.T8 and P5.T7.
  - size: M

## Phase 67.7: Review round 1 and HD performance (user: fix in M67 before UAT)

Codex's review on PR #42 has 13 findings, and the user chose to fix the HD composite slowdown within M67. Both run as batch B13: parallel implementers with disjoint file sets, then one build, one rebench and one repackage.

- [x] M67.P7.T1 — TransformMasked correctness and the affine resampler fast path (findings 0, 1, 10)
  - files: `Engine/Nodes/Transform/Transform.{h,cpp}`, `Engine/Nodes/Image/Resampler.{h,cpp}`, `Tests/Native/NativeTransform_Test.cpp`, `Tests/Native/Resampler_Test.cpp`
  - size: L
- [x] M67.P7.T2 — Shared spatial-filter helpers, cancellable band parallelism, Blur gather, base comment (findings 3, 5, 9, 12)
  - files: `Engine/Nodes/Image/NativeImageEffect.{h,cpp}`, a new shared helper module under `Engine/Nodes/Filter/`, `Engine/Nodes/Filter/{Blur,ErodeDilate,EdgeDetect}.cpp`
  - size: L
- [x] M67.P7.T3 — Merge alpha-only toggles, Merge operator dispatch hoisted out of the pixel loop, CheckerBoard spans (findings 2, 7, 8)
  - files: `Engine/Nodes/Merge/{Merge,MergeOperators}.{h,cpp}`, `Engine/Nodes/Generator/CheckerBoard.cpp`, `Tests/Native/NativeMerge_Test.cpp`, `Tests/Native/MergeOperators_Test.cpp`
  - size: M
- [x] M67.P7.T4 — ColorCorrect: drop `ToneCurve` for `CurveSnapshot`, skip identity groups (findings 4, 11)
  - files: `Engine/Nodes/Color/ColorCorrect.{h,cpp}`
  - size: M
- [x] M67.P7.T5 — Share the extent knobs between `NativeGenerator` and Crop (finding 6)
  - files: `Engine/Nodes/Image/NativeGenerator.{h,cpp}`, `Engine/Nodes/Transform/Crop.{h,cpp}`, a new shared extent module
  - size: M
- [ ] M67.P7.T6 — Build, full ctest (both modes), per-node HD rebench against the M63 tip, profile anything still slower than OFX, repackage the AppImage
  - verify: full ctest green in both modes; every parity test still replays; HD mixed, wide and footagecomp ≤ 1.0x of base, or profiled with the reason recorded; AppImage launch check passes
  - size: L
- [ ] M67.P7.T7 — Reply on each review thread and close the round

## Batch plan

One detached debug build + ctest per batch (both scheduler modes where noted), in the single-tenant `natron-dev` container. Check `pgrep -x ninja` is 0 and use a fresh `.done` marker before relaunching. **Registration owner** means the only task in the batch allowed to edit `Engine/AppManager.cpp`; it adds the `#include` and `registerBuiltInPlugin` lines for every node of its batch, with the class and header names fixed in this plan. With `Tests/Native/*_Test.cpp` globbed after P1.T3, no later batch edits `Tests/CMakeLists.txt`.

| Batch | Tasks (parallel) | Hotspot owners | Then |
|---|---|---|---|
| B1 | P1.T1, P1.T2, P1.T3 | `Tests/CMakeLists.txt`, `Tests/wmain.cpp`: P1.T3 | debug build + ctest (both modes) |
| B2 | P1.T4 | `Tests/wmain.cpp` (if needed): P1.T4 | debug build + ctest |
| B3 | P1.T5 | AppManager: P1.T5 | debug build + ctest; release build |
| B4 | P1.T6 (bench, container exclusive) | — | **Gate**: GO → B5; NO-GO → stop |
| B5 | P2.T1, P2.T2, P2.T3, P2.T4, P2.T5, P2.T6 | AppManager: P2.T6 (registers ColorCorrect, Saturation, Clamp, Invert, Add/Multiply/Gamma, and sets Grade's icon); qrc: P2.T1 | debug build + ctest; release build |
| B6 | P2.T7, P2.T8, P3.T1 | — | debug build + ctest (both modes); then P2.T9 (PM-run record) |
| B7 | P2.T10, P3.T2, P3.T3, P3.T4 | AppManager: P3.T2 (Constant, Solid, CheckerBoard) | PM copies `build/assets/Plugins` to `Plugins.pre-m67` before re-running `fetch-assets.sh`; then debug build + ctest + smoke |
| B8 | P3.T5, P3.T6 | AppManager: P3.T5 (Merge, 9 presets, Dissolve) | debug build + ctest (both modes); no release build. P3.T7 triage and GUI check run on this build, then P3.T8's record step |
| B9 | P3.T8, P4.T1, P4.T3, P4.T5, P4.T6, then P4.T2, P4.T4, P4.T7 (second wave edits only, no build in between) | AppManager: P4.T3 in wave 1 (Crop), P4.T2 in wave 2 (Position, Transform, TransformMasked, Reformat, Blur); `Tests/CMakeLists.txt` define: P4.T6 | `fetch-assets.sh`; debug build + ctest (both modes) + smoke. P4.T8 triage and GUI check run on this build, then P4.T9's record step |
| B10 | P4.T9, P5.T1, P5.T2, P5.T3, P5.T4, P5.T5, then P5.T6 (second wave, after ColorLookup exists) | AppManager: P5.T1 (Keyer, ChromaKeyer, Erode, Dilate, EdgeDetect, ColorLookup); `NativePluginList_Test`: P4.T9 | `fetch-assets.sh`; debug build + ctest (both modes) + smoke. P5.T7 triage and GUI check run on this build, then P5.T8's record step |
| B11 | P5.T8, P6.T2, P6.T3 | — | `fetch-assets.sh`; debug build + ctest (both modes) + smoke |
| B12 | P6.T1, then P6.T4 | — | release builds (base and head), bench, package |

Notes:
- Within a batch, file sets are disjoint. The retirement task of family k runs in the same batch as family k+1's first nodes because it touches only `fetch-assets.sh`, `smoke_test.py`, `OfxEffectInstance.cpp`, `NativePluginList_Test.cpp` and the fork.
- P4.T6's test needs `NATRON_TESTS_CIMG_DIR`. Its one-line define goes into `Tests/CMakeLists.txt` as the only exception to the no-edit rule, owned by P4.T6 in B10.

**Verification gate:** All of the following must hold:
- P1.T6's gate passed and is recorded in `## Decisions`.
- Full ctest is green in Legacy and Task graph modes on the debug build at the tip, with the OFX plugins retired and every `Native*` parity test running in replay mode.
- `NativePluginList_Test` shows each of the 34 retired IDs (colour 8, merge/generators 14, spatial 6, keying/misc 6) with exactly one, native, version.
- `tools/ci/local/test.sh smoke debug` passes, with `verify_plugin_loads` on Misc and CImg.
- `PyPlugInstantiate_Test` and `PyPlugNativeParams_Test` pass.
- `renderBothWays` cases for every family are bit-exact with 0 unplanned pulls.
- The P6.T1 "After M67" section exists with in-run ratios.
- The fork PR `charlesangus/openfx-misc` `m67/retire-native-core` is open and pinned by its branch head, to be merged with this milestone's PR and re-pinned to the merge commit.
- `build/appimages/M67-<sha>.AppImage` launches.
- `build/appimages/M67-uat.md` lists every GUI screenshot for the user's parcel UAT.

## Decisions
- 2026-10-06 — **Scope (user decision):** M67 rewrites all four core-node families as native nodes, in this order:
  1. colour: Grade, ColorCorrect, Saturation, Clamp, Invert, Add/Multiply/Gamma;
  2. merge + generators: Merge, Dissolve, Constant, CheckerBoard;
  3. spatial: Blur, Transform, Crop, Reformat, Position;
  4. keying/misc: Keyer, ChromaKeyer, Erode/Dilate, EdgeDetect, ColorLookup.

  The first task is the native-vs-OFX chain benchmark, behind a go/no-go gate. The milestone is stacked on M63's #41 for one parcel UAT, with M64 parked. See `DECISIONS/2026-10-06-m67-core-node-families.md`.
- 2026-10-06 — **Elaboration (consultant), design calls:**
  - **IDs and versions:**
    - Native nodes take over the OFX IDs at OFX major + 1, so every unversioned or older request resolves to them. Side-by-side parity uses the OFX exact major until retirement, and replays recorded `.f32` references after it.
    - All OFX compat versions are retired with their IDs.
    - MergeRoto is dropped (unused). DirBlur and the other CImgBlur-family plugins stay OFX.
  - **Rendering model:**
    - Flat nodes are float-only and tile-capable. Point ops are `FullySafeFrame`, sliced by the host under the M63 budget. Spatial filters are `FullySafe` with whole-row and whole-column internal parallelism for determinism.
    - Unpremult, mask, mix and channel pass-through run in-kernel, with the host copy pass skipped.
    - Point ops expose an immutable `PixelKernel` for a later M64 fusing pass. No fusion here.
  - **Gate thresholds:** mem ≤ 0.50 and HD per-node ≤ 1.05 (hard); build and tiny ≤ 1.15 (soft). Each is the median of ≥3 ABAB rounds in one run, with load and PSI recorded.
  - **The stub's "OFX projects load onto native" acceptance is dropped** under the clean-break rule.
  - **Saturation:** the ACES AP1 fall-through bug is fixed rather than replicated.
  - **Plugin bundle:** one fork PR, `m67/retire-native-core`, takes one commit per family.
- 2026-10-06 — **User decisions on the elaboration's questions:** (1) native Saturation uses the real ACES AP1 coefficients rather than copying the OFX plugin's CCIR 601 bug, with its parity case exempt for that option; (2) keep P5.T6, the native ColorLookup ramp/histogram curve background; (3) the P1.T6 gate thresholds stand as proposed. Work runs in worktree `build/wt/m64` (directory name kept so its build dirs stay valid), on branch `milestone/m67-native-core-nodes`, off #41's tip `ce575b4d2`.
- 2026-10-06 — **B1 implemented (P1.T1–T3), not yet built.** Deviations: P1.T2 also edits `Engine/ImagePremult.cpp` and `Engine/Image.h` (shared premult helpers so both paths stay identical), the resolver is two non-const overloads, and the test plane is `specular`. P1.T3 puts NativeParity in `Tests/` (task list) rather than `Tests/Native/` (Design §6), writes one reference file per plane with `.mip<N>` in the name, and `makeParityPair` takes an `AppInstancePtr`.
- 2026-10-06 — **B1 landed** (`e5c5120ed`, `0c0fda301`, `209406e51` on `milestone/m67-native-core-nodes`): it compiled with no fixes, targeted tests passed 64/64, and full debug ctest passed 999/999 in both scheduler modes. Assets were re-fetched for the re-pinned forks.
- 2026-10-06 — **P1.T4/P1.T5 landed** (`593a878d6`, `0d6c0307c`): native-vs-host comparisons are bit-exact; Grade parity max diff is 0, except 2.98e-8 on multiply-offset at mipmap 0; both modes are bit-exact with 0 unplanned pulls. Deviations: divisor planning goes in the default `getComponentsNeededDefault` path (non-multiplanar effects never call the override), so OFX nodes using `hostUnPremultBy` on another layer get it planned too. **A pre-existing engine bug was fixed:** two paths cached a window-dependent identity answer computed on the project format (`getNearestNonIdentity`, `addIdentityNodesRecursively`), so those no longer cache. Traits are fixed in the subclass constructor rather than in `NativePluginDescription`. With unpremult and mask/mix both on, mix blends with the undivided source, which differs from OFX only where divisor ≤ ε. Six old tests that assume an unversioned Grade is OFX are being pinned to major 2 ahead of P2.T7.
- 2026-10-06 — **P1.T6 gate run 1: NO-GO on HD only** (`f4c6f890c`, harness labelled it contended: 3 of 24 configs started at load 0.54–0.70, no IO pressure; spreads ≤ 0.06). Native/OFX medians: mem 0.216, build 0.884, tiny 0.503, **HD 1.697** (29.9 → 50.7 ms/node). Old OFX-hosting tests are pinned to Grade major 2 (`c78748768`); full ctest 1020/1020 in both modes. **PM decision:** the family phases stay unstarted per the gate, but a slower point op is more likely an implementation defect (threading, per-pixel double maths, a source conversion copy) than a property of native nodes. A consultant is diagnosing and fixing it in the shared base, then the gate re-runs. Only if the gap can't be closed does M67 go `blocked`.
- 2026-10-06 — **P1.T6 gate: GO after a fix** (`m67gate2`, release; native configs all started quiet, 4 OFX configs at load 0.54–1.59). Medians: mem 0.216, build 0.871, tiny 0.498, **HD 0.700** (30.0 → 21.0 ms/node), spreads ≤ 0.037. Root cause of run 1's 1.70x: native point ops ran single-threaded (parallelism 1.0 vs OFX 3.0), because the host only fans out over several rects and its single-rect split is commented out (`EffectInstanceRenderRoI.cpp:1174`). **Fix and Design departure (accepted by PM):** `NativeImageEffect` splits its window into row bands on `parallelForOnGlobalPool` within the host thread budget (≥16384 px per band, ≤4 bands per thread) and declares `eRenderSafetyFullySafe`, like OFX Grade, rather than `FullySafeFrame`. Design §2/§9's host-slices-the-RoI model does not hold; every later node inherits the band split. Code `0e0cb4948`; ctest 1023/1023 in both modes; parity unchanged. The gate-2 table still needs appending to `BASELINE.md` (next batch).
- 2026-10-06 — **B5 landed** (`68c975b86`..`7fe48113f`): native ColorCorrect, Saturation, Clamp, Invert and Add/Multiply/Gamma, plus 29 icons. Targeted tests pass 107/107 and full ctest 1094/1094 in both modes. Max parity diff is 0 for Clamp/Invert/ColorMath, 5.96e-8 for Saturation and 2.38e-7 for ColorCorrect. Fixes: ColorCorrect's tone curves are evaluated lock-free (`ToneCurve` repeats `Curve::getValueAt` exactly) instead of a locked `Curve` copy per pixel; a `ColorMath.h` overload ambiguity. No old tests needed pinning (all pass on the native nodes), so P2.T7 has nothing failing to triage. Open gaps: XY planes are accepted but never rendered end to end; ColorMath identity checks all channels, not just processed ones (conservative). `Engine/Nodes` source glob lacks `CONFIGURE_DEPENDS`, so debug builds need `--reconfigure` after new files.
- 2026-10-06 — **P2.T7 closed with no change:** the B5 full ctest had no failures, so every old colour-node test now passes on the native nodes and none needed re-pointing.
- 2026-10-06 — **P2.T9 recorded** (`bf6b3a6dc`): 44 `.f32` files, 1.35 MB, replay passes 84/84. **PM decision: the per-family reference cap is raised from 600 KB to 1.5 MB** rather than cutting coverage, because the references are the only parity check left once the OFX plugins are retired. `test.sh` has no `-R` and doesn't forward `NATRON_PARITY_RECORD_DIR`; recording ran `ctest -R` directly (`build/m67-gui/record.sh`).
- 2026-10-06 — **P2.T8 GUI check run** (`build/m67-gui/`, Natron exit 0): knob order is identical for all 8 pairs apart from the 7 documented hidden OFX knobs, and the panels are pixel-identical except two native defects now being fixed: ColorCorrect's Shadows/Midtones/Highlights children are parented to the page rather than their group (so they never fold), and Clamp's colour knobs aren't folded to one value. P2.T8 stays open until a re-shoot after the fix. Screenshots go to the parcel UAT.
- 2026-10-06 — **B7 landed** (`a206d9191`..`0aaff24f7`): panel fixes re-shot and matching OFX (`build/m67-gui/colour-{colorcorrect,clamp}-native-fixed.png`, `gen-constant-native-overlay.png`); colour family retired via openfx-misc#7 (head `47293e94`, merges with M67's PR); rectangle overlay, generators (Constant/Solid/CheckerBoard parity 0 vs OFX major 1) and the Merge operator library landed. Full ctest passes 1093/1093 in both modes, and smoke passes. Engine fix: `Project::reset` left stale format-menu entries. **After retirement, unrecorded colour parity cases and the OFX knob-parity tests were deleted** (Design §6: about 3 recorded cases per node remain, 19 in total). OFX-hosting tests were re-pointed at ColorMatrix/Quantize, and OFX Constant tests are pinned to major 1 until P3.T7. Lesson for later families: decide which cases to keep **before** the record step, because unrecorded cases die at retirement. `run-gui.sh` deletes `colour-*.png`, so the B7 shots used `build/m67-b7/run-gui-b7.sh`.
- 2026-10-06 — **Batch plan consolidated (user request: fewer, slower builds).** B8–B17 become B8–B12: from five builds for the spatial and keying families down to two, and no per-batch release builds (the only release builds are B12's bench and package). Each family's triage and GUI check now run on that family's implementation build instead of on a build of their own; a fix found there gets one incremental rebuild. Dependent tasks within a batch (P4.T2 on P4.T1, P4.T7 on P4.T6) run as a second wave of edits, still without a build in between. The cost is more compile errors surfacing at once, which costs less than the full builds saved.
- 2026-10-06 — **B8 landed** (`b77c8fa3b`, `1f2cb4eb4`, `7e3a729c6`): native Merge, its nine presets and Dissolve. Full debug ctest passes 1125/1125 in both modes, smoke passes, and the release build succeeds. Parity is 0 everywhere except MergePlugin `op-divide` at 3.05e-5, and `grain-extract`/`hypot` below 5e-7, all in the transcendental class. P3.T7's triage only had to deal with B7's OFX Constant pins: OFX-clip tests moved to ColorWheel major 1, and Constant-as-source tests now run unpinned on the native node. GUI: visible knobs match for all 14 pairs (`build/m67-gui/merge-*.png`, `gen-*.png`; scripts in `build/m67-b8/`).
  - **Deadlock fix in the shared base:** band threads called `Image::getBounds()`, which takes the read lock, and queued behind a writer while the calling thread held the image. `TwoWritersOnTwoBranchesRenderedTogether` hung in 8 of 30 legacy runs. Bounds are now read once on the calling thread, and it then passed 30/30.
  - **Engine change:** native Merge and Dissolve are `InspectorNode`s, as their OFX versions are (`AppInstance::isEntitledForInspector`), because the node type is chosen before the effect exists.
  - **Behaviour fixes:** Dissolve's RoD with partial inputs falls back to the project extent, as OFX does. The Merge panel now creates explicit separator knobs, because `setAddSeparator` draws nothing on native knobs.
  - **Parity references:** the parity windows exceed Design §6's 64×48. The estimated references come to about 1.44 MB, against the 1.5 MB cap.
- 2026-10-07 — **B9 wave 1 written; wave 2 dispatched; nothing built yet.**
  - **P3.T8 retirement:** references recorded and committed (`7fa817a80`, 1.45 MB; replay 57/57). The fork commit "M67: retire merge and generators" is pushed at `0424c257` and pinned. Unrecorded parity cases and the OFX knob-parity tests for the family are deleted.
  - **P4.T1 resampler:** in a standalone host check, its output matched openfx-misc's `ofxsFilter.h` bit for bit over 80k random cases. Interpolation is done in double, as in OFX.
  - **P4.T6 blur kernels:** bit-exact against CImg 2.9.9. **Deviation:** the test define is `NATRON_TESTS_CIMG_HEADER` (full path), because an `#include` can't join a directory macro to a file name.
  - **P4.T3 Crop:** it declares the generator extent knobs itself, because `NativeGenerator` has no static knob helper. Folding the two into one helper is a follow-up. AppManager registration was split: Crop's is in wave 1, the rest in wave 2 under P4.T2.
- 2026-10-07 — **B9 wave 2 written** (Transform/TransformMasked, Reformat, Blur); the batch build and P4.T8 run as one agent.
  - **Deviation:** the Transform class is `TransformNode`, because `namespace Transform` (holding `Matrix3x3`) already exists.
  - **Behaviour departures:**
    - Reformat's `useRoD` is a metadata slave, so toggling it refreshes the output format.
    - Blur passes the source through where the mask is 0. It does not reproduce OFX's boundary-mode copy, which differs only with mask, "nearest" and an expanded RoD together.
  - **Missing icon:** Blur had none; it is copied in with the build.
- 2026-10-07 — **B9 landed** (`2e2804fc0`, `249cbd7d7`, `c9face1b3`, `c3f03b59e`): merge and generators retired, and all six spatial nodes native. Full debug ctest passes 1200/1200 in both modes, as does smoke and the release build. 40/40 clean loops of the scheduler tests over native Transform.
  - **Parity:** max diff is 0 for Crop, Position and Blur, and ≤2.4e-7 for Transform, TransformMasked and Reformat.
  - **P4.T8 triage:** no old test failed.
  - **Fixes:** Crop repeats edge pixels outside the source, like OFX's `getPixelAddressNearest`, and panel layout matches OFX (sliders, Scale folding, spacing).
  - **GUI:** shots are `build/m67-gui/spatial-*`, and overlay drags match OFX.
  - **Found, not fixed:**
    - (a) The host `PositionInteract::penUp` commits `lastPenPos`, so a release with no final motion lands one step short.
    - (b) **GUI crash:** `GuiPrivate::setUndoRedoActions` keeps a raw pointer to the undo action of a destroyed node's panel, and the next knob edit segfaults. This is generic, not M67's.
  - **References:** estimated at 1.28 MB.
- 2026-10-07 — **Spatial references recorded** (`1f5d37957`, 1.36 MB; 75/75 pass). **B10 dispatched.** EdgeDetect moved into wave 1, because it only needs `BlurKernels`, which has landed. Only P5.T6 waits for wave 2, because it edits `ColorLookup.cpp`.
- 2026-10-07 — **B10 written; the build and P5.T7 run as one agent.**
  - **P4.T9:** spatial plugins retired in the fork (`99817f41`, force-pushed over a bad partial commit, parent `0424c257` intact).
  - **P5.T3 split:** Erode/Dilate is `ErodeDilate` plus a thin `Dilate` subclass, and its CImg comparison test is split out into `ErodeDilateKernels_Test`. EdgeDetect reuses `ErodeDilateKernels::filterLine`.
  - **P5.T6 API:** `KnobParametric::setBackgroundPainter` returns quads, additive quads and polylines, and `CurveWidget` paints them.
  - **Behaviour departures:**
    - The histogram does not unpremultiply, because `Image` carries no premult state.
    - Keyer and ChromaKeyer with an RGB source treat alpha as 1 for Normal, and add nothing for "Add to Inside Mask". OFX reads out of bounds there.
    - Neither keyer mirrors OFX's output-premult preference, because `NodeMetadata` has no field for it.
  - **Brief lesson:** the P5.T1 agent read "own AppManager" as its whole task and waited for a Keyer author who never came. Keyer was re-dispatched. When one agent both implements and owns registration, the brief must lead with the implementation.
- 2026-10-07 — **B10 landed** (`610c41c08`, `9572c894c`, `4d1ff597c`): spatial OFX retired, and the keying/misc nodes are native.
  - **Test results:**
    - Full debug ctest 1244/1244 in both modes.
    - After the last panel fixes, a targeted run of 289/289 in both modes, plus smoke, the release build, and 40/40 hang loops.
  - **No old tests re-pointed.**
  - **Parity:** max diff is 0 for Erode/Dilate and ≤7.7e-7 for the others.
  - **GUI:** ColorLookup's ramp and histogram render the same as OFX, and the knob order matches for all six (`build/m67-gui/keying-*`).
  - **Parity decisions:**
    - **(1) EdgeDetect channel combining:** in this host, OFX EdgeDetect always feeds alpha into the rms/max/tensor modes. `Node.cpp:2940` keeps the plugin's own RGBA switches on and applies the channel set afterwards. Native combines only the processed channels, as upstream Natron's plugin does. So parity pairs process RGBA on both sides.
    - **(2) `-Ofast` ulps:** the OFX plugins are built with `-Ofast`, and their ulp differences flip tied pixels under Sobel plus non-maxima suppression. The recorded case uses Gaussian, which is bit-exact.
    - **(3) ChromaKeyer unpremultiplied tolerance:** these cases use a measured 1e-4 relative tolerance. The same ulps get divided by near-zero key alpha; 5e-5 fails.
- 2026-10-07 — **Keying references recorded** (`9c1f8f362`, 1.11 MB; 74/74 pass). B11 dispatched: P5.T8, P6.T2, P6.T3.
- 2026-10-07 — **B11 written; build and verification dispatched.**
  - **P5.T8:** keying retired in the fork (`59ae4c26`).
  - **P6.T2 audit:** found no missing real knobs. The allow-list gains Merge's `aChannelsChanged`/`bChannelsChanged`, OFX state flags that native Merge has no use for.
  - **Regression found:** DropShadow's `Multiply1` and PIKColor's two Dilates set the OFX `unPremultBy`, which is a no-op on native nodes, so they would silently stop unpremultiplying. The fix goes in the PyPlugs (`hostUnPremultBy`), with a test, rather than an alias knob, per the clean-break rule.
- 2026-10-07 — **B11 landed** (`ec8eb4629`, `eeb5d8172`, `31fc0fbbe`): every OFX plugin M67 replaces is now retired.
  - **Tests:** full debug ctest passes 1210/1210 in both modes, smoke passes (`verify_plugin_loads` on Misc and CImg), and the release build succeeds. `NativePluginList_Test` passes with all 34 retired IDs, each a single native version. The PyPlug tests pass.
  - **Replay mode:** a gdb trace of `compareParity` showed every native comparison replays. The only live calls are `NativeParitySelf_Test` on ColorMatrix, which is not retired.
  - **PyPlug fix:** DropShadow and PIKColor now set `hostUnPremultBy` to `rgba.A`, and a test checks it. The allow-list was trimmed to the names PyPlugs still set (`NatronOfxParamProcess*`, `aChannelsChanged`/`bChannelsChanged`). Dropping `premultChanged` departs from the spec, accepted because an entry nothing uses can only hide a regression.
- 2026-10-07 — **B12 landed; gate green; PR #42 open** (stacked on #41; CI dispatched manually; Codex review running).
  - **Bench** (`6fc2ec38f`, 44/44 configs exit 0, 6 head configs contended, ratios only):
    - mem 0.198 and HD 0.708. Both are within 0.1 of the gate medians (0.216 and 0.700).
    - Tiny graphs 0.34–0.72.
    - **Finding:** HD mixed, wide and footagecomp are 1.10–1.20x slower. By subtraction, Merge and CheckerBoard use about 1.9x the OFX CPU, and Blur, Transform and ColorCorrect about 1.3x. Not profiled; follow-up below.
  - **Input for M64:**
    - A native Grade is 21.3 ms/node against a ~5 ms bandwidth floor. One memory pass is ~24% of a node's cost, up from ~17%.
    - Even removing all of the bandwidth cost would give ≤1.23x, still under M64's 1.3x kill gate. M64 should re-estimate around fusing native kernels.
  - **AppImage:** `build/appimages/M67-31fc0fbbe.AppImage`; the launch check passes.
  - **UAT script:** `build/appimages/M67-uat.md`, which lists all 111 shots and is stacked on M63's.
  - **Process notes:**
    - `Plugins.pre-m67` lives in the worktree's `build/assets/`.
    - Packaging needs `build/appimagetool-wrapper` (`-n`) copied into the worktree's `build/`, because the container has no network.
  - **Published:** `docs/decisions/2026-10-06-m67-core-node-families.md` (`7d1db14cb`).
- 2026-10-07 — **Codex review round 1 (PR #42): 13 findings, all accepted.** The user chose to fix the HD composite slowdown in M67 before UAT rather than in a follow-up. Phase 67.7 / B13 was added.
- 2026-10-07 — **B13 fixes written (P7.T1–T5); build, rebench and repackage dispatched (P7.T6).**
  - **TransformMasked:** the OFX host also concatenates into TransformMasked, and OFX has the same pass-through bug. Native now blends with its immediate input. This is a **deliberate divergence**, fixed rather than replicated.
  - **Affine resampler path:** claimed bit-identical (same operation order, no FMA).
  - **Cancellation:**
    - **Cause:** `aborted()` only works on the render thread.
    - **Fix:** `RenderCancellation` publishes an atomic flag, and `parallelForCancellable`'s caller wait is timed at 10 ms. The remaining nodes are being swapped over in P7.T6.
  - **Merge and CheckerBoard:** per-operator templated row functions and CheckerBoard runs, both fuzz-verified bit-identical on the host.
  - **ColorCorrect:** skips identity groups, but not the tone blend, whose weights don't sum to exactly 1 in float.
  - **Extent knobs:** shared by Crop and the generators (`ExtentKnobs`); each node keeps its own knob order.
