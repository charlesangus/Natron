# M301 - GPU Cache Tier

Add device memory as a tier of M300 - Unified Tiered Cache, so GPU-rendered node outputs are reused rather than recomputed. Today a GPU region's outputs go only to the per-frame `FrameStore` (`Engine/GpuRegionTask.cpp` `storeOutputs` ~:2853-2930). Interior members are not even stored there (~:2861). Placement's `isCached` (`Engine/GpuPlacement.cpp` ~:925) sees only the host cache, so once a chain lands on the GPU it stays uncached and every downstream edit re-runs it. `GpuBufferPool` (`Engine/Gpu/GpuBufferPool.cpp` ~:233-280) already keeps an LRU of evictable leases whose `onEvict` downloads to RAM (`evictToRam`). This milestone makes those leases cache entries, publishes region member outputs to them, and has placement serve hits from any tier.

## Scope

**In:**
- A device tier implementing M300 - Unified Tiered Cache's tier interface, inside the existing GPU pool budget (80% of the driver heap budget, capped by `gpuMemoryBudgetMB`). Render working buffers always win: cached entries are demoted or dropped first.
- Device → host demotion by M300 - Unified Tiered Cache's cost rule: download when the recompute cost exceeds the download cost, otherwise drop.
- GPU region members publish their outputs to the device tier, subject to M300 - Unified Tiered Cache's admission policy.
- Placement consults every tier. A device hit serves the node without executing it, and the region splits around served nodes; a host hit uploads.
- Partial device hits render only the missing rectangles.
- The host-import cache's reused-address hole (G1, `docs/cache-invalidation/gpu.md`), closed for imports from cache entries.

**Out:**
- Host and disk tiers, the policy and priorities (M300 - Unified Tiered Cache).
- Draft behaviour and downscaling of non-scaling nodes on the device (M303 - Interactive Edit Latency).
- Node-highlight changes (M302 - Viewer Render State Cues).
- Kernel fusion (M87 - Point-Op Kernel Fusion, cancelled).

**Edge cases:**
- **Strip mode** (`GpuRegionStrips.cpp`, used on device OOM or for large regions): members' full outputs never exist at once, so only the region's own outputs are published, assembled from the strips.
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

## Phase 301.1: Device tier

- [x] M301.P1.T1 — Confirm how region members hold their outputs
  - files: `docs/cache/tiered-cache.md` (device-tier section), read-only: `Engine/GpuRegionTask.cpp`, `Engine/GpuRegionStrips.cpp`, `Engine/Gpu/GpuBufferPool.cpp`
  - approach: Trace the member-output buffers through a region run: separate leases or aliased/reused, their lifetime, and their behaviour in strip mode. Record the findings and the publish design that follows (keeping the lease vs copying), with file:line evidence.
  - verify: The design-note section answers the assumption in `## Scope` with evidence, and the remaining tasks' approach is checked against it.
  - size: M

- [ ] M301.P1.T2 — Make pool leases device-tier entries
  - files: `Engine/Gpu/GpuBufferPool.h`, `Engine/Gpu/GpuBufferPool.cpp`, `Engine/CacheDeviceTier.h` (new), `Engine/CacheDeviceTier.cpp` (new), `Tests/Gpu/GpuBufferPool_Test.cpp`
  - approach: A cached lease carries a `CacheEntryHeader`. Under the pool budget, working allocations evict cached entries first. Eviction calls the tier's `demote`: download to the host tier (`evictToRam` path) when the recompute cost exceeds the download cost, else release.
  - verify: `GpuBufferPool_Test` cases: a working allocation under pressure evicts a cached entry; an expensive entry is demoted to the host with identical pixels; a cheap one is dropped.
  - size: L

- [ ] M301.P1.T3 — Publish region outputs to the device tier
  - files: `Engine/GpuRegionTask.cpp` (`storeOutputs`), `Engine/GpuRegionStrips.cpp`, `Tests/GpuRegionFallback_Test.cpp`
  - approach: After a region runs, each admitted member output, interior members included, is inserted into the device tier under its `ImageKey` with its recompute cost. In strip mode only the region outputs are published.
  - verify: A GPU test renders a three-member region and finds every admitted member in the device tier by key.
  - size: L

## Phase 301.2: Lookups and placement

- [ ] M301.P2.T1 — Serve placement from every tier
  - files: `Engine/GpuPlacement.cpp`, `Engine/RenderScheduler.cpp`, `Tests/GpuPlacement_Test.cpp`
  - approach: `isCached` becomes a tier lookup. A device hit marks the task served: it is not executed, and its consumers read the cached buffer. A region splits so that served members drop out. A host hit with a GPU consumer uploads.
  - verify: `GpuPlacement_Test`: with A→B→C on the GPU and A, B cached on the device, re-rendering after a C edit executes only C.
  - size: L

- [ ] M301.P2.T2 — Render only the missing part of a partial device hit
  - files: `Engine/GpuRegionTask.cpp`, `Engine/CacheDeviceTier.cpp`, `Tests/GpuRegionFallback_Test.cpp`
  - approach: A device entry's bitmap gives the rendered rectangles. A larger RoI (pan, zoom out) renders only the missing rectangles, then grows the buffer with a device-side copy, mirroring `ensureBounds` on the host.
  - verify: A GPU test renders a half RoI, then the full RoI, and the profile shows only the missing half rendered, with output identical to a cold full render.
  - size: L

- [ ] M301.P2.T3 — Key host imports by cache entry identity
  - files: `Engine/GpuRegionTask.cpp` (`importSources` ~:1384), `docs/cache-invalidation/gpu.md`, `Tests/GpuRegionFallback_Test.cpp`
  - approach: An upload of a host cache entry is keyed by the entry's key, not the host address, so a reused address can never serve a stale import (G1). Mark G1 closed in the doc.
  - verify: A test frees an entry and reallocates the same address with new pixels; the import serves the new pixels.
  - size: M

## Phase 301.3: Gate

- [ ] M301.P3.T1 — Add GPU cache stress cases and run the benchmark with the GPU on
  - files: `Tests/CacheStress_Test.cpp`, `Tests/CacheStressConcurrent_Test.cpp`, `tools/bench/baselines/edit-latency.jsonl`
  - approach: Cases for device demotion during a concurrent render, device loss, and OOM with cached entries present. Run M276 - Upstream Reuse On Edits' benchmark with `NATRON_GPU` on, on the user's host, and record the result.
  - verify: The cases pass. On Grade and HSVKeyer edits after the first render, the benchmark shows only the edited node executing.
  - size: M

**Verification gate:** GPU, cache and CacheStress suites pass in debug and release, and the NVIDIA runner passes too. With the GPU on, M276 - Upstream Reuse On Edits' benchmark shows no upstream execution on a downstream edit after the first render, at 100% and at fit-to-window.
