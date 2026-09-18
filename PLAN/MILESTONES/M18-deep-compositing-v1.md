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

- [x] M18.P2.T3 — Viewer per-sample probe plumbing (Gui half)
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

- [x] M18.P3.T4 — Deep integration test in CI
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

- [x] M18.P3.T7 — Fix `DeepRead` output format defaulting to the project format instead of the file's
  - files: `Engine/Nodes/Deep/DeepRead.h`, `Engine/Nodes/Deep/DeepRead.cpp`, `Tests/DeepReadWrite_Test.cpp`
  - approach: add `getPreferredMetadata(NodeMetadata&)` (declared `OVERRIDE FINAL WARN_UNUSED_RETURN` in `DeepRead.h` beside `getRegionOfDefinition`), mirroring `DeepCrop.cpp`'s `metadata.setOutputFormat(RectI)` pattern (`DeepCrop.cpp:203-215`) rather than inventing a new one. `DeepRead` currently overrides `getRegionOfDefinition()` (correct — mirrors the data window into Natron's RoD) but never touches the output format, so it inherits the project's default HD format regardless of the file; on the small fixture EXRs this makes the format visibly wrong even though the RoD is right. Open the file the same way `getRegionOfDefinition` does (OIIO `ImageInput::open()`/`spec()`), but build the format `RectI` from the *display* window (`spec.full_x`/`full_y`/`full_width`/`full_height`), mirrored top-to-bottom the same way the RoD's data window already is — for these fixtures data window == display window so the two rects coincide, but display window is the semantically correct source for format regardless. Leave the format untouched (falls through to the default) if the file can't be opened; `getRegionOfDefinition`/`renderDeep` already report that error, no need to duplicate it here.
  - verify: a `DeepReadWrite_Test.cpp` case pointing `DeepRead` at a small (few-pixel) fixture and asserting `EffectInstance::getOutputFormat()` matches the file's dimensions, not the project's default HD format; driven red-then-green by temporarily reverting the override. Whole ctest suite green.
  - size: S

- [x] M18.P3.T8a — Root `NativeEffectBase` at `OutputEffectInstance` and let the plugin description declare writer-ness
  - files: `Engine/Nodes/NativeEffectBase.h`/`.cpp`, `Engine/Nodes/Deep/DeepWrite.cpp`, `Tests/DeepReadWrite_Test.cpp`
  - approach: user-reported and confirmed real: `DeepWrite::writeDeepImage()`'s OIIO logic is correct, but `DeepWrite` is a plain `NativeEffectBase : public EffectInstance`, never an `OutputEffectInstance`, so every real writer entry point (GUI Render menu, CLI `-w`, Python `app.render()`; see M18.P3.T8c) rejects it via `dynamic_cast<OutputEffectInstance*>`, and `isWriter()`/`isOutput()` both default false. The fix is to the framework, not the node: `OutputEffectInstance` (`Engine/OutputEffectInstance.h`) is not "a writer", it is "an effect that can own a `RenderEngine`" — `OfxEffectInstance`, `NodeGroup`, `NoOpBase`, `DiskCacheNode` and `ViewerInstance` all derive from it and each overrides `isOutput()` to say whether the instance really is a render root (`OfxEffectInstance::isOutput()` returns a flag set from the plugin's context). M17 rooting `NativeEffectBase` at `EffectInstance` put native nodes alongside `RotoPaint`/`PrecompNode`/`OneView`, internal helpers that can never be render roots; that is the actual defect, and it would hit `WriteScene` (M20.P3.T2) identically. Change `NativeEffectBase` to derive `OutputEffectInstance`; add `bool isWriter` (default false) to `NativePluginDescription` and override `NativeEffectBase::isOutput()`/`isWriter()` to return it, so a native node's writer-ness is declared in its description exactly as an OFX plugin's is in its context; `DeepWrite`'s description sets it. No helper extraction, no sibling base class, `renderDeepTwoPass()` stays where it is. The engine is cheap — `RenderEngine::RenderEngine` only connects a signal and the scheduler thread is created lazily on the first `renderFrameRange`/`renderCurrentFrame` — and every non-writer already carries one (each OFX Blur is an `OutputEffectInstance`). Implementation checks: `OutputEffectInstance::initializeData()` is `FINAL` and calls `createRenderEngine()`, which does `shared_from_this()` — confirm native nodes are constructed through the same `Node::load` sequence so that holds; `OutputEffectInstance` has a protected copy ctor `NativeEffectBase` must not fall through to accidentally.
  - verify: existing `DeepReadWrite_Test.cpp` and `DeepNodes_Test.cpp` cases (calling `renderDeepRoI()` directly) still pass unchanged; new assertions that `dynamic_cast<OutputEffectInstance*>(deepWriteNode->getEffectInstance().get())` succeeds with `isWriter()`/`isOutput()` both true, and that a non-writer native node (`DeepRecolor`) has `isOutput() == false`. Whole ctest suite green.
  - size: S

- [x] M18.P3.T8b — Dispatch the render scheduler on the writer's output data kind
  - files: `Engine/OutputSchedulerThread.cpp`, `Engine/EffectInstance.h`/`.cpp`
  - approach: depends on M18.P3.T8a landing (needs a real `OutputEffectInstance`-derived deep writer to drive). `DefaultScheduler` is already kind-agnostic everywhere except two call sites: `DefaultRenderFrameRunnable::renderFrame` (`OutputSchedulerThread.cpp:~2348`) and `DefaultScheduler::processFrame` (`~2471`) both hard-code `renderRoI()` (2D `Image`); threading, abort, buffering (`BufferableObject` has no image-specific members), progress and the Python frame callbacks never look inside the frame. Do **not** add a `DeepScheduler` sibling or a per-node `createRenderEngine()` override — that forks the scheduler per data kind and `WriteScene` would need a third. Instead, at those two sites, branch on `activeInputToRender->getOutputDataKind()`: `eDataKindImage` keeps the exact existing `renderRoI` block; `eDataKindDeep` builds a `RenderDeepRoIArgs` over the same RoD/scale-1 render window and calls `renderDeepRoI()`; any other kind fails the render with a clear "no scheduler support for this output kind yet" message (the hook M19/M20 fill in for scene). The writer's own `renderDeep()` performs the file write — exactly how OFX writers work today (`renderRoI` on the writer runs the plugin's render action, which writes) — so the scheduler never calls `writeDeepImage()` or knows a deep writer from a deep pass-through. Factor the deep branch's RoD/hash/args setup so it reads like the image branch, not a copy of it; if that pushes toward a single `EffectInstance::renderOutputFrame(kind, …)` helper that both branches call, that is fine but not required.
  - verify: an integration test that drives a `DeepWrite` node through `OutputEffectInstance::renderFullSequence()` (not a direct `renderDeepRoI()` call) over a small frame range, asserting the files land on disk with correct content and that abort is honored mid-sequence; the existing 2D path is provably untouched (an existing 2D Write ctest case still green, and the image branch's code is byte-identical apart from the enclosing `switch`). Whole ctest suite green.
  - size: M

- [x] M18.P3.T8c — Wire up and prove the GUI, CLI, and Python entry points
  - files: `Engine/AppInstance.cpp`, `Tests/DeepReadWrite_Test.cpp`
  - approach: depends on M18.P3.T8a/T8b. Once `DeepWrite` is a real `isWriter()`/`OutputEffectInstance` with a working deep scheduler, the GUI Render menu (`Gui::renderSelectedNode()`/`renderAllWriters()`, `Gui/Gui40.cpp`) and the Python `App::render()`/`renderInternal()` (`Engine/PyAppInstance.cpp:~323/~370`) need **no changes** — they already operate generically off `OutputEffectInstance*`/`isWriter()`/`isOutput()`. The CLI `-w` path (`AppInstance::getWritersWorkForCL()`, `Engine/AppInstance.cpp:452-521`) has one deep-specific check to make: its output-filename-knob override (lines 473-481) looks up a `KnobOutputFile` named `kOfxImageEffectFileParamName` — confirm `DeepWrite`'s file knob (named `"filename"` in `initializeKnobs()`) matches that constant, and if not, add the lookup-by-name fallback needed for a deep writer. The CLI's `mustCreate` auto-node-creation branch (lines 482-505, creates a `PLUGINID_NATRON_WRITE` node when the named writer doesn't exist) is explicitly out of scope: a deep graph must already contain an explicit `DeepWrite`.
  - verify: a `Tests/DeepReadWrite_Test.cpp` (or `tools/ci/`, matching wherever T8b's integration test landed) case driving the CLI path (`AppInstance::getWritersWorkForCL()` + `startWritersRendering()`) and the Python `App::render()` binding, each asserting a file lands on disk — this is the test that would have caught the original defect, since the pre-existing round-trip test only ever called `renderDeepRoI()` directly. Manual GUI checklist addition (this milestone already has one pending for M18.P2.T3; add to it rather than opening a second): right-click a `DeepWrite` node → Render, confirm the file is written and the node's progress/abort UI behaves like an ordinary Write node. Whole ctest suite green.
  - size: M

## Phase 18.4: Post-checklist fixes and glyph rollback

- [ ] M18.P4.T1 — `DeepFromImage`: don't create samples for zero-alpha pixels
  - files: `Engine/Nodes/Deep/DeepFromImage.cpp`, `Tests/DeepNodes_Test.cpp`
  - approach: in `DeepFromImage::renderDeep` (`DeepFromImage.cpp:180`), the count-pass lambda `[&sourceBounds](int x, int y) -> U32 { return sourceBounds.contains(x, y) ? 1 : 0; }` creates a sample for every in-bounds pixel regardless of alpha, so fully-transparent source pixels still produce a (zero-alpha) deep sample. Capture `sourceAccess` in that lambda too and return 0 when the source pixel's alpha channel (index 3 of RGBA) is `<= 0.f`; leave the fill lambda unchanged, since `renderDeepTwoPass` only invokes it for pixels the count lambda already approved (`NativeEffectBase.cpp:276-281`). Add a regression test near `DeepFromImageThenDeepToImageReproducesTheImageAtAConstantDepth` (`Tests/DeepNodes_Test.cpp:1130`) using a source image with a mix of zero- and nonzero-alpha pixels (e.g. a small `DeepSyntheticSource`-style fixture or a per-pixel-pattern extension of `createImageSource`), asserting zero-alpha pixels get zero samples and nonzero-alpha pixels keep exactly one.
  - verify: new test fails against the current code (asserts 0 samples, gets 1), passes after the fix; full ctest suite green.
  - size: M

- [ ] M18.P4.T2 — `DeepCrop`: make the Reformat/Bbox knobs actually refresh the output format
  - files: `Engine/Nodes/Deep/DeepCrop.cpp`, `Tests/DeepNodes_Test.cpp`
  - approach: same root cause `c2bd4da27` fixed for `DeepRead`'s `filename` knob — `NativeEffectBase` has none of the OFX host's automatic "re-run getClipPreferences after a slave param changes" wiring, so `DeepCrop::getPreferredMetadata()` (`DeepCrop.cpp:204`) is only ever consulted once, at node creation, unless the driving knob is marked `setIsMetadataSlave(true)`. `DeepCrop::initializeKnobs()` (`DeepCrop.cpp:69-113`) never marks `reformat` or `bbox` (the format computed when `reformat` is on depends on `bbox`) as metadata slaves. Mark both the same way `DeepRead::_filename` was marked. Add a test mirroring `DeepRead`'s `OutputFormatRefreshesWhenTheFileKnobChangesWithNoExplicitRefresh` case: create a `DeepCrop`, render once with `reformat` off, then toggle `reformat` (and/or edit `bbox`) with no explicit refresh call, and assert the output format changes to match `bbox`.
  - verify: new test fails before the fix (format stays stale after the knob change), passes after; full ctest suite green.
  - size: S

- [ ] M18.P4.T3 — Remove the input-kind glyph (dot/diamond) drawn on dangling input pipes
  - files: `Gui/NodeGui.cpp`, `Gui/NodeGui.h`
  - approach: M17 (`8bb4403f2`) added `NodeGui::createInputKindGlyphItem()` and `refreshInputKindGlyphs()` (`NodeGui.cpp:1654-1712`), which draw a filled dot (Deep) or diamond (Scene) at the free end of an unconnected input arrow, tinted via `kindTintColor()`. Remove this glyph machinery entirely — the member(s) storing the glyph items, both functions, and their call sites (node construction, input-count changes, wherever `refreshInputKindGlyphs()` is invoked) — so a dangling input arrow renders exactly as it did before M17. Do not touch `kindTintColor()`, `kindSilhouetteCornerRadiusPx()`, or `Edge::refreshDataKindPen()` (`Edge.cpp:818-835`, the connected-edge pen styling) — those are separate M17 features not in scope here.
  - verify: build, then an Xvfb GUI check (per this project's Xvfb-GUI practice): create a `DeepRead` node and confirm its dangling input arrow shows no dot; ctest suite green.
  - size: S

- [ ] M18.P4.T4 — Remove the tinted rectangular fill drawn behind a node's body
  - files: `Gui/NodeGui.cpp`
  - approach: in `NodeGui::paint()` (`NodeGui.cpp:2336-2368`), M17 added a `painter->drawRect(bbox)` filled with `kindTintColor()`'s tint behind the node, plus a per-kind corner radius on `_boundingBox` sized to hide most of that fill in the node's rounded corners. Remove the tint-fill `drawRect()` call and the corner-radius-from-kind logic together (`_boundingBox`'s corner radius reverts to its pre-M17 default), so a node's silhouette paints exactly as it did before M17. Leave `kindTintColor()` itself alone — `Edge::refreshDataKindPen()` still uses it.
  - verify: Xvfb GUI check: a Deep-kind node (e.g. `DeepRead`) shows no colored fill or corner rounding behind its body; ctest suite green (Gui-only change, no ctest coverage expected to move).
  - size: S

**Verification gate:** all unit tests and the M18.P3.T4 end-to-end CI test green; deep EXR round-trip clean; Viewer flattens a deep stream with per-frame caching (second scrub pass hits cache); deep cache budget respected under a memory-pressure test; `DeepWrite` actually writes a file through the GUI Render menu, the CLI `-w` flag, and the Python `app.render()` binding (not just direct `renderDeepRoI()` calls in a test); `DeepFromImage` never creates a zero-alpha sample; `DeepCrop`'s Reformat/Bbox knobs update the output format with no explicit refresh; the node graph shows no per-kind input-pipe dot and no tinted rectangle behind a node's body; entire ctest suite still green.

## Decisions

- 2026-09-17 — **Manual GUI checklist run by the user: items 1, 2, 4, 5
  pass; items 3 and 6 failed**, both fixed in the same commit.
  - Item 3 (tooltip): the per-sample list was only reachable as the
    info-bar label's tooltip, and the label is cleared the moment the mouse
    leaves the image on its way to the bar — so the affordance was
    unreachable by construction. `ViewerGL::event()` now handles
    `QEvent::ToolTip`: hover a pixel of a deep image, pause, and the same
    list pops up over the pixel (`InfoViewerWidget::getDeepSamplesToolTip()`
    hands it over). Verified under Xvfb by synthesising a mouse-move plus
    `QHelpEvent` over `big-deep.exr`: screenshot
    `build/deeprepro/shot-tooltip.png` shows the `Z / ZBack / A / R G B`
    table over the hovered pixel, bar reading `deep: 1 smp`.
  - Item 6 ("stays on Queued, knobs greyed"): `DeepWrite::writeDeepImage`
    sized OIIO's `DeepData` with `set_samples()` per pixel, which rewrites
    OIIO's cumulative-capacity table for every pixel after the one being
    set — quadratic in the pixel count. A 640×360 frame took **15 s** to
    write (the ctest fixtures are a few pixels wide, so never noticed), and
    the progress panel shows "Queued" until the first frame lands while the
    scheduler has already frozen the knobs; re-triggering Render while that
    ran queued the new run behind the un-abortable write. Reproduced under
    Xvfb via `build/deeprepro/gui_deepwrite.py` (`app.render()` twice 0.3 s
    apart on `big-deep.exr`, 250 frames): 3 frames written then stuck 40 s+,
    two "Parallel render" threads in `DeepData::insert_samples`. Fixed with
    one `set_all_samples()` call: **15 s → 1 s** per frame, sample-exact
    round trip (0 mismatches vs source), and the re-trigger scenario now
    completes all 250 frames in ~10 s, same as an ordinary Write. New ctest
    `WritesAFrameSizedImageInLinearTime` (SeNoise → DeepFromImage →
    DeepWrite over the project format, 60 s bound, read-back sample counts
    equal) runs in ~1 s on the fix and does not finish within 150 s on the
    old code. Suite green.

- 2026-09-15 — M18.P3.T8c landed as `1119c937d`. The brief's one
  hypothesised gap was not real: `DeepWrite`'s file knob is `"filename"`
  and so is `kOfxImageEffectFileParamName`, so the CLI `-w <name> <file>`
  override in `getWritersWorkForCL()`, the `-i` override and
  `getSequenceNameFromWriter()` already resolve it. Every entry point read
  end to end — `CLArgs.cpp` parsing, `startWritersRendering[FromNames]`,
  `NodeCollection::getWriters`, `PyAppInstance.cpp` `App::render`, and
  `Gui40.cpp` `renderAllWriters()`/`renderSelectedNode()` — is generic over
  `OutputEffectInstance*`/`isWriter()` with no plugin-ID or knob-name
  filter, so after T8a/T8b all three work for `DeepWrite` unchanged. Only
  production edit: `getWritersWorkForCL()` made public so the CLI path is
  testable directly. Tests: ctest
  `CLIWriterArgOverridesTheFileKnobAndRendersThroughGetWritersWorkForCL`
  (asserts the override lands on the `KnobOutputFile` and the overridden
  file is written), and a new `check_deep_write_render()` in
  `tools/ci/smoke_test.py` driving `app.render()` on DeepRead → DeepWrite
  through the built `NatronRenderer` (the Python binding is not reachable
  from the `Tests` binary). Suite 205/205; smoke passes. The GUI half is
  item 6 of the manual checklist below.

- 2026-09-15 — M18.P3.T8b landed as `334abaf3d`. Both `DefaultScheduler`
  sites (`DefaultRenderFrameRunnable::renderFrame`,
  `DefaultScheduler::processFrame`) now `switch` on `getOutputDataKind()`;
  everything kind-agnostic (RoD, hash, `ParallelRenderArgsSetter`,
  request pass, abort/failure handling, `notifyFrameRendered`) stays
  shared, the image-only component/bit-depth pre-work is guarded, and the
  deep case is ~10 lines per site — no `renderOutputFrame` helper needed.
  `renderDeepRoI` reads hash and abort state from the TLS frame args the
  setter installs, so no extra TLS setup. `default:` fails with "No
  scheduler support for the output data kind of <node> yet" — the hook a
  scene scheduler fills. Findings: (1) the image branch is
  whitespace-normalised, not byte-identical — re-indenting made the
  clang-format gate touch it, no token changed; (2) `processFrame` is dead
  for `DefaultScheduler` (`renderFrame` reports FFA and never
  `appendToBuffer`), dispatched anyway; `DeepImage` is not a
  `BufferableObject` so its deep case re-pulls through the deep cache;
  (3) **latent debug-only SIGFPE in `DeepWrite`**: OIIO 3.1's first EXR
  `open()` builds `ColorConfig::default_colorconfig()`, whose OCIO probing
  raises `FE_INVALID`; OFX plugins are shielded by
  `OfxImageEffectInstance::mainEntry`'s `exception_trapping trap(0)`, a
  native node's `renderDeep` is not — fixed with the same `#ifdef DEBUG`
  idiom in `writeDeepImage()`. This would have crashed any debug
  `Natron`/`NatronRenderer` rendering a `DeepWrite`, and is the same
  family as M25. Tests: `DeepWriteRendersASequenceThroughTheRenderScheduler`
  (DeepRead → DeepWrite via `startWritersRendering(true)` over frames 1–3;
  files exist, `oiiotool --diff` 0, re-read matches) and
  `DeepWriteSequenceStopsWhereTheRenderIsAborted` (abort after frame 1;
  frames 2–3 absent; `numberOfParallelRenders` pinned to 1; 60/60 under
  repeat). Red-then-green real: pre-change `renderRoI` on the writer
  returned OK with no planes and no files. Existing 2D scheduler cases
  (`BaseTest.GenerateDot`, the M18.P3.T4 pipeline test) green. Suite
  204/204.

- 2026-09-15 — M18.P3.T8a landed as `603de39c0`: `NativeEffectBase` now
  derives `OutputEffectInstance`; `NativePluginDescription::isWriter`
  (default false) drives `isWriter()`/`isOutput()` (`OVERRIDE FINAL`);
  `DeepWrite` sets it. Every implementation check in the brief held:
  `Node::load` owns the effect by `shared_ptr` before `initializeData()`, so
  `createRenderEngine()`'s `shared_from_this()` is safe; no native node
  overrides `initializeData()`; the protected copy ctor is never reached
  (`createRenderClone()` is not overridden by any native node).
  `Engine/Nodes/README.md` updated. Two new tests
  (`DeepWriteIsARenderRootOutputEffectInstance`,
  `DeepRecolorIsNotAnOutputNode`), the first driven red-then-green by
  flipping `desc.isWriter`. Suite 202/202.

- 2026-09-15 — User reported `DeepWrite` "does not appear to have any way to
  actually write a file." Confirmed real: `writeDeepImage()`'s OIIO logic is
  correct, but `DeepWrite` never overrides `EffectInstance::isWriter()` and is
  a plain `NativeEffectBase`, not an `OutputEffectInstance` — so every real
  writer entry point (GUI Render menu, CLI `-w`, Python `app.render()`)
  rejects it via `dynamic_cast<OutputEffectInstance*>`, and even a successful
  cast wouldn't help because `OutputSchedulerThread`'s frame-pulling core
  only calls `renderRoI()` (2D `Image`), with no notion of `renderDeep()` at
  all. `Tests/DeepReadWrite_Test.cpp`'s round-trip test calls
  `renderDeepRoI()` directly on the node, bypassing every one of those entry
  points — the same "ctest pass proves nothing about the real invocation
  chain" failure mode already hit once this milestone with `DeepRead`
  (M18.P3.T7). User chose full parity (GUI + CLI + Python, not a GUI-only
  v1) as the fix scope. Raised and sequenced as **M18.P3.T8a/T8b/T8c**,
  before the gate — split immediately (rather than as a single T8, per the
  M18.P3.T3 precedent) because the class-hierarchy fix, the scheduler's
  deep-frame-pulling branch, and the three entry points are each a coherent
  chunk with its own verify, and together would have been an 8+ file task.
  The board's `# Open questions` manual-GUI-checklist item, already
  blocking the gate for M18.P2.T3, now also covers T8c's GUI check rather
  than opening a second one.
- 2026-09-15 — User questioned the shape of T8a/T8b (extract the two-pass
  helper, give `DeepWrite` a separate `OutputEffectInstance`-derived base,
  add a `DeepScheduler` sibling with a per-node `createRenderEngine()`
  override): it changed the node rather than the write machinery, and
  `WriteScene` (M20.P3.T2) would need a third base and a third scheduler.
  The original plan's premise was wrong: it claimed deriving
  `NativeEffectBase` from `OutputEffectInstance` "would make every deep node
  a writer", but `OutputEffectInstance` is the render-engine owner, not a
  writer marker — every `OfxEffectInstance` (Blur included), `NodeGroup`,
  `NoOpBase` and `DiskCacheNode` already derives from it and gates
  writer-ness on an `isOutput()` override. The reason it is spread so wide
  is historical: OFX plugins are one host class whose writer-ness is only
  known per instance (plugin context), and C++ picks a base per class, so
  the engine slot had to go on every OFX instance; `NodeGroup` follows
  because `WriteNode : NodeGroup`. The engine is cheap and lazy (thread
  created on first render request), so the cost is one null pointer per
  node. The principled fix in this codebase is therefore to move
  `NativeEffectBase` to the same base and declare writer-ness in
  `NativePluginDescription`, and to make `DefaultScheduler`'s two
  `renderRoI` call sites dispatch on `getOutputDataKind()` — one generic
  seam that `WriteScene` reuses. T8a/T8b rewritten accordingly (T8c
  unchanged). The cleaner long-term shape is composition — `Node` owns an
  optional `RenderEngine` created iff `effect->isOutput()`, and
  `OutputEffectInstance` goes away — but that touches ~28 files across
  Engine/Gui and is a refactor in its own right; filed as **M31.P1.T1** in
  the deferred architectural-cleanup milestone. Nothing in T8a/T8b
  conflicts with doing it afterwards.

- 2026-09-15 — `34ce1f926` was verified only against the ctest suite, which
  calls `refreshMetadata_public()` manually in test setup, and the user found
  the bug still fully reproduced in the built AppImage. Root cause:
  `NativeEffectBase` has none of the OFX host's automatic "re-run
  `getClipPreferences` after any param the plugin declared via
  `addClipPreferencesSlaveParam`" wiring (`Engine/OfxImageEffectInstance.cpp:690`),
  so with no input to trigger `Node::refreshAllInputRelatedData()`,
  `getPreferredMetadata()` was only ever consulted once, at node creation,
  before a file is chosen — the RoD looked right because it is computed live
  on every render, while the format is cached `NodeMetadata` state. Fixed in
  `c2bd4da27` by marking the filename knob `setIsMetadataSlave(true)` in
  `initializeKnobs()`, the same mechanism the *actual* native 2D Read node's
  embedded OFX plugin gets for free. Also corrected the format rect itself
  (`34ce1f926`'s version preserved `full_x` as the origin; OFX formats must
  start at `(0, 0)` per `ReadOIIO::getFrameBounds`'s `specFormat` — no
  observable difference on these fixtures since their `full_x`/`full_y` are
  0, but wrong for a file with a nonzero display-window origin). New test
  `OutputFormatRefreshesWhenTheFileKnobChangesWithNoExplicitRefresh`
  reproduces the real bug (no manual refresh call) and was driven
  red-then-green. Full ctest suite **200/200**. Verified a second way this
  time, not just ctest: visually, through Xvfb
  (`build/deeprepro/check_format.py` against a freshly built
  `build/release/App/Natron`) — format reads `0 0 1920 1080` before the file
  is set and `0 0 640 360` immediately after, Viewer info bar shows
  "640x360", screenshot captured. **Lesson for this milestone: a ctest pass
  is not sufficient proof for anything that depends on the knob-change →
  evaluate → metadata-refresh chain — that chain is largely untested by the
  suite's own `refreshMetadata_public()`-calling helpers, and needs an
  Xvfb/GUI check before being called done.**

- 2026-09-15 — M18.P3.T7 landed as `34ce1f926`; full ctest suite **199/199**
  passing (confirmed by the implementer's own run, `OSGLContext.Basic` and
  `GPUContextPool.Basic` are the two pre-existing environment-disabled
  GPU-context tests, unaffected). `getPreferredMetadata()` derives the output
  format from the EXR's display window (`full_x/full_y/full_width/full_height`),
  mirrored top-to-bottom the same way `getRegionOfDefinition()`'s data-window
  RoD already is — mirroring the *whole* display window within itself always
  normalizes its y-origin to 0 regardless of `full_y` (the offset cancels),
  while x preserves any horizontal offset via `full_x`, since only the
  vertical axis is flipped between EXR and Natron conventions. On open
  failure the metadata is left untouched so it falls through to the project
  default, since `getRegionOfDefinition()`/`renderDeep()` already own error
  reporting for that case. New test
  `OutputFormatIsTheFilesDisplayWindowNotTheProjectDefault` driven
  red-then-green (failed with the override stubbed to a no-op, passed with
  the real implementation).
  Landed on top of an unrelated pre-existing WIP found uncommitted in the
  working tree at session start — mipmap-level subsampling support for
  `DeepRead::renderDeep()`'s `filePixelIndex` plus a `NativeEffectBase`
  `supportsRenderScaleMaybe` default fix — which the user asked to be
  committed first, as its own commit (`da8370e7f`), verified by a full green
  ctest run before T7 was implemented on top of a clean tree.

- 2026-09-15 — User found `DeepRead` sets the output format to the project's
  default HD format rather than the file's, on small test deep EXRs, even
  though the RoD (`getRegionOfDefinition`) is correct. Raised as **M18.P3.T7**
  and sequenced before the gate rather than folded into M18.P3.T1: the format
  is user-visible on any deep source smaller than the project default, and
  `DeepCrop` (M18.P3.T3b) already has the `getPreferredMetadata` pattern to
  mirror.

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

- 2026-09-11 — M18.P3.T4 landed as `e5e00d68c`; suite **197/197**, confirmed on
  my own run. The test compares against an oracle computed in the test from the
  fixtures' documented samples rather than a committed flat reference: the new
  `deep-interleaved.exr` fixture was laid out so no depth range overlaps
  `deep-scanline.exr`'s in the same pixel, which makes a plain front-to-back
  over exact and keeps the engine's tidy/flatten code out of the expected side.
  Red-then-green was done twice — halving the transmittance step in
  `flattenFrontToBack` (36 failures, the nine multi-sample pixels × RGBA) and
  dropping input B in `DeepMerge::renderCombine` (24 failures, exactly the six
  pixels B contributes to). Known blind spot, inherent to the briefed graph:
  `DeepRecolor` sets every sample's RGB to colour × alpha, so the flattened RGB
  is colour × flattened alpha and a sample-*ordering* bug in the merge is
  invisible here; ordering is covered by `DeepNodes_Test.cpp`. Also noted, not
  fixed: the Tests binary exits with an OIIO "pending error message …
  ImageOutput::create() called with no filename" whenever a WriteOIIO node is
  created — pre-existing, reproducible with `BaseTest.RenderFrameRange*` alone.
  The comment-policy checker flags line 2 of the mandatory GPL header on every
  *new* Natron source file ("restates what the code is"); the gate was passed
  with that as the only finding, on the grounds that the licence block is not
  a comment the policy governs.

- 2026-09-11 — User confirmed they will run M18.P2.T3's manual Viewer checklist
  themselves (over shipping on build+review only, or deferring the Gui probe to
  M21). T3 is therefore implemented and built here, then handed over with the
  exact checklist; its result is recorded in this section before the gate.

- 2026-09-11 — **Handoff (system reboot mid-task).** M18.P2.T3's code is
  written but **uncommitted and unverified** in the working tree — 13 modified
  files: `Engine/EffectInstance.h`, `Engine/EffectInstanceRenderDeep.cpp`,
  `Engine/EngineFwd.h`, `Engine/FrameEntry.h`, `Engine/FrameParams.h`,
  `Engine/OpenGLViewerI.h`, `Engine/UpdateViewerParams.h`,
  `Engine/ViewerInstance.cpp`, `Gui/InfoViewerWidget.{h,cpp}`,
  `Gui/ViewerGL.{h,cpp}`, `Gui/ViewerGLPrivate.h`. The implementer's rebuild
  (`tools/ci/local/build.sh debug`, log at `/tmp/m18-t3-build.log`) was at
  173/388 when the box went down; `build/debug/App/Natron` still dates from
  21:28, i.e. pre-T3. The implementer never reached its report, so nothing about
  the change has been checked by anyone. **Pick up:** (1) `git status` should
  show exactly those 13 files and nothing else — if the tree is clean the WIP
  was lost and T3 must be re-delegated from its brief; (2) run
  `tools/ci/local/build.sh debug` in the foreground (it resumes incrementally);
  (3) run the whole ctest suite (was 197/197 at `e5e00d68c`); (4) run
  `git clang-format ... --diff HEAD -- Engine/ Gui/` and
  `check-comments.py --files` on the 13 files; (5) have a reviewer-grade look at
  the diff against the T3 brief (payload cleared wherever `lastRenderedTiles`
  is cleared; null passed on the image path; no mipmap fallback in
  `getDeepSamplesAt`; raw untidied samples; same mutex as `lastRenderedTiles`)
  before committing — nobody has; (6) commit as T3, then write the manual
  checklist from the `deep-scanline.exr` sample table in
  `Tests/DeepReadWrite_Test.cpp` (name one multi-sample pixel with its count /
  Z range / values and one empty pixel) and hand it to the user, who has agreed
  to run it; record the result here before the gate. Background `until`-loops
  waiting on the build were killed twice by the harness for "low memory" — wait
  in foreground chunks with `timeout`, or just re-run `build.sh` and let it
  finish.

- 2026-09-12 — **T3 resumed and closed out.** The 13-file WIP survived the
  reboot intact. The Docker content store was corrupted by the reboot
  (`natron-dev` image's layers were unreadable, then `aswf/ci-vfxall` itself
  came back with a missing blob) — fixed by `docker pull
  aswf/ci-vfxall:2027-clang21.1` followed by rebuilding `natron-dev`; not a
  code issue. Incremental debug build then found nothing to do (the
  implementer's build had actually finished, at 22:26, after the 21:44/21:48
  edits — the "173/388, pre-T3 binary" note in the prior entry was a stale
  snapshot taken before that run completed). Full ctest suite: 197/197.
  `git clang-format --diff main` (pip `clang-format==21.1.8`, `--user
  --break-system-packages`) and `check-comments.py`: both clean.
  A reviewer-grade consultant pass (opus) against the T3 brief then found no
  brief-compliance defects but flagged 5 issues, of which 4 were fixed
  (sonnet implementer, then rebuilt/retested/reformatted/re-gated, all green
  again) before committing as `119005b64`:
  - **Perf regression (fixed):** the viewer's flat-image-cache-hit path in
    `renderDeepRoIFlattened()` was unconditionally calling `renderDeepRoI()`
    to populate the probe's deep payload, so any refresh where the *deep*
    cache entry had been evicted (they lose the eviction race — they're
    large) triggered a full synchronous deep re-render nobody asked for, on
    the strength of a texture-cache-hit render alone. Replaced with a
    cache-only peek (new file-local `lookupCachedDeepImage()`) that leaves
    the payload null on a miss rather than rendering — consistent with the
    probe's existing "no fallback, show nothing" rule for the wrong-mipmap
    case.
  - **Stale-payload risk (fixed):** `setLastRenderedDeepImage()` was called
    whenever the frame wasn't partial, even when the color payload
    (`originalImage`) was null and `endTransferBufferFromRAMToGPU` therefore
    left the *previous* color texture in place — so an expired deep
    weak-ref on that path could wipe a still-valid previous deep payload out
    from under an otherwise-unchanged display. Now gated on `originalImage`
    too, matching the color path's own condition.
  - **Stale label (fixed):** the deep-probe label wasn't cleared when the
    cursor left the valid probe area unless the color label happened to
    already be visible (three sites in `ViewerGL.cpp`) — `hideDeepInfo()`
    is now called unconditionally alongside the existing conditional
    `hideColorInfo()` at each.
  - **Qt string bug (fixed):** `InfoViewerWidget.cpp`'s tooltip build used
    chained `.arg(header).arg(rows)`, which re-scans the substituted string
    for the next placeholder — a channel literally named `%2` would corrupt
    the tooltip. Changed to the two-argument `.arg(header, rows)` form.
  - **Not fixed, accepted as scope:** the probe indexes by
    `getMipmapLevelCombinedToZoomFactor()`, which doesn't know about
    auto-proxy, so a draft-mode frame at `mipmapLevelWithDraft` shows a dash
    even though a same-resolution deep image would satisfy it. Consistent
    with the deliberate no-mipmap-fallback rule; the user-visible scope is
    wider than "not rendered yet" but not wrong. Left for a future task if
    it proves to matter in practice.

  **Manual Viewer checklist**, handed to the user per the 2026-09-11 decision
  above (built binary: `build/debug/App/Natron`, this commit):
  1. Connect `DeepRead` (pointed at `Tests/fixtures/deep-scanline.exr`)
     directly to the Viewer, no `DeepToImage` in between. The image should
     display.
  2. Scrub the timeline forward one frame then back to the original frame;
     confirm the second pass does not re-render (render counter, or launch
     with `--enable-render-stats` and check the second pass logs a cache
     hit, not a render).
  3. Hover pixel **(3, 0)** (a multi-sample pixel): the info-bar label
     should read `deep: 3 smp  Z 1.00–5.50`; pausing the mouse over the
     pixel pops up a tooltip listing all three samples — `Z=1 ZBack=1 A=1 R=1 G=0 B=0`, `Z=4 ZBack=6 A=0.5 R=0
     G=1 B=0`, `Z=5.5 ZBack=5.5 A=0.75 R=0 G=0 B=1` (order not guaranteed —
     raw/untidied, so match as a set) plus the `AOV` column (14, 15, 16
     respectively).
  4. Hover pixel **(1, 1)** (an empty pixel — no sample in the fixture):
     the label should show a dash (`deep: –`), not a stale list from the
     previous hover.
  5. Confirm the whole ctest suite is still green after any changes made
     while running this checklist.
  6. *(added 2026-09-15 for M18.P3.T8c)* Add a `DeepWrite` downstream of the
     `DeepRead`, set its `filename` to a writable `.exr` path, right-click
     it → Render (or Render → Render All Writers). Confirm the file is
     written and the progress/abort UI behaves like an ordinary Write node.
  Result (2026-09-17, user's run at `1119c937d`): 1, 2, 4, 5 pass; 3 and 6
  failed — see the 2026-09-17 decision above for the root causes and fixes.
  Items 3 and 6 are to be re-run on an AppImage built at that fix or later.

- 2026-09-18 — **Manual checklist re-run confirmed all 6 items pass** (user,
  on a rebuilt AppImage at `94ceb9407` or later). The gate's manual-checklist
  requirement is now satisfied; what remains before M18 can be marked `done`
  is Phase 18.4 below. Separately from the checklist, the user flagged four
  more items during this pass: `DeepFromImage` creating deep samples for
  fully-transparent source pixels (should create none there); `DeepCrop`'s
  `Reformat` knob visibly doing nothing; and two M17-introduced node-graph
  visuals they don't want — the tinted dot/diamond glyph M17 draws on
  dangling input pipes, and the tinted rectangle M17 draws behind a node's
  body. Root-caused (not yet fixed): `DeepFromImage`'s sample-count pass
  never looks at alpha; `DeepCrop` never marks `reformat`/`bbox` as metadata
  slaves, so `NativeEffectBase`'s lack of OFX-style automatic slave-param
  wiring (the same defect `c2bd4da27` fixed for `DeepRead`'s `filename`)
  leaves its `getPreferredMetadata()` stuck at its node-creation value; both
  visuals are M17's `NodeGui.cpp` glyph/fill machinery
  (`createInputKindGlyphItem`/`refreshInputKindGlyphs` and the `drawRect`
  tint fill in `NodeGui::paint()`), which the user wants rolled back outright
  rather than restyled. Filed as Phase 18.4, all four sized S/M, none blocked
  on anything — added to M18 rather than a new milestone since M18 is still
  `doing` and these surfaced from testing this same milestone's work.
