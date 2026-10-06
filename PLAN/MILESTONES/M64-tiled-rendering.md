# Milestone 64: Tiled / fused rendering for bandwidth-bound chains

> **Elaborated 2026-10-06** (planning consultant, on the M63 tip `18aab690f`). The user approved starting M64 now. It sits on a branch stacked on M63's tip and is UAT'd as one parcel with M67 on top. Architecture: tiles are tasks inside M63's frame graph. A **segment** is a connected set of tile-capable point-op nodes whose outputs are not cached. One **tile task** per row strip runs the segment's tail `renderRoI` over that strip and pulls the segment's interior nodes inside the task with per-strip RoIs. The segment's inputs come from the `FrameStore`. A **join task** stores the assembled tail image. Everything else stays a whole-image task. Two go/no-go gates (P1.T2, P4.T3) stop the milestone if HD chains don't reach at least 1.3x.

At HD, node-at-a-time rendering writes and re-reads a full frame for every node, plus the engine's own per-image passes: allocation and zero fill, the NaN check, and unprocessed-channel copies. M64 splits frame-varying, uncached chains of tile-capable point ops into strip tasks so that each strip goes through every node of the segment while it is still in cache. The parallelism moves from per-node fork-joins (the multithread suite, host frame threading, OpenMP) to independent strips with one join per segment. That should also help many-core machines, where a fork-join across 64 threads per node is expensive whatever the bandwidth. Peak memory on long chains drops because intermediate images are strip-sized. Cache semantics are unchanged by construction. Only nodes whose output the cache would not keep can join a segment. A partial image never reaches the cache. The `FrameStore` only ever receives the whole assembled tail image.

The correctness bar is bit-exact equivalence against whole-image task-graph mode, which is already bit-exact against Legacy. M63's both-modes harness gets a third mode pairing, and the whole `SchedulerEquivalence` suite runs tiled. Non-tile-capable plugins, spatial (halo) nodes, GL-rendering nodes, deep/scene data kinds, cached nodes, render clones and non-thread-safe plugins all stay whole-image tasks and produce identical pixels.

## Scout notes (2026-10-06, at `18aab690f` on `milestone/m63-task-graph-render-scheduler`)

All anchors are at `18aab690f`. The shared checkout at `/home/bosley/git/Natron` was switched to `milestone/m65-rgba-rgb-alpha-layers` by another session during this scout, so read through `git show 18aab690f:<path>` or check out the M64 branch before trusting line numbers.

- **Numbers to beat** (`tools/bench/BASELINE.md` "After thread budget", `2db83845a`, task graph, N100, cooled): HD chain 30 is 1.0125 s per frame and chain 100 is 3.0829 s. That is a marginal **~29.6 ms per Grade node** at HD, with parallelism 3.1–3.2 of 4 (≈95 ms CPU per node). Range per-frame times are 0.864 s and 4.581 s. RSS before render is 130 and 173 MB. `max concurrent tasks` is 1, so a chain is one task at a time today. Realistic `readchain 30` is 1.126 s, CPU-bound. `rambound`/`footagecomp` peak RSS is 2.4 and 4.8 GB.
- **`stream_bench` (run read-only on the host for this scout, load 0.22, 4 threads, HD RGBA float):** a node-at-a-time pass costs **5.1 ms/node (12.9 GB/s)**. Tiled 128² costs 1.6 ms/node, 64² 1.4, 256² 2.4. Fused costs 1.15 ms/node at n=30, where its inner loop is ALU-bound. The stub's "~9 ms/node at ~7 GB/s" floor is pessimistic for this host today. **Pre-estimate:** the pure bandwidth share of a Grade node is 5 of ~30 ms. Tiling only pays off if the engine's extra per-image passes (alloc/fill 7.8%, `checkForNaNsAndFix` 2.3%, `copyUnProcessedChannels`, mask-mix copies) and the per-node fork-join overhead also become in-cache work. The range runs from ~1.15x (bandwidth only) to ~1.6x (every pass in cache and fork-joins gone). That uncertainty is why P1.T2 is a measuring spike with a kill gate, not an assumption.
- **Frame graph:** `FrameGraph::Task` is at `Engine/RenderScheduler.h:~50-90`. It holds `key`, `roi`, `renderedRoI`, `dependencies`/`consumerTasks`, `consumers`, `dfsPostOrder`, `isIdentity`, `startsBranch`, `estimatedBytes`, `sharesCachedOutput`, `nodeHash`, `cacheMipmapLevel` and `remainingDeps`. `buildGraph` is at `Engine/RenderScheduler.cpp:237-367`. It expands edges from `FrameViewRequest::dependencies` (`:276-299`) and computes per-task fields at `:301-353`. `rendersFullScale` is at `:330`. The whole-RoD rule for non-tile effects is at `:339`. `sharesCachedOutput` comes from `effect->shouldCacheOutput(...)` at `:344-351`, and `startsBranch` is computed at `:354-362`.
- **Task execution:** `executeTask` is at `RenderScheduler.cpp:686-827`. It builds `RenderRoIArgs` from the task (`:697-710`) and installs `AppTLS::FrameContextScope(context, budget, priority)` plus `OpenMPThreadsScope(budget)` (`:711-712`). It calls `effect->renderRoI` (`:719`) and rejects GL planes (`:727-736`). Inputs are released before `store.put(task.key, planes, task.renderedRoI, task.consumers)` (`:756-766`). Completion accounting is at `:773-813` (`_reservedBytes -= estimatedBytes`, `remainingDeps`, `pushReadyLocked`), and the failure abort is at `:816-825`. Admission and the bytes budget are `admitGatedLocked` (`:903`), `bytesInFlightLocked` (`:937`) and `pushReadyLocked` (`:855`, gated vs free heaps). `computeTaskBudget` is at `:504`.
- **Store:** `FrameStore` is at `Engine/FrameRenderContext.h:~45-120`. `put(key, layers, renderedRoI, consumers)` and `find(key, needed, pixelRoI, out)` require that both the bounds and the `renderedRoI` contain the request. That is the guard that already rejects a partial image for a larger request. `bytesInFlight` counts each image once and counts cache-backed images as 0. `FrameRenderContext` holds `getArgsForHolder` and `isRotoPaintTreeNode`.
- **Hand-off and pulls:**
  - `EffectInstance::lookupFrameStore` (`Engine/EffectInstance.cpp:865-896`) clips the needed RoI to the input's RoD from the request.
  - `renderInputOrTakeFromStore` (`:915-940`) counts `noteUnplannedPull` unless the input is a RotoPaint internal-tree node (`isRotoPaintTreePull` `:943`). A tile's interior pulls need the same exemption.
  - `getImage` uses the store at `:1381`.
- **The RoI problem tiles must solve:** inside a render, inputs are pulled with the request pass's **merged** `finalRoi`, not the RoI of the current call. `treeRecurseFunctor` sets `roiIsInRequestPass = true` whenever the input has frame args with a request (`Engine/ParallelRenderArgs.cpp:~228-236`), then reads `getFrameViewCanonicalRoI` (`:~300-306`). `getImage` does the same at `EffectInstance.cpp:1110-1120` (`roiWasInRequestPass`). `renderInputImagesForRoI` (`EffectInstance.cpp:2438-2475`) skips `getRegionsOfInterest_public` entirely when a request exists. Without a change, a strip render of a chain tail would pull every interior node over the whole frame.
- **Request pass:** `visitRequestPassItem` is at `ParallelRenderArgs.cpp:536-782`. The `finalRoi` merge is at `:654-663`, and identity edges (including the `-2` self-at-other-time case) are at `:665-690`. Input RoIs come from `getRegionsOfInterest_public(..., canonicalRenderWindow, ..., &fvPerRequestData.inputsRoi)` at **`:697`**, followed by the transform reroute at `:700-706`. This is where a "point op" flag (every input RoI equals the output RoI) can be recorded for free. `FrameViewRequest` is at `ParallelRenderArgs.h:~195-233` (`TaskEdge` `:205`, `dependencies` `:223`, `dfsPostOrder` `:232`).
- **renderRoI tile paths:** the RoI is clipped to the RoD only when `tilesSupported` (`Engine/EffectInstanceRenderRoI.cpp:644-665`). The image is allocated at RoI size unless `NATRON_ALWAYS_ALLOCATE_FULL_IMAGE_BOUNDS` (`:677-686`, macro commented out at `:75`). The cache decision is at `:782-793` (`shouldCacheOutput(isFrameVaryingOrAnimated, ...)`). Host frame threading is skipped when `!tilesSupported` or `budget <= 1` (`:1164`). Render-clone and instance locks are at `:1410-1434`: `eRenderSafetyInstanceSafe` takes `getRenderInstancesSharedMutex` and `eRenderSafetyUnsafe` takes the plugin lock, so concurrent strips of such a node would serialise. Input pre-render is at `:1207-1236`.
- **Per-image engine passes in `renderRoIInternal`/`tiledRenderingFunctor` (`EffectInstance.cpp`):** `fillZero` at `:2862/:2880/:2899`, the `tmpImage` allocation at `:2973`, `checkForNaNsAndFix` at `:3117`, and `copyUnProcessedChannels` at `:~3200`.
- **Tiles flag:** `Node::refreshDynamicProperties` sets `setCurrentSupportTiles(multiRes && tiles)` (`Engine/Node.cpp:396-401`). It reaches `ParallelRenderArgs::tilesSupported` at `EffectInstance.cpp:370` (`ParallelRenderArgs.h:151`). The OFX side is `OfxEffectInstance::supportsTiles` at `OfxEffectInstance.cpp:3360-3375`, and the host advertises `kOfxImageEffectPropSupportsTiles=1` at `OfxHost.cpp:263`, `:693`. The base default is `false` (`EffectInstance.h:1684`). Native nodes in `Engine/Nodes/Channel` and `Engine/Nodes/Deep` override it. `getOutputDataKind()` is at `EffectInstance.h:544`, and `DataKindEnum {Image, Deep, Scene, Polymorphic}` is at `Global/Enums.h:514`.
- **Cache rule:** `Node::shouldCacheOutput` (`Engine/Node.cpp:7185-7303`) caches when a node has more than one output, is a direct viewer input, feeds a Roto, **is not frame-varying**, has its output's panel open, does temporal access, **does not support tiles**, has force caching, aggressive caching or a preview in the GUI, or is a roto being edited. Consequence: in a static (un-animated) graph every node is cached, so nothing tiles. That is accepted for v1 (see `## Decisions`).
- **Eligibility of frames:** `Engine/SchedulerEligibility.h` (`SchedulerEligibility`, `isFrameEligibleForScheduler`) excludes partial updates, deep upstream, paint strokes, forced refreshes and analysis from the task graph entirely. Tiling can only apply inside eligible frames.
- **Thread-local plumbing:** `AppTLS::currentFrameContext()` (`Engine/TLSHolder.h:129`, `.cpp:112`), `currentThreadBudget()`, `FrameContextScope` (`TLSHolder.cpp:250`), `SpawnedThreadScope` (gives spawned threads budget 1 and OpenMP 1), and `parallelForOnGlobalPool` (`Engine/PoolParallelFor.h:97`). Pool threads have a 64 MB stack (M63 P4.T8), which bounds how deep an in-task pull may recurse.
- **Settings and mode pattern:** `_renderSchedulerMode` (`Engine/Settings.cpp:388-405`) and `AppManager::getRenderSchedulerMode()` (`AppManager.h:429`), with the `NATRON_RENDER_SCHEDULER` env override honoured first and the loaded value pushed after `restoreSettings` (M63 `af9bbfc25`). `RenderStats` has `incTasksRun` and `getMaxConcurrentTasks` (`Engine/RenderStats.h:140-148`), printed by `--render-stats`.
- **Tests:**
  - `Tests/RenderBothWays.h:46-89`: `renderBothWays(writer, first, last, poolSizes, beforeTaskGraph, tolerance)` compares Legacy against Task graph through EXRs, and `renderBothWaysDirect` compares through `renderRoI` and the scheduler.
  - `Tests/SchedulerEquivalence_Test.cpp` has 16 cases (`:521-889`) built on `renderAndCheck`/`checkObserved`, which assert legacy fallbacks and unplanned pulls.
  - Other suites: `FrameGraphBuild_Test`, `RenderScheduler_Test`, `RenderSchedulerAbort_Test`, `PoolSharing_Test`, `PoolParallelFor_Test`, `SchedulerWriters_Test`, `ViewerScheduler_Test` and `GLScheduler_Test` (strict under `build/m63-gui/run-gl-tests.sh`).
  - Helpers: `Tests/CountingTestEffect.h` (delay, cacheOutput, throwInRender, concurrency high-water) and `Tests/CacheMemoryPressureGuard.h`.
  - A release Tests binary exists at `build/release/Tests/Tests`, so timing spikes can run optimised.
- **Bench:**
  - `tools/bench/run_matrix.sh <tag> <res> <frames> <range> topo:n,...` with `BENCH_SETTINGS` (`;`-separated `name=value` passed as `--setting`), `BENCH_RENDER_STATS=1`, cooldowns and frequency logs through `lib.sh`.
  - `compare.py --pair-settings` computes mode ratios.
  - `graph_bench.py` builds chains as an animated CheckerBoard followed by N OFX Grades (`build_chain` `:297`), and `mixed` adds CImgBlur, Transform and ColorCorrect.
  - `classify.py` reports boundedness.
  - GUI: `build/m63-gui/run-gui.sh`, `viewer_check.py`, `gl_check.py` and `run-gl-gui.sh`.

## Design (binding for the briefs below)

1. **Segment eligibility.** A frame-graph task joins a segment only if all of the following hold:
   - The node is tile-capable (`frameArgs->tilesSupported`) and a flat image (`getOutputDataKind()==eDataKindImage`, `!producesDeepData()`).
   - It is a point op for this frame: every input RoI the request pass computed equals its output window (new flag, P2.T1).
   - It is not cached (`!task.sharesCachedOutput`, and `shouldCacheOutput` false).
   - Its render safety is `FullySafe` or `FullySafeFrame`.
   - It is not GL: `currentOpenglSupport` would not choose GL storage, or GL rendering is off.
   - It renders at its key's mipmap level (no `rendersFullScale`).
   - Every edge inside the segment has the same time, view and mipmap.
   - It is not the graph root.
   - It is not an identity at another time (`-2`) and not a RotoPaint node or internal tree node.

   An identity node at the same time and view may sit inside a segment. A node with more than one consumer task can only be a segment tail. Segments are capped at `kMaxSegmentNodes` (64) so that the in-task pull's recursion depth and the strip working set stay bounded on deep graphs; longer chains split into consecutive segments. A single-node segment is not formed, because it would only add a copy.
2. **Tile shape.** Tiles are full-width row strips of the tail's `renderedRoI`, which keeps memory contiguous and the assembly a row copy. The strip height is derived from the cache topology (`sysconf(_SC_LEVEL2_CACHE_SIZE)`, `_SC_LEVEL3_CACHE_SIZE`, hardware threads): the live strip images of one tile (the segment's widest input fan-in + 2) should fit in `min(L2, L3 / threads)`. It is then clamped to `[kMinStripRows, ...]` so the per-call overhead stays bounded, and lowered if needed so a frame yields at least `2·P` strips. `NATRON_TILE_ROWS` overrides it for benches. This must work on a 4-core 6 MB-L3 box and on a 64-thread box with large L3 alike.
3. **Tile task = pull inside a task.** A tile task runs the tail's `renderRoI` over its strip with a `TileScope` installed (segment membership plus strip rect). Inside that scope:
   - A segment member pulls its inputs with the RoI of the current call instead of the request pass's merged `finalRoi`. Because members are point ops, that input RoI is the call's own window; no per-tile `getRegionsOfInterest` action is needed.
   - Segment-boundary inputs are found in the `FrameStore` (containment check).
   - Interior pulls are not counted as unplanned pulls.

   This keeps `renderRoI` as the unit of work, as M63 does for deep subtrees and Roto trees.
4. **Join.** The first tile task to start allocates the segment's assembled output image (RAM, bounds = the tail's `renderedRoI`) once. Every tile copies its tail strip into it (disjoint rows, no lock). A join task depending on all tiles `put`s it into the store under the tail's `TaskKey` with the full `renderedRoI`. Partial content therefore never satisfies a `find`, and no tile image is ever handed to the cache.
5. **Threads.** Tile tasks are ordinary scheduler tasks, so `computeTaskBudget` gives each one 1 thread while at least P are ready. Host frame threading and the suite's fan-out then switch off (`EffectInstanceRenderRoI.cpp:1164`), and parallelism comes from strips. The tail end of a segment widens the budget automatically.
6. **Mode.** A new `tiledRendering` setting {Off, On} plus `NATRON_TILED_RENDERING=0|1` (env first), following the `renderSchedulerMode` pattern. It only acts when the scheduler mode is Task graph. The default is Off until P4.T3 passes, and P6.T2 flips it. There is no project-format change and no legacy fixtures (clean-break rule).
7. **Out of scope for v1, recorded as follow-ups:** spatial nodes with halos (Blur, Transform filtering) as segment members, which would need overlap recompute or halo exchange; tiling cached nodes into cache entries through the trimap; static graphs, where every node is cached by the existing rule; and fusing kernels across nodes, which needs native nodes (M67).

## Phase 64.1: Re-baseline and the go/no-go spike

- [ ] M64.P1.T1 — Re-baseline on the current tip and record the bandwidth floor
  - files: `tools/bench/BASELINE.md`, `tools/bench/README.md` (only if a command changes)
  - approach: Build release at the M64 branch point (M63 tip, with `main` merged in now that parcel 1 is merged). With the container held exclusively and `/proc/loadavg` recorded before each config (`lib.sh` cooldowns, frequency log), run the following in task-graph mode (the shipped default):
    - `BENCH_RENDER_STATS=1 tools/bench/run_matrix.sh m64start-hd hd 3 8 chain:30,100 wide:100 comp:100 mixed:100`
    - `run_matrix.sh m64start-tiny tiny 5 0 chain:1000 wide:1000 comp:300`
    - `run_matrix.sh m64start-real hd 3 8 readchain:30 footagecomp:32`, with the plates from `make_plates.py` if `build/bench/fixtures/` is gone

    Then repeat chain 30/100 HD with `BENCH_SETTINGS=renderSchedulerMode=0` for continuity. Compile `tools/bench/stream_bench.c` in the container (`gcc -O2 -fopenmp`) and record `stream_bench 1920 1080 {30,100} {64,128,256}` and `3840 2160 30 128`. Write an "M64 start" section with the tables, `rss_peak`, clocks, and the per-node marginal cost at HD ((chain100 − chain30)/70). It replaces the stale "Input for M64" paragraph's premise.
  - verify: `compare.py` of the task-graph rows against `results-m63final-*` flags nothing beyond 1.15x; `stream_bench` rows are present; every config exited 0 with load < 0.5 at start.
  - size: M

- [ ] M64.P1.T2 — Strip-pull spike: measure what tiling can buy before building it (KILL GATE 1)
  - files: `Tests/TileSpike_Test.cpp` (new, env-gated), `Tests/CMakeLists.txt`, `tools/bench/BASELINE.md` (results section), this file's `## Decisions`
  - approach: This needs no engine change. In a gtest that runs only when `NATRON_TILE_SPIKE=1` (otherwise it returns early and prints a skip line, as `GLScheduler_Test` does; the repo's gtest has no `GTEST_SKIP`), build an HD project (1920×1080, RGBA float) of an animated CheckerBoard followed by N OFX Grades (`graph_bench.py` parameters), for N = 30 and 100. Measure the median of 5 warm runs of each:
    - (a) **Whole:** one frame of the chain tail through the M63 scheduler (`RenderScheduler::buildGraph` + `submit`, as in `renderBothWaysDirect`).
    - (b) **Strip pull:** split the RoD into S full-width strips. On `parallelForOnGlobalPool`, each strip runs on its own thread with its own `ParallelRenderArgsSetter` for the tail, `computeRequestPass` over **that strip's** canonical rect (so every interior `finalRoi` is the strip), and the tail's `renderRoI(strip, byPassCache)`, writing into a shared frame image by row copy. Use strip heights of 8, 16, 32, 64, 128 and 270 rows.

    Also record the per-call fixed cost: a single Grade's `renderRoI` over 1×W vs 64×W strips, single thread, so that cost ≈ a + b·area can be fitted. Compare (a) and (b) bit for bit (`memcmp` over the RoD). Run on `build/release/Tests/Tests` in the container, with a cooled, quiet host. The per-strip request pass makes (b) a lower bound on what P2/P3 can reach.
  - verify: The results table goes into `BASELINE.md` ("M64 spike"): whole vs best strip time for N=30/100, the best strip height, a and b, and the pixel comparison. **Gate:** if the best strip-pull speed-up is below **1.3x on both** chain 30 and chain 100 HD, stop the milestone. Report to the user with the cost model, without starting Phase 64.2, and record the outcome in `## Decisions`. Pixels must be identical; a mismatch is reported as a finding (a plugin is not RoI-invariant) before going on.
  - size: L

## Phase 64.2: Tile plan in the frame graph

- [ ] M64.P2.T1 — Request pass records which frame/views are point ops
  - files: `Engine/ParallelRenderArgs.h`, `Engine/ParallelRenderArgs.cpp`, `Tests/FrameGraphBuild_Test.cpp`
  - approach: Add `bool inputRoIsMatchOutput = true;` to `FrameViewRequest` (`ParallelRenderArgs.h:~195-233`). In `visitRequestPassItem`, after `getRegionsOfInterest_public` and the transform reroute (`ParallelRenderArgs.cpp:697-706`), set it to false when any entry of `fvPerRequestData.inputsRoi` differs from `canonicalRenderWindow`, or when a reroute or transform applies. AND it over every visit of the frame/view. Identity frame/views keep `true` (they forward the same window). Do not add any action calls.
  - verify: In `FrameGraphBuild_Test`, Grade, a masked Grade and Merge get `true`; Transform with a translate, CImgBlur and a crop that shrinks its input window get `false`; a node visited by two consumers with different windows stays `true` only if every visit matched. Full ctest in both scheduler modes is unchanged.
  - size: M

- [ ] M64.P2.T2 — `tiledRendering` setting, env override, stats counters and the strip-height policy
  - files: `Engine/Settings.h`, `Engine/Settings.cpp`, `Engine/AppManager.h`, `Engine/AppManager.cpp`, `Engine/TileGeometry.h` (new, header-only), `Engine/RenderStats.h`, `Engine/RenderStats.cpp`
  - approach:
    - Add a `KnobChoice tiledRendering` {"Off", "On"} on the threading page under `renderSchedulerMode` (mirror `Settings.cpp:388-405`), default Off. Add `AppManager::isTiledRenderingEnabled()`, which honours `NATRON_TILED_RENDERING=0|1` first, is pushed after `restoreSettings` like the scheduler mode (`af9bbfc25`), and returns false unless the scheduler mode is Task graph.
    - `TileGeometry.h` provides `int stripRows(int width, int bytesPerPixel, int liveImages, int height, int poolThreads)` per Design §2, with the cache sizes read once (`sysconf`, falling back to 1 MB L2 / 8 MB L3 when 0), the `NATRON_TILE_ROWS` override, `kMinStripRows = 8` and `kMaxSegmentNodes = 64`.
    - `RenderStats` gets counters for segments formed, tile tasks run, tiled nodes, and a per-reason tally of why a node did not join a segment (enum, printed by `--render-stats`).
  - verify: A gtest in `Tests/RenderSchedulerMode_Test.cpp` sets the knob and the env and reads both back, including the forced Off under Legacy. A `stripRows` unit case covers HD/UHD at P=4 and P=64, the env override, and the ≥2·P strips rule; it goes in the same file to avoid a CMake edit. `NatronRenderer --setting tiledRendering=1` is accepted.
  - size: M

- [ ] M64.P2.T3 — Partition the frame graph into segments with tile and join tasks
  - files: `Engine/RenderScheduler.h`, `Engine/RenderScheduler.cpp` (`buildGraph` and the `FrameGraph` structs only), `Tests/TilePlan_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: When `isTiledRenderingEnabled()`, after `buildGraph`'s per-task pass (`RenderScheduler.cpp:301-353`), mark each task eligible or not per Design §1 and record the first failing reason in the stats. Grow segments from each eligible tail downwards (DFS over dependencies) under the cap. Then rewrite the graph:
    - Each segment's member tasks are replaced by S **tile** tasks (`kind = Tile`, `segment` index, strip rect inside the tail's `renderedRoI`, dependencies = the segment's boundary inputs) and one **join** task (`kind = Join`, keyed by the tail's `TaskKey`, depending on all S tiles, with the tail's consumers).
    - Store consumer counts must match edges: each tile releases each boundary input once, so a boundary input's `consumers` grows by S−1 per segment.
    - `estimatedBytes`: a tile reserves its live strip images; the join reserves the assembled image.
    - `startsBranch` and `dfsPostOrder` are recomputed so a generator-headed segment's tiles are gated like leaves and tiles of one segment are ordered together.
    - A `Segment` record (members, tail, strip count, assembled bounds/components/bit depth) is kept in `FrameGraph`.
    - With tiling off, the graph must be identical to today's.
  - verify: `TilePlan_Test` asserts, at P=4 and P=16:
    - chain 30 HD → 1 segment covering the 30 Grades (CheckerBoard included when eligible), S = `stripRows`-derived count;
    - chain 150 → 3 segments;
    - a Grade with two consumers ends a segment;
    - a CImgBlur, a Transform, a cached node (`CountingTestEffect cacheOutput`), a node at mipmap 1 without render scale, a TimeOffset edge and a deep branch each break segments with the expected reason;
    - Merge of two Grade chains forms one tree segment;
    - consumer counts equal edge counts;
    - tiling off gives a byte-identical graph dump.
  - size: L

## Phase 64.3: Tile execution

- [ ] M64.P3.T1 — Tile scope: per-call RoIs for segment members, store hits at the boundary, no unplanned-pull counting
  - files: `Engine/TLSHolder.h`, `Engine/TLSHolder.cpp`, `Engine/ParallelRenderArgs.cpp` (`treeRecurseFunctor` only), `Engine/EffectInstance.cpp` (`getImage` RoI choice, `renderInputImagesForRoI`, `renderInputOrTakeFromStore`), `Engine/EffectInstance.h`
  - approach:
    - Add `AppTLS::TileScope(const TileContext*)` and `AppTLS::currentTile()`. `TileContext` holds the set of segment member nodes (`const Node*`), the strip pixel rect, and the mipmap level.
    - While a tile scope is active, when the consumer is a segment member: `treeRecurseFunctor` (`ParallelRenderArgs.cpp:~228-306`) must use the input RoI equal to the current call's `canonicalRenderWindow` (point op, P2.T1) instead of `getFrameViewCanonicalRoI`. Convert the pixel strip directly rather than round-tripping through canonical, so anamorphic PAR does not grow it by a pixel. `getImage` (`EffectInstance.cpp:1110-1120`) must do the same for the input RoI.
    - `renderInputOrTakeFromStore` must not call `noteUnplannedPull` for an input that is a member of the current tile's segment. It counts tile-interior pulls in the new stats counter instead.
    - Nothing changes outside a tile scope.
  - verify: A gtest with `CountingTestEffect` (add a per-call rendered-area accumulator) renders, by direct `renderRoI` under a hand-built `TileScope`, a strip of a 5-node counting chain. Each member renders exactly strip-area pixels (no whole-frame pull), the boundary input is taken from a seeded `FrameStore`, and unplanned pulls stay 0. Without the scope the same call renders the merged RoI as today. A PAR 2 format case is included. Full ctest in both scheduler modes.
  - size: L

- [ ] M64.P3.T2 — Scheduler runs tile and join tasks: assembly, accounting, abort and failure
  - files: `Engine/RenderScheduler.cpp`, `Engine/RenderScheduler.h`, `Engine/FrameRenderContext.h`, `Engine/FrameRenderContext.cpp`, `Tests/RenderScheduler_Test.cpp`
  - approach:
    - In `executeTask` (`RenderScheduler.cpp:686-827`), a Tile task installs the `TileScope` around the tail's `renderRoI(strip)`. The segment's assembled image is allocated under a `std::once_flag` by the first tile: RAM, bounds = the tail's `renderedRoI`, the tail's components and bit depth, uncached. The tile copies its strip rows from the returned planes into it, then releases the boundary inputs once each.
    - A Join task renders nothing. It `put`s the assembled planes under the tail key with the full `renderedRoI` and the tail's consumer count, or hands them to `rootPlanes` if the tail feeds nothing, and then follows the normal completion path.
    - Reservations are released per task as today.
    - Abort purges queued tiles and the join.
    - A failed tile fails the frame and aborts its siblings (existing `abortLocked` path), and the assembled image is dropped.
    - The GL-plane rejection and the context unbinding apply to tiles unchanged.
    - Count `tilesRun` and `segments` in `RenderStats`.
  - verify: `RenderScheduler_Test` adds the following, each also with `--gtest_repeat=3` at pool sizes 1, 2, 4, 8 and 16:
    - a tiled counting chain matches the untiled one bit for bit;
    - `getReservedBytesForTests()==0` and store `bytesInFlight()==0` after success, abort mid-segment and a throwing tile (`CountingTestEffect throwInRender` on one member);
    - `FailedTileStopsSiblings`;
    - a segment whose tail is consumed twice;
    - peak bytes for chain 30 HD tiled < whole-image peak.

    Full ctest in both scheduler modes, plus once with `NATRON_TILED_RENDERING=1`.
  - size: L

## Phase 64.4: Correctness, cache semantics, performance

- [ ] M64.P4.T1 — Equivalence: the whole suite tiled, plus tile-specific cases
  - files: `Tests/RenderBothWays.h`, `Tests/RenderBothWays.cpp`, `Tests/SchedulerEquivalence_Test.cpp`, `Tests/RenderBothWaysSelf_Test.cpp`
  - approach:
    - Generalise the harness to a reference mode and a candidate mode (`RenderModePair` {LegacyVsTaskGraph, TaskGraphVsTiled, LegacyVsTiled}; the default keeps today's behaviour). Mode guards restore both settings. The negative self-test proves a tiled pass that ran no tile task is reported as a mismatch.
    - Run every existing `SchedulerEquivalence` case in TaskGraphVsTiled at pool sizes 1, 4 and 16, and assert 0 legacy fallbacks and 0 unplanned pulls as today.
    - New cases, each asserting `tilesRun > 0` or `== 0` as expected:
      - chain 30 on a 1920×1081 format (remainder strip);
      - `NATRON_TILE_ROWS=1` and `=1081`;
      - mipmap 1 and 2 (direct variant);
      - a tree segment (Merge of two Grade chains with a Grade mask);
      - a segment fed by a cached two-consumer node;
      - CImgBlur and Transform in the middle (segment boundaries);
      - a disabled node inside a segment;
      - a RoD smaller than the format (Crop as head);
      - PAR 2;
      - chain 150 (three segments);
      - two views;
      - a deep branch flattened into a tiled chain;
      - an animated expression link across two segments.
    - Add a GL variant to `GLScheduler_Test` only if the file can take it without a CMake change: an OCIO GL node inside a chain must break the segment, with pixels identical, strict under `run-gl-tests.sh`.
  - verify: All cases green 3× in ctest (debug), and the GL case green under `build/m63-gui/run-gl-tests.sh`. These suites are part of the gate.
  - size: L

- [ ] M64.P4.T2 — Cache semantics under tiling
  - files: `Tests/TiledCache_Test.cpp` (new), `Tests/CMakeLists.txt`, `Tests/CacheMemoryPressureGuard.h` (reuse)
  - approach: With the cache cleared before each pass and the RAM purge pinned (`CacheMemoryPressureGuard`), render the same writer frame tiled and untiled (task graph). Enumerate the image cache's entries for the project's nodes (by node hash / `ImageKey`) and assert the two sets are equal: the same intermediate images are kept, and no segment member appears. Then:
    - (a) Opening a member's panel, enabling force caching on it, enabling aggressive caching, or making the graph static (no animation) removes those nodes from segments. The kept set again matches untiled, and pixels match.
    - (b) Every cached entry produced during a tiled render is fully rendered over its key's bounds (trimap/bitmap complete, no partial tile).
    - (c) A second render of the same frame after a tiled one is served from the cache identically to the untiled sequence, measured by the same counts of renders per node (`CountingTestEffect`).
    - (d) A downstream viewer-style cached node fed by a segment receives the full assembled image.
  - verify: The suite is green 3×. The entry-set comparison prints the kept keys on failure.
  - size: M

- [ ] M64.P4.T3 — Bench tiled vs whole-image, tune the strip height (KILL GATE 2)
  - files: `tools/bench/graph_bench.py` (record `tiles_run`/`segments` from the stats pass), `tools/bench/BASELINE.md`, `tools/bench/README.md`
  - approach: On a release build, with the container exclusive, cooldowns and frequency logs, run each pair back to back with `BENCH_SETTINGS="tiledRendering=0"` and `="tiledRendering=1"` (and `BENCH_RENDER_STATS=1`):
    - HD `chain:30,100` 3 frames + range 8, `wide:100 comp:100 mixed:100`
    - tiny `chain:1000 wide:1000 comp:300`
    - `readchain:30 footagecomp:32 rambound:192` (UHD)

    Sweep `NATRON_TILE_ROWS` ∈ {8, 16, 32, 64, 135} on chain 30/100 HD. Run `noRenderThreads=8/16` on chain 100 HD for the scaling form and peak RSS. Run `sample_states` on chain 30 HD in both settings. Use `compare.py --pair-settings` for the ratios. Write an "After M64" section.
  - verify: **Targets:** HD chain 30 and chain 100 single frame ≥ 1.5x faster tiled (acceptance) with lower `rss_peak`; range per-frame ≥ 1.3x; wide/comp/mixed/realistic no worse than 1.05x; tiny no worse than 1.05x. **Gate:** if chain 30 or chain 100 HD is below **1.3x**, stop before Phase 64.5. Profile one tiled chain 30 frame (`profile_run.sh`, `analyze_stacks.py`), and report to the user with the overhead breakdown (per-call fixed cost, allocation and page faults, copy-in/copy-out), recording it in `## Decisions`. Between 1.3x and 1.5x, P4.T4 runs before the default decision.
  - size: M

- [ ] M64.P4.T4 — Trim per-tile overhead (runs only if P4.T3 lands below 1.5x or the profile names a clear cost)
  - files: whichever of `Engine/Image.cpp`/`Engine/Image.h` (strip buffer reuse), `Engine/EffectInstance.cpp` (per-call passes), `Engine/RenderScheduler.cpp` (join copy) the P4.T3 profile names; at most 5 files, plus the matching test in `Tests/RenderScheduler_Test.cpp`
  - approach: Fix only what the profile shows. Candidates:
    - a per-thread recycled buffer for uncached strip images, avoiding fresh-mmap page faults per tile (check `minflt` from `/proc/<pid>/stat` across a render);
    - letting the tail render straight into the assembled image through a `RenderRoIArgs` output-image field instead of copying;
    - skipping repeated per-call work that does not depend on the RoI (component and identity queries are already request-cached; check action property churn).

    Every change keeps `memcmp` equivalence.
  - verify: P4.T1 and P4.T2 suites green; the P4.T3 chain rows re-run show the gain and nothing else regresses beyond 1.05x; full ctest in both scheduler modes and tiled.
  - size: L

## Phase 64.5: Viewer and GUI

- [ ] M64.P5.T1 — Viewer and GUI check under Xvfb with tiling on
  - files: `build/m64-gui/tiled_check.py` (new, modelled on `build/m63-gui/viewer_check.py`), `build/m64-gui/run-gui.sh` (copy of the M63 recipe)
  - approach: Use a release build under Xvfb (`build/m63-gui/run-gui.sh` recipe, `checkForUpdates=false` pre-seeded). Load an HD chain project (Read plate → 30 Grades → Merge with a second plate → Viewer + Write). In both `tiledRendering` settings:
    - view frame 1; scrub 1→13 quickly and confirm it lands on 13 with no stale strip or banding;
    - play, then change a Grade mid-play and confirm it re-renders;
    - abort a GUI Write mid-render;
    - write 6 frames from the GUI.

    The output EXRs must be byte-identical between settings (`cmp -i 128`, `oiiotool --diff`). Take screenshots at each step. Also run `build/m63-gui/gl_check.py` with `NATRON_TILED_RENDERING=1`: the GL graph must still match and report no fallbacks.
  - verify: `results.txt` all pass in both settings; no `[BUG]`/assert lines in the logs; screenshots sent to the user before sign-off.
  - size: M

## Phase 64.6: Decision, default, AppImage

- [ ] M64.P6.T1 — Publish the decision and update dependent milestones
  - files: `PLAN/DECISIONS/2026-10-06-tiled-segments.md` (new), `PLAN/DECISIONS/INDEX.md`, `docs/decisions/` copy, `PLAN/MILESTONES/M67-native-core-nodes.md`, `PLAN/MILESTONES/M31-architectural-cleanup.md` (PM work)
  - approach: Write up the segment design, the measured gates and the follow-ups: halos, tiled cached nodes, static graphs, and cross-node fused kernels for native point ops in M67 (native nodes declaring point-op/tile traits directly instead of relying on the RoI probe).
  - verify: files present, INDEX row added, repo copy committed.
  - size: M

- [ ] M64.P6.T2 — Default On, CI leg, AppImage and UAT script
  - files: `Engine/Settings.cpp`, `.github/workflows/ci.yml`, `tools/ci/local/test.sh`, `build/appimages/M64-<sha>.AppImage`, `build/appimages/M64-uat.md`
  - approach:
    - If P4.T3 passed, set the `tiledRendering` default to On. The Off setting stays as the comparison mode, with no compatibility shims.
    - Forward `NATRON_TILED_RENDERING` in `test.sh`. The CI task-graph leg now runs tiled by default; add `NATRON_TILED_RENDERING=0` to the legacy leg's environment so both remain meaningful.
    - Dispatch CI manually on the stacked branch, and run the full-tree `git clang-format` against the merge base before pushing.
    - Package with `package.sh` to `build/appimages/M64-<sha>.AppImage`, launch-check it, and write `M64-uat.md` with these steps:
      - an HD 30-node Grade chain written from the CLI with `NATRON_TILED_RENDERING=0/1`: identical frames and the speed difference visible;
      - a footage comp with a Blur and a Transform in the chain: identical frames;
      - viewer scrub and play on the chain, with a parameter change mid-play;
      - a GUI Write with abort;
      - peak memory of a 100-node chain in `htop` in both settings;
      - the M63 UAT items still pass.
  - verify: AppImage launch-checks; full ctest and smoke green with the default (tiled), `NATRON_TILED_RENDERING=0` and `NATRON_RENDER_SCHEDULER=legacy`; CI green on the branch. User sign-off is deferred to the parcel UAT (M63 + M64 + M67).
  - size: M

**Verification gate:** All of the following must hold:
- P1.T2 spike ≥ 1.3x on HD chain 30 and 100, or the milestone was stopped and reported.
- P4.T1 equivalence suite (TaskGraphVsTiled and LegacyVsTiled) green 3× with zero mismatching floats.
- P4.T2 cache-semantics suite green: the kept cache entries are identical to untiled and no partial image is ever cached or stored.
- Full ctest and `smoke_test.py` green with tiling On, with tiling Off, and in Legacy.
- Strict GL tests green under Xvfb with tiling On.
- P4.T3 targets met: HD chain 30/100 ≥ 1.5x with lower peak RSS (≥ 1.3x hard floor), and no other family slower than 1.05x.
- P5.T1 Xvfb check passed in both settings, with screenshots shared.
- AppImage `build/appimages/M64-<sha>.AppImage` packaged with `M64-uat.md`.
- Decision published.

Execution notes:
- **Batches** (implementers edit in parallel without building; one detached debug build + ctest per batch in the single-tenant `natron-dev` container, launched with setsid/nohup and a fresh `.done` marker; check `pgrep -x ninja` is 0 first; the debug build defines NDEBUG, so tests use EXPECT/ASSERT, never `assert()`):

  | Batch | Tasks | Notes |
  |---|---|---|
  | B1 | P1.T1 | release build + bench, container-exclusive |
  | B2 | P1.T2 | debug build for ctest registration, release build to run the spike; **gate 1** |
  | B3 | P2.T1, P2.T2 | disjoint: `ParallelRenderArgs.*` + `FrameGraphBuild_Test` vs `Settings`/`AppManager`/`TileGeometry.h`/`RenderStats` + `RenderSchedulerMode_Test` |
  | B4 | P2.T3, P3.T1 | disjoint: `RenderScheduler.*` + new `TilePlan_Test` (sole `Tests/CMakeLists.txt` editor) vs `TLSHolder.*`, `treeRecurseFunctor`, `EffectInstance.*` |
  | B5 | P3.T2 | first end-to-end tiled render; full ctest in all three modes; a quick release `chain:30` tiled run as an early signal |
  | B6 | P4.T1, P4.T2, then P4.T3 | P4.T2 alone edits `Tests/CMakeLists.txt`; P4.T3 is the bench (exclusive) and **gate 2** |
  | B7 | P4.T4 | only if needed; re-run the P4.T3 chain rows after it |
  | B8 | P5.T1 | release + Xvfb |
  | B9 | P6.T1, P6.T2 | |

- Any batch from B5 on runs full ctest three ways: default, `NATRON_RENDER_SCHEDULER=legacy` and `NATRON_TILED_RENDERING=1` (or `=0` once the default flips).
- Threading tests run with `--gtest_repeat=3` at pool sizes 1, 2, 4, 8 and 16, restoring `maxThreadCount` in RAII.
- Pixels are compared with `memcmp` on floats; a mismatch is a planning or hand-off bug, never a tolerance. The only exception is the existing mixed GPU/CPU GL case.
- ctest alone does not cover the viewer path; P5.T1's Xvfb run does. Bench tasks hold the container exclusively and record `/proc/loadavg` and clocks.
- Stacked PR on `milestone/m63-task-graph-render-scheduler`. CI does not run on stacked PRs automatically; dispatch `ci.yml`/`checks.yml` by hand.

## Decisions
- 2026-10-05 — **Premise updated after M63:** tiles are tasks inside M63's frame graph. Today there is one task per node per frame; M64 would split tile-capable chains into per-tile tasks and pipeline the per-image engine passes. M63's measurements: HD chains are kernel-CPU-bound within 2–3x of the memory-bandwidth floor, and serial spines gain nothing from branch parallelism, so M64 is the lever for chains. The realistic workloads showed nothing IO- or RAM-bound on the dev box. Still blocked on the user's go-ahead.
- 2026-10-06 — **Go-ahead and elaboration:** the user approved M64, stacked on M63's tip and part of the parcel UAT with M67. Design calls:
  - Segments are connected sets of uncached, tile-capable, thread-safe point-op image nodes. Tile tasks pull the segment interior over full-width row strips sized from the cache topology, and a join task stores the whole assembled tail image.
  - Cache semantics hold by construction: only uncached nodes are tiled, and the store only ever gets whole images.
  - Spatial halos, tiled cached nodes and static graphs are left as follow-ups.
  - Two kill gates at 1.3x on HD chain 30/100: a no-engine-change strip-pull spike (P1.T2) and the real implementation (P4.T3).
  - Today's host `stream_bench` puts the bandwidth floor at ~5 ms/node against ~30 ms/node for an OFX Grade, so the spike decides whether OFX chains can reach the target at all.
- 2026-10-06 — **Elaborated by consultant (14 tasks, 6 phases); P6.T1 resized S→M** (writing a decision doc needs judgement). Work happens in worktree `build/wt/m64` because the main checkout is busy with the parcel-1 merges. The Docker daemon was empty this session, so `natron-dev` is being rebuilt.
- 2026-10-06 — **PM default if kill gate 1 trips:** the consultant estimates 1.15–1.6x for OFX Grade chains, since plugin compute (~30 ms/node at HD) dwarfs one full-frame memory pass (~5 ms/node). If the spike lands below 1.3x, M64 goes `blocked` (parked until M67's native nodes make bandwidth the dominant cost), the result goes to the user, and M67 starts stacked on #41.
- 2026-10-06 — **P1.T1/P1.T2 measured on a contended host (load 2–21 on 4 cores, IO pressure ~95%, from outside the sandbox); code `30700800a` on `milestone/m64-tiled-rendering` (local, unpushed).** The spike's pixels are identical at every strip height. The best strip-pull speed-ups are chain 30 **0.60x/0.72x** and chain 100 **1.14x/1.21x** (two runs), both under kill gate 1 (1.3x). Per-call fit: a≈0.27 ms, b≈68 ns/px for one Grade. Absolute times run ~3x those after M63, so the baseline is not trusted as a reference. **Applying the PM default: M64 is `blocked`** pending a re-run of the spike on a quiet host. If it still lands below 1.3x, M64 stays parked until M67's native nodes exist. Chain 30 strips being slower than the whole frame at every height is unexplained and is the first thing to look at on the re-run. P1.T1 and P1.T2 stay unchecked until the quiet re-run.
- 2026-10-06 — **Kill gate 1 confirmed on a quiet host** (load 0.25/0.53, IO pressure 0): best strip pull is 1.05–1.17x on chain 30 and 1.14–1.15x on chain 100, with pixels identical everywhere. a≈0.20 ms, b≈58 ns/px per Grade call. There is a cliff between 16- and 32-row strips (4 strips in flight × input+output planes cross the 6 MB L3); recursive strip pulls keep N intermediates alive and zero-fill fresh images per strip; the chain-100 win comes from the whole-frame path slowing with N (31→51 ms/node). **M64 stays parked until M67's native nodes exist.** Revisit then with native point-op kernels, where fusing without per-node images is the real lever. Logs: `build/m67-b1/spike{1,2}.log`.
