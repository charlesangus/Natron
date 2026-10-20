# Native node framework

Decided 2026-10-19, M137 - Native Base Class Flexibility. `NativeImageEffect`/`NativeImageTraits` are replaced by three composed layers, built on `milestone/m137-native-base-class-flexibility` and used by every native node. The user approved the design at the Phase 137.3 stop, and the departures below with it.

Spike commits: a5f0efc72 (P2.T1), 1523db758 (P2.T2), a50ffa921 (P2.T3), 707882940 (P2.T4), cdae61066 (P2.T5), f7b324360 (P2.T6), 56d1b5898 (P2.T7), 52e85927f (P2.T8), d0aee6ea8 (P2.T9). Baselines: 4da5d47df, 56a19689c, e349888f0 (goldens), 78297e2c4, de253aab4 (bench).

## Departures

Each item is a place where the framework differs from M137's `## Scope` or its task text.

- **Spec text is not translated.** `NodeSpec` labels and descriptions are constexpr `std::string_view`s. Before, descriptions went through `tr()`, as Grade's did. Help text in other locales regresses until a lookup is added at the query sites (`NativeEffectBase::getPluginDescription`).
- **Some spec fields are declared but unused.** Scope asks for RFV reads and per-input time, boundary and layer policy.
  - `NodeSpec::rfvReads` holds names only.
  - `InputSpec::time`, `boundary` and `layer` exist, but no executor reads them.
  - `KnobReader` takes no RFV context from thread-local storage. It is the place that context would be captured once knobs have one.
- **The compile-time spec check covers constexpr specs only.** `nodeSpec()` is a virtual returning a pointer, so a runtime spec (MissingPluginNode, P4.T5) goes through no `static_assert`. Scope says an invalid spec has no runtime path.
- **One change in the engine core, outside the milestone's file list.** `EffectInstance::isIdentity_public`'s identity cache now stores "not identity" as `INT_MIN`. Before, it stored -1, so a cached `Route::black()` (identity, input -1) read back as "render".
- **Merge does not keep its own `render()`.** `StagedImageEffect::render`/`isIdentity` are FINAL. As a Custom node (until M101 - Clean-Sheet Merge Node), Merge implements `renderOp(args, plan)`/`isOpIdentity` instead, and the composed mix-0 and mask-miss identities are layered over its own.
- **Grade and Blur carry `Stage::PassThroughUnprocessed`.** P2.T5/P2.T7 listed `{Premult, MaskMix}` and `{MaskMix}`. Without the stage, the host copies unprocessed channels, and the result is not bit-exact.
- **Switch:**
  - No `getTransform` concatenation through Switch. Scope allows leaving it out.
  - If `which` lands on an unconnected input below the highest connected one, the output is black. Scope covers only the case beyond range.
  - It is a new ID, `fr.natron.Switch`, registered alongside OFX `net.sf.openfx.switchPlugin`, and it reuses that plug-in's icon.
  - Like Dissolve, it is excluded from the inspector rule (`AppInstance.cpp`).
- **Retired tools:** `grade_main` is gone, and so are its `Tests/Gpu` twins (`GradeCpu_Test`, `GradeGpu_Test`). `tools/gpu-spike/CMakeLists.txt` still builds `grade_gl`/`grade_twin` from `grade_main`, so those targets are broken. They are left that way as a history tool outside the main build.
- **`StagedImageEffect::canRenderOnGpu` resolves a `StagePlan` on every call.** P10.T1 watches placement cost.
- **Composite is a fold, not a fixed source list.** An op names a base input and any number of further inputs, and defines one step that combines the running result with one more input. There is no four-source cap and no Mask-role source read. An op reading several inputs at once is left until a concrete node needs it.
- **Keyer and ChromaKeyer are Custom nodes.** They keep their own render and GPU kernels until M95 - Knob Cleanup And Keyer Redesign. Fitting them to Composite needed per-pixel source coverage, as-stored layer reads and RGB blend sources on the GPU, all keyer-only. They opt into the GPU through the Custom hooks.
- **Crop is a Resample op in the integer-offset form,** with an optional row matte and a source clip to the region of interest. It keeps its behaviour exactly, `reformat` translating the box to the origin included. It is not a Point op.
- **Curve-driven Point ops relax `Params` to `std::copyable`.** ColorCorrect and ColorLookup hold their tables through a `std::shared_ptr` to immutable data cached by the curve knobs' hash, and `GpuOpRef::packed` carries their GPU words. A trivially copyable inline snapshot cannot stay bit-exact for unbounded keys.
- **Generators have no input.** Constant, Solid and CheckerBoard have no Source input, no stages and no pass-through.
- **Dissolve gains a `mix` knob** through the `MaskMix` stage, in place of its own mask handling.
- **Rounding-only diffs from a conversion are accepted.** A conversion that moves results only by rounding, because an op step and a stage step that used to be fused are now split (Dissolve), is re-recorded rather than fixed. Any other CPU diff is a bug.

## Layer 1: `NodeSpec`, route, `KnobReader` (kind-agnostic, `NativeEffectBase`)

`Engine/Nodes/NodeSpec.h`, `Engine/Nodes/KnobReader.h`, `Engine/Nodes/NativeEffectBase.h`:

    enum class InputRole : unsigned char { Source, Aux, Mask, Variadic };
    struct InputSpec { std::string_view label; DataKindEnum kind = eDataKindImage;
        InputRole role = InputRole::Source; bool optional = false; InputTime time; InputBoundary boundary;
        InputLayer layer; /* plane() | select(knob) | pairWith(i) */ int count = 1; int firstNumber = 0; };
    struct NodeSpec { std::string_view id, label, description, grouping; int majorVersion, minorVersion;
        bool isWriter; DataKindEnum outputKind; std::span<const std::string_view> rfvReads;
        std::span<const InputSpec> inputs; constexpr ResolvedInput resolveInput(int) const; };
    template <const NodeSpec& SPEC> const NodeSpec* staticNodeSpec();   // carries the static_asserts
    struct Route { enum class Kind { None, Input, Black }; int inputNb; double time;
        static Route none(); static Route input(int nb, double t); static Route black(); };

    // NativeEffectBase
    virtual const NodeSpec* nodeSpec() const = 0;        // pure; the only description
    virtual Route route(double time, ViewIdx view);      // isIdentity() answers from it for spec'd nodes
    virtual std::optional<OpKind> opKind() const;        // empty: not an image op
    virtual std::optional<GpuCostHint> gpuCostHint(double, ViewIdx, unsigned int mipmapLevel) const;
    virtual bool canRenderOnGpu(const GpuQuery&) const;  // + getGpuInputRoI, getGpuInputLayer, renderGpu
    virtual GpuStageSummary getGpuStageSummary(double time, ViewIdx view);

    class KnobReader { KnobReader(double time, ViewIdx view); auto value(knob, dim) const; auto valueOr(knob, fallback, dim) const; int readCount() const; };
    template <typename T> concept BakesParams = requires(const T& b, const KnobReader& r) { { b.bake(r) } -> std::same_as<typename T::Params>; };
    template <BakesParams T> typename T::Params bakeParams(const T&, double time, ViewIdx view);  // Params std::copyable

- **Variadic inputs:** a Variadic input is a role plus `count`, and it must come last. Its labels are `label + (firstNumber + k)`.
- **Compile-time checks:** masks must be image inputs, Variadic must be last, counts must be valid, and every `pairWith` must name another input.
- **GPU virtuals:** they live on `NativeEffectBase` with defaults that refuse. Every engine cast targets `NativeEffectBase` (`GpuPlacement`, `GpuRegionTask`, `GpuCostModel`, `RenderScheduler`); `GpuCostModel` casts to no node class.

## Layer 2: `ImagePipelineSpec` and the `Op` concepts

`Engine/Nodes/Image/ImagePipelineSpec.h`, `Op.h`:

    enum class Stage : unsigned char { Premult = 1, MaskMix = 2, PassThroughUnprocessed = 4 };
    struct LayerPolicy { enum class Kind { Color, All, TargetSelect, None }; bool defaults[4]; };
    enum class OpKind : unsigned char { Point, Composite, Route, Passes, Resample, Custom };
    struct ImagePipelineSpec { StageSet stages; LayerPolicy layers; OpKind op = OpKind::Point; };
    template <const ImagePipelineSpec& P, const NodeSpec& S> const ImagePipelineSpec& staticImagePipelineSpec();

    struct GpuOpRef { enum class Kind { None, Kernel, RuntimeShader };
        template <typename P> static GpuOpRef compiled(int kernelId, const P&);
        static GpuOpRef packed(int kernelId, std::vector<std::uint32_t> words);
        static GpuOpRef runtime(const GpuRuntimeShader&, const GpuShaderInputs&); };
    // every concept starts with BakesParams and std::copyable<Params>
    concept PointOp  = { isIdentity(params) -> bool; row(params, RowIO) -> void; gpu(params) -> GpuOpRef; }   // or all three taking a PassContext
    concept PassesOp = { isIdentity(params, PassContext) -> bool; passes(params, PassContext) -> PassList; }
    concept RouteOp  = { route(params, RouteContext) -> Route; }
    struct CompositeInputs { CompositeInput base; std::vector<CompositeInput> further; int blendInput = -1; };
    concept CompositeOp = { isIdentity(params) -> bool; inputs(params, RouteContext) -> CompositeInputs; step(params, RowIO) -> void; }
    concept GpuCompositeOp = CompositeOp + { gpu(params) -> GpuOpRef; }       // a kernel built with runCompositeStep
    concept ResampleOp = exactly one of
        MatrixResampleMembers { isIdentity(params, ctx); settings(params, ctx) -> ResampleSettings; transform(params, ctx, amount, invert, Mat3*) -> bool; }
        OffsetResampleMembers { isIdentity(params, ctx); offset(params, ctx) -> PixelOffset; }   // + optional rowMatte(params, ctx), kClipsSourceToRoI
    concept OpOfKind<OP, KIND>   // Point | Composite | Passes | Route | Resample; Custom has no op

`StagedImageEffect` (on `NativeEffectBase`) and `ImageNode<OP, SPEC = OP::kNodeSpec, PIPELINE = OP::kPipeline> requires OpOfKind<OP, PIPELINE.op>`:

    // FINAL in StagedImageEffect: render, isIdentity, canRenderOnGpu, renderGpu, getGpuInputRoI, getGpuInputLayer,
    // getGpuStageSummary, opKind, wantsHostUnPremultSelector, rendersUnprocessedChannels, getLayerKnobSpec, ...
    virtual bool isOpIdentity(double, const RenderScale&, const RectI& roi, ViewIdx) = 0;
    virtual StatusEnum renderOp(const RenderActionArgs&, const StagePlan&) = 0;
    virtual GpuOpRef gpuOp(double, ViewIdx, const RenderScale&) const;   // default none
    virtual PassList opPasses(double, ViewIdx, const RenderScale&) const;
    virtual ResampleRequest opResample(double, ViewIdx, const RenderScale&) const;
    virtual CompositeInputs opInputs(double, ViewIdx) const;
    virtual bool clipRegionOfDefinition(double, const RenderScale&, ViewIdx, const EffectInstancePtr&, const RectI&, RectI*);
    // Custom GPU opt-in; default: stays on the CPU
    virtual bool canRenderOpOnGpu(const GpuQuery&) const;
    virtual StatusEnum renderOpGpu(const GpuRenderArgs&);
    virtual bool getOpGpuInputLayer(int inputNb, const ImageLayerDesc& plane, double, ViewIdx, ImageLayerDesc*) const;

- **Composed identity:** `route()` ≠ None → (MaskMix) mix == 0 → `isOpIdentity` → (MaskMix) a non-inverted mask that misses the window. Every stage identity resolves to the blend input at the same time: the first Source input, or the Composite op's `blendInput`.
- **Compile-time checks:** a Premult stage needs a Source input, a Route op has no stages, and a Resample or Composite op has no Premult stage (a resampler reads its source as stored, and no one divisor applies to several inputs).
- **Any op may have `route()`.** `ImageNode` asks it first whatever the kind (Dissolve answers an integer `which` with it).
- **Ops hold their own knobs:** the op is a member of `ImageNode`. It owns its knob handles and `createKnobs(holder, page)`.

## Layer 3: `StagePlan` and its executors

`Engine/Nodes/Image/StagePlan.h`, `PassExecutor.h`:

    struct StagePlan { double time; ViewIdx view; std::bitset<4> processChannels; StageSet stages; int sourceInput;
        double mix; bool maskInvert, applyMask; int maskInput; ImageLayerDesc maskLayer; int maskChannel;
        ImageLayerDesc divisorLayer; int divisorChannel; std::vector<StageSource> composite; };   // composite: base, then further inputs, in fold order
    StagePlan resolveStagePlan(EffectInstance&, const StageSet&, const StageKnobs&, int sourceInput, double, ViewIdx, const std::bitset<4>&);
    StagePlan resolveGpuStagePlan(EffectInstance&, const StageSet&, const StageKnobs&, int sourceInput, const GpuQuery&);
    StatusEnum runStagesCpu(EffectInstance&, const StagePlan&, const RenderActionArgs&, const StageRowFn& row);
    bool canRunStagesGpu(const StagePlan&, const GpuQuery&);   // float, 1-4 channels, divisor in plane
    void resolveStageSources(const NodeSpec*, const CompositeInputs&, StagePlan*);   // fills plan.composite
    bool canRunStageSourcesGpu(const StagePlan&, const GpuQuery&);   // a Composite fold: a base, no divisor, every input at the query's time and plane
    StatusEnum recordStagesGpu(const StagePlan&, const GpuRenderArgs&, const GpuOpRef&);
    StatusEnum recordStagesGpuDivide(const StagePlan&, const GpuRenderArgs&, const GpuImageView& scratch);
    StatusEnum recordStagesGpuFinish(const StagePlan&, const GpuRenderArgs&, const GpuImageView& filtered);

    enum class PassKind : unsigned char { LineX, LineY, Gather, Point, Resample, Downsample };  // last two declared, not run by the Passes executor
    struct Pass { PassKind kind; int slot, haloX, haloY, growX, growY; std::size_t tapsPerSample; int columnBlock;
        bool perPlane; double sigma; bool recursive; /* lineWorker | rowWorker */ };
    struct PassList { std::vector<Pass> passes; PassExec::Boundary boundary; int scratchSlots, resultSlot; PassGpuRecorder gpu; };
    struct PassContext { RenderScale scale; double pixelAspectRatio = 1.; };
    StatusEnum runPassesCpu(EffectInstance&, const StagePlan&, const RenderActionArgs&, const PassList&);
    bool canRecordPassesGpu(const StagePlan&, const GpuQuery&, const PassList&);
    StatusEnum recordPassesGpu(EffectInstance&, const StagePlan&, const GpuRenderArgs&, const PassList&);

    struct ResampleRequest { enum class Form { None, Matrix, Offset }; Form form; ResampleSettings settings;
        Resampler::CanonicalTransformFn transform; PixelOffset offset; ResampleRowMatte rowMatte; bool clipSourceToRoI; };
    StatusEnum runResampleCpu(EffectInstance&, const StagePlan&, const RenderActionArgs&, const ResampleRequest&);
    bool canRecordResampleGpu(const EffectInstance&, const StagePlan&, const GpuQuery&, const ResampleRequest&);   // matrix form only
    StatusEnum recordResampleGpu(EffectInstance&, const StagePlan&, const GpuRenderArgs&, const ResampleRequest&, const ResampleRegionStages&);

- **CPU loop:** `runStagesCpu` carries the whole Point pipeline: fetch and lock every image, split the window into row bands, divide, call the row function, multiply back, blend by mask × mix, pass the unprocessed channels through. Mask layer and channel resolve before the divisor's layers.
- **Point GPU path:** `recordStagesGpu` emits one `pixelwrap` dispatch for a Kernel. For a RuntimeShader it runs divide → shader → finish. A report-once gate (`StageGpuShaderGate`) keeps a runtime shader that fails to build from being rebuilt.
- **Composite fold:** with a non-empty `plan.composite`, `runStagesCpu` starts the result as the base input's row and calls the row function, the op's `step`, once per further input; mask and mix apply once to the final result. On the GPU, `recordStagesGpu` dispatches the op's `ICompositeStep` kernel (`compositestep.slang`, `runCompositeStep`) once per further input, alternating between two scratch buffers of the output's size, and `wrap_finish` then blends against the blend input. No input is divided.
- **Resample executor:** `runResampleCpu` runs its own loop, not a row function under `runStagesCpu`: it fetches through the concatenated input transform and samples the whole source per plane. The matrix form samples through `Resampler`'s matrices; the offset form shifts the source by whole pixels, weights each shifted row by the optional row matte, and with `clipSourceToRoI` reads zero outside the region of interest the source was fetched for. The mask/mix blend is a copy of `runStagesCpu`'s. `transform()` takes `amount`/`invert` for motion and directional blur, and `ResampleSettings::transformIsIdentity` is the region identity, separate from the node identity. The GPU path takes the matrix form when the op sets `kResamplesOnGpu`, the sampling matrices are affine and no transform is concatenated into the source.
- **Passes executor:** `PassExec` holds the line, gather and band helpers (`splitLines` serves line and row work). Halo, RoI and RoD growth are summed over the passes.
- **Passes on the GPU:** the executor divides only when the Premult stage resolves a divisor, and writes straight to the output when no stage changes it.

## How the op kinds map

| Kind | Status | Host behaviour |
|---|---|---|
| Point | built | `row()` through `runStagesCpu`; `gpu()` through `recordStagesGpu` |
| Passes | built | `passes()` through `runPassesCpu`/`recordPassesGpu`. RoI = window + halo, RoD = source + grow, then `clipRegionOfDefinition` |
| Route | built | identity everywhere. RoD, frames needed and metadata come from the routed input; Black is empty. `renderOp` fails. No stages |
| Composite | built | a fold: `inputs()` names a base input, any number of further inputs and a blend input; `step()` combines the result so far with one more input's row. CPU through `runStagesCpu`; GPU one two-input step kernel per further input plus a finish pass. No Premult stage |
| Resample | built | matrix form (`settings`/`transform`, concatenation, optional GPU) or integer-offset form (`offset`, optional row matte and source-RoI clip, CPU only), through `runResampleCpu`. No Premult stage |
| Custom | built | no op concept. Subclass `StagedImageEffect` directly and implement `renderOp`/`isOpIdentity`. CPU by default; the node opts into the GPU with `canRenderOpOnGpu`/`renderOpGpu`/`getOpGpuInputLayer` |

## How each node maps

- **Spike:**
  - **Grade:** `ImageNode<GradeOp>`, `{Premult, MaskMix, PassThroughUnprocessed}`, `color(1,1,1,0)`, Point. Params are doubles, and the GPU block takes floats from them. GPU body is `grade_wrapped`. Normalize and `knobChanged` stay in `Grade`.
  - **Blur:** `ImageNode<BlurOp>`, `{MaskMix, PassThroughUnprocessed}`, `all()`, Passes. The FIR/IIR lines are LineX/LineY passes. The GPU recorder is left out past the FIR tile radius, so those renders go to the CPU. `cropToFormat` goes through `clipRegionOfDefinition`.
  - **Switch:** `ImageNode<SwitchOp>`, 64 Variadic optional inputs, no stages, `LayerPolicy::none()`, Route.
- **P4, NodeSpec only (render paths untouched):** metadata nodes, TypedPassthrough, Shuffle/ShuffleCopy/AddLayers/RemoveLayers, the 11 deep nodes, NativeRead/Write, and MissingPluginNode (the only runtime spec).
- **P5, Point:**
  - Invert, Clamp, Saturation, ColorCorrect and ColorLookup.
  - ColorMath: one op with three `NodeSpec`s.
  - Constant, Solid and CheckerBoard: generators without inputs, `GeneratorNode<OP>`. TargetSelect, no stages, `sourceInput` -1, no pass-through; `NativeGenerator` is a helper.
  - ColorCorrect and ColorLookup: `Params` is copyable and holds the curve tables through a shared pointer to immutable data; `GpuOpRef::packed` carries their packed GPU words.
- **P6, Passes:** ErodeDilate/Dilate (`morph_*`) and EdgeDetect (CPU only), without "(Un)premult by".
- **P7, Resample:**
  - Transform and TransformMasked: concatenation through the op's 3×3.
  - Reformat and Position: CPU only.
  - Position: the offset form, with `PassThroughUnprocessed`.
  - Crop: the offset form, with the row matte (black border and softness fade) and `kClipsSourceToRoI`. `reformat` is its offset (the crop rectangle translated to the origin, rounded to the nearest pixel at the render scale). The node overrides `getRegionOfDefinition` and `getRegionsOfInterest`, since they follow the crop rectangle.
- **P8, Composite:**
  - Dissolve: a Composite fold over the two of its 64 sources that `which` lies between, with `route()` at an integer `which` and a `mix` knob through the `MaskMix` stage. Its conversion moves masked and one-input pixels by a few ulps, and unprocessed channels now always come from the lower input; its goldens are re-recorded.
  - Keyer and ChromaKeyer: Custom nodes, rendering through `renderOp` with their own kernels. They opt into the GPU through `canRenderOpOnGpu`/`renderOpGpu`/`getOpGpuInputLayer` until M95 - Knob Cleanup And Keyer Redesign.
  - Merge: Custom via `renderOp`/`isOpIdentity`, with B as the first Source. The blend against B reads mask, inversion and mix from the `StagePlan`. It keeps its own early mask-miss identity in `isOpIdentity` beside the framework's, so the old answers hold when the mask region fails or touches the window edge. One old answer does not hold: with a B channel off under an enabled output channel and a non-inverted mask missing the window, the framework answers identity to B. No golden or test covers it. GPU stays refused.

## What the design changed from the plan, and why

- **`nodeSpec()` is a virtual returning a pointer, not CRTP.** Runtime specs and preset families can share one class this way. `staticNodeSpec<kSpec>()` keeps the compile-time check for constexpr specs.
- **Black is identity with input -1.** The render paths already output -1 as empty. The identity-cache encoding fix (above) was needed so that Switch with no inputs stays black once cached.
- **`render`/`isIdentity` are FINAL, with `renderOp`/`isOpIdentity` hooks.** Nodes cannot fork the stage rules again. `route()` has no scale or RoI, so window-dependent identity composes in `StagedImageEffect::isIdentity`.
- **`PassThroughUnprocessed` is a stage.** It is required for bit-exactness on Grade and Blur.
- **One set of executors.** One copy of the CPU loop and one of the GPU wrap path serve every kind. A conversion that only moves results by rounding is re-recorded, not fixed (Departures).
- **`PassContext` carries the source's pixel aspect ratio.** Blur's horizontal size is canonical, so `passes()` cannot be built from scale alone.
- **One GPU recorder per pass list, not per pass.** Blur's GPU kernels fuse their passes. A per-pass recorder would have split them.
- **`gpuCostHint` replaces the `GpuCostModel` cast to Blur.** An op provides it, or `passListCostHint` derives it from the passes' sigma.
- **`getGpuStageSummary` replaces placement's by-name reads** of `mix` and the unpremult selector. It is the resolved `StagePlan`'s.
- **`opKind()` returns `std::optional`.** Empty means "not an image op"; a Custom node reports Custom. `RenderScheduler` tests `== OpKind::Point`.

## Phase 137.2 results against Phase 137.1

- **Build and tests:** d0aee6ea8, remote fast preset, GNU 14.2.1 -O1 NDEBUG. 2564/2564 tests pass on both the legacy and task-graph schedulers.
- **Goldens:** all 24 `NativeGoldenTest` nodes are bit-exact against the e349888f0 goldens, in the configuration that recorded them. That covers Grade and Blur on the framework, and every other image node on the moved executors.
- **GPU parity:** `NativeGradeGpuTest`, `NativeBlurGpuTest` and `NativeRuntimeShaderGpuTest` pass on the CI runner's software Vulkan, including the latter's two new staged-op cases. Blur's "(Un)premult by" parity cases were removed together with the knob.
- **New suites:** `NodeSpecTest` (5), `NativeSwitchTest` (6) and `StageKnobFooterTest` (3) pass.
- **Bench:** before-timings are in `tools/bench/BASELINE.md` ("M137 before", e349888f0). They are noisy: per-node spread is 19–50% on a loaded 4-core host. **No after-timings exist yet.** P10.T1 measures before (the e349888f0 AppImage) and after back-to-back, interleaved, on a quiet host. The performance gate is unproven.

## Risks and limits

- **Transform family:**
  - Concatenation goes through the Resample op's 3×3 and the host's `getCanTransform`/`getTransform`; a node with the `MaskMix` stage applies concatenated transforms but does not offer its own.
  - `GpuCostModel` reads a `gpuCostHint` from the op, not a node class.
  - TransformMasked is `TransformImageNode<true>`, not a `TransformNode`.
- **Keyers:** Keyer and ChromaKeyer keep their own render and GPU kernels as Custom nodes, so M95 - Knob Cleanup And Keyer Redesign lands on the framework rather than on a conversion. ChromaKeyer's input layers come from `getOpGpuInputLayer`; the executors do not read `InputSpec::layer` for a Custom node.
- **Merge as Custom:** see How each node maps for the one identity answer the composed rules change.
- **Composite:** the GPU fold needs every input at the query's time and in the query's plane, and no folded input may be the input the `MaskMix` stage reads as its mask, since the region engine hands an input over once, in one layer.
- **Rebase onto M109/M107:** conflicts are expected in `GpuRegionTask.cpp`, `GpuCostModel.cpp`, `RenderScheduler.cpp`, `GpuPlacement.cpp` and `GpuResidency_Test.cpp`. The user accepted these at claim.
- **Translation of spec text:** see Departures.
- **Performance:**
  - A plan is resolved per `canRenderOnGpu` call.
  - `bakeParams` runs on every query (identity, RoD, RoI, cost hint, render). Blur bakes its knobs several times per frame.
  - M139 owns the Passes overhead, but a regression in P10.T1 blocks the gate.

## Deep, scene and geometry boundary

`NodeSpec`, `route()` and `KnobReader`/`bakeParams` live in `NativeEffectBase` and are kind-agnostic. `InputSpec::kind` and `NodeSpec::outputKind` take any `DataKindEnum`. Only `ImagePipelineSpec` is restricted to image output, by a `static_assert`.

- **This milestone:** every deep, metadata, channel and IO node takes a `NodeSpec` (P4) but keeps its own render path.
- **Later owners:**
  - M21 - Deep Tier-2 And Bridges: deep per-sample plan types.
  - M54 - SOPs-Style Geometry: geometry plan types.
  - M19 - USD/Hydra Foundation: scene authoring.
- **How they adopt it:** each adopts `NodeSpec` and the route hook, and adds its own pipeline spec beside `ImagePipelineSpec` instead of forking a base.
- **Bridges:** the scene-to-image bridges (HydraRender, ProjectTexture) stay Custom image nodes with a scene input.
