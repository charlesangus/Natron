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

- [x] M18.P1.T5 — Make the deep cache tests hermetic against the low-memory handler
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

- [x] M18.P2.T4 — Pull deep inputs over the window actually being rendered
  - files: `Engine/EffectInstanceRenderDeep.cpp`, `Tests/DeepRenderPipeline_Test.cpp`
  - approach: in `renderDeepRoI`, the canonical RoI handed to the upstream pull is derived from the requested `roi`, not from `boundsToRender` (the candidate is the `canonicalRoI` computation around `EffectInstanceRenderDeep.cpp:309`). After the bounds-growth path widens `boundsToRender` to the union of the requested and cached bounds, the node therefore renders a wider window than it pulled its inputs over. With an upstream cache hit the extra region happens to be covered; on an upstream miss it is silently truncated data rather than a re-render — a wrong answer, not a slow one. `renderDeepRoIFlattened` and so the whole Viewer deep path inherit it. Found while implementing M18.P2.T2; the M18.P2.T1 tests missed it because `BoundsGrowthReRendersOverTheUnionOfRequestedRoIs` exercises growth with the upstream still cached.
  - verify: a test that grows bounds **with the upstream entry evicted between the two renders**, asserting full sample coverage over the union (not just the second RoI); driven red-then-green by reverting the fix. Whole ctest suite green.
  - size: M

- [x] M18.P2.T5 — Make the two deep cache-growth retractions symmetric
  - files: `Engine/EffectInstanceRenderDeep.cpp`, `Tests/DeepRenderPipeline_Test.cpp`
  - approach: two small cache-lifetime defects, opposite in sign, found by M18.P2.T4's audit of the growth path. (1) `renderDeepRoI()` never retracts the narrow entry it just superseded, so after growth the deep cache holds both the narrow and the wide entry under the same key and `getDeepImage()`'s list grows monotonically per key across successive widenings — dead weight until LRU, and deep frames are exactly the entries whose size justified a separate budget. `renderDeepRoIFlattened()` already does this via `removeFromNodeCache(cached)`; match it. (2) `renderDeepRoIFlattened()` removes the superseded narrow flattened entry **before** rendering, so an abort or failure during a widening flatten retracts the new entry too and leaves nothing cached where a valid narrower image existed. Retract the old entry only once the new one is complete. Neither is a wrong answer — both are cache efficiency — so do not let the fix complicate the success path; if making the ordering safe costs more than it saves, say so and close the task by recording that instead.
  - verify: after a growth render, exactly one entry remains under the key, with the grown bounds (driven red-then-green by dropping the retraction); an abort during a widening flatten leaves the pre-existing narrower image still servable (red-then-green by restoring the eager removal). Whole ctest suite green.
  - size: S

## Phase 18.3: Tier-1 node set

All nodes below are `NativeEffectBase` subclasses in `Engine/Nodes/Deep/`, one
.cpp each, registered in `loadBuiltinNodePlugins()`. The vocabulary is a cap,
not a floor (design doc, "Scope gravity") — Tier-2 is M21.

- [x] M18.P3.T1 — `DeepRead` and `DeepWrite`
  - files: `Engine/Nodes/Deep/DeepRead.cpp`, `Engine/Nodes/Deep/DeepWrite.cpp`, `Engine/CMakeLists.txt` (CMake source list + new OIIO dependency)
  - approach: `NatronEngine` does not link OpenImageIO today (`Engine/CMakeLists.txt` finds only Freetype and OpenColorIO), so this task adds `find_package(OpenImageIO CONFIG REQUIRED)` and links `OpenImageIO::OpenImageIO` — 3.1.16 with its CMake config ships in the `aswf/ci-vfxall:2027-clang21.1` image CI and `tools/ci/local/` both use, so no image change is needed. Then OIIO `DeepData` for both directions; EXR deep scanline and tiled parts. `DeepData`'s layout maps 1:1 onto `DeepImage`'s structure-of-channels, so I/O is a per-channel copy, not a transform. Deep AOVs ride the existing plane concept.
  - verify: round-trip test — read a reference deep EXR, write it back, `oiiotool --diff` clean; sample counts and Z order preserved.
  - size: M

- [x] M18.P3.T2 — `DeepMerge`, `DeepToImage`, `DeepFromImage`
  - files: `Engine/Nodes/Deep/DeepMerge.cpp`, `Engine/Nodes/Deep/DeepToImage.cpp`, `Engine/Nodes/Deep/DeepFromImage.cpp`, CMake source list
  - approach: `DeepMerge` — combine and holdout modes over M18.P1.T2's tidy/merge math, tidying lazily. `DeepToImage` — explicit mid-graph flatten (the graph always shows where information is destroyed). `DeepFromImage` — image + optional Z input → single-sample-per-pixel deep.
  - verify: merge of two deep EXRs matches Nuke-generated reference within tolerance; DeepFromImage→DeepToImage round-trip reproduces the source image.
  - size: L

- [x] M18.P3.T3a — `DeepImage` in-place aliasing, `NativeEffectBase` rewrite helper, and `DeepRecolor`
  - files: `Engine/DeepImage.h`/`.cpp`, `Engine/Nodes/NativeEffectBase.h`/`.cpp`, `Engine/Nodes/Deep/DeepRecolor.h`/`.cpp`, `Engine/AppManager.cpp` (registration), `Tests/DeepImage_Test.cpp`, `Tests/DeepNodes_Test.cpp`
  - approach: the output `DeepImage` is a cache-owned instance fixed at entry construction and `renderDeepTwoPass` always allocates fresh buffers, so COW sharing has to be in place: add `bool DeepImage::aliasContentsOf(const DeepImage& source)` (bounds must match, else false; shares the sample table, every channel and the tidy flag — the shallow-copy semantic the class comment already promises). Add `NativeEffectBase::renderDeepFromInput(args, input, channelsToWrite, alphaChannelIndex, rewrite)`: alias the input, or fall back to a `renderDeepTwoPass` copy when the cached input entry is wider than the window; `getChannelForWriting()` only `channelsToWrite`; then the pass-2 chunk/TLS/abort loop factored out of `renderDeepTwoPass` (not duplicated), handing the callback a const `DeepPixelView` of the input and a `MutableDeepPixelView` restricted to `channelsToWrite` with z/zback null so a node cannot write through aliased storage into the input's cache entry. `getSizeInBytes()` stays aliasing-blind: an aliased entry over-counts and evicts early, never desyncs. `DeepRecolor`: inputs `A` (deep) + `Color` (image, pulled as `DeepFromImage` pulls its image); per sample `rgb = color.rgb / color.a * sample.a` (0 when `color.a == 0` or outside the Color image); one `KnobBool targetInputAlpha` (default off) that rescales `a_i' = 1 - (1 - a_i)^k`, `k = log(1 - At) / log(1 - Af)`, `Af = 1 - ∏(1 - a_i)`, with the `Af ∈ {0,1}` / `At == 1` edge cases handled; `channelsToWrite` is `{R,G,B}` or `{R,G,B,A}`. Persistent message when `A` is unconnected.
  - verify: `DeepImage_Test`: `aliasContentsOf` shares table and channels, refuses mismatched bounds, and `getChannelForWriting` after aliasing detaches one channel only. `DeepNodes_Test`: synthetic deep + image source → DeepRecolor: per-sample rgb matches the formula; output `sharesSampleTableWith` / `sharesChannelStorageWith(Z, ZBack, A)` the input and not `R`; with `targetInputAlpha` the flattened alpha equals the image alpha within 1e-5 and `A` is no longer shared; a wider-than-window cached input takes the copy path with equal values. Whole ctest suite green.
  - size: M

- [x] M18.P3.T3b — `DeepCrop` (bbox, Z range, reformat toggle)
  - files: `Engine/Nodes/Deep/DeepCrop.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/DeepNodes_Test.cpp`
  - approach: one node, no separate `DeepReformat`: a resample-free reformat is only metadata (format flows from `getPreferredMetadata()`, and Natron already hijacks `kNatronParamFormatChoice/Size/Par` knobs on every node), and sample-position scaling would be a DeepTransform — Tier-2. Knobs: `bbox` (`KnobDouble` 4-dim, `setAsRectangle()`, default project format), `useBBox`, `zRange` (2-dim), `useZRange`, `reformat`. `getRegionOfDefinition` = input RoD ∩ bbox; `isIdentity` → input 0 when nothing would change (`renderDeepRoI` then shares the input's entry outright — the real zero-copy path; a zero-copy XY sub-rectangle is impossible because offsets must stay a prefix sum); otherwise a `renderDeepTwoPass` copy whose count/fill keep samples with `zmin <= Z && ZBack <= zmax`; tidy flag propagated. `getPreferredMetadata` sets the output format to the bbox when `reformat` is on. `keepOutside` and PAR editing deferred.
  - verify: crop of a synthetic 8x8 yields bounds = intersection with unchanged samples inside; Z-range drops exactly the out-of-range samples and keeps order; a no-op crop returns the input's cache entry (same `DeepImagePtr`); `reformat` sets the output format to the bbox. Whole ctest suite green.
  - size: S

- [x] M18.P3.T3c — `DeepExpression` on a self-contained per-sample evaluator (after T3a)
  - files: `Engine/Nodes/Deep/DeepExpressionEvaluator.h`/`.cpp`, `Engine/Nodes/Deep/DeepExpression.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/DeepExpressionEvaluator_Test.cpp`, `Tests/DeepNodes_Test.cpp`
  - approach: "the existing expression machinery" is the Python knob evaluator — GIL-serialised, one value per knob per frame — and nothing else is linkable (no exprtk/muParser/tinyexpr in `libs/` or the ASWF image; SeExpr is a plugin-build artifact statically linked into `IO.ofx`), so write a ~400-line recursive-descent parser → postfix op vector, compiled once per `renderDeep()`, evaluated per sample on a fixed local stack with no allocation. Grammar: float literals; identifiers resolved at compile time to any input channel by name (`R G B A Z ZBack`, AOVs incl. dotted names) or the builtins `x y sampleIndex sampleCount frame pi`; unary `- !`; `+ - * / % ^`; comparisons; `&& ||`; `?:`; parentheses; C precedence; functions `abs floor ceil round sqrt exp log pow min max clamp lerp step smoothstep sin cos tan atan2`; IEEE semantics (÷0 → inf/NaN passes through). Node: six `KnobString` expressions (`R G B A Z ZBack`), empty = pass-through (channel stays aliased); compile all at `renderDeep()` start, errors → `setPersistentMessage` with column number + `eStatusFailed`, `clearPersistentMessage` on success, never throw from the fill callback; render through `renderDeepFromInput` with `channelsToWrite` = channels with non-empty expressions, reading the input's views; untidy iff `Z`/`ZBack` is written. AOV outputs and mask inputs are Tier-2.
  - verify: evaluator unit tests (precedence, ternary, every function, column-numbered errors, unknown identifier); node test: `A*0.5` halves `A` and leaves `R/G/B/Z/ZBack` shared with the input (COW hooks); an expression on `Z` clears `isTidy`; a syntax error sets a persistent message and does not crash. Whole ctest suite green.
  - size: M

- [ ] M18.P3.T4 — Deep integration test in CI
  - files: `Tests/` (the ctest suite; the directory is capital-T `Tests/`, and M11's OFX integration test is `tools/ci/smoke_test.py` + `tools/ci/verify_plugin_loads.cpp`, not a ctest case), reference deep EXR asset under `Tests/fixtures/` (pinned, same discipline as existing test assets)
  - approach: end-to-end graph — DeepRead → DeepMerge → DeepRecolor → DeepToImage → Write — rendered headless in CI, output diffed against a committed reference. This is the deep analog of M11's OFX plugin test.
  - verify: test green in the `build-and-test` job; deliberately breaking the merge math makes it fail.
  - size: M

- [x] M18.P3.T5 — Fix the full-suite SIGSEGV in the deep cache-retraction test
  - files: `Engine/EffectInstanceRenderDeep.cpp`, `Engine/DeepImage.h`/`.cpp`, `Tests/DeepRenderPipeline_Test.cpp`, `Tests/BaseTest.cpp` (whichever the cause turns out to be in)
  - approach: `DeepRenderPipelineTest.BoundsGrowthRetractsTheSupersededDeepCacheEntry` passes its assertions and then the process dies with SIGSEGV in teardown — but only in a full `ctest -V` run, never in isolation (20/20 clean under `ctest -R`). Reproduce it first: run the full suite in a loop to get a failure rate, then run it under gdb (`test.sh` has no per-test `--gdb`, so drive `ctest -V` or the `Tests` binary directly inside `tools/ci/local/devshell.sh`) to get a real backtrace — the one seen so far goes through `libQt6Core.so.6` with no useful symbols, so build with symbols and do not accept a symbol-less trace as the diagnosis. Two hypotheses worth separating early: (a) the retraction added by M18.P2.T5 (`bddf34987`) leaves a dangling `DeepImagePtr` or double-removes a cache entry, which would make this a real product bug that a user hits on any bounds-growth render, not just a test artifact; (b) it is the shared-fixture teardown-ordering family M26 already fixed once, in which case the deep cache is merely the new thing outliving its `AppManager`. Decide which by evidence, not by whichever is cheaper to fix. If it proves to be (a), the fix belongs in the engine and the test stays as-is.
  - verify: the full ctest suite run 10+ times consecutively with zero failures (record the count in this file's `## Decisions`); the specific crash driven red-then-green if a deterministic reproducer is found. Whole ctest suite green.
  - size: M

- [x] M18.P3.T6 — Retract the deep cache entry by identity, not by key
  - files: `Engine/Cache.h`, `Engine/EffectInstanceRenderDeep.cpp`, `Tests/DeepRenderPipeline_Test.cpp`
  - approach: `Cache::removeEntry()` (`Engine/Cache.h:1533`) matches on `(*it)->getKey() == entry->getKey()` and removes the **first** bucket entry with that key. `DeepImageKey` deliberately excludes bounds, so during a bounds-growth render the narrow and the wide entry coexist under one key — which makes "first match" the wrong entry roughly whenever it matters. The abort and failure paths in `renderDeepRoI` (`EffectInstanceRenderDeep.cpp:423` and `:431`) therefore evict the *narrow* entry and leave the *half-built wide* one cached; a later request whose RoI the wide bounds contain is then served an unpopulated deep frame instead of re-rendering. That is a silent wrong answer, not a slow one. Fix the identity comparison (pointer identity, or key plus bounds) rather than special-casing the deep path — but check who else calls `removeEntry()` before changing shared behaviour, and if a by-key removal is load-bearing for another caller, add the identity-matching variant alongside it instead. Found during M18.P3.T5; `AbortDuringUpstreamRenderLeavesNothingCached` misses it because that test involves no bounds growth.
  - verify: a test that aborts a *widening* deep render and asserts nothing stale is servable afterwards — driven red-then-green against the current by-key removal. Whole ctest suite green.
  - size: M

**Verification gate:** all unit tests and the M18.P3.T4 end-to-end CI test green; deep EXR round-trip clean; Viewer flattens a deep stream with per-frame caching (second scrub pass hits cache); deep cache budget respected under a memory-pressure test; entire pre-existing ctest suite still green.

## Decisions

- 2026-09-11 — User confirmed the built-in evaluator for `DeepExpression`
  (over vendoring exprtk or deferring the node to M21). Also noted in
  M18.P3.T3a: with a deep input *unconnected*, `renderDeepRoI` answers a null
  RoD with an empty `DeepImage` + `eRenderRoIRetCodeOk` before ever reaching
  `renderDeep()`, so the per-node "input unconnected" persistent messages in
  `DeepRecolor`/`DeepFromImage`/`DeepMerge` are unreachable through the deep
  pipeline; the tests assert the reachable contract instead. Suite: 174/174.

- 2026-09-11 — M18.P3.T3 split into T3a/T3b/T3c (consultant scoping). The
  original brief's "reusing the existing expression machinery" for
  `DeepExpression` is not viable: that machinery is the Python knob evaluator,
  GIL-serialised and one-value-per-frame, unusable per sample from parallel
  render threads; no C++ expression library is linkable (none in `libs/`, none
  in the ASWF image, SeExpr is a plugin-build artifact inside `IO.ofx`), and
  vendoring exprtk is 40k lines for a v1 need. `DeepExpression` therefore gets a
  small purpose-built evaluator. `DeepCrop`/`DeepReformat` collapse into one
  `DeepCrop` with a `reformat` toggle — a resample-free reformat is only format
  metadata. COW sharing for colour-only nodes needs one `DeepImage` addition
  (`aliasContentsOf`) and one `NativeEffectBase` helper
  (`renderDeepFromInput`), because the output is a cache-owned instance the
  two-pass helper always fills with fresh buffers. Order: T3a → (T3b ∥ T3c).

- 2026-09-11 — M18.P3.T2's "Nuke-generated reference" verify was replaced: no
  Nuke reference exists in the repo or on the build host, so `DeepMerge` combine
  is checked against an independent serial front-to-back reference written in
  the test, and holdout against analytic cases (opaque/half-transparent/volume
  mattes in front, behind, straddling, coincident, hand-computed overlaps).
  Implementation choices worth knowing: holdout cuts each A sample at the
  matte's boundaries and can therefore *increase* the sample count (exact under
  the OpenEXR model rather than a per-sample approximation); combine does not
  tidy; holdout RoD is A's only; `DeepToImage` returns empty `getFramesNeeded()`
  / `getRegionsOfInterest()` so the image pre-render never calls `renderRoI()`
  on the deep input (which would cache a bogus `Image` under the hash
  `renderDeepRoIFlattened()` assumes free), and renders `eRenderSafetyFullySafe`
  so a frame is one deep pull, not per-thread tiles; `DeepFromImage` emits a
  sample for every pixel, zero alpha included, which is what makes the
  round-trip exact. COW sharing of Z/ZBack/A in holdout was not done —
  `renderDeepTwoPass` allocates fresh buffers and `DeepImage` has no API to
  alias another image's table; that is the M18.P3.T3 `DeepRecolor` verify's
  concern and may need a `DeepImage` addition there. Suite: 167/167.

- 2026-09-11 — M18.P3.T6 changed `Cache::removeEntry(EntryTypePtr)` for every
  cache, not just the deep one: every caller (`EffectInstance.cpp`,
  `EffectInstanceRenderRoI.cpp`, `EffectInstanceRenderDeep.cpp`,
  `ViewerInstance.cpp`) passes an instance obtained from the cache, so no caller
  relied on by-key matching and no by-key variant was kept. The image cache had
  the same latent defect (`ImageKey` excludes mipmap level/components/depth, so
  first-match could evict a sibling level). The rewrite also consults
  `_diskCache` when the memory bucket exists but does not hold the instance;
  the old code only looked at disk when the hash was absent from memory.
  Verified red-then-green by `AbortDuringWideningRenderRetractsTheHalfBuiltWideEntry`;
  full suite 159/159.

- 2026-09-11 — The M18.P3.T5 crash was a use-after-free in `Engine/Cache.h`'s
  teardown, not in the deep retraction path. `Cache` declared `_deleterThread`
  and `_cleanerThread` *before* `_memoryFullCondition` and the containers they
  touch, so the threads were destroyed — and therefore joined — only after those
  members were already gone; `DeleterThread::run()` ends every loop turn,
  including the one that consumes the quit sentinel, with
  `notifyMemoryDeallocated()` → `_memoryFullCondition.wakeAll()` on freed memory.
  `waitForDeleterThread()` compounded it by stopping the deleter before the
  cleaner that feeds it, so draining the cleaner restarted the deleter behind the
  back of the call meant to quiesce it. Fix: move both threads to be the last
  declared members (first destroyed), cleaner second so it stops first, and
  reverse the quit order. Evidence: valgrind found **0 errors** in the retraction
  path, ruling out a dangling `DeepImagePtr`; a gdb-injected deleter restart in
  `~Cache` reproduced `QWaitCondition::wakeAll(): mutex lock failure` 3/3 pre-fix
  and 0/3 post-fix. **Caveat: the crash was never reproduced naturally on this
  box** (0 failures in 3 sequential full runs, 2 `-j4` runs, ~80 solo runs and a
  valgrind run pre-fix), so the mechanism and its precondition are proven but the
  final link to the one observed failure is argued from the code path, not from a
  captured natural failure. The bug is real and user-visible at application exit
  regardless: any session that does a bounds-growth deep render leaves the deep
  cache's deleter thread running into `~Cache`. `bddf34987` did not introduce a
  memory bug — it introduced the only call in the suite that leaves that thread
  running, exposing a pre-existing `Cache<T>` defect.

- 2026-09-11 — Two further `Cache`/`AppManager` defects found during M18.P3.T5
  and deliberately **not** fixed there, being outside a teardown-crash fix and
  not deep-specific: (1) `AppManager::evictLRUFromMemoryCaches()` and
  `checkCacheFreeMemoryIsGoodEnough()` dereference `_imp->_nodeCache` and
  `_imp->_deepImageCache` with no null check, while `~AppManager` resets them
  before `tearDownPython()` — a late cache allocation would crash on a null
  `this` inside `QMutex::lock`, i.e. indistinguishable from the bug just fixed.
  (2) `CacheCleanerThread::run()` executes its own quit sentinel as a real
  request (a full scan matching nothing), and `DeleterThread::run()` mirrors it
  by reporting a deallocation that never happened — the very call that reached
  the freed `QWaitCondition`. Neither is deep-specific; they belong to whoever
  next owns `Engine/Cache.h`.

- 2026-09-11 — M18.P3.T5 is executed before M18.P3.T2/T3/T4, out of task order.
  Every remaining task in this phase carries "whole ctest suite green" in its
  `verify`, so an intermittent SIGSEGV in the full suite makes each of those
  verdicts unreliable — a red run could be the task under test or could be the
  flake, and telling them apart costs a re-run every time. Fixing the flake first
  makes the rest of the phase's verification trustworthy. No scope change; the
  task IDs keep their numbering.

- 2026-09-11 — `DeepRenderPipelineTest.BoundsGrowthRetractsTheSupersededDeepCacheEntry`
  segfaults intermittently during teardown, and it is a **full-suite-only** flake:
  the gtest body itself reports `[ OK ]`, then the process dies with SIGSEGV
  (backtrace through `libQt6Core.so.6`, no useful symbols) after the assertions
  pass. Run in isolation via `ctest -R` it passed **20/20**; it has failed once
  in a full `ctest -V` run and passed in another. The test is one of the two
  added by M18.P2.T5 (`bddf34987`), so the retraction path is the prime suspect,
  but the signature — clean body, crash in teardown, only when sharing a process
  with the rest of the suite — is the same family as M26's shared-fixture
  teardown flake. Tracked as M18.P3.T5 rather than folded into M18.P3.T1, whose
  scope is `DeepRead`/`DeepWrite` and which is unrelated to it. It must be closed
  before the milestone gate: CI runs the full suite, so an intermittent SIGSEGV
  there is an intermittently red PR.

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

- 2026-09-11 — M18.P1.T5 landed as `3ec2a9a4d`; suite is **148/148** green, which
  I confirmed on my own run and not only from the implementer's report. The fix
  is an RAII `DisableUnreachableRAMPurging` (`Tests/CacheMemoryPressureGuard.h`)
  pinning `unreachableRAMPercent` to 0 and restoring it; no assertion was
  weakened. Two things went beyond the brief and were right to. The guard is a
  `DeepRenderPipelineTest` **fixture member** rather than a per-test local,
  because `SecondRenderOfTheSameFrameIsACacheHit` and
  `SecondFlattenOfTheSameFrameIsACacheHit` assert retention too — they were not
  failing at this host's exact memory level, but they carry the identical hazard
  and would have started failing nondeterministically later. And the restoration
  is asserted by a test (`DisableUnreachableRAMPurgingRestoresPreviousValue`),
  not by inspection, which was worth it: it is itself non-vacuous, going red
  when the pin was neutered. The rest of `Tests/` was swept for the same hazard
  — only these two files touch the app-wide caches at all; everything else uses
  local `Cache<T>` instances, which the low-memory handler never reaches.

- 2026-09-11 — `getAmountFreePhysicalRAM()` reading `MemFree` rather than
  `MemAvailable` is a real engine defect, not a test artifact, and is raised as
  **M28** rather than fixed inside M18. Every user's caches get drained by page
  cache occupying memory that is fully reclaimable, and the eviction loop cannot
  even reach its exit condition that way — freeing cache entries returns memory
  to the allocator, not to the OS, so `MemFree` barely moves and the loop
  evicts until a cache is empty. M18.P1.T5 only stops it distorting this
  milestone's tests.

- 2026-09-11 — M18.P2.T4 landed as `c5edf95f2`; suite **149/149**, confirmed on
  my own run. The fix is one line plus a rename — `canonicalRoI` becomes
  `canonicalRenderWindow`, computed from `boundsToRender` — and the audit it
  came with is the valuable part: every other `roi` use after the divergence was
  checked and justified rather than assumed. Two are *correctly* the request and
  must stay so: the `entryImage->getBounds().contains(roi)` cache test (asking
  whether an entry satisfies the caller, and `boundsToRender` is derived from it,
  so using the window there would be circular) and the identity/pass-through
  forwards (moot anyway — they all return above `boundsToRender`'s declaration).
  `getFramesNeeded_public()` carries no render window at all, so growth being
  spatial-only cannot affect it. The red proof is the instructive part: under the
  bug the render *counts* were still 2/2, so any count-based assertion would have
  been vacuous — only the per-pixel walk catches it, and it did, with 0 samples
  where 3 were expected across the whole left half. This is the third vacuous
  assertion this milestone would have shipped with a weaker verify.

- 2026-09-11 — Growth remains unbounded: `boundsToRender.merge()` over
  everything ever requested means a pathological sequence of disjoint RoIs
  re-renders an ever-larger union. Deliberate for v1 per the design doc's
  "bounds growth, not tiling" and the comment at the merge site, and left as a
  known limitation rather than a task — a size cap is only worth designing once
  there is a real workload to size it against. Revisit in M21.

- 2026-09-11 — M18.P2.T5 landed as `bddf34987`; suite **150/150**, confirmed on
  my own run. Half the task was completed and half was **deliberately declined**,
  which was the right call. The two defects looked symmetric but are not the same
  bug: the two caches have different identity contracts, and I verified all three
  premises myself rather than accepting the report.
  `DeepImageParams::operator==` compares `_bounds` (`DeepImageParams.h:72`), so a
  narrow and a wide entry are genuinely distinct and may coexist until the wider
  render succeeds — `renderDeepRoI()` now defers its retraction to exactly there,
  so an aborted render no longer costs the entry it was replacing.
  `ImageParams::operator==` (`ImageParams.h:240`) compares RoD, components,
  bitdepth, mipmap and premult but **not bounds**, so if
  `renderDeepRoIFlattened()` deferred its removal, `getImageOrCreate()` would
  match the surviving narrow entry against the wider params and hand it back
  instead of allocating — the widening render would then write against
  narrow bounds, a worse failure than the one being fixed. Measured, not
  reasoned: the deferred version left 0 flattened entries after a
  widen-then-abort, where the eager version leaves the old one servable.
  Eager-remove-before-re-render is also the general render path's own idiom
  (`EffectInstanceRenderRoI.cpp:1010-1013`), so it is load-bearing rather than a
  local shortcut. Defect (2) is therefore **accepted, not fixed**, with a comment
  at the site so a future symmetry cleanup does not reintroduce it; fixing it for
  real means making `ImageParams` bounds-aware, which is a blast radius across
  every non-deep node and does not belong in a deep-cache cleanup. The
  complementary abort test was written, failed against the "fixed" version, and
  was then removed rather than kept — a test asserting behaviour the codebase
  deliberately does not provide is worse than no test.
