# M301 - GPU Cache Tier

Add device memory as a tier of M300 - Unified Tiered Cache, so GPU-rendered node outputs are reused rather than recomputed. Today a GPU region's outputs go only to the per-frame `FrameStore` (`Engine/GpuRegionTask.cpp` `storeOutputs` ~:2853-2930). Interior members are not even stored there (~:2861). Placement's `isCached` (`Engine/GpuPlacement.cpp` ~:925) sees only the host cache, so once a chain lands on the GPU it stays uncached and every downstream edit re-runs it. `GpuBufferPool` (`Engine/Gpu/GpuBufferPool.cpp` ~:233-280) already keeps an LRU of evictable leases whose `onEvict` downloads to RAM (`evictToRam`). This milestone makes those leases cache entries, publishes region member outputs to them, and has placement serve hits from any tier.

## Scope

**In:**
- A device tier implementing M300 - Unified Tiered Cache's tier interface, inside the existing GPU pool budget (80% of the driver heap budget, capped by `gpuMemoryBudgetMB`). Render working buffers always win: cached entries are demoted or dropped first.
- Device → host demotion by M300 - Unified Tiered Cache's cost rule: download when the recompute cost exceeds the download cost, otherwise drop.
- GPU region members publish their outputs to the device tier, subject to M300 - Unified Tiered Cache's admission policy, only within the plan's pool headroom.
- Placement consults every tier. A device hit serves the node without executing it, and the region splits around served nodes; a host hit uploads.
- Partial device hits render only the missing bounding rectangle (multi-rect output is out).
- G1 (`docs/cache-invalidation/gpu.md`) is already closed (M260); a regression test for disk round-trips.
- Region outputs read back to RAM are admitted to the host tier.

**Out:**
- Host and disk tiers, the policy and priorities (M300 - Unified Tiered Cache).
- Draft behaviour and downscaling of non-scaling nodes on the device (M303 - Interactive Edit Latency).
- Node-highlight changes (M302 - Viewer Render State Cues).
- Kernel fusion (M87 - Point-Op Kernel Fusion, cancelled).

**Edge cases:**
- **Strip mode** (`GpuRegionStrips.cpp`, used on device OOM or for large regions): members' full outputs never exist at once, so a stripped run publishes nothing to the device tier; its region outputs go to the host tier.
- **Device loss or a driver reset:** the device tier is emptied and lookups fall through to the host.
- **OOM during a render:** cached device entries are evicted before the region falls back to strips or the CPU (`GpuRegionTask.cpp` ~:3360-3380).
- **The viewer hand-off root** (`ViewerInstance.cpp` ~:527-541): it is published like any member, and the hand-off reads the cached buffer.

**Assumptions:**
- Region members each write their output to a separate device buffer, with no fusion since M87 was cancelled, so publishing a member is keeping its lease rather than an extra copy. P1.T1 verifies this before anything is built on it.
- One device per render; multi-GPU placement (M239 - Weight-Based GPU Placement) keys entries per device.

## Decisions

- 2026-11-01 — **The device tier shares the existing GPU budget, and render buffers win (user).**
- 2026-11-01 — **Device caching is a tier of the unified cache, not a separate GPU cache (user):** see `DECISIONS/2026-11-01-unified-tiered-cache.md`.
- 2026-11-12 — **P1.T1 (f78e18417):** each rendering member writes its own lease and no kernel writes in place, so publishing is keeping the lease. Identity and pass-through members share the producer's slot and aren't published; strip mode holds no full output on the device and publishes nothing there; the viewer root already outlives the run. The trace found P1.T2 is two changes and P2.T3 stale (G1 closed in M260); the remaining tasks are being re-planned.
- 2026-11-12 — **Re-planned after P1.T1 (consultant):** a device-only entry lives in the device tier's own table and `peek` returns it as a stub, as the disk tier does (a RAM-less Image in the host table would be the host policy's first eviction). Downloaded region outputs go to the host tier in this milestone (P1.T5): without it strip mode, the large-frame and low-VRAM path, caches nothing. P1.T2 split into the pool's eviction classes (P1.T2) and the tier itself (P1.T4); P2.T4 added for the probe; P2.T3 shrunk to a regression test since G1 closed in M260. Both are engineering calls; the extra host RAM under GPU renders is stated in Scope.

## Phase 301.1: Device tier

- [x] M301.P1.T1 — Confirm how region members hold their outputs
  - files: `docs/cache/tiered-cache.md` (device-tier section), read-only: `Engine/GpuRegionTask.cpp`, `Engine/GpuRegionStrips.cpp`, `Engine/Gpu/GpuBufferPool.cpp`
  - approach: Trace the member-output buffers through a region run: separate leases or aliased/reused, their lifetime, and their behaviour in strip mode. Record the findings and the publish design that follows (keeping the lease vs copying), with file:line evidence.
  - verify: The design-note section answers the assumption in `## Scope` with evidence, and the remaining tasks' approach is checked against it.
  - size: M

- [ ] M301.P1.T2 — Give pool leases an eviction class and evict cached leases first
  - files: `Engine/Gpu/GpuBufferPool.h`, `Engine/Gpu/GpuBufferPool.cpp`, `Tests/Gpu/GpuBufferPool_Test.cpp`
  - approach: Each lease gets a class (required download, or cached) and a score getter next to its `onEvict`. `evict` and `acquire` (~:134-171, ~:233-273) take cached leases first, lowest score first, then required ones oldest first. A cached `onEvict` may drop the lease. No Engine headers are included; whether the score is a lease getter or a pool callback into the tier is decided here.
  - verify: `GpuBufferPool_Test`: under pressure a cached lease goes before a required one, the lower score goes first, and a dropped cached lease frees its budget.
  - size: M

- [ ] M301.P1.T4 — Add `CacheDeviceTier` with its table, cost-rule demotion and device loss
  - files: `Engine/CacheDeviceTier.h` (new), `Engine/CacheDeviceTier.cpp` (new), `Engine/Cache.h`, `Engine/GpuContext.cpp`, `Tests/Gpu/CacheDeviceTier_Test.cpp` (new)
  - approach: A `CacheTier` whose `next()` is the host tier, with its own table of `CacheDeviceEntry : AbstractCacheEntryBase` (the shared `CacheEntryHeaderPtr` plus a `shared_ptr<GpuDeviceImage>`) keyed by hash under its own lock, the way the disk tier keeps its own index. `budget()` is the pool budget minus working bytes; `insert(key, GpuDeviceImage, header)` marks the lease cached. `demote` downloads into a host Image sharing the header (`residency |= host`) when `recomputeCostNs` exceeds the download time at `GpuTransferRates` (`GpuPlacement.cpp` ~:1056), else drops. On `onDeviceLost` (`Engine/Gpu/GpuDevice.h` ~:94) the tier clears every device bit and empties the table. A `downloaded` image (which `pin()` refuses) counts as not resident.
  - verify: tier tests: an expensive entry demotes to the host with identical pixels, a cheap one drops, and device loss empties the table while the host copies survive.
  - size: L

- [ ] M301.P1.T3 — Publish whole-run region members and the viewer root to the device tier
  - files: `Engine/GpuRegionTask.cpp` (`plan`, `releaseSlotsReadLastBy`, `storeOutputs`), `Engine/GpuRegionStrips.cpp`, `Tests/GpuRegionFallback_Test.cpp`
  - approach: Set a `publish` flag on the slots of admitted non-identity members, only within the plan's headroom under the pool budget, most expensive per byte first. A published `download` slot keeps its handle; a stripped run clears every `publish` flag. The key is `outputCacheKey(...).forEntry(mip, layer, float)` (not the region's `GpuImageLayout::key`, which differs on scale-one-inputs and draft flags); the cost comes from `reportTimesByTask`, after `chargeEvenly` on devices without timestamps. Register the `deviceRoot` image under its key.
  - verify: a three-member region run whole finds every admitted member in the tier by key, with cost > 0; a stripped run and a run with zero headroom publish none; the pool's peak never passes the plan's.
  - size: L

- [ ] M301.P1.T5 — Admit region outputs read back to RAM to the host tier
  - files: `Engine/GpuRegionTask.cpp` (`storeOutputs`, the download path), `Tests/GpuRegionFallback_Test.cpp`
  - approach: Region outputs downloaded as `Image`s, from whole or stripped runs, go through M300 - Unified Tiered Cache's admission into the host tier under the same key and cost as in P1.T3.
  - verify: a stripped run's region output is a host-tier hit on the next probe, and its pixels match the render.
  - size: M

## Phase 301.2: Lookups and placement

- [ ] M301.P2.T4 — Make the non-promoting probe see device entries
  - files: `Engine/Cache.h` (`peek`), `Engine/EffectInstance.cpp` (`probeOutputInCache`), `Engine/RenderScheduler.cpp` (`pruneBehindCacheHits`), `Tests/RenderScheduler_Test.cpp`
  - approach: `peek` checks the device tier after memory and disk and returns a stub sharing the entry's header (as it already does for disk-only entries), accepted at the exact mip only. Pruning records which tier serves the task.
  - verify: with an entry only on the device, the probe reports Full, the task is `servedFromCache`, and its inputs are pruned.
  - size: M

- [ ] M301.P2.T1 — Serve device hits in place, and promote host hits that feed the GPU
  - files: `Engine/RenderScheduler.cpp`, `Engine/GpuPlacement.cpp`, `Engine/CacheDeviceTier.cpp`, `Engine/FrameRenderContext.cpp`, `Tests/GpuPlacement_Test.cpp`
  - approach: A device-served task puts the entry's `GpuDeviceImage` in the frame store and does not run `renderRoI`; a CPU consumer downloads it into the host tier. A host hit with a GPU consumer uploads through the tier's `promote` and keeps the host copy. Placement keeps served tasks out of regions, as M300 - Unified Tiered Cache already does.
  - verify: `GpuPlacement_Test`: A→B→C on the GPU with A and B on the device; after a C edit only C executes, and the output matches a cold render.
  - size: L

- [ ] M301.P2.T2 — Render the missing bounding rectangle of a partial device hit
  - files: `Engine/RenderScheduler.cpp` (~:797-805), `Engine/GpuPlacement.cpp`, `Engine/GpuRegionTask.cpp`, `Engine/CacheDeviceTier.cpp`, `Tests/GpuRegionFallback_Test.cpp`
  - approach: A GPU-capable task with a partial device hit stays a GPU candidate instead of being placed Cached → CPU. The member renders the bounding rectangle of the missing area into a lease sized to the union; a device-side copy brings in the cached part, and the entry is replaced with its bitmap updated, as `ensureBounds` does on the host.
  - verify: rendering a half RoI and then the full RoI renders only the missing half, and the output is identical to a cold full render.
  - size: L

- [ ] M301.P2.T3 — Test that a host entry round-tripped through the disk tier is re-imported correctly
  - files: `Tests/Gpu/CacheStressGpu_Test.cpp`
  - approach: G1 was closed in M260 (`docs/cache-invalidation/gpu.md` ~:99-111), so this only adds a regression case next to `DiskCacheSourcesAreNotImportedByAddress`: an entry demoted to disk, promoted into a new `HostImageMemory` buffer with changed pixels under a new key, and uploaded.
  - verify: the upload serves the new pixels.
  - size: M

## Phase 301.3: Gate

- [ ] M301.P3.T1 — Add GPU cache stress cases and run the benchmark with the GPU on
  - files: `Tests/CacheStress_Test.cpp`, `Tests/CacheStressConcurrent_Test.cpp`, `tools/bench/baselines/edit-latency.jsonl`
  - approach: Cases for device demotion during a concurrent render, device loss, and OOM with cached entries present. Run M276 - Upstream Reuse On Edits' benchmark with `NATRON_GPU` on, on the user's host, and record the result.
  - verify: The cases pass. On Grade and HSVKeyer edits after the first render, the benchmark shows only the edited node executing.
  - size: M

**Verification gate:** GPU, cache, CacheStress and `CacheDeviceTier_Test` suites pass in debug and release, and the NVIDIA runner passes too; publishing causes no new OOM retries. With the GPU on, M276 - Upstream Reuse On Edits' benchmark shows no upstream execution on a downstream edit after the first render, at 100% and at fit-to-window, and with a small `gpuMemoryBudgetMB` that forces strip mode (served through host hits).
- 2026-11-12 — P1.T2 implemented (b4adf3790): a cached lease's score is a number the tier pushes with `setCachedScore`, not a getter, so the pool never calls into the tier under its lock.
- 2026-11-12 — P1.T4 implemented (3b5523050): `Cache` gains `addDeviceTier`; device images gained cached/evictable/resident helpers and `evictInto`; `Image` gained a constructor sharing an existing header. Lock order: device image mutex → tier registry → tier lock → pool lock. Left to P1.T3: creating the app's tier, a fallback transfer rate before calibration (today an uncalibrated download prices at infinity, so demotion always drops), and `clearInRam` advancing the clear generation.
- 2026-11-12 — P1.T3 (d9e63e852), P1.T5 (57322e9c2), P2.T4 (2f7cab6e3) and P2.T1 (3eb1fb0e3) implemented. The app makes one tier per runtime (`AppManager::getDeviceCacheTier`). The uncalibrated download rate is `kUncalibratedDownloadBytesPerSecond` = 8e9. A downloaded output is published only when the transfer and compute queue families match. Device serving moves an entry to the host tier for CPU-only consumers rather than copying it, since a downloaded `GpuDeviceImage` can't be pinned again. Phase 301.1 build running.
- 2026-11-12 — P2.T2 (79724469d), the grown-entry cost and multi-holder fixes plus P2.T3 (e6bcd11f6), and P3.T1's stress cases (a0c3c508b) implemented. The stress cases live in `GpuRegionFallback_Test.cpp` (`GpuDeviceTierStressTest`), which already has region reports and tier lookups. A pooled host buffer reused at the same address maps the same pages, so it can't go stale; only `reallyFree` (pool overflow, trim, reaper) fires the import-forget hook. Full release gate running. Still left from P3.T1 and the gate: the NVIDIA runner and the GPU-on M276 benchmark on the user's host, collected at F6 - Unified Tiered Cache's UAT.
- 2026-11-13 — Gate run 43366730613 failed in 4 suites. The causes: the strip tests' 16 KiB cap was below the pool's 64 KiB minimum size class, so they fell back to the CPU; the device-serve test needed a forced CPU-device context; and the evicted viewer root and frame-store layer lost their eviction count after the tier moved their pixels (a real regression). The residency test now clears the tier before its leak check. Fixed in c5b513eaf. **Stripped runs now also admit their cacheable interior members to the host tier (PM call, from the gate):** the gate requires no upstream execution under a strip-forcing budget, and P1.T5 admitted only region outputs. Interior members are downloaded and the strips re-planned, dropping the lowest cost-per-byte downloads first if the plan wouldn't fit. In progress.
- 2026-11-13 — Gate run 43416040786 on 9696c7bdd: only `GpuDeviceServeTest.EditingTheLastNodeAfterAStrippedFrame...` failed, a test bug: a host-tier hit still enters `renderRoI` (only device hits skip it), so the `renderRoICallsOf == 0` check could never hold. It now checks 0 misses, >0 hits and nothing rendered for the interior grade (82c84b9d0). Re-gating.
