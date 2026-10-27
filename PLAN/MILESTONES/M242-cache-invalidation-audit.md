# M242 - Cache Invalidation Audit

In M185 - Variable Warps UAT the user disconnected STMap's Map input, and STMap kept rendering as if the old Map were still connected. It didn't reproduce later. M178 - Render Context Core's UAT had already found two other holes: the timeline's cached-frames line ignored invalidation, and OpenFX binary stamps changed with every AppImage mount. This milestone audits every cache in the engine and viewer for invalidation holes and delivers three things. First, a written map of each cache, its key, and what invalidates it, kept in the repo as design docs under `docs/cache-invalidation/`. Second, randomized stress tests that compare cached renders against a cache-off render after each edit. Third, fixes for the holes found. It runs on top of M178 - Render Context Core (context keys, timeline fix, mount-independent stamps) and M185 - Variable Warps (the Map input), so both must be on `main` first.

The hash side is content-addressed. `Node::computeContentHash` (`Engine/Node.cpp`) folds in plugin ID, version and binary stamp, knob digest, connected inputs by slot, roto, enclosing group state, frame range and project digest. Dirty marks propagate through `markHashDirtyInternal`/`flushHashDirtyMarks`, and `EffectInstance::onNodeHashChanged` deliberately keeps old entries: a node that returns to an earlier state is served what it had then. The consequence is that a single image stored under the wrong key is never cleaned up. It comes back every time the graph returns to that state, so a hole has to be fixed at the source, not worked around with a re-render.

## Scope

**In:**
- The map: one doc per cache family plus an index (`docs/cache-invalidation.md`). Each entry names the cache, where it lives (file:symbol), its key, its lifetime and eviction, every invalidation trigger, the tests that cover it, and suspected holes.
- Families: hash and render-hash keys; input topology edits (including optional Map/Mask inputs, groups and the queued gui-input path); `ActionsCache` and per-frame metadata; the image caches (`NodeCache`, `DiskCache`, `DeepImageCache`, `FrameStore`); the viewer cache and timeline cached-frames line; GPU caches and residency; expressions, links and metadata readers; node-private caches (scene memos, readers, curve tables).
- A seeded stress harness and suites: random edit/connect/disconnect sequences, each step compared against a cache-off render. CPU on both schedulers, edits racing in-flight renders, GPU on lavapipe, and a GUI viewer/timeline script.
- Fixes for the holes found, including the two suspected ones in Phase 242.4.

**Out:**
- Making the viewer cache survive save, close and reopen, if it is by design (the M178 - Render Context Core UAT question): none. The map records whether render hashes are stable across sessions, and a hash that isn't stable is a finding here.
- Cache sizing, eviction policy and performance work, except where a fix needs it: none.
- Caches internal to OpenFX plug-ins. The host's side is in: the host must call instanceChanged and clip-change actions, and must re-key when a plug-in reports a change.
- Reading projects saved by older builds: none (no backward compatibility in this fork).
- Platforms other than Linux x86-64.

**Edge cases:**
- A disconnect while the node is rendering is queued on `guiInputs` and applied by `Node::dequeueActions` once the node's rendering count reaches zero (`Engine/NodeInputs.cpp`, `Engine/Node.cpp`). The `computeHash()` call that comes with the queued disconnect hashes the unchanged real inputs.
- An optional input (STMap/IDistort Map, any Mask) whose connection changes only the RoD/RoI and the render branch. Unconnected inputs are left out of `HashInputs` by slot, so disconnecting must still move the hash.
- Groups, GroupInput/GroupOutput, precomps and the viewer's A/B active inputs: the hash folds through redirection (`isSkippedByHashInputs`, `getHashDependents`), not through plain edges.
- A node that returns to an earlier state, such as connect → disconnect → connect, is served its old cache entries by design. The stress sequences must revisit states on purpose.
- Time: `ImageKey` drops time when `isFrameVaryingOrAnimated_Recursive()` is false (`Engine/ImageKey.cpp`). That test reads keyframes (`KnobHolder::getHasAnimation`, `Node::hasAnimatedKnob`) and frame-varying metadata, not expressions.
- Background (command-line) renders: `unsetNodeIsRendering` never emits `mustDequeueActions` there.
- Project load and close: `onInputChanged` and the hash marks return early, and are refreshed globally after load.

**Assumptions:**
- The cache-off reference is faithful: a render after `appPTR->clearAllCaches()`, `clearActionsCache()` on every node, and `byPassCache` (`Tests/RenderBothWays.h` `renderNodePlanesDirect`) gives the same pixels as a fresh load of the same project. M242.P3.T1 checks this once.
- CPU renders of the same graph state are bit-exact across schedulers and pool sizes, as `renderBothWays` already relies on. GPU-versus-CPU differences stay within `Tests/Gpu/Tolerance.h` / `Tests/Native/NativeGpuParity.h`.
- Debug builds define NDEBUG, so `assert` never proves an invariant here. Every check is a gtest expectation or a scripted GUI check.

## Phase 242.1: Cache map — keys and engine caches

- [ ] M242.P1.T1 — Write the map index and the keys family
  - files: `docs/cache-invalidation.md` (new index), `docs/cache-invalidation/keys.md` (new); read `Engine/Node.cpp` (`computeContentHash`, `markHashDirtyInternal`, `flushHashDirtyMarks`, `refreshDirtyHashes`, `getHashDependents`, `getHashInputs`), `Engine/EffectInstance.cpp` (`getHash`, `getRenderHash`), `Engine/ParallelRenderArgs.cpp` (per-frame node hash), `Engine/RenderContext.h`, `Engine/Plugin.cpp` (binary stamp)
  - approach: the index lists every cache found in Phases 242.1 and 242.2, one row each: name, owner file, key, family doc link, and status (covered / suspected hole / fixed). The rows are filled in as each family doc lands. Then write the keys doc. It covers what goes into the content hash and the render hash, when the render hash is captured relative to the frame args TLS, how dirty marks reach dependents (and where the walk stops), the M178 - Render Context Core context key, the project digest versus the frame range, and whether every input to the key is stable across sessions and AppImage mounts. Every claim cites file:symbol, and anything that can't be verified from the code goes under "Suspected holes" with its reasoning. Each family doc follows the same layout: Caches, Key, Invalidated by, Not invalidated by, Covered by tests, Suspected holes.
  - verify: a cat-scout check that every file:symbol cited in the two docs exists on the branch, and that each index row links to a doc section (rows for later tasks may say "pending").
  - size: M

- [ ] M242.P1.T2 — Map input topology edits
  - files: `docs/cache-invalidation/topology.md` (new), `docs/cache-invalidation.md`; read `Engine/NodeInputs.cpp` (`connectInput`, `replaceInputInternal`, `switchInput0And1`, `disconnectInput`, `disconnectInputInternal`, `onInputChanged`, `endInputEdition`), `Engine/Node.cpp` (`dequeueActions`, `setNodeIsRenderingInternal`, `unsetNodeIsRendering`), the `onInputChanged` overrides (`Engine/Nodes/Filter/IBlur.cpp`, `Engine/Nodes/Merge/Switch.cpp`, `Engine/Nodes/Merge/Dissolve.cpp`, `Engine/Nodes/Transform/{STMap,IDistort,CornerPin,Transform}.cpp`, `Engine/Nodes/Raw/*.cpp`, `Engine/OfxEffectInstance.cpp`)
  - approach: for each edit path, record whether it changes real inputs immediately or queues on `guiInputs`, when `computeHash()` runs relative to the real change, what `onInputChanged` refreshes (data-kind cache, mask enabledness, clip preferences, render clones, node warnings), and how groups, GroupInput/GroupOutput, precomps, the Inspector/viewer active inputs and optional inputs (Map, Mask) propagate. Spell out the queued path step by step: what a render that starts between `mustDequeueActions` and `dequeueActions` sees, and which hash its outputs are stored under. Also list node members derived from input state that live outside the hash.
  - verify: cat-scout citation check as in M242.P1.T1; the doc has a row for every function listed in `files`.
  - size: M

- [ ] M242.P1.T3 — Map the ActionsCache and per-frame metadata
  - files: `docs/cache-invalidation/actions-and-metadata.md` (new), `docs/cache-invalidation.md`; read `Engine/EffectInstancePrivate.{h,cpp}` (`ActionsCache`), `Engine/EffectInstance.cpp` (`onNodeHashChanged`, `getMetadata`, `getRenderMetadata`, `clearActionsCache`), `Engine/NodeMetadata.{h,cpp}`, `Engine/Node.cpp` (`invalidateEffectiveOutputDataKindCache`, `onMetadataMayHaveChanged`)
  - approach: list every result `ActionsCache` keeps: identity, components needed, RoD, frames needed, time domain, metadata per (hash, time, view), pass-through identity, time-invariant metadata, range summary, metadata generation. For each, record its key and how many hashes it retains (`maxAvailableHashes`, and the M178 - Render Context Core context sizing). Note which results ignore the hash argument and read live state instead: for example, `STMap::getRegionOfDefinition` takes `U64 /*hash*/` and reads `getInput()` live. Record what `moveMetadataGeneration` invalidates, whether a metadata read during a render can see a newer graph than its frame args, and the effective-data-kind cache.
  - verify: cat-scout citation check; every `ActionsCache` get/set pair in `EffectInstancePrivate.h` has a row.
  - size: M

- [ ] M242.P1.T4 — Map the image caches
  - files: `docs/cache-invalidation/image-caches.md` (new), `docs/cache-invalidation.md`; read `Engine/Cache.{h,cpp}`, `Engine/ImageKey.{h,cpp}`, `Engine/DeepImageKey.{h,cpp}`, `Engine/EffectInstanceRenderRoI.cpp` (cache lookup, `shouldCacheOutput`, `isFrameVaryingOrAnimated`), `Engine/EffectInstance.cpp` (`isFrameVaryingOrAnimated_impl`), `Engine/FrameRenderContext.{h,cpp}` (`FrameStore`), `Engine/DiskCacheNode.cpp`, `Engine/AppManager.cpp` (`clearAllCaches`, `removeAllCacheEntriesForHolder`)
  - approach: for `NodeCache`, `DiskCache`, `DeepImageCache`, `FrameStore` and DiskCache nodes, record the key fields and what each field omits: time when the node is not frame-varying, mipmap level, layers, draft. Also record when an entry stored by a render whose graph changed mid-flight can be served afterwards, holder-based removal, and what persists across sessions (`NATRON_CACHE_VERSION`, disk cache). Note specifically whether an expression that reads `frame` with no keyframes makes a node frame-varying.
  - verify: cat-scout citation check; every field of `ImageKey` and `FrameStore::TaskKey` has a row.
  - size: M

## Phase 242.2: Cache map — display, GPU and node-private caches

- [ ] M242.P2.T1 — Map the viewer cache and the timeline cached-frames line
  - files: `docs/cache-invalidation/viewer-and-timeline.md` (new), `docs/cache-invalidation.md`; read `Engine/ViewerInstance.cpp` (viewer hash, `FrameKey` construction, `getRenderViewerArgsAndCheckCache`, the hash-changed redraw check), `Engine/FrameKey.{h,cpp}`, `Engine/ViewerHandoff.{h,cpp}`, `Gui/ViewerGL*.cpp` (last rendered images), `Gui/TimeLineGui.cpp`, `Gui/ViewerTab40.cpp`, `Engine/OutputSchedulerThread.cpp` (playback viewer hash)
  - approach: record the `FrameKey` fields (tree version = the viewer's content hash folding its active inputs, gain, gamma, display transform, channels, mipmap level, draft) and when playback captures the viewer hash relative to the edits made during it. Record what the GL viewer keeps on screen between renders and when it is replaced, and what the timeline line lists after M178 - Render Context Core's fix (frames under the viewer's current hash). Check the line on: an edit, a cache clear, LRU eviction, and an edit made during playback.
  - verify: cat-scout citation check; every `FrameKey` field has a row.
  - size: M

- [ ] M242.P2.T2 — Map GPU caches and residency
  - files: `docs/cache-invalidation/gpu.md` (new), `docs/cache-invalidation.md`; read `Engine/Gpu/GpuTexture.{h,cpp}` (`GpuTextureCache`), `Engine/Nodes/Image/CurveSnapshot.h` (`CurveTablesCache`), `Engine/Gpu/GpuTransfer.cpp` (host import cache), `Engine/Gpu/GpuBufferPool.cpp`, `Engine/HostImageMemory.cpp` (pool, free hooks), `Engine/GpuRegionStrips.{h,cpp}`, `Engine/GpuRegionTask.cpp`, `Engine/Nodes/Image/PassExecutor.{h,cpp}`, `Engine/Gpu/GpuPipelineCache.cpp`
  - approach: for each GPU cache, record the key and whether the key "identifies the description and the texels", as `GpuTextureCache` requires. For example, check whether a curve table keyed by anything short of its content can be served after a curve edit. Record whether a host import keyed by address can outlive a buffer the pool reuses for different content, and when region-resident device images are dropped relative to a graph edit. Also record whether a GPU result can enter the image cache or `FrameStore` under a key the CPU path would compute differently.
  - verify: cat-scout citation check; every cache class in the listed files has a row.
  - size: M

- [ ] M242.P2.T3 — Map expression, link and metadata-reader invalidation
  - files: `docs/cache-invalidation/expressions-and-links.md` (new), `docs/cache-invalidation.md`; read `Engine/Knob.h`/`Engine/KnobImpl.h` (`_exprRes`, `clearExpressionsResults`, `clearExpressionsResultsIfNeeded`), `Engine/Knob.cpp` (expression dependencies, `getContentHash`), `Engine/KnobLinkGroup.{h,cpp}`, `Engine/Node.cpp` (`getKnobsContentDigest`, `appendMetadataSources`, `getMetadataReaders`, `mayHaveKnobExpressions`), `Engine/PyExprUtils.cpp`
  - approach: record what moves a node's hash when a knob it reads through an expression or a link changes on another node, when a renamed node changes what an expression resolves to, and how metadata-reading expressions register as hash sources. Record when `_exprRes` is cleared: only in `onNodeHashChanged` and only if `mayHaveKnobExpressions()`. That leaves the question of whether a link alone, with no expression, keeps stale results. Also record what an expression on `frame` or `time` does to caching (cross-reference M242.P1.T4).
  - verify: cat-scout citation check.
  - size: M

- [ ] M242.P2.T4 — Map node-private caches
  - files: `docs/cache-invalidation/node-private.md` (new), `docs/cache-invalidation.md`; read `Engine/Nodes/Scene/SceneLayerMemo.h`, `Engine/Nodes/Scene/SceneCaches.h`, `Engine/Nodes/Scene/USDRead.cpp`, `Engine/Nodes/IO/NativeRead.cpp`, `Engine/Nodes/IO/OiioReadSupport.cpp`, `Engine/Nodes/IO/RawDecodeSupport.cpp`, `Engine/FileStampRegistry.{h,cpp}`, `Engine/RotoItem.cpp`/`Engine/RotoStrokeItem.cpp` (stroke marks); `grep -rn "mutable\|static thread_local" Engine/Nodes` for members derived from inputs or knobs
  - approach: list every cache a node keeps in its own members: layer memos, query stages, decoded-file caches, file stamps, roto geometry, and members refreshed in `onInputChanged` or `knobChanged`. For each, record its key and whether it can disagree with the node's content hash after an edit, an input change or a file change on disk.
  - verify: cat-scout citation check; every hit of the grep in `files` is either listed or marked "not a cache" with a reason.
  - size: M

## Phase 242.3: Stress tests

- [ ] M242.P3.T1 — Write the seeded cache stress harness
  - files: `Tests/CacheStress.h`, `Tests/CacheStress.cpp` (new), `Tests/CacheStressSelf_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: a seeded generator of graph operations: connect, disconnect, replace and swap inputs (optional inputs included), set a value, add or remove a keyframe, set or clear an expression (including one on `frame`), link or unlink a knob, enable or disable a node, move the project frame range, change the time, and step back to an earlier state on purpose. After each step it renders the probe nodes cached: `renderNodePlanesDirect(..., byPassCache=false)` and `renderNodePlanesInMode` on the Task graph. It then renders a reference after `appPTR->clearAllCaches()` and `clearActionsCache()` on every node, with `byPassCache`, and compares bit-exactly. On a mismatch it reports the seed, the step and the op log, and replays a single seed from `NATRON_CACHE_STRESS_SEED`. The step budget is fixed per test so a ctest leg stays bounded. The self-test checks three things: a test-only effect that caches its output in a member keyed on nothing is caught; a hole-free graph passes; and for one sample graph the reference matches a render of the same project saved and loaded into a fresh app (which checks the Assumption).
  - verify: `CacheStressSelf_Test` passes; the planted hole is reported with a replayable seed.
  - size: L

- [ ] M242.P3.T2 — CPU topology and knob stress suite
  - files: `Tests/CacheStress_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: native graphs built on the harness. (1) STMap with UV, Source, Mask and Map, the Map toggled often. (2) IDistort and IBlur with a Map. (3) Merge, Switch and Dissolve with growing inputs. (4) Grade → Blur → Transform chains with expressions and links across nodes. (5) The same chain inside a Group, edited through GroupInput/GroupOutput. (6) A viewer-like A/B probe. Each runs a few fixed seeds on Legacy and Task graph. A mismatch is a finding. Record it in the family doc's "Suspected holes", and disable that case with a `DISABLED_` prefix that names the fix task, so the suite stays green until the fix lands.
  - verify: the suite runs under ctest, and every disabled case is listed in a family doc with a reason.
  - size: M

- [ ] M242.P3.T3 — Stress edits racing in-flight renders
  - files: `Tests/CacheStressConcurrent_Test.cpp` (new), `Tests/CMakeLists.txt`, a test-only gate effect beside `Tests/CountingTestEffect.h` if none fits
  - approach: one or more abortable renders of the probe run on worker threads while the main thread applies connect, disconnect and knob edits. This exercises the `isNodeRendering()` → `guiInputs` queue and `dequeueActions`, both sides of every input edit (the gate effect holds a render open across it), and the restart of a render that starts while a dequeue is pending. After the renders settle and events are processed, the cached render must match the cache-off reference. Revisiting the state from before the edit must also match, which catches entries stored under the wrong hash. Record failures as in M242.P3.T2.
  - verify: the suite runs under ctest with a fixed seed set; the failures it finds are recorded.
  - size: L

- [ ] M242.P3.T4 — GPU stress suite on lavapipe
  - files: `Tests/Gpu/CacheStressGpu_Test.cpp` (new), `Tests/Gpu/CMakeLists.txt`
  - approach: the M242.P3.T2 STMap/IDistort/Grade graphs with GPU placement on (no `GpuPlacementOffGuard`), GPU strips and residency included. Cached GPU renders are compared against a CPU cache-off reference using the tolerance from `Tests/Gpu/Tolerance.h` / `Tests/Native/NativeGpuParity.h` (a bit-exact check against the GPU cache-off render too, where kernels are deterministic). The suite also edits a curve or LUT knob between steps to exercise `GpuTextureCache`. It skips cleanly without a device, the way the other `Tests/Gpu` suites do.
  - verify: passes on lavapipe in PR CI; findings recorded as in M242.P3.T2.
  - size: M

- [ ] M242.P3.T5 — Viewer and timeline GUI check
  - files: `Tests/gui/cache_invalidation_viewer.py` (new), its fixture under `Tests/gui/`
  - approach: the user's scenario run under Xvfb at 1920x1080 with the `Tests/gui` runner: a CheckerBoard, a ramp UV and a Map feed STMap, which feeds the Viewer. Disconnect the Map while idle, during a scrub and during playback, then reconnect it. Each time, read the viewer's displayed pixels back and compare them with a fresh render of the same state. Check that the timeline cached-frames line clears frames that are no longer valid. Take screenshots before and after each edit.
  - verify: the script passes under Xvfb; screenshots shared with the user.
  - size: M

## Phase 242.4: Fixes

- [ ] M242.P4.T1 — Never apply queued input edits while a render holds the node
  - files: `Engine/Node.cpp` (`dequeueActions`, `setNodeIsRenderingInternal`, `unsetNodeIsRendering`), `Engine/NodeInputs.cpp` (the `useGuiInputs = isNodeRendering()` sites), `Tests/QueuedInputEdit_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: suspected hole, found while planning. A render that starts after `mustDequeueActions` is emitted but before the main thread runs `dequeueActions` passes the `nodeIsDequeuing` wait (the flag isn't set yet). `dequeueActions` then swaps `_imp->inputs` under it. The render captured its node hash at start, and STMap (like every node) reads `getInput()` live. So a frame can render the new topology and be stored under the old content hash, and connect → disconnect then serves the image rendered with the Map. Write the test first, using the deterministic interleaving: hold a `RenderingFlagSetter`, queue a disconnect, release it, take a new `RenderingFlagSetter` before processing events, then process events. Then make the queue's apply step and a render's start exclusive under one lock: `dequeueActions` defers while any render holds the node, and the next `unsetNodeIsRendering` re-emits. Also cover background mode, where nothing ever dequeues (dequeue there, or never queue when `appPTR->isBackground()`). If the test passes on unmodified code, keep it, record the path as covered in `topology.md`, and change no engine code.
  - verify: `QueuedInputEdit_Test` — real inputs never change while a rendering flag is held, the queued edit is applied once the flag is released, and the image stored by the overlapping render matches the cache-off render of the state its hash names; M242.P3.T3 passes with its related cases re-enabled.
  - size: L

- [ ] M242.P4.T2 — Make expression time-dependence frame-varying for the image cache
  - files: `Engine/EffectInstance.cpp` (`isFrameVaryingOrAnimated_impl`), `Engine/Node.cpp` (`hasAnimatedKnob`), `Engine/Knob.cpp` (an expression-reads-time query), `Tests/ExpressionFrameVarying_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: suspected hole, found while planning. `ImageKey` drops time when a node isn't frame-varying or animated, and that test reads keyframes and metadata only. A Grade whose `white` is `frame / 10`, with no keyframes over a static Constant, could therefore serve frame 1's image at frame 2. Write the test first: render frames 1 and 2 cached and compare each with the cache-off render. If it fails, treat any knob with an expression as frame-varying, the way `computeContentHash` treats `hasExpression` for the frame range, unless the expression is known not to read time. A narrower read-set test is fine if M178 - Render Context Core's read-sets already report `frame`. If the test passes, keep it and record why in `image-caches.md`.
  - verify: `ExpressionFrameVarying_Test` — cached frames 1 and 2 match their cache-off renders, in both schedulers; a node with no expression and static inputs still shares one entry across frames.
  - size: M

- [ ] M242.P4.T3 — Triage the map and stress findings into fix tasks
  - files: `docs/cache-invalidation/*.md` (status column), `docs/cache-invalidation.md`, this milestone file (new tasks M242.P4.T4 onward, inserted by the PM)
  - approach: collect every "Suspected holes" entry and every disabled stress case. Confirm or dismiss each by reading the code or running a single-seed replay. Group the confirmed holes by root cause, and write one fix task per cause in this file's format: files, approach, a verify step that re-enables the stress case or adds a deterministic test, and a size. Mark each dismissed entry in the map with its reason. Put any fix that needs a design choice (for example, snapshotting inputs per frame instead of locking) to the user before writing its task.
  - verify: no "Suspected holes" entry or `DISABLED_` stress case is left without a fix task ID or a recorded dismissal (a cat-scout check over the docs and `Tests/`).
  - size: L

**Verification gate:** release gate green on both schedulers, and the debug PR-CI leg green (asserts on); `CacheStressSelf_Test`, `CacheStress_Test`, `CacheStressConcurrent_Test` and `QueuedInputEdit_Test` pass with no `DISABLED_` cases left; `CacheStressGpu_Test` passes on lavapipe; 50 extra random seeds of each stress suite pass locally; `cache_invalidation_viewer.py` passes under Xvfb at 1920x1080, with screenshots shared and the user's OK; every row of `docs/cache-invalidation.md` is "covered" or "fixed", with the tests that cover it.
