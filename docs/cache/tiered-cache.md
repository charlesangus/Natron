# Tiered cache

Design note for M300 - Unified Tiered Cache (task M300.P1.T1). It specifies one cache for rendered node outputs,
holding Image and DeepImage payloads, with a host RAM tier and a persistent disk tier and room for the device tier
of M301 - GPU Cache Tier. The viewer playback cache stays apart, on its own budget.

File and line references are to commit e4f71dc77. The milestone's own line numbers date from cd383143f; where they
have moved, this note gives the current ones.

Figures come from `docs/perf/interactive-edit-attribution.md`, measured on the user's host (RX 7900 XTX, 32
configurations, 10 edits each). The 384 MB entry size used below is an estimate (24 MP × 4 channels × 4 bytes), not
a measurement.

## How the gate is met

The gate of M300 - Unified Tiered Cache: with `NATRON_GPU=off`, the benchmark of M276 - Upstream Reuse On Edits shows
no raw-chain execution on Grade or HSVKeyer edits after the first render. That milestone attributes every upstream execution to
two causes.

| Panel | Upstream executions per edit | Cause | Software host records |
| --- | --- | --- | --- |
| closed | 5 | `not_admitted`: `RawInputColor` and the four raw nodes run in state `bypass`, because nothing stored them | 400 |
| open | 4 | `behind_hit`: `RawInputColor` hits, the four raw nodes above it still run | 320 |

At fit the cost is about 608 ms of which `RawDemosaic` is 383.6 ms; the edited node is about 1 ms. At 100% the chain
costs less per node but `Read1`'s hit alone costs 8.3 ms of 42 ms.

### `not_admitted` goes away through M300.P2.T3 and M300.P2.T4

Today admission is the yes/no of `Node::shouldCacheOutput` (`Engine/Node.cpp:8710`), read at
`Engine/EffectInstanceRenderRoI.cpp:933` into `createInCache`. A node it refuses gets a private image
(`getOrCreateFromCacheInternal`, `Engine/EffectInstance.cpp`, the `!useCache` branch) and records state `bypass`
(`Engine/EffectInstanceRenderRoI.cpp:1380`). The raw nodes have one output, a closed panel and no forcing reason, so
they are refused.

Since M300.P2.T3 the reasons are also a priority ([Priority signals](#priority-signals)), but the decision is unchanged:
`Node::shouldCacheOutput` returns `CacheRetention::admit(reasons)`, true for any reason, so the raw nodes are still
refused. Since M300.P2.T4 `createInCache` is the admission policy's answer ([Admission](#admission)). Every raw node is
on the active viewer's branch, which is a signal stamped before the frame's first admission question, so both policies
store all five on the first render, and a later edit of a downstream node finds them at an unchanged hash and records
`hit`. That is read from the code, not measured; M300.P4.T1 measures it.

What stays a refusal, independent of policy, because it is not a retention question: OpenGL renders and loop variables
(`Engine/EffectInstanceRenderRoI.cpp:929-930`), `allowsStoreUnder` (`:978`), and the overlay pass (`:963-972`). The
analysis override at `:933` stays too.

`Engine/RenderScheduler.cpp:632` is a second consumer of the same yes/no: it sets `sharesCachedOutput`. It must read the
same answer, or a task's frame-store handling would disagree with what `renderRoI` stores. Since M300.P2.T4 it and
`renderRoI` ask one function, `EffectInstance::admitsOutputToCache`, with the frame's real `isFrameVaryingOrAnimated`.

The scheduler's `sharesCachedOutput` is false for a node below the floor that an entry stored earlier could still
serve, because lookup is not gated and this flag is. So the probe has its own flag: `buildGraph` sets
`probesCachedOutput` on any task `renderRoI` would look up, whether or not the node would store its output now.

### Pruning the task graph behind a hit

`RenderScheduler::buildGraph` expands every `fvRequest->dependencies` with no cache probe, and `submit` starts each task
whose dependencies are done. Inside `renderRoI`, a hit does skip its inputs: the pre-render loop iterates only rectangles
left to render, and a legacy render plans nothing ahead of that loop. So only the task graph has `behind_hit`, and the
change is in planning.

The planning pass, `RenderScheduler::pruneBehindCacheHits(graph, context)`, runs after `buildGraph` at both call sites,
before `placeRenderGraph` (`Engine/ViewerInstance.cpp`, `Engine/OutputSchedulerThread.cpp`):

1. Walk from the root in reverse topological order. A task is *needed* if it is the root, or if a consumer that is
   needed still reads it after that consumer's own probe.
2. Probe each needed task once, through `EffectInstance::probeOutputInCache`: every layer at the exact key `renderRoI`
   computes (`outputCacheKey`), and the draft key too on a draft frame. Each entry answers through the same trimap
   bitmap `renderRoI` reads, so pixels marked as being rendered elsewhere count as missing.
3. A task whose region is all there is *covered*: it keeps its node and loses its dependencies. It still runs, as a
   `hit`, and the consumer reads its image from the frame store as usual.
4. A task with part of its region there keeps the dependencies its effect names for the missing part, asked through
   `getRegionsOfInterest`. A node with input transforms keeps them all, and so does a dependency on an edge with no input
   number. A temporal node's input is read at each frame the edge names, under the same region of interest.
5. Remove tasks that are not needed and recompute `consumers` and `remainingDeps` of the rest. A dependency shared with
   a branch that needs it stays. An identity task is never probed and keeps its dependency, so the walk reaches the
   target through it.

`servedFromCache` records steps 3 and 4 on the task. Placement reads that flag as `inputs.isCached` instead of probing
again, so it plans the pruned graph with one answer per task, and it does not run with `NATRON_GPU=off`.

**Cost.** The walk is O(V+E), and a task a covered consumer alone reads is never probed. Each task is probed at most once
per plan. A probe tries at most layers x (mip levels + 1) x 2 candidate layers x 4 depths x 2 draft keys, a hash and a lock
pair each, so 72 to 144 keys in the worst combinations and 1 or 2 usually. The cache lock is taken per key and released
between keys, so the worst plan does N x 144 short lock pairs for N needed tasks and never holds the lock across a plan.

**The probe changes nothing.** `Cache::peek` reads through `LRUHashTable::peek`, which does not relocate. It does not
call `markUsed`, so `lastUse` and `reuseCount` stay as they were, and it does not re-open a file mapping. A plan's
probes leave the eviction order alone.

**No pin.** The pass holds no entry. A plan-wide hold would keep `use_count() > 1` on each covered entry, which makes it
immune to eviction ([Pins](#pins-and-holds)), and on a modest machine that overshoots the budget for a whole plan, with
nothing throttling it: the frame store's byte-budget admission only sees a task's output once it runs. A covered task has
no dependencies, so it runs just after `submit`, and from then on the frame store holds its output under that budget.

If a covered entry goes anyway, whether evicted between plan and use, retracted
(`Engine/EffectInstanceRenderRoI.cpp`) or failing to re-open from disk, the task's `renderRoI` misses and renders. Its
input read then misses the frame store, so `getImage` notes an unplanned pull and renders the input by recursion
(`Engine/EffectInstance.cpp`). The pixels are right, and the cost shows as an unplanned pull. A partial task that loses
its entry widens the same way: its render window is now the whole region, so no input is skipped.

**Tiers.** The memory tier answers with a real entry. A disk entry of the node cache answers from its metadata without
re-opening the file; `renderRoI` re-opens it and a failure there is the fallback above. An entry restored with an empty
bitmap reads as missing everything. A later tier that adds a container to the `peek` loop inherits the same contract: a
false hit costs an unplanned pull and never a wrong pixel.

**Other requests.** Tasks are keyed by node, time, view, mip and context, and consumers are per task, so a temporal
node's other frames and a multi-output node's several consumers each stay needed exactly while something reads them.
A draft frame probes the non-draft key and then the draft key, as `renderRoI` does. An overlay image pass, a paint
stroke in progress, a writer rendering its own sequence and a loop variable's context are not probed, since `renderRoI`
either skips the lookup for them or doesn't take the entry at face value. The scheduler's frames never bypass the cache,
so the pass has no bypass case.

**Partial tasks at render time.** A skipped input has no plan, so the effect must not pull it. When a task's render
window is part of its RoI, `renderInputImagesForRoI` asks the effect for the RoI of that window, and both
`treeRecurseFunctor` and `getImage` return nothing for an input whose RoI misses its RoD (`inputRoIMissesRoD`).

**What the pass leaves.** A dependency that stays keeps its planned RoI, which may include a pruned consumer's demand, so
it can render more than the frame still needs. `estimatedBytes` of a hit doesn't shrink. An entry at a finer mip is trusted
only when it covers the whole region, and a part-covered non-tiled effect renders it all. A region being rendered
elsewhere keeps its dependencies, and `renderRoI` waits on it as it always has.

The benchmark needs no new semantics: it iterates only records present in the profile, so a pruned task counts as not
executed, and a covered task records `hit` with no `deps`. A `behind_hit` can still appear for a hit a plan could not
foresee, such as one rendered by another frame after the plan. Its baseline (`tools/bench/baselines/edit-latency.jsonl`)
is re-recorded by M300.P4.T1.

### Why both changes are needed

| Combination | Gate on the benchmark graph | Note |
| --- | --- | --- |
| `all`, no pruning | passes by hits | every raw task still runs as a hit; `Read1`'s 8.3 ms stays at 100%; a disk-resident entry is promoted for nothing; any evicted upstream entry re-executes behind a hit |
| `priorityFloor`, no pruning | passes only while every raw entry survives | the attribution's objection stands: a node below the floor, or an evicted one, re-runs behind a hit |
| either policy, pruning | passes | the edit runs the edited node and one hit; upstream entries can be evicted without effect until the hit itself goes |

Pruning without admission does not pass: with the panel closed `RawInputColor` has no entry, so nothing covers it.

## Entry header

`CacheEntryHeader` (`Engine/CacheEntry.h`, M300.P1.T3) is what a tier and the policy see. It carries nothing specific to
a payload type, and it has to outlive any one payload buffer: an entry on disk, and later one only on a device, has no
pixels in RAM.

| Field | Type | Written | Purpose |
| --- | --- | --- | --- |
| `key` | `shared_ptr<const KeyHelper<U64>>` | at creation | identity; `ImageKey` and `DeepImageKey` both derive from `KeyHelper<U64>` (`Engine/ImageKey.h:37`, `Engine/DeepImageKey.h:43`) |
| `hash` | `U64` | at creation | table key; `DeepImageKey::fillHash` (`Engine/DeepImageKey.cpp`) starts with a domain tag so an image and a deep key cannot share a bucket by accident, and `ImageKey` needs none |
| `payload` | enum: image, deep | at creation | selects the typed entry and the file format |
| `retention` | atomic `shared_ptr<CacheRetentionSignals>`, in place of an interned holder id | store, hit | the holder's signals. Shared rather than looked up so that the score scan takes no other lock and an entry can outlive its node without an interning table to clean |
| `bounds` | `RectI` | under `_entryLock` | as `Image::getBounds` (`Engine/Image.h:287`) |
| `bitmap` | `Bitmap`, absent for deep | under `_entryLock` | the trimap (`Engine/Image.h:67-169`); null in RAM while the entry is disk-resident |
| `pending` | `ImageBeingRenderedPtr` | under the entry's `pendingLock` | the entry's slot in the being-rendered registry; see [A1](#assumptions-checked) for why not the stripe |
| `residency` | atomic mask: host, disk, device | tier moves | which tiers hold a copy; a mask because a device and a host copy can coexist |
| `bytes` | not stored | | payload plus bitmap, from `size()` (`Engine/Image.h`); the scan calls it only on entries nobody holds, where no resize can race it and its `tryLockForRead` cannot wait |
| `state` | atomic enum: normal, demoting, promoting | tier moves | a move in flight, which a lookup can overtake |
| `pinned` | bool | when published to or restored from `pins/` | DiskCache node output |
| `lastUse` | atomic wall-clock ns | every lookup today, real hits only after M300.P2.T5 | age term of the score; wall clock so it survives a restart. `Cache::getInternal` still bumps it for a probe that serves nothing, such as `appPTR->getImage` at the graph-level probes; the non-promoting probe of M300.P2.T5 ends that |
| `reuseCount` | atomic `U32` | a render served by the entry | reuse multiplier; counted at the render sites, so no probe counts, and once per render request through `lastReuseRender` (the request's `AbortableRenderInfo::getRenderSequence`). The render that stores the entry does not count |
| `lastReuseRender` | atomic `U64` | store, hit | the request `reuseCount` last counted |
| `priority` | atomic `float` | last score | the value last computed, for diagnostics and the file's stats block; the score itself recomputes it |
| `recomputeCostNs` | atomic `float` | after each render | own render time scaled to the full bounds; see [Recompute cost](#recompute-cost) |

The score scan reads atomics and the `size()` of unheld entries, so it waits on no per-entry lock.

An `Image` always has a header, including uncached images built with `useBitmap` (`Engine/Image.h:191`), because
`markImagesAsBeingRendered` accepts any image with a bitmap (`Engine/EffectInstancePrivate.cpp:881-919`). `Image` keeps
the pixels and delegates. A demoted entry keeps its `Image` object and swaps the buffer, as `deallocate` does today
(`Engine/CacheEntry.h:963-991`), so the pointer the registry and every holder use stays valid.

Partially rendered entries: an entry can only move once no render holds it, so it normally contains only 0 and 1 in its
bitmap. It is not only 0 and 1 in the stale-marker windows described under [Pins and holds](#pins-and-holds), where a
render keeps its marks without a hold. The 2 ("being rendered") still never reaches disk, because the writer converts any
it finds to 0, which also covers those windows. On demotion the bitmap goes to the entry's file and leaves RAM. It cannot
stay: a 20 GiB tier of float RGBA is 1.3 G pixels, so 1.3 GiB of RAM at one byte per pixel. Promotion reads it back.
`Image::onMemoryAllocated` no longer forces every restored pixel to rendered (`Engine/Image.cpp:792`, changed in
M300.P1.T3), which would have turned a partial entry into a full one.

Deep entries have no bitmap and stay all-or-nothing (`isFullyRendered`, `Engine/DeepImageCacheEntry.h:88`). Their header
carries the key and bounds only. A deep entry costs nothing from creation until `allocateMemory()` after its render, when
its real size is captured once and reported to the cache, and the render then trims the cache to its budget, because
eviction at creation could not know that size.

## Tier interface

```cpp
// Engine/CacheTier.h
class CacheTier {
public:
    virtual std::size_t budget() const = 0;              // bytes this tier may hold right now
    virtual std::size_t used() const = 0;                // bytes it holds or has queued, in atomics
    virtual bool demote(const CacheEntryPtr& entry) = 0; // moves entry out, to next() or nowhere; false if it stayed
    virtual bool promote(const CacheEntryPtr& entry) = 0; // brings entry in from the tier below; false if it could not
    CacheTier* next() const;                             // the tier below, or null
};
```

The policy lives above the tiers and is the same for all: past a tier's high-water mark, pick the lowest score in that
tier and call `demote` on it.

`demote(entry)` decides between the next tier and a drop: to the next tier if `next()` exists and recompute cost exceeds
demotion cost, otherwise dropped. It requires that no render holds the entry.

| Tier | `budget()` | `used()` | `demote` | `promote` |
| --- | --- | --- | --- | --- |
| Host (`Engine/CacheHostTier.cpp`) | `min(maxRAMPercent × total RAM, used + available RAM − keep-free RAM)`; defaults 50% and 20% (`Engine/Settings.cpp:1799-1800`), the second term fed by `checkCacheFreeMemoryIsGoodEnough` (`Engine/AppManager.cpp:3029-3035`) | live entries plus idle `HostImageMemory` pool (`Engine/HostImageMemory.h`, class comment). This is a reading for the tier's callers; eviction compares live bytes only, see [Score and high-water marks](#score-and-high-water-marks) | queue for the disk writer if worth it, else drop; the idle publisher also writes behind and keeps the RAM copy | allocate from `HostImageMemory`, `pread` the file, verify it, reload the bitmap |
| Disk (`Engine/CacheDiskTier.cpp`) | the size preference, capped at 25% of free space; each process computes the cap and the smallest view wins | last scan plus the sum of the sessions' ledgers, refreshed each second | drop: unlink the file | not called: nothing lies below it |
| Device (M301 - GPU Cache Tier) | see [Device tier hooks](#device-tier-hooks) | | | |

Host demotion is off the cache lock. The current path syncs under `_lock` (`tryEvictInMemoryEntry`,
`Engine/Cache.h:1877-1897`, its comment says "EXPENSIVE"); writing 384 MB there would stall every lookup on the machine.
`demote` sets `state = demoting` and hands the entry to one writer thread, modelled on `DeleterThread`
(`Engine/Cache.h:81`). Its RAM stays charged until the write completes, and a lookup in the meantime reads RAM and
cancels the move. An allocation that finds the tier full waits on `_memoryFullCondition` as it does for the deleter
(`Engine/Cache.h:890`). `createInternal` holds `_getLock` throughout, and `get` takes it too, so that wait stalls every
lookup of the cache; the disk tier must not make it wait on a write. When the queue holds more than `demotionQueueFraction` of the host budget, `demote` drops
instead, so a slow disk costs recomputes and not a stalled render.

Promotion happens when a render reads the entry, on that render's thread, never during a probe. Pinned DiskCache
entries are read through their mapping and not copied on a local disk, and read into RAM on a network mount ([Pins](#pins-and-holds)).

## Score and high-water marks

The score, per tier, is computed only once occupancy passes the high-water mark, which for the host tier is
`min(highWater × limit, used + available RAM − keep-free RAM)`. The mark is not `highWater × budget()`: that would scale
the RAM term too, and start evicting once `available − keep-free` falls below about `0.11 × used`, where today nothing is
evicted until available RAM is below keep-free.

The two terms have separate drivers, and neither reads `used()`, so it can stay behind the pool mutex:

- The limit term: `createInternal` and `clearExceedingEntries` compare live bytes to `highWater × limit`, after
  `trimIdlePool`. The idle pool is not part of the comparison. It is trimmed first, to what the mark leaves above live
  bytes, so live plus pool is under the mark whenever live is. A cache with a limit of 0 therefore still empties the
  process-wide pool on every create. That is the behaviour before the score, and the score does not need it changed.
- The RAM term: `used` cancels, so past the mark means available RAM below keep-free. The free-RAM check at the top of
  `createInternal` evicts until the shortfall is covered (`evictMemoryCachesUntilShortfallCovered`). `createInternal`'s own
  loop does not test it, because the reading goes stale as soon as that check has evicted.

The formula:

```
score = priority × recomputeCost × reuseMultiplier ÷ bytes
reuseMultiplier = (1 + log2(1 + reuseCount)) × 2^(−age / reuseHalfLife)
```

No eviction happens below the mark. That is the user's decision ("just not evict stuff until we need to"), so there is
no low-water mark: as today (`Engine/Cache.h:859`), eviction stops as soon as the incoming entry fits under the mark.

**The age term is an interpretation.** The milestone's formula has no age term, but its header has `last use` with no
stated role. Without one, an orphaned expensive entry (from an old file, an old hash) outranks live cheap entries
forever. Age inside the reuse multiplier keeps the formula's shape. It is deliberately slow: at 30 minutes per half-life,
an orphaned `RawDemosaic` outranks a live `RawInputColor` (640× cheaper at equal bytes) for 9.3 half-lives. Those hours
are cheap, because what gets evicted first is what costs little to redo.

Entries do not leave on an edit today and do not start to: `removeAllEntriesWithDifferentNodeHashForHolderPrivate`
(`Engine/Cache.h:1679`) only ever runs with `removeAll` for a node's deletion (`Engine/Cache.h:1630-1634`,
`Engine/Node.cpp:2223-2236`), and that returns early while a project closes, so persisted entries survive it. Undo keeps
hitting the entry from before the edit.

A hit is a reuse; a probe is not. Pruned tasks are not touched, so an entry shielded by a hit below it ages. That is
intended: if the hit is lost, its own recompute hits whatever survives above it.

Selection is a sampled scan, cached. On crossing the mark, score `evictionSample` entries from a cursor that continues
where the last scan stopped, keep the `evictionBatch` lowest with `nth_element`, consume them, rescan when they run out. A
walk over the whole table per batch is not acceptable here: the host tier's scan holds `_lock`, which every lookup takes,
and the tier can hold hundreds of thousands of entries. The cursor makes successive scans cover the table, at most
`⌈entries ÷ evictionSample⌉` of them, without bias toward recency; a hit that moves the cursor's own entry to the recent
end skips ahead, and what it skips comes round on the next lap. A heap would need a rebuild anyway, because scores move
with time.

A candidate holds no reference, because a reference would read as a hold. When taken it is looked up by key without
touching recency, and refused if it has gone, is held, or was hit since the scan. A table no larger than the sample gets
an exact order, because each pass rescans; in a larger one, an entry that appeared after the scan waits for the next batch.

Eviction work per pass (one `createInternal` or `clearExceedingEntries` call) is bounded: at most `evictionScansPerPass`
scans of `evictionSample` visits each, plus one O(1) lookup and erase per eviction. Once a pass has used its scans, or a scan
finds nothing unheld, the remaining evictions take the oldest unheld entry as before, because a hard cap on evictions would
let a stream of large allocations overshoot RAM against many small entries. That fallback walks only the held prefix of the
recency list, which is the cost eviction has always had. The free-RAM check runs one pass per eviction, releasing `_lock`
between them. The number of evictions is what the bytes above the mark require, and each one removes an entry some earlier
create paid to insert.

Only `NodeCache` opts in (`useEvictionScore`). `ViewerCache` keeps the recency order.

An entry that no longer fits under any budget is not rejected: it lives in RAM while held, as today, and is then the
first to go.

A large deep render no longer pushes out images in recency order. Its entry weighs nothing until `allocateMemory()`
captures its size after the render; `clearExceedingEntries` then trims by score while the render still holds that entry, so
what leaves is what costs least per byte, whichever payload it is. Once released, a large deep entry of modest cost is
itself a low scorer.

### Recompute cost

Recorded in `renderRoI` (`Engine/EffectInstanceRenderRoI.cpp`, M300.P2.T2) and `renderDeepRoI`
(`Engine/EffectInstanceRenderDeep.cpp`), around the render action, for both schedulers: own wall time, minus time in nested
`renderRoI` and `renderDeepRoI` calls (a thread-local accumulator), scaled by `bounds area ÷ rendered area`. A hit, an
abort or a failure records nothing. Each entry's cost is an exponentially weighted average (weight 0.5) when it re-renders
in part. All planes of a render are charged the whole action, because recomputing any one of them runs it.

Tiles on worker threads publish their nested time to the render's stopwatch. The stopwatch subtracts the longest single
tile's nested time or the calling thread's, whichever is larger, and does not add them, because they overlap in wall time.
That under-subtracts when one worker pulls two slow inputs in two tiles. The cost only has to rank nodes that differ by
factors of hundreds.

An entry with no recorded cost scores as `unmeasuredCost`, 16 ms, so it is neither first to go nor protected: restored
entries, planes a plug-in adds on the fly, and members a GPU region publishes until M301 - GPU Cache Tier. An entry in
flight has none either, but a render holds it, so it is not a candidate. The holder's remembered cost, the last cost
any of its renders recorded (private images included), answers for these when the entry is bound to a holder that has
one. An entry holding no bytes scores 0: an unheld one is a render that never completed.

Why not reuse the scheduler's task wall time: see [Assumptions](#assumptions-checked).

### Demotion cost

`per-file latency + bytes ÷ measured write throughput`, per M300.P3.T1. The latency term is 2 ms, and the measured throughput excludes it so that it is not charged twice. Throughput starts at `diskWriteBytesPerSecond` and is averaged from
the writer's own measurements through `fdatasync`, so a modest disk learns it is modest within a few writes. The
per-node test has a problem for chains; see [Departures](#departures-for-the-user).

## Priority signals

`CacheRetentionSignals` (`Engine/CacheRetention.h/.cpp`) holds per-holder signal state, all atomics, written by whichever
thread sees the event and read by the score scan without a lock. Weights and half-lives are starting values, unmeasured;
see [Tunable constants](#tunable-constants).

```
priority = 1 + Σ wᵢ · sᵢ
```

| Signal | Set by | `sᵢ` | Weight | Half-life |
| --- | --- | --- | --- | --- |
| Input of a recently edited node | `Node::onKnobContentChanged`, reached from `KnobHolder::bumpKnobsContentGeneration` (`Engine/Knob.cpp`), for a knob that evaluates on change on a created node; stamps the node's direct inputs. An identity task in `buildGraph` passes its stamp to its inputs, unrenewed, so a Dot or a disabled node does not stop the boost; the legacy render path has no such carry | `2^(−t/h)` since the edit | 8 | 120 s |
| Input of an open panel | `Node::shouldCacheOutput`, testing the outputs' `isSettingsPanelVisible`: every call with one output, at most once a second with several | decays after the last render where it held, so an open panel with no renders fades | 4 | 60 s |
| Active viewer's branch | `noteActiveViewerBranch` (`Engine/ViewerInstance.cpp`): every node the frame's args setter installed, on both render paths, before the first admission question | decays after the last such frame | 2 | 10 min |
| Expensive node | remembered cost ≥ `expensiveCostMs` | 1 or 0 | 1 | none |
| Forced | `isForceCachingEnabled`, also through a group's output | 1 | 16 | none |
| Structural: output is read more than once or is costly to re-read | more than one output, roto output, temporal access, no tile support, not frame-varying, preview enabled, roto painting, nested render | 1 | 4 | none |

Direct viewer input and a panel on the single output are reasons that admit but carry no weight: the branch and panel
signals already say them.

Fresh values, highest first: forced 17, edited input 9, structural 5, panel input 5, branch 3, expensive 2, off-branch 1.
A signal past 8 half-lives reads as exactly 0, so an idle holder is back at exactly neutral. Signals add.

The reuse count is not in this sum; per the milestone it multiplies recompute cost: 3 hits is ×3, between a fresh panel
input and a fresh branch node when nothing else differs. It is per entry, the signals per holder.

Priority is read when scoring, so a panel opened after a store protects the entry once a render has seen the panel. A hit
by another holder moves `retention` to it, since entries are shared by content
(`docs/cache-invalidation/image-caches.md`, "Holder"). Reasons are per holder, refreshed by each admission question, not
per time or view, so the nested-render bit follows the latest call.

Costs of the signals are bounded for deep and broad graphs. Admission asks the same questions a node with no reason
always did. A stamp inside one second of the last is a load and a compare, so that a drag or a playing viewer does not
bounce a cache line. An edit visits direct inputs, at most once a second per node. The branch stamp is one load per node
the setter already walked. No new upstream walk.

`RotoDrawableItem` is also a holder (`Engine/RotoDrawableItem.cpp:365`). It has no signal state and scores at priority 1
unless its entries get others.

## Admission

Setting `cacheAdmissionPolicy` (`all` | `priorityFloor`), environment override `NATRON_CACHE_ADMISSION`, read once at
startup; an unknown value warns and leaves the setting in charge. The policy is an atomic, written on the setting's change
and after preferences load, because admission runs on every render call.

`renderRoI` and `RenderScheduler::buildGraph` ask one function, `EffectInstance::admitsOutputToCache`: loop variables
refuse, the analysis input is admitted, and everything else goes to the virtual `shouldCacheOutput`, whose default reaches
`CacheRetention::admit`. `renderRoI` adds the refusals that only it can know: OpenGL, the overlay pass and
`allowsStoreUnder`. Those are not retention questions, and there the scheduler's answer is a superset. A nested render's
reason depends on the thread's recursion level, so `renderRoI` can admit a node the graph did not plan to share.

`DiskCacheNode` and the test effect keep their overrides, reached through the same virtual, and record no reasons. The
first is a pin, which is outside the automatic policy; the second's knob is what its tests control. Neither
is a retention question.

- **`all`:** admit every output that passes the non-policy refusals above, including a node nothing reads.
- **`priorityFloor`:** admit when the output has any reason, a live edit, panel or branch stamp, or a remembered cost of
  at least `admissionCostMs`. Otherwise it renders into a private image, read by consumers from the frame store and gone
  with its last consumer. The noted static output is not a signal here: `renderRoI` notes it before asking and the
  scheduler does not, so a node with no output would get two answers. With an output it is already a reason.

The default is `all` (`CacheRetention::kDefaultAdmissionPolicy`). The edit-latency benchmark showed the chain identical
under both policies, while off the active branch `priorityFloor` re-ran unadmitted work on every edit.

Deep images do not ask: `renderDeepRoI` stores every output under both policies. Interior members of a GPU region are not
stored under either, until M301 - GPU Cache Tier.

Lookup is not gated. `getImageFromCacheAndConvertIfNeeded` ignores its `useCache` argument
(`Engine/EffectInstance.cpp:2226`), so an entry stored earlier still serves a node now below the floor, and records `hit`
or `partial`. `bypass` now means that a render used no entry.

Cost is known only after a render, and the decision comes before it. A node never rendered has no remembered cost, so
on a first render only signals can admit it.

**As worded, the two policies coincide on the benchmark graph.** The active viewer's branch is a signal, so
`priorityFloor` admits the whole branch, and every node of the benchmark's chain is on it. They differ only off the
active branch: other viewers, a background render, unviewed parts of a broad graph. A narrower reading, where the branch
signal does not admit, fails the gate: the first render knows no cost and has no edit, so it stores nothing and the
first edit runs the chain. M300.P4.T1 therefore needs a graph with off-branch work to tell the policies apart; see
[Gaps](#gaps-against-the-task-list).

`all` replaces the former aggressive caching preference, retired with no migration.

## Pins and holds

Two different things, both of which the score skips.

**An in-use hold** is "pinned" in the milestone's edge case, and exists today: a `shared_ptr` beyond the cache's own
(`use_count() == 1` is the test, `Engine/LRUHashTable.h:651`). The frame store and any running render hold one, and now
the pruning pass. Such an entry is never demoted, and the writer's own reference does not count as a render's. It ends
when the render lets go.

The being-rendered registry does not hold one: since M300.P1.T3 the slot lives in the header and keeps no `ImagePtr`. A
render normally holds its image for as long as its marker exists, but not everywhere. `renderRoI` drops its pointers while
its marks stand before the re-lookup (`Engine/EffectInstanceRenderRoI.cpp:1267`), and swaps the cache image for a resized
one (`:1524`). In those stale-marker windows an image can be evicted with its slot, and a waiter holding that slot's
`ImageBeingRenderedPtr` finds its image gone rather than kept alive by the registry.

A deep render hands its consumer a `DeepImagePtr`, not the entry, so that pointer shares ownership with the entry
(`DeepImageCacheEntry::pinnedDeepImage`, `Engine/DeepImageCacheEntry.h:75`) and pins it as an `ImagePtr` pins an `Image`.

**A pin** is a DiskCache node output, kept in a section of `NodeCache`'s disk tier; the node has no cache of its own:

- Written when the render ends. The node renders into an ordinary RAM entry, and `renderRoI` publishes it on its own thread
  (`Engine/EffectInstanceRenderRoI.cpp:1802`) to `<tier>/<build>/pins/<cacheID>/`, with the same temp file and `rename` as
  an automatic entry ([Writing](#writing)). Writing in place could not work: a published inode is never truncated, and a
  partial render would be visible to peers. Each holder has its own file, since two nodes of equal content share one RAM
  entry. The RAM copy stays until evicted, and is then dropped without demotion, because the file already exists.
- Persistent, and outside sampling: the eviction pass never lists `pins/` ([Budget and eviction](#budget-and-eviction)).
- On its own budget: `maxDiskCacheNode`, default 10 GiB (`Engine/Settings.cpp:1827`), across all holders. It does not
  count against the 20 GiB automatic tier: the scan skips a live build's `pins/`, and a pin write touches no ledger. Its
  used figure is a listing of `pins/` taken under a lock for each write, since pins are few and written once per output.
- Read in place on a local disk: privately mapped, with its descriptor closed, so it counts as RAM while mapped and not
  against the open-file cap (`CacheDiskTier::Reader::mapRest`). A later partial render writes to its own copy of the pages.
  On a network mount a pin is read into RAM instead, because a peer's unlink there turns a mapping into `ESTALE`, which is
  `SIGBUS` ([Fallback](#fallback)).
- Looked up before the automatic tier by the node's own key, and then in it, so that an output that did not fit the budget
  still hits (`Cache::getPinned`, `Cache::getOrCreatePinned`).
- Never truncated once published. Extending a pin (a larger render of the same output) writes a new temporary file and
  renames over the old one; a peer that has the old inode mapped keeps a valid mapping.
- Marked by the sessions that hold it: each live session writes `pins/<cacheID>/.live-<session>` at its first lookup or
  publish for that holder.

What removes a pin:

- Deleting the node (`Engine/AppManager.cpp:2608`) removes this session's marker, and unlinks the pins only if no live
  peer's marker remains for that holder. A marker of a dead session does not count, since any session may reap it.
- Clearing caches removes the pins of the holders this session has, and never another holder's. A peer re-renders and
  re-pins.
- A build with no live session being reclaimed ([Builds](#builds)), pins included.
- The rule below.

A deleted project's pins are not removed by anything else: they hold their budget until a clear or a new build. This is
the user's choice.

A pin is never evicted, which leaves the question of a full pin budget:

1. When a pin would exceed the budget, first drop this holder's pins at a different node hash. They are invalid, not
   evicted.
2. If it still does not fit, the output is not pinned: the render returns its pixels through RAM, and one warning is
   logged per session.

## Persistence

Applies to the automatic tier and the pins. The `Cache<FrameEntry>` playback cache keeps its own files and table of
contents (`Engine/AppManagerPrivate.cpp:408`, `:458`).

The tier is shared by any number of sessions (the GUI, background renderers, several GUIs), none of them privileged. The
protocol's correctness rests on `rename` alone. No lock gates access, and the locks and caches below only make it faster.
Every file is immutable once published, so a reader sees a whole old file or a whole new one.

### Location and size

Preferences: location (default `~/.cache/Natron/tier`), size (default 20 GiB). The effective budget is capped at 25% of
free space. Free space here includes the tier's own files, or the cap would shrink a full tier a little more at every
launch; this is an interpretation of the milestone's wording. Each process computes the cap from its own view of the free
space and the smallest view wins.

### Layout

```
<tier>/sessions/<host>-<boot>-<pid>-<start>        one writer: build, signed byte delta, heartbeat, closed
<tier>/tmp/<session>/
<tier>/evict.lock                                  try-lock, optimisation only
<tier>/<build>/lastlive                            mtime: when a session of this build last ran
<tier>/<build>/auto/<xxx>/<base>-<variant>-<params>.ntc
<tier>/<build>/pins/<cacheID>/<base>-<variant>-<params>.ntc
<tier>/<build>/pins/<cacheID>/.live-<session>      this session holds these pins
```

- `base` is the XXH3 of the key without mip level, layer and bit depth. `variant` is the XXH3 of those three, and `params`
  of the entry's params (`Engine/Cache.h:1549`: several entries can share a key and differ in params). A file is named by
  hash, never by content. `<xxx>` is a three-digit hexadecimal shard taken from `base`; at 300,000 files that is about 70
  files per shard (an estimate).
- A session is named by host, boot ID, process ID and process start time, so a recycled PID is a different session.
- `base` leaving out mip, layer and depth is what lets a probe list one shard and filter by `base` to answer every variant
  of an output in one call ([Reading](#reading-and-restoring)).

Each file holds, in order:

| Part | Contents |
| --- | --- |
| Header, 4 KiB | magic, format version, `NATRON_CACHE_VERSION` (`Global/GlobalDefines.h:97`), payload kind, typed key, typed params, bounds, sizes, payload XXH3, all-rendered flag |
| Stats block | best-effort: `lastUse`, `reuseCount`, `priority`, `recomputeCostNs`. Rewritten in place, never trusted for correctness |
| Bitmap | run-length coded, absent when the flag says all rendered, and for deep |
| Payload | the pixels |

Deep payloads are not flat: counts, offsets, then a shared sample vector (`Engine/DeepImage.h:108-109`, `:168`), so each
payload type provides its own write and read, in those three sections. M300.P1.T2's new key fields and the deep key and
params need serializers, which live in the file header. `Engine/CacheSerialization.h:83` is not involved: the tier has no
index for it to extend, and it stays with the viewer cache, which restores through it as before.

The name is not an identity. `Hash64` is CRC-64, so two keys can name the same file. Every read compares the full key and
params in the header with the requested ones and treats a difference as a miss. A node-hash collision remains
possible, as it is in RAM today. The top bit of a node hash carries the salted flag ([Builds](#builds)), which leaves 63
bits, and about 5×10⁻⁹ with 300,000 entries (computed from the birthday bound).

### Writing

1. Create the file in `tmp/<session>/`. `posix_fallocate` it to its full size. `MemoryFile::resize` only calls `ftruncate`
   (`Engine/MemoryFile.cpp:304`), which makes a sparse file; the sparse file is the reason for the call, and `ENOSPC` is
   then a clean failure at this step.
2. `pwrite` the header, bitmap and payload, then `fdatasync`.
3. If a file of that name exists in `auto/<xxx>/` (a `stat` and a header read) and has at least as many rendered pixels,
   skip: remove the temporary file. Otherwise `rename` it over the old one. The larger result replaces the smaller.
4. Mark the entry disk-resident, and free its RAM and bitmap if this was a demotion. Add the file's size to this session's
   ledger.

There is no pixel merge. GPU and CPU outputs share keys while differing by rounding, so merging the rendered parts of two
files could leave a seam between them.

A published inode is never truncated or written again, which is what keeps a concurrent `pread` or mapping safe.

Any failure removes the temporary file, drops the entry and logs one warning per session, through a once-only flag.
`RLIMIT_FSIZE` is such a failure.

**Publish-behind.** Demotion is not the only writer. An idle low-priority thread publishes entries that pass the
recompute-versus-write test, and keeps their RAM copy, so a peer can see an entry before it is evicted and a crash loses
nothing a peer could have used. The write-cost estimate has a per-file latency term next to `bytes ÷ throughput`, since
thousands of `fdatasync` per second are not realistic; a small cheap entry is below the test whatever its size. A
demotion of a published entry first `stat`s the file: a peer may have evicted it, and then the demotion writes again or
drops by the usual test. A retracted entry (dropped, replaced or cleared) cancels its queued publish; one removed because
its result is invalid also unlinks the file it published, and a removal by key unlinks the key's files of every params
variation, whichever session wrote them.

A clear cancels the writes, empties RAM, then empties the tier, in that order: emptying the tier first would let the idle
publisher sample an entry still in RAM and write it into the emptied tier, where a later lookup would take it for valid. A
publish batch sampled before any cancellation is dropped rather than queued after it, and the idle publisher samples
nothing while a clear runs. Emptying RAM keeps an entry only while something other than the cache holds it: the reference
of a publish the clear abandoned is not such a use, though the write may hold it until after RAM is emptied, and a held
entry no longer shields the entries behind it in eviction order. Queued demotions are dropped under
the same lock that empties RAM, and a write the clear cancelled is never handed back to a lookup, even while it is still
running: either would put the cleared entry back in RAM. A promotion that read its file before or during a clear finds
the clear generation moved and returns a miss instead of adding the entry.

A demotion's write failure drops the entry, as it must leave RAM. A publish-behind failure keeps the RAM copy, which is
still valid, abandons the file and backs the publisher off for 30 s. Both warn once per session.

An `Image` file's params hash leaves the bounds out, since an image grows in place and a larger result must find the
smaller file to replace. A `DeepImage` has no other params, so its bounds name the file.

**Threads and locks.**

| Thread | Count and priority | Work | Locks |
| --- | --- | --- | --- |
| Render thread | the caller's | probe and promotion, `pread` and checksum | no cache lock during I/O; `_lock` briefly to evict for the promotion and to seal the result; one `PromotionGate` per key, so two renders read a file once |
| Scheduler thread | the caller's | `peek`: a probe after RAM misses, returning unallocated stubs from verified headers; a partial file claims no pixels, so the plan only over-estimates | no cache lock during the probe |
| Demotion writers | `clamp(hardware threads ÷ 8, 1, 4)`, normal priority | write, `fdatasync`, rename | `queueMutex` for the queue only, a leaf taken under `_lock` by offers, reclaims and peeks, never held across I/O; the pixels' `_entryLock` read-locked per 4 MiB chunk while copying to a scratch buffer, never across a write |
| Publisher | 1, nice 19 and idle I/O class | publish-behind, at most 8 per batch, yielding to demotions | `publishMutex`, never taken under `_lock`; holds weak references until it writes |
| Maintenance | 1, normal CPU and idle I/O class | ledger, budget, eviction, publish candidates, on a tick of at most 1 s | `_lock` briefly through `publishCandidates`, sampling the table |
| Scanner | 1, nice 19 and idle I/O class | deletes cleared directories, walks the tree every 60 s | none of the cache's |

Many-core machines do not serialise on the publisher because demotions never pass through it, and writers and readers
share no lock over I/O. Modest machines do not stall on disk because neither `_getLock` nor `_lock` is held over a read
or a write, and the queue bound turns a slow disk into recomputes. The last reference to a published entry can drop on
the publisher, where freeing it takes `_sizeLock` at nice 19 for the time of a counter update.

### Reading and restoring

There is no index and no start-up scan. A restart finds an entry by the name its key computes, so the first session to
start has no advantage over the others.

A probe runs off the cache lock, after a RAM miss:

1. List the one shard `auto/<xxx>/` that `base` names, and keep the names with that `base`. One listing answers every mip,
   layer and depth variant, which the fallbacks in [Existing classes](#existing-classes) need.
2. For a candidate, open it, read and verify the header (magic, versions, full key and params), `pread` the payload and
   check its XXH3. Any error, `ESTALE` or mismatch is a miss.
3. A promoting render copies into a `HostImageMemory` buffer and reloads the bitmap. A non-promoting probe
   (M300.P2.T5) reads only the header.

Negative results are cached for 1 s, or 2 s for a listing on a network mount, and a listing is cached on a network mount.
There is no `inotify` and no polling: a peer's publish is seen at the next probe after the negative result expires.

Automatic entries are not mapped. With sharing, another client's unlink on a network mount gives `ESTALE`, which on a
mapping is a `SIGBUS` rather than an error. Promotion copies the file anyway, so reading it with `pread` costs nothing
extra. See [Departures](#departures-for-the-user), D2.

A restored entry is found only when the session computes the same key. It does, with these provisos:

- Hashes are expected to be stable across sessions and AppImage remounts (`docs/cache-invalidation/keys.md`, "Stability
  across sessions"; `Tests/HashInvariants_Test.cpp`). This has been read in the code, not proved across processes:
  `SameGraphHashesEqualInASecondAppInstance` runs both instances in one process. M300.P3.T4 proves it with real
  processes, including `NatronRenderer`.
- The holder is not in the hash or equality (`Engine/ImageKey.cpp`, `fillHash`, `operator==`). Project files keep a node's
  cache ID since M260 - Cache Invalidation Follow-ups, so a holder's later removal still finds its entries.
- A changed source file changes the key: file knobs stamp path, mtime, size and inode (`computeStamp`,
  `Engine/FileStampRegistry.cpp`), so the stale entry misses, and Reload re-reads the file. The stale entry ages out. A
  source rewritten while sessions run keeps its old stamp until Reload, so two running sessions can hold either version.
- A volatile plug-in's hash is salted per process (`Engine/Plugin.cpp`, `kVolatilePlugins`). A salted hash, and every hash
  downstream of one, is flagged and never published, so no file is written that could not hit.

Since the bitmap travels in the file, a restored entry keeps its rendered pixels: `Image::onMemoryAllocated` starts every
entry unrendered, and `Image::diskRestore` then writes the file's bitmap over it.

### Budget and eviction

Used bytes = the last scan of the tier + the sum of the sessions' ledgers (their signed deltas since that scan), refreshed
each second. A slow scan, run in the background, corrects the drift. Concurrent writers can overshoot the budget by about
processes × write rate × 1 s; this is assumed to fit inside the 2 GiB between the 90% mark and a 20 GiB budget (an
estimate, to be checked in M300.P3.T4 with four writers).

Past the mark, a session samples about 1024 files and unlinks them lowest first by the note's score
([Score](#score-and-high-water-marks)), using the persisted stats, until the tier is back under the mark. A pass takes 64
files at most, and if that is not enough the next tick continues. As with the host tier there is no low-water mark: a
tier whose entries are so large that it holds fewer than 64 files (20 GiB of 384 MB entries is about 52) loses only what
takes it under the mark, not all of them. The pass then writes its ledger and refreshes used bytes, so neither this
session nor a peer evicts for the same bytes twice. A process substitutes the live priority for the entries it knows.
Files of builds with no live session are taken first, the build with the oldest `lastlive` before the next, and a dead
build's pins are sampled with its automatic files. `evict.lock` is tried with `flock(LOCK_NB)` before sampling, and a
session that does not get it skips the pass; it is only there to avoid duplicate work. Two evictors at once are harmless,
and `ENOENT` on an unlink is fine.

### Sessions and crashes

Only `tmp/` ever holds partial data, so a crash leaves nothing that a reader can mistake for an entry.

Every session writes its ledger under `sessions/` and refreshes its heartbeat with the budget. Any session reaps a dead
session's `tmp/<session>/` and ledger, from the scanner thread. Same host: dead means a different boot ID, or no process of
that PID with that start time. Another host: dead means a heartbeat that has not changed for `heartbeatStaleMs`, measured
on the reaper's own clock since two hosts' clocks need not agree. The reaper first `rename`s the ledger into its own
`tmp/<session>/` to claim it, so two reapers do not both add its delta.

A clean exit writes `closed` in its ledger, which makes it dead at once, and removes only what lies directly in its temp
directory, so that closing the application never waits on a large tree: a peer, or the next session, finishes the job.
`kill -9` leaves the ledger and temp directory for the same reaping. A session on the same hostname in another PID
namespace can look dead to this one; if it is reaped by mistake, its next tick registers it again, and its writes lost
only their temp files.

At start, on the main thread, a session stats the two executables and opens the tier: at most four `mkdir`s, the ledger,
the self-test (a few small file operations, no `fsync`), `evict.lock`, `statfs`, the build stamp, one listing of `sessions/`
and `statvfs`. There is no restore scan. Reaping, reclaiming and the scan run on the scanner thread at idle priority, and
the heartbeat and eviction on the maintenance thread.

A lock that fails (`flock` on some network mounts) loses an optimisation and never access.

### Builds

The identity is the hash of `NATRON_VERSION_STRING`, `GIT_COMMIT` and the executable stamp (`Engine/Plugin.cpp:67-72`),
and it names the build directory. The key keeps hashing the binary as well (the user's decision), so a new build would miss anyway; the directory is what lets a dead build's files be found and reclaimed as a unit.

- That stamp hashes `/proc/self/exe`, and Natron and NatronRenderer are separate executables, so the stamp must cover both
  siblings, sorted by name, or the same build would key differently in the two. The AppImage ships both in `bin/`, where
  `tools/release/stage-bundle.sh` renames each ELF to `<name>.bin` and leaves a shell shim under the plain name, so the
  stamp reads the `.bin` files and never the shims. Without both side by side, as in a build tree (`build/App/`,
  `build/Renderer/`), it falls back to the running executable's own stamp, and the two executables then have separate
  identities. M300.P3.T4 runs the staged pair on one project and compares their hashes.
- When `statx` fails, the stamp falls back to a per-process salt (`Engine/Plugin.cpp:74`, hole K8). Salted hashes
  (`Plugin::getBinaryContentStamp`; the two sites in `Engine/Knob.cpp` that call `ContentHasher::appendSessionSalt`) and
  everything downstream carry a flag in the top bit of the hash, which travels with the value through memos and keys, and
  are never published. A missed flag only wastes disk, since a salted key never hits. A session whose own stamp is salted
  is `unstamped`, with `Config.publish` off, so it reads but fills nothing. K8 stays with M260 - Cache Invalidation
  Follow-ups.
- An OFX plug-in rebuild changes only its own nodes' keys (`Plugin::ofxBinaryStamp`). Those entries age out.

Each identity has its own directory, so builds coexist and nobody wipes at start. A new build starts cold and touches no
other build's files. A build with no live session is dead after 7 days (the user's choice), or earlier under budget
pressure, oldest first, pins included (also the user's choice). Age is the mtime of `<build>/lastlive`, which each live
session touches about once a minute; a directory without that file is not a build and is never touched. A background `NatronRenderer` started from the GUI
re-executes the same binary, so it has the same identity and shares the tier, DiskCache pins included.

### Fallback

At start the tier runs `statfs` and a `rename` self-test in `tmp/`. A network mount loses only the speed-ups: listings stay
cached, and pins are read into RAM instead of mapped. If the self-test fails (a filesystem whose `rename` is not atomic or
not permitted), every session uses the local default location, and if that fails too, RAM only, with one warning
(`CacheDiskTier::openWithFallback`). A failed location keeps none of the session's files. The
decision is the same in every session because it follows from the same test.

### What each session sees

| Case | Behaviour |
| --- | --- |
| Different projects | No shared hits except identical upstream. They share the budget by score, both persist, and closing one takes nothing from the other |
| Same project | Hits on matching hashes, seen after publish-behind plus at most 1 s. Concurrent renders duplicate work, and the last larger publish wins |
| A node is deleted | Its automatic files stay and age out. Its pins follow [Pins](#pins-and-holds) |
| Clear caches (the user's command) | This build's automatic files and this session's pins go. Peers re-render and re-pin |
| The project's views change | RAM only: `AppManager::clearAllCachesInRam`. The project digest holds the view names, so the keys move and no peer loses a file |

### Assumed costs

Unmeasured until `tools/bench/cache_probe_bench.py` has been run against a built `Tests`; the numbers below are estimates. A
cached local miss is 10-30 µs. A cold miss is about 100 µs on NVMe, 10 ms on an HDD and about 1 ms on NFS. A frame with 1000
nodes missing costs about 30 ms before the negative cache.

```
python3 tools/bench/cache_probe_bench.py --tests build/release/Tests/Tests --dir <a directory on the disk under test>
```

It plants 300,000 sparse files and appends one line to `build/bench/cache-probe.jsonl`. `miss_per_frame_ms` is one frame's
worth of 1000 missing nodes over a second at 24 fps, one frame of it paying for the probes and the rest answered by the
negative cache; the bench exits non-zero when that passes 30 ms. `miss_no_negative_cache_ms` is the same frame with the
negative cache forgotten before every probe, `scan_ms` the slow scan, `tick_ms` a budget tick and `tick_with_eviction_ms` a
tick that samples and unlinks. The planted files have no valid header, so a sample reading one sees what a corrupt file costs.

### Testing across processes

`Tests/CacheDiskTierProcess_Test.cpp` runs sessions as separate processes through a worker mode of the test executable
(`Tests --natron-tier-worker`, which skips loading the application). Four free-running writers may pass the budget between
budget refreshes, by design: the test prints the peak it saw and asserts the budget where all four have just ticked.

The content-hash case compares two test processes, then a hard-linked `Natron` and `NatronRenderer` in one directory, because
a build tree keeps them apart and each would stamp only itself. A release's own `.bin` pair is checked by hand:

```
tools/bench/cache_hash_pair.sh <AppImage or bundle directory>
```

## Device tier hooks

M301 - GPU Cache Tier fills these; M301.P1.T1 adds its section below. M300 only has to not close them off.

| Need of M301 - GPU Cache Tier | What this design provides |
| --- | --- |
| `CacheDeviceTier` is another tier | `CacheTier` with `next()` pointing at the host tier, under the same score and high-water logic; budget shared with the GPU pool and working buffers winning, per `DECISIONS/2026-11-01-unified-tiered-cache.md` |
| A cached lease carries the entry header | The header is separately owned (`shared_ptr`) and requires no RAM buffer, so a device-only entry has a key, bitmap and costs |
| Working allocations evict cached leases first | `demote` is callable from the pool's eviction callback (`Engine/Gpu/GpuBufferPool.cpp:157`, `:264`); download through `evictToRam` (`Engine/FrameRenderContext.cpp:308`) when recompute cost exceeds download cost, else release |
| Region members publish under `ImageKey` with a recompute cost, subject to admission | Admission is one function; `storeOutputs` (`Engine/GpuRegionTask.cpp:2982`) is where publishing goes. Nothing measures per-member cost today; that milestone defines it |
| `isCached` becomes a tier lookup; a device hit marks a task served and splits its region | The non-promoting probe covers all tiers through `residency`. `pruneBehindCacheHits` runs before placement, and "covered" is the same notion as "served"; M301.P2.T1 should extend it rather than add its own walk |
| A host hit with a GPU consumer uploads | `promote` on the device tier; `residency` is a mask, so the host copy can stay |
| Partial device hits | The bitmap is in the header, usable with no host pixels |
| Host import keyed by entry key, not address (G1) | `key` in the header; `HostImageMemory::addFreeHook` stays (`Engine/HostImageMemory.cpp:454`) |
| Device loss empties the tier | Clear the device bit on every entry; entries with another copy live on, and later lookups fall through to the host |
| Multi-GPU keys per device | `residency` and per-device tiers are that milestone's to extend; the mask is not a promise of one device |

Hook for M303 - Interactive Edit Latency: M300.P1.T2's lookup asks for the exact key and then falls back through finer mips.
A draft miss trying the non-draft key at the same or a finer mip is another step in the same fallback chain, on host or
device. A mip-0 output of a non-scaling node is its own entry once mip is in the key. This note designs neither.

## Existing classes

| Class | Today | Becomes |
| --- | --- | --- |
| `Cache<T>` (`Engine/Cache.h:396-397`) | One entry type per instance, constructed at `:919`; four instances (`Engine/AppManager.cpp:1105-1115`); LRU and RAM/disk split inside | `Cache<FrameEntry>` stays as it is for `ViewerCache`, tiles and own budget. `NodeCache`, `DeepImageCache` and `DiskCache` go into one non-template tiered cache with typed `Image` and `DeepImageCacheEntry` accessors, so `AppManager`'s getters keep their shape. As of M300.P1.T4 the deep entries are in `NodeCache`, which is still `Cache<Image>`: its containers hold `AbstractCacheEntryBase` pointers, and `get` and `getOrCreate` are member templates on the payload that filter by type. `DiskCache` and the non-template cache remain ahead |
| `LRUHashTable` (`Engine/LRUHashTable.h`) | Active variant `BoostLRUHashTable` (`:476`), since `NATRON_CACHE_USE_HASH` and `NATRON_CACHE_USE_BOOST` are defined (`:67-69`); the milestone's `:199` is an inactive one. Any lookup moves to most recent; `evict` (`:544`) takes the front with `use_count() == 1` (`:549`). M300.P2.T2 added `scan`, the cursor of the sampled scan, and `takeIf`, a removal that leaves recency alone, to the active variant | Used only by `Cache<FrameEntry>`. The tiered cache uses a plain hash table of entry lists, with recency in the header |
| `CacheEntryHolder` (`Engine/CacheEntryHolder.h:40-57`) | Interface with `getCacheID()`, a UUID on `Node` (`Engine/Node.cpp:912`) | Unchanged. Still owner identity for removal and stats, and now the key into `CacheRetention`, interned on the header |
| `HostImageMemory` (`Engine/HostImageMemory.h`) | Pooled importable RAM; pool counts against the limit and is trimmed first (`trimTo`, `Engine/Cache.h:839`, `:1170-1176`) | The host tier's allocator, unchanged. The tier trims the idle pool before scoring anything. Promotion allocates here, so a promoted entry stays importable |
| `DeepImageCacheEntry` (`Engine/DeepImageCacheEntry.h:58-59`) | `CacheEntryHelper<char, …>` with `DeepImagePtr` beside an unused generic buffer; size snapshot at `:86-90`; its own 2 GiB instance | A payload type with a header, without the buffer accounting (done in M300.P1.T4, its instance and setting retired). Its size is real bytes for the score. The old reason for its own budget (`Engine/AppManager.cpp:1112-1113`, a big deep render would evict all 2D) is answered by the score: large and cheap goes first, whatever the type |
| `MemoryFile` (`Engine/MemoryFile.h`) | One mapped file, not MT-safe (`:43`); destructor keeps the file (`:98-100`) | Backs nothing in the tier: a pin on a local disk is a private `mmap` of its published file (`PrivateFileMapping`), and automatic entries use `pread` and `pwrite` on a plain descriptor. Its `ftruncate` at `Engine/MemoryFile.cpp:304` makes a sparse file, so the disk tier reserves with `posix_fallocate` instead |

Also: `ImageKey` gains mip level, layer and bit depth (`Engine/ImageKey.h:42-51`, `Engine/ImageKey.cpp` `fillHash` and
`operator==`). The filtering at `Engine/EffectInstance.cpp:2279-2326` that this makes redundant goes; the finer-mip downscale
stays as a fallback, by probing finer keys. M260 - Cache Invalidation Follow-ups found that `RgbThenRgbaRequest` already passes
(`docs/cache-invalidation/image-caches.md`, I4); that test is what the colour-layer and depth fallbacks must keep passing. Today
equal or deeper depth and any colour layer match (`Engine/EffectInstance.cpp:2304-2305`), so an exact key alone would turn those
hits into misses. Those fallbacks need their own probe steps, or a secondary index by base hash.

The `getElementsCountFromParams` used as a byte count at `Engine/Cache.h:1931` is a unit mismatch that goes away with that code.

## Hardware range

**Many cores, much RAM.** A probe, a hit and a scan each take the cache lock, and today `_getLock` plus `_lock` serialise every
get (`Engine/Cache.h:506`, `:637`). This design moves the slowest things off it: disk writes, promotion, the free (the deleter
stays), and per-entry state, which is atomics. On a 256 GiB machine the marks are percentages, so no absolute constant limits
the cache. `HostImageMemory`'s pool and the score scale with it. Shard the lock only if measured; see
[Departures](#departures-for-the-user).

**Modest machines.** At 16 GiB and default settings the budget is 8 GiB, and `all` stores about 1.9 GB per frame on the
benchmark's chain (5 × 384 MB, estimated), so about four frames. There the score matters and `RawInputColor`, 640× cheaper per
byte than `RawDemosaic`, goes first, re-running in under a millisecond off whatever is above it. A slow disk learns its
throughput and gets few demotions. The demotion queue bound keeps a slow disk from stalling a render. The tier's 25% cap
applies on a small disk.

**Deep graphs.** Pruning follows only what will run, and the edit touches direct inputs. The O(N²) upstream walks in the render
scaling findings are not added to.

**The free-RAM check under `all`.** `createInternal` runs `checkCacheFreeMemoryIsGoodEnough` per store, and a private image
never reaches it. `all` turns every missing output into such a store, so `/proc/meminfo` is parsed once per output on a first
render, under `_getLock` (unmeasured; hits and refused outputs do not pay it). It also means the check sees allocations it
used to miss, and memory that was private becomes accounted and evictable once its render lets go. Entries the frame still
holds count in `getMemoryCacheSize` but cannot be evicted, so a shortfall can stop the loop with RAM still short, as private
images did. The floor keeps today's blind spot: a refused output's allocation neither refreshes the budget's RAM term nor
runs the check.

**Broad graphs.** Many small entries: the scan is bounded by `evictionSample` per batch whatever the table holds, a header is a few hundred bytes, and the bitmap of a
disk entry is not in RAM. The branch signal separates what is viewed from what is not. This is where `all` and `priorityFloor`
differ.

## Assumptions checked

**A1: the bitmap and the being-rendered registry can move out of `Image` without changing
`waitForImageBeingRenderedElsewhere`. True, with conditions.**

- The registry is not in `Image` today. It is a global map keyed by `ImagePtr`, leaked on purpose
  (`Engine/EffectInstancePrivate.cpp:835-861`). Holding it per entry removes `registry.mutex`. The recheck at `:999-1003` stays:
  the race between the last unmarker and a new mark outlives the global map.
- Each entry has its own `pendingLock` for its slot, not the reservation stripe the first draft of this note named. The stripe
  is held across bitmap scans, so a wait and an unmark would stall behind it, and taking it there risks deadlock with
  `_entryLock`.
- Keep the reservation stripes. One render must reserve its rectangles on all its layers before another queries any
  (`:836-839`, `:884`), and that is a property across entries, which no per-entry slot gives.
- Keep the bitmap under `_entryLock` as well as `ibr->lock`: `getRestToRender_trimap` takes the former
  (`Engine/Image.h:570-579`), the wait holds the latter (`Engine/EffectInstancePrivate.cpp:945-952`), and `ensureBounds` swaps the
  bitmap together with the buffer under the former (`Engine/Image.cpp:1144-1158`).
- Uncached images use it too, so `Image` always has a header.
- An entry with a writer is held by `ImagePtr`, so it cannot be demoted, and the pointer keeps its identity across moves. A
  waiter holds the slot, not the image, and the registry no longer holds an `ImagePtr`, so outside the windows named in
  [Pins and holds](#pins-and-holds) the wait is covered by the render's own hold and in those windows the image can be
  evicted with its slot. The semantics survive tiers on those terms.

**A2: recompute cost can be measured as the node's own render wall time, "already split out per task by the scheduler". False as
stated.** Own wall time is the right quantity, and M300.P2.T2 records it in the right file, but nothing provides it today:

- The scheduler reads the clock only when the profile sink is enabled (`Engine/RenderScheduler.cpp:1216`, `:1227-1229`), and only to
  write a record. It stores no cost anywhere.
- A task's wall time can include its inputs. An input that misses the frame store renders by recursion inside the consumer
  (`Engine/EffectInstance.cpp:1155-1158`, `:1625-1660`), and the pre-render path recurses (`Engine/EffectInstanceRenderRoI.cpp:1405`).
- The legacy scheduler has no tasks, and renders inputs inside `renderRoI`.
- A hit's or a partial's time is not a recompute cost. A GPU region has no per-member time at all.

Hence the accumulator in [Recompute cost](#recompute-cost). Wall time also varies with the thread budget
(`OpenMPThreadsScope`, `Engine/RenderScheduler.cpp:1214`) and with load. That is accepted: the score compares magnitudes, and the
measured nodes differ by factors of hundreds.

## Tunable constants

| Constant | Start | What measurement would change it |
| --- | --- | --- |
| `highWater` | 0.9, all tiers (today's `NATRON_CACHE_LIMIT_PERCENT`, `Engine/CacheTier.h:38`) | Allocation waits or free-RAM evictions in the peak-RAM run of M300.P4.T1 lower it; a run where a tier sits unused above it raises it |
| `reuseHalfLife` | 30 min | Eviction count in a scripted session that switches branches and returns: raise it if returning re-runs expensive nodes, lower it if orphans hold the budget |
| Edit half-life, weight | 120 s, 8 (unmeasured) | Upstream executions on the second edit after a pause, in a variant of the benchmark with memory pressure: raise if a pause re-runs the edited node's inputs, lower if stale edits keep entries over the viewed branch |
| Panel half-life, weight | 60 s, 4 (unmeasured) | As above, panel open, then closed |
| Branch half-life, weight | 10 min, 2 (unmeasured) | Same run on a broad graph: raise if the viewed branch is evicted before off-branch work |
| Forced weight; structural weight | 16; 4 (unmeasured) | CacheStress under pressure: a forced node's entry must outlast unforced ones of equal cost |
| Expensive signal weight, `expensiveCostMs` | 1, 16 ms, one 60 Hz frame (unmeasured) | Measured walls straddle this: `RawDemosaic` 383.6 ms at fit, 7.1 at 100%. Move it if the signal separates nothing |
| Signal stamp granularity; decay cutoff | 1 s; 8 half-lives (unmeasured) | Cache-line contention in a profile of a drag on a large graph coarsens the first; only precision against a half-life limits it. The cutoff moves only if entries of idle holders still separate at its value, which is under 1% of a weight at 8 half-lives |
| `admissionCostMs` | 16 ms | The off-branch graph in M300.P4.T1: lower if off-branch re-runs dominate latency; raise if off-branch writes dominate peak RAM |
| `evictionBatch` | 64 | Scan time from a profile of a cache with many small entries; a scan over 1 ms of lock hold lowers the scan rate by raising it |
| `evictionSample` | 1024 (the disk tier's sample) | The same profile lowers it if a scan holds the lock too long; eviction quality on a large table (an expensive entry evicted while cheaper ones wait outside the sample) raises it |
| `evictionScansPerPass` | 4 | How often a pass falls back to recency order under a large allocation; raise it if that evicts expensive entries |
| `unmeasuredCost` | 16 ms, as `expensiveCostMs` | How restored entries fare against measured ones once persistence lands |
| `diskWriteBytesPerSecond` | 500 MB/s, then learned | Start value only; matters for the first writes. Set it from the median over the user's and a SATA host's first-minute rates |
| `demotionQueueFraction` | 0.1 of host budget | Allocation stalls versus drops during a sustained run on a slow disk |
| Negative-cache lifetime | 1 s; 2 s for a listing on a network mount | `tools/bench/cache_probe_bench.py`: raise it if misses cost more than 30 ms per 1000-node frame, lower it if a peer's publish is seen too late |
| Budget refresh; slow scan period | 1 s; 60 s (unmeasured) | Overshoot of four writers in `CacheDiskTierProcess_Test`: shorten the refresh if they exceed the 2 GiB between the mark and the budget |
| Eviction sample; unlinked per pass | about 1024; 64 (`evictionBatch`) | Sampling time on a 300,000-file tier: `tick_with_eviction_ms` of `tools/bench/cache_probe_bench.py` |
| Per-file write latency term | 2 ms, unmeasured | A small-entry publish bench with `fdatasync`; it decides how small an entry the write test rejects |
| Dead build; heartbeat stale | 7 days (the user's choice); 60 s (unmeasured) | The age is fixed by the user; the heartbeat by false reaps seen on a network mount |
| Default disk size and cap | 20 GiB, 25% | Fixed by the milestone; changed only by the user |
| Entry-file bitmap coding | run-length | Average bitmap bytes per entry from a real tier: if partials are rare, a flag alone would do |

No constant caps the size of a user's image: an entry above every budget still renders and is held while in use.

## Edge cases

| Case | Behaviour |
| --- | --- |
| Disk full or a write error on demotion | Dropped, with one warning per session ([Writing](#writing)). `posix_fallocate` makes full-disk a clean error rather than a short file |
| A second Natron instance | A peer on the same tier, none locked out or given scratch space ([What each session sees](#what-each-session-sees)) |
| A changed source file across sessions | Miss through the file stamps of M260 - Cache Invalidation Follow-ups; Reload re-reads ([Reading and restoring](#reading-and-restoring)) |
| An entry in use | In-use hold; never demoted while a render holds it ([Pins](#pins-and-holds)) |
| A partially rendered entry | Bitmap goes with the file and returns on promotion ([Entry header](#entry-header)) |

## Gaps against the task list

**1. Skipping dependencies behind a hit.** Owned by M300.P2.T5, designed under
[Pruning the task graph behind a hit](#pruning-the-task-graph-behind-a-hit). M301.P2.T1 extends the pass to device hits
instead of walking on its own.

**2. Task file lists that omit files their work needs.**

| Task | Missing | Why |
| --- | --- | --- |
| M300.P1.T2 | `Engine/GpuPlacement.cpp` | It builds an `ImageKey` itself (`:1032`) and would stop matching |
| M300.P1.T2 | Key serialization | The new fields must persist with the key |
| M300.P1.T4 | `Engine/Cache.h`, `Engine/Settings.cpp`, `Engine/EffectInstanceRenderDeep.cpp` | `Cache<T>` holds one entry type and constructs it at `:919`, so one cache cannot hold both without splitting it; the 2 GiB setting lives in Settings; the deep renderer calls `removeFromDeepImageCache` (`:563`). The hash needs its domain tag here. Node-cache clears, memory stats and pressure eviction now reach deep entries, and a holder purge ignores pins for them as it does for images |
| M300.P2.T2 | Thread-local nested-time accumulator, `Engine/EffectInstance.cpp` | [A2](#assumptions-checked) |
| M300.P2.T3 | `Engine/RenderScheduler.cpp`, `Engine/ViewerInstance.cpp`, `Engine/AppInstance.h/.cpp`, `Gui/Gui05.cpp`, `Gui/Gui20.cpp`, `Engine/EffectInstanceRenderDeep.cpp` | `:626` is a second consumer of `shouldCacheOutput`, unchanged here, and graph build passes edits through identities. The branch stamp is at the viewer's args setter rather than graph build, so that it also covers the legacy path and precedes admission; a render thread learns the active viewer from a pointer the GUI publishes. The hook is in `Engine/Node.cpp`, which is listed, rather than `Engine/Knob.cpp` |
| M300.P2.T4 | `Engine/EffectInstanceRenderRoI.cpp`, `Engine/EffectInstance.h/.cpp`, `Engine/RenderScheduler.cpp`, `Engine/Node.cpp`, `Engine/AppManager.h/.cpp`, `Engine/Settings.h` | The state record, the shared admission question, and the aggressive caching preference `all` replaced. `Engine/Cache.h` needed no change |
| M300.P3.T1 | `Engine/MemoryFile.h/.cpp`, `Engine/Image.cpp`, `Engine/DeepImage.h` | Reservation before mapping; bitmaps need persisting, since `onMemoryAllocated` starts restored entries unrendered (`:792`, changed in M300.P1.T3 rather than here); deep needs a writer and reader |
| M300.P3.T2 | `Engine/Plugin.cpp`, `Engine/AppManagerPrivate.cpp` | `runningExecutableStamp` is file-local (`:67`) and has to stamp both executables; the old restore path lives in `AppManagerPrivate.cpp` and goes |

**3. M300.P4.T1 cannot discriminate the policies on its current graph.** See [Admission](#admission). Proposed addition to its approach:
"Also run a graph with off-branch work (a second viewer on an expensive branch, under memory pressure), since the policies coincide on a
single chain." Eviction under pressure was not measured by M276 - Upstream Reuse On Edits either, and this run covers it.

**4. The verification gate should name the pruning.** Proposed addition: "…and no record at all for raw nodes behind `RawInputColor`'s hit",
so that passing by hits alone does not read as done.

## Departures for the user

D1 and D3 are not adopted; the design above follows the planned architecture. D2 is adopted, for the reason given.

**D1. Recompute cost that includes uncached upstream cost, in the demotion test and the score.** The milestone defines it as the node alone,
excluding inputs. The user's decision is "spill to disk only when cheaper than a recompute", and the real recompute of a dropped entry is the
chain above it, if that is also gone. Measured walls at fit, with the estimated 384 MB write:

| Node | Own ms | Throughput at which its own test demotes | With uncached upstream, ms | Throughput then |
| --- | --- | --- | --- | --- |
| `RawHighlights` | 108 | 3.6 GB/s | 223 | 1.7 GB/s |
| `RawDemosaic` | 383.6 | 1.0 GB/s | 607 | 633 MB/s |
| `RawInputColor` | 0.6 | never (640 GB/s) | 608 | 632 MB/s |

Between about 630 MB/s and 1 GB/s, the planned rule drops the whole chain to save a write that one 607 ms recompute would repay. Against it: the
effective cost changes as entries come and go, so it has to be computed or estimated at decision time, and it can double-count across a shared
upstream. In the RAM score the own-cost rule largely heals itself: `RawInputColor` goes first and re-runs in 0.6 ms off a surviving
`RawDemosaic`. So this matters mainly for demotion. **Recommendation:** keep own cost for now, have M300.P4.T1 count the case, and change
the demotion test only if drops are seen re-running chains.

**D2. `pread`/`pwrite` instead of mapping for automatic entries (adopted).** The milestone says mapped files, reusing `MemoryFile`. With a
mapping, a read error on a promoted entry is `SIGBUS`, not an error code, and with a disk tier shared between sessions that
error is no longer rare: another client's unlink on a network mount gives `ESTALE`, which is a `SIGBUS` on a mapping. A `pread`
into a `HostImageMemory` buffer turns it into a miss and a recompute, and keeps demotion from depending on mapping limits.
Promotion copies anyway, so nothing is lost. Pins stay mapped on local disks, where an unlink leaves the inode alive, and are
read into RAM on network mounts.

**D3. Sharding the cache lock.** Not measured. `_getLock` and `_lock` serialise lookups today, and the design moves I/O off them, not the
lookups. **Recommendation:** no change until the CacheStress concurrent run or a many-core profile shows contention.

## Benchmark results

Filled by M300.P4.T1: latency, upstream executions, peak RAM and disk writes under each policy, with `NATRON_GPU=off`, on the user's host. The
default is the user's choice.

## Device tier

Filled by M301.P1.T1 (member-output tracing).

## Coverage checklist

Scope, In:

- [x] Tier-neutral entry header and tier interface; host RAM and disk tiers: [Entry header](#entry-header), [Tier interface](#tier-interface)
- [x] Image and DeepImage in one cache with one policy; DeepImageCache retired: [Existing classes](#existing-classes), [Entry header](#entry-header)
- [x] Eviction score, per tier, only past the high-water mark; demote if recompute exceeds demotion cost, else drop: [Score and high-water marks](#score-and-high-water-marks), [Tier interface](#tier-interface)
- [x] Retention signals (edited inputs with decay, open panels, viewer branch, expensive nodes, reuse multiplier): [Priority signals](#priority-signals), [Score and high-water marks](#score-and-high-water-marks)
- [x] Two admission policies behind a setting; the benchmark decides, the user signs off: [Admission](#admission), [Benchmark results](#benchmark-results)
- [x] Persistent disk tier shared by concurrent sessions, location and size preference, 25% cap, a build directory per binary identity: [Persistence](#persistence), [Layout](#layout), [Builds](#builds)
- [x] DiskCache node as a pin: forced to disk, persistent, own budget, never auto-evicted, shared by peers through live markers: [Pins and holds](#pins-and-holds)
- [x] `ImageKey` completed with mip, layer, bit depth; finer-mip downscale stays as fallback: [Existing classes](#existing-classes)

Edge cases:

- [x] Disk full or write error on demotion: [Writing](#writing), [Edge cases](#edge-cases)
- [x] Second Natron instance, as a peer on one tier: [Sessions and crashes](#sessions-and-crashes), [What each session sees](#what-each-session-sees)
- [x] Changed source file across sessions: [Reading and restoring](#reading-and-restoring)
- [x] Entry in use: [Pins and holds](#pins-and-holds)
- [x] Partially rendered entry: [Entry header](#entry-header)

The brief's further items:

- [x] `not_admitted` and `behind_hit`, with planning change and owner: [How the gate is met](#how-the-gate-is-met), [Gaps](#gaps-against-the-task-list)
- [x] Device tier hooks: [Device tier hooks](#device-tier-hooks)
- [x] Assumptions checked against the code: [Assumptions checked](#assumptions-checked)
- [x] Constants with start values and measurements: [Tunable constants](#tunable-constants)
- [x] Hardware range: [Hardware range](#hardware-range)
