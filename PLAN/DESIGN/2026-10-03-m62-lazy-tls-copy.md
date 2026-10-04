# M62.P3.T2 — lazy per-holder TLS copy for spawned render threads

Consultant scout, 2026-10-03, on `milestone/m62-render-scaling-hotspots` with the P2.T3 lazy-setter edits in the working tree. Governs P3.T2's brief.

## Current mechanism

- `_spawns` (`TLSHolder.h:100-101,161-162`): worker `QThread*` (as `uintptr_t`) → spawner `QThread*`. Added only by `softCopy` (`TLSHolder.cpp:116-117`), called on the worker by the multithread suite at `OfxHost.cpp:1228` (pool) and `:1288` (`OfxThread`); both call sites fetch `getOFXHost()->getTLSData()` first (`:1223`, `:1285`). Erased on the worker's first TLS access (`TLSHolderImpl.h:240`) or by `cleanupTLSForThread`, which returns early without cleaning if the entry is still present (`TLSHolder.cpp:135-140`).
- Host frame threading (`EffectInstance.cpp:2314`, cleanup `:2338`) and deep chunks (`Nodes/NativeEffectBase.cpp:206`, cleanup `:214`) call the eager `AppTLS::copyTLS` (`TLSHolder.cpp:86-104`) instead.
- Registry `_object->objects`: `weak_ptr<TLSHolderBase>` set; a holder is added on first create (`TLSHolderImpl.h:185-186`, `TLSHolder.cpp:63-65`) and removed only when its per-thread map empties (`TLSHolder.cpp:165-169`). Main never cleans up, so N is every effect/knob/clip/param holder that ever had data on main.
- `copyTLSFromSpawnerThreadInternal` (`TLSHolderImpl.h:263-282`) visits every registered holder; only `TLSHolder<EffectTLSData>` copies anything (`:48-77`, deep copy via copy ctor `:63`); the generic `copyTLS`/`copyAndReturnNewTLS` are no-ops (`:79-97`). `copyAbortInfo` (`TLSHolder.cpp:69-83`) is O(1) and stays.
- `EffectTLSData` (`EffectInstance.h:2257-2320`): `frameArgs` list of `shared_ptr<ParallelRenderArgs>` (shared, not duplicated) + `currentRenderArgs` by value + counters + `userLayerStrings`.
- The six instantiations are at `TLSHolder.cpp:192-197`. A worker reaches `EffectTLSData` (own effect via `aborted()` `EffectInstance.cpp:512`, `getCurrentTime/View` `:5658/:5681`, `tiledRenderingFunctor` `:2367-2406`; inputs via `getImage` → `getParallelRenderArgsTLS()` → `renderRoI` `EffectInstanceRenderRoI.cpp:327`; expression dependencies' `getCurrentTime`). `OfxHostTLSData`, `KnobTLSData`, `ClipTLSData`, `OfxParamTLSData`, `ProjectTLSData` are reached but never copied today (fresh per thread).

Lock order today: `_objectMutex` → `perThreadDataMutex`; `_spawnsMutex` never held with another lock. Every `getTLSData`/`getOrCreateTLSData`, even a hit, takes `_spawnsMutex` read (`TLSHolderImpl.h:137`, `:164`). A worker's first access takes `_objectMutex` write and each Effect holder's `perThreadDataMutex` write, serialising all workers' first touches app-wide.

## Design

`TLSHolderImpl.h`
- Trait `TLSInheritsFromSpawner<T>`: false by default, true for `EffectInstance::EffectTLSData`. Delete `copyTLSFromSpawnerThread[Internal]`, the `copyTLS` virtuals, `copyAndReturnNewTLS`, `canCleanupPerThreadData`.
- `getTLSData`: (1) look up `perThreadData[cur]` under the holder read lock; return on hit, never touching `_spawns`. (2) Miss and trait false → null. (3) Else `chain = appTLS->getSpawnerChain(cur)`: under `_spawnsMutex` read follow worker → spawner → …, stop at depth 16 or a repeat; release. (4) Empty chain → null. (5) Holder write lock; first thread in the chain with an entry; none → null. (6) Insert a copy for `cur`, release, record the holder in the thread-local list.
- `getOrCreateTLSData`: same; if no ancestor has data, create fresh `T` and record.
- Nested multithreading: the chain walk covers a middle worker that never touched the holder; its spawn entry now lives until its own cleanup.

`TLSHolder.h/.cpp`
- `static thread_local std::vector<TLSHolderBaseConstWPtr> tHoldersWithData`; `AppTLS::recordHolderForCurrentThread(wp)`.
- `cleanupTLSForThread`: clear abort info; under `_spawnsMutex` write erase `_spawns[cur]` with no early return; swap the thread-local list out and call `cleanupPerThreadData(cur)` on each live holder (holder write lock only).
- `softCopy(from, to)`: assert `to == currentThread()`; first purge `tHoldersWithData` (a reused pool thread whose previous task skipped cleanup would otherwise see stale data instead of copying); then `_spawns[to] = from` under write.
- Remove `_object`, `_objectMutex`, `registerTLSHolder`, `AppTLS::copyTLS` (or keep `copyTLS` as an alias of `softCopy`).
- Optional: a spawn kind in `_spawns` (`MultiThreadSuite` | `HostFrameThreading`); for frame-threading/deep-chunk workers copy only `frameArgs` and counters, leaving `currentRenderArgs`/`userLayerStrings` default, since the worker sets its own scoped render args (`EffectInstance.cpp:2416`, `EffectInstancePrivate.cpp:720-740`). This closes the race below.

Call sites
- `EffectInstance.cpp:2314` and `NativeEffectBase.cpp:206`: `copyTLS` → `softCopy`.
- `OfxHost.cpp`: move `softCopy` (`:1228`, `:1288`) ABOVE the OfxHost TLS fetch (`:1223`, `:1285`), or the purge drops the `threadIndexes` data and `multiThreadIndex` (`:1458`) returns 0.
- RAII guard `AppTLS::SpawnedThreadScope(from)` around each softCopy/cleanup pair: an exception escaping `tiledRenderingFunctor` would otherwise skip the cleanup at `:2338` and leave a stale `_spawns` entry for a reused pool thread pointing at a deleted spawner (`OfxThread` deleted at `OfxHost.cpp:1410`).

Lock order after: hit → holder read only; miss → `_spawnsMutex` read, released, then holder write; `softCopy` → holder locks one at a time, then `_spawnsMutex` write; cleanup → `_spawnsMutex` write, released, then holder locks one at a time. No lock is held while taking another.

Invariants: (1) a thread holds a holder's entry only if that holder is in its thread-local list; (2) a spawn entry lives from `softCopy` to the worker's cleanup; (3) `softCopy` is the first TLS-touching statement of a spawned task; (4) a worker copies at most once per holder it touches.

## Why the non-creating getter must copy too

Frame args live per effect in `TLSHolder<EffectTLSData>::frameArgs`: pushed by `setParallelRenderArgsTLS` (`EffectInstance.cpp:372-378`) and the setter (`:314-344`); read by `getParallelRenderArgsTLS()` (`:397-407`) via `getTLSData()`; the P2.T3 `getImage` setter checks that getter (`:1005`); `renderRoI` uses `getOrCreateTLSData` (`EffectInstanceRenderRoI.cpp:327`) and invents args with a "[BUG]" log if empty (`:332-345`). Spawner lifetime is safe: the suite blocks on `waitForFinished` (`OfxHost.cpp:1371`) / `wait()` (`:1407`), frame threading on `waitForFinished` (`EffectInstanceRenderRoI.cpp:1858`), deep chunks on `blockingMap` (`NativeEffectBase.cpp:201`); the setter pops after they return.

## Risks

- Unlocked concurrent mutation (exists today, wider window lazily): Qt lets the spawner run map items itself; in frame threading its own tile mutates `currentRenderArgs` (`EffectInstancePrivate.cpp:722-756`) while a worker's copy ctor reads it. Mitigation: the spawn-kind field-wise copy. On the suite path the spawner's data is effectively read-only during the plugin function.
- Stale pool-thread data: the purge in `softCopy`, which depends on moving `softCopy` first.
- Repeated misses walk the chain each time: bounded by depth, O(1) in N.

## Tests

Thread control: single-threaded `appPTR->setNumberOfThreads(-1)` disables the suite (`OfxHost.cpp:1344`) and frame threading (`EffectInstanceRenderRoI.cpp:1160`); multithreaded `setNumberOfThreads(0)` + `QThreadPool::globalInstance()->setMaxThreadCount(>=4)`; `appPTR->clearAllCaches()` between runs. `BaseTest::TearDown` sets threads to 0 (`BaseTest.cpp:122`). No existing test compares single- vs multithreaded output.

1. Deterministic unit test (`Tests/GraphScaling_Test.cpp`): main creates effects A and B, `setParallelRenderArgsTLS(argsA)` on A. A `QThread` calls `softCopy(main, this)`; `A->getParallelRenderArgsTLS().get() == argsA.get()`; B null. A nested `QThread` does `softCopy(worker, nested)` without the worker touching A and still sees `argsA`. After cleanup a fresh `softCopy` sees nothing stale. Test-only counter `AppTLS::getNumInheritedCopies()`: with 1000 extra effects holding frame args on main, the worker reports exactly 1 copy.
2. Pixel test, 512×512: CheckerBoard (`net.sf.openfx.CheckerBoardPlugin`) → 50 nodes alternating `net.sf.openfx.GradePlugin` (pointwise, multithread suite, `setHostFrameThreading(false)` `Grade.cpp:1244`) and `net.sf.cimg.CImgBlur` (host frame threading when built without OpenMP, `CImgBlur.cpp:241-245`; its own work is OpenMP, suite only via `OFX::PixelCopier` `CImgFilter.h:672-809`). Animate `Grade1.multiply`; every other Grade's `multiply` is an expression on `Grade1` (style of `GraphScalingExpressionDeps_Test.cpp:78`). Timeline at frame 1, render frame 10 through a Write (`startWritersRendering`, as `ChannelSetRender_Test.cpp:166-190`): a worker that loses the dependency's frame args evaluates at frame 1 and diverges. All-Grade chain compares bit-for-bit; chains with CImgBlur need a tolerance (~1e-5) because tiled blur seams may differ from an untiled render.
