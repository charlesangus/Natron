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

- [ ] M18.P1.T5 — Make the deep cache tests hermetic against the low-memory handler
  - files: `Tests/DeepImageCache_Test.cpp`, `Tests/DeepRenderPipeline_Test.cpp`, possibly `Engine/Settings.h`/`.cpp`
  - approach: `DeepImageCacheTest.HashChangePurgeRemovesOnlyThatNodesStaleDeepEntries` and `DeepRenderPipelineTest.BoundsGrowthReRendersOverTheUnionOfRequestedRoIs` fail on any host whose **free** physical RAM is under `getSystemTotalRAM() * getUnreachableRamPercent()` (default 20%). `AppManager::checkCacheFreeMemoryIsGoodEnough()` runs on every `allocateMemory()` and evicts LRU entries from the node *and* deep caches until free RAM clears that bar; `getAmountFreePhysicalRAM()` reads `sysinfo.freeram`, i.e. `MemFree`, which page cache keeps low regardless of how much memory is actually reclaimable, and evicting cache entries barely moves it — so on such a host the loop drains both caches and the first test's own sanity check (`DeepImageCache_Test.cpp:318`) fails before the purge under test is even called. Any test that asserts a cache *retains* something must pin the setting it depends on: set `unreachableRamPercent` to 0 for the duration (fixture setup/teardown, restoring the previous value) so the handler cannot fire. Do **not** weaken the assertions, and do not touch M18.P1.T4's `evictLRUFromMemoryCaches()` test — it drives the helper directly and is unaffected. While there, judge whether the reading `MemFree` rather than `MemAvailable` is worth its own milestone and say so rather than fixing it here.
  - verify: both tests pass on this host as it is now (free RAM below the threshold — check with `free -m` before and confirm the arithmetic in the task notes still holds), and still pass with `unreachableRamPercent` restored; each driven red-then-green by reverting the pin. Whole ctest suite green.
  - size: M

## Phase 18.2: Render path

- [x] M18.P2.T1 — `renderDeepRoI` pull pipeline
  - files: `Engine/EffectInstance.h`, `Engine/EffectInstanceRenderRoI.cpp` (or new `Engine/EffectInstanceRenderDeep.cpp`), `Engine/Nodes/NativeEffectBase.h`
  - approach: `renderDeepRoI(args, DeepImagePtr*)` with the same shape as `renderRoI` — walks up typed inputs, honors RoI/FramesNeeded (deep RoI propagation is identical to image: deep ops are spatially local), respects abort flags and the render scheduler, consults the M18.P1.T3 cache first. Capability virtual `renderDeep()` on EffectInstance; `NativeEffectBase` helpers make two-pass evaluation (pass 1: parallel per-pixel sample counts → single allocation; pass 2: parallel fill) the path of least resistance.
  - verify: unit test with two stub deep nodes chained — cache hit on second render, abort honored, two-pass helper produces identical output to a serial reference.
  - size: L

- [x] M18.P2.T2 — Deep→image adapter seam and the cached flatten (Engine half)
  - files: `Engine/EffectInstance.h`, `Engine/Node.cpp`, `Engine/DeepFlatten.h`/`.cpp` (new), `Engine/EffectInstanceRenderDeep.cpp`, `Engine/ViewerInstance.h`/`.cpp`, `Engine/CMakeLists.txt`, `Tests/DataKindTestEffect.h`, `Tests/DeepFlatten_Test.cpp` (new), `Tests/DeepRenderPipeline_Test.cpp`
  - approach: the adapter is a per-input opt-in virtual `EffectInstance::inputAcceptsDataKindViaAdapter(int, DataKindEnum)` (default false, overridden only by `ViewerInstance` for `eDataKindDeep`) **gated by a fixed one-row table** `isRegisteredAdapter(from, to)` in `Node.cpp` — both must hold, so neither a node nor a table row can widen the system alone, and deep→image cannot leak mid-graph. Three guard sites in `Node.cpp`: `isInputDataKindUnacceptable()` (serves connect-time *and* project-load conflict reporting in one place), the `constraint->merge()` in `collectDownstreamDataKindRequirement()` (without it a `Dot` between a deep source and the Viewer resolves ambiguous — the M17 regression), and `findDataKindConflictDownstream()`. New `Engine/DeepFlatten.{h,cpp}` holds `flattenToImage()` (build a `DeepPixelView` over the source's existing per-channel runs, `tidySamples()` then `flattenFrontToBack()` straight into the destination Image row — zero copy) and `getSamplesAtPixel()` for the probe; it is shared code, not Viewer-private — M18.P3.T2's `DeepToImage` calls the same functions. `EffectInstance::renderDeepRoIFlattened()` in `EffectInstanceRenderDeep.cpp`, called on the deep upstream effect, mints an `ImageKey` from the deep node's own `nodeHash` (exact automatic purge via `removeAllImagesFromCacheWithMatchingIDAndDifferentKey`; safe because a deep-output node never produces `Cache<Image>` entries of its own), looks it up per the `RotoContext.cpp` precedent, on miss calls `renderDeepRoI()` — never reimplements the pull — and retracts the half-filled entry with `removeFromImageCache()` on abort. `ViewerArgs::deepUpstream` is detected main-thread-only from the memoized effective kind; the branch in `renderViewer_internal` bypasses the layer/components block and synthesizes RGBA (deep channels are not Natron planes; the v1 Viewer contract is flatten-to-RGBA).
  - verify: ten ctest cases, each driven red-then-green by perturbing one line — adapter accepts / adapter refuses scene even when the effect asks for it (the invariant) / polymorphic node between deep source and adapter sink resolves deep (the M17 regression) / adapter edge survives project load with no persistent message / flatten matches a serial reference **over a pixel with overlapping samples** / second flatten is a cache hit / abort leaves nothing cached / cache invalidates on deep-node hash change / probe samples match the payload / probe returns raw not tidied samples. Whole ctest suite still green.
  - size: L

- [ ] M18.P2.T3 — Viewer per-sample probe plumbing (Gui half)
  - files: `Engine/UpdateViewerParams.h`, `Engine/OpenGLViewerI.h`, `Engine/ViewerInstance.cpp`, `Gui/ViewerGL.cpp`, `Gui/ViewerGLPrivate.h`, `Gui/InfoViewerWidget.h`/`.cpp`, `Gui/ViewerTab40.cpp`
  - approach: **execute after M18.P3.T1** so the manual verify has a real deep source to hover. Carry `DeepImagePtr deepImage` on `UpdateViewerParams` beside `colorImage`; hand it to the GUI through one new `OpenGLViewerI::setLastRenderedDeepImage(textureIndex, mipmapLevel, deepImage)` rather than an 18th argument on `endTransferBufferFromRAMToGPU`, passing null on the image path so a stale payload cannot outlive its frame; stash it in `TextureInfo::lastRenderedDeepTiles` next to `lastRenderedTiles` and clear it wherever that is cleared. `ViewerGL::getDeepSamplesAt()` beside `getColorAt()` delegates to `DeepFlatten::getSamplesAtPixel()` — and unlike `getColorAt` it does **not** fall back to a neighbouring mipmap level, because samples from the wrong scale are actively misleading; return false and show a dash. The info bar is one text-line tall, so it gets a summary label (`deep: 7 smp  Z 12.40–48.90`) and the full per-sample list (`Z / ZBack / A / R G B`, monospace, capped ~16 with a trailing count) in that label's dynamic tooltip — the only multi-line affordance there. Probe reads raw untidied samples: tidying splits and merges, and would show the user values the source file does not contain.
  - verify: no ctest coverage is possible — the Viewer plugin is not registered in the `Tests` binary (`registerBuiltInPlugin<ViewerInstance>` is gated on `!isBackground()`) and `Tests` does not link `NatronGui`, so there is no test to write and `DISABLED_` is the wrong tool. Manual app-launch checklist instead, recorded in this milestone's `## Decisions` with its result: DeepRead connects to the Viewer with no explicit `DeepToImage`; the image appears; scrub forward then back and confirm the second pass does not re-render (render counter or `--enable-render-stats`); hover a known multi-sample pixel and match bar count + tooltip values against the source EXR; hover an empty pixel and get a dash, not a stale list. Whole ctest suite still green.
  - size: M

- [ ] M18.P2.T4 — Pull deep inputs over the window actually being rendered
  - files: `Engine/EffectInstanceRenderDeep.cpp`, `Tests/DeepRenderPipeline_Test.cpp`
  - approach: in `renderDeepRoI`, the canonical RoI handed to the upstream pull is derived from the requested `roi`, not from `boundsToRender` (the candidate is the `canonicalRoI` computation around `EffectInstanceRenderDeep.cpp:309`). After the bounds-growth path widens `boundsToRender` to the union of the requested and cached bounds, the node therefore renders a wider window than it pulled its inputs over. With an upstream cache hit the extra region happens to be covered; on an upstream miss it is silently truncated data rather than a re-render — a wrong answer, not a slow one. `renderDeepRoIFlattened` and so the whole Viewer deep path inherit it. Found while implementing M18.P2.T2; the M18.P2.T1 tests missed it because `BoundsGrowthReRendersOverTheUnionOfRequestedRoIs` exercises growth with the upstream still cached.
  - verify: a test that grows bounds **with the upstream entry evicted between the two renders**, asserting full sample coverage over the union (not just the second RoI); driven red-then-green by reverting the fix. Whole ctest suite green.
  - size: M

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


- 2026-09-10 — The teardown-abort watch item above is superseded: it is now
  **M26**, to be fixed and merged before M18 continues, rather than raised at the
  M18 gate. It stopped being confined to `BaseTest` — measured at ~50% across
  three runs, and it takes down M18's own `DeepRenderPipelineTest` cases — so it
  would have made the verify step of every remaining M18 task meaningless. See
  `PLAN/DECISIONS/2026-09-10-fix-test-fixture-teardown-flake.md`.

- 2026-09-10 — M18.P2.T1's four assertions were each driven red-then-green by
  perturbing one line of the code under test, per this milestone's standard, and
  the first attempt found a real gap. Disabling the bounds-growth cache block
  (`EffectInstanceRenderDeep.cpp:281`) left the whole suite green: `renderDeepRoI`
  has two cache paths, and the tests only ever rendered one full-frame RoI, so
  every cache hit was being served by the exact-bounds path at
  `getDeepImageOrCreate()` and the bounds-growth path — the strategy the design
  doc names for v1 — was never executed. The cache-hit assertion was then
  confirmed red by stubbing the path it actually uses. A
  `BoundsGrowthReRendersOverTheUnionOfRequestedRoIs` case was added to cover the
  gap rather than recording it as a known one. The other three perturbations:
  early abort check (`:190`) → `AbortBeforeRenderingIsHonoured` red; the
  `removeFromDeepImageCache()` retraction on abort (`:413`) →
  `AbortDuringUpstreamRenderLeavesNothingCached` red; dropping the last scanline
  chunk in `makeDeepScanlineChunks()` → `TwoPassHelperMatchesSerialReference` red
  on a 342-vs-371 sample-count mismatch.

- 2026-09-10 — M18.P2.T2 is split in two at `Engine/DeepFlatten.h`, and the
  adapter is a per-input opt-in virtual gated by a fixed table rather than a
  registry. Both came out of scouting the Viewer path. The split is forced by
  testability: `registerBuiltInPlugin<ViewerInstance>` is gated on
  `!isBackground()` and the `Tests` target never links `NatronGui`, so **no ctest
  case can create a Viewer node or touch `Gui/`** — leaving the whole task as one
  unit would have put its substance behind a boundary where this milestone's
  red-then-green standard cannot reach. So everything substantive lives in Engine
  code a headless test can call (T2, ten cases), and the Gui half is thin
  plumbing with a manual checklist (new T3). T3 is sequenced after M18.P3.T1
  because `DeepRead` is the only thing that gives its manual verify a real deep
  source to hover; the original brief's verify ("connect a deep source to the
  Viewer") was not executable at this point in the milestone at all.
  On the adapter: a registry keyed on (from-kind, to-kind) alone cannot express
  "Viewer only" and would legalize deep→image at every image input — the
  mid-graph leak the design doc forbids — and keying it on consumer identity
  makes it the per-input virtual plus a global mutable table, with an init-order
  problem and a plugin-reachable registration hook, for a table the doc says will
  only ever have one row. The tempting one-liner — `ViewerInstance::getInputDataKind()`
  returning `eDataKindPolymorphic` — is rejected on purpose: it also accepts
  **scene**, and it makes the Viewer invisible to `findDataKindConflictDownstream()`,
  so a real scene→Viewer mistake becomes silent instead of reported. Two
  conditions (fixed table AND per-input opt-in) mean neither a node nor a table
  row can widen the system alone, and a test asserts exactly that.
  Also recorded: `Engine/DeepFlatten.{h,cpp}` is shared, not Viewer-private —
  M18.P3.T2's `DeepToImage` calls the same `flattenToImage()`.

- 2026-09-11 — M18.P2.T2 landed as `9af908d7d`, with all eleven cases driven
  red-then-green (the eleventh is over the ten briefed: the
  `findDataKindConflictDownstream()` guard is only reachable when the sink is
  wired to the pass-through *before* the deep source is, so it needed its own
  case). Four corrections to the design brief, all verified against the code:
  `Engine/CMakeLists.txt` has no source list to edit — it globs, so a new file
  needs only a `--reconfigure`; `appPTR->removeFromImageCache()` does not exist
  and the `Cache<Image>` retraction is `AppManager::removeFromNodeCache(const
  ImagePtr&)`; and **two of the briefed perturbations were vacuous**, which is
  the second time this milestone's stated perturbations have been weaker than
  the tests they were meant to validate. Skipping the cache lookup does not make
  the cache-hit test red, because `Cache::getOrCreate()` matches on key *and*
  params (`Cache.h:1017`) and so returns the same `ImagePtr` to be re-flattened
  into — a sentinel-pixel assertion detects that instead. Keying with a constant
  does not reliably make the invalidation test red either, because
  `computeHashInternal()`'s purge drops any entry of that holder whose tree
  version differs from the new hash, asynchronously — a direct assertion that
  the cached image's tree version equals the node's hash is deterministic.
  Bounds growth with stale-entry removal on a too-narrow hit was added beyond
  the brief so a later wider request cannot be served a narrower image; it ships
  untested, as does everything inside `ViewerInstance` (unreachable from ctest).

- 2026-09-11 — Two deep-cache tests fail on this host for a reason that is
  neither a regression nor a flake: free physical RAM sits under the 20%
  `unreachableRamPercent` bar, so `checkCacheFreeMemoryIsGoodEnough()` drains
  both caches on every allocation. Confirmed three ways — the implementer built
  HEAD-only and modified binaries side by side and got 8/8 failures from each;
  the arithmetic holds on this host (15735 MB x 0.20 = 3147 MB against 2990 MB
  `MemFree`); and the first failure lands on its own sanity check, before the
  code under test runs. Raised as **M18.P1.T5** and sequenced next, for the same
  reason M26 was: a suite that depends on the host's page-cache pressure makes
  every later verify in this milestone untrustworthy. Note the tests are
  *fragile*, not wrong — asserting that a cache retains an entry while the
  low-memory handler is live is the defect.

- 2026-09-11 — `renderDeepRoI` pulls its inputs over the requested `roi` rather
  than over `boundsToRender`, so after the bounds-growth path widens the render
  window the node renders wider than it pulled. Found while implementing
  M18.P2.T2, which inherits it through `renderDeepRoIFlattened`. M18.P2.T1's own
  growth test missed it because it grows bounds with the upstream still cached,
  where the extra region is covered by luck. Raised as **M18.P2.T4** rather than
  fixed in passing: it is a wrong answer on an upstream miss, and it needs a
  test that evicts between the two renders.
