# M167 - Per-Frame Node Metadata

Make a node's metadata (colour layout and components, bit depth, PAR, fps, format, field order) a function of time and view rather than one value per refresh. Today each node keeps one `NodeMetadata` in `EffectInstance::_imp->metadata`. Only the main thread recomputes it, through `Node::refreshAllInputRelatedData` (`Engine/Node.cpp` ~:7945-8003). Every getter (`Engine/EffectInstance.cpp` ~:6413-6540) first runs `refreshInputRelatedDataIfStale()`, which does nothing off the main thread, so render threads read whatever the last refresh left. The derivations that depend on time read `getCurrentTime()`, which on the main thread is the timeline frame (`EffectInstance.cpp` ~:6021-6033). That holds for NativeRead (`Engine/Nodes/IO/NativeRead.cpp` ~:1052), DeepRead (`Engine/Nodes/Deep/DeepRead.cpp` ~:158), NativeWrite (`NativeWrite.cpp` ~:1141), Crop (`Crop.cpp` ~:532), DeepCrop (`DeepCrop.cpp` ~:193) and Reformat (`Reformat.cpp` ~:946): their metadata is whatever frame was current at the last refresh. A keyed Disable is resolved as enabled at every frame (`EffectInstance.cpp` ~:6150-6153), and a Switch reports the widest layout of all its inputs (`getDefaultMetadata`, ~:6238-6410).

The key/value metadata already works per frame: `NativeEffectBase::getOutputMetadata` (`Engine/Nodes/NativeEffectBase.cpp` ~:382-426) caches per (time, view) under the node hash with a leaf lock and a generation check, and Route ops answer with their routed input (`StagedImageEffect.cpp` ~:600-618). This milestone gives `NodeMetadata` the same shape. Each node computes it on request at (time, view), and the actions cache keys it by (hash, time, view). A node that is an identity at a frame reports its input's metadata at the identity time. Native nodes derive it at a time. An OpenFX node answers per frame too: getClipPreferences runs once per distinct input-metadata signature (user decision, 2026-10-24). Renders read the metadata snapshotted with their frame's node hashes. That removes the main-thread-only refresh and M100's stale-data abort. Consumers outside the render (Viewer, Write, node graph, Python) ask at an explicit time. Premult is not a metadata field: M43 - Drop Premult Concept removes it. Prior art: Natron 3 (`origin/master` `Engine/EffectInstanceActions.cpp` ~:1933, `getTimeInvariantMetadata_public`) keys metadata by hash from any thread and keeps its side effects on the main thread.

Filed 2026-10-24 by M110 - Remaining Native GPU Ports, on the user's ruling that node metadata is not the same at every frame (M110's `## Decisions`, 2026-10-24). It absorbs M144 - Render-Thread Metadata Refresh (cancelled 2026-10-24), and carries M144's acceptance in Phases 167.3 and 167.6. M110.P3.T4 fixes the Switch's per-frame layers through the components path, ahead of this milestone.

Deps: M100 - Content Node Hashing.

## Scope

**In:**
- `EffectInstance::getMetadata(time, view)`, cached in `ActionsCache` under (hash, time, view) and safe from any thread. Plus `getTimeInvariantMetadata()`, cached per hash: today's derivation, kept as the answer at the OpenFX boundary.
- The identity rule: a node that is an identity at (time, view) reports its input's metadata at the identity time and view. This covers Switch, integer Dissolve, keyed Disable, Dot and group pass-throughs, and the OpenFX TimeOffset and FrameHold.
- Every native `getPreferredMetadata` override derives at an explicit (time, view), and no derivation reads `getCurrentTime()`.
- Renders read the metadata snapshotted per frame, alongside `ParallelRenderArgs::nodeHash`. File, tracker and colour-sampler renders never use stale metadata (M144).
- OpenFX clip preferences per frame: getClipPreferences runs once per distinct input-metadata signature (the inputs' metadata merged over every frame the render reads), under a per-instance mutex; answers are cached and each render thread's clips answer from its frame's result. The identity rule applies to OpenFX nodes too.
- Main-thread UI side effects (channel selectors, the Write's views menu, bit-depth/PAR/FPS warnings, the identity state) are driven from the current frame's answer on a hash change or a frame change, and no render reads them.
- Viewer, node-graph warnings, Python and Write follow the frame. Write follows the policy the user chooses.
- Delete `_imp->metadata`, the timeless getters, `refreshMetadata_public/_internal`, the `inputRelatedDataStale` flag with its posted refresh, and M100's viewer stale-data abort.

**Out:**
- Native TimeOffset, FrameHold, FrameRange and Retime nodes: M124 - Native Time Remaps.
- A premult field in `NodeMetadata`, or any premult tracking: M43 - Drop Premult Concept.
- Per-frame key/value `ImageMetadata`, which is already per frame (M74, M77). It is touched only where a reader moves to the new API.
- Snapshotting knob values for a render in progress: pixels computed from live knobs after a mid-render edit. No owner; recorded as a known gap if P3.T7 shows it.
- The components path and `presentsOnlyPassThroughLayers`, which belong to M110.P3.T4.

**Edge cases:**
- The identity check needs the node's own format (`isIdentity_public(..., getOutputFormat(), ...)`, `EffectInstance.cpp` ~:4772). Use the format from the node's own non-identity derivation at that time, so the check never recurses into itself.
- A Route op set to Black, or with no input connected: RGBA float at the project format, as `getDefaultMetadata` answers with no input.
- A keyed Disable reports its input's metadata at disabled frames and the node's own at enabled frames, replacing `isNodeDisabledAtAllTimes` (`EffectInstance.cpp` ~:6150).
- A missing frame in a Read sequence answers what the Read's missing-frame mode renders: the held frame's header, or the representative path when it renders black.
- Deep graphs: P3.T1 snapshots upstream nodes first, in `buildArgsMap`'s dependency order, so each derivation hits warm input caches instead of recursing the whole depth on a pool thread's stack.
- Broad graphs: cached frames per node are capped, like `kMaxCachedMetadataFrames` (256, `NativeEffectBase.cpp` ~:351), and an older hash's entries go with the `ActionsCache` LRU.
- Sub-frame times (motion blur, Retime) are cached under their exact double time, within the cap.
- Two threads deriving the same key both derive, and only one result is stored. A generation check drops a result that raced an invalidation.
- Metadata that changes under the same hash, such as a file reload (`Node::refreshMetadataReaders`, `Node.cpp` ~:1767), also drops the per-frame entries.
- Project load: nothing is refreshed eagerly. The first query computes.
- A hash that moves between the snapshot and the render: the frame keeps the snapshot it was set up with, and its cache entries go under the snapshot's hashes.

**Assumptions:**
- `Node::getHashValue()` may be called from any thread. `NativeEffectBase::getOutputMetadata` already relies on this (`NativeEffectBase.h` ~:361-365).
- Calling getClipPreferences once per input signature departs from the OpenFX spec ("constant for the duration of an effect", `libs/OpenFX/include/ofxImageEffect.h:333-342`; not a render-thread action, `ofxThreadSafety.rst:26-34`). Real plugins tolerate it: the Support library re-reads clip properties on every call (`ofxsImageEffect.cpp:1904-1931`) and parameters resolve at the per-thread time. `SET_CAN_SET_VALUE` is debug-only, so P1.T1 must find another way to stop parameter writes off the main thread. Known risk: ofxsMultiPlane rebuilds channel menus inside getClipPreferences (`SupportExt/ofxsMultiPlane.cpp:954-956`); those rebuilds are suppressed or deferred off the main thread.
- The tracker and colour-sampler renders go through `ParallelRenderArgs` with a frozen `nodeHash`. P3.T6 checks this.
- `OiioReadSupport::readHeader` caches headers, so a per-frame Read answer doesn't reopen the file on every query. P2.T5 checks this.
- M110.P3.T4 (`fd155a8e5`) is not on main (`9b6196a5c`). M167 rebases onto it when M110 merges; expect conflicts in `EffectInstance.cpp` and `StagedImageEffect.cpp`.
- `PremultAbsence_Test` arrives with M43. Until then, the premult check at the gate is the grep.

## Phase 167.1: Spike and per-frame core

- [ ] M167.P1.T1 — Spike OpenFX clip preferences off the main thread, and inventory the input-related data
  - files: `Engine/OfxEffectInstance.cpp`, `Engine/OfxEffectInstance.h`, `Tests/OfxClipPreferencesThread_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: Add a test-only entry point that runs `getClipPreferences_safe` from a pool thread under `preferencesLock` (write) with `SET_CAN_SET_VALUE(false)`, without the `assert` at ~:2006. Run it against the natronTest plugins. Grep `build/assets/plugin-src/openfx-misc` at the pinned ref for plugins that set params, or call `syncPrivateData`-style code, in `getClipPreferences`. Inventory into `## Decisions`: every field `refreshAllInputRelatedData` writes, and which thread reads each; every timeless getter call (about 150), classed as render-time, UI-at-current-frame or OpenFX boundary; and what `onSyncPrivateDataRequested` needs (`EffectInstance.cpp` ~:6155-6170). Check that per-signature calls on a non-main thread, under a per-instance mutex and while renders hold the preferences read lock, neither deadlock nor write instance members, and find how to block or defer parameter writes and ofxsMultiPlane menu rebuilds there. Recommend the details for P4.T1.
  - verify: at the phase build, the spike test passes under `--repeat until-fail:50` with 16 threads. The inventory is recorded, and the user approves the P4 thread model before P4 starts.
  - size: L

- [x] M167.P1.T2 — Add `getMetadata(time, view)` and `getTimeInvariantMetadata()` to `EffectInstance`, with the identity rule
  - files: `Engine/EffectInstance.h`, `Engine/EffectInstance.cpp`, `Engine/EffectInstancePrivate.h`, `Engine/EffectInstancePrivate.cpp`, `Engine/NodeMetadata.h`
  - approach: Add `typedef std::shared_ptr<const NodeMetadata> NodeMetadataConstPtr`, with no new fields and no premult. Add a new virtual `getPreferredMetadataAt(time, view, md)` whose default forwards to the old `getPreferredMetadata(md)`, so overrides can move family by family. `getDefaultMetadataAt(time, view)` is `getDefaultMetadata` reading each input's `getMetadata` at the same time. `getMetadata` reads the hash first, looks in a new `ActionsCache` slot under (hash, time, view), and otherwise derives with no lock held: if the identity check at that time (whole format, identity scale) names an input, copy the output fields from that input at the identity time; otherwise default, then preferred, then the clamping half of `checkMetadata` (warnings stay out). It stores the result only if the generation hasn't moved, and drops the cache on the same-hash invalidation. `getTimeInvariantMetadata()` is today's path, cached per hash. The timeless getters stay untouched until P6.
  - verify: P1.T3's tests, at the phase build `tools/ci/remote/build.sh fast --tests`.
  - size: L

- [x] M167.P1.T3 — Test effect and acceptance tests for per-frame metadata
  - files: `Tests/NativeMetadataTestEffect.h`, `Tests/PerFrameMetadata_Test.cpp` (new), `Tests/CMakeLists.txt`, `Tests/TimeVaryingLayers_Test.cpp`
  - approach: Give the metadata source test effect animatable nComps, bit depth, PAR and format knobs, answered through `getPreferredMetadataAt`, plus a derivation counter. Cases: a Switch keyed between rgb+spec at f1 and rgba at f2 gives `getMetadata` RGB at f1 and RGBA at f2, and so does a native node below it; an integer Dissolve does the same; a keyed Disable passes its input's metadata through per frame; `kNatronTestFrameWindowPluginID` with a one-frame range reports the held frame's metadata; the Switch's `getTimeInvariantMetadata()` is still the widest; a second query does not derive again; 16 threads querying random frames while the main thread toggles a knob 1000 times: every cached answer equals a fresh derivation under the hash it was stored under. In `TimeVaryingLayers_Test`, the Switch cases also assert the metadata layout.
  - verify: `tools/ci/remote/build.sh fast --tests` passes in both legs, and `PerFrameMetadata` passes `--repeat until-fail:50` in natron-dev against that build.
  - size: L

**Phase gate:** one `tools/ci/remote/build.sh fast --tests` is green, including `PerFrameMetadata`, `TimeVaryingLayers` and `OfxClipPreferencesThread`.

## Phase 167.2: Native derivations at a time

- [x] M167.P2.T1 — Route ops: Black and no-input answers at a time
  - files: `Engine/Nodes/Image/StagedImageEffect.h`, `Engine/Nodes/Image/StagedImageEffect.cpp`, `Tests/Native/NativeSwitch_Test.cpp`, `Tests/Native/NativeDissolve_Test.cpp`
  - approach: Override `getPreferredMetadataAt` for Route ops. For `Route::Kind::Black`, answer RGBA float at the project format; Input is covered by the identity rule. Tests: a Switch keyed between two Constants with different format and PAR gives each at its frame; a Switch past its connected inputs and an empty Switch give RGBA at the project format; a fractional Dissolve keeps the default derivation.
  - verify: the phase build; `NativeSwitch` and `NativeDissolve` green.
  - size: M

- [x] M167.P2.T2 — AddLayers and the ChannelCopy colour-storage helper at a time
  - files: `Engine/Nodes/Channel/AddLayers.h`, `Engine/Nodes/Channel/AddLayers.cpp`, `Engine/Nodes/Channel/ChannelCopy.h`, `Engine/Nodes/Channel/ChannelCopy.cpp`, `Tests/TimeVaryingLayers_Test.cpp`
  - approach: `ChannelCopy::metadataColorStorage(effect, inputNb, time, view)` reads `input->getMetadata(time, view)`. AddLayers moves to `getPreferredMetadataAt`, and `listRegistryPlanes` is called at the given time instead of 0 (`AddLayers.cpp` ~:207-224). Delete the "must not depend on the current frame" comment. Every other timeless metadata read in these files moves to the explicit time.
  - verify: the phase build; a new `TimeVaryingLayers` case shows a Switch (rgb+spec at f1, rgba at f2) feeding AddLayers reports RGB widened per AddLayers' rule at f1 and RGBA at f2.
  - size: M

- [x] M167.P2.T3 — Crop and Reformat at a time
  - files: `Engine/Nodes/Transform/Crop.h`, `Engine/Nodes/Transform/Crop.cpp`, `Engine/Nodes/Transform/Reformat.h`, `Engine/Nodes/Transform/Reformat.cpp`, `Tests/Native/NativeReformat_Test.cpp`, `Tests/Native/NativeCrop_Test.cpp`
  - approach: Replace `getCurrentTime()` and the timeless `getValue()` calls in both overrides with the given (time, view). Move these files' other timeless metadata reads too.
  - verify: the phase build; with an animated Reformat box and Crop rectangle, the format at f1 and f2 matches the knob at each frame whatever the timeline frame is.
  - size: M

- [x] M167.P2.T4 — DeepCrop and DeepRead at a time
  - files: `Engine/Nodes/Deep/DeepCrop.h`, `Engine/Nodes/Deep/DeepCrop.cpp`, `Engine/Nodes/Deep/DeepRead.h`, `Engine/Nodes/Deep/DeepRead.cpp`, `Tests/DeepReadWrite_Test.cpp`
  - approach: DeepRead reads `getFilenameAtTime(time)` (~:158); DeepCrop reads `getBBoxCanonical(time)` (~:193). Delete DeepRead's comment about being consulted only once (~:76).
  - verify: the phase build; a deep sequence whose frames differ in size (written by the test into a temp dir) reports each frame's format.
  - size: M

- [x] M167.P2.T5 — NativeRead and NativeWrite read-back at a time
  - files: `Engine/Nodes/IO/NativeRead.h`, `Engine/Nodes/IO/NativeRead.cpp`, `Engine/Nodes/IO/NativeWrite.h`, `Engine/Nodes/IO/NativeWrite.cpp`, `Tests/Native/NativeReadMetadata_Test.cpp`
  - approach: NativeRead uses `targetAtTime(time)` (~:1052), with the missing-frame rule from Edge cases, and that frame's header format, PAR and nComps for the view's colour part rather than view 0. NativeWrite uses `readBackFrame(time, view)` (~:1141), and `selectedFormat`'s Input type uses `input->getMetadata(time, view)` (~:1814-1817). Confirm headers are cached. Leave `onMetadataRefreshed` to P5.T4.
  - verify: the phase build; an EXR sequence the test writes, RGB 1920x1080 at frame 1 and RGBA 2048x858 PAR 2 at frame 2, reports each frame's values. `NativeWriteReadBack` green.
  - size: L

- [x] M167.P2.T6 — NativeGenerator and RemoveLayers at a time
  - files: `Engine/Nodes/Image/NativeGenerator.h`, `Engine/Nodes/Image/NativeGenerator.cpp`, `Engine/Nodes/Channel/RemoveLayers.h`, `Engine/Nodes/Channel/RemoveLayers.cpp`, `Tests/Native/NativeConstant_Test.cpp`
  - approach: The generator's extent knobs are read with `getValueAtTime(time, …, view)` (~:161-185). RemoveLayers reads its input's metadata at the time.
  - verify: the phase build; a Constant with an animated format extent reports each frame's format.
  - size: M

- [x] M167.P2.T7 — Move Merge, Keyer and ChromaKeyer to `getPreferredMetadataAt`
  - files: `Engine/Nodes/Merge/Merge.h`, `Engine/Nodes/Merge/Merge.cpp`, `Engine/Nodes/Keyer/Keyer.h`, `Engine/Nodes/Keyer/Keyer.cpp`, `Engine/Nodes/Keyer/ChromaKeyer.h`, `Engine/Nodes/Keyer/ChromaKeyer.cpp`
  - approach: mechanical rename only. Each override takes `(double /*time*/, ViewIdx /*view*/, NodeMetadata&)` and its body is unchanged.
  - verify: the phase build compiles; `Native.*Merge|Keyer` green.
  - size: S

- [x] M167.P2.T8 — Move DeepReformat, DeepToImage, RawDemosaic and RawToImage to `getPreferredMetadataAt`
  - files: `Engine/Nodes/Deep/DeepReformat.h`, `Engine/Nodes/Deep/DeepReformat.cpp`, `Engine/Nodes/Deep/DeepToImage.h`, `Engine/Nodes/Deep/DeepToImage.cpp`, `Engine/Nodes/Raw/RawDemosaic.h`, `Engine/Nodes/Raw/RawDemosaic.cpp`, `Engine/Nodes/Raw/RawToImage.h`, `Engine/Nodes/Raw/RawToImage.cpp`
  - approach: mechanical rename as in T7; bodies unchanged.
  - verify: the phase build compiles; the Deep and Raw suites green.
  - size: S

- [x] M167.P2.T9 — Move RotoPaint and the test effects to `getPreferredMetadataAt`
  - files: `Engine/RotoPaint.h`, `Engine/RotoPaint.cpp`, `Tests/NativeParity.h`, `Tests/NativeParity.cpp`, `Tests/Native/RawTestSource.h`, `Tests/Native/RawKindFramework_Test.cpp`
  - approach: mechanical rename as in T7. Afterwards `git grep -n "getPreferredMetadata(NodeMetadata"` lists only the default in `EffectInstance` and `OfxEffectInstance`.
  - verify: the phase build compiles; the grep holds.
  - size: S

**Phase gate:** one `tools/ci/remote/build.sh fast --tests` is green in both legs, and `git grep -n getCurrentTime` over the touched overrides' derivations is empty.

## Phase 167.3: Renders read their frame's metadata (M144)

- [x] M167.P3.T1 — Snapshot each node's metadata with its hash when a frame is set up
  - files: `Engine/ParallelRenderArgs.h`, `Engine/ParallelRenderArgs.cpp`, `Engine/EffectInstance.h`, `Engine/EffectInstance.cpp`
  - approach: In `buildArgsMap`, after the hashes are read, store `getMetadata(time, view)` in each node's `ParallelRenderArgs`, upstream nodes first, keyed to `nodeHash`. Add `EffectInstance::getRenderMetadata()`, returning the frame args' snapshot on a render thread and otherwise `getMetadata(getCurrentTime(), view)`. A derivation reports the hash it ran under; if that differs from the frame's, re-read hashes and redo the snapshot, at most 3 times, then keep the latest.
  - verify: the phase build; a gtest that sets up a frame with a test hook moving a knob mid-snapshot gets a snapshot whose hashes all match the args.
  - size: L

- [x] M167.P3.T2 — Engine render internals read the snapshot
  - files: `Engine/EffectInstance.cpp`, `Engine/EffectInstanceRenderRoI.cpp`, `Engine/EffectInstanceRenderDeep.cpp`
  - approach: Move the render-time reads to `getRenderMetadata()`: `EffectInstance.cpp` ~:711, 885, 1256-1260, 1539-1642, 2838, 5082, 5290; `EffectInstanceRenderRoI.cpp` ~:378, 837; and the deep path. Starts after T1.
  - verify: the phase build; `SchedulerEquivalence` and `CacheEquivalence` green in both legs; a render of a Switch at f1 and f2 produces RGB and RGBA images.
  - size: L

- [x] M167.P3.T3 — Native executors and the scheduler read the snapshot
  - files: `Engine/Nodes/Image/ResampleExecutor.cpp`, `Engine/Nodes/Image/PassExecutor.cpp`, `Engine/Nodes/Image/StagePlan.cpp`, `Engine/GpuRegionTask.cpp`, `Engine/RenderScheduler.cpp`
  - approach: Move each timeless read to `getRenderMetadata()`: ResampleExecutor (~9 reads), GpuRegionTask ~:517, RenderScheduler ~:430 and 569-571, where the memory estimate now sizes from the frame's layout rather than the widest.
  - verify: the phase build; the GPU legs green on lavapipe.
  - size: M

- [x] M167.P3.T4 — Node-level render reads in the filter, merge and deep families
  - files: `Engine/Nodes/Filter/Blur.cpp`, `Engine/Nodes/Filter/EdgeDetect.cpp`, `Engine/Nodes/Merge/Merge.cpp`, `Engine/Nodes/Deep/DeepRecolor.cpp`, `Engine/Nodes/Channel/ChannelCopy.cpp`
  - approach: Move each remaining timeless metadata read to `getRenderMetadata()`, or to `getMetadata(time, view)` where the action already has a time.
  - verify: the phase build green.
  - size: M

- [x] M167.P3.T5 — Node-level render reads in the Raw family
  - files: `Engine/Nodes/Raw/RawDemosaic.cpp`, `Engine/Nodes/Raw/RawTemperature.cpp`, `Engine/Nodes/Raw/RawNodeSupport.cpp`, `Engine/Nodes/Raw/RawDisplay.cpp`, `Engine/Nodes/Raw/RawColorCalibrationSpot.cpp`, `Engine/Nodes/Raw/RawColorCalibrationCheckerNode.cpp`
  - approach: as in T4.
  - verify: the phase build; the Raw suites green.
  - size: M

- [x] M167.P3.T6 — File-render, viewer, tracker and colour-sampler paths read the snapshot
  - files: `Engine/OutputSchedulerThread.cpp`, `Engine/ViewerInstance.cpp`, `Engine/NodeColorSampler.cpp`, `Engine/TrackerFrameAccessor.cpp`, `Engine/TrackMarker.cpp`
  - approach: Move the reads at `OutputSchedulerThread.cpp` ~:2386 and 2597-2604, `ViewerInstance.cpp` ~:1033 and 1550, `NodeColorSampler.cpp` ~:119, `TrackerFrameAccessor.cpp` ~:309, `TrackMarker.cpp` ~:1167. Confirm the tracker and sampler renders carry frame args; otherwise read `getMetadata` at their frame under the hash they captured.
  - verify: the phase build; `Tracker`, `NodeColorSampler` and `ViewerScheduler` green.
  - size: M

- [ ] ~~M167.P3.T7 — Regression tests for edits during non-abortable renders (M144 acceptance)~~ (cancelled 2026-10-24: the user ruled mid-render edits impossible outside the viewer; moved to M177)
  - files: `Tests/MidRenderEdit_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: Three renders — a 20-frame blocking file render of a NativeWrite, a 10-frame track, a colour-sampler pass. During each, a test hook changes an upstream Switch's `which`, a Reformat size and a Read file from the main thread. Assert: the job completes under a 60 s guard; every cached or written frame's stored hash equals the hash recomputed from that frame's inputs at its snapshot; each frame's metadata, header channels and size match a fresh render of that state.
  - verify: the phase build; `ctest -R MidRenderEdit --repeat until-fail:50` in natron-dev with 8 parallel copies: 0 failures.
  - size: L

**Phase gate:** one `tools/ci/remote/build.sh fast --tests` is green in both legs, including `SchedulerEquivalence`, `CacheEquivalence` and `TimeVaryingLayers`.

## Phase 167.4: OpenFX boundary: per-frame answers

- [ ] M167.P4.T1 — Per-signature OpenFX clip preferences under a per-instance mutex
  - files: `Engine/OfxEffectInstance.cpp`, `Engine/OfxEffectInstance.h`, `Engine/EffectInstance.cpp`, `Engine/EffectInstancePrivate.h`
  - approach: For an OpenFX node, `getMetadata(time, view)` is the identity rule's answer on pass-through frames; otherwise it builds the input signature — each input's `getMetadata` merged (widest components, deepest depth; PAR and fps from frame t) over every frame `getFramesNeeded` asks for at (time, view) — and looks up a cache keyed by (hash, signature, view), adding time only when the node has animated or expression knobs. On a miss, run `getClipPreferences_safe` under a new per-instance mutex (not the preferences write lock, which renders hold for reading), with the input clips answering the signature through per-thread clip storage, and capture the result without writing instance members. Parameter writes and channel-menu rebuilds off the main thread are blocked or deferred per P1.T1. `onSyncPrivateDataRequested` moves to the knob-change path on the main thread (`EffectInstance.cpp` ~:6155-6170).
  - verify: the phase build; `OfxTestPlugin` and `Metadata_Test` green.
  - size: L

- [ ] M167.P4.T2 — OpenFX clip properties answer from the render thread's frame
  - files: `Engine/OfxClipInstance.cpp`, `Engine/OfxClipInstance.h`, `Engine/OfxImageEffectInstance.cpp`, `Engine/OfxImageEffectInstance.h`
  - approach: Store the frame's clip-preferences answer in the per-thread clip storage (`OfxClipInstance.h` ~:268-292) for the render and for any action called on a render thread. The clip getters (~14 in `OfxClipInstance.cpp`, ~:185, 241, 259, 358-434, 475, 530, 570) read it there; the instance-wide answer is only a fallback for actions on the main thread with no frame (computed at the current frame). Fetched frames are converted to the frame's merged layout (~:852-862). Drop `refreshInputRelatedDataIfStale` from `getCurrentMetadata` (~:1835), keeping `dropStaleClipMetadata`.
  - verify: the phase build; `OfxMetadataBridge` and every OpenFX test green.
  - size: L

- [ ] M167.P4.T3 — OpenFX tests across the boundary
  - files: `Tests/OfxPerFrameMetadata_Test.cpp` (new), `Tests/CMakeLists.txt`, `Tests/OfxTestPlugins/natronTest/TraitTestPlugins.cpp`
  - approach: Add a clip-preferences call counter to a natronTest plugin. Cases: an OpenFX node below a per-frame Switch (RGB at f1, RGBA at f2) sees RGB at f1 and RGBA at f2, rendered in parallel on 16 threads, and getClipPreferences runs once per distinct signature across 100 frames; a temporal OpenFX plugin (TimeBlur-like test plugin asking for f1 and f2) at f1 gets the merged RGBA layout and both fetched frames in it; a native node below an OpenFX FrameWindow identity gets the held frame's metadata with no getClipPreferences call on identity frames; querying from 16 threads while the main thread edits does not deadlock (30 s guard).
  - verify: the phase build; `OfxPerFrameMetadata` passes `--repeat until-fail:50`.
  - size: L

**Phase gate:** one `tools/ci/remote/build.sh fast --tests` is green, with the OpenFX suites unchanged apart from the new one.

## Phase 167.5: Consumers outside the render

- [ ] M167.P5.T1 — Main-thread UI hook driven by hash and frame changes
  - files: `Engine/Node.cpp`, `Engine/Node.h`, `Engine/EffectInstance.h`, `Engine/EffectInstance.cpp`, `Engine/Nodes/NativeEffectBase.cpp`
  - approach: Replace `onMetadataRefreshed` with `onCurrentFrameMetadataChanged(md)`, run on the main thread when the node's hash or the timeline frame changes, for nodes with a shown panel, a layer knob or a views menu. It refreshes the channel selectors, the identity state, the produced-layer registry, and the bit-depth/PAR/FPS warnings, which move out of `checkMetadata` (~:6659) and are computed from the current frame's answer. Renders never read anything it sets. `NativeEffectBase::onMetadataRefreshed` (~:472) moves to it.
  - verify: the phase build; `TimeVaryingLayers.TimeChangeRelistsLayerMenus` and `LayerMenusListReadLayersAtCurrentFrame` green; a new case shows a PAR warning appears and clears as the frame moves over a Switch.
  - size: L

- [ ] M167.P5.T2 — Viewer and node graph follow the current frame
  - files: `Gui/ViewerTab30.cpp`, `Gui/ViewerTabPrivate.cpp`, `Gui/ViewerGL.cpp`, `Gui/NodeGraph15.cpp`
  - approach: The format, PAR and fps box (`ViewerTab30.cpp` ~:1274-1344), `ViewerTabPrivate.cpp` ~:249, `ViewerGL.cpp` ~:4567 and the edge PAR/fps mismatch tooltip (`NodeGraph15.cpp` ~:75-88) read `getMetadata(currentFrame, view)`, refreshed on the timeline's `frameChanged`. The channel menu follows P5.T1.
  - verify: the phase build, then an Xvfb check on the phase AppImage: scrubbing over a keyed Switch (RGB HD at f1, RGBA 2K PAR 2 at f2) changes the viewer format, PAR and channel menu; screenshots shared before sign-off. GUI bench if M117 has landed.
  - size: M

- [ ] M167.P5.T3 — Python asks at an explicit time and view
  - files: `Engine/PyNode.h`, `Engine/PyNode.cpp`, `Engine/typesystem_engine.xml`, `Tests/Native/EffectMetadataPython_Test.cpp`
  - approach: `getOutputFormat`, `getFrameRate`, `getPixelAspectRatio` and `getBitDepth` take `(time, view)`, like `getRegionOfDefinition` (`PyNode.h` ~:401), with no default (clean break).
  - verify: the phase build; a Python test reads different values at f1 and f2 over a keyed Switch, and the no-argument call is gone.
  - size: M

- [ ] M167.P5.T4 — Write applies the agreed policy over a varying range
  - files: `Engine/Nodes/IO/NativeWrite.cpp`, `Engine/Nodes/IO/NativeWrite.h`, `Engine/OutputEffectInstance.cpp`, `Tests/Native/NativeWriteMetadata_Test.cpp`
  - approach: Implement the user's ruling (see Decisions). `NativeWrite::onMetadataRefreshed` moves to P5.T1's hook, so its views menu follows the frame.
  - verify: the phase build; writing f1-f2 over the keyed Switch gives files whose headers match the ruling; `NativeWrite*` green.
  - size: M

**Phase gate:** one `tools/ci/remote/build.sh fast --tests` is green, and the P5.T2 Xvfb check passes on the phase AppImage with screenshots shared.

## Phase 167.6: Delete the refresh machinery, measure and soak

- [ ] M167.P6.T1 — Remove the timeless metadata state and getters
  - files: `Engine/EffectInstance.h`, `Engine/EffectInstance.cpp`, `Engine/EffectInstancePrivate.h`, `Engine/Nodes/NativeEffectBase.h`, `Engine/Nodes/NativeEffectBase.cpp`
  - approach: Delete `_imp->metadata`, `metadataMutex`, `setDefaultMetadata`, `setMetadataInternal`, `refreshMetadata_public/_internal`, the old `getPreferredMetadata(NodeMetadata&)` virtual and the timeless getters (`getOutputFormat()`, `getAspectRatio(int)`, `getFrameRate()`, `getBitDepth(int)`, `getMetadataComponents`, `getMetadataNComps`, `getFieldingOrder`, `isFrameVarying`, `canRenderContinuously`). Any leftover caller fails to compile and gets the time it needs.
  - verify: the phase build; `git grep -n -E "refreshMetadata_|_imp->metadata\b|getPreferredMetadata\(NodeMetadata" Engine Gui Tests` is empty.
  - size: L

- [ ] M167.P6.T2 — Remove the stale-data abort and the render-start refresh
  - files: `Engine/ParallelRenderArgs.cpp`, `Engine/ViewerInstance.cpp`, `Engine/OutputSchedulerThread.cpp`, `Engine/OutputSchedulerThread.h`, `Engine/AppInstance.cpp`
  - approach: Delete the stale scan and viewer abort (`ParallelRenderArgs.cpp` ~:1099-1139, `_abortedForStaleInputData`), `ViewerInstance.cpp` ~:596 and 1456, `RenderEngine::refreshInputRelatedDataForRender` (`OutputSchedulerThread.cpp` ~:3201-3287) and its call at `AppInstance.cpp` ~:1906.
  - verify: the phase build; `ViewerScheduler` and `MidRenderEdit` green.
  - size: M

- [ ] M167.P6.T3 — Shrink the Node refresh to the UI hook
  - files: `Engine/Node.cpp`, `Engine/Node.h`, `Engine/NodePrivate.h`, `Gui/NodeGui.cpp`
  - approach: Starts after T2. Delete `inputRelatedDataStale`, `requestInputRelatedDataRefresh` and `refreshInputRelatedDataForRender`. Reduce `refreshAllInputRelatedData` and the `markDownstream…`/`refreshShownInputRelatedData` walks to firing P5.T1's hook (`Node.cpp` ~:7939-8355). `NodeGui.cpp` ~:607 and 674 call the hook.
  - verify: the phase build; `git grep -n -E "inputRelatedDataStale|requestInputRelatedDataRefresh|refreshInputRelatedDataForRender" Engine Gui` is empty; full ctest green.
  - size: L

- [ ] M167.P6.T4 — Scaling bench and soak
  - files: none (results go in `## Decisions`)
  - approach: On the target host, using stripped `release` AppImages of main at the claim commit and of this branch, run `tools/bench/graph_bench.py` with `BENCH_RES=tiny` for `chain` N=500 and `wide` N=500, and `BENCH_RES=hd` for `comp` N=200, per frame and over `BENCH_RANGE=1`. Soak `PerFrameMetadata`, `OfxPerFrameMetadata` and `MidRenderEdit` with 8 parallel copies and `--repeat until-fail:100`.
  - verify: no configuration more than 5% slower than baseline (a larger regression needs the user's acceptance); soaks 0 failures; both recorded.
  - size: M

**Verification gate:** All six phase builds (`tools/ci/remote/build.sh fast --tests`) and a final `tools/ci/remote/build.sh debug --tests` pass in both scheduler legs, including `PerFrameMetadata`, `TimeVaryingLayers`, `SchedulerEquivalence`, `CacheEquivalence`, `MidRenderEdit`, `OfxPerFrameMetadata`, the unchanged OpenFX suites and, once M43 has landed, `PremultAbsence`. `git grep -n -i premult Engine/NodeMetadata.h Engine/NodeMetadata.cpp` is empty. Every grep in P6 is empty. P6.T4's bench and soaks are recorded with no unaccepted regression. The Xvfb viewer check on the release AppImage (format, PAR, fps and channel menu following the frame over a keyed Switch) is shared and approved. If M117 has landed, the GUI bench runs because `Gui/NodeGraph15.cpp` changes.

## Decisions

- 2026-10-24 — **Phase 3 gate passed (PM):** fast 416/416 both legs on 53610aaef. Fixes during the gate: the identity rule keeps the fields a node sets itself (a Grade at defaults or a staged effect at mix 0 keeps its RGBA widening), and the keyed-Disable RemoveLayers frame expects its input layout per the per-frame rule. Known gap: the viewer reads its aspect ratio live before its frame args exist.
- 2026-10-24 — **No render but the viewer sees mid-render edits (user):** a foreground Write render blocks the UI behind a modal; a background render works from a frozen copy captured at submission; tracking and the colour sampler block the UI like a foreground render; only viewer renders restart on a knob change. That work is M177 - Renders Isolated From Edits, not M167. Consequences here: revert the abortAnyEvaluation changes from Phase 3 (no blocking-render exemption, no viewer-only rule), and drop Tests/MidRenderEdit_Test.cpp; M144 acceptance for file, tracker and sampler renders under edits moves to M177. M167 keeps the per-frame snapshot those renders read (P3.T6) and its hash-match test.
- 2026-10-24 — **Phase 2 gate passed (PM):** fast 404/404 both legs on 886c0baf1; `PerFrameMetadataTest.*:OfxClipPreferencesThread*` clean over 300 repeats, and `ConcurrentQueriesCacheOnlyConsistentAnswers` clean over 4 parallel x 200. The only `getCurrentTime` left near `getPreferredMetadataAt` is the base-class current-time wrapper, not an override derivation.
- 2026-10-24 — **Cache race, third cause (PM, from gdb on the HEAD test binary):** 200-repeat runs still failed `ConcurrentQueriesCacheOnlyConsistentAnswers` after the write gate (7b170e390). Root cause: `Node::refreshDirtyHashes`'s last retry (`recomputeHashIfDirty(false)`) hashed over a still-dirty input's old hash and marked the node clean. The mark flush stops at dirty inputs, so a downstream node could hold a clean, stale hash before any derive began, and no during-derive check catches that. Fix 886c0baf1: hashing over a dirty input leaves the node dirty, and metadata is cached only under `getHashValueIfUpToDate`. Left as follow-ups (inbox): the identity, actions/RoD and image caches also accept a not-up-to-date hash; `NativeEffectBase::getOutputMetadata` has no mark-count or write check, so it can cache under an A→B→A hash.
- 2026-10-24 — **P1.T1 inventory and the P4 thread-model recommendation (PM, from the spike in 70a03eb30; needs user approval before P4):**
  - **`refreshAllInputRelatedData` (Node.cpp ~:7961, main thread) writes:** the stale flags; `draftModeUsed` and `supportsRenderScaleMaybe` (from RoD calls at the timeline frame); `_imp->metadata`; then `onMetadataRefreshed`, which pushes clip preferences, components, depth and PAR to OpenFX clips and clears their caches. It also writes the channel selectors (`layerListRefreshed`), the project layer registry (`registerProducedLayers`, main-thread only), and the identity state (`refreshIdentityState`, with its signals and mask selector). Render threads read the metadata and the clip values; the GUI reads the selectors and identity state.
  - **Timeless getters, 163 call lines:** about 110 at render time (EffectInstance, RenderRoI, RenderDeep, GpuRegionTask, ResampleExecutor, OutputSchedulerThread, ViewerInstance, TrackMarker), about 35 for UI at the current frame (NodeGraph15, ViewerTab30, NodeInputs, Node, RotoContext), and about 15 at the OpenFX boundary (OfxClipInstance components, format, fps, fielding and continuous; OfxImageEffectInstance ~:480).
  - **`onSyncPrivateDataRequested` (OfxEffectInstance.cpp ~:3445)** needs the main thread, `RECURSIVE_ACTION` and `SET_CAN_SET_VALUE(true)`, and must not hold `preferencesLock`; a per-frame call must never trigger it.
  - **Recommendation:** a thread-local "per-frame preferences" flag. While it is set, OpenFX parameter setters and setOption record the write and post one queued main-thread refresh instead of applying it, and ofxsMultiPlane `buildChannelMenus` (also OneView ~:109, WriteOIIO ~:517) skips its menu rebuild, leaving it to that refresh. Per-frame calls run on a dedicated pool so pool starvation cannot block a render holding read locks. They take `preferencesLock` downstream to upstream only, because the recursive QReadWriteLock cannot upgrade a read lock. Fix first the existing ABBA ordering between `preferencesLock` and `Instance::_metadataMutex` in `OfxEffectInstance::onNodeHashChanged`.
- 2026-10-24 — **Phase 2 deviations (PM):** `ExtentKnobs::getExtent(time, view)` added for the generator and Crop. The generator no longer skips reformatting for an animated Size extent, so native and OpenFX may differ there. `AddLayersRender_Test.MetadataDoNotDependOnTheFrameTheTimelineIsOn` now checks per-frame `getMetadata`. `CropOp::crop` is non-const. Reformat gained `getReformatType(time, view)` and `getSourcePixelAspectRatio(time, view)`. Read and Write cache file headers by path, modification time and size.
- 2026-10-24 — **Metadata cache race fixed; value-before-mark window left open (PM):** the Phase 1 fast build failed `ConcurrentQueriesCacheOnlyConsistentAnswers` on the legacy leg (initial run and rerun). Cause: an upstream knob that changes and changes back during a derive leaves the downstream hash and the per-instance generation unchanged, so an answer derived from the intermediate state was stored under the original key. Fix (e2c29c1b3): `Node::getHashMarkCount()`, read before the hash; `getMetadata` and `getTimeInvariantMetadata` store only if the mark count and the hash are both unchanged after the derive. Left open: a knob write stores the value before it marks the hash, so a derive that finishes inside that gap can still cache under the old key. Every hash-keyed engine cache (identity, RoD, render) shares this gap, and closing it needs a writer-side edit counter around knob writes, so it is outside this milestone.
- 2026-10-24 — **User decision: per-frame OpenFX metadata.** getClipPreferences runs once per distinct input-metadata signature — the inputs' metadata merged over every frame the render reads, because temporal plugins (TimeBlur, FrameBlend, Retime, TimeOffset, FrameHold) fetch neighbours and reject mismatched formats — under a per-instance mutex, cached by (hash, signature, view[, time when animated]), with each render thread's clips answering from its frame's result. This knowingly departs from the OpenFX spec, which defines clip preferences as constant for an effect; real plugins tolerate it. The identity rule applies to OpenFX nodes too (TimeOffset/FrameHold read another frame, which getClipPreferences can't see, and identity frames skip the call). Rejected: one answer per hash (wrong when upstream varies); a plugin clone per signature (heavy `createInstanceAction`). Phase 167.4 rewritten.

- 2026-10-24 — **User decision: a Write over a range whose metadata varies writes per-file headers.** Each image-sequence file's header follows its own frame's metadata; an OpenFX video writer keeps its per-hash clip preferences and the host converts each frame to them, as today (P5.T4). Open: the user asked whether OpenFX can be handed the correct metadata for the frame it is processing rather than a time-invariant answer; P4's design waits on that analysis, and the identity-rule question for OpenFX follows from it.

- 2026-10-24 — **Elaborated from the stub (PM, opus consultant).** M110 is not added to Deps: M110.P3.T4 (`fd155a8e5`) touches `EffectInstance.cpp` and `StagedImageEffect.cpp`, and M167 rebases onto it when M110 merges (textual conflicts expected). P5.T3 drops the no-argument Python metadata calls (clean break).
