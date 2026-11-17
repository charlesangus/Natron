# M153 - Distortion Concatenation

An engine-wide mechanism for concatenating non-matrix warps, in both directions, so a chain like Transform → SplineWarp → Transform (or IDistort, LensDistortion, CornerPin) resamples its source once.
- A node that can distort exposes an **inverse map**: output pixel → input coordinate, with an optional Jacobian, over a window.
- The engine composes the maps of adjacent concatenating nodes the way `tryConcatenateTransforms` (`Engine/EffectInstance.cpp:2208-2315`) composes 3x3 matrices today.
- The consumer at the end of the chain samples the original source through the composed map, using M113 - Native Warp Nodes's coordinate sampler.
- RoI propagation goes through the composed map.

This is the conceptual equivalent of OFX standard change 243's `getInverseDistortion`, which the vendored host lib defines but the engine never consumes (`libs/OpenFX/HostSupport/src/ofxhClip.cpp:81,833`, `ofxhImageEffect.cpp:159,1706`, `libs/OpenFX/include/ofxNatron.h:1032+`).

SplineWarp is the first client in both directions: a downstream Transform folds into SplineWarp's map, and SplineWarp folds into a downstream warp. Converting M113 - Native Warp Nodes's IDistort, STMap and CornerPin, and M132 - Native Lens Distortion, follows.

## Scope

**In:**
- An engine chain of matrix and inverse-map steps.
- Providers SplineWarp and LensDistortion; matrix nodes already provide.
- Consumers: Transform-family resample, SplineWarp, IDistort, STMap, IWarp, ITransform, ICornerPin and LensDistortion.
- The RoI is pulled back through every step.

**Out:**
- Image-driven providers (IDistort, STMap and IWarp feeding a downstream consumer): a follow-up milestone.
- LensCorrect (per-channel coordinates): a follow-up.
- OFX `getInverseDistortion`: none.
- Overlay point maps: M433 - Overlay Point Maps.

**Edge cases:**
- A provider with a mask, mix ≠ 1, partial channels, an enabled output-mask layer (SplineWarp), sub-frames or motion blur, or the STMap output mode declines, and the chain ends before it.
- Edge modes that differ end the chain, as today (`EffectInstance.cpp:2867`).
- A consumer with motion or directional blur takes matrix chains only.
- "No source" travels as NaN, which `resampleRowFromCoords` reads as black.
- Pixels at RoD borders may differ from per-node renders, as matrix chains already do, so tests compare the interior.

**Assumptions:**
- Provider caches (`SplineWarpLruCache`, mutex-guarded) can be read from a consumer's render threads.
- Reading a provider's knobs at the consumer's time is thread-safe.

## Phase 153.1: Engine chain

- [ ] M153.P1.T1 — InverseWarpMap and InverseWarpChain types
  - files: new `Engine/InverseWarpChain.{h,cpp}`, new `Tests/Native/InverseWarpChain_Test.cpp`
  - approach: `InverseWarpMap` is immutable, with `mapRow(n, sx, sy, J)` working in place, in the provider's output pixels at render scale (pixel centres +0.5). It gives its input pixels, applies J ← D·J when J is set, and writes NaN for no source. Its `pullBack(RectD canonical, filterSupport)` grows the region by the support times its largest kernel scale. A chain is a list of steps, downstream to upstream. Neighbouring matrices are multiplied into one step, and matrix steps reuse the `mapRowThrough` maths (moved here, passing NaN through). `prepare(window)` gives each step its window by pulling back the previous one.
  - verify: gtest. An analytic swirl ∘ matrix ∘ barrel matches the closed form within 1e-6. J matches finite differences within 1e-4. NaN survives every step. pullBack contains every mapped point.
  - size: L
- [ ] M153.P1.T2 — Concatenation finds map providers
  - files: `Engine/EffectInstance.{h,cpp}` (`tryConcatenateTransforms`), `Engine/Node.cpp` (a cached flag next to `getCurrentCanTransform`), `Engine/ParallelRenderArgs.h` (`InputMatrix`), new `Tests/WarpChainTestEffects.h` (added to `Tests_HEADERS`), new `Tests/WarpChainConcat_Test.cpp`
  - approach: add virtuals `canOfferInverseWarp(time, view, draft)` (decided from knobs only), `getInverseWarpMap(time, view, scale, draft, window)` and `acceptsInverseWarpChain(inputNb, time, view)`, all defaulting to false or null. The walk passes a provider only when the consumer accepts. `InputMatrix` gains `warpChain`, and `cat` is set only for all-matrix chains, so the matrix path is untouched. `getConcatenatedWarpChain(inputNb)` reads the TLS redirections, so `getImage` keeps its signature. GPU tasks already fall back through `hasConcatenatedInput` (`GpuPlacement.cpp`).
  - verify: gtest with test provider and consumer effects:
    - The chain is recorded.
    - A consumer that does not accept gets nothing.
    - A disabled node in between is skipped.
    - A Transform→Transform `cat` is bit-identical to today's.
    - `NativeTransform_Test` ChainConcatenates passes.
  - size: L
- [ ] M153.P1.T3 — RoI through the chain
  - files: `Engine/EffectInstance.cpp` (`transformInputRois`), `Engine/ParallelRenderArgs.cpp` (the request pass), `Tests/WarpChainConcat_Test.cpp`
  - approach: matrix steps keep `transformRegionFromRoD`. Map steps call `pullBack` with the consumer's filter support, and the RoI goes to the last step's input. The request pass, which concatenates without a window today, builds the maps it needs.
  - verify: gtest. The RoI the request pass sends to the source equals the composed pullback, and is strictly smaller than the source RoD for a cropped window.
  - size: M

## Phase 153.2: SplineWarp and matrix consumers

- [ ] M153.P2.T1 — SplineWarp provides its map
  - files: `Engine/Nodes/Transform/SplineWarp/SplineWarp.{h,cpp}`, `SplineWarpField.{h,cpp}` (add `mapPoints(n, sx, sy, J)` for scattered points), `Tests/Native/NativeSplineWarp_Test.cpp`
  - approach: decline under the edge cases. Otherwise build the field through `computeField` over the window (cached), crop with `cropRowToFormat`, and report `getConcatenationEdgeMode` from the settings.
  - verify: gtest. SplineWarp → test consumer at identity matches SplineWarp rendered alone within 1e-4 in the interior. A mask, mix 0.5 and sub-frames each decline.
  - size: L
- [ ] M153.P2.T2 — SplineWarp consumes chains
  - files: `SplineWarp.cpp` (`renderOp`), `NativeSplineWarp_Test.cpp`
  - approach: the chain replaces `plane.concatenated` / `plane.inverse` for the warped rows and the mask and mix reference row. A matrix-only chain gives identical results.
  - verify: ConcatenatesAPrecedingTransform passes unchanged. Test provider → SplineWarp matches a reference row built from composed coordinates within 1e-4.
  - size: M
- [ ] M153.P2.T3 — Resample consumers take map chains
  - files: `Engine/Nodes/Image/ResampleExecutor.cpp`, `Tests/Native/StagedResample_Test.cpp`, `NativeTransform_Test.cpp`
  - approach: for the Matrix form with exactly one sampling matrix, make coordinates and J from the inverse matrix at pixel centres, apply the chain, and call `resampleRowFromCoords` with `eWrapNone`. The Offset form, blur and matrix-only chains keep their existing path.
  - verify: Transform → SplineWarp → Transform renders the source once (SplineWarp's render count stays 0) and matches a one-resample reference within 1e-4. A motion-blurred Transform falls back, with a result identical to today's.
  - size: L

## Phase 153.3: Coordinate warps and lens

- [ ] M153.P3.T1 — IDistort, STMap and IWarp consume chains
  - files: `IDistort.cpp`, `STMap.cpp`, `IWarp.cpp`, `Engine/Nodes/Image/Resampler.{h,cpp}` (expose the internal `wrapCoord` as `wrapRow`), their `Tests/Native/*_Test.cpp`
  - approach: the same pattern at each call site. Wrap in the node's own source space, keeping the clamp rule that zeroes J rows, then apply the chain and sample with `eWrapNone`.
  - verify: SplineWarp → IDistort matches a reference within 1e-4, and its source RoI is SplineWarp's pullback of IDistort's request. STMap repeat and mirror after a test provider match the reference. ITransform works through a test provider.
  - size: L
- [ ] M153.P3.T2 — LensDistortion provides and consumes
  - files: `LensDistortion.{h,cpp}`, `LensModels.h` (read-only), `Tests/Native/NativeLensDistortion_Test.cpp`
  - approach: as a provider it uses `Model::map` in the render direction plus `Model::jacobian`, with the node's format normalisation; no solution gives NaN, and it declines in STMap output mode. As a consumer it applies the chain after its own coordinates.
  - verify: Transform → LensDistortion → Transform renders one resample within 1e-4. SplineWarp → LensDistortion matches its reference.
  - size: M

## Phase 153.4: GPU

- [ ] M153.P4.T1 — Chains reach GPU tasks
  - files: `Engine/Nodes/Image/GpuRender.h` (`GpuQuery`, `GpuRenderArgs`), `Engine/GpuPlacement.cpp`, `Engine/GpuRegionTask.cpp` (`setInputs`), `Engine/InverseWarpChain.{h,cpp}`, `Tests/GpuRegionFallback_Test.cpp`
  - approach: next to `hasConcatenatedInput`, `GpuQuery` gains a per-input mask of the step kinds in the chain (matrix, SplineWarp, lens), filled by placement from `globalData.transforms`. `GpuRenderArgs` gains `inputChains[k]`, which the region task builds from the FrameViewRequest over its output rect at `mappedScale`, not from TLS. The edges already point at the redirected source, so `inputs[k]` is the original source. ChannelCopy, LoopBegin and LoopEnd keep refusing any concatenated input.
  - verify: gtest. The mask and the chain arrive for a test provider → test consumer. A consumer that declines falls back to the CPU with a result identical to today's. Redirected inputs reach `setInputs` for context and timed inputs.
  - size: M
- [ ] M153.P4.T2 — warpchain.slang and step packing
  - files: new `Engine/Gpu/Kernels/warpchain.slang`, `warpchain_test.slang` and its `.cmake`, `InverseWarpChain.{h,cpp}` (`packGpu`), `SplineWarpField.{h,cpp}` (the `getGpuTables` layout gains the grid origin and spacing), `LensModelsGpu.h` (read-only), new `Tests/Native/WarpChainGpu_Test.cpp`
  - approach: one word buffer holds a header (step count, then a tag and offset per step) followed by the step blocks.
    - Matrix steps are 3x3 matrices in float pairs, with the projective divide.
    - SplineWarp steps do a scattered-point lookup mirroring P2.T1's `mapPoints`: lattice coordinates from the grid origin and spacing, the same two-float `lerpFF` as `fieldAt`, and the feather bit of the containing pixel.
    - Lens steps call `lensMap` between the node's normalisation affines; no solution gives NaN.
    `warpChainMap(inout FF x, y, inout float4 J, bool jac)` walks the steps downstream to upstream, with J ← D·J. A chain whose tables exceed `maxScratchBytes` declines and renders on the CPU.
  - verify: the test kernel against CPU `mapRow` on the P1.T1 swirl ∘ matrix ∘ barrel and on real SplineWarp and lens steps. Coordinates match within 1e-6 px, J within 1e-4 relative, and NaN survives every step.
  - size: L
- [ ] M153.P4.T3 — Resample consumers on the GPU
  - files: `Engine/Nodes/Image/ResampleExecutor.cpp` (`canRecordResampleGpu`, `recordResampleGpu`), `Engine/Gpu/GpuResample.{h,cpp}`, `Engine/Gpu/Kernels/resample.slang`, `Tests/Native/NativeTransformGpu_Test.cpp`, `NativeCornerPinGpu_Test.cpp`, `NativeReformatGpu_Test.cpp`, `NativeAutoStraightenGpu_Test.cpp`
  - approach: all-matrix chains fold `cat` into `gpuSamplingTransforms`, so they keep the exact and blur kernels; this removes the GPU refusal for concatenated inputs. Map chains use a new `resample_chain_main`: pixel centre → inverse matrix → `warpChainMap` → `sampleAt` (`coordmap.slang`) with `kWrapNone`, the same path the CPU's `resampleRowFromCoords` takes. Blur and the Offset form take matrix chains only, as on the CPU.
  - verify: GPU-vs-CPU parity at `ParityTolerance::resampling()` in the interior for Transform → Transform, Transform(blur) → Transform, and Transform → SplineWarp → Transform. Tests that now expect `hasConcatenatedInput` → false expect true for matrix chains.
  - size: L
- [ ] M153.P4.T4 — SplineWarp and LensDistortion consume on the GPU
  - files: `SplineWarp.cpp` (`renderOpGpu`), `splinewarp.slang`, `LensDistortion.cpp` (`renderOpGpu`), `lensdistortion.slang`, `NativeSplineWarpGpu_Test.cpp`, `NativeLensDistortionGpu_Test.cpp`
  - approach: the chain is applied after the node's own coordinate, replacing `plane.concatenated`, and goes in as an extra storage binding. SplineWarp with sub-frames takes matrix chains only.
  - verify: parity for Transform → SplineWarp, test provider → SplineWarp, Transform → LensDistortion → Transform, and SplineWarp → LensDistortion.
  - size: M
- [ ] M153.P4.T5 — IDistort, STMap and IWarp consume on the GPU
  - files: `IDistort.cpp`, `STMap.cpp`, `IWarp.cpp`, `stmap.slang`, `iwarp.slang`, `NativeWarpGpu_Test.cpp`, `NativeIWarpGpu_Test.cpp`
  - approach: the kernels wrap in the node's own source space with `wrapCoord`, zeroing J rows on clamp as P3.T1 does, then call `warpChainMap` and sample with `kWrapNone`. ITransform and ICornerPin go through `iwarp.slang`.
  - verify: parity for SplineWarp → IDistort, STMap repeat and mirror after a test provider, and ITransform after a test provider.
  - size: L
- [ ] M153.P4.T6 — Chains run on the GPU end to end
  - files: `SplineWarp.cpp` and `LensDistortion.cpp` (`gpuWeight`), new `Tests/WarpChainGpuPlacement_Test.cpp`
  - approach: a consumer's `gpuWeight` adds the weight of the map steps it absorbs (SplineWarp's displacement weight, for example), so placement does not under-weigh a chain.
  - verify: Transform → SplineWarp → Transform, SplineWarp → IDistort and Transform → LensDistortion → Transform each render through the scheduler with the GPU placed. The report shows the consumer on the GPU, the providers not rendered, and no CPU fallback; the image matches the CPU chain render at resampling tolerance in the interior.
  - size: M

**Verification gate:**
- New and existing concatenation gtests pass: Transform, SplineWarp, StagedResample, IDistort, STMap, IWarp, LensDistortion, FrameGraphBuild, SchedulerEquivalence.
- GPU gtests pass on lavapipe with `NATRON_TEST_REQUIRE_GPU`: WarpChainGpu, WarpChainGpuPlacement, TransformGpu, CornerPinGpu, ReformatGpu, SplineWarpGpu, LensDistortionGpu, WarpGpu, IWarpGpu and GpuRegionFallback. When placement chooses the GPU, matrix-only and map chains render on it with no fallback.
- Full ctest and PR CI are green on a remote release build.
- Render and GUI benches show no regression against main.
- A UAT AppImage plus steps .md covers Transform → SplineWarp → Transform sharpness and SplineWarp → IDistort.

## Decisions

- 2026-10-20 — **Separate milestone, after SplineWarp v1 (user):** engine architecture is kept out of node work. SplineWarp v1 absorbs upstream transforms only (M147 - SplineWarp Node Core).
- 2026-11-14 — M433 - Overlay Point Maps lands a per-node CPU point-map API (forward and inverse) first, for viewer overlays; design this milestone's windowed render map later and reuse its per-node math where it fits (user, M433 grill).
- 2026-11-17 — Elaborated (consultant):
  - D1: the map is a row callback rather than a baked field. Both samplers already take rows of (sx, sy, J), the composition is exact per pixel, and SplineWarp's grid is cached inside its own step.
  - D2: maps are built per window at render or RoI time, not in `tryConcatenateTransforms`, because SplineWarp fields depend on the window. Concatenation decides from knobs only.
  - D3: consumers opt in. OFX and unconverted consumers never receive a map chain, and `cat` is untouched for matrix-only chains.
  - D4: the consumer's filter and clamp apply once, with J composed by the chain rule, as in matrix concatenation.
- 2026-11-17 — **GPU phase in M153 (user):** map chains sample on the GPU inside this milestone, not as a follow-up; a Phase 153.4 is being drafted.
- 2026-11-17 — **Waits for M433 and M179 (user):** M153 reuses M433 - Overlay Point Maps' per-node maps, and its RoI task (P1.T3) builds on M179 - Render Graph Continuation's incremental request pass.
- 2026-11-17 — **Image-driven providers are a follow-up on M179 (user):** IDistort, STMap and IWarp as chain providers need M179's probe → extend → continue, since their source footprint is known only after their UV image renders. They go to /cat-plan as a new milestone; in M153 they are consumers only.
- 2026-11-17 — Phase 153.4 GPU (consultant): chains reach GPU tasks through their render arguments, built over the task rect, not through TLS. Matrix-only chains fold into the existing resample matrices and keep the exact and blur kernels. Map chains sample through `coordmap.slang`'s `sampleAt`, mirroring the CPU `resampleRowFromCoords`. Step maths uses the same two-float arithmetic as splinewarp.slang and lensmodels.slang. Node kernels are extended, not replaced, keeping mask, mix and premult. PM call: map chains ending in impulse or box follow `resampleRowFromCoords` rather than the matrix path's exact floor rule, as on the CPU. A chain over the scratch limit renders on the CPU.
