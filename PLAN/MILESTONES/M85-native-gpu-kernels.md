# M85 - Native GPU Kernels

Native nodes get GPU implementations on the M84 Vulkan compute backend (design note `PLAN/DESIGN/2026-10-10-gpu-compute-backend.md`), so the resident regions M84 built stop breaking at every node that isn't Grade or Blur.

- **Kernel description:** a backend-neutral GPU description of a point operator sits next to `makeKernel()` (`Engine/Nodes/Image/PixelKernel.h`). It holds a registry kernel ID and a parameter block, built from the same knob reads as the CPU kernel. The base class then renders every point op on the GPU without per-node host code.
- **Per-pixel wrapping:** mask, mix, process channels and "(Un)premult by" become one shared Slang module. It reproduces `NativeImageEffect::render()` exactly, and spatial nodes get divide and finish passes from it. Grade and Blur move onto it, which closes the M84 hand-off.
- **Nodes, heavy first (M82 ranking):**
  - Transform, including motion blur (55% of `defocus_retime`);
  - Blur, completed: wrapping, IIR and box filters, a faster vertical pass;
  - ChromaKeyer, ColorCorrect, Erode/Dilate and Keyer (the `keying_grade` chain);
  - then the cheap point ops, so they don't break a region: Invert, Clamp, Add/Multiply/Gamma, Saturation, ColorLookup, Constant/Solid and CheckerBoard.
- **Proof:** each ported node passes a CPU/GPU parity test with a stated tolerance. A quick directional bench, then a thorough one, measures the M82 comps and the heavy spatial nodes up to 24k on RADV.
- **Out of scope:**
  - **Merge** belongs to M101 - Clean-Sheet Merge Node.
  - **Split off (confirmed by the user 2026-10-12) into M107 - GPU Viewer Hand-off, M108 - Retire Engine GL Host Path, M109 - GPU Region Engine Hardening and M110 - Remaining Native GPU Ports:** viewer interop, the GL host-path retirement and the Preferences device list, the region-engine hardening follow-ups, and the remaining node ports.

Scouted facts this plan relies on (2026-10-12, `milestone/m85-native-gpu-kernels` = `main` `6fcf9c4fa`):
- **Point-op pipeline (`Engine/Nodes/Image/`):**
  - `PixelKernel.h`: `KernelContext`, the row-based `RowIO` with `src[kPixelKernelMaxInputs = 4]`, and `PixelKernel::processRow`. It has no GPU types.
  - `NativeImageEffect.h:127-153` holds the M84 GPU virtuals: `canRenderOnGpu(const GpuQuery&)`, `getGpuInputRoI`, `renderGpu(const GpuRenderArgs&)` (default `eStatusFailed`). `makeKernel` is at `:182`.
  - `GpuQuery`, `GpuImageView` and `GpuRenderArgs` are in **`Engine/Nodes/Image/GpuRender.h`**. M101's brief says `Engine/Gpu/GpuRender.h`; that path is wrong. `GpuRenderArgs::inputs` is indexed by input number, so multi-input GPU nodes already work at the interface.
  - `NativeImageEffect::render` is `NativeImageEffect.cpp:405-615`. The per-pixel wrapping, which is the spec the GPU must reproduce, is at `:560-610`, in this order:
    1. divide by the divisor where `Image::unPremultDivisorIsUsable(d)` (`!(d <= FLT_EPSILON)`, `Image.h:810`), skipping `skipChannel`;
    2. `processRow`;
    3. multiply back with `premultiplyValue(v, d) = v * max(0, d)` (`Image.h:823`);
    4. blend `v * alpha + (1 - alpha) * src`, where `alpha = mix * (maskInvert ? 1 - m : m)`, `alpha == 0` gives src and `alpha == 1` gives v;
    5. pass unprocessed channels through from the undivided source.

    Reads outside the source give 0, the mask outside its image gives 0, and the divisor outside gives 1 (`readSourceRow`/`readChannelRow`, `:136-200`). Channel bits follow `pixelKernelChannelBit`: a 1-channel image is alpha, bit 3.
  - Spatial nodes (Blur, ErodeDilate, EdgeDetect) share `Engine/Nodes/Filter/SpatialFilterSupport.h`. `fillBuffer` (`:212`) divides the source by the divisor where usable. `writeWindow` (`:236`) multiplies back, blends with the undivided source by mask × mix, and passes through unprocessed channels and pixels outside the processed window.
- **Point-op nodes** (`makeKernel` overrides): Grade, Clamp, Saturation, ColorCorrect (tone curves through `CurveSnapshot`), Invert, ColorLookup (a 1024-interval LUT per curve, `ColorLookup.cpp:60-225`), ColorMathNode (Add/Multiply/Gamma), Constant/Solid and CheckerBoard (generators, no input).
- **Not point ops:**
  - Keyer and ChromaKeyer have their own `render()` and 4 inputs: Source, InM, OutM, Bg (`Keyer.h:55-58`, `ChromaKeyer.h:52-55`).
  - Dissolve, ErodeDilate, EdgeDetect, Transform/TransformMasked, Reformat, Crop and Position are not point ops either.
  - Shuffle/ShuffleCopy/AddLayers/RemoveLayers derive from `NativeEffectBase` and are multi-planar (`Shuffle.h:68,96`). M84 never makes multi-plane tasks placement candidates.
- **M84's GPU nodes:**
  - `Grade::canRenderOnGpu` (`Grade.cpp:390`) refuses masks, mix < 1 and premult. `renderGpu` (`:402-455`) dispatches `grade_main` with offsets and strides, and fails when the input view doesn't contain the window (`windowOffset`, `:258`).
  - `Blur::canRenderOnGpu` (`Blur.cpp:894-920`) also refuses channel subsets, non-FIR filters, derivative orders and `alphaThreshold > 0`. Its `renderGpu` (`:936-996`) calls `gpu::recordSeparableBlur` (`Engine/Gpu/GpuBlur.h`). Blur's alpha threshold runs at `Blur.cpp:844`.
- **Placement and regions:**
  - `makeGpuQueryForTask` (`Engine/GpuPlacement.cpp:479-529`) fills `hasMask`, `mix`, `processChannels` and `unPremultChannel` (the divisor channel, with no check that the divisor's layer is the task's plane).
  - `canRenderOnGpu` is gated on `GpuCostModel::hasGpuCost` (`:550-553`). **A node with no GPU seed row is never placed.**
  - In `GpuRegionTask.cpp`, `slotOfTask` (`:510`) keys **every** input, masks included, by the member's own plane (`:593`). External inputs are converted to that layer on upload (`:749-757`). `renderGpu` is called at `:1009` (whole region) and `:1369` (strips).
- **Cost seeds:**
  - `Engine/GpuCostSeed.inc` is generated by `tools/bench/render-profile-report.py --emit-cost-table`. Its GPU rows exist only for Grade and Blur (`GPU_KERNEL_UHD_MS`, `render-profile-report.py:282`).
  - Its CPU rows cover only the 18 plugins in the M82 comps. `GpuTaskTraits` is `{pluginId, sigma, iir}` (`GpuCostModel.h:49`).
  - The M82 profiles it regenerates from are present locally in `build/bench/m82-profile/`.
- **Kernels:**
  - `Engine/Gpu/CMakeLists.txt` lists each `slang_add_kernel(... REGISTRY NatronGpu)` and links every kernel target by hand. `cmake/SlangKernels.cmake:108` already records registry kernels in the global property `SLANG_REGISTRY_NatronGpu`.
  - `Kernels/pointops.slang` is the spike's `IPixelOp`/`Masked<>`/`Chain<>`. Its `Masked` lerps on its own alpha and has no bounds, strides or channel mask, so it does **not** match the CPU. Only `Tests/Gpu/PointOpGpu_Test.cpp` and `PointOpCompose_Test.cpp` use it.
  - Push constants are capped at 128 B (design note §4 #9). `GpuRecordContext::updateBuffer` writes up to 64 KiB and `scratch()` hands out pool buffers (`Engine/Gpu/GpuRecord.h`).
- **Tests:**
  - `Tests/Native/NativeGradeGpu_Test.cpp:206-270` and `NativeBlurGpu_Test.cpp` each carry their own `createRuntime`/upload/download helpers.
  - `Tests/Native/NativeGpuCapability_Test.cpp` holds the allowlist of nodes that may answer `canRenderOnGpu` (Grade, Blur).
  - End-to-end GPU renders use `appPTR->swapGpuContextForTests` and `GpuPlacementOffGuard` (`Tests/RenderBothWays.h:67`), as `Tests/GpuResidency_Test.cpp:265-303` does.
  - `Tests/CMakeLists.txt` lists root sources explicitly and globs `Native/*_Test.cpp` (`:119`).
  - Per-node CPU parity tolerance classes are in `Engine/Nodes/README.md:650-690`.
- **Transform:**
  - The source is fetched with a concatenated input transform (`Transform.cpp:1065`, `getImage(..., &job->inputTransform)`) and folded in by `Resampler::concatenateInputTransform` (`:1113`). The redirections come from `ParallelRenderArgs::transforms` (`ParallelRenderArgs.h:171`, `EffectInstance::tryConcatenateTransforms`, `EffectInstance.h:2327`).
  - Filters and supersampling are `Resampler::interpolate`/`interpolateSuper` (`Resampler.h:319-337`). Adaptive motion-blur sampling is `motionBlurPixel` (`:507`).
- **Dropped from the stub's hand-offs:** the `submitAndWait` timeout. M84 fixed it in review (`d91d3a487`): a timeout now marks the device lost and waits for the fence.

## Phase 85.1: Kernel description and per-pixel wrapping

- [x] M85.P1.T1 — Write the shared per-pixel wrapping module and its engine-free host side
  - files: `Engine/Gpu/Kernels/pixelwrap.slang` (new), `Engine/Gpu/GpuWrap.{h,cpp}` (new), `Engine/Gpu/CMakeLists.txt`, `Tests/Gpu/PixelWrap_Test.cpp` (new), `Tests/Gpu/CMakeLists.txt`
  - approach:
    - **Semantics:** reproduce `NativeImageEffect.cpp:560-610` exactly (see Scouted facts):
      - read source pixels by view offset, stride and bounds, with 0 outside;
      - divide by the divisor where `!(d <= FLT_EPSILON)`, skipping `skipChannel`, with the divisor read from the source pixel (1 outside);
      - `op.apply`;
      - multiply back by `max(0, d)`;
      - `alpha = mix * (invert ? 1 - m : m)`, with the mask read from its own view and channel (0 outside), giving `alpha == 0 → src`, `alpha == 1 → v`, else `v * alpha + (1 - alpha) * src`. Mark the arithmetic `precise` so the driver can't contract it into an FMA;
      - channels not in `channelMask` copy the undivided source.

      nComps is 1–4, mapped through `pixelKernelChannelBit` (1 channel = alpha).
    - **Layout:**
      - push constants `WrapParams`: output origin and size; the src, dst and mask offset, stride and bounds; the mask channel and nComps; the divisor and skip channels; mix; flags. Keep it ≤ 128 B, mirrored in `GpuWrap.h` with a `static_assert`;
      - bindings: 0 src, 1 dst, 2 mask, 3 op params (a storage buffer). An unused binding gets a 16-byte pool buffer.
    - **Entry shapes:**
      - `runWrapped<Op : IPixelOp>(Op, tid)` for point ops, where an op's `load(paramsBuffer)` reads its own params;
      - two standalone kernels for spatial nodes: `wrap_divide` (source → a divided copy) and `wrap_finish` (filtered result + undivided source + mask → dst).
    - **Host side:** `GpuWrap.{h,cpp}` packs `WrapParams` from plain inputs (no Engine headers) and records the divide and finish passes on a `GpuRecordContext`.
    - **CMake:** link the registry kernels from the `SLANG_REGISTRY_NatronGpu` property instead of the hand-kept list, so each later port adds one `slang_add_kernel` line.
    - **Test kernels** (in `Tests/Gpu/CMakeLists.txt`, with CPP twins): `pixelwrap_identity` and `pixelwrap_scale` (× 2 on processed channels).
  - verify:
    - `GpuTests` `PixelWrap.*` passes on lavapipe and on RADV in the container. It compares GPU against a hand-written C++ transcription of `NativeImageEffect.cpp:576-610` over:
      - nComps 1/3/4 and channel subsets;
      - mask none, partial bounds and inverted;
      - mix 0, 0.5 and 1;
      - divisors 0, negative, NaN, < FLT_EPSILON and normal, plus a skip channel;
      - a source smaller than the window;
      - nonzero offsets and strides.
    - **Bounds:** bit-exact without a divisor, and ≤ 3 ULP relative where the divisor divides (Vulkan `OpFDiv` is 2.5 ULP).
    - The build adds no hand-listed kernel link.
  - size: L

- [x] M85.P1.T2 — Extract a shared GPU parity harness for native-node tests
  - files: `Tests/Native/NativeGpuParity.{h,cpp}` (new), `Tests/CMakeLists.txt` (add the `.cpp` and header to the explicit lists), `Tests/Native/NativeGradeGpu_Test.cpp`, `Tests/Native/NativeBlurGpu_Test.cpp`, `Engine/Nodes/README.md`
  - approach:
    - Move the duplicated `createRuntime`/upload/download/`makeBuffer` helpers (`NativeGradeGpu_Test.cpp:206-270`) into the harness. The runtime skips with a reason, or fails under `NATRON_TEST_REQUIRE_GPU=1`.
    - Add `runRenderGpu(effect, query, inputs, mask, outputRect, layout)`: it uploads the input images into views with configurable offset, stride and bounds (including a source smaller than the window), records, submits and downloads.
    - Add `renderEndToEnd(graph, root)`: it renders a small graph once with a forced GPU context (`swapGpuContextForTests`) and once under `GpuPlacementOffGuard`. It returns both outputs plus the frame's region records, so a test can assert that the node really ran in a region.
    - Comparisons reuse `ParityTolerance` from `Tests/NativeParity.h`.
    - Port the Grade and Blur GPU tests onto the harness with no change in what they check.
    - Add a "GPU parity" subsection to `Engine/Nodes/README.md`'s tolerance section, saying that every GPU path states its tolerance there.
  - verify: `NativeGradeGpuTest.*` and `NativeBlurGpuTest.*` pass unchanged on lavapipe and RADV, and still skip cleanly with no ICD.
  - size: M

- [x] M85.P1.T3 — Add the backend-neutral point-op description next to makeKernel() and render point ops on the GPU in the base
  - files: `Engine/Nodes/Image/PixelKernel.h`, `Engine/Nodes/Image/GpuRender.h`, `Engine/Nodes/Image/NativeImageEffect.{h,cpp}`, `Engine/GpuPlacement.cpp`, `Tests/Native/NativeImageEffectGpu_Test.cpp` (new)
  - approach:
    - **The description, in `PixelKernel.h`** (no Vulkan types): `struct PixelKernelGpu { int kernel = -1; std::vector<std::byte> params; }`, plus a `setParams<P>()` helper for trivially-copyable structs. `kernel` holds a `gpu::GpuKernelId` cast to int, so the header builds with `NATRON_GPU=OFF`.
    - **Two virtuals next to `makeKernel`:**
      - `virtual int gpuKernelId(const GpuQuery&) const`: a cheap check of settings, default −1;
      - `virtual bool makeGpuKernel(const KernelContext&, PixelKernelGpu*)`: default false.
    - **One knob read for both backends:** a node reads its knobs once into one params struct that feeds both `makeKernel` and `makeGpuKernel`, so the CPU and GPU descriptions cannot drift. The CPU kernels stay hand-written, as the backend decision requires.
    - **Base path:** when `isPointOp()`:
      - `canRenderOnGpu` accepts float with nComps 1–4 when `gpuKernelId(query) >= 0` and the divisor (if any) lies in the task's plane;
      - `renderGpu` builds the `KernelContext` and calls `makeGpuKernel`. It refuses params over 64 KiB, writes them into a `scratch()` buffer with `updateBuffer`, and dispatches with `WrapParams` from P1.T1;
      - the mask view is `args.inputs[getMaskInput()]`. The mask channel comes from `resolveInputPlaneForRender`, and `maskInvert`, `mix`, the divisor and the skip channel (`Node::getUnPremultSkipChannel`) are read at `query.time`/`view` as `render()` reads them.
    - Nodes that override `canRenderOnGpu`/`renderGpu` themselves (Grade and Blur, for now) are unaffected.
    - **Query:** `GpuQuery` gains `bool unPremultInPlane`, set by `makeGpuQueryForTask` (`GpuPlacement.cpp:516-526`) when the divisor layer equals the task's plane.
    - **Multi-input:** out of scope here. The description is single-source; Keyer, ChromaKeyer and the M101 Merge write their own `renderGpu` and import `pixelwrap.slang` (see Decisions).
  - verify: `NativeImageEffectGpu_Test` defines a test-only point-op node whose CPU kernel scales processed channels by 2 and whose GPU kernel is `pixelwrap_scale`, registered at runtime through `GpuKernelRegistry::registerKernel`. Through the P1.T2 harness, its `renderGpu` matches its CPU `render()` bit-exactly without a divisor, and within the P1.T1 bound with one, over the P1.T1 matrix. `NativeGpuCapabilityTest` still passes, with no new production node answering true.
  - size: L

- [x] M85.P1.T4 — Give GPU-capable nodes without seed rows a class-default cost
  - files: `Engine/GpuCostModel.{h,cpp}`, `Engine/GpuPlacement.cpp`, `Tests/GpuPlacement_Test.cpp`
  - approach:
    - `GpuTaskTraits` gains a cost class (point op / spatial / other), taken from `NativeImageEffect::isPointOp()` in `traitsOf`.
    - **Point op with no GPU row:** Grade's GPU row scaled by its buffer count, (inputs + 1) / 2.
    - **Point op with no CPU row:** Grade's CPU row.
    - **Spatial with no row:** the Blur σ25 rows, both CPU and GPU, as a deliberately rough seed that the session EMA corrects after the first frame.
    - Measured rows always win. P6.T2 replaces the defaults for the ported nodes with measured rows.
  - verify: `GpuPlacement_Test` cases:
    - a point op with no rows becomes placeable, and joins a Grade → X → Grade region instead of splitting it;
    - a node whose `canRenderOnGpu` is false is still never placed;
    - the existing Grade and Blur placement tests are unchanged.
  - size: M

- [x] M85.P1.T5 — Retire the spike's pointops.slang
  - files: `Engine/Gpu/Kernels/pointops.slang` (delete, keeping `Chain<A, B>` in `pixelwrap.slang` for M87), `Engine/Gpu/CMakeLists.txt`, `Tests/Gpu/CMakeLists.txt`, `Tests/Gpu/PointOpGpu_Test.cpp`, `Tests/Gpu/PointOpCompose_Test.cpp`
  - approach:
    - Drop the `pointop_grade/invert/chain/masked` registry entries and their test twins.
    - Keep the composition check `Chain<Grade, Invert>` bit-exact against two passes, re-targeted at a `pixelwrap_chain` test entry built from `pixelwrap.slang`. M87 still needs that evidence.
    - Delete `PointOpGpu_Test`'s masked case, which P1.T1 supersedes.
  - verify: `GpuTests` builds with no `pointop_*` kernel left, and `PointOpCompose.ChainEqualsInvertAfterGrade` (renamed as needed) passes on the CPP twin and on lavapipe.
  - size: M

## Phase 85.2: Grade and Blur complete; masks reach regions

- [x] M85.P2.T1 — Move Grade onto the description, gaining mask, mix and premult
  - files: `Engine/Nodes/Color/Grade.{h,cpp}`, `Engine/Gpu/Kernels/grade.slang`, `Engine/Gpu/CMakeLists.txt`, `Tests/Native/NativeGradeGpu_Test.cpp`
  - approach:
    - Add a `GradeOp : IPixelOp` and an entry `grade_wrapped` built on `runWrapped`, reusing `gradeops.slang`.
    - Keep `grade_main`, because the engine-free `GpuTests` Grade fixtures use it.
    - Replace Grade's `canRenderOnGpu`/`renderGpu` overrides with `gpuKernelId`/`makeGpuKernel`. The params struct is the one `GradeKernel` is built from (computed in double, cast once).
  - verify: on lavapipe and RADV, the existing fixture cases still meet the M83 ULP-conditioned bound. New harness cases also meet it:
    - mask (RGBA and Alpha-only mask images, inverted, partial bounds);
    - mix 0.5;
    - unpremult by A;
    - a channel subset;
    - a source smaller than the window, which `windowOffset` used to refuse.
  - size: M

- [x] M85.P2.T2 — Resolve each input's own layer in regions (masks and side inputs)
  - files: `Engine/Nodes/Image/NativeImageEffect.{h,cpp}`, `Engine/GpuRegionTask.cpp`, `Engine/GpuPlacement.cpp`, `Tests/GpuResidency_Test.cpp`
  - approach:
    - Add `virtual bool getGpuInputLayer(int inputNb, const ImageLayerDesc& plane, double time, ViewIdx view, ImageLayerDesc* layer) const`:
      - the default returns `plane`;
      - the base returns `resolveInputPlaneForRender`'s layer for mask inputs (anything `NativeInputDescription` marks as a mask), so Keyer's InM/OutM work too.
    - In `GpuRegionTask::plan` (`:586-595`), external slots for such inputs are keyed and converted by that layer. A member producer whose plane differs from the requested layer makes the member refuse, falling back to the CPU through the existing reason path.
    - `makeGpuQueryForTask` keeps `hasMask`; nothing else in the query changes.
  - verify: new `GpuResidencyTest` cases in both scheduler modes:
    - Grade masked by a CPU-rendered Alpha-only mask: one region, the mask uploaded once, pixels within Grade's bound of the CPU render;
    - Grade masked by an RGBA mask produced inside the same region: the mask is never downloaded;
    - a mask whose layer isn't the member's plane falls back to the CPU and matches.

    The existing residency tests are unchanged.
  - size: L

- [x] M85.P2.T3 — Blur on the GPU with mask, mix, premult, channel subsets and alpha threshold
  - files: `Engine/Nodes/Filter/Blur.{h,cpp}`, `Engine/Nodes/Image/NativeImageEffect.{h,cpp}` (spatial helpers), `Tests/Native/NativeBlurGpu_Test.cpp`
  - approach:
    - **Shared helpers** for every spatial node, on the base:
      - `recordGpuDivide(args, scratch)` runs `wrap_divide` when a divisor applies;
      - `recordGpuFinish(args, filtered)` runs `wrap_finish`. It covers mask × mix, multiply-back and passthrough of unprocessed channels, exactly as `SpatialFilterSupport::writeWindow`.
    - **Blur:**
      - divide into a scratch buffer when the divisor applies;
      - blur all channels of the scratch, or of the source;
      - then the finish pass.
    - **Alpha threshold:** applied to the filtered alpha before the finish pass, in the order of `Blur.cpp:844`. Use a small Blur-specific pass or a `WrapParams` field, whichever is less code.
    - **Lifted refusals:** channel subsets, mask, mix and unpremult. The FIR-only refusal stays until P2.T4, and derivative orders stay CPU-only (see Decisions).
  - verify: GPU vs CPU Blur within 2e-6 absolute (the M84 FIR bound) for:
    - σ 0.5, 3, 25 and 100;
    - masks (inverted and partial);
    - mix 0.3;
    - unpremult by A;
    - an R+A channel subset;
    - alpha threshold 0.003;
    - black and nearest edges, `expandRoD` on and off, and odd sizes.

    Runs on lavapipe and RADV.
  - size: L

- [x] M85.P2.T4 — GPU IIR (Deriche, van Vliet) and box/triangle/quadratic Blur
  - files: `Engine/Gpu/Kernels/blur_lines.slang` (new), `Engine/Gpu/GpuBlur.{h,cpp}`, `Engine/Gpu/CMakeLists.txt`, `Engine/Nodes/Filter/Blur.cpp`, `Tests/Native/NativeBlurGpu_Test.cpp`
  - approach:
    - One thread per line, running the same recursions as `BlurKernels::deriche`/`vanVliet`/`boxFilter` (`BlurKernels.h:186-188`) in float: coefficients computed on the host by the same functions, causal and anticausal passes, and Neumann/zero boundaries.
    - Rows first, then columns. Columns use the coalesced one-thread-per-column layout `blur.slang` uses for r ≤ 32.
    - `Blur::canRenderOnGpu` accepts these filters at order 0.
  - verify: GPU vs CPU within the `iir()` class (1e-4 absolute, `Engine/Nodes/README.md`) for each filter at sizes 3, 25 and 100, both edges, and a 1-channel alpha. Also runs on RADV, recording the UHD kernel times in Decisions.
  - size: L

- [x] M85.P2.T5 — Speed up the FIR vertical pass for wide radii
  - files: `Engine/Gpu/Kernels/blur.slang`, `Engine/Gpu/GpuBlur.{h,cpp}`, `Tests/Gpu/BlurGpu_Test.cpp`, `tools/gpu-spike/bench/GpuBench_main.cpp`
  - approach:
    - For r > 32, replace the column pass with either a 2D-tiled pass or a transpose → horizontal pass → transpose (design note §2.4 and §4 #5). Pick by measurement on RADV.
    - Pad or skew the shared tile to remove the 8K σ25 stride aliasing, where V is 6× H.
    - Fix `GpuBench_main.cpp` passing the full weight table to the half-kernel layout (an M84 follow-up), so the bench output is correct again.
    - This task does not touch `Blur.cpp`, so it can run from the start.
  - verify:
    - `Gpu.BlurGpuTest.*` stays within 2e-6 on lavapipe and RADV.
    - `GpuBench --sizes uhd,8k` on RADV records H and V times in Decisions. Target: V ≤ 1.5× H at σ25 and σ100 at UHD and 8K. If it misses, record why.
    - `run-host.sh` GpuTests stay green.
  - size: L

## Phase 85.3: Point ops

Each task follows the same pattern, using P1.T3's description:
- a Slang op plus an entry in a new kernel file;
- `gpuKernelId`/`makeGpuKernel` reading one params struct shared with `makeKernel`;
- a harness parity test with a stated tolerance, including a masked, mix 0.5, unpremult-by-A case;
- one `renderEndToEnd` case asserting a Grade → node → Grade chain forms one region;
- the node's ID added to `NativeGpuCapability_Test`'s allowlist.

Tolerance classes are those of `Engine/Nodes/README.md`, applied GPU-vs-CPU. A node that cannot meet its class records a measured bound and the reason beside it in that README.

- [x] M85.P3.T1 — Invert on the GPU
  - files: `Engine/Nodes/Color/Invert.{h,cpp}`, `Engine/Gpu/Kernels/invert.slang` (new), `Engine/Gpu/CMakeLists.txt`, `Tests/Native/NativeInvertGpu_Test.cpp` (new), `Tests/Native/NativeGpuCapability_Test.cpp`
  - approach: follow the Phase 85.3 pattern.
  - verify: exact with no divisor, mask, or mix below 1; otherwise `arithmetic()` (1e-6 relative). Runs on lavapipe and RADV.
  - size: M

- [x] M85.P3.T2 — Clamp on the GPU
  - files: `Engine/Nodes/Color/Clamp.{h,cpp}`, `Engine/Gpu/Kernels/clamp.slang` (new), `Engine/Gpu/CMakeLists.txt`, `Tests/Native/NativeClampGpu_Test.cpp` (new), `Tests/Native/NativeGpuCapability_Test.cpp`
  - approach:
    - Follow the Phase 85.3 pattern.
    - Spell the selects out as Grade's clamps do (`gradeops.slang`), so that NaN lands where `std::min`/`std::max` put it.
  - verify: the same bounds as Invert, plus NaN and ±inf inputs.
  - size: M

- [x] M85.P3.T3 — Add, Multiply and Gamma (ColorMathNode) on the GPU
  - files: `Engine/Nodes/Color/ColorMathNode.{h,cpp}`, `Engine/Gpu/Kernels/colormath.slang` (new, three entries), `Engine/Gpu/CMakeLists.txt`, `Tests/Native/NativeColorMathGpu_Test.cpp` (new)
  - approach:
    - Follow the Phase 85.3 pattern, with one entry per operation, selected by the node's plugin ID.
    - Gamma follows Grade's pow handling, including pow(v, 0) = 1, negatives and subnormals (the design note §3 value-or-0 rule).
    - Add the three IDs to `NativeGpuCapability_Test.cpp`. That makes this a 6th file, accepted for a one-line edit.
  - verify: Add and Multiply within `arithmetic()`, and Gamma within `transcendental()` with the subnormal rule. Runs on lavapipe and RADV.
  - size: M

- [x] M85.P3.T4 — Saturation on the GPU
  - files: `Engine/Nodes/Color/Saturation.{h,cpp}`, `Engine/Gpu/Kernels/saturation.slang` (new), `Engine/Gpu/CMakeLists.txt`, `Tests/Native/NativeSaturationGpu_Test.cpp` (new), `Tests/Native/NativeGpuCapability_Test.cpp`
  - approach:
    - Follow the Phase 85.3 pattern.
    - Luminance coefficients for every colourspace option, including the real ACES AP1 ones, come from the host params, not constants in the kernel.
  - verify: `transcendental()` for each luminance option. Runs on lavapipe and RADV.
  - size: M

- [x] M85.P3.T5 — ColorCorrect on the GPU, tone curves included
  - files: `Engine/Nodes/Color/ColorCorrect.{h,cpp}`, `Engine/Nodes/Image/CurveSnapshot.h` (a flat export of keys for the GPU), `Engine/Gpu/Kernels/colorcorrect.slang` (new), `Engine/Gpu/CMakeLists.txt`, `Tests/Native/NativeColorCorrectGpu_Test.cpp` (new)
  - approach:
    - Port the per-range (shadows/midtones/highlights/master) saturation, contrast, gamma, gain and offset maths.
    - **Tone curves:** evaluate the two `CurveSnapshot` curves in the kernel from their keys, which are uploaded in the params buffer (time, value, left and right derivatives, interpolation type). Port the subset of `Interpolation::interpolate` the tone-range curves can use, with the y-range clamp. Do not bake a LUT: it would not hold the tolerance.
    - Add the ID to the allowlist.
  - verify: `transcendental()` against the CPU across all four ranges, with:
    - default curves and an edited curve with constant, linear, smooth and broken keys;
    - inputs below 0 and above 1.

    Record a measured bound if double-vs-float exceeds it. Runs on lavapipe and RADV.
  - size: L

- [x] M85.P3.T6 — ColorLookup on the GPU
  - files: `Engine/Nodes/Color/ColorLookup.{h,cpp}`, `Engine/Gpu/Kernels/colorlookup.slang` (new), `Engine/Gpu/CMakeLists.txt`, `Tests/Native/NativeColorLookupGpu_Test.cpp` (new), `Tests/Native/NativeGpuCapability_Test.cpp`
  - approach:
    - Upload the five 1024-interval LUTs `ColorLookupKernel` already builds (`ColorLookup.cpp:110-113`), about 20 KB, as the params block.
    - The kernel repeats the lookup and interpolation of `:214-225` for each master-curve mode.
    - Values outside the range follow the CPU branch exactly.
  - verify: `transcendental()` for every master-curve mode, out-of-range inputs and an edited curve. Runs on lavapipe and RADV.
  - size: M

- [x] M85.P3.T7 — Constant and Solid on the GPU
  - files: `Engine/Nodes/Generator/Constant.{h,cpp}`, `Engine/Gpu/Kernels/generators.slang` (new), `Engine/Gpu/CMakeLists.txt`, `Tests/Native/NativeConstantGpu_Test.cpp` (new)
  - approach:
    - A zero-input point op whose op ignores its input.
    - Confirm that the base path and `GpuRegionTask` accept a member with no input slot, and that placement lets a generator start a region (only identity tasks are barred).
    - Add both IDs to the allowlist.
  - verify:
    - Exact match with the CPU.
    - An end-to-end Constant → Grade → Blur frame forms one region with **zero uploads** (region record).
  - size: M

- [x] M85.P3.T8 — CheckerBoard on the GPU
  - files: `Engine/Nodes/Generator/CheckerBoard.{h,cpp}`, `Engine/Gpu/Kernels/generators.slang`, `Tests/Native/NativeCheckerBoardGpu_Test.cpp` (new), `Tests/Native/NativeGpuCapability_Test.cpp`
  - approach:
    - Follow P3.T7's pattern, with the square size, line width and colours in the params.
    - Compute cell membership from integer pixel coordinates at the mapped render scale, as the CPU kernel does. If the CPU uses double coordinates, mirror its rounding.
  - verify: exact at mipmap 0 and 1 for odd box sizes and a centre-line case. Otherwise record a measured bound with the reason.
  - size: M

## Phase 85.4: Keyers and morphology

- [x] M85.P4.T1 — Keyer on the GPU
  - files: `Engine/Nodes/Keyer/Keyer.{h,cpp}`, `Engine/Gpu/Kernels/keyer.slang` (new), `Engine/Gpu/CMakeLists.txt`, `Tests/Native/NativeKeyerGpu_Test.cpp` (new)
  - approach:
    - Override `canRenderOnGpu`/`renderGpu` directly, as a multi-input node: Source, InM, OutM and Bg come through `args.inputs` by input number.
    - Import `pixelwrap.slang`'s per-pixel helpers for the mask, mix and premult parts that apply.
    - InM and OutM take their layer from P2.T2's `getGpuInputLayer`.
    - Port each key mode and the soft/tolerance ranges from `Keyer::render`. Refuse the settings the port doesn't cover.
    - Add the ID to the allowlist.
  - verify:
    - `transcendental()` for every key mode, with and without InM, OutM and Bg, including an input missing over part of the window.
    - An end-to-end Grade → Keyer → Grade frame forms one region.
  - size: M

- [x] M85.P4.T2 — ChromaKeyer on the GPU
  - files: `Engine/Nodes/Keyer/ChromaKeyer.{h,cpp}`, `Engine/Gpu/Kernels/chromakeyer.slang` (new), `Engine/Gpu/CMakeLists.txt`, `Tests/Native/NativeChromaKeyerGpu_Test.cpp` (new)
  - approach:
    - Follow P4.T1's pattern. ChromaKeyer is the heaviest per-pixel node in `keying_grade`: 64 ms/Mpx serial.
    - Port the key-colour, acceptance/suppression angles, tolerances, despill and the source/Bg/InM/OutM combination of `ChromaKeyer::render` (`ChromaKeyer.cpp:783-948`; output modes `ChromaKeyer.cpp:66-75`).
    - Add the ID to the allowlist.
  - verify:
    - `transcendental()` across the output modes (intermediate, premultiplied, unpremultiplied, composite) and the source-alpha options.
    - An end-to-end case forms one region.
  - size: L

- [x] M85.P4.T3 — Erode and Dilate on the GPU
  - files: `Engine/Nodes/Filter/ErodeDilate.{h,cpp}`, `Engine/Gpu/Kernels/morphology.slang` (new), `Engine/Gpu/CMakeLists.txt`, `Tests/Native/NativeErodeDilateGpu_Test.cpp` (new)
  - approach:
    - Separable min (Erode) or max (Dilate): H then V, with the `blur.slang` tile layout.
    - Use direct windowed min/max for small radii, and a van Herk/Gil-Werman prefix/suffix tile for wide ones, matching `ErodeDilateKernels::filterLine` (`ErodeDilate.h:52-72`), including its nearest boundary and its short-line rule.
    - Wrap with P2.T3's divide and finish helpers.
    - `getGpuInputRoI` returns the CPU `getRegionsOfInterest` window.
    - Add both IDs to the allowlist.
  - verify:
    - Exact match with the CPU at sizes 1, 2, 6 and 40 (the `keying_grade` sizes plus a wide one), on short lines, with mask, mix and unpremult.
    - A stripped region (`setGpuStripOverridesForTests`) matches the whole region exactly.
    - Runs on lavapipe and RADV.
  - size: L

## Phase 85.5: Transform

- [x] M85.P5.T1 — Transform and TransformMasked on the GPU without motion blur
  - files: `Engine/Gpu/Kernels/resample.slang` (new), `Engine/Gpu/CMakeLists.txt`, `Engine/Nodes/Transform/Transform.{h,cpp}`, `Tests/Native/NativeTransformGpu_Test.cpp` (new)
  - approach:
    - Resample from **storage buffers** with the CPU filter maths, not `VkImage` sampling (see Decisions):
      - port `Resampler::filter1D`/`filterWeights` for impulse, box, bilinear, cubic, Keys, Simon, Rifman, Mitchell, Parzen and Notch, with the clamp option;
      - port `interpolate`, and `interpolateSuper` for minification (`Resampler.h:255-337`);
      - honour black-outside.
    - The inverse matrix and the supersampling Jacobian come from `buildSamplingTransforms` on the host, passed as push constants or params.
    - `getGpuInputRoI` returns the CPU `getRegionsOfInterest` window for the output rect, so strips get correct halos under rotation.
    - **Refused, falling back to the CPU:**
      - motion blur > 0 (P5.T2);
      - a concatenated input transform. Detect it with the same `ParallelRenderArgs::transforms` lookup that `getImage(..., &inputTransform)` uses (`Transform.cpp:1065`), exposed to the GPU query.
    - Mask and mix go through the finish pass.
    - Add both IDs to the allowlist.
  - verify:
    - `resampling()` (1e-5 absolute) against the CPU for each filter, under:
      - translate, scale up and down (supersampling), rotate 30°, skew;
      - clamp on and off, black-outside on and off;
      - mipmap 0 and 1.
    - TransformMasked's `mask-mix05` case.
    - A rotated Transform in a stripped region equals the whole region exactly.
  - size: L

- [x] M85.P5.T2 — Transform motion blur on the GPU
  - files: `Engine/Gpu/Kernels/resample.slang`, `Engine/Nodes/Transform/Transform.cpp`, `Engine/GpuCostModel.{h,cpp}` (motion-blur traits), `Tests/Native/NativeTransformGpu_Test.cpp`
  - approach:
    - Port `motionBlurPixel` (`Resampler.h:507`) as a per-pixel loop: van der Corput sample times, per-time matrices precomputed on the host into the params buffer (bounded by `motionBlurMaxIterations`), and the same adaptive error stop.
    - Directional blur and the shutter options follow the CPU.
    - `GpuTaskTraits` gains a motion-blur flag, so the 996 ms/Mpx seed CPU row (measured with motion blur) no longer also prices a plain Transform. The plain Transform takes the spatial class default until P6.T2.
  - verify:
    - `iir()` (1e-4 absolute) against the CPU for motion blur 0.5, 1 and 4, all shutter offsets and directional blur.
    - An 8K case on RADV records the kernel time in Decisions.
  - size: L

## Phase 85.6: Benches and close-out

- [x] M85.P6.T1 — Label fallen-back members in profiles
  - files: `Engine/GpuRegionTask.{h,cpp}`, `Engine/RenderScheduler.cpp` (`formatTaskRecord`/`formatRegionRecord`), `tools/bench/render-profile-report.py`, `Tests/RenderProfile_Test.cpp`
  - approach:
    - Members of a region that fell back to the CPU are recorded as `"device":"cpu"`, keeping their `regionId`.
    - The region record gains `fallback` (the reason string, or empty) and `retries`.
    - The report script counts fallbacks per plugin.
    - This is the profile half of the M84 hand-off. The fallback thread budget goes to the proposed M109.
  - verify: `RenderProfileTest` asserts both fields on an injected-OOM fallback and on a clean region, and the report self-test passes.
  - size: M

- [x] M85.P6.T1b — Device-timed kernelMs on the non-pipelined region paths
  - files: `Engine/GpuRegionTask.cpp` (`submit()`, `record()`, `recordStrip`), `Tests/RenderProfile_Test.cpp` or `Tests/GpuResidency_Test.cpp`
  - approach:
    - **Bug:** `submit()` (`GpuRegionTask.cpp:899-925`) adds `msBetween(segmentBegin, end)` to `kernelMs` even when there is no recorder to submit, and `segmentBegin` is only reset when a recorder is created (`:1021`). A member that downloads inside `record()`'s loop (`:1066`) submits, then the unconditional final `submit()` (`:1072`) adds the same interval again, so a region whose last member downloads reports about 2× its wall time.
    - Account only when a recorder was actually submitted, and reset `segmentBegin` after each submit.
    - On the non-pipelined paths, `kernelMs` is a host interval that includes recording, pipeline creation, submit and fence wait. Write a timestamp pair around each segment as the pipelined path does (`:1652`, `:1671`), read it back after `submitAndWait`, and report device time. Where timestamps are unavailable, keep the host interval.
  - verify: a test asserts `kernelMs <= wallMs` for an unstripped region whose last member downloads, and that the region's `kernelMs` is unchanged by adding a download; the GPU sweep stays green.
  - size: M

- [x] M85.P6.T2 — Measure the new kernels and seed their cost rows
  - files: `tools/bench/render-profile-report.py` (`GPU_KERNEL_UHD_MS`, plus CPU rows for native nodes the M82 comps lack), `Engine/GpuCostSeed.inc` (regenerated), `Engine/GpuCostModel.cpp` (if traits need it)
  - approach:
    - On RADV in the container, take the UHD kernel-only time of each node ported in Phases 85.2–85.5 from the region record's GPU timestamps on a single-node region (`NATRON_RENDER_PROFILE`).
    - Take in-situ CPU ms/Mpx for the ported nodes with no CPU row, at 1 thread and at the full thread count, as M84.P1.T1 did.
    - Add the rows and regenerate the seed with the command in its header. The inputs are in `build/bench/m82-profile/`.
    - Record the table in Decisions.
  - verify: the regenerated `GpuCostSeed.inc` has rows for every ported node, `GpuPlacement_Test` and `GpuCostModel` tests pass, and the self-test in `render-profile-report.py` is updated and passes.
  - size: M

- [x] M85.P6.T3 — Quick directional bench (~1 hour)
  - files: none (results go in Decisions); a `gbg`/spatial topology may be added to `tools/bench/graph_bench.py` if the M84 local copy's G-B-G case is still uncommitted
  - approach:
    - Release build in the RADV container (`devshell.sh --gpu`), `NATRON_RENDER_SCHEDULER=taskgraph`, with GPU Auto against Off on the same host.
    - Record `nproc` and the render thread count actually used. The user describes the host as 4 cores, so numbers are not comparable with M84's 7800X3D table.
    - **Workloads:**
      - `keying_grade`, `cg_multipass` and `defocus_retime` at UHD, 6 frames each, first frame dropped;
      - Grade → Transform (motion blur 1) → Grade and Grade → Erode 6 → Blur σ25 → Grade, at UHD and 16k.
    - **Also:** a CPU-only check of `chain`, `comp` and `blurchain` against `main` `6fcf9c4fa`, both with `NATRON_GPU=off`.
    - **Record:** per-frame wall time, regions formed and their sizes, fallbacks (P6.T1), and transfer hidden/exposed.
  - verify:
    - The direction holds: every comp is at least as fast under Auto as under Off, and the regions in `keying_grade` grow beyond M84's mostly single-node ones (Merge and the OFX Despill are the expected breaks).
    - CPU-only is within ±5% of `main`.
    - Any slower case is explained by the cost model and fixed before P6.T4.
  - size: M

- [x] M85.P6.T4 — Thorough bench matrix (only after P6.T3's direction holds)
  - files: none (results go in Decisions)
  - approach:
    - Release build, RADV, interleaved Auto/Off, 3 rounds.
    - The three M82 comps at UHD and 8K.
    - The heavy spatial nodes alone and in G-X-G chains at UHD, 8K, 16k and 24k, tiled (stripped) against untiled where the frame fits: Transform with and without motion blur, Blur FIR σ25/σ100 and IIR, Erode 6/40, ChromaKeyer.
    - Probe the host's available memory before each case and skip, with the reason recorded, any size whose footprint (about 4× the float RGBA frame) doesn't fit. Nothing assumes a particular machine.
    - Report each case's speed-up and the per-node share of region time.
  - verify: a Decisions table with a measured end-to-end speed-up on each M82 comp, and no Auto case slower than Off. Any limit found becomes a follow-up in the proposed M109.
  - size: M

- [ ] M85.P6.T5 — Record outcomes and hand-offs
  - files: `.plan/PLAN/DESIGN/2026-10-10-gpu-compute-backend.md` (a §10 "Outcomes from M85"), `.plan/PLAN/MILESTONES/M101-clean-sheet-merge-node.md`, `.plan/PLAN/MILESTONES/M87-point-op-kernel-fusion.md`, the proposed split milestones
  - approach:
    - **Design note:** the wrap layout and tolerances, the bench tables, the vertical-pass result, and the decision to resample from storage buffers rather than sampled images (it supersedes §1's "sampled images arrive with M85's filtered Transform").
    - **M101:** `merge.slang` imports `pixelwrap.slang` for mask, mix and premult, and the `GpuRender.h` path correction.
    - **M87:** the measured lengths of point-op runs inside regions, as fusion evidence.
    - **The split milestones:** any follow-ups the benches found.
  - verify: the named files carry the outcomes.
  - size: S

**Dependency map**
- **Start in parallel:** P1.T1, P1.T2, P1.T4 and P2.T5. P2.T5 touches only `blur.slang`, `GpuBlur` and the bench.
- **P1.T3:** after P1.T1 and P1.T2.
- **P1.T5:** after P1.T1.
- **Phase 2:**
  - P2.T1 after P1.T3;
  - P2.T2 after P2.T1, because its region tests use a masked Grade;
  - P2.T3 after P1.T3, in parallel with P2.T1. It touches `NativeImageEffect` like P2.T2, so the two run one after the other;
  - P2.T4 after P2.T3 (same `Blur.cpp`) and P2.T5 (same `GpuBlur`).
- **Phase 3:** every task after P1.T3 and P1.T4, and all of them in parallel. They share one-line edits to `Engine/Gpu/CMakeLists.txt` and `NativeGpuCapability_Test.cpp`, which the PM merges serially. P3.T8 after P3.T7 (same `generators.slang`).
- **Phase 4:**
  - P4.T1 and P4.T2 after P2.T2 (InM/OutM layers) and P1.T3; they can run in parallel;
  - P4.T3 after P2.T3 (spatial helpers).
- **Phase 5:** P5.T1 after P2.T3, then P5.T2.
- **Phase 6:**
  - P6.T1 after P2.T2 (same `GpuRegionTask.cpp`);
  - P6.T1b after P6.T1, and before P6.T2, which seeds from `kernelMs`;
  - P6.T2 after all of Phases 2–5;
  - P6.T3 after P6.T1 and P6.T2;
  - P6.T4 after P6.T3;
  - P6.T5 last.
- **Critical path:** P1.T1 → P1.T3 → P2.T3 → P5.T1 → P5.T2 → P6.T2 → P6.T3 → P6.T4 → P6.T5. A parallel near-critical path runs P1.T3 → P2.T1 → P2.T2 → P4.T2.
- **Builds:** one at a time through the natron-dev container (the PM runs them). Implementers never run cmake configure, and new `Engine/Gpu/Kernels/*.slang` files need a reconfigure, which the PM batches.

**Verification gate:**
- **Container, lavapipe:** full ctest is green in both scheduler modes (`NATRON_RENDER_SCHEDULER=legacy|taskgraph`) in natron-dev, with `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json` and `NATRON_TEST_REQUIRE_GPU=1`. That includes:
  - `GpuTests` (`PixelWrap.*` among them);
  - every `Native*Gpu_Test` parity suite, none skipped;
  - `NativeImageEffectGpu_Test`;
  - the new `GpuResidency_Test` mask cases;
  - `NativeGpuCapability_Test`, whose allowlist is exactly the ported set.
- **Container, no Vulkan ICD:** full ctest is green in both modes. Every GPU test skips with a reason, and renders silently fall back to the CPU.
- **Container, RADV** (`devshell.sh --gpu`): the same suite is green in both modes.
- **Host:** `GpuTests` is green on RADV through `tools/gpu-spike/run-host.sh`, including semaphore interop.
- **Build:** `-DNATRON_GPU=OFF` builds.
- **CI:** green, with the GPU tests run on lavapipe, not skipped.
- **Benches:**
  - P6.T3 shows the direction and no CPU-only regression beyond ±5% of `main`;
  - P6.T4 shows a measured end-to-end speed-up on each M82 comp on RADV, with no Auto case slower than Off.
- **Records:** P6.T5's hand-offs are written, and every ported node's GPU tolerance is in `Engine/Nodes/README.md`.

## Decisions

- 2026-10-12 — **Merge is out of scope** (user): M101 - Clean-Sheet Merge Node replaces the old `MergeNode` with a new node that ships its own GPU kernel, so porting the old one would be throwaway work. Not a dependency: M85 runs without it.
- 2026-10-12 — **Elaborated from the stub by a planning consultant; the scope is cut**. M85 keeps the coherent first slice:
  - the kernel description and per-pixel wrapping;
  - Grade and Blur completed;
  - every native point op except the multi-plane channel nodes;
  - Keyer, ChromaKeyer and Erode/Dilate;
  - Transform, including motion blur;
  - the profile fallback labels;
  - the benches on the M82 comps.

  **Moved out, as proposed stubs:**
  - viewer interop and a GPU viewer process → **M107 - GPU Viewer Hand-off**;
  - the host GL wrapper retirement and the Vulkan device list in Preferences → **M108 - Retire Engine GL Host Path**;
  - the region-engine follow-ups from M84 → **M109 - GPU Region Engine Hardening**: per-shape strip memory, contention, helper-thread imports, the fallback thread budget, the `HostImageMemory` idle trim, per-member timestamps, and multi-plane tasks;
  - EdgeDetect, Reformat, Crop, Position, Dissolve and the channel nodes → **M110 - Remaining Native GPU Ports**.

  So the stub's acceptance items "no host-side image operation renders through GL" and "Preferences lists Vulkan devices" move to M108.

  **Why:** each of those is its own coherent change, touching the viewer, the GL engine and the scheduler. Together with the node ports they would make a PR far bigger than M84's. The node ports are what the M82 ranking and residency need first.
- 2026-10-12 — **Dropped hand-off:** the `submitAndWait` timeout returning buffers in use. M84's review fixed it in `d91d3a487`.
- 2026-10-12 — **The backend-neutral description is a kernel ID plus a parameter block** (`PixelKernelGpu` in `PixelKernel.h`), built from the same knob read as `makeKernel()`.
  - The CPU kernels stay hand-written, per `2026-10-10-gpu-compute-backend-chosen.md`.
  - Shared C++/Slang maths headers are allowed per node where they pay off (M101's `MergeMath.h` precedent), but M85 doesn't require them.
  - Mask, mix, process channels and premult live once, in `pixelwrap.slang`. It reproduces `NativeImageEffect::render()` exactly, with `precise` blend arithmetic.
  - Op parameters go in a storage buffer and wrap geometry in push constants, because the 128 B push-constant limit can't hold both (Grade alone needs 64 B).
- 2026-10-12 — **Multi-input kernels and M101:**
  - M85's point-op description stays single-source.
  - Multi-input nodes (Keyer, ChromaKeyer, and later Merge) override `renderGpu` and import `pixelwrap.slang`'s helpers. `GpuRenderArgs::inputs` already carries every input by number.
  - M101.P2.T1, which feeds several inputs into the CPU `RowIO`, is independent of M85 and not duplicated. If a generic multi-source GPU point-op description is wanted later, it extends `PixelKernelGpu` together with M101.P2.T1 or in M87.
- 2026-10-12 — **"(Un)premult by" runs on the GPU only when the divisor lies in the task's own plane;** otherwise the node renders on the CPU. Masks may come from any layer: P2.T2 resolves the layer of each mask input.
- 2026-10-12 — **Blur on the GPU:**
  - every filter (FIR, IIR Deriche/van Vliet, box/triangle/quadratic) is ported at order 0;
  - derivative orders (`orderX/Y > 0`) stay CPU-only, because they are rare and the FIR kernel assumes a symmetric half kernel.
- 2026-10-12 — **Transform resamples from storage buffers with the CPU's filter maths, not `VkImage` sampling.** Hardware filtering has coarse fixed-point weights and only bilinear, so it can't meet the 1e-5 resampling tolerance or Natron's ten filters. This supersedes the design note's "sampled images arrive with M85's filtered Transform".
- 2026-10-12 — **OCIO stays on the CPU in M85.** The OCIO nodes are OpenFX. Read's colour conversion runs on the CPU next to decode. OCIO on Vulkan, without runtime shader compilation, is the open question of the proposed M107.
- 2026-10-12 — **Plugin versions are unchanged.** M85 changes no knob, default or CPU result; GPU paths match the CPU within stated tolerances. This follows the hard-cut rule: no compatibility machinery and nothing version-gated.
- 2026-10-12 — **The user answered the elaboration's questions:**
  - accept the split into M107 - GPU Viewer Hand-off, M108 - Retire Engine GL Host Path, M109 - GPU Region Engine Hardening and M110 - Remaining Native GPU Ports;
  - M108 stays separate from M86 - OFX 1.5 GPU Suites;
  - port Blur's IIR and box filters to the GPU (P2.T4);
  - decide how OCIO runs on Vulkan when M107 starts.
- 2026-10-12 — The bench host is a Ryzen 7 7800X3D with 8 cores and 16 threads, not the 4-core host M64 - Tiled Rendering was benched on.
- 2026-10-12 — **P1.T1** (`6be53a69d`), **P1.T2** (`cea3e3ef2`) and **P1.T4** (`4fd4b3920`) done. Their verify set passes 64/64 on lavapipe and on RADV in both scheduler modes, and on RADV without push descriptors. With no ICD the GPU cases skip.
  - **P1.T1 notes for later tasks:**
    - `WrapParams` is 112 B.
    - The test kernels live in `pixelwrap.slang` itself, because `slang_add_kernel` has no include path.
    - Multiply-back uses `(0 < d) ? d : 0`, so a NaN divisor matches the CPU's `std::max`.
    - The divisor and skip channels are component indices.
    - Ops read their params through `load(StructuredBuffer<uint>)` with `wrapParamFloat` and `wrapParamFloat4`.
    - `GpuWrap` has an explicit `applyMask` flag.
    - "≤ 3 ULP" is checked as 3 × FLT_EPSILON × the largest term. "Bit-exact" accepts ±0 and any NaN.
    - On RADV, divided cases reach at most 1.96 ε.
  - **P1.T2:**
    - `runRenderGpu` takes the runtime as its first argument.
    - `renderEndToEnd` has no caller yet; P1.T3 is its first.
    - Grade keeps its `gradefix` ULP bound.
  - **P1.T4:**
    - `GpuTaskTraits` gains `costClass` and `inputCount`, and `hasGpuCost` includes the class defaults, so `GpuPlacement.cpp` is unchanged.
    - `inputCount` includes the mask input, which errs toward the CPU.
- 2026-10-12 — **P1.T5 done** (`871a330ea`).
  - `Chain<A, B>`, `WrapGrade` and `WrapInvert` moved into `pixelwrap.slang`. The test entries are `pixelwrap_grade`, `pixelwrap_invert` and `pixelwrap_chain`.
  - `PixelWrapCompose.ChainEqualsInvertAfterGrade` passes on the C++ twin. It also runs in the `Gpu.` and `GpuNoPushDescriptor.` ctest registrations, on lavapipe and RADV.
  - `GpuKernelRegistry_Test` now checks `wrap_divide` instead of `pointop_masked`.
  - `Chain::load` loads both ops from the same block, which is fine for Grade then Invert. Two ops that both read params would need offsets; that is for M87.
- 2026-10-12 — **P1.T3 done** (`f7e5aa13e`). The alpha-source failure was in the test, not the base path: the region engine converts inputs to the task's plane before `renderGpu()`, and the test now does the same with `Image::convertToFormat`. 156 of 216 matrix cases render (96 divided); the 60 refusals are the divisor or mask falling outside the plane. The GPU/PixelWrap/NativeImageEffect set passes 265/265 on lavapipe, RADV and no-ICD, in both scheduler modes.
- 2026-10-12 — **P2.T3** (`1875fa346`), **P2.T1** (`40d83b426`), **P3.T1** (`7367c6c39`) and **P3.T2** (`b3ba54792`) done, implemented in parallel and swept together: 392/392 on lavapipe, RADV and no-ICD in both scheduler modes.
  - **Blur:** the alpha threshold is a `WrapParams` field (now 116 B, flag 4) applied in `wrap_finish`. `Blur::renderGpu` crops the source view to the input's valid rect before the divide and finish passes, because a view past the RoD reads a divisor of 0 instead of 1. Unpremult cases hold 2e-6 absolute plus 1e-6 relative: divided values reach ~650, and RADV is 1–5 ulp off there, while lavapipe is exact.
  - **Grade:** `GradeOp` and `grade_wrapped` live in `pixelwrap.slang`, because `grade.slang`'s own buffer bindings would clash. `Grade::readSettings` feeds both kernels.
  - **Clamp:** compares use sign-magnitude integer keys, because the device flushes denormals in float compares and the CPU doesn't.
  - **Tests:** parity windows stay inside the node's RoD, because `GpuRegionTask` never asks `renderGpu` for pixels outside it. Saturation replaces Invert as the "no GPU kernel" node in `NativeImageEffectGpuTest` and `GpuResidencyTest`.
- 2026-10-13 — **P2.T2** (`d39696926`), **P3.T3** (`85c3945dd`), **P3.T4** (`efa00fc6a`), **P3.T7** (`2b4af5100`), **P3.T6** (`2febcd573`) and **P4.T3** (`bfe6a3c5e`) done as one parallel batch. The sweep passes 485/485 on lavapipe, RADV and no-ICD in both scheduler modes, and the CPU suites (`Native|Residency|Placement`) pass 378/378.
  - **P2.T2:**
    - A mask outside the task's plane no longer refuses the GPU. The region uploads it in its own layer.
    - An in-region producer whose plane differs from the layer its consumer reads makes the consumer refuse.
    - **Bug fixed:** `GpuRegionTask::plan` also adds an edge for an input whose node is already a dependency through another input. Before, one node wired to both a member's source and its mask left the source with no slot. With the same layer that rendered wrong; with different layers it hung.
    - The GPU view helpers (`isUsableGpuView`, `cropGpuView`, `getGpuMaskViewComponent`, `toGpuBlurRect`/`toGpuBlurImage`) are protected statics on `NativeImageEffect`, shared by Blur and ErodeDilate.
    - "No GPU path" tests use the test op with `gScaleKernel = -1` and EdgeDetect, not production point ops.
  - **P3.T6 ColorLookup:**
    - Out-of-range values are evaluated from the curve keys (≤ 128 keys, more falls back). Two-component pixels stay on the CPU.
    - **Measured bounds:** weighted standard holds 5e-4 relative and luminance 3e-5, because both divide by a near-zero quantity. The standard and film-like modes hold `transcendental()`.
  - **P4.T3 Erode/Dilate:** direct min/max up to half width 16, van Herk/Gil-Werman above. The mask-shrunk window is computed on the device. Compares are integer-keyed, bit-exact against `filterLine`.
  - **P3.T7 Constant/Solid:** no region or placement change was needed.
  - **Follow-ups for M109 - GPU Region Engine Hardening:**
    - A region whose member had an empty input slot hung instead of falling back to the CPU. It wasn't diagnosed: ptrace isn't allowed in the container.
    - `collectPreRenderFrames` records one edge when a node feeds two inputs of one consumer, which contradicts `FrameGraph::Task`'s comment. The CPU pre-renders one input and fetches the other on demand.
- 2026-10-13 — **P6.T1** (`881ce8501`), **P3.T5** (`e56fb6638`), **P3.T8** (`12d3983b8`), **P5.T1** (`cd8473e25`), **P4.T1** (`acdf52617`) and **P4.T2** (`f218b474a`) done as one parallel batch, plus `25cba7ba4` fixing P4.T3's strips test. The sweep passes 1073/1073 on lavapipe, RADV and no-ICD in both scheduler modes; `DefaultChannelSetTest`'s two project round-trips failed once under `ctest -j8` with temp-file errors and passed 3/3 alone.
  - **Strip tests:** `GpuBufferPool` never allocates less than 64 KiB (`kMinClass`), and the planner checks allocated sizes against the cap, so any cap below 65536 makes even a 4-row strip too large and the region falls back to the CPU. The Transform and Erode/Dilate strips tests now use a 128 KiB cap on outputs several times larger and assert the node ran on the GPU and `strips > 1` (about 4 and 9 strips).
  - **ColorCorrect:** tone curves read the CPU table inside its range and evaluate keys outside it, shared with ColorLookup through `CurveSnapshot::packGpu` and `curvesnapshot.slang`.
  - **ChromaKeyer:** unpremultiplied mode holds 1e-3 relative (it divides by the key alpha); the other modes hold `transcendental()`.
  - **Transform:** motion blur, directional blur and concatenated inputs stay on the CPU; `GpuQuery` gains `hasConcatenatedInput` and `draftMode`. `ResamplingMatchesTheCpuRender` takes ~100 s (about 900 GPU runs), on RADV as on lavapipe.
  - **`kernelMs` is double-counted on the non-pipelined paths** (found from a profile reporting 390 ms kernel against 195 ms wall), and it is a host interval that includes pipeline compilation there. P6.T1b fixes it before P6.T2 seeds from it.

- 2026-10-13 — **P6.T1b done** (`565396a7e`). `submit()` accounts only for a submitted recorder, and each submitted segment on the unpipelined paths is bracketed by a 2-query timestamp pool, with the host interval as fallback. `GpuResidencyTest.RegionKernelTimeIsNotCountedTwiceWhenTheLastMemberDownloads` passes on lavapipe and RADV in both modes and skips with no ICD.
- 2026-10-14 — **P2.T5** (`1f4f17947`) and **P5.T2** (`e1b72fd64`) done. The sweep passes 1080/1080 on lavapipe, RADV and no-ICD in both scheduler modes.
  - **P2.T5, vertical pass:** two variants were built and measured on RADV (7900 XTX): transpose → H → transpose, and the single-dispatch shared tile with a float4-per-pixel tile and column-order groups. The tile won at every size, so the transpose route was removed. GpuBench kernel ms, H / V:

    | | M84 base | M85 |
    |---|---|---|
    | UHD σ25 | 4.28 / 13.19 | 1.57 / 1.92 (1.22×) |
    | UHD σ100 | 16.11 / 26.69 | 5.47 / 5.53 (1.01×) |
    | 8K σ25 | 16.55 / 141.48 | 5.26 / 6.45 (1.23×) |
    | 8K σ100 | 64.01 / 185.23 | 18.90 / 19.22 (1.02×) |

    σ3 (r ≤ 32, the coalesced path) is unchanged at V ≈ 4.0 ms UHD and 15.0 ms 8K; its H halved, so V/H reads ~10×. A follow-up for M109 - GPU Region Engine Hardening if the narrow vertical pass matters. The bench's weight table bug is fixed.
  - **P5.T2, motion blur:** motion blur at any quality, every shutter offset and directional blur are on the GPU. Only concatenated inputs, non-float images and > 4 channels refuse. `GpuTaskTraits` gains `motionBlur`; the 996 ms/Mpx seed row moved to `NATRON_GPU_COST_SEED_CPU_MOTION_BLUR_ROWS`, so a plain Transform takes the spatial default.
    - **Tolerance:** `iir()` with an exception for motion-blurred cases only: at most 1 pixel in 250 per window may miss it, none by more than `Resampler::motionBlurMaxError` (quality / 1000). Cause: the CPU filters each sample in double, the device in float, so a pixel at a sample-count boundary takes one sample more or fewer. Exact agreement would need double-accurate filtering on the device; moving the variance to fp64 alone changes nothing (its error is 2^24 smaller than the per-sample one). Measured worst on lavapipe 6.1e-4 at quality 4 (allowed 4e-3); RADV misses 1 pixel.
    - **8K kernel time on RADV** (debug build, source resident): quality 1 26.3 ms (0.79 ms/Mpx), quality 4 74.3 ms (2.24 ms/Mpx), against the 996 ms/Mpx CPU row.

- 2026-10-14 — **P2.T4 done** (`d29005d05`). Deriche, van Vliet, box, triangle and quadratic Blur run on the GPU at order 0. `NativeBlurGpuTest.*` and the other Blur tests pass 83/83 on lavapipe and RADV.
  - **Precision bug fixed:** the double-float state's product error used `fma`, which Vulkan doesn't guarantee to round once; on lavapipe the van Vliet state (gain ~2e4 at size 100) drifted to 1.7e-3 with nearest edges. `ffMul` now uses Veltkamp/Dekker splitting (`ffSplit`, `ffProductError`), exact on any conforming device.
  - **UHD RGBA kernel time on RADV (7900 XTX, debug build):**

    | filter | size 10 | size 100 |
    |---|---|---|
    | quasigaussian | 5.1 ms | 6.5 ms |
    | gaussian | 5.9 ms | 5.9 ms |
    | box | 5.0 ms | 4.9 ms |
    | triangle | 6.5 ms | 7.0 ms |
    | quadratic | 8.5 ms | 8.8 ms |

- 2026-10-14 — **P6.T2 done** (`7545f2ede`). `GpuCostSeed.inc` has CPU and GPU rows for every ported node; `Placement|CostModel` pass 25/25 and the report self-test passes. RADV 7900 XTX, debug build for GPU, release build for CPU, `nproc` 16. GPU ms is the median region `kernelMs` of a lone node at UHD.

  | node | GPU ms | CPU ms/Mpx 1 thread | CPU ms/Mpx 16 threads |
  |---|---|---|---|
  | Grade | 0.497 | 13.31 | 2.85 |
  | ColorCorrect | 0.514 | 34.20 | 5.23 |
  | Invert | 0.477 | 9.51 | 2.35 |
  | Clamp | 0.494 | 12.39 | 2.55 |
  | Add / Multiply | 0.480 / 0.482 | 9.32 / 9.36 | 2.32 / 2.34 |
  | Gamma | 0.487 | 19.70 | 4.03 |
  | Saturation | 0.482 | 11.38 | 2.48 |
  | ColorLookup | 0.504 | 20.35 | 3.73 |
  | Keyer | 0.441 | 12.59 | 2.70 |
  | ChromaKeyer | 0.514 | 61.47 | 7.55 |
  | Erode / Dilate 6 | 6.83 / 6.92 | 51.29 / 45.05 | 12.29 / 11.94 |
  | Constant / Solid | 0.520 / 0.510 | 4.79 / 4.85 | 3.63 / 3.59 |
  | CheckerBoard | 0.550 | 5.68 | 3.62 |
  | Transform plain | 0.670 | 26.13 | 6.10 |
  | Transform motion blur q1 | 9.79 | 744.6 | 75.1 |
  | Blur FIR σ3 / σ25 / σ100 | 9.51 / 7.07 / 18.75 | 35.4 / 81.7 / 172.8 | 6.76 / 12.4 / 24.2 |
  | Blur Quasi-Gaussian 240 | 20.01 | 32.6 | 4.81 |
  | Blur Gaussian IIR 240 / box 240 | 16.19 / 13.50 | 37.7 / 34.9 | 5.34 / 6.46 |

  - **Region vs isolated:** a lone spatial node's region `kernelMs` reads 1.7–3× the isolated Blur numbers (probably the halo pass over cold, just-uploaded input; unconfirmed). Seeded from the lone-node figures, which is what `observeGpu` converges to. Placement is unaffected: CPU costs are 10–100× larger. Point ops agree between lone and chained regions.
  - **Seed limits:** `GpuTaskTraits` has no Erode size or Blur-filter detail beyond `iir` and `motionBlur`, so Erode/Dilate seed at size 6, the IIR row is Quasi-Gaussian (the slower), and box/triangle/quadratic use the FIR rows. Motion blur q4 (27.1 ms) is not seeded.
  - **Generators** vary 0.45–0.63 ms run to run; the seed is the mean of 4.
  - Blur FIR σ100 CPU now reads 173 against 267 in the old seed; other M82 rows agree within 20% and are unchanged.

- 2026-10-14 — **P6.T3 done** (`5271ea4b0`). Quick directional bench: the direction holds on all three criteria. Release build, RADV (7900 XTX), `NATRON_RENDER_SCHEDULER=taskgraph`, Auto vs Off alternated over 2 rounds.
  - **Host:** `nproc` is 16 (Ryzen 7 7800X3D), not the 4 cores the plan assumed, so the numbers compare directly with M84's. Render pool 16 threads; measured CPU parallelism under Off reached 14.3.
  - **Per-frame wall time (6 frames for comps, first dropped; graph_bench: 1 warm-up + 5 UHD / 2 at 16k):**

    | workload | Off s | Auto s | speed-up | M84 |
    |---|---|---|---|---|
    | keying_grade UHD | 0.945 | 0.349 | 2.71× | 1.12× |
    | cg_multipass UHD | 0.852 | 0.418 | 2.04× | 1.26× |
    | defocus_retime UHD | 2.610 | 0.884 | 2.95× | – |
    | Grade→Transform(mb 1)→Grade UHD / 16k | 1.384 / 64.05 | 0.153 / 3.05 | 9.0× / 21.0× | – |
    | Grade→Erode 6→Blur σ25→Grade UHD / 16k | 0.383 / 14.75 | 0.110 / 2.54 | 3.5× / 5.8× | – |

  - **Regions (Auto):** keying_grade 6 regions (4, 3, 2, 2, 1, 1 nodes), cg_multipass 9 (largest 5), defocus_retime 9 (largest 3, with the motion-blur Transform). Both graph_bench chains form one region; the 16k ones run 9 strips. No fallbacks, no retries. Transfer is hidden only on strip-pipelined regions (16k: 159/22 ms hidden/exposed gtg, 136/19 gebg).
  - **CPU-only vs `main` 6fcf9c4fa** (UHD, median frame): chain:30 −1.7%, comp:50 0.0%, blurchain:10 −0.3%.
  - **Caveats:**
    - defocus_retime alternates by frame (Auto 0.51 s then 1.43 s, Off 2.5 s and 2.8 s); the slow frames have a ~1.0 s CPU FrameBlend in both modes.
    - cg_multipass has six lone-Grade regions that each move ~171 MB for a 0.3 ms kernel, all exposed. The comp is still 2× faster, but this placement is the likeliest to turn negative on a slower bus.
    - The uncompressed EXR Write dominates 16k Auto frames (~2.4 s of 3.05 s).
  - Raw results: `.local/m85/batchF/bench3/summary-gpu.txt`.

- 2026-10-14 — **P6.T4 in progress: two gate failures found, one fixed.** First full pass (UHD/8K/15k/16k/24k, 3 rounds) is in `.local/m85/batchF/bench4/summary.txt`.
  - **Erode 40 at 16k was slower than Off** (21 ms/Mpx). The wide van Herk pass read each column one 4-byte load per row pitch, so a power-of-two width aliased every load onto one memory channel. Rewritten register-blocked (16 outputs per thread, all four channels, adjacent columns per group) in `eacf34f98`: 0.5–0.6 ms/Mpx at every size (was 7.6 at UHD/8K), bit-exact, `ErodeDilate` 16/16 on RADV and lavapipe; Auto ≈ 5× Off at 16k.
  - **24k Transform/Blur Auto slower than Off was a bench artifact:** Off timed one cold frame, Auto a warm-up plus two, and from the second ~4.7 GB EXR on, dirty-page throttling made the Write disk-bound (Auto frame 0 wrote in the same 3.3 s as Off). `graph_bench.py` now deletes each output and syncs outside the timed part; 16k/24k rerun with equal frame counts in both modes.
  - **No host RAM figure is assumed anywhere** (user, 2026-10-14): the drivers probe available memory before each case and skip what won't fit, and the dirty-page drain wait is absolute, not sized to a machine. The figure recorded on 2026-10-12 was wrong; RAM figures are dropped from these notes.

- 2026-10-15 — **P6.T4 done.** The gate holds: every M82 comp is faster end to end, and no Auto case is slower than Off in any round. Release build, RADV, `NATRON_RENDER_SCHEDULER=taskgraph`, Auto and Off interleaved, 3 rounds. The code under test is `eacf34f98` (Erode wide pass) plus `90f27cc64` (bench only).
  - **Transient host (user, 2026-10-15):** the host rebooted during the 24k phase and can come back on different hardware even when the model strings match. Off motion blur ran 25% slower and the EXR Write ~40% slower after the reboot. So speed-ups are the **median of per-round Auto/Off ratios**, each pair measured on one boot, and absolute seconds are not comparable across rounds. The one pair split across the reboot (gxg-blur25 24k, round 2) was rerun as `r2b`.
  - **M82 comps** (bench4, one boot; median s/frame, 6 frames, first dropped):

    | comp | UHD Off / Auto | UHD speed-up | 8K Off / Auto | 8K speed-up |
    |---|---|---|---|---|
    | keying_grade | 0.946 / 0.342 | 2.77× | 3.754 / 1.295 | 2.90× |
    | cg_multipass | 0.855 / 0.429 | 1.99× | 3.412 / 1.635 | 2.09× |
    | defocus_retime | 2.602 / 0.886 | 2.94× | 9.775 / 3.392 | 2.88× |

    Exposed transfer is 59–84% of region time in the comps: regions there are unstripped, so nothing overlaps their uploads and downloads.
  - **Heavy spatial nodes**, alone (`node-`) and between Grades (`gxg-`). Median end-to-end speed-up, Write included. UHD and 8K come from bench4 (Erode 40 from bench5, after its fix); 15k, 16k and 24k from bench5:

    | case | UHD | 8K | 16k | 24k | worst round |
    |---|---|---|---|---|---|
    | Transform | 1.36× / 1.72× | 1.45× / 1.84× | 1.65× / 2.30× | 1.79× / 2.50× | 1.35× |
    | Transform motion blur | 8.46× / 8.70× | 11.95× / 12.29× | 19.64× / 19.76× | 21.13× / 20.85× | 8.42× |
    | Blur FIR σ25 | 2.18× / 2.64× | 2.56× / 3.07× | 3.11× / 3.73× | 3.22× / 3.48× | 2.16× |
    | Blur FIR σ100 | 3.53× / 3.90× | 4.13× / 4.44× | 5.03× / 5.50× | – | 3.44× |
    | Blur IIR σ25 | 1.54× / 1.90× | 2.04× / 2.44× | 2.31× / 2.86× | 2.23× / 2.77× | 1.52× |
    | Erode 6 | 2.16× / 2.56× | 2.48× / 2.94× | 3.46× / 4.08× | – | 2.09× |
    | Erode 40 | 2.52× / 2.91× | 2.93× / 3.48× | 3.63× / 4.20× | 2.99× / 3.59× | 2.41× |
    | ChromaKeyer | 1.93× / 2.29× | 2.05× / 2.54× | 2.46× / 3.18× | 2.18× / 2.81× | 1.86× |

    Each cell is node / gxg. Blur σ100 and Erode 6 were not run at 24k. Probing memory before each case skipped nothing.
  - **Excluding the Write**, Auto is 4.5–61× faster. The uncompressed EXR Write is 60–90% of every Auto frame at 16k and 24k.
  - **Tiled vs untiled:** a 15k frame already strips (8 strips) on this 24 GB device, so "untiled" means UHD and 8K. Stripped regions hide their transfer (exposed 0.00–0.07 ms/Mpx at 16k), while unstripped ones expose ~0.5 ms/Mpx, and ~2 ms/Mpx for Transform. So region time per Mpx is lower tiled than untiled for every node, e.g. Blur σ25 0.79 tiled against 1.34 untiled. At 24k, ChromaKeyer and Erode 40 expose more again (0.24–0.40 ms/Mpx).
  - **Per-node share of region time** (16k gxg): Blur σ100 91%, IIR 92%, Erode 77%, Transform motion blur 83%. ChromaKeyer is only 19%, behind Grade at 37%. Plain Transform regions are 55% "other" (time outside member kernels and transfer), which is unexplained.
  - **Follow-ups for M109 - GPU Region Engine Hardening:**
    - overlap transfer in unstripped regions, or strip them anyway: this is the comps' largest cost;
    - find where plain Transform's "other" region time goes, and why its unstripped exposed transfer is 4× other nodes';
    - find why exposed transfer rises again at 24k for ChromaKeyer and Erode.
  - Raw results: `.local/m85/batchF/bench4/summary.txt` (comps, UHD, 8K) and `.local/m85/batchF/bench5/summary.txt`.
