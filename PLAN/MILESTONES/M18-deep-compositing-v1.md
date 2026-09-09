# Milestone 18: Deep compositing v1

"M-Deep-1" in the design doc: `DeepImage` as a first-class payload integrated
with caching, RoI, and the render scheduler — the part the Niik-l fork skipped —
plus the Tier-1 node set, viewer auto-flatten, and a per-sample pixel probe.
Requires M17 (typed edges, `NativeEffectBase`). Parallelizable with M19.

Design authority: `PLAN/DESIGN/2026-09-05-deep-and-3d-native-extensions.md`
("Part 1 — Deep compositing"). Semantics follow the OpenEXR "Interpreting Deep
Pixels" document verbatim.

## Phase 18.1: Data model and math

- [x] M18.P1.T1 — `DeepImage` data structure
  - files: `Engine/DeepImage.h`, `Engine/DeepImage.cpp` (payload transport lives in `Engine/`, not `Engine/Nodes/`)
  - approach: structure-of-channels mirroring OpenEXR deep layout — `RectI bounds` + `RenderScale` + `ViewIdx`; a COW `shared_ptr` `SampleTable` (per-pixel `counts` uint32, `offsets` uint64 prefix sum); `map<string, DeepChannelBuffer>` of per-channel contiguous float arrays indexed by offsets. Color-only ops copy only touched channels and share Z/ZBack/A via COW. Per-pixel tidiness (sorted/non-overlapping) is a recorded flag, tidied on demand. `getSizeInBytes()` counts table + *owned* buffers only so COW sharing is not double-counted.
  - verify: unit tests — COW sharing (mutating R leaves Z buffer shared), offsets/counts invariants, size accounting under sharing.
  - size: M

- [x] M18.P1.T2 — Deep sample math per "Interpreting Deep Pixels"
  - files: `Engine/DeepPixelOps.h`, `Engine/DeepPixelOps.cpp`
  - approach: free functions over sample ranges — point vs volumetric sample handling, sample split at a depth, sort, tidy (merge overlapping via Hillman's volumetric merge math), and flatten-to-front compositing. Pure functions, no Image/EffectInstance dependencies, so they unit-test in isolation and both `DeepMerge` and the viewer flatten reuse them.
  - verify: unit tests against the worked examples in the OpenEXR doc (split/merge identities, volumetric merge commutativity where the doc guarantees it).
  - size: M

- [x] M18.P1.T3 — `Cache<DeepImage>` with its own memory budget
  - files: `Engine/Cache.h` (already templated, :382), `Engine/DeepImageKey.h`/`.cpp`, `Engine/Settings.cpp`, `Engine/Settings.h`
  - approach: dedicated `Cache<DeepImage>` instantiation with `DeepImageKey = (nodeHash, time, view, scale)`; bounds live in params, not the key (leaves room for future tiling). Variable-size entries costed by actual `getSizeInBytes()`. New settings knob for a separate deep cache budget — deep frames must not evict the entire 2D image cache. Bounds growth, not tiling, in v1.
  - verify: unit test — insert/lookup round-trip; eviction respects the deep budget while the image cache stays untouched.
  - size: M

- [x] M18.P1.T4 — Wire the deep cache into the app-wide cache lifecycle
  - files: `Engine/AppManager.cpp`, `Engine/AppManager.h`, `Engine/AppManagerPrivate.h`, `Tests/DeepImageCache_Test.cpp`
  - approach: M18.P1.T3 wires `_deepImageCache` into construction, teardown and the size knob only, leaving it invisible to every other app-wide cache operation. Reach the same six sites the other three caches are wired into, following each one's existing shape rather than inventing a parallel path: `clearAllCaches()` (AppManager.cpp:1246), `clearExceedingEntriesFromNodeCache()` (:2103), `removeAllEntriesWithDifferentNodeHashForHolderPublic()` (:2361) and `removeAllEntriesForHolderPublic()` (:2382), the memory-stats reporting (:2351), and `checkCacheFreeMemoryIsGoodEnough()` (:2766-2792). Line numbers are from 2026-09-09 and will have moved — locate by name. The low-memory handler is the load-bearing one: as it stands, system memory pressure evicts the 2D node cache while the deep cache holds its full budget.
  - verify: unit test — a populated deep cache is emptied by `clearAllCaches()`; a per-node purge on hash change removes that node's deep entries and leaves another node's alone; the deep cache's bytes appear in the memory-stats total. Whole ctest suite still green.
  - size: M

## Phase 18.2: Render path

- [ ] M18.P2.T1 — `renderDeepRoI` pull pipeline
  - files: `Engine/EffectInstance.h`, `Engine/EffectInstanceRenderRoI.cpp` (or new `Engine/EffectInstanceRenderDeep.cpp`), `Engine/Nodes/NativeEffectBase.h`
  - approach: `renderDeepRoI(args, DeepImagePtr*)` with the same shape as `renderRoI` — walks up typed inputs, honors RoI/FramesNeeded (deep RoI propagation is identical to image: deep ops are spatially local), respects abort flags and the render scheduler, consults the M18.P1.T3 cache first. Capability virtual `renderDeep()` on EffectInstance; `NativeEffectBase` helpers make two-pass evaluation (pass 1: parallel per-pixel sample counts → single allocation; pass 2: parallel fill) the path of least resistance.
  - verify: unit test with two stub deep nodes chained — cache hit on second render, abort honored, two-pass helper produces identical output to a serial reference.
  - size: L

- [ ] M18.P2.T2 — Viewer deep support: auto-flatten adapter and per-sample probe
  - files: `Engine/ViewerInstance.cpp`, `Gui/ViewerTab*.cpp`, `Gui/InfoViewerWidget.cpp`
  - approach: the Viewer accepts a deep edge via the registered deep→image adapter — the **only** implicit conversion in the system; the flatten result is cached as a normal Image so scrubbing costs one flatten per frame, once. Pixel probe shows per-sample values (count, Z/ZBack/A/RGB per sample, DeepSample-style) for the pixel under the cursor.
  - verify: connect a deep source to the Viewer — image appears, no explicit DeepToImage needed; probe lists samples matching the input EXR's values.
  - size: L

## Phase 18.3: Tier-1 node set

All nodes below are `NativeEffectBase` subclasses in `Engine/Nodes/Deep/`, one
.cpp each, registered in `loadBuiltinNodePlugins()`. The vocabulary is a cap,
not a floor (design doc, "Scope gravity") — Tier-2 is M21.

- [ ] M18.P3.T1 — `DeepRead` and `DeepWrite`
  - files: `Engine/Nodes/Deep/DeepRead.cpp`, `Engine/Nodes/Deep/DeepWrite.cpp`, `Engine/CMakeLists.txt` (CMake source list + new OIIO dependency)
  - approach: `NatronEngine` does not link OpenImageIO today (`Engine/CMakeLists.txt` finds only Freetype and OpenColorIO), so this task adds `find_package(OpenImageIO CONFIG REQUIRED)` and links `OpenImageIO::OpenImageIO` — 3.1.16 with its CMake config ships in the `aswf/ci-vfxall:2027-clang21.1` image CI and `tools/ci/local/` both use, so no image change is needed. Then OIIO `DeepData` for both directions; EXR deep scanline and tiled parts. `DeepData`'s layout maps 1:1 onto `DeepImage`'s structure-of-channels, so I/O is a per-channel copy, not a transform. Deep AOVs ride the existing plane concept.
  - verify: round-trip test — read a reference deep EXR, write it back, `oiiotool --diff` clean; sample counts and Z order preserved.
  - size: M

- [ ] M18.P3.T2 — `DeepMerge`, `DeepToImage`, `DeepFromImage`
  - files: `Engine/Nodes/Deep/DeepMerge.cpp`, `Engine/Nodes/Deep/DeepToImage.cpp`, `Engine/Nodes/Deep/DeepFromImage.cpp`, CMake source list
  - approach: `DeepMerge` — combine and holdout modes over M18.P1.T2's tidy/merge math, tidying lazily. `DeepToImage` — explicit mid-graph flatten (the graph always shows where information is destroyed). `DeepFromImage` — image + optional Z input → single-sample-per-pixel deep.
  - verify: merge of two deep EXRs matches Nuke-generated reference within tolerance; DeepFromImage→DeepToImage round-trip reproduces the source image.
  - size: L

- [ ] M18.P3.T3 — `DeepRecolor`, `DeepCrop`/`DeepReformat`, `DeepExpression`
  - files: `Engine/Nodes/Deep/DeepRecolor.cpp`, `Engine/Nodes/Deep/DeepCrop.cpp`, `Engine/Nodes/Deep/DeepExpression.cpp`, CMake source list
  - approach: `DeepRecolor` — hybrid node (deep + image inputs; input-arrow glyphs from M17.P3.T2 document the ports) applying flat color to deep samples. `DeepCrop`/`DeepReformat` — bounds/format ops touching only the sample table. `DeepExpression` — per-sample expression over channels, reusing the existing expression machinery.
  - verify: per-node unit render tests; DeepRecolor shares Z buffers with its input (COW assertion from M18.P1.T1's test hooks).
  - size: L

- [ ] M18.P3.T4 — Deep integration test in CI
  - files: `Tests/` (the ctest suite; the directory is capital-T `Tests/`, and M11's OFX integration test is `tools/ci/smoke_test.py` + `tools/ci/verify_plugin_loads.cpp`, not a ctest case), reference deep EXR asset under `Tests/fixtures/` (pinned, same discipline as existing test assets)
  - approach: end-to-end graph — DeepRead → DeepMerge → DeepRecolor → DeepToImage → Write — rendered headless in CI, output diffed against a committed reference. This is the deep analog of M11's OFX plugin test.
  - verify: test green in the `build-and-test` job; deliberately breaking the merge math makes it fail.
  - size: M

**Verification gate:** all unit tests and the M18.P3.T4 end-to-end CI test green; deep EXR round-trip clean; Viewer flattens a deep stream with per-frame caching (second scrub pass hits cache); deep cache budget respected under a memory-pressure test; entire pre-existing ctest suite still green.

## Decisions

- 2026-09-09 — Promotion freshness check (PLAN-FORMAT.md §5a): the milestone's
  premise holds — `Engine/Nodes/NativeEffectBase.h`, the templated `Engine/Cache.h`,
  `Engine/EffectInstanceRenderRoI.cpp`, `Engine/ViewerInstance.cpp` and
  `Gui/InfoViewerWidget.cpp` all exist as briefed, and `Engine/Nodes/README.md`
  documents the registration path. Two task briefs were re-planned in place:
  M18.P3.T1 (OpenImageIO is not yet a `NatronEngine` dependency — the task now
  owns wiring it; OIIO 3.1.16 with a CMake config is already in the
  `aswf/ci-vfxall:2027-clang21.1` image) and M18.P3.T4 (the test directory is
  `Tests/`, and M11's OFX integration test lives in `tools/ci/`, not in ctest).

- 2026-09-09 — `DeepImage::getSizeInBytes()` counts all channel buffers, not
  only uniquely-owned ones: `Engine/Cache.h` keeps `_memoryCacheSize` as a
  running integer (added at allocation, re-queried and subtracted at
  `deallocate()`, clamped at zero), so an entry's cost must be aliasing-blind
  and identical at insert and destroy, as `Image::size()` already is. The
  use-count-based reading the brief invited would report ~0 bytes for any deep
  frame a downstream node is rendering from, so the deep budget would never
  evict. `getUniquelyOwnedSizeInBytes()` keeps the aliasing-aware figure for
  diagnostics. The design doc's parenthetical, which was the origin of the
  wording, is amended in the same change.

- 2026-09-09 — `DeepPixelOps` takes non-owning structure-of-channels views, not
  an owning array-of-structs sample list. The first implementation used
  `struct DeepSample { float z, zback; std::vector<float> channels; }`, which
  cannot point at a `DeepImage`'s existing per-channel runs (one pixel's data
  for a channel is already contiguous at `data() + offset`), cannot express a
  missing ZBack channel without materializing it, and costs a heap allocation
  per sample on the render scheduler's hot path — no per-pixel allocation exists
  anywhere in the image path (`ImageMaskMix.cpp`, `ImageCopyChannels.cpp`,
  `ImageConvert.cpp` all walk raw `PIX*` with tile-granularity scratch). It was
  reworked before dependants existed because M18.P2.T1's `NativeEffectBase`
  helpers turn this into node-author-facing surface for six M18 nodes and the
  M21 ports. `DeepPixelView`/`MutableDeepPixelView` + a caller-owned
  `DeepPixelScratch`/`DeepTidyWorkspace` replace it; the numerics (the
  `-expm1(f * log1p(-a))` split, the Hillman merge, the depth tie-break) carried
  over unchanged, as did all 14 tests' expected values. `DeepSample` survives
  only as a convenience type for the pixel probe and tests. Not templated on
  channel count: deep channel counts are genuinely dynamic, unlike the image
  path's 1-4.

- 2026-09-09 — Watch item, not yet a task: `BaseTest.ConcreteDeclaredInputDoesNotResolvePolymorphicOutput` and `BaseTest.ClearingTheDataKindErrorLeavesAnUnrelatedErrorAlone` (both from M17) failed once with "Subprocess aborted" and passed on
  an immediate re-run with no intervening change. If they recur, they need their
  own milestone — an intermittently red `build-and-test` job is worse than a
  failing one. **It recurred**, on a third case
  (`BaseTest.StillUnconstrainedPolymorphicConnects`), whose gtest body printed
  `[  OK  ]` before the process aborted during teardown with
  `QThread: Destroyed while thread is still running`. That points at the shared
  `BaseTest` fixture's teardown, not at any one case, and it will make this
  milestone's own PR intermittently red. Raise it as its own milestone at the
  M18 gate.

- 2026-09-09 — The deep cache must be wired into the full app-wide cache
  lifecycle inside this milestone, not deferred. M18.P1.T3 reached only
  construction, teardown and the size knob, leaving `_deepImageCache` absent
  from `clearAllCaches()`, `clearExceedingEntriesFromNodeCache()`, both
  `removeAllEntries*ForHolderPublic()` hooks, the memory-stats reporting and
  `checkCacheFreeMemoryIsGoodEnough()`. That was proposed as a known v1 gap and
  rejected on the grounds that it is core caching functionality, not polish: a
  "Clear cache" action that silently leaves deep entries resident is wrong, and
  a low-memory handler that evicts the 2D node cache while the deep cache holds
  its full budget inverts the very isolation the separate budget exists to
  provide. Split out as M18.P1.T4 rather than folded into T3 so it carries its
  own verify and commit.

- 2026-09-09 — M18.P1.T3's two new cache tests both failed on their first real
  run, for the reason a pre-commit review predicted: `Cache::getOrCreate()`
  returns `true` when an entry is *found* and `false` when it is *created*, so
  four `ASSERT_TRUE` calls on freshly-created entries were inverted. The
  eviction test was also vacuous — it held `entry1` alive while creating
  `entry2`, so `LRUHashTable::evict()`'s `use_count() == 1` guard skipped it and
  nothing was ever evicted. Both fixed, and the eviction test was re-checked by
  temporarily raising the budget to confirm its assertions fail when no eviction
  occurs rather than passing regardless.

- 2026-09-09 — The low-memory eviction step was extracted from
  `AppManager::checkCacheFreeMemoryIsGoodEnough()` into
  `AppManager::evictLRUFromMemoryCaches()` so it could be tested: the enclosing
  loop is driven by real `getAmountFreePhysicalRAM()` readings and cannot be
  driven hermetically, but the per-pass step can. The helper asks both caches on
  every call rather than short-circuiting, and its test asserts the deep entry
  goes on the *first* call while the node cache still holds three — which is
  exactly what a short-circuiting implementation would fail.

- 2026-09-09 — All three app-wide wiring tests were verified red-then-green by
  reverting one `AppManager.cpp` line at a time, not by reading: clear-all fails
  with `getDeepImage` returning true; the hash-change purge fails on its 5s poll
  timeout; the memory-stats test reports 0 instead of 448 bytes. None was
  vacuous. Worth keeping as the standard for this milestone's remaining tests —
  the first version of the T3 cache tests passed review by inspection and then
  failed on their first real run.

