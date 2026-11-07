# M300 - Unified Tiered Cache

One cache logic for node outputs across storage tiers. Entries share a tier-neutral header: key, bounds, the rendered-pixel bitmap, the being-rendered registry, which tiers hold the entry, last use, retention priority and measured recompute cost. Host RAM and a new disk tier (this milestone), and device memory (M301 - GPU Cache Tier), implement one tier interface: budget, used and demote. A single cost-aware policy evicts only past a high-water mark, so nothing is evicted until room is needed. `Node::shouldCacheOutput`'s yes/no rule becomes a retention priority. Deep images become a payload type in the same cache. The DiskCache node becomes a pin. The viewer playback cache stays separate.

What exists on main (cd383143f): four `Cache<T>` instances (`Engine/AppManager.cpp` ~:1041-1046): NodeCache (RAM), DiskCache (mmapped `MemoryFile`s, DiskCache node only, persisted), ViewerCache (tiled `FrameEntry`) and DeepImageCache (RAM, own 2 GiB budget). Eviction is blind LRU (`Engine/LRUHashTable.h` ~:199) past 90% (`Engine/Cache.h` ~:66), pinned by `use_count() > 1`. RAM evictions are dropped, never spilled. `ImageKey` (`Engine/ImageKey.cpp` ~:69-98) leaves out mip level, layer and bit depth, which are filtered per entry after lookup (`Engine/EffectInstance.cpp` ~:2053-2166). Entries grow through `ensureBounds` and a trimap `Bitmap` (`Engine/Image.h` ~:67-130).

## Scope

**In:**
- A tier-neutral entry header and tier interface; host RAM and disk tiers.
- Image and DeepImage payloads in one cache with one policy; the DeepImageCache instance retired.
- An eviction score of priority × recompute cost × reuse multiplier ÷ bytes, applied per tier and only past the high-water mark. A demotion goes to the next tier when the recompute cost exceeds the demotion cost, and the entry is dropped otherwise.
- Retention priority signals: inputs of recently edited nodes (decaying), inputs of open panels, the active viewer's branch, expensive nodes, and a reuse-count multiplier on recompute cost.
- Two admission policies behind a setting: admit everything, or admit only above a priority floor. The benchmark decides the default and the user signs off.
- A persistent disk tier shared by concurrent sessions, with a location and size preference (default `~/.cache/Natron/tier`, 20 GiB in total, capped at 25% of the free space). Each build has its own directory, so a new build starts cold without touching another build's files.
- The DiskCache node as a pin: forced to disk, persistent, on its own budget (the existing 10 GiB setting), never evicted by the automatic policy.
- `ImageKey` completed with mip level, layer and bit depth (hole I4). The lookup that downscales a finer mip stays as a fallback.

**Out:**
- The device tier and GPU region publishing (M301 - GPU Cache Tier).
- Draft and auto-proxy lookup behaviour (M303 - Interactive Edit Latency).
- The viewer playback cache (`ViewerCache`, `FrameEntry` tiles); it stays on its own budget.
- Key holes K3–K8 beyond what persistence needs (M260 - Cache Invalidation Follow-ups owns them).

**Edge cases:**
- **Disk full or a write error on demotion:** the entry is dropped and one warning is logged per session.
- **A second Natron instance:** concurrent instances are peers on one tier; none is locked out or given scratch space.
- **A changed source file across sessions:** Read keys carry M260's file stamps, so a persisted entry for a changed file misses and Reload re-reads it.
- **An entry in use:** it is pinned and never demoted while a render holds it, as today.
- **A partially rendered entry:** its bitmap travels with it through demotion and promotion.

**Assumptions:**
- The rendered-pixel bitmap and the being-rendered registry can move out of `Image` without changing the `waitForImageBeingRenderedElsewhere` semantics (`Engine/EffectInstancePrivate.cpp` ~:921-959).
- Recompute cost can be measured as the render wall time of the node alone, excluding its inputs, which is already split out per task by the scheduler.

## Decisions

- 2026-11-07 — **Peer-shared disk tier (user):** any number of sessions use one tier with no lock gating access. Each entry is an immutable file published by atomic rename and read with `pread`; automatic entries aren't mapped. Each build has its own directory. Background publishing is on, bounded by the recompute-versus-write test. A build with no live session is reclaimed after 7 days or sooner under pressure, pins included. A deleted project's pins hold their budget until a clear or a new build. No pixel merging: the larger result replaces the smaller. Consultant's design, not yet built; cost figures are estimates.
- 2026-11-07 — M300.P1.T4 at 4cfe3b531, phase gate running. `Cache<Image>` with templated members for now, not the note's non-template cache; a large deep render can evict images by LRU until M300.P2.T2. `DeepReadWrite_Test` `EachFrameOfASequenceRendersItsOwnFileThroughTheCache` proves less now that held entries are pinned; tighten it at the next test pass.
- 2026-11-07 — **Concurrent sessions coexist on the disk tier (user):** two sessions with different projects are very common and with the same project somewhat rare. Locking the tier to the first session is rejected; this replaces the second-instance edge case and the directory lock. Redesign pending before Phase 300.3.
- 2026-11-07 — M300.P1.T3 at 9a477bf16, unbuilt until the phase gate. A restored DiskCache entry now has an empty bitmap and re-renders until M300.P3 persists bitmaps; the old code served never-rendered pixels as rendered, so the fix stays and the gap closes before the Feature ships. Each entry has its own `pendingLock` rather than the note's stripe.
- 2026-11-07 — M300.P1.T2 at 900e2277c, unbuilt until the phase gate. Dropped from the old filter: a differently laid out RGB, Alpha or XY entry (except RGB or Alpha from RGBA), and Short for a Half request. A miss costs up to 72 probes for Alpha byte at mip 8 (144 on draft), 1 for RGBA float at mip 0; watch this in the bench.
- 2026-11-06 — Design note at 126e17bb4 on milestone/m300-unified-tiered-cache. Assumption on recompute cost was false: the scheduler stores no cost and task wall time can include nested inputs, so a stopwatch inside `renderRoI` subtracts nested time. Five open points in the note await the user: `priorityFloor` wording, the age term, the disk cap's free space, a full pin budget, background renderer scratch.
- 2026-11-06 — **A cached output skips its dependencies (user):** "that's pretty much the whole point of the cache". The task graph must not run a task's dependencies when the consumer's output is already served by the cache. M276 - Upstream Reuse On Edits measured the opposite as `behind_hit`.
- 2026-11-01 — **One cache logic across RAM, disk and VRAM (user):** "keep more in the cache if there's budget… just not evict stuff until we need to". Cost-aware tiers with demotion replace blind LRU. See `DECISIONS/2026-11-01-unified-tiered-cache.md`.
- 2026-11-01 — **Spill to disk only when cheaper than a recompute (user).**
- 2026-11-01 — **The disk tier persists across sessions, and any new build invalidates it (user):** the key keeps hashing the binary. Safe, at the cost of a cold cache after a rebuild.
- 2026-11-01 — **Retention signals (user):** inputs of recently edited nodes, inputs of open panels, the viewer branch, expensive nodes, and a reuse multiplier on recompute cost for outputs read more than once.
- 2026-11-01 — **Test both admission policies (user):** the user's intuition favours the priority floor; the benchmark decides.
- 2026-11-01 — **Deep is one payload type in the cache, not a separate cache; the DiskCache node becomes a pin on its own budget; the viewer cache stays separate (user).**

## Phase 300.1: Entry and key

- [x] M300.P1.T1 — Write the tiered-cache design note
  - files: `docs/cache/tiered-cache.md` (new)
  - approach: Specify the entry header fields, the tier interface (`budget()`, `used()`, `demote(entry)`, `promote(entry)`), the score formula and the high-water marks, the priority signals with their decay, both admission policies, pin semantics, persistence (index format, binary identity, directory lock) and the device tier's hooks that M301 - GPU Cache Tier will fill. Map each existing class (`Cache<T>`, `LRUHashTable`, `CacheEntryHolder`, `HostImageMemory`, `DeepImageCacheEntry`, `MemoryFile`) to what it becomes. Every requirement in this milestone's `## Scope` appears.
  - verify: The note covers every `## Scope` In and Edge-case item; a reviewer checks it against the list.
  - size: L

- [x] M300.P1.T2 — Complete `ImageKey` with mip level, layer and bit depth
  - files: `Engine/ImageKey.h`, `Engine/ImageKey.cpp`, `Engine/EffectInstance.cpp` (lookup filter ~:2053-2166), `Engine/GpuPlacement.cpp` ~:1032, key serialization, `Tests/CacheStress_Test.cpp`
  - approach: Add the three fields to the key's hash and equality. Lookups ask for the exact key first. On a miss they fall back to the existing finer-mip-downscale path by probing the finer keys. Drop the per-entry filtering the key now makes redundant.
  - verify: A CacheStress case with RGB then RGBA requests of one node (I4, `RgbThenRgbaRequest`) and a mip0/mip1 pair passes, and existing cache suites pass.
  - size: L

- [x] M300.P1.T3 — Move the rendered bitmap and pending registry into a tier-neutral entry header
  - files: `Engine/CacheEntry.h`, `Engine/Image.h`, `Engine/Image.cpp`, `Engine/EffectInstancePrivate.cpp`, `Engine/EffectInstanceRenderRoI.cpp`
  - approach: Create `CacheEntryHeader` holding the key, bounds, trimap bitmap, being-rendered registry, tier residency, last use, priority and recompute cost. `Image` keeps pixels and delegates bitmap calls to its header. Behaviour is unchanged.
  - verify: The cache, CacheStress (including concurrent) and render suites pass unchanged.
  - size: L

- [x] M300.P1.T4 — Make deep images a payload of the node cache
  - files: `Engine/DeepImageCacheEntry.h`, `Engine/DeepImageCacheEntry.cpp`, `Engine/DeepImageKey.cpp`, `Engine/AppManager.cpp` (instances ~:1041), `Engine/Cache.h`, `Engine/Settings.cpp`, `Engine/EffectInstanceRenderDeep.cpp`, `Tests/DeepImageCache_Test.cpp`
  - approach: Deep entries use `CacheEntryHeader` and live in the node cache under the shared budget. Retire the DeepImageCache instance and its 2 GiB setting.
  - verify: `DeepImageCache_Test` and the deep compositing suites pass against the shared cache.
  - size: L

## Phase 300.2: Tiers and policy

- [ ] M300.P2.T1 — Introduce the tier interface and the host RAM tier
  - files: `Engine/CacheTier.h` (new), `Engine/CacheHostTier.cpp` (new), `Engine/Cache.h`
  - approach: Per the design note. The host tier wraps today's in-RAM storage and `HostImageMemory` pool trimming. Its `demote` drops the entry until the disk tier exists. The M28 free-RAM check (`checkCacheFreeMemoryIsGoodEnough`, `Engine/AppManager.cpp` ~:2987) feeds the tier's budget.
  - verify: The cache suites pass and a new unit test drives the host tier's budget and demotion.
  - size: L

- [ ] M300.P2.T2 — Replace blind LRU with the cost-aware score
  - files: `Engine/Cache.h` (`createInternal` ~:813-905), `Engine/LRUHashTable.h`, `Engine/EffectInstanceRenderRoI.cpp` (record recompute cost), `Engine/EffectInstance.cpp`, `Tests/CacheStress_Test.cpp`
  - approach: Record each entry's own render wall time as its recompute cost. Evict the entry with the lowest priority × cost × reuse multiplier ÷ bytes, and only once occupancy passes the high-water mark. Pinned entries are skipped as today.
  - verify: A unit test shows a cheap large entry evicted before an expensive small one of the same age, and no eviction below the high-water mark. CacheStress passes.
  - size: L

- [ ] M300.P2.T3 — Turn `shouldCacheOutput` into a retention priority
  - files: `Engine/Node.cpp` (`shouldCacheOutput` ~:8225-8343), `Engine/CacheRetention.h` (new), `Engine/CacheRetention.cpp` (new), `Engine/Knob.cpp` (edit notification hook), `Engine/RenderScheduler.cpp` ~:626
  - approach: A priority built from the signals in `## Scope` In. Edit recency comes from knob-change notifications, with a decaying boost on the edited node's inputs. A reuse count is kept per entry. Reasons that today force caching (temporal access, no tile support, multiple outputs, forced) map to a high priority.
  - verify: A unit test checks the priority ordering for an edited node's input, an open panel's input, an off-branch node and a reused output.
  - size: L

- [ ] M300.P2.T4 — Add the two admission policies
  - files: `Engine/Cache.h`, `Engine/CacheRetention.cpp`, `Engine/Settings.cpp`, `Engine/EffectInstanceRenderRoI.cpp` ~:1380 (retire `aggressiveCaching`)
  - approach: Add an advanced setting `cacheAdmissionPolicy` (`all` | `priorityFloor`), with env override `NATRON_CACHE_ADMISSION`. `priorityFloor` admits an output only when it carries a priority signal or a recompute cost above a threshold. The default is set after P4.T1.
  - verify: A unit test checks that `all` admits a cheap unflagged output and `priorityFloor` refuses it.
  - size: M

- [ ] M300.P2.T5 — Skip dependencies behind a cache hit
  - files: `Engine/RenderScheduler.cpp` and the task-graph planning it uses, `Engine/GpuPlacement.cpp` if placement plans the same walk, `Tests/CacheStress_Test.cpp`
  - approach: Before scheduling a task's dependencies, ask whether its output is already available in full for the requested region; on a hit, prune the subgraph that only that task needed. A partial hit keeps only the dependencies the missing region needs. Pruning holds across tiers once they exist.
  - verify: A render test edits the last node of a chain whose second-last output is cached, and the profile shows no upstream execution. With the settings panel open, M276's benchmark shows no `behind_hit`.
  - size: L

## Phase 300.3: Disk tier and pins

- [ ] M300.P3.T1 — Add the peer-shared disk tier
  - files: `Engine/CacheDiskTier.h` (new), `Engine/CacheDiskTier.cpp` (new), `Engine/Settings.cpp`, `Engine/CacheHostTier.cpp`, `MemoryFile`, `Engine/Image.cpp`, `Engine/DeepImage.h`
  - approach: Per `docs/cache/tiered-cache.md`. Each entry is an immutable file under `<tier>/<build>/auto`, holding key, params, checksum and bitmap, published by rename and read by `pread`. Probes list one shard off the cache lock, with a short negative cache. An idle low-priority publisher and demotion both apply the recompute-versus-write test, with a per-file latency term. Budget comes from per-session ledgers plus a slow scan; eviction samples the persisted score. Write errors drop the entry with one warning per session. A retracted entry cancels its queued publish.
  - verify: A tiny RAM budget demotes an expensive entry and drops a cheap one; promotion returns identical pixels. A second process hits an entry the first published. A forged key misses.
  - size: L

- [ ] M300.P3.T2 — Persist without an index, and let builds coexist
  - files: `Engine/CacheDiskTier.cpp`, `Engine/CacheSerialization.h`, `Engine/AppManager.cpp`, `Engine/Plugin.cpp`, `Engine/AppManagerPrivate.cpp`
  - approach: Entries are found by name, so startup does no restore scan. Sessions register for liveness; any peer reaps dead sessions' temp files and ledgers. The identity stamps both executables (Natron and NatronRenderer) and selects the build directory; salted hashes are flagged and never published. A build with no live session is reclaimed after 7 days or sooner under pressure, oldest first. A rename self-test at start falls back to the default location, then to RAM only, with one warning.
  - verify: A restart gets hits, whichever session started first. Two identities run together with neither losing files. A dead build is reclaimed. Natron and NatronRenderer hash a project identically across processes.
  - size: L

- [ ] M300.P3.T3 — Make the DiskCache node a pin
  - files: `Engine/DiskCacheNode.cpp`, `Engine/DiskCacheNode.h`, `Engine/CacheDiskTier.cpp`, `Engine/AppManager.cpp`
  - approach: Pins live under `pins/<cacheID>` on their own budget (the existing DiskCache size setting), excluded from sampling. A live session writes `.live-<session>` there. Deleting a node removes that session's marker and unlinks only if no live peer holds the holder. Clearing unlinks this session's holders' pins and never other holders'; a peer's mapping stays valid. Pins stay mapped on local disks and are read into RAM on network mounts. Retire the separate DiskCache `Cache<Image>` instance.
  - verify: Overfilling the automatic tier leaves pins intact, and a restart restores them. Deleting in session A keeps session B's pins; deleting in both removes them.
  - size: M

- [ ] M300.P3.T4 — Prove the shared tier across real processes
  - files: `Tests/CacheDiskTierProcess_Test.cpp` (new), `tools/bench/` (probe bench), `docs/cache/tiered-cache.md`
  - approach: Spawn real processes for: hash equality including the renderer; peer visibility while both run; `kill -9` of a writer in a loop with no invalid read and `tmp/` reaped; unlink under an active `pread` and a mapped pin; four writers staying within budget; two fake builds with retention; the pin delete and clear matrix; `RLIMIT_FSIZE` giving one warning; an injected rename failure taking the fallback. Separately bench a 300,000-file probe and sampling run, and confirm the AppImage ships both executables in one directory.
  - verify: The cases pass in debug and release. The bench's miss cost per frame is recorded, and the 1 s negative cache keeps a 1000-node miss under the 30 ms estimate or the note says what changed.
  - size: L

## Phase 300.4: Measure and guard

- [ ] M300.P4.T1 — Benchmark both admission policies and set the default
  - files: `tools/bench/baselines/edit-latency.jsonl`, `docs/cache/tiered-cache.md` (results section), `Engine/Settings.cpp` (default)
  - approach: Run M276 - Upstream Reuse On Edits' edit-latency benchmark with `NATRON_GPU=off` under each policy on the user's host, plus an off-branch graph, since the viewer-branch signal admits the whole chain under both policies. Record latency, upstream executions, peak RAM and disk writes. Put the numbers to the user; set the default they choose.
  - verify: The results are recorded, the user's choice is in `## Decisions`, and the default matches it.
  - size: M

- [ ] M300.P4.T2 — Add CacheStress cases for tiers, pins and persistence
  - files: `Tests/CacheStress_Test.cpp`, `Tests/CacheStressConcurrent_Test.cpp`, `Tests/CacheStress.cpp`
  - approach: Cases for concurrent demotion while a render waits on a pending region, promotion of a partially rendered entry, pinned entries under pressure, and a restart with a changed binary identity.
  - verify: The new cases pass in the debug (asserts on) and release presets.
  - size: M

**Verification gate:** cache, CacheStress (including concurrent) and deep suites pass in debug and release. With `NATRON_GPU=off`, M276 - Upstream Reuse On Edits' benchmark shows no raw-chain execution on Grade or HSVKeyer edits after the first render, and no records behind `RawInputColor`'s hit. The admission default is the user's choice.
