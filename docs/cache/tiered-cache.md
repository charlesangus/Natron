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

After M300.P2.T3 the reasons become a priority ([Priority signals](#priority-signals)), and after M300.P2.T4
`createInCache` is the admission policy's answer ([Admission](#admission)). Every raw node is on the active viewer's
branch, which is a signal, so both policies store all five on the first render.

What stays a refusal, independent of policy, because it is not a retention question: OpenGL renders and loop variables
(`Engine/EffectInstanceRenderRoI.cpp:929-930`), `allowsStoreUnder` (`:978`), and the overlay pass (`:963-972`). The
analysis override at `:933` stays too.

`Engine/RenderScheduler.cpp:625-632` is a second consumer of the same yes/no: it sets `sharesCachedOutput`, which gates
every graph-level probe. It must read the same answer or the pruning below sees nothing. M300.P2.T3 does not list it;
see [Gaps](#gaps-against-the-task-list).

### `behind_hit` goes away only if dependency planning changes, and no task owns that

The scheduler's dependency planning must change. `RenderScheduler::buildGraph` expands every
`fvRequest->dependencies` (`Engine/RenderScheduler.cpp:561-576`) with no cache probe. `submit` then starts each task
whose dependencies are done (`:661-709`), and `executeTask` runs it (`:1195`). The only graph-level probe is placement's
`inputs.isCached` (`Engine/GpuPlacement.cpp:1024-1067`), used solely to keep tasks off regions (`:371`); it prunes
nothing, and with `NATRON_GPU=off` placement does not run at all, so pruning cannot live there.

Inside `renderRoI`, a hit does skip its inputs: the pre-render loop (`Engine/EffectInstanceRenderRoI.cpp:1397`)
iterates only rectangles left to render. So by reading, the legacy recursion has no `behind_hit`; M276 - Upstream Reuse On Edits did not measure
that scheduler.

**No existing task in the milestone owns this.** M300.P2.T2, M300.P2.T3 and M300.P2.T4 change what is stored and kept;
none touches graph planning. M301.P2.T1 covers only device hits in placement. The attribution table also says "no task
yet" (`docs/perf/interactive-edit-attribution.md:32`). The proposed task is under
[Gaps](#gaps-against-the-task-list); this note designs the mechanism but does not absorb the work.

The planning pass, `RenderScheduler::pruneBehindCacheHits(graph, context)`, runs after `buildGraph` at both call sites,
before `placeRenderGraph` (`Engine/ViewerInstance.cpp:1331-1339`, `Engine/OutputSchedulerThread.cpp:2594-2598`):

1. Walk from the root in reverse topological order. A task is *needed* if it is the root, or if some consumer is needed
   and is not *covered*.
2. Probe each needed task once. A task is covered if `sharesCachedOutput` holds and an entry at its exact key, for all
   its layers, has no unrendered pixel in `renderedRoI`, in any tier. Pixels marked as being rendered elsewhere count as
   not covered.
3. A covered task keeps its node and loses its dependencies. It still runs, as a `hit`: the consumer reads its image
   from the frame store as usual. Partial hits and identity tasks are not covered and keep their dependencies.
4. Remove tasks that are not needed and recompute `consumers` and `remainingDeps` of the rest. A dependency shared with
   a branch that needs it stays.
5. Hold each covered entry's `ImagePtr` in the `FrameRenderContext` until its task has run. That is an in-use hold
   ([Pins](#pins-and-holds)), so eviction cannot take the entry between probe and read.

The walk is O(V+E) with one probe per needed task, so its cost follows the part of the graph that will run. It is
skipped when the frame bypasses the cache.

The probe must not promote, must not count as a reuse and must not bump recency. `Cache::get` does all of that today
(`Engine/Cache.h:1735-1809`; the table's `operator()` relocates to most recent). It also has to compute the key
exactly as `renderRoI` does, through one shared function. Placement's probe tolerates a wrong full-scale flag by
trying each variant ("a false hit only keeps a task on the CPU", `Engine/GpuPlacement.cpp:1021-1032`); a false hit here
costs more.

If a covered entry is removed anyway (retraction at `Engine/EffectInstanceRenderRoI.cpp:1716`, or a false probe), the
existing fallback still gives correct pixels: the consumer's `renderRoI` misses, its input read misses the frame store,
on the first path it notes an unplanned pull and renders the input by recursion (`Engine/EffectInstance.cpp:1155-1176`); the
second path (`:1625-1660`) renders inline without noting one, so a test counting pulls has to count that path too.

The benchmark needs no new semantics: it iterates only records present in the profile
(`tools/bench/edit_latency_bench.py:138-157`), so a pruned task counts as not executed. Its baseline
(`tools/bench/baselines/edit-latency.jsonl`) is re-recorded by M300.P4.T1.

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
| `holder` | interned `U32` | creation, hit | reference into `CacheRetention`, from `CacheEntryHolder::getCacheID()` |
| `bounds` | `RectI` | under `_entryLock` | as `Image::getBounds` (`Engine/Image.h:287`) |
| `bitmap` | `Bitmap`, absent for deep | under `_entryLock` | the trimap (`Engine/Image.h:67-169`); null in RAM while the entry is disk-resident |
| `pending` | `ImageBeingRenderedPtr` | under the entry's `pendingLock` | the entry's slot in the being-rendered registry; see [A1](#assumptions-checked) for why not the stripe |
| `residency` | atomic mask: host, disk, device | tier moves | which tiers hold a copy; a mask because a device and a host copy can coexist |
| `bytes` | atomic `size_t` | allocation, resize | payload plus bitmap, as `Image::size` (`Engine/Image.h:293-304`) |
| `state` | atomic enum: normal, demoting, promoting | tier moves | a move in flight, which a lookup can overtake |
| `pinned` | bool | at creation | DiskCache node output |
| `lastUse` | atomic wall-clock ns | real hits only | age term of the score; wall clock so it survives a restart |
| `reuseCount` | atomic `U32` | real hits only | reuse multiplier; counted once per frame through `lastReuseFrame` |
| `priority` | atomic `float` | last score | the value last computed, for diagnostics and the index; the score itself recomputes it |
| `recomputeCostNs` | atomic `float` | after each render | own render time scaled to the full bounds; see [Recompute cost](#recompute-cost) |

The score scan reads atomics, so it takes no per-entry lock.

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
| Host (`Engine/CacheHostTier.cpp`) | `min(maxRAMPercent × total RAM, used + available RAM − keep-free RAM)`; defaults 50% and 20% (`Engine/Settings.cpp:1799-1800`), the second term replacing `checkCacheFreeMemoryIsGoodEnough` (`Engine/AppManager.cpp:3029-3035`) | live entries plus idle `HostImageMemory` pool, as today (`Engine/HostImageMemory.h`, class comment) | queue for the disk writer if worth it, else drop | allocate from `HostImageMemory`, copy from the mapped file, reload the bitmap |
| Disk (`Engine/CacheDiskTier.cpp`) | the size preference, capped at 25% of free space at start | file bytes | drop: remove the file | not called: nothing lies below it |
| Device (M301 - GPU Cache Tier) | see [Device tier hooks](#device-tier-hooks) | | | |

Host demotion is off the cache lock. The current path syncs under `_lock` (`tryEvictInMemoryEntry`,
`Engine/Cache.h:1877-1897`, its comment says "EXPENSIVE"); writing 384 MB there would stall every lookup on the machine.
`demote` sets `state = demoting` and hands the entry to one writer thread, modelled on `DeleterThread`
(`Engine/Cache.h:81`). Its RAM stays charged until the write completes, and a lookup in the meantime reads RAM and
cancels the move. An allocation that finds the tier full waits on `_memoryFullCondition` as it does for the deleter
(`Engine/Cache.h:876`). When the queue holds more than `demotionQueueFraction` of the host budget, `demote` drops
instead, so a slow disk costs recomputes and not a stalled render.

Promotion happens when a render reads the entry, on that render's thread, never during a probe. Pinned DiskCache
entries are read through their mapping and not copied ([Pins](#pins-and-holds)).

## Score and high-water marks

The score, per tier, is computed only once `used() > highWater × budget()`:

```
score = priority × recomputeCost × reuseMultiplier ÷ bytes
reuseMultiplier = (1 + log2(1 + reuseCount)) × 2^(−age / reuseHalfLife)
```

No eviction happens below the mark. That is the user's decision ("just not evict stuff until we need to"), so there is
no low-water mark: as today (`Engine/Cache.h:846`), eviction stops as soon as the incoming entry fits under the mark.

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

Selection is a scan, cached. On crossing the mark, scan the tier's entries, keep the `evictionBatch` lowest with
`nth_element`, consume them, rescan when they run out. It is O(entries) per batch, on atomics, and each candidate is
rechecked for holds when taken. A heap would need a rebuild anyway, because scores move with time.

An entry that no longer fits under any budget is not rejected: it lives in RAM while held, as today, and is then the
first to go.

### Recompute cost

Recorded in `renderRoI` (`Engine/EffectInstanceRenderRoI.cpp`, M300.P2.T2), around the render action, for both
schedulers: own wall time, minus time in nested `renderRoI` calls (a thread-local accumulator), scaled by
`bounds area ÷ rendered area`. A hit records nothing. Each entry's cost is an exponentially weighted average when it
re-renders in part; a per-node remembered cost, kept by `CacheRetention`, answers for an entry not rendered yet.

Why not reuse the scheduler's task wall time: see [Assumptions](#assumptions-checked).

### Demotion cost

`bytes ÷ measured write throughput`, per M300.P3.T1. Throughput starts at `diskWriteBytesPerSecond` and is averaged from
the writer's own measurements through `fdatasync`, so a modest disk learns it is modest within a few writes. The
per-node test has a problem for chains; see [Departures](#departures-for-the-user).

## Priority signals

`CacheRetention` (`Engine/CacheRetention.h/.cpp`, M300.P2.T3) holds per-holder signal state:

```
priority = 1 + Σ wᵢ · sᵢ
```

| Signal | Set by | `sᵢ` | Weight | Half-life |
| --- | --- | --- | --- | --- |
| Input of a recently edited node | `Node::onKnobContentChanged` (`Engine/Node.cpp:2050`), reached from `KnobHolder::bumpKnobsContentGeneration` (`Engine/Knob.cpp:8556`); boosts the node's direct inputs, walking through identities | `2^(−t/h)` since the edit | 8 | 120 s |
| Input of an open panel | graph build, testing each output's `isSettingsPanelVisible` as `Engine/Node.cpp:8770` does | decays after the last graph where it held | 4 | 60 s |
| Active viewer's branch | graph build of the active viewer's frame: every task's node | decays after the last such graph | 2 | 10 min |
| Expensive node | remembered cost ≥ `expensiveCostMs` | 1 or 0 | 1 | none |
| Forced | `isForceCachingEnabled`, also through a group's output (`Engine/Node.cpp:8792-8797`) | 1 | 16 | none |
| Structural: output is read more than once or is costly to re-read | more than one output, temporal access, no tile support (`:8780`), not frame-varying (`:8766`), preview enabled (`:8807`), roto painting (`:8812-8818`), nested render | 1 | 4 | none |

The reuse count is not in this sum; per the milestone it multiplies recompute cost. It is per entry, the signals per
holder.

Priority is read when scoring, so a panel opened after a store protects the entry. A hit by another holder moves
`holder` to it, since entries are shared by content (`docs/cache-invalidation/image-caches.md`, "Holder").

Costs of the signals are bounded for deep and broad graphs: the edit boost touches direct inputs, not the upstream
tree. Panel and branch signals are one write per task while `buildGraph` already visits it. No new upstream walk.

`RotoDrawableItem` is also a holder (`Engine/RotoDrawableItem.cpp:365`). It has no signal state and scores at priority 1
unless its entries get others.

## Admission

Setting `cacheAdmissionPolicy` (`all` | `priorityFloor`), environment override `NATRON_CACHE_ADMISSION`. Asked at
`Engine/EffectInstanceRenderRoI.cpp:933` and `Engine/RenderScheduler.cpp:626`, through one function.

- **`all`:** admit every output that passes the non-policy refusals above.
- **`priorityFloor`:** admit when the output carries a priority signal or its remembered cost is at least
  `admissionCostMs`, as M300.P2.T4 words it. Otherwise it renders into a private image, read by consumers from the frame
  store and gone with its last consumer.

Lookup is not gated. `getImageFromCacheAndConvertIfNeeded` ignores its `useCache` argument
(`Engine/EffectInstance.cpp:2226`), so an entry stored earlier still serves a node now below the floor. The state record
at `Engine/EffectInstanceRenderRoI.cpp:1380` calls that `bypass` and has to test for the hit first.

Cost is known only after a render, and the decision comes before it. A node never rendered has no remembered cost, so
on a first render only signals can admit it.

**As worded, the two policies coincide on the benchmark graph.** The active viewer's branch is a signal, so
`priorityFloor` admits the whole branch, and every node of the benchmark's chain is on it. They differ only off the
active branch: other viewers, a background render, unviewed parts of a broad graph. A narrower reading, where the branch
signal does not admit, fails the gate: the first render knows no cost and has no edit, so it stores nothing and the
first edit runs the chain. M300.P4.T1 therefore needs a graph with off-branch work to tell the policies apart; see
[Gaps](#gaps-against-the-task-list).

`aggressiveCaching` (`Engine/Settings.cpp:1498`, read at `Engine/Node.cpp:8802`, `:8826`) is the same question as `all`.
Retire it with no migration.

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

**A pin** is a DiskCache node output (M300.P3.T3):

- Forced to disk: `Engine/EffectInstanceRenderRoI.cpp:841` picks `eStorageModeDisk` for that node. It is written directly
  to a mapped file, never passing through the host tier.
- Persistent: listed in the tier's index with `pinned` set, in the subdirectory `pinned/`.
- On its own budget: `maxDiskCacheNode`, default 10 GiB (`Engine/Settings.cpp:1802`). A separate `CacheTier` instance whose
  `demote` is never called by the automatic policy. It does not count against the 20 GiB automatic tier.
- Read through its mapping, counted as RAM while mapped and against the open-file cap, as today
  (`notifyEntryAllocated`, `Engine/Cache.h:1313`; the cap at `:823`).

What removes a pin: deleting the node (`Engine/AppManager.cpp:2543-2546`), the clear-cache actions, a new build, and the
rule below. Today the DiskCache instance evicts by LRU past its budget (`Engine/Cache.h:846`, `:893`); "never evicted"
means that stops, which leaves the question of a full pin budget. This note's answer:

1. When a pin would exceed the budget, first drop this holder's pins at a different node hash. They are invalid, not
   evicted, and `removeAllEntriesWithDifferentNodeHashForHolderPrivate` already does this.
2. If it still does not fit, the output is not pinned: the render returns its pixels through RAM, and one warning is
   logged per session.

The milestone does not state this, so the user should confirm it. It can change without touching
the rest of the design.

## Persistence

Applies to the automatic tier and the pins. The `Cache<FrameEntry>` playback cache keeps its own files and table of
contents (`Engine/AppManagerPrivate.cpp:408`, `:458`).

### Location and size

Preferences: location (default `~/.cache/Natron/tier`), size (default 20 GiB). The effective budget is capped at 25% of
free space at start. Free space here includes the tier's own files, or the cap would shrink a full tier a little more at
every launch; this is an interpretation of the milestone's wording.

### Layout

```
<tier>/lock
<tier>/index.bin
<tier>/<aa>/<hash>[-n].ntc      automatic entries
<tier>/pinned/<aa>/<hash>[-n].ntc
```

The two-character fan-out is today's (`generateStringFromHash`, `Engine/CacheEntry.h`). The suffix exists because several
entries can share a key and differ in params (`Engine/Cache.h:1549`).

An entry file holds the payload, then the bitmap run-length coded at its tail, or a flag for "all rendered". Deep payloads
are not flat: counts, offsets, then a shared sample vector (`Engine/DeepImage.h:108-109`, `:168`), so each payload type
provides its own write and read. They are written as those three sections in order.

### Writing

1. `posix_fallocate` the file to its full size, then map. `MemoryFile::resize` only calls `ftruncate`
   (`Engine/MemoryFile.cpp:304`), which makes a sparse file: on a full disk it succeeds and the copy into the mapping dies
   with `SIGBUS`. Without this step the disk-full edge case would be a crash. `ENOSPC` here is a clean failure.
2. Copy, flush with `MemoryFile::flush(eFlushTypeSync)`, `fdatasync`. Close: only mapped entries hold a descriptor.
3. Mark the entry disk-resident, free its RAM and its bitmap. Only now may a checkpoint list it.

Any failure removes the partial file, drops the entry and logs one warning per session, through a once-only flag.

### Index format

`index.bin` is a fixed header followed by a boost binary archive, the form `Engine/CacheSerialization.h` and
`Engine/AppManagerPrivate.cpp:378-405` already use.

| Part | Contents |
| --- | --- |
| Fixed header | magic, index format version, `NATRON_CACHE_VERSION` (`Global/GlobalDefines.h:97`), binary identity `U64`, entry count |
| Each entry | payload kind, hash, typed key, typed params, relative file name, bytes, pinned, holder ID, `recomputeCostNs`, `reuseCount`, `lastUse` |

M300.P1.T2's new key fields and the deep key and params need serializers. `Engine/CacheSerialization.h:83` iterates only
what `SerializedEntry` knows, which M300.P3.T2 extends.

It is replaced atomically (temporary file, `fsync`, `rename`, `fsync` of the directory): at clean exit, and from the writer
thread every `indexCheckpointSeconds` while changed. Today's table is written only at exit and removed after reading
(`restoreCache`, `Engine/AppManagerPrivate.cpp:418-455`), so a crash loses it, and with it every file. Here a crash loses
at most the entries since the last checkpoint.

### Restoring

At start, in the slot of `restoreCaches` (`Engine/AppManager.cpp:1135`), after taking the lock:

1. A short or unreadable index, a different format or cache version, or a different binary identity: wipe the tier,
   pins included, and start empty.
2. Otherwise stat each listed file. One that is missing or of the wrong size is forgotten.
3. Remove files the index does not list, as `Cache<T>::restore` does (`Engine/CacheSerialization.h`, the sweep at the end
   of `restore`).
4. Enter each entry as disk-resident, with no RAM and no bitmap. Over budget (a lowered preference, less free space):
   apply the score now.

Since M300.P1.T3 `Image::onMemoryAllocated` starts a restored entry unrendered, since nothing records which of its pixels
were rendered. A restored entry therefore re-renders on a hit until bitmaps persist in M300.P3.

A restored entry is found only when the session computes the same key. It does:

- Hashes are stable across sessions and AppImage remounts, by test (`docs/cache-invalidation/keys.md`, "Stability across
  sessions"; `Tests/HashInvariants_Test.cpp`).
- The holder is not in the hash or equality (`Engine/ImageKey.cpp`, `fillHash`, `operator==`). Project files keep a node's
  cache ID since M260 - Cache Invalidation Follow-ups, so a holder's later removal still finds its entries.
- A changed source file changes the key: file knobs stamp path, mtime, size and inode (`computeStamp`,
  `Engine/FileStampRegistry.cpp`), so the stale entry misses, and Reload re-reads the file. The stale entry ages out.
- A volatile plug-in's hash is salted per process (`Engine/Plugin.cpp`, `kVolatilePlugins`), so its entries never hit in a
  later session. They age out too. That is safe.

### Binary identity

The hash of `NATRON_VERSION_STRING`, `GIT_COMMIT` and `runningExecutableStamp` (`Engine/Plugin.cpp:67`, mtime and size of
`/proc/self/exe`).

The key alone would already make any new build miss, since native plug-in stamps hash the same values
(`Plugin::getBinaryContentStamp`, `Engine/Plugin.cpp:349`). The identity in the index exists so that the orphans free
their space at once, rather than occupying the 20 GiB tier and the pin budget, where they could never be evicted. It
also keeps the user's decision: the key keeps hashing the binary.

- `runningExecutableStamp` is file-local today and has to be exposed; see [Gaps](#gaps-against-the-task-list).
- When `statx` fails, the stamp falls back to a per-process salt (`Engine/Plugin.cpp:74`, hole K8). The identity then differs
  at each launch and the tier wipes. That is safe, and K8 stays with M260 - Cache Invalidation Follow-ups.
- An OFX plug-in rebuild changes only its own nodes' keys (`Plugin::ofxBinaryStamp`). Those entries age out without a wipe.

### Directory lock

`<tier>/lock`, opened `O_CLOEXEC`, held with `flock(LOCK_EX | LOCK_NB)` for the process's lifetime. The kernel releases it
on a crash, so there is no stale-lock cleanup. `O_CLOEXEC` keeps spawned processes from inheriting it.

When the lock is taken, or `flock` fails for any other reason such as on a network mount: use a session-only scratch
directory beside the tier, `<tier>.scratch-<pid>`, with its own lock. It behaves as a normal tier but restores nothing,
checkpoints nothing, and is removed at exit. At start, any `<tier>.scratch-*` whose lock can be taken is left from a crash
and is removed.

Only the lock holder may wipe, so a second instance on another build cannot delete the first's files.

A background `NatronRenderer` started from the GUI is a second instance. It gets a scratch directory, so it neither
reads nor writes the persisted tier, and that includes DiskCache pins.

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
| `LRUHashTable` (`Engine/LRUHashTable.h`) | Active variant `BoostLRUHashTable` (`:578`), since `NATRON_CACHE_USE_HASH` and `NATRON_CACHE_USE_BOOST` are defined (`:67-69`); the milestone's `:199` is an inactive one. Any lookup moves to most recent; `evict` (`:646`) takes the front with `use_count() == 1` (`:651`) | Used only by `Cache<FrameEntry>`. The tiered cache uses a plain hash table of entry lists, with recency in the header. M300.P2.T2 lists this file; nothing changes in it |
| `CacheEntryHolder` (`Engine/CacheEntryHolder.h:40-57`) | Interface with `getCacheID()`, a UUID on `Node` (`Engine/Node.cpp:912`) | Unchanged. Still owner identity for removal and stats, and now the key into `CacheRetention`, interned on the header |
| `HostImageMemory` (`Engine/HostImageMemory.h`) | Pooled importable RAM; pool counts against the limit and is trimmed first (`trimTo`, `Engine/Cache.h:839`, `:1170-1176`) | The host tier's allocator, unchanged. The tier trims the idle pool before scoring anything. Promotion allocates here, so a promoted entry stays importable |
| `DeepImageCacheEntry` (`Engine/DeepImageCacheEntry.h:58-59`) | `CacheEntryHelper<char, …>` with `DeepImagePtr` beside an unused generic buffer; size snapshot at `:86-90`; its own 2 GiB instance | A payload type with a header, without the buffer accounting (done in M300.P1.T4, its instance and setting retired). Its size is real bytes for the score. The old reason for its own budget (`Engine/AppManager.cpp:1112-1113`, a big deep render would evict all 2D) is answered by the score: large and cheap goes first, whatever the type |
| `MemoryFile` (`Engine/MemoryFile.h`) | One mapped file, not MT-safe (`:43`); destructor keeps the file (`:98-100`) | Backs entry files and pins, used by one thread at a time. Needs a reservation step before mapping (`Engine/MemoryFile.cpp:304`) |

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

**Broad graphs.** Many small entries: the scan is O(entries) per batch, a header is a few hundred bytes, and the bitmap of a
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
| `highWater` | 0.9, all tiers (today's `NATRON_CACHE_LIMIT_PERCENT`, `Engine/Cache.h:67`) | Allocation waits or free-RAM evictions in the peak-RAM run of M300.P4.T1 lower it; a run where a tier sits unused above it raises it |
| `reuseHalfLife` | 30 min | Eviction count in a scripted session that switches branches and returns: raise it if returning re-runs expensive nodes, lower it if orphans hold the budget |
| Edit half-life, weight | 120 s, 8 | Upstream executions on the second edit after a pause, in a variant of the benchmark with memory pressure |
| Panel half-life, weight | 60 s, 4 | As above, panel open, then closed |
| Branch half-life, weight | 10 min, 2 | Same run on a broad graph: raise if the viewed branch is evicted before off-branch work |
| Forced weight; structural weight | 16; 4 | CacheStress under pressure: a forced node's entry must outlast unforced ones of equal cost |
| Expensive signal weight, `expensiveCostMs` | 1, 16 ms (one 60 Hz frame) | Measured walls straddle this: `RawDemosaic` 383.6 ms at fit, 7.1 at 100%. Move it if the signal separates nothing |
| `admissionCostMs` | 16 ms | The off-branch graph in M300.P4.T1: lower if off-branch re-runs dominate latency; raise if off-branch writes dominate peak RAM |
| `evictionBatch` | 64 | Scan time from a profile of a cache with many small entries; a scan over 1 ms of lock hold lowers the scan rate by raising it |
| `diskWriteBytesPerSecond` | 500 MB/s, then learned | Start value only; matters for the first writes. Set it from the median over the user's and a SATA host's first-minute rates |
| `demotionQueueFraction` | 0.1 of host budget | Allocation stalls versus drops during a sustained run on a slow disk |
| `indexCheckpointSeconds` | 30 | Index size and write time in a full 20 GiB tier: raise if a checkpoint costs visible I/O |
| Default disk size and cap | 20 GiB, 25% | Fixed by the milestone; changed only by the user |
| Entry-file bitmap coding | run-length | Average bitmap bytes per entry from a real tier: if partials are rare, a flag alone would do |

No constant caps the size of a user's image: an entry above every budget still renders and is held while in use.

## Edge cases

| Case | Behaviour |
| --- | --- |
| Disk full or a write error on demotion | Dropped, with one warning per session ([Writing](#writing)). `posix_fallocate` makes full-disk a clean error rather than `SIGBUS` |
| A second Natron instance | [Directory lock](#directory-lock): scratch directory beside the tier, session-only |
| A changed source file across sessions | Miss through the file stamps of M260 - Cache Invalidation Follow-ups; Reload re-reads ([Restoring](#restoring)) |
| An entry in use | In-use hold; never demoted while a render holds it ([Pins](#pins-and-holds)) |
| A partially rendered entry | Bitmap goes with the file and returns on promotion ([Entry header](#entry-header)) |

## Gaps against the task list

**1. No task owns skipping dependencies behind a hit.** This is the one the gate depends on: without it, `behind_hit` stays for any
evicted upstream entry, and the attribution's priority-floor objection stands. Proposed wording, to go before M300.P2.T4:

> **M300.P2.T5 — Prune the task graph behind a cache hit.**
> files: `Engine/RenderScheduler.cpp`, `Engine/RenderScheduler.h`, `Engine/ViewerInstance.cpp`, `Engine/OutputSchedulerThread.cpp`,
> `Engine/GpuPlacement.cpp`, `Engine/FrameRenderContext.h`, `Engine/ImageKey.cpp`, `Tests/CacheStress_Test.cpp`.
> approach: Between `buildGraph` and `placeRenderGraph`, walk from the root; a task whose entry covers its `renderedRoI` in every layer
> keeps its node, loses its dependencies and runs as a hit, and a task no needed consumer reads is removed. The probe is shared with
> placement, computes `renderRoI`'s exact key, promotes nothing and counts no reuse. Covered entries are held by the frame until
> their task has run. A cache-bypassing frame skips the pass.
> verify: With `NATRON_GPU=off`, on a chain whose last upstream node is cached, a Grade edit runs the edited node and one hit, with no
> record for the nodes above it and no unplanned pull. Eviction of an upstream entry behind a hit causes no execution. A consumer
> rendering a part of its RoI keeps its dependencies. A dependency shared with an uncached branch still runs. The result matches a
> full render, for both a dropped hit and a partial one.
> size: L

M301.P2.T1 then extends this pass to device hits instead of walking on its own.

**2. Task file lists that omit files their work needs.**

| Task | Missing | Why |
| --- | --- | --- |
| M300.P1.T2 | `Engine/GpuPlacement.cpp` | It builds an `ImageKey` itself (`:1032`) and would stop matching |
| M300.P1.T2 | Key serialization | The new fields must persist with the key |
| M300.P1.T4 | `Engine/Cache.h`, `Engine/Settings.cpp`, `Engine/EffectInstanceRenderDeep.cpp` | `Cache<T>` holds one entry type and constructs it at `:919`, so one cache cannot hold both without splitting it; the 2 GiB setting lives in Settings; the deep renderer calls `removeFromDeepImageCache` (`:563`). The hash needs its domain tag here. Node-cache clears, memory stats and pressure eviction now reach deep entries, and a holder purge ignores pins for them as it does for images |
| M300.P2.T2 | Thread-local nested-time accumulator, `Engine/EffectInstance.cpp` | [A2](#assumptions-checked) |
| M300.P2.T3 | `Engine/RenderScheduler.cpp` | `:626` is a second consumer of `shouldCacheOutput`, and graph build refreshes the panel and branch signals. The hook is in `Engine/Node.cpp:2050`, which is listed, rather than `Engine/Knob.cpp` |
| M300.P2.T4 | `Engine/EffectInstanceRenderRoI.cpp`, retiring `aggressiveCaching` | The state record at `:1380`, and the setting now duplicating `all` |
| M300.P3.T1 | `Engine/MemoryFile.h/.cpp`, `Engine/Image.cpp`, `Engine/DeepImage.h` | Reservation before mapping; bitmaps need persisting, since `onMemoryAllocated` starts restored entries unrendered (`:792`, changed in M300.P1.T3 rather than here); deep needs a writer and reader |
| M300.P3.T2 | `Engine/Plugin.cpp`, `Engine/AppManagerPrivate.cpp` | `runningExecutableStamp` is file-local (`:67`); the restore path lives there |

**3. M300.P4.T1 cannot discriminate the policies on its current graph.** See [Admission](#admission). Proposed addition to its approach:
"Also run a graph with off-branch work (a second viewer on an expensive branch, under memory pressure), since the policies coincide on a
single chain." Eviction under pressure was not measured by M276 - Upstream Reuse On Edits either, and this run covers it.

**4. The verification gate should name the pruning.** Proposed addition: "…and no record at all for raw nodes behind `RawInputColor`'s hit",
so that passing by hits alone does not read as done.

## Departures for the user

None of these is adopted. The design above follows the planned architecture.

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

**D2. `pread`/`pwrite` instead of mapping for automatic entries.** The milestone says mapped files, reusing `MemoryFile`. Evidence: with a mapping, a
read error on a promoted entry is `SIGBUS`, not an error code. `posix_fallocate` covers the full-disk write side, not the read side. A
`pread` into a `HostImageMemory` buffer would turn it into a drop and a recompute, and would keep demotion from depending on mapping limits. Pins
are read in place and stay mapped either way. Against it: no failure has been observed, and the mapped path exists. **Recommendation:** stay mapped
unless a disk failure in the field says otherwise.

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
- [x] Persistent disk tier, location and size preference, 25% cap, new build invalidates: [Persistence](#persistence), [Binary identity](#binary-identity)
- [x] DiskCache node as a pin: forced to disk, persistent, own budget, never auto-evicted: [Pins and holds](#pins-and-holds)
- [x] `ImageKey` completed with mip, layer, bit depth; finer-mip downscale stays as fallback: [Existing classes](#existing-classes)

Edge cases:

- [x] Disk full or write error on demotion: [Writing](#writing), [Edge cases](#edge-cases)
- [x] Second Natron instance: [Directory lock](#directory-lock)
- [x] Changed source file across sessions: [Restoring](#restoring)
- [x] Entry in use: [Pins and holds](#pins-and-holds)
- [x] Partially rendered entry: [Entry header](#entry-header)

The brief's further items:

- [x] `not_admitted` and `behind_hit`, with planning change and owner: [How the gate is met](#how-the-gate-is-met), [Gaps](#gaps-against-the-task-list)
- [x] Device tier hooks: [Device tier hooks](#device-tier-hooks)
- [x] Assumptions checked against the code: [Assumptions checked](#assumptions-checked)
- [x] Constants with start values and measurements: [Tunable constants](#tunable-constants)
- [x] Hardware range: [Hardware range](#hardware-range)
