# M260 - Cache Invalidation Follow-ups

Closes the holes M242 - Cache Invalidation Audit left open (`docs/cache-invalidation.md`, follow-up list). All were re-checked on main at 50ee71b60. A Read pins to the file as it was last loaded or reloaded. Scene layers and USDRead closures get keys that match what was authored and read. Input edits mark the hash before any render or `onInputChanged` can see them. GPU imports only cache memory whose free they see. The keying and expression gaps close. The timeline line marks only frames the viewer would serve. Every fix enables or adds a CacheStress case (`Tests/CacheStress*.cpp`; empty renders are valid references).

## Scope

**In:**
- N3, N1, N2, T8, T4, T3, T5, G1, G2, I3, I4, E2 (listener path), E3, A4 (the frame on disk), A5, K3, K4, K5, K6, K7, V2, V3.
- Updating the family docs under `docs/cache-invalidation/` and the follow-up table as each hole closes.
**Out:**
- K8: a failed `statx` on `/proc/self/exe` (Engine/Plugin.cpp:72-74) only causes misses. M242 classed it as safe, so it gets no change.
- E1 and its metadata residual (M72).
- K1 and `DISABLED_KnobEditsDuringRender` (M202's gate). Re-enabling that case is not this milestone's work.
- Watching files on disk, in any form (Decisions).
- macOS/Windows (the fork is Linux x86-64 only).
**Edge cases:**
- A file rewritten in place, replaced by rename, or deleted after its stamp: the Read reports "changed on disk since it was loaded, Reload to read it" for anything not already cached. It never mixes old and new.
- Frames of a sequence first touched after a rewrite are stamped at first use, which is correct under the stamp model.
- USDRead with a sublayer that is missing at load: it is stamped as missing, and Reload picks it up once it appears.
- Group connection edits queued during a render (T5) must mark the group as well as its GroupInput and GroupOutput.
**Assumptions:**
- Stat before open and stat after the read (mtime ns, size, inode) catches every in-place rewrite that matters. The alternative is pinning through an OIIO IOProxy on the opened fd.
- The 1 MiB malloc path and mmapped disk-cache images are the only host sources the free hook misses (gpu.md G1).
- I4 is unverified: P5.T2 starts with a CacheStress case and fixes only if the case fails.

## Decisions

- 2026-10-27 — PM: T5 becomes the real group bug — removing a GroupInput returns early in NodeGroup::notifyNodeDeactivated without syncing the GUI list, so the next dequeue restores it (nothing queues group connection changes, so the planned test can't fail). Clear All Caches does not re-stamp files: only Reload re-reads (user's rule), so a changed file errors after Clear Caches until Reload. The A/B switch marks inside refreshActiveInputs so the viewer's own onInputChanged runs under the new hash.
- 2026-10-27 — PM: a Write frame whose render args moved mid-render (A4) is re-rendered once under fresh args, then fails with an error if they move again. A pinned Read's "changed on disk since it was loaded, Reload to read it" error text goes to the user at UAT.
- 2026-10-27 — **Reload is the recheck (user):** Natron never watches files on disk. For N3, a Read pins to the file as it was when last loaded or reloaded: the OIIO header and Raw decode caches must not pick up a changed file on their own; only Reload re-reads it (and moves the node hash). Old header plus new pixels must never mix.

## Phase 260.1: Read file pinning

- [x] M260.P1.T1 — FileStampRegistry records each file's identity, not only its hash
  - files: Engine/FileStampRegistry.h, Engine/FileStampRegistry.cpp
  - approach: Add `struct FileIdentity { bool exists; int64 mtimeSec, mtimeNsec; uint64 size, inode; }` and `FileIdentity identity(const std::string& path)`. It is recorded on first use under the same generation guard as `stamp()` (FileStampRegistry.cpp:111-131). `sequenceStamp()` records the identity of each member file it stats (:94). `invalidate()` drops identities along with stamps. Add `bool matchesDisk(path, FileIdentity*)`, which stats now and compares. `computeStamp` (:41-61) hashes the same record, so the two can never disagree.
  - verify: gtest in Tests/FileStampRegistry_Test.cpp (new or existing). Identity is stable until `invalidate`. An in-place rewrite and a rename-replace each fail `matchesDisk`. A sequence's members get identities.
  - size: M
- [x] M260.P1.T2 — OIIO reader pins to the stamped identity
  - files: Engine/Nodes/IO/OiioReadSupport.cpp, Engine/Nodes/IO/OiioReadSupport.h
  - approach: Key `g_cache` (:63-80) on the registry's `FileIdentity`, inode included, instead of a fresh `last_write_time` and `file_size` (:146-147). `readHeader` fails with the "changed on disk, Reload" error when `matchesDisk` is false, and never re-parses. `decode` (.h:274) stats before open and after the read and fails the same way on a mismatch. Only `evictHeaders` (called from Reload, NativeRead.cpp:628) lets a new header in.
  - verify: gtest. Write an EXR, read the header and a tile, rewrite it at another size, then `readHeader` and `decode` both fail. After `invalidate` and `evictHeaders` they return the new file. CacheStress primitives `ePrimRewriteFile` and `ePrimReload` (Tests/CacheStress.h:134) on a NativeRead graph give a new case, `CacheStressTest.ReadPinnedUntilReload`. Its `compare` hook (CacheStress.h:186) accepts only the pre-rewrite reference or an empty render between rewrite and Reload, then an exact match after Reload.
  - size: M
- [x] M260.P1.T3 — Raw decode caches pin to the stamped identity
  - files: Engine/Nodes/IO/RawDecodeSupport.cpp
  - approach: Same as T2 for `FileKey` (:80-88) and `statFile` (:146-160). The key becomes the registry identity, inode included. A mismatch fails before decoding. `evict` (:906) stays the only path to a new decode.
  - verify: gtest with a Raw fixture rewritten under the decoder (`setDecodedHookForTests` counts decodes): no second decode and an error until Reload. Add a Raw variant to `CacheStressTest.ReadPinnedUntilReload`.
  - size: M

## Phase 260.2: USD layers

- [x] M260.P2.T1 — N1: a scene layer is memoized only if its key still holds after authoring
  - files: Engine/Nodes/Scene/SceneOps.cpp, Engine/Nodes/Scene/SceneLayerMemo.h, Engine/Nodes/NativeEffectBase.cpp
  - approach: `SceneLayerMemo::produce` (SceneOps.cpp:1040-1136) computes `layerHash` before `author` (:1114) and stores it unconditionally (:1126-1132). Pass a `std::function<bool(const Key&)> keyStillValid` that `NativeEffectBase::emitSceneLayer` (:948-961) builds by re-running `getSceneLayerKey` (:903-945) and comparing. If the key moved, return the layer without memoizing it (`valid` stays false), as `CurveTablesCache` does.
  - verify: Tests/Scene/SceneLayerMemo_Test.cpp: an author callback edits a knob mid-author, the memo does not serve that layer for the old key, and undo re-authors. CacheStress: add `CacheStressConcurrentTest.SceneKnobEditDuringAuthoring` (a USDPrim graph with knob edits during evaluation, reference from empty caches).
  - size: M
- [x] M260.P2.T2 — N2: USDRead stamps its closure when the file is first loaded
  - files: Engine/Nodes/Scene/USDRead.cpp
  - approach: `closureDigest` is 0 until the first `purgeCaches` (:107-115, :256-284). Factor the stamp loop (:270-281) into `stampClosure()`. On File knob change and on project load (`knobChanged` :206, plus the load hook), outside any render, `FindOrOpen` the file, `setClosure`, `stampClosure()` and `markHashDirty`. Rendering keeps not re-keying (the comment at :107-110 holds).
  - verify: gtest in Tests/Scene: a sublayer is rewritten between two app instances, and the node hash in the second session differs from the first with no Reload. CacheStress: add `CacheStressSelfTest`-style case `USDReadSublayerChangedWhileClosed` using the save, rewrite, fresh-load reference path (`TheReferenceEqualsAFreshAppLoadOfTheSavedProject`, CacheStressSelf_Test.cpp:196) with a persisted DiskCache.
  - size: M

## Phase 260.3: Topology and hash ordering

- [x] M260.P3.T1 — T8 and T4: mark the hash inside the render-start lock, right after the input write
  - files: Engine/NodeInputs.cpp, Engine/Node.cpp
  - approach: In `connectInput` (NodeInputs.cpp:832-850), `replaceInputInternal`, `switchInput0And1`, both disconnects and `dequeueActions` (Node.cpp:8072-8091), call `markHashDirty()` while `lockRenderStarts` is still held, after the `inputs` write. The trailing `computeHash()` calls (NodeInputs.cpp:870, :962, :1065, :1177, :1245; Node.cpp:8105) then come after `onInputChanged` and become redundant; drop them. `markHashDirtyInternal` (Node.cpp:1408-1436) takes only the queue mutex, so it is safe under the lock. Because the hash is already dirty when `onInputChanged` runs, its `buildArgsMap` reads the new hash (fixes T4).
  - verify: Tests/QueuedInputEdit_Test.cpp: use the input-edit hook (Node.cpp:1492-1497) to start a render between write and mark; the render's `nodeHash` is the new one. CacheStress: CacheStressTestEffect stores an ActionsCache answer from `onInputChanged`. Add `CacheStressConcurrentTest.OnInputChangedUnderTheNewHash` with connect, revert and render.
  - size: M
- [x] M260.P3.T2 — T3: the inspector changes its active inputs before it marks
  - files: Engine/NodeInputs.cpp
  - approach: In `InspectorNode::setActiveInputAndRefresh` (:1842-1858), call `onInputChanged(inputNb, isASide)` first, then `computeHash()`. The mark then follows every change to `_activeInputs`.
  - verify: `CacheStressTest.ViewerABSwitch` (CacheStress_Test.cpp:553) plus a concurrent variant `CacheStressConcurrentTest.ViewerABSwitchDuringRender`.
  - size: S
- [ ] M260.P3.T3 — T5: queued group connection changes count as a change
  - files: Engine/NodeGroup.cpp, Engine/NodeGroup.h, Engine/Node.cpp
  - approach: Make `NodeGroup::dequeueConnexions` (NodeGroup.cpp:1397-1403) return true when `inputs` or `outputs` differ from the gui lists. `dequeueActions` (Node.cpp:8056-8059) ORs that into `hasChanged`.
  - verify: gtest: add a GroupInput to a group during a render, then dequeue, and the group's hash moves. CacheStress: `CacheStressConcurrentTest.GroupConnectionsDuringRender` on the group graph of `GradeBlurTransformInsideAGroup` (CacheStress_Test.cpp:515).
  - size: M

## Phase 260.4: GPU caches

- [ ] M260.P4.T1 — G1: cache host imports only for memory whose free the cache sees
  - files: Engine/Gpu/GpuTransfer.h, Engine/Gpu/GpuTransfer.cpp, Engine/GpuRegionTask.cpp
  - approach: Add a per-call `cacheImport` flag to the upload and download options. `resolveImports` (GpuTransfer.cpp:800-900) takes the cached path only when the flag is set and otherwise takes the uncached path (:825-840). GpuRegionTask sets it at :1153 and :2258 only when `HostImageMemory::bytesFrom(data) >= bytes` and `bytes >= kMinPooledBytes` (HostImageMemory.cpp:57, :489-491, :547-549). That excludes small mallocs and mmapped disk-cache images (CacheEntry.h:428).
  - verify: Tests/Gpu/GpuTransfer_Test.cpp: upload from a malloc'd page-aligned buffer, free it, reallocate at the same address, and the import cache count (`cachedImports`) never held it. CacheStress: `CacheStressGpuTest` (Tests/Gpu/CacheStressGpu_Test.cpp) adds a DiskCache-backed source case.
  - size: M
- [ ] M260.P4.T2 — G2: the shader gate keys rejections by device
  - files: Engine/Nodes/Image/StagePlan.cpp
  - approach: In `StageGpuShaderGate::accept` (:1635-1697), resolve the runtime first and prefix `key` (:1639-1641) with the pipeline cache's device tag (Engine/Gpu/GpuPipelineCache.h:86).
  - verify: gtest with two device tags: a shader rejected on one is still tried on the other. CacheStress: `CacheStressGpuTest.DeviceSwitchRetriesRejectedShader`.
  - size: M

## Phase 260.5: Keying, expressions and metadata

- [ ] M260.P5.T1 — E3: expression results are stored unclamped
  - files: Engine/KnobImpl.h
  - approach: In both `getValueFromExpression` bodies (:482-560, :596-660), store the raw result and clamp on return, both after a cache hit and after evaluating (:543, :655).
  - verify: gtest: an expression outside min/max, read clamped then unclamped and the reverse, returns the right value each time. CacheStress: `CacheStressTest.ExpressionOutsideRange`.
  - size: S
- [ ] M260.P5.T2 — I4: a colour-layer conversion only from a superset
  - files: Engine/EffectInstance.cpp, Tests/CacheStress_Test.cpp
  - approach: First add `CacheStressTest.RgbThenRgbaRequest` (render RGB, then request RGBA from the same node). If it fails, restrict `convertible` (EffectInstance.cpp:2192-2200) to cached components that contain the requested ones. If it passes, record that I4 holds and change no engine code.
  - verify: the new case passes in Legacy and Task graph.
  - size: M
- [ ] M260.P5.T3 — E2: rename rewrites listeners identifier-aware
  - files: Engine/NodeName.cpp, Engine/Knob.cpp, Engine/Knob.h
  - approach: NodeName.cpp:474 calls the plain `replaceNodeNameInExpression` (Knob.cpp:3467-3497, `QString::replace` at :3490). Switch it to `replaceNodeNameInExpressionCode` (:3500) and delete the plain variant (no backward compat). After the rename, call `getApp()->recheckInvalidExpressions()`.
  - verify: Tests/ExpressionFrameVarying_Test.cpp (:202) and a new case: renaming `Blur1` leaves `Blur10.size` untouched. CacheStress: add op `eOpRename` (renames an expression source) to `defaultOps()`.
  - size: M
- [ ] M260.P5.T4 — A4: a non-abortable render does not write a frame of mixed states
  - files: Engine/EffectInstance.cpp, Engine/ParallelRenderArgs.h, Engine/Nodes/IO/NativeWrite.cpp
  - approach: M202's `allowsStoreUnder` already keeps the frame out of the caches, so this task covers the frame written to disk. At EffectInstance.cpp:7525-7531, when the render cannot abort, set `frameArgs->stateMoved` instead of silently continuing. NativeWrite re-renders a frame whose args moved, once under fresh args, and fails it with an error if they move again.
  - verify: `CacheStressConcurrentTest.MetadataEditDuringWrite`: the written file equals the pre-edit or post-edit reference, never a mix.
  - size: M
- [ ] M260.P5.T5 — A5: same-hash metadata changes also drop downstream action results
  - files: Engine/Node.cpp
  - approach: `onMetadataChangedUnderSameHash` (:1893-1921) also calls `clearActionsCache()` on each visited output and on the node itself. Today's callers (NativeRead.cpp:630, NativeWrite.cpp:1587) also move the hash, so the gap is latent.
  - verify: gtest via the RawKindFramework_Test.cpp:132 path: a downstream RoD is recomputed after `invalidateOutputMetadata` with the hash unchanged. CacheStress: a scripted case using that test effect.
  - size: M
- [ ] M260.P5.T6 — K6: frame-range folding and marking use the same predicate
  - files: Engine/Node.cpp
  - approach: `dependsOnProjectFrameRange` (:1051-1070) counts any knob with an expression, while `computeContentHash` (:1036) folds the range only for render-hashed ones. Restrict the former to `isRenderHashed()` knobs.
  - verify: gtest: a range edit with an expression only on a non-hashed knob does not mark. Run the existing `eOpFrameRange` walks with `NATRON_CACHE_STRESS_SEEDS=8`.
  - size: S
- [ ] M260.P5.T7 — K7: render-affecting Settings fold into the project digest
  - files: Engine/Settings.cpp, Engine/Settings.h, Engine/Project.cpp
  - approach: Add a render-affecting flag on Settings knobs. `Project::getContentDigest` folds a digest of the flagged knobs. `invalidateAllHashes` (Settings.cpp:2402) stays for the unflagged ones. Flag every Settings knob read from Engine render paths (grep `getSettings()->` under Engine/ render code).
  - verify: gtest: changing a flagged knob changes `getContentDigest`. CacheStress: `CacheStressTest.RenderSettingEdit`, if a flagged knob exists.
  - size: M
- [ ] M260.P5.T8 — K3: CacheStress sweep over non-hashed knobs
  - files: Tests/CacheStress_Test.cpp, Tests/HashPluginSweep_Test.cpp
  - approach: For each native plug-in, change each knob for which `isRenderHashed()` (Knob.cpp:4276-4283) is false, and compare the cached render with an empty-cache render. Fix any knob found by making it persistent or evaluate-on-change.
  - verify: `CacheStressTest.NonHashedKnobSweep` passes.
  - size: M
- [ ] M260.P5.T9 — K4: dynamic expression reads become dependencies
  - files: Engine/Knob.cpp, Engine/KnobImpl.h
  - approach: During `evaluateExpression`, record on the thread every knob read through the Python value getters. After evaluation, register them as listeners and hash dependencies (`getContentHashDependencies`, Knob.cpp ~7117), merged with the parser-found set. The fixed token list (:6774-6781) stays for volatility.
  - verify: gtest: an expression using `getattr(app, "Bl"+"ur1")` moves its node's hash when Blur1 changes. CacheStress: add op `eOpDynamicExpression` to `defaultOps()`.
  - size: L
- [ ] M260.P5.T10 — K5: audit node state outside knobs
  - files: Engine/Nodes (audit), the overrides found
  - approach: Walk the 12 `appendNodeContentHash` hits and the node-private grep audit (docs/cache-invalidation/node-private.md). For each node with non-knob state read while rendering, add an override plus `markHashDirty` on change. Record the result in node-private.md.
  - verify: a gtest per new override (changing the state moves the hash). CacheStress: `CacheStressTest.AddLayersRegistryChange` for the existing pattern.
  - size: M
- [ ] M260.P5.T11 — I3: unique, persisted cache IDs
  - files: Engine/NodePrivate.h, Engine/Node.cpp, Engine/NodeSerialization (save/load)
  - approach: `cacheID` is the first script name (NodePrivate.h:377-381), unique only within a project. Make it a UUID made at creation and serialized. Load needs no fallback.
  - verify: gtest: two projects with same-named nodes have distinct IDs, and removing one holder's entries leaves the other's. CacheStress: extend `TheReferenceEqualsAFreshAppLoadOfTheSavedProject` with a second same-named project on a shared DiskCache.
  - size: M

## Phase 260.6: Timeline cache line

- [ ] M260.P6.T1 — V2: the line counts only entries the viewer would serve
  - files: Engine/AppManager.cpp, Engine/AppManager.h, Gui/TimeLineGui.cpp
  - approach: `getViewerCachedFrames` (AppManager.cpp:2642-2660) filters only on tree version, mipmap and holder. Pass a filter from the viewer's current view, input name, layer, channels, alpha channel and draft=false (FrameKey.h fields :87-102), and drop non-matching keys. A frame needs tiles covering its texture bounds (`getTexRect`), or it is not counted.
  - verify: `CacheStressTest.TimelineLineMatchesLookups`: after each step, every frame the line marks must hit a viewer-cache lookup under the current settings.
  - size: M
- [ ] M260.P6.T2 — V3: the line refreshes when a render ends without an upload
  - files: Gui/ViewerGL.cpp, Gui/ViewerTab40.cpp, Gui/TimeLineGui.cpp
  - approach: Today `refreshTimelineCacheLineIfStale` is called only from uploads (ViewerGL.cpp:1646, :1927). Also call it when a viewer render ends failed, black or aborted, and on pause, through the viewer's render-finished path.
  - verify: extend Tests/gui/m178_timeline_cache_line.py: after an edit whose render fails (a Read of a missing file), the line drops the old frames. A GUI script stands in for CacheStress here because the refresh is GUI-only.
  - size: M

**Verification gate:** one build of the full tree and ctest. All new and re-enabled CacheStress cases pass with `NATRON_CACHE_STRESS_SEEDS=8` in Legacy and Task graph, plus CacheStressGpuTest on a GPU host. Run `Tests/gui/m178_timeline_cache_line.py` and `Tests/gui/cache_invalidation_viewer.py` under Xvfb. The follow-up table in docs/cache-invalidation.md and the family docs mark every hole in scope as fixed.
