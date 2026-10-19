# Native node framework

Decided 2026-10-19, M137 - Native Base Class Flexibility, Phase 137.3. This replaces `NativeImageEffect`/`NativeImageTraits` with three composed layers. Phase 137.2 built them as a spike on `milestone/m137-native-base-class-flexibility`. This note records the spike's final shapes and asks the user to approve them before any other node migrates.

Spike commits: a5f0efc72 (P2.T1), 1523db758 (P2.T2), a50ffa921 (P2.T3), 707882940 (P2.T4), cdae61066 (P2.T5), f7b324360 (P2.T6), 56d1b5898 (P2.T7), 52e85927f (P2.T8), d0aee6ea8 (P2.T9). Baselines: 4da5d47df, 56a19689c, e349888f0 (goldens), 78297e2c4, de253aab4 (bench).

## Departures for your approval

Each item is a place where the spike differs from M137's `## Scope` or its task text. Veto any one and it is reworked before Phase 137.4.

- **Spec text is not translated.** `NodeSpec` labels and descriptions are constexpr `std::string_view`s. Before, descriptions went through `tr()`, as Grade's did. Help text in other locales regresses until a lookup is added at the query sites (`NativeEffectBase::getPluginDescription`).
- **Some spec fields are declared but unused.** Scope asks for RFV reads and per-input time, boundary and layer policy.
  - `NodeSpec::rfvReads` holds names only.
  - `InputSpec::time`, `boundary` and `layer` exist, but no executor reads them.
  - `KnobReader` takes no RFV context from thread-local storage. It is the place that context would be captured once knobs have one.
- **The compile-time spec check covers constexpr specs only.** `nodeSpec()` is a virtual returning a pointer, so a runtime spec (MissingPluginNode, P4.T5) goes through no `static_assert`. Scope says an invalid spec has no runtime path.
- **One change in the engine core, outside the milestone's file list.** `EffectInstance::isIdentity_public`'s identity cache now stores "not identity" as `INT_MIN`. Before, it stored -1, so a cached `Route::black()` (identity, input -1) read back as "render".
- **Merge does not keep its own `render()`.** `StagedImageEffect::render`/`isIdentity` are FINAL. As a Custom node, Merge implements `renderOp(args, plan)`/`isOpIdentity` instead, and the composed mix-0 and mask-miss identities are layered over its own.
- **Grade and Blur carry `Stage::PassThroughUnprocessed`.** P2.T5/P2.T7 listed `{Premult, MaskMix}` and `{MaskMix}`. Without the stage, the host copies unprocessed channels, and the result is not bit-exact.
- **Switch:**
  - No `getTransform` concatenation through Switch. Scope allows leaving it out.
  - If `which` lands on an unconnected input below the highest connected one, the output is black. Scope covers only the case beyond range.
  - It is a new ID, `fr.natron.Switch`, registered alongside OFX `net.sf.openfx.switchPlugin`, and it reuses that plug-in's icon.
  - Like Dissolve, it is excluded from the inspector rule (`AppInstance.cpp`).
- **Retired tools:** `grade_main` is gone, and so are its `Tests/Gpu` twins (`GradeCpu_Test`, `GradeGpu_Test`). `tools/gpu-spike/CMakeLists.txt` still builds `grade_gl`/`grade_twin` from `grade_main`, so those targets are broken. They are left that way as a history tool outside the main build.
- **`NativeImageEffect::canRenderOnGpu` now resolves a `StagePlan` on every call.** P10.T1 must watch placement cost.

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
    virtual const NodeSpec* nodeSpec() const;            // null: falls back to getNativePluginDescription()
    virtual Route route(double time, ViewIdx view);      // isIdentity() answers from it for spec'd nodes
    virtual std::optional<OpKind> opKind() const;        // empty: not an image op
    virtual std::optional<GpuCostHint> gpuCostHint(double, ViewIdx, unsigned int mipmapLevel) const;
    virtual bool canRenderOnGpu(const GpuQuery&) const;  // + getGpuInputRoI, getGpuInputLayer, renderGpu
    virtual GpuStageSummary getGpuStageSummary(double time, ViewIdx view);

    class KnobReader { KnobReader(double time, ViewIdx view); auto value(knob, dim) const; auto valueOr(knob, fallback, dim) const; int readCount() const; };
    template <typename T> concept BakesParams = requires(const T& b, const KnobReader& r) { { b.bake(r) } -> std::same_as<typename T::Params>; };
    template <BakesParams T> typename T::Params bakeParams(const T&, double time, ViewIdx view);  // Params trivially copyable

- **Variadic inputs:** a Variadic input is a role plus `count`, and it must come last. Its labels are `label + (firstNumber + k)`.
- **Compile-time checks:** masks must be image inputs, Variadic must be last, counts must be valid, and every `pairWith` must name another input.
- **GPU virtuals:** these moved up from `NativeImageEffect` with defaults that refuse. Every engine cast now targets `NativeEffectBase` (`GpuPlacement`, `GpuRegionTask`, `GpuCostModel`, `RenderScheduler`).

## Layer 2: `ImagePipelineSpec` and the `Op` concepts

`Engine/Nodes/Image/ImagePipelineSpec.h`, `Op.h`:

    enum class Stage : unsigned char { Premult = 1, MaskMix = 2, PassThroughUnprocessed = 4 };
    struct LayerPolicy { enum class Kind { Color, All, TargetSelect, None }; bool defaults[4]; };
    enum class OpKind : unsigned char { Point, Composite, Route, Passes, Resample, Custom };
    struct ImagePipelineSpec { StageSet stages; LayerPolicy layers; OpKind op = OpKind::Point; };
    template <const ImagePipelineSpec& P, const NodeSpec& S> const ImagePipelineSpec& staticImagePipelineSpec();

    struct GpuOpRef { enum class Kind { None, Kernel, RuntimeShader };
        template <typename P> static GpuOpRef compiled(int kernelId, const P&);
        static GpuOpRef runtime(const GpuRuntimeShader&, const GpuShaderInputs&); };
    concept PointOp  = BakesParams + { isIdentity(params) -> bool; row(params, RowIO) -> void; gpu(params) -> GpuOpRef; };
    concept PassesOp = BakesParams + { isIdentity(params, PassContext) -> bool; passes(params, PassContext) -> PassList; };
    concept RouteOp  = BakesParams + { route(params, RouteContext) -> Route; };
    concept OpOfKind<OP, KIND>   // Point | Passes | Route only; other kinds are refused

`StagedImageEffect` (on `NativeEffectBase`) and `ImageNode<OP, SPEC = OP::kNodeSpec, PIPELINE = OP::kPipeline> requires OpOfKind<OP, PIPELINE.op>`:

    // FINAL in StagedImageEffect: render, isIdentity, canRenderOnGpu, renderGpu, getGpuInputRoI, getGpuInputLayer,
    // getGpuStageSummary, opKind, wantsHostUnPremultSelector, rendersUnprocessedChannels, getLayerKnobSpec, ...
    virtual bool isOpIdentity(double, const RenderScale&, const RectI& roi, ViewIdx) = 0;
    virtual StatusEnum renderOp(const RenderActionArgs&, const StagePlan&) = 0;
    virtual GpuOpRef gpuOp(double, ViewIdx) const;                   // default none
    virtual PassList opPasses(double, ViewIdx, const RenderScale&) const;
    virtual bool clipRegionOfDefinition(double, const RenderScale&, ViewIdx, const EffectInstancePtr&, const RectI&, RectI*);

- **Composed identity:** `route()` ≠ None → (MaskMix) mix == 0 → `isOpIdentity` → (MaskMix) a non-inverted mask that misses the window. Every stage identity resolves to the first Source input at the same time.
- **Ops hold their own knobs:** the op is a member of `ImageNode`. It owns its knob handles and `createKnobs(holder, page)`.

## Layer 3: `StagePlan` and its executors

`Engine/Nodes/Image/StagePlan.h`, `PassExecutor.h`:

    struct StagePlan { double time; ViewIdx view; std::bitset<4> processChannels; StageSet stages; int sourceInput;
        double mix; bool maskInvert, applyMask; int maskInput; ImageLayerDesc maskLayer; int maskChannel;
        ImageLayerDesc divisorLayer; int divisorChannel; };
    StagePlan resolveStagePlan(EffectInstance&, const StageSet&, const StageKnobs&, int sourceInput, double, ViewIdx, const std::bitset<4>&);
    StagePlan resolveGpuStagePlan(EffectInstance&, const StageSet&, const StageKnobs&, int sourceInput, const GpuQuery&);
    StatusEnum runStagesCpu(EffectInstance&, const StagePlan&, const RenderActionArgs&, const StageRowFn& row);
    bool canRunStagesGpu(const StagePlan&, const GpuQuery&);   // float, 1-4 channels, divisor in plane
    StatusEnum recordStagesGpu(const StagePlan&, const GpuRenderArgs&, const GpuOpRef&);
    StatusEnum recordStagesGpuDivide(const StagePlan&, const GpuRenderArgs&, const GpuImageView& scratch);
    StatusEnum recordStagesGpuFinish(const StagePlan&, const GpuRenderArgs&, const GpuImageView& filtered);

    enum class PassKind : unsigned char { LineX, LineY, Gather, Point, Resample, Downsample };  // last two not run
    struct Pass { PassKind kind; int slot, haloX, haloY, growX, growY; std::size_t tapsPerSample; int columnBlock;
        bool perPlane; double sigma; bool recursive; /* lineWorker | rowWorker */ };
    struct PassList { std::vector<Pass> passes; PassExec::Boundary boundary; int scratchSlots, resultSlot; PassGpuRecorder gpu; };
    struct PassContext { RenderScale scale; double pixelAspectRatio = 1.; };
    StatusEnum runPassesCpu(EffectInstance&, const StagePlan&, const RenderActionArgs&, const PassList&);
    bool canRecordPassesGpu(const StagePlan&, const GpuQuery&, const PassList&);
    StatusEnum recordPassesGpu(EffectInstance&, const StagePlan&, const GpuRenderArgs&, const PassList&);

- **CPU loop:** `runStagesCpu` is `NativeImageEffect::render`'s loop, moved verbatim. Mask layer and channel now resolve before the divisor's layers, with the arithmetic unchanged.
- **Point GPU path:** `recordStagesGpu` is the old `makeGpuWrap`/`renderGpu` path. It emits one `pixelwrap` dispatch for a Kernel. For a RuntimeShader it runs divide → shader → finish.
- **Both bases share one copy:** the old and new bases use the same CPU loop, the same GPU wrap path, and the same report-once gate for a runtime shader that fails to build (`StageGpuShaderGate`).
- **Passes executor:** `SpatialFilterSupport`'s helpers moved into `PassExec` with their arithmetic order unchanged, and one band helper (`splitLines`) now serves line and row work. Halo, RoI and RoD growth are summed over the passes.
- **Passes on the GPU:** the executor divides only when the Premult stage resolves a divisor, and writes straight to the output when no stage changes it.

## How the op kinds map

| Kind | Status | Host behaviour |
|---|---|---|
| Point | built | `row()` through `runStagesCpu`; `gpu()` through `recordStagesGpu` |
| Passes | built | `passes()` through `runPassesCpu`/`recordPassesGpu`. RoI = window + halo, RoD = source + grow, then `clipRegionOfDefinition` |
| Route | built | identity everywhere. RoD, frames needed and metadata come from the routed input; Black is empty. `renderOp` fails |
| Composite | declared only | no concept, executor or RowIO multi-source fill (P8.T1) |
| Resample | declared only | no concept or executor. `PassKind::Resample`/`Downsample` are declared and not run (P7.T1) |
| Custom | declared only | no concept. Subclass `StagedImageEffect` directly and implement `renderOp`/`isOpIdentity`. GPU is refused (`canRenderOnGpu` FINAL) |

## How each node maps

- **Done in the spike:**
  - **Grade:** `ImageNode<GradeOp>`, `{Premult, MaskMix, PassThroughUnprocessed}`, `color(1,1,1,0)`, Point. Params are doubles, and the GPU block takes floats from them. GPU body is `grade_wrapped`. Normalize and `knobChanged` stay in `Grade`.
  - **Blur:** `ImageNode<BlurOp>`, `{MaskMix, PassThroughUnprocessed}`, `all()`, Passes. The FIR/IIR lines are LineX/LineY passes. The GPU recorder is left out past the FIR tile radius, so those renders go to the CPU. `cropToFormat` goes through `clipRegionOfDefinition`.
  - **Switch:** `ImageNode<SwitchOp>`, 64 Variadic optional inputs, no stages, `LayerPolicy::none()`, Route.
- **P4, NodeSpec only (render paths untouched):** metadata nodes, TypedPassthrough, Shuffle/ShuffleCopy/AddLayers/RemoveLayers, the 11 deep nodes, NativeRead/Write, and MissingPluginNode (the only runtime spec).
- **P5, Point:**
  - Invert, Clamp, Saturation, ColorCorrect and ColorLookup.
  - ColorMath: one op with three `NodeSpec`s.
  - Constant, Solid and CheckerBoard: TargetSelect, no stages, `sourceInput` -1, with `NativeGenerator` reduced to a helper.
- **P6, Passes:** ErodeDilate/Dilate (`morph_*`) and EdgeDetect (CPU only). After these, `SpatialFilterSupport` (now a `using` shim) is deleted.
- **P7, Resample:**
  - Transform and TransformMasked: concatenation through the op's 3×3.
  - Reformat and Position: CPU only.
  - Crop: a Point op with a RoD override. `getRegionOfDefinition` is not FINAL.
- **P8, Composite:**
  - Dissolve: 64 Variadic sources, plus `route()` at an integer `which`.
  - Keyer and ChromaKeyer: `*_main` become `IPixelOp`s.
  - Merge: Custom via `renderOp`.

## What the spike changed from the planned design, and why

- **`nodeSpec()` is a virtual returning a pointer, not CRTP.** Runtime specs and preset families can share one class this way. `staticNodeSpec<kSpec>()` keeps the compile-time check for constexpr specs.
- **Black is identity with input -1.** The render paths already output -1 as empty. The identity-cache encoding fix (above) was needed so that Switch with no inputs stays black once cached.
- **`render`/`isIdentity` are FINAL, with `renderOp`/`isOpIdentity` hooks.** Nodes cannot fork the stage rules again. `route()` has no scale or RoI, so window-dependent identity composes in `StagedImageEffect::isIdentity`.
- **`PassThroughUnprocessed` is a stage.** It is required for bit-exactness on Grade and Blur.
- **Shared executors.** One copy of the CPU loop, and one of the GPU wrap path, runs both bases. Every not-yet-migrated image node therefore already runs on the moved executors, and its goldens prove the move.
- **`PassContext` carries the source's pixel aspect ratio.** Blur's horizontal size is canonical, so `passes()` cannot be built from scale alone.
- **One GPU recorder per pass list, not per pass.** Blur's GPU kernels fuse their passes. A per-pass recorder would have split them.
- **`gpuCostHint` replaces the `GpuCostModel` cast to Blur.** An op provides it, or `passListCostHint` derives it from the passes' sigma.
- **`getGpuStageSummary` replaces placement's by-name reads** of `mix` and the unpremult selector. The old base fills it from its knobs until P9.
- **`opKind()` returns `std::optional`.** Empty means "not an image op". The old base reports Point or Custom. `RenderScheduler` tests `== OpKind::Point`.

## Phase 137.2 results against Phase 137.1

- **Build and tests:** d0aee6ea8, remote fast preset, GNU 14.2.1 -O1 NDEBUG. 2564/2564 tests pass on both the legacy and task-graph schedulers.
- **Goldens:** all 24 `NativeGoldenTest` nodes are bit-exact against the e349888f0 goldens, in the configuration that recorded them. That covers Grade and Blur on the framework, and every other image node on the moved executors.
- **GPU parity:** `NativeGradeGpuTest`, `NativeBlurGpuTest` and `NativeRuntimeShaderGpuTest` pass on the CI runner's software Vulkan, including the latter's two new staged-op cases. Blur's "(Un)premult by" parity cases were removed together with the knob.
- **New suites:** `NodeSpecTest` (5), `NativeSwitchTest` (6) and `StageKnobFooterTest` (3) pass.
- **Bench:** before-timings are in `tools/bench/BASELINE.md` ("M137 before", e349888f0). They are noisy: per-node spread is 19–50% on a loaded 4-core host. **No after-timings exist yet.** P10.T1 measures before (the e349888f0 AppImage) and after back-to-back, interleaved, on a quiet host. The performance gate is unproven.

## Risks for Phases 137.4–137.9

- **Composite and Resample are unbuilt.** P7.T1 and P8.T1 design their concepts, executors and multi-source `StagePlan` from scratch. Today `StagePlan` holds one `sourceInput`, and `OpOfKind` refuses both kinds.
- **Transform family:**
  - Concatenation must survive through the Resample op's 3×3.
  - `GpuCostModel` still casts to `TransformNode` for motion blur, and must be replaced by a hint or summary.
  - `StagedImageEffect::getRegionOfDefinition`/RoI handle only Route and Passes.
- **Keyers:** Keyer and ChromaKeyer have custom GPU bodies and extra inputs. `keyer_main`/`chromakeyer_main` → `IPixelOp` must stay within the parity bounds. ChromaKeyer's custom `getGpuInputLayer` has to become a spec layer policy, but the executors do not read `InputSpec::layer` yet.
- **Merge as Custom:**
  - Its own mask/mix blending must read the `StagePlan` rather than its knobs.
  - The composed identities must agree with its old `isIdentity` (861). That needs B declared as the first Source.
  - GPU stays refused.
- **Dissolve:** `route()` at an integer `which` needs `ImageNode` to call an op's `route()` for a non-Route kind. Today it only does so for Route. The fold past `kPixelKernelMaxInputs` is new code.
- **ColorLookup:** its LUT is written into the GPU parameter words from a per-instance vector. Its `Params` must hold the LUT by value to stay trivially copyable.
- **Rebase onto M109/M107:** conflicts are expected in `GpuRegionTask.cpp`, `GpuCostModel.cpp`, `RenderScheduler.cpp`, `GpuPlacement.cpp` and `GpuResidency_Test.cpp`. The user accepted these at claim.
- **Translation of spec text:** see Departures.
- **Performance:**
  - A plan is resolved per `canRenderOnGpu` call on both bases.
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
