# Milestone 18: Deep compositing v1

"M-Deep-1" in the design doc: `DeepImage` as a first-class payload integrated
with caching, RoI, and the render scheduler — the part the Niik-l fork skipped —
plus the Tier-1 node set, viewer auto-flatten, and a per-sample pixel probe.
Requires M17 (typed edges, `NativeEffectBase`). Parallelizable with M19.

Design authority: `PLAN/DESIGN/2026-09-05-deep-and-3d-native-extensions.md`
("Part 1 — Deep compositing"). Semantics follow the OpenEXR "Interpreting Deep
Pixels" document verbatim.

## Phase 18.1: Data model and math

- [ ] M18.P1.T1 — `DeepImage` data structure
  - files: `Engine/DeepImage.h`, `Engine/DeepImage.cpp` (payload transport lives in `Engine/`, not `Engine/Nodes/`)
  - approach: structure-of-channels mirroring OpenEXR deep layout — `RectI bounds` + `RenderScale` + `ViewIdx`; a COW `shared_ptr` `SampleTable` (per-pixel `counts` uint32, `offsets` uint64 prefix sum); `map<string, DeepChannelBuffer>` of per-channel contiguous float arrays indexed by offsets. Color-only ops copy only touched channels and share Z/ZBack/A via COW. Per-pixel tidiness (sorted/non-overlapping) is a recorded flag, tidied on demand. `getSizeInBytes()` counts table + *owned* buffers only so COW sharing is not double-counted.
  - verify: unit tests — COW sharing (mutating R leaves Z buffer shared), offsets/counts invariants, size accounting under sharing.
  - size: M

- [ ] M18.P1.T2 — Deep sample math per "Interpreting Deep Pixels"
  - files: `Engine/DeepPixelOps.h`, `Engine/DeepPixelOps.cpp`
  - approach: free functions over sample ranges — point vs volumetric sample handling, sample split at a depth, sort, tidy (merge overlapping via Hillman's volumetric merge math), and flatten-to-front compositing. Pure functions, no Image/EffectInstance dependencies, so they unit-test in isolation and both `DeepMerge` and the viewer flatten reuse them.
  - verify: unit tests against the worked examples in the OpenEXR doc (split/merge identities, volumetric merge commutativity where the doc guarantees it).
  - size: M

- [ ] M18.P1.T3 — `Cache<DeepImage>` with its own memory budget
  - files: `Engine/Cache.h` (already templated, :382), `Engine/DeepImageKey.h`/`.cpp`, `Engine/Settings.cpp`, `Engine/Settings.h`
  - approach: dedicated `Cache<DeepImage>` instantiation with `DeepImageKey = (nodeHash, time, view, scale)`; bounds live in params, not the key (leaves room for future tiling). Variable-size entries costed by actual `getSizeInBytes()`. New settings knob for a separate deep cache budget — deep frames must not evict the entire 2D image cache. Bounds growth, not tiling, in v1.
  - verify: unit test — insert/lookup round-trip; eviction respects the deep budget while the image cache stays untouched.
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
  - files: `Engine/Nodes/Deep/DeepRead.cpp`, `Engine/Nodes/Deep/DeepWrite.cpp`, CMake source list
  - approach: OIIO `DeepData` for both directions; EXR deep scanline and tiled parts. `DeepData`'s layout maps 1:1 onto `DeepImage`'s structure-of-channels, so I/O is a per-channel copy, not a transform. Deep AOVs ride the existing plane concept.
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
  - files: `tests/` (alongside the M11 OFX integration test), reference deep EXR asset (pinned, same discipline as existing test assets)
  - approach: end-to-end graph — DeepRead → DeepMerge → DeepRecolor → DeepToImage → Write — rendered headless in CI, output diffed against a committed reference. This is the deep analog of M11's OFX plugin test.
  - verify: test green in the `build-and-test` job; deliberately breaking the merge math makes it fail.
  - size: M

**Verification gate:** all unit tests and the M18.P3.T4 end-to-end CI test green; deep EXR round-trip clean; Viewer flattens a deep stream with per-frame caching (second scrub pass hits cache); deep cache budget respected under a memory-pressure test; entire pre-existing ctest suite still green.
