# Milestone 62: Render scaling — fix the algorithmic hotspots

A side session on 2026-09-25 benchmarked large synthetic graphs at `b0e212d5a` (release build, `tools/bench/`, results and logs in `build/bench/`). It asked whether Natron needs a scanline, tiled or top-down rework to handle thousands of nodes. The data says not yet. A few superlinear algorithms in the engine dominate long before render architecture matters, so this milestone fixes them and leaves the benchmark harness in the repo as the regression gate. A task-graph scheduler (M63) and tiled rendering (M64) are stubbed behind it, blocked on this milestone's re-benchmark. See `DECISIONS/2026-09-25-perf-hotspots-before-render-architecture.md`.

Baseline (median seconds per frame; N = number of processing nodes; 4-core Intel N100; absolute times vary up to 2x between identical runs, so compare ratios):

| Graph | Build | Frame | Notes |
|---|---|---|---|
| tiny chain 100 / 1000 / 3000 | 0.7 / 41 / 914 s | 0.056 / 1.67 / 16.4 | Frame cost is quadratic in N. Build at 3000 is naming (`checkNodeName`). |
| tiny wide 1000 / 3000 | 13 / 69 s | 0.96 / 3.39 | Linear, about 1.1 ms per node. |
| tiny comp 300 / 1000 | 3.6 s / never finished | 1.27 / — | Exponential cycle check on diamond DAGs. |
| HD chain 30 / 100 | 0.19 / 0.77 s | 3.8 / 19.6 | About 100–130 ms per OFX Grade against a ~9 ms bandwidth floor. Parallelism 1.0–1.5 on 4 cores. |
| RSS before render | — | — | 0.6–1.0 MB per node (tiny chain 3000: 1962 MB before any render). |

Causes, from eu-stack samples (`build/bench/samples-*.txt`, summarised by `tools/bench/analyze_stacks.py`):

- **Per-frame upstream walk.** `EffectInstance::isFrameVaryingOrAnimated_Recursive()` (`Engine/EffectInstance.cpp` ~5377–5404) walks everything upstream from every node on every `renderRoI` (`Engine/EffectInstanceRenderRoI.cpp` ~783). On a chain that is O(N²) per frame (64% of busy time in chain 1000). It also has no visited set, so it is exponential on diamond DAGs whenever the answer is false.
- **TLS copy per multithreaded call.** When a plugin multithreads, `AppTLS::copyTLSFromSpawnerThreadInternal` (`Engine/TLSHolderImpl.h` ~253) copies thread-local storage for *every* TLS holder in the project, and `AppTLS::cleanupTLSForThread` (`Engine/TLSHolder.cpp` ~121) tears it all down again. The host frame-threading path does the same through `AppTLS::copyTLS` (`Engine/EffectInstance.cpp` ~2171). That is O(N) per call.
- **Naming.** `NodeCollection::checkNodeName` (`Engine/NodeGroup.cpp` ~472) rescans every node, calling `isActivated()` and `getScriptName_mt_safe()` for each, once per candidate suffix. That is O(N²) per node and O(N³) to build a graph. `app.createNode` with an explicit `CreateNodeArgsPropNodeInitialName` avoids most of it, and the bench defaults to that (`BENCH_NAMED=1`).
- **Connect.** `Node::onInputChanged`'s path (`Engine/NodeInputs.cpp` ~1625–1660) builds a `ParallelRenderArgsSetter` over the whole upstream graph, including expression dependencies, on every connect. That is O(N) per connect.
- **Cycle check.** `Node::isNodeUpstream` (`Engine/NodeInputs.cpp` ~585) recurses without a visited set, so it is exponential on diamond DAGs. The comp-1000 graph never finished building.
- **HD per-node cost.** Plugin scalar per-pixel code is about 70% of an OFX Grade's time. The host's single-threaded `Image::copyUnProcessedChannels` (`Engine/ImageCopyChannels.cpp` ~395, per-pixel `pixelAt` on the source), which restores Grade's unprocessed alpha, is about 25%. Plugin internals are out of scope here; the host copy is not.

Out of scope: plugin (openfx-misc) inner loops, the scheduler (M63) and tiles (M64).

Execution notes:
- Benchmarks run against `build/release` in the `natron-dev` container. Correctness tests run against the debug build as usual. The container is single-tenant: one build or one benchmark at a time, launched detached with a done-marker, never both at once. A benchmark run concurrently with a build produces meaningless numbers.
- Profiling recipe: `docker exec -u root --privileged natron-dev eu-stack ...` via `tools/bench/profile_run.sh`. Take thread concurrency from `tools/bench/sample_states.sh`, not from eu-stack samples. perf is unavailable.
- The debug build defines NDEBUG, so tests use EXPECT/ASSERT, not assert().
- Timing assertions in gtest must be generous structural bounds (such as "a 30-level diamond finishes in under 5 s", where the exponential version needs hours), never tight wall-clock thresholds that flake on CI.
- Every task re-runs the bench configurations it targets, before and after, and records the ratio in this file's `## Decisions`.

## Phase 62.1: Benchmark harness in the repo

- [ ] M62.P1.T1 — Commit tools/bench with its README and a baseline summary
  - files: `tools/bench/*` (currently untracked: `graph_bench.py`, `run_matrix.sh`, `profile_run.sh`, `sample_stacks.sh`, `sample_states.sh`, `analyze_stacks.py`, `stream_bench.c`, `README.md`), `tools/bench/BASELINE.md` (new)
  - approach: bring the scripts up to the repo's comment policy (comment-policy skill) and lint (`lint-ci` covers shell/Python). Don't change behaviour. `BASELINE.md` records the 2026-09-25 numbers in this file's table plus the machine, commit and exact commands, generated from `build/bench/results-{tiny,hd,hdrange}.jsonl`. Don't commit the jsonl, sample or log files; they stay in `build/bench/`.
  - verify: `format`, `lint-ci` and `build-and-test` stay green. `tools/bench/run_matrix.sh smoke tiny 2 0 chain:10 wide:10` runs in the container and appends two result lines.
  - size: M
- [ ] M62.P1.T2 — Add a results comparison script
  - files: `tools/bench/compare.py` (new), `tools/bench/README.md`
  - approach: `compare.py BEFORE.jsonl AFTER.jsonl` matches rows on (topo, n, res, named) and prints build time, median frame time, parallelism and RSS before/after as ratios, flagging any regression beyond a `--threshold` (default 1.5x, given the 2x run-to-run noise). Standard library only.
  - verify: comparing `results-tiny.jsonl` with itself prints all ratios 1.00 and exits 0. A hand-edited copy with one frame time tripled is flagged and exits non-zero.
  - size: M

## Phase 62.2: Graph construction

- [ ] M62.P2.T1 — Give the cycle check a visited set
  - files: `Engine/NodeInputs.cpp`, `Engine/Node.h`, `Tests/GraphScaling_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: rewrite `Node::isNodeUpstream` as an iterative DFS with a visited set of `const Node*`, keeping its signature and main-thread assertion. While in the file, grep `Engine/` for other recursive upstream/downstream walks with no visited set (`isNodeUpstream`-style `*_Recursive` helpers on `Node` and `EffectInstance`). List any on connect or render paths in `## Decisions` as candidate follow-up tasks; don't fix them here. `isFrameVaryingOrAnimated_impl` belongs to P3.T1.
  - verify: a new gtest builds a diamond ladder 40 levels deep (each level is two nodes reading the previous level's pair, then merging) and connects a node at the bottom. It must finish in under 5 s where the old code would need ~2^40 visits. A second gtest checks that connecting a node into its own upstream is still refused. Full debug ctest green.
  - size: M
- [ ] M62.P2.T2 — Make auto-naming linear per node
  - files: `Engine/NodeGroup.cpp`, `Tests/GraphScaling_Test.cpp`
  - approach: in `NodeCollection::checkNodeName`, collect the script names of the other activated nodes into an `std::unordered_set<std::string>` once, under `nodesMutex`, then probe candidate suffixes against it. Keep today's semantics exactly: the lowest free suffix from 1, other activated nodes only, and the same errors for `errorIfExists`/`!appendDigit` and for group-knob collisions.
  - verify: gtest: create 2000 Grades with default names through `AppInstance::createNode`; names are `Grade1`..`Grade2000` with no gaps, and after deleting `Grade7` the next Grade is named `Grade7`. Bench: `BENCH_NAMED=0` tiny chain 1000 build time drops by at least 10x against the baseline (compare.py).
  - size: M
- [ ] M62.P2.T3 — Stop walking the whole upstream graph on every connect
  - files: `Engine/NodeInputs.cpp`, `Engine/ParallelRenderArgs.cpp`, `Engine/ParallelRenderArgs.h`, `Tests/GraphScaling_Test.cpp`
  - approach: `onInputChanged` builds a full `ParallelRenderArgsSetter` (all upstream nodes plus expression dependencies, with TLS set on each) only so that a plugin's input-changed/clip-preferences code can call into the tree. Find out what TLS the effect actually reads there: `getClipPreferences` and metadata need the node and its direct inputs, not the whole tree. Then replace it with a setter scoped to the node plus its direct inputs, or one that sets TLS lazily on first access. Record which one and why in `## Decisions`. If a plugin in the bundle genuinely needs deeper TLS, keep the full walk for that case only and document it. The preview path (`Engine/Node.cpp` ~3876) is a real render and keeps the full setter.
  - verify: full debug ctest green, including the OFX plugin integration tests (they exercise onInputChanged across the bundle). Bench: tiny chain 1000 with `BENCH_NAMED=1` builds at least 5x faster than baseline. Under Xvfb, connecting and reconnecting a Read → Grade → Merge in the GUI still refreshes metadata and the viewer.
  - size: L

## Phase 62.3: Per-frame render overhead

- [ ] M62.P3.T1 — Compute "frame varying or animated" once per render, not per node per frame
  - files: `Engine/EffectInstance.cpp`, `Engine/EffectInstance.h`, `Engine/EffectInstanceRenderRoI.cpp`, `Engine/ParallelRenderArgs.cpp`, `Engine/ParallelRenderArgs.h`
  - approach: `ParallelRenderArgsSetter` already visits every upstream node once per render through `getAllUpstreamNodesRecursiveWithDependencies_internal`, with a visited map. Compute each node's `isFrameVaryingOrAnimated` bottom-up (memoised, so each node is O(its inputs)) while setting up, and store it in that node's `ParallelRenderArgs`. `renderRoI` reads it from TLS, falling back to the recursive call (now given a visited set) only when there is no frame TLS. The value must match the old function exactly: same predicate (`isFrameVarying() || getHasAnimation() || getRotoContext()`), same inputs traversed. It is per-render state, so animation added mid-session is picked up on the next render with no invalidation logic.
  - verify: a new gtest in `Tests/GraphScaling_Test.cpp` checks that a chain with one animated node at the top reports varying on every node downstream and not upstream, both through the TLS path and the fallback. Full debug ctest green (cache tests exercise `shouldCacheOutput`). Bench: tiny chain 1000 frame time drops at least 3x against baseline, and chain 3000 scales at most ~3.5x of chain 1000.
  - size: L
- [ ] M62.P3.T2 — Copy TLS only for the holders the spawned thread will use
  - files: `Engine/TLSHolderImpl.h`, `Engine/TLSHolder.cpp`, `Engine/TLSHolder.h`, `Engine/EffectInstance.cpp`, `Tests/GraphScaling_Test.cpp`
  - approach: a thread spawned by the multithread suite or host frame threading runs one effect's render and reaches other holders only through that effect's inputs and its expression dependencies. Replace the copy-everything loop with lazy copy-on-first-access: record the spawner in `_spawns` (as today), and let `TLSHolder<T>::getOrCreateTLSData` copy just that holder from the spawner when it is first touched. Keep the spawn-map entry until the thread's cleanup rather than erasing it on the first copy. `cleanupTLSForThread` then visits only holders that actually have data for the thread: keep a per-thread list of touched holders instead of scanning every object. The old eager copy's main job was `ParallelRenderArgs` for input effects; check that `getImage` from a spawned thread still sees its input's frame args. Consult an opus reviewer on the concurrency before landing (this is lock-order-sensitive code).
  - verify: full debug ctest green, run three times (TLS bugs are racy). A new gtest renders an HD-ish (512×512) chain of 50 multithreaded OFX Blurs with an expression linking one knob across nodes, and matches the pixels of the same graph rendered single-threaded. Bench: tiny wide 1000 and HD chain 30 get no slower; `samples` from `profile_run.sh` on chain 1000 no longer show `copyTLSFromSpawnerThreadInternal` or `cleanupTLSForThread` above 2% self time.
  - size: L

## Phase 62.4: HD per-node host overhead

- [ ] M62.P4.T1 — Make copyUnProcessedChannels row-based and multithreaded
  - files: `Engine/ImageCopyChannels.cpp`, `Engine/Image.h`, `Tests/Image_Test.cpp`
  - approach: the per-pixel loop (`ImageCopyChannels.cpp` ~100–200) resolves `pixelAt` on the source for every pixel. Resolve the source row pointer once per row and step through it, handling the case where the source bounds don't cover the whole ROI the same way as today (zero or skip, per the existing branches). Then split the ROI into row bands across `QThreadPool`/`MultiThread` workers when the ROI is above a size threshold (for example 256×256), and run single-threaded below it. The copy is called from inside a render; make sure the parallel path doesn't need TLS (it shouldn't: pure pixel copying).
  - verify: `Tests/Image_Test.cpp` gains cases covering every PIX type × nComps combination the templates instantiate, partial source bounds, and the premult branch. The output is bit-identical to a reference produced by the old per-pixel loop kept in the test. Bench: HD chain 30 frame time improves, and `analyze_stacks.py` on a fresh `hdchain30` profile shows `copyUnProcessedChannels` below 10% of busy time (baseline ~25%).
  - size: M

## Phase 62.5: Memory per node

- [ ] M62.P5.T1 — Find out where 0.6–1.0 MB per idle node goes
  - files: `build/bench/` (outputs only), this file's `## Decisions`
  - approach: investigation only; no code change. Run `valgrind --tool=massif` (available in the container) on NatronRenderer building tiny chain 300 without rendering, then `ms_print` the peak snapshot. Attribute the bytes to call sites (knob construction, OFX instance/clip descriptors, cache structures, TLS, Python wrappers). Record the top 5 sources with their per-node bytes in `## Decisions`, and propose fixes as new tasks in this phase, or in M31 if they are structural. The user decides whether to take them into this milestone.
  - verify: the Decisions entry exists with numbers that add up to at least 70% of the measured per-node RSS, and the proposal has been put to the user.
  - size: M

## Phase 62.6: Re-benchmark and hand-off

- [ ] M62.P6.T1 — Re-run the full matrix and record the new scaling picture
  - files: `tools/bench/BASELINE.md`, this file's `## Decisions`
  - approach: on the milestone tip's release build, re-run the tiny (chain/wide/comp up to 3000, including comp 1000, which never finished before) and HD (chain/mixed/wide/comp up to 300) matrices and the three profiles (`chain1000`, `comp300`, `hdchain30`), with `sample_states.sh` for concurrency. Append an "after M62" section to `BASELINE.md` with compare.py output against the baseline. In `## Decisions`, write the input M63 needs: at HD, what fraction of wall time is spent with fewer than 4 threads busy, and which of the remaining costs are per-node serial overhead versus plugin pixel work.
  - verify: `BASELINE.md` has both sections. Tiny chain 3000 renders in under 3 s per frame, and comp 1000 builds and renders at all. The M63 input is written.
  - size: M

**Verification gate:** full debug ctest green (run twice, given the TLS change), plus CI `format`, `lint-ci` and `build-and-test`. `tools/bench/compare.py` against the 2026-09-25 baseline shows: tiny chain 1000 frame ≥3x faster, tiny chain 3000 frame under 3 s, comp 1000 builds, `BENCH_NAMED=0` chain 1000 build ≥10x faster, and no configuration more than 1.5x slower. An Xvfb GUI smoke check (open a project, connect nodes, scrub, render) behaves as before. The AppImage is packaged to `build/appimages/` for the user's check.
