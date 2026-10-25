# M110 - Remaining Native GPU Ports

Port the native nodes M85 left on the CPU to the GPU, each as an op body on the framework M137 - Native Base Class Flexibility built. The ports are:
- EdgeDetect, a Passes list recorder on M85's line-filter kernels, extended to first-order derivatives and Neumann edges;
- Reformat, a matrix-form Resample op opting into `resample.slang`;
- Position and Crop, on a new GPU executor for the offset form of Resample;
- Dissolve, a GPU Composite step;
- Shuffle, ShuffleCopy, AddLayers and RemoveLayers, on a shared channel-route description.

The Shuffle family needs two more pieces underneath. First, a multi-plane `GpuQuery`/`GpuRenderArgs` in `Engine/Nodes/Image/GpuRender.h`. Second, region members split per plane, so multi-plane tasks can join regions. Every port lands with a CPU/GPU parity test and a GPU bound in its node doc. The milestone closes on a forced-region chain test, Read → Reformat → Grade → Shuffle → Write, run on lavapipe in CI and on RADV in the natron-dev container.

**Scouted facts** (at `bf781cbcd`, in the `.worktrees/m110` worktree):
- **The framework (M137).**
  - `StagedImageEffect::canRenderOnGpu`/`renderGpu` are FINAL and dispatch by op kind (`Engine/Nodes/Image/StagedImageEffect.h:132,176`).
  - The routes are: Point and Composite through `recordStagesGpu`, Passes through `recordPassesGpu` (`PassExecutor.h:512-526`), and matrix-form Resample through `recordResampleGpu` (`ResampleExecutor.h:197-230`).
  - `ImageNode<OP>` builds the `ResampleRequest`. The offset form copies `rowMatte` and `kClipsSourceToRoI` (`StagedImageEffect.h:617-627`), and `opResamplesOnGpu()` is `GpuResampleOp<OP>`, which is matrix-only (`:656-663`, `Op.h:360`).
  - `Op.h:322-328` says of the offset form: "Neither runs on the GPU".
- **Composite on the GPU.**
  - `GpuCompositeOp` needs `gpu(params)` naming a registry kernel built with `runCompositeStep` (`Op.h:304`, `Engine/Gpu/Kernels/compositestep.slang:16,48`).
  - No production node uses it yet. The only step is the test step `StepHalveAdd` (`compositestep.slang:64`).
- **EdgeDetect.**
  - It is `OpKind::Passes` with no `PassList::gpu` (`Engine/Nodes/Filter/EdgeDetect.h:142-200`, comment at `:136` "It runs on the CPU only").
  - `edgeDetectPasses` (`EdgeDetect.cpp:512-616`) uses `Boundary::Nearest`, 3 or 4 scratch slots, and Neumann line filters of **order 1** (`:575-587`).
  - The finite-difference schemes add a Gaussian pre-blur plus a 3x3 stencil gather (`:546-560`). Then come a combine point pass, an `ErodeDilateKernels::filterLine` erode/dilate (`:132-145`, `:592-605`), and a non-maxima-suppression (NMS) gather (`:607-613`).
- **Blur's device line filters are order 0 only, with black outside the line.**
  - The declaration is `Engine/Gpu/GpuBlur.h:127-128` ("the CPU's LineFilter kinds at order 0"), with the header comment at `Kernels/blur_lines.slang:1`.
  - The translation helper `toGpuLineFilter` is file-local in `Engine/Nodes/Filter/Blur.cpp:157-180`, and `lineGpuRecorder` is at `:231-257`.
  - The CPU order-1 paths are van Vliet's difference recursion (`BlurKernels.cpp:446-500`), the box's `centredDifference` (`:553`, used at `:631,694,751`), and Deriche's order-1 coefficients with Neumann initialisation (`:344-385`).
- **Erode/dilate on the device.** `gpu::recordMorphology` runs `ErodeDilateKernels::filterLine` up to radius 1024 and 4 channels (`Engine/Gpu/GpuMorphology.h:52-69`).
- **Reformat.**
  - `ReformatOp` is a matrix-form Resample without `kResamplesOnGpu` (`Engine/Nodes/Transform/Reformat.h:96-169`).
  - Transform is the model to copy: `kResamplesOnGpu` and `costHint` (`Transform.h:136,171`), with GPU tests in `Tests/Native/NativeTransformGpu_Test.cpp` (chain-region test at `:935`).
- **Crop and Position** are offset-form Resample ops.
  - Crop has `kClipsSourceToRoI` (`Crop.h:89`), a `rowMatte` closure that reads the node (`Crop.cpp:178-238`), and its own `getRegionsOfInterest` (`Crop.h:183`).
  - Position's pipeline has `PassThroughUnprocessed` (`Position.h:75-79`).
  - The CPU executor applies the matte at `ResampleExecutor.cpp:470-471`.
  - `resampleGpuSourceRoI` asks `resampleRegionsOfInterest`, not the node's virtual (`ResampleExecutor.cpp:697-721`).
- **Dissolve.**
  - It is `OpKind::Composite` with MaskMix and PassThroughUnprocessed (`Merge/Dissolve.h:67-110`).
  - Its step is `(to - from) * blend + from` in float (`Dissolve.cpp:109-127`).
  - `route()` makes an integer `which` an identity (`:82`).
- **The Shuffle family.**
  - Shuffle, ShuffleCopy, AddLayers and RemoveLayers derive from `NativeEffectBase` directly and are multi-planar (`Channel/Shuffle.h:77-78,327-328`, `AddLayers.h:65-66`, `RemoveLayers.h:67-68`). M137.P4.T2 kept them off the image pipeline on purpose.
  - The shared CPU helpers are in `namespace ChannelCopy` (`Channel/ChannelCopy.h:43-79`).
  - Shuffle fails the render when a non-colour channel is missing (`checkExtraChannelsPresent`, `Shuffle.h:~265`).
- **`NativeEffectBase`'s GPU virtuals.**
  - They are `opKind()` (empty means placement never considers the node), `canRenderOnGpu`, `getGpuInputRoI`, and `getGpuInputLayer`, which gives **one** layer per input. `renderGpu` is there too (`Engine/Nodes/NativeEffectBase.h:274-344`).
  - `GpuQuery` has no plane identity, only `nComps` (`GpuRender.h:54-101`). `GpuRenderArgs` has one view per input number and one output (`:208-226`).
- **Placement and the region engine are single-plane.**
  - `makeGpuQueryForTask` refuses `task.components.size() != 1` and an empty `opKind()` (`Engine/GpuPlacement.cpp:493-498`). It is declared at `GpuPlacement.h:97` and called by `Tests/GpuRegionFallback_Test.cpp:349` and `Tests/GpuResidency_Test.cpp:497`.
  - `GpuRegionTask::plan()` fails "the task does not render exactly one plane" (`Engine/GpuRegionTask.cpp:584-585`).
  - It asks `getGpuInputLayer` once per input edge (`:639`) and fills `GpuRenderArgs` at `:1302-1316`.
  - The strip planner calls `inputRoI(k, rect)` with `k` the index into `inputSlots` (`Engine/GpuRegionStrips.cpp:150-155`). The lambda that supplies it is in `GpuRegionTask.cpp:1404-1410`.
  - So per-layer input slots need only a k → inputNb map inside `GpuRegionTask.cpp`, and `GpuRegionStrips.cpp` stays untouched.
- **The cost model.** Without seed rows, a node falls back to the class rows: Point uses Grade's row × (inputs + 1) / 2, and Spatial uses Blur's default-sigma row (`Engine/GpuCostModel.cpp:400-401,543-554`). Measured GPU rows are hard-coded in `tools/bench/render-profile-report.py`, which per `Engine/GpuCostSeeds/inputs.txt` sits in M80's area.
- **Build globs.**
  - `Engine/Gpu/Kernels/*.cmake` is globbed with `CONFIGURE_DEPENDS` (`Engine/Gpu/CMakeLists.txt:25`), so a new kernel needs no CMake edit.
  - `Engine/Gpu/*.cpp` is an explicit list (`:33-51`), so this plan adds no new Engine/Gpu `.cpp`.
  - `Engine/Nodes/**/*.cpp` is globbed **without** `CONFIGURE_DEPENDS` (`Engine/CMakeLists.txt:43-44`), so the plan adds no new Engine `.cpp`.
  - `Tests/Native/*_Test.cpp` is globbed with `CONFIGURE_DEPENDS` (`Tests/CMakeLists.txt:140`). `Tests/Gpu/` is an explicit list, so this plan avoids it.
- **The test harness.**
  - `Tests/Native/NativeGpuParity.h` provides `createRuntime`, `runRenderGpu` (takes `StagedImageEffect&`, `:138`), `renderEndToEnd` with `ranInRegion` (`:179`) and `compareImages`.
  - The tolerance classes are `exact`, `arithmetic` 1e-6 relative, `transcendental` 1e-5 relative, `resampling` 1e-5 absolute and `iir` 1e-4 absolute (`Tests/NativeParity.h:165-169`).
  - The README table is `Engine/Nodes/README.md:1025-1031`. Each node's row lives in `Engine/Nodes/<Dir>/docs/<Node>.md`: EdgeDetect is IIR, Reformat resampling, Crop exact/resampling, Position exact, Dissolve arithmetic, and Shuffle "none recorded".
  - `Tests/Native/NativeGpuCapability_Test.cpp:62-87` is the allowlist of IDs that may answer `canRenderOnGpu()`. Every port adds its ID there.
  - The test op patterns (including `OffsetTestOp`) are in `Tests/Native/StagedResample_Test.cpp:98-232`.
  - Multi-layer fixtures exist (`Tests/fixtures/flat-seq-layers.0001.exr`, `flat-rgba-pattern.exr`).
- **Recording from node code.** A node can dispatch registry kernels directly with `ctx.dispatch(gpu::GpuKernelId::<name>, buffers, params, threads)`, as `Keyer.cpp:1432-1442` does. No runtime GLSL or pipeline-cache work is needed: every new kernel is a build-time Slang registry entry.

## Scope

**In:**
- GPU paths for EdgeDetect, Reformat, Position, Crop (including softness), Dissolve, Shuffle, ShuffleCopy, AddLayers and RemoveLayers. Each is an op body or a shared description; none is a per-node hand-written `renderGpu`.
- Order-1 and Neumann support in the device line filters, with Blur's own output unchanged.
- An offset-form Resample GPU executor and kernel.
- A separable Resample matte that both the CPU and the GPU executors read, replacing `ResampleRowMatte`.
- A multi-plane GPU API (`GpuQuery::plane`, layer inputs in `GpuRenderArgs`, `getGpuExtraInputLayers`).
- Multi-plane region members: one member per requested plane, planes the node passes through as identity members, and extra input slots per layer.
- A `ChannelRouteEffect` base, a `channelroute` kernel and a CPU route executor. The channel nodes' CPU and GPU paths both run one `ChannelRoute` description.
- A parity test and a GPU bound in the node doc for each port, plus the capability allowlist.
- A forced-region chain test, Read → Reformat → Grade → Shuffle → Write, run on lavapipe and RADV.
- README updates.

**Out:**
- Measured cost-seed rows (`*.costseed.inc`) for the ported nodes. Generating them edits `tools/bench/render-profile-report.py`. The class fallbacks apply. No owner.
- Auto-mode (cost-model) placement of the acceptance chain as a pass/fail gate. It is recorded as an observation only. No owner.
- Concatenated upstream transforms into Reformat on the GPU. They stay on the CPU, as for Transform. No owner.
- Multi-plane *outputs* of StagedImageEffect ops, beyond pass-through planes becoming identity members. No owner.
- Despill's spill map and other multi-plane-output ops. Owner: M122 - Native Colour Utilities / none.
- The strip-planner data model (`GpuRegionStrips.*`). Owner: M156 - GPU Download Throughput In Comps.
- Viewer hand-off. Owner: M107 - GPU Viewer Hand-off.
- Non-float inputs to the channel nodes on the GPU. They stay on the CPU. No owner.

**Edge cases:**
- **Neumann boundaries.** EdgeDetect's line filters reach the domain ends with Neumann (nearest) boundaries, and slot 0 reads the source as `Boundary::Nearest`. The device path fills the window by clamping coordinates to `sourceValid` before the line passes. It does not read zero there.
- **Erode radius.** An EdgeDetect erode radius above `kGpuMorphMaxRadius` has no recorder and stays on the CPU.
- **EdgeDetect settings matrix.** The parity cases are: all 8 filters × 4 multiChannel modes × NMS on/off × erode positive/negative/0, blurSize 0, `expandRoD`/`cropToFormat` off, 1 to 4 components, and a tiled window against a full frame.
- **Position's unprocessed channels** come from the *unshifted* source under the output pixel, as `runResampleCpu` does. The device finish must read that reference too, not the shifted source.
- **Crop's source clip.** Crop reads its source as zero outside the node's own region of interest (`kClipsSourceToRoI`). The device source region must come from the node's `getRegionsOfInterest` override. `resampleRegionsOfInterest` gives a different region for Crop.
- **Crop's matte.**
  - A black-border or outside pixel is *written* 0, never multiplied by 0. That keeps a NaN or infinity source pixel at 0, as the CPU's `std::fill` does.
  - Softness 0 must stay `exact()` on the GPU.
  - Soft edges compute `t = wx * wy` in double on the host. The device multiply is held to `resampling()`.
- **Dissolve.** An integer `which` is a route identity and never reaches the GPU. An unconnected source reads zero. The device blend is marked `precise`, so no fused multiply-add diverges from the CPU's float arithmetic.
- **Shuffle's missing channels.** Where the CPU fails the render on a missing non-colour channel, `canRenderOnGpu` answers false, so the CPU reports the error.
- **Colour views.** They alias the colour plane by channel bit. Bits Shuffle does not write pass through from the main input's colour plane, and a colour channel the input lacks reads 0.
- **ShuffleCopy's bbox.** Its union or intersection region of definition makes reads outside an input's bounds return 0, as on the CPU.
- **Pass-through planes.** A requested plane the node only passes through becomes an identity member on the pass-through input's slot in that layer, resolved through `getComponentsNeededAndProduced_public`. If it cannot be resolved, the whole task stays on the CPU.
- **Extra layers from in-region producers.** If a producer inside the region does not produce the extra layer a member reads, the member is refused and the region splits, as with today's layer-mismatch rule.
- **The mask-layer residency test.** `GpuResidencyTest.MaskInAnotherLayerThanItsProducersPlaneRendersOnTheCpu` (`Tests/GpuResidency_Test.cpp:~1090`) may flip once multi-plane producers are accepted. Its expectation is updated only if the GPU render then matches the CPU render.
- **Placement and recording must agree.** Every `canRenderOnGpu` answer depends only on the query and the node's settings.

**Assumptions:**
- `Reformat::getInverseTransformCanonical()` (reached via `ReformatOp::transform`) is safe to call from the placement and recording threads, since the CPU calls it on any action thread. If it is not, the Reformat task bakes the matrix inputs into `ReformatParams` instead.
- `renderEndToEnd` places every accepting node in regions without consulting the cost model ("with every node of graph whose effect accepts the task on the GPU, as regions").
- A Shuffle-family task asked for one plane by its consumer has `task.components == {that plane}` (`RenderScheduler.cpp:576` copies `componentsRequested`).
- Region slots are float, so a channel node's inputs arrive on the device as float even when the CPU fetch would not map depths.

## Phase 110.1: Independent ports and foundations

- [x] M110.P1.T1 — Run Reformat on the GPU through the matrix-form resampler
  - files: `Engine/Nodes/Transform/Reformat.h`, `Engine/Nodes/Transform/Reformat.cpp`, `Engine/Nodes/Transform/docs/Reformat.md`, `Tests/Native/NativeReformatGpu_Test.cpp` (new), `Tests/Native/NativeGpuCapability_Test.cpp`
  - approach:
    - Add `static constexpr bool kResamplesOnGpu = true;` and `static_assert(GpuResampleOp<ReformatOp>)`.
    - Add a `costHint(params, ctx)` modelled on `TransformOp::costHint` (no blur).
    - Confirm that `transform()` is safe on the placement and record threads (see Assumptions). If it is not, bake the input format, output format and source region of definition into `ReformatParams` in `bake()`, so `transform()` reads only Params.
    - Model the test on `NativeTransformGpu_Test.cpp`. Cover every resize type, center/flip/flop/turn, all 10 filters with clamp and black outside on and off, preserveBB on and off, and mipmap 0 and 1.
    - Also test eligibility: a concatenated input (`hasConcatenatedInput`) stays on the CPU, and the GPU input region equals the CPU region of interest.
    - Add `PLUGINID_NATRON_REFORMAT` (and only it) to the capability allowlist.
    - Record the GPU bound in `Reformat.md`: `resampling()`, or a measured exception with its reason.
  - verify: at the phase build, `NativeReformatGpu_Test`, `NativeReformat_Test` and `NativeGpuCapability_Test` pass on lavapipe.
  - size: M

- [x] M110.P1.T2 — Give Dissolve a GPU Composite step
  - files: `Engine/Nodes/Merge/Dissolve.h`, `Engine/Nodes/Merge/Dissolve.cpp`, `Engine/Gpu/Kernels/dissolve.slang` (new), `Engine/Gpu/Kernels/dissolve.cmake` (new), `Engine/Nodes/Merge/docs/Dissolve.md`, `Tests/Native/NativeDissolveGpu_Test.cpp` (new), `Tests/Native/NativeGpuCapability_Test.cpp`
  - approach:
    - Add `GpuOpRef gpu(const Params&) const` returning `GpuOpRef::compiled(gpu::GpuKernelId::dissolve, block)`. The block holds `blend` and is mirrored with `static_assert(sizeof...)`.
    - The kernel is an `ICompositeStep` in the style of `compositestep.slang`: `precise float4 r = (input - result) * blend + result;`, with `runCompositeStep<StepDissolve>`. Register it with `slang_add_kernel(dissolve Kernels/dissolve.slang ENTRY dissolve REGISTRY NatronGpu)`.
    - Add `static_assert(GpuCompositeOp<DissolveOp>)`.
    - Test fractional `which` between several input pairs, an unconnected second input, mask and mix, a channel subset, and 1 to 4 components.
    - Check that an integer `which` is identity and is never asked on the GPU.
    - Add `PLUGINID_NATRON_DISSOLVE` to the allowlist. Record the GPU bound (`arithmetic()`) in `Dissolve.md`.
  - verify: at the phase build, `NativeDissolveGpu_Test`, `NativeDissolve_Test` and `NativeGpuCapability_Test` pass on lavapipe.
  - size: M

- [x] M110.P1.T3 — Extend the device line filters to first-order derivatives and Neumann boundaries
  - files: `Engine/Gpu/GpuBlur.h`, `Engine/Gpu/GpuBlur.cpp`, `Engine/Gpu/Kernels/blur_lines.slang`, `Engine/Nodes/Filter/BlurKernels.{h,cpp}`, `Engine/Nodes/Filter/Blur.cpp`, `Tests/Native/GpuLineFilter_Test.cpp` (new)
  - approach:
    - Add `order` (0 or 1) and `neumann` to `GpuLineFilter` and `LinesParams`/`GpuLineBlurParams`, and update the `static_assert` size.
    - In `blur_lines.slang`, port the order-1 paths with the CPU's rounding points: Deriche's order-1 coefficients and Neumann initialisation (`BlurKernels.cpp:344-385`), van Vliet's difference recursion (`:446-500`), and the box's `centredDifference` with Neumann samples (`:525-590`). Keep the two-float state the file already uses.
    - Move `toGpuLineFilter` out of `Blur.cpp`'s anonymous namespace into `BlurKernels` as `BlurKernels::toGpuLineFilter`, under `#ifdef NATRON_HAVE_GPU`, carrying order and Neumann. `LineFilter` exposes them through `order()` and a new `neumann()` accessor.
    - Blur keeps order 0 with no Neumann, so its device output stays bit-for-bit what it is.
    - The new test records `gpu::recordLineBlur` through `createRuntime()`. It compares against `LineFilter::apply` for Deriche, van Vliet and box (1 to 3 iterations), orders 0 and 1, Neumann on and off, short lines (1 to 3 pixels) and long lines, within `iir()`.
  - verify: at the phase build, `GpuLineFilter_Test` passes on lavapipe, and `NativeBlurGpu_Test` and `BlurKernels_Test` pass unchanged.
  - size: L

- [x] M110.P1.T4 — Render offset-form Resample ops on the GPU
  - files: `Engine/Nodes/Image/Op.h`, `Engine/Nodes/Image/StagedImageEffect.h`, `Engine/Nodes/Image/ResampleExecutor.{h,cpp}`, `Engine/Gpu/GpuResample.{h,cpp}`, `Engine/Gpu/Kernels/offset.slang` (new), `Engine/Gpu/Kernels/offset.cmake` (new), `Tests/Native/StagedOffsetGpu_Test.cpp` (new)
  - approach:
    - Add the concept `GpuOffsetResampleOp`: an offset-form op with `kResamplesOnGpu`. Make `GpuResampleOp` cover both forms, or add the parallel concept, and have `opResamplesOnGpu()` answer for either.
    - Rewrite the "Neither runs on the GPU" comment at `Op.h:326`.
    - In `ResampleExecutor`, add offset branches to `canRecordResampleGpu`, `resampleGpuSourceRoI` and `recordResampleGpu`. The offset form accepts any plan that `canRunStagesGpu` takes and at most 4 components; a concatenated input is impossible for it.
    - The source region is the node's *virtual* `getRegionsOfInterest` source entry, so a node with its own region of interest (Crop) is honoured. With `clipSourceToRoI`, the source is clipped to that region.
    - Add a `gpu::recordOffset` in `GpuResample` and the `offset` kernel: `dst(x, y) = src(x - dx, y - dy)`, zero outside the valid source rect.
    - With a mask, a mix below 1 or unprocessed channels, finish through `recordStagesGpuFinish` against the **unshifted** source, matching `runResampleCpu`'s reference row.
    - Leave a request that carries a matte refused here; P2.T3 adds it.
    - The test defines a test offset op with `kResamplesOnGpu`, on the pattern of `StagedResample_Test.cpp:203`. It covers positive, negative and zero offsets, `clipSourceToRoI`, PassThroughUnprocessed, a source smaller than the window, and padded output views, all `exact()`, with `writtenOutside == 0`.
  - verify: at the phase build, `StagedOffsetGpu_Test`, `StagedResample_Test`, `NativeTransformGpu_Test` and `NativeCrop_Test` pass on lavapipe.
  - size: L

- [x] M110.P1.T5 — Add a multi-plane GPU query and render arguments
  - files: `Engine/Nodes/Image/GpuRender.h`, `Engine/Nodes/NativeEffectBase.h`, `Engine/Nodes/NativeEffectBase.cpp`, `Tests/Native/NativeGpuParity.h`, `Tests/Native/NativeGpuParity.cpp`
  - approach:
    - Add `ImageLayerDesc plane` to `GpuQuery`. It is the plane the task renders, and `nComps` stays its component count; include `Engine/ImageLayerDesc.h`.
    - Add `struct GpuLayerInput { int inputNb; ImageLayerDesc layer; GpuImageView view; }` and `std::vector<GpuLayerInput> layerInputs` to `GpuRenderArgs`. They are the extra layers an input is read in, beyond `inputs[inputNb]`, which keeps the layer `getGpuInputLayer` names.
    - Add `virtual bool getGpuExtraInputLayers(int inputNb, const ImageLayerDesc& plane, double time, ViewIdx view, std::vector<ImageLayerDesc>* layers) const`. The default leaves the list empty and returns true. Document it beside `getGpuInputLayer`.
    - Generalise `runRenderGpu`/`runRenderGpuOn` to take `NativeEffectBase&` and an optional list of extra layer images uploaded into `layerInputs`. Set `query.plane` from the caller, or to RGBA/Alpha by `nComps` when unset. Existing callers that pass a `StagedImageEffect&` compile unchanged.
    - Neither placement nor the region engine is touched here (P2.T5).
  - verify: at the phase build, every `Tests/Native/*Gpu_Test` and `StagedImageEffectGpu_Test` passes on lavapipe unchanged.
  - size: M

**Phase gate:** a single `tools/ci/remote/build.sh fast --tests` passes, including the five tasks' tests on lavapipe.

## Phase 110.2: Recorders on the foundations

- [x] M110.P2.T1 — Record EdgeDetect's pass list on the GPU
  - files: `Engine/Nodes/Filter/EdgeDetect.h`, `Engine/Nodes/Filter/EdgeDetect.cpp`, `Engine/Gpu/Kernels/edgedetect.slang` (new), `Engine/Gpu/Kernels/edgedetect.cmake` (new), `Engine/Nodes/Filter/docs/EdgeDetect.md`, `Tests/Native/NativeEdgeDetectGpu_Test.cpp` (new), `Tests/Native/NativeGpuCapability_Test.cpp`
  - approach:
    - Set `list.gpu` in `edgeDetectPasses` to a `PassGpuRecorder` in `EdgeDetect.cpp`, following `Blur.cpp`'s `lineGpuRecorder`. Dispatch through `ctx.dispatch(gpu::GpuKernelId::edgedetect_*, ...)`, with the parameter blocks mirrored by `static_assert`. The steps run in this order:
      1. Use `recordFilterWindow` for the domain.
      2. Run an `edgedetect_fill` entry that copies the source into scratch over the window with coordinates clamped to `sourceValid` (`Boundary::Nearest`).
      3. Run the line passes through `gpu::recordLineBlur`, using P1.T3's order and Neumann via `BlurKernels::toGpuLineFilter`, into the Gx/Gy scratch buffers. For the finite-difference schemes, run the pre-blur, then an `edgedetect_stencil` entry (simple, Sobel, rotation-invariant, CImg `get_gradient` in float).
      4. Run `edgedetect_combine`: the `Normalization` factors computed on the host, and separate, RMS, max or tensor in the CPU's operand order.
      5. Run `gpu::recordMorphology` for erode/dilate.
      6. Run `edgedetect_nms`.
    - Write the result to `io.filtered` over `io.output`.
    - Leave the recorder out (CPU) when the erode radius exceeds `kGpuMorphMaxRadius`.
    - Add a `costHint` as Blur's, with sigma from blurSize and iir for the Gaussian family.
    - Update the header comment ("It runs on the CPU only").
    - The test sweeps the Scope's settings matrix, on full frame and tiles, against the CPU render.
    - Add `PLUGINID_NATRON_EDGEDETECT` to the allowlist. Record the GPU bound in `EdgeDetect.md` (start from `iir()`; a measured exception needs its reason).
  - verify: at the phase build, `NativeEdgeDetectGpu_Test`, `NativeEdgeDetect_Test` and `NativeGpuCapability_Test` pass on lavapipe.
  - size: L

- [x] M110.P2.T2 — Run Position on the offset-form GPU executor
  - files: `Engine/Nodes/Transform/Position.h`, `Engine/Nodes/Transform/docs/Position.md`, `Tests/Native/NativePositionGpu_Test.cpp` (new), `Tests/Native/NativeGpuCapability_Test.cpp`
  - approach:
    - Add `kResamplesOnGpu = true` and the P1.T4 concept's `static_assert`.
    - Test integer and fractional translations (rounded as `offset()` does) at mipmap 0 and 1, a non-square pixel aspect ratio, a channel subset (PassThroughUnprocessed reads the unshifted source), and a translation that moves the source out of the window. All are `exact()`.
    - Add `PLUGINID_NATRON_POSITION` to the allowlist. Record the GPU bound (`exact()`) in `Position.md`.
  - verify: at the phase build, `NativePositionGpu_Test`, `NativePosition_Test` and `NativeGpuCapability_Test` pass on lavapipe.
  - size: M

- [x] M110.P2.T3 — Replace the row matte with a separable matte both executors read
  - files: `Engine/Nodes/Image/ResampleExecutor.{h,cpp}`, `Engine/Nodes/Image/StagedImageEffect.h`, `Engine/Nodes/Image/Op.h`, `Engine/Gpu/GpuResample.{h,cpp}`, `Engine/Gpu/Kernels/offset.slang`, `Engine/Nodes/Transform/Crop.{h,cpp}`
  - approach:
    - Replace `ResampleRowMatte` with `ResampleMatte { std::function<double(int x)> column; std::function<double(int y)> row; }` over output pixels at the render scale. The executors sample it over the window on the action or record thread.
    - The rule for a pixel weight `w = column(x) * row(y)`: `w == 0` writes 0 (never a multiply, so NaN and infinity become 0), `w >= 1` leaves the pixel, and anything else multiplies by `(float)(pix * w)` as the CPU does today.
    - The CPU loop at `ResampleExecutor.cpp:470` applies it per row.
    - `recordResampleGpu`'s offset branch uploads per-column and per-row weight tables over the window, as (hi, lo) float pairs, into `gpu::recordOffset`. `offset.slang` applies them with the same zero and one rules.
    - `ImageNode::opResample` detects `matte(params, ctx)` in place of `rowMatte`.
    - Rewrite `CropOp::rowMatte` as `CropOp::matte`:
      - The column weight is 0 on the x border column or when `blackOutside` and `dx <= 0`. Otherwise it is 1, or `rampSmooth(dx / softness)` inside the softness.
      - The row weight is the same for y.
      - This is separable because the CPU's `t = wx * wy` and every zero case is a whole row or column.
    - Crop's CPU output must stay bit-identical. If it cannot, bump `PLUGIN_MAJOR_NATRON_CROP` and record why.
    - Crop does not yet opt into the GPU (P3.T1).
  - verify: at the phase build, `NativeCrop_Test` (CPU parity against the OFX plug-in, unchanged constants), `StagedResample_Test` and `StagedOffsetGpu_Test` pass. A new case in `StagedOffsetGpu_Test` gives a test op a matte with zero, partial and NaN-source pixels and holds the GPU to `resampling()`, with the zero cases exact.
  - size: L

- [x] M110.P2.T4 — Add a channel-route description, base class and device kernel for the channel nodes
  - files: `Engine/Nodes/Channel/ChannelCopy.h`, `Engine/Nodes/Channel/ChannelCopy.cpp`, `Engine/Gpu/Kernels/channelroute.slang` (new), `Engine/Gpu/Kernels/channelroute.cmake` (new), `Tests/Native/ChannelRouteGpu_Test.cpp` (new), `Tests/Native/ChannelRouteCpu_Test.cpp` (new)
  - approach:
    - In `ChannelCopy`, add `struct ChannelRoute`:
      - `sources`: a list of `{inputNb, ImageLayerDesc layer}`;
      - per output component, either a source index plus channel index, or a constant 0 or 1.
    - Add `class ChannelRouteEffect : public NativeEffectBase`. It implements FINAL `opKind()` (Point), `canRenderOnGpu`, `getGpuInputLayer`, `getGpuExtraInputLayers`, `getGpuInputRoI` (the output rect) and `renderGpu` from one pure virtual: `bool channelRoute(double time, ViewIdx view, const ImageLayerDesc& plane, ChannelRoute* route) const`.
      - `false` means CPU.
      - The GPU takes float bit depth only. The route carries a `cpuOnly` flag for non-float sources.
      - Each source also says how a non-float plane is read, so every node keeps today's behaviour: Shuffle reads it as 0 (`Shuffle.cpp:870`), while AddLayers and RemoveLayers convert it through `copyChannels`' depth table. Unifying this is out of scope (user); see the inbox item.
      - The first source per input goes to `inputs[]`, the rest to `layerInputs`.
    - `renderGpu` records one `channelroute` dispatch per source, each writing its mapped channels and reading 0 outside the source's bounds or for a missing view. A final dispatch writes the constants. Compute barriers separate the dispatches.
    - Add `runChannelRouteCpu(effect, args, route per plane)` in `ChannelCopy.cpp`. It fetches each source as stored before taking any lock, generalises `selectCopyRow`'s depth table to one source per channel, writes the constants, and reads 0 outside a source's bounds, as the GPU does. `ChannelRouteEffect::render` is FINAL and calls it.
    - The four nodes are moved onto this base in Phase 110.3. This is a shared executor and description, so no node writes its own `renderGpu`.
    - The test effect derives from `ChannelRouteEffect` with a fixed route: two layers of one input plus a second input, with constants. It runs through P1.T5's `runRenderGpu` with extra layer images, checks `exact()` against a hand-computed reference, and checks `writtenOutside == 0`. `ChannelRouteCpu_Test` runs the same routes through `runChannelRouteCpu`, at byte, short and float, with both non-float read modes.
  - verify: at the phase build, `ChannelRouteGpu_Test` and `ChannelRouteCpu_Test` pass on lavapipe, and `ShuffleRender_Test`, `AddLayersRender_Test` and `RemoveLayersRender_Test` pass unchanged.
  - size: L

- [x] M110.P2.T5 — Let multi-plane tasks join regions as one member per plane
  - files: `Engine/GpuPlacement.h`, `Engine/GpuPlacement.cpp`, `Engine/GpuRegionTask.cpp`, `Tests/Native/GpuMultiPlaneRegion_Test.cpp` (new), `Tests/GpuResidency_Test.cpp`
  - approach:
    - **Overlaps M156: sequence it after M156's edits to these files merge, or rebase onto them.** Do not touch `GpuRegionStrips.*`.
    - **Placement.**
      - Keep `makeGpuQueryForTask(task, context, query)` (tests call it), and have it set `query->plane`.
      - Add `makeGpuQueryForTaskPlane(task, plane, context, query)` and drop the single-plane refusal there.
      - `makeRenderPlacementInputs`' `canRenderOnGpu` accepts a task when every requested plane is either accepted by `canRenderOnGpu` with that plane's query, or passed through by the node to a resolvable input (`getComponentsNeededAndProduced_public`).
    - **`GpuRegionTask::plan()`.**
      - Make one `Member` per (task, plane), each with its own query and output slot, and map `(task, layer)` to a member for `slotOfTask`.
      - A pass-through plane becomes an `identity` member onto the pass-through input's slot in that layer.
      - For each input, add slots for `getGpuExtraInputLayers`, appended to `inputSlots`, with a parallel `inputSlotInput` vector (slot index → inputNb).
      - Map the `inputRoI` lambda (`:1404-1410`) through that vector.
      - Fill `GpuRenderArgs::layerInputs` at `:1302-1316` and in the strip path.
      - Store a task's planes together when outputs are stored (`storeOutputs`, `:2233-2270`), so one `put()` per task key carries every plane.
      - Split the cost-model weight per plane by megapixels.
    - **Test.** A test effect in the test file implements `canRenderOnGpu`/`getGpuExtraInputLayers`/`renderGpu` over an existing registry kernel (`compositestep_halveadd`) and reads two layers of one input. It checks:
      - a frame where a consumer requests two planes runs both in one region (`ranInRegion`) and matches the CPU;
      - a pass-through plane is an identity member;
      - an extra layer not produced by an in-region producer splits the region.
    - Re-run `MaskInAnotherLayerThanItsProducersPlaneRendersOnTheCpu`. If the producer now joins the region and matches the CPU, update the expectation and say why in the test.
  - verify: at the phase build, `GpuMultiPlaneRegion_Test`, `GpuResidency_Test`, `GpuPlacement_Test`, `GpuRegionFallback_Test`, `RenderScheduler_Test` and every `Tests/Native/*Gpu_Test` pass on lavapipe.
  - size: L

**Phase gate:** a single `tools/ci/remote/build.sh fast --tests` passes, including the five tasks' tests on lavapipe.

## Phase 110.3: Crop and the channel nodes

- [x] M110.P3.T1 — Run Crop, softness included, on the GPU
  - files: `Engine/Nodes/Transform/Crop.h`, `Engine/Nodes/Transform/docs/Crop.md`, `Tests/Native/NativeCropGpu_Test.cpp` (new), `Tests/Native/NativeGpuCapability_Test.cpp`
  - approach:
    - Add `kResamplesOnGpu = true` and the offset concept's `static_assert`.
    - Test every extent type, `reformat` on and off, `intersect`, `blackOutside`, softness 0 (`exact()`) and above 0 (`resampling()`), a NaN source pixel on the border, a crop rectangle larger than the source, mipmap 0 and 1, a non-square pixel aspect ratio, and tiles.
    - Check that the GPU source region equals Crop's own CPU region of interest.
    - Add `PLUGINID_NATRON_CROP` to the allowlist. Add the GPU bound to `Crop.md`'s row.
  - verify: at the phase build, `NativeCropGpu_Test`, `NativeCrop_Test` and `NativeGpuCapability_Test` pass on lavapipe.
  - size: M

- [x] M110.P3.T2 — Move Shuffle and ShuffleCopy onto the channel route
  - files: `Engine/Nodes/Channel/Shuffle.h`, `Engine/Nodes/Channel/Shuffle.cpp`, `Engine/Nodes/Channel/docs/Shuffle.md`, `Engine/Nodes/Channel/docs/ShuffleCopy.md`, `Tests/Native/NativeShuffleGpu_Test.cpp` (new), `Tests/Native/NativeGpuCapability_Test.cpp`
  - approach:
    - Derive `Shuffle` from `ChannelRouteEffect`; `ShuffleCopy` inherits it.
    - Implement `channelRoute` from the CPU's own resolution: `getEffectiveSource`, `getSlotInput`/`getSlotLayer`, `getOutputLayer`, `getOutputColorBits` and `getOutputColorStorage`.
      - Colour bits not written come from the main input's colour plane.
      - A colour channel the input lacks reads 0.
    - Return false (CPU) wherever `checkExtraChannelsPresent` would fail, which stays the engine's pre-render gate, or a source is non-float.
    - Delete `Shuffle::render` and `fetchInputPlane`. The table-building code (`Shuffle.cpp:881-963`) moves into `channelRoute`, and the CPU runs the route. A non-float source plane still reads 0, as today.
    - Test with the GPU-vs-CPU harness (`exact()`):
      - rgba channel swaps with constants 0 and 1;
      - in2 from a second layer (fixture `flat-seq-layers.0001.exr` or the test multi-layer source);
      - colour views alpha, rgb and xy into rgba;
      - ShuffleCopy with "1" and "2", each bbox choice, and one input disconnected;
      - a missing non-colour channel refusing the GPU.
    - Add `renderEndToEnd` cases that check `ranInRegion`.
    - Add both IDs to the allowlist. Replace "Tolerance: none recorded." in both docs with an `exact` row and the GPU bound.
  - verify: at the phase build, `NativeShuffleGpu_Test`, `Shuffle_Test`, `ShuffleRender_Test`, `ShuffleMatrix_Test`, `ColorViewsRender_Test`, `NativeChannelNodesSpec_Test` and `NativeGpuCapability_Test` pass on lavapipe. The existing CPU suites are unchanged, so the CPU output is bit-identical. If it can't be, bump `PLUGIN_MAJOR` for Shuffle and ShuffleCopy and record why.
  - size: L

- [x] M110.P3.T3 — Move AddLayers and RemoveLayers onto the channel route
  - files: `Engine/Nodes/Channel/AddLayers.{h,cpp}`, `Engine/Nodes/Channel/RemoveLayers.{h,cpp}`, `Engine/Nodes/Channel/docs/{AddLayers,RemoveLayers}.md`, `Tests/Native/NativeLayersGpu_Test.cpp` (new), `Tests/Native/NativeGpuCapability_Test.cpp`
  - approach:
    - Derive both from `ChannelRouteEffect`, and delete both `render()`s: the CPU runs the route and keeps today's depth conversion.
    - AddLayers' route comes from `computeOutcome`:
      - an added non-colour layer is all constant 0;
      - a widened colour plane copies `inputBits` from the input's colour storage via `mapColorChannels` and writes 0 elsewhere;
      - a layer the input already carries passes through, so the region engine treats it as an identity plane.
    - RemoveLayers' route comes from the `ColorOutcome`: Narrowed copies the kept bits, Unchanged and Dropped are never rendered, and non-colour layers are pass-through only.
    - Keep AddLayers' `appendNodeContentHash` override.
    - Test GPU against CPU (`exact()`) for each outcome kind and for time-varying layers, with `renderEndToEnd` cases checking `ranInRegion`.
    - Add both IDs to the allowlist. Record the tolerance in both docs.
  - verify: at the phase build, `NativeLayersGpu_Test`, `AddLayers_Test`, `AddLayersRender_Test`, `RemoveLayers_Test`, `RemoveLayersRender_Test`, `TimeVaryingLayers_Test`, `DeepChannelNodes_Test` (the deep nodes share the `ChannelCopy` helpers) and `NativeGpuCapability_Test` pass on lavapipe, with the CPU suites unchanged.
  - size: L

- [x] M110.P3.T4 — Make a routing node present only its active input's layers, colour layout included, at each frame
  - files: `Engine/EffectInstance.{h,cpp}`, `Engine/Nodes/Image/StagedImageEffect.{h,cpp}`, `Tests/Native/NativeLayersGpu_Test.cpp`, `Tests/TimeVaryingLayers_Test.cpp`
  - approach: Add a virtual `presentsOnlyPassThroughLayers(time, view)`, default false. `StagedImageEffect` answers true for Route ops whose `route()` is Input at that time. When it is true, `getComponentsNeededAndProduced_public` takes the disabled-node branch: produce nothing, and pass through the routed input's layers at the routed time, colour layout included. A Switch routed to a colourless input then presents no colour. `NodeMetadata` stays time-invariant until the per-frame metadata milestone lands.
  - verify: at the phase build, the following all pass on lavapipe in both legs:
    - `d40ab5123`'s narrowing reverted, so the Layers GPU Switch cases vary colour by frame (rgb+spec at f1, rgba at f2) and match CPU and GPU;
    - a new `TimeVaryingLayers_Test` case: Switch present layers are RGB at f1 and RGBA at f2, and none for a colourless input;
    - the Switch, TimeVaryingLayers, scheduler-equivalence and cache-equivalence suites.
    The viewer shows an RGB frame of an RGBA-metadata Switch correctly; check it in the UAT.
  - size: M

**Phase gate:** a single `tools/ci/remote/build.sh fast --tests` passes, including the three tasks' tests on lavapipe.

## Phase 110.4: Acceptance chain, docs and the GPU host

- [x] M110.P4.T4 — Ask upstream for the planes a node passes through (runs before P4.T1)
  - files: `Engine/ParallelRenderArgs.cpp`, `Engine/ParallelRenderArgs.h` (if needed), `Tests/Native/GpuMultiPlaneRegion_Test.cpp`, `Tests/GpuResidency_Test.cpp`
  - approach: The request pass doesn't ask a node's pass-through input for the planes the node passes through, so in Read → Grade → Shuffle (Shuffle reading another layer) the Grade can't alias that plane as an identity member and stays on the CPU. When a consumer requests a plane that a node passes through (`getComponentsNeededAndProduced_public`), propagate the request for that plane to the pass-through input at the same time and view, as `renderRoI` would fetch it. Keep the CPU render's results unchanged. Then re-check `MaskInAnotherLayerThanItsProducersPlaneRendersOnTheCpu`: its pinned assertion that the source lacks the layer may now flip. Update it only if the GPU render matches the CPU, and say why in the test.
  - verify: at the phase build, a new `GpuMultiPlaneRegion_Test` case puts Grade → Shuffle (in2 from another layer of the source) in one region matching the CPU, and `RenderScheduler_Test`, `GpuResidency_Test`, `GpuPlacement_Test`, every `Tests/Native/*Gpu_Test` and the CPU render suites stay green in both legs.
  - size: L

- [x] M110.P4.T1 — Test the Read → Reformat → Grade → Shuffle → Write chain as one region
  - files: `Tests/Native/NativeGpuChain_Test.cpp` (new)
  - approach:
    - Build a NativeRead of `Tests/fixtures/flat-rgba-pattern.exr` (or a multi-layer fixture), then a Reformat with a non-identity resize and filter, a Grade with non-neutral gain, a Shuffle with a non-identity mapping (R↔B, alpha from 1, so it is not a route identity), and a NativeWrite.
    - Run `renderEndToEnd` with the Shuffle as root, as the Write pulls it.
    - Assert exactly one region, holding the Reformat, Grade and Shuffle tasks and neither Read nor Write (`ranInRegion`, `regionNodes`). Assert the GPU image matches the CPU one within `resampling()`, since Reformat's bound dominates.
    - Add a second case where the Shuffle reads in2 from another layer of the Read (multi-plane input).
    - Follow `NativeTransformGpu_Test.cpp:935`.
  - verify: at the phase build, `NativeGpuChain_Test` passes on lavapipe.
  - size: M

- [x] M110.P4.T2 — Update the native-node README for the new GPU paths
  - files: `Engine/Nodes/README.md`
  - approach:
    - **Overlaps M80**, which also edits this file. Keep the edits to these passages and rebase on M80 if it has merged.
    - Op kinds (`:782-860`):
      - the offset form runs on the GPU with `kResamplesOnGpu`;
      - `matte()` replaces `rowMatte()`, with its zero and one rules;
      - Composite has a production GPU user (Dissolve);
      - EdgeDetect's Passes recorder exists.
    - `NativeEffectBase` (`:54-80`): `GpuQuery::plane`, `getGpuExtraInputLayers`, `GpuRenderArgs::layerInputs`, and `ChannelRouteEffect` as the channel nodes' GPU path.
    - GPU parity (`:1041-1063`): point to the node docs' new GPU bounds, and note that multi-plane tasks run as one member per plane.
  - verify: the PR reviewer reads the diff against the merged code. `tools/ci/remote/build.sh fast --tests` is not needed for a doc-only change, but it runs at the milestone gate.
  - size: M

- [x] M110.P4.T3 — Verify on RADV in the natron-dev container and record the results
  - files: none (results go to this file's `## Decisions`)
  - approach:
    - On the GPU host, `NATRON_DEV_GPU=1`, in the natron-dev container, build `fast --tests`. Run with `NATRON_TEST_REQUIRE_GPU=1`:
      - every new test of this milestone (`GpuLineFilter_Test`, `StagedOffsetGpu_Test`, `ChannelRouteGpu_Test`, `GpuMultiPlaneRegion_Test`, `NativeGpuChain_Test`);
      - every `Native<Node>Gpu_Test` of the ported nodes;
      - `NativeBlurGpu_Test`, `NativeTransformGpu_Test`, `GpuResidency_Test`, `GpuPlacement_Test` and `GpuRegionFallback_Test`.
    - Then build the stripped `release` AppImage. Render a `.ntp` of the acceptance chain at UHD with `env -u LD_LIBRARY_PATH NATRON_GPU=auto NATRON_RENDER_PROFILE=<file>`.
    - Summarise the profile with `tools/bench/gpu_region_summary.py` (read-only). Record whether the cost model placed Reformat, Grade and Shuffle in one region, and the region's wall and kernel ms.
    - Compare only runs from the same boot.
    - Bench the resample kernel before and after `1b73356be`'s exact-double impulse/box entries at UHD, 8K and 24k (Transform and Reformat, impulse, box and one smooth filter), same boot. If impulse/box regressed materially, add a fast path that uses exact maths only near a decision boundary.
  - verify: every listed test passes on RADV, and Decisions lines record the Auto-mode outcome, the numbers and the resample before/after table.
  - size: M

- [ ] M110.P4.T5 — Fast path for the box (and impulse) resample: exact maths only near a decision boundary
  - files: the GPU resample kernel that `216e3028f`/`7e777a7d8` changed (find it from those commits), `Tests/Native/NativeTransformGpu_Test.cpp`, `Tests/Native/NativeReformatGpu_Test.cpp` (only if a boundary case needs a new assertion)
  - approach: P4.T3's bench (`.local/m110/p4t3/resample-1a6741d9/table.txt`) shows the exact emulated-double sample positions make box kernels 2.3–5.8× slower than before `216e3028f` (Reformat UHD 0.55 → 2.44 ms, 8K 1.54 → 8.98; Transform 24k 50 → 113), and impulse up to ~1.5×. Cubic is unchanged. Compute the sample position in the two-float path first, and fall back to the exact emulated-double computation only when the result lies within a small, provable bound of a rounding or footprint decision boundary (the tie the exact maths exists to get right). Derive the bound from the two-float error, and state it in a short comment. CPU parity must stay bit-for-bit where the exact path made it so.
  - verify: `NativeTransformGpu_Test` and `NativeReformatGpu_Test` pass on lavapipe (local `fast --tests`) and on RADV with `NATRON_TEST_REQUIRE_GPU=1`; then rerun `.local/m110/p4t3/resample_bench.sh` (cand = new release AppImage, base = `216e3028f^`'s) on one boot, with box and impulse kernel ms within ~15% of the base.
  - size: L

**Verification gate:**
- `tools/ci/remote/build.sh fast --tests` passes with no regressions on lavapipe. That includes every new test: `NativeReformatGpu`, `NativeDissolveGpu`, `GpuLineFilter`, `StagedOffsetGpu`, `NativeEdgeDetectGpu`, `NativePositionGpu`, `ChannelRouteGpu`, `GpuMultiPlaneRegion`, `NativeCropGpu`, `NativeShuffleGpu`, `NativeLayersGpu` and `NativeGpuChain`. It also includes the unchanged CPU parity suites (`NativeCrop`, `NativeEdgeDetect`, `NativeReformat`, `NativeDissolve`, `NativePosition`) and `NativeGpuCapability_Test` with the nine new IDs.
- M110.P4.T3's RADV run passes with `NATRON_TEST_REQUIRE_GPU=1`, and `NativeGpuChain_Test` shows Reformat, Grade and Shuffle as one region between the Read and the Write.
- Each ported node's doc states a GPU tolerance.
- `git-clang-format` is clean.
- The GUI bench rule does not apply: no `Gui/NodeGraph*`, `Gui/NodeGui*`, `Gui/Edge*`, `Engine/Knob*` or `Engine/NodeGroup*` file is in the diff.

## Decisions

- 2026-10-23 — **Elaborated at claim by a consultant**, scouted on bf781cbcd.
- 2026-10-23 — **The Shuffle family runs on the GPU through a shared `ChannelRouteEffect` base** (in `ChannelCopy`), whose FINAL GPU virtuals record a node-supplied `ChannelRoute` with one `channelroute` kernel. M137.P4.T2 kept these nodes off `StagedImageEffect`, so a Point/Composite body does not fit. A shared description and executor keeps to the rule that no node hand-writes `renderGpu`. ~~Their CPU `render()` stays as it is.~~ Superseded below: the CPU runs the route too.
- 2026-10-23 — **Multi-plane tasks become one region member per plane.** Planes the node passes through become identity members, and extra input layers become extra input slots. This keeps `Member::outSlot` singular, so `GpuRegionStrips.*` (M156) is not touched. The overlap with M156 is confined to M110.P2.T5.
- 2026-10-23 — **Crop's `ResampleRowMatte` closure gives way to a separable `ResampleMatte` that both executors read.** The rule is one description and two executors. Crop's matte is a product of a column weight and a row weight, every zero case is a whole row or column, and zero is written rather than multiplied, which matches the CPU on NaN and infinity. Crop's CPU output must stay bit-identical; otherwise its plugin major is bumped (hard cut).
- 2026-10-23 — **Dissolve is ported as a GPU Composite step, not the stub's "multi-input point op".** M137 made it `OpKind::Composite`, and `GpuCompositeOp` is the framework's route for that kind.
- 2026-10-23 — **No cost-seed rows are added.** The new nodes use the Point and Spatial class fallbacks. The acceptance chain is gated on the forced-region test on RADV, and the Auto-mode placement on RADV is recorded but does not gate. Measured GPU rows would edit `tools/bench/render-profile-report.py`, which is in M80's area.
- 2026-10-23 — **No new `.cpp` under `Engine/` or `Engine/Gpu/`.** `Engine/Nodes/**/*.cpp` is globbed without `CONFIGURE_DEPENDS`, and `Engine/Gpu` sources are an explicit list. Recorders live in existing files (`GpuBlur.cpp`, `GpuResample.cpp`, `EdgeDetect.cpp`, `ChannelCopy.cpp`), and kernels come in through the globbed `Kernels/*.cmake`.
- 2026-10-23 — **Acceptance is the forced-region chain test** (user). `NativeGpuChain_Test`, on lavapipe in CI and on RADV, gates "runs as one region". Auto-mode placement on RADV is recorded as an observation only, and measured cost-seed rows are a follow-up.
- 2026-10-23 — **M110 and M156 - GPU Download Throughput In Comps run together** (user). Only M110.P2.T5 overlaps M156 (`Engine/GpuRegionTask.cpp`, `Engine/GpuPlacement.*`). It waits for M156's edits to merge, or rebases onto them; every other M110 task proceeds.
- 2026-10-23 — **Pending the user:** the Shuffle-family framework (`ChannelRouteEffect` vs a multi-plane op kind) and the multi-plane breadth (widen to any node vs channel nodes only). The user asked for a detailed explanation. These gate P2.T4, P2.T5, P3.T2 and P3.T3; Phase 110.1 proceeds.
- 2026-10-23 — **M110.P1.T1 code in (`c99bdccfb`), awaiting the phase build.** `transform()` is safe on the placement and record threads, because it is the same code `runResampleCpu` already calls on render threads, so nothing is baked into `ReformatParams`. `costHint` returns a default hint. Syntax-checked only. Risk: turn and flip cases on a 50x20 box may land samples on pixel edges, as the Transform test avoided by shifting off-grid.
- 2026-10-23 — **M110.P1.T2 code in (`eb987e79d`), awaiting the phase build.** The operand order is checked: the composite fold passes `result` as from (base) and `input` as to. No framework gap was found, since the Composite path already handles mask, mix, pass-through and unconnected inputs. The open risks for the build: whether `resolveGpuStagePlan` accepts 1–3 components when the node's connected sources are RGBA, and whether `dissolve.slang`'s imports of `pixelwrap` and `compositestep` resolve.
- 2026-10-23 — **Multi-plane regions are widened to every node** (user: "Definitely want to widen so GPU regions can handle multilayer processing, that's key"). Any GPU-capable node may join a region per plane, with pass-through planes as identity members and extra input layers as extra slots, not only the channel nodes. The guards stay: a task whose pass-through can't be resolved stays on the CPU, and `MaskInAnotherLayerThanItsProducersPlaneRendersOnTheCpu` changes its expectation only if the GPU render matches the CPU. This settles M110.P2.T5's breadth.
- 2026-10-23 — **The Shuffle-family framework narrows after widening.** Each region member renders one plane, so a channel node needs an op that reads several layers and writes one plane, not a multi-plane-output op kind. A consultant is comparing "B-lite" (`StagedImageEffect` ops declare extra input layers, so Shuffle is a Point op) against a `ChannelRoute` base whose CPU path also runs from the route. The answer goes to the user. It gates P2.T4, P3.T2 and P3.T3.
- 2026-10-23 — **M110.P1.T3 code in (`8826682eb`), awaiting the phase build.**
  - **Params:** the block appends `order` and `neumann` at offsets 88 and 92, for 96 bytes, confirmed by slangc reflection.
  - **Ports:**
    - Deriche's order 1 differs from order 0 only in its coefficients. Neumann initialises from the boundary samples × `coefp`/`coefn`.
    - van Vliet order 1 is a new function that keeps the CPU's boundary-value quirk at positions 0 and 1.
    - The box at order 1 writes `(n − p) * 0.5`, rounded as the CPU does.
  - **Blur:** everything sits behind `order != 0 || neumann`, so Blur's device output should stay bit-identical.
  - **Test:** `GpuLineFilter_Test` runs ~1,300 small submits on lavapipe.
  - **Watch:** van Vliet σ 30 at order 1 sits near the 1e-4 bound.
  - **Checked:** `-fsyntax-only`, and `slangc` from m107's `.natron-deps`.
- 2026-10-23 — **The Shuffle family uses option C** (user): one `ChannelRoute` description, run by both a CPU and a GPU executor in `ChannelCopy`, and the four nodes' hand-written `render()`s go. The consultant's comparison ruled out B-lite (`StagedImageEffect` ops reading extra layers). The staged pipeline reads sources in the plane being rendered and mapped to the clip (`StagePlan.cpp:641-644`), `RowIO` has one `nComps` (`PixelKernel.h:69-80`), `getGpuInputLayer` is FINAL, and the stage stack is float-only (`StagedImageEffect.cpp:63-66`). B-lite would need ~700–900 framework lines and lose AddLayers' and RemoveLayers' non-float support. P2.T5's `getGpuExtraInputLayers` is the engine mechanism either way. CPU output must stay bit-identical, or the plugin major is bumped.
- 2026-10-23 — **Non-float source planes keep each node's current behaviour** (user): Shuffle reads them as 0, AddLayers and RemoveLayers convert them. How non-float planes should be handled, perhaps as their own kind beside deep and image, is a bigger question and out of scope. It is filed to the inbox for investigation.
- 2026-10-23 — **M110.P1.T4 code in (`65f1f23e8`), awaiting the phase build.**
  - **Concepts:** `GpuResampleOp` now covers both forms, narrowed by `GpuMatrixResampleOp` and `GpuOffsetResampleOp`.
  - **Source RoI:** `resampleGpuSourceRoI` takes the source entry from the node's own `getRegionsOfInterest_public()`, for both forms (unchanged for Transform and Reformat), and drops its unused `request`/`stages` params. `StagedImageEffect.cpp` call sites are updated, outside the brief.
  - **Kernel:** `offset` copies raw bits, with the host computing the in-source thread range in 64-bit.
  - **Finish:** mask, mix and unprocessed channels finish against the unshifted, RoI-limited source view.
  - **Risk for P2.T2 (Position):** with pass-through, the CPU reads unprocessed channels from whatever image it fetched, possibly a cached one larger than the RoI. The GPU reads 0 outside the RoI. The test bypasses the cache. The durable fix is adding the window to the RoI under pass-through, which changes the CPU fetch.
  - **Checked:** `-fsyntax-only` and slangc.
- 2026-10-23 — **M110.P1.T5 code in (`d947df339`); Phase 110.1 build dispatched.** `GpuQuery::plane` defaults to the none layer, and nothing hashes or compares `GpuQuery`. `getGpuExtraInputLayers` is inline on `NativeEffectBase`, so `NativeEffectBase.cpp` is unchanged. `runRenderGpu` takes `NativeEffectBase&` plus layer images uploaded at slots after the output, and finds the mask input by `dynamic_cast`. Checked with `-fsyntax-only` on 28 TUs.
- 2026-10-23 — **Phase 110.1 build (`d947df339`, run 40054275558, lavapipe): P1.T2–P1.T5 done, and P1.T1 fails one entry.** Every other ctest entry passes in both legs, none of the new suites skipped: `NativeDissolveGpu`, `GpuLineFilter`, `StagedOffsetGpu`, `NativeGpuCapability`, `NativeReformatGpu` (16 cases), plus the unchanged `NativeBlurGpu`, `BlurKernels`, `StagedResample`, `NativeTransformGpu`, `NativeCrop` and `StagedImageEffectGpu`. Blur's GPU output is therefore unchanged by P1.T3's line-filter work.
  - **The failure:** `Filters/NativeReformatGpuFilterTest` fails for impulse (filter 0) and box (filter 1) only. Impulse fails every fit, scale and distort-turn case; box fails fit at mipmap 0/1 and distort-turn at mipmap 0. Worst errors are 0.05 at mipmap 0 and 0.98 at mipmap 1. Filters 2–9 pass.
  - **Suspected cause:** samples tie exactly on pixel edges, and the GPU rounds them unlike the CPU. Exact-scale Reformats are common, so a consultant is diagnosing and fixing it in the kernel rather than shifting the test off-grid.
  - **Lane note:** an implementer container left the lane's `build/` root-owned, which blocked the log download. Fixed.
- 2026-10-23 — **M110.P2.T1 code in (`4444e6668`), awaiting the phase build.**
  - **Kernels:** four entries (`edgedetect_fill`, `_stencil`, `_combine`, `_nms`) with a 104-byte push block. Combine and NMS emulate the CPU's double arithmetic in two-float, with no fma. The stencil uses `precise` and should match the CPU bit for bit.
  - **Scratch:** 3 buffers, or 4 with erosion plus NMS.
  - **Accepted deviation (PM):** `PassGpuIO` gains `std::bitset<4> channels`, set from `plan.processChannels` in `recordPassesGpu` (`Engine/Nodes/Image/PassExecutor.{h,cpp}`, one line each). Without it, RMS, Max and Tensor would mix unprocessed channels into the result on the default RGB-on-RGBA setting.
  - **Test:** a ~250-submit sweep.
  - **Watch:** NMS near-ties after a blur above 0, and the nearest-pixel fill's clamp (`src.bounds ∩ input RoD` versus the CPU's fetched bounds).
- 2026-10-23 — **M110.P2.T2 code in (`6526069fa`), awaiting the phase build.** Position opts in with `kResamplesOnGpu`, and its GPU bound is `exact()`.
  - **Pre-existing CPU cache dependence, found by reading the code:** under `PassThroughUnprocessed`, a pixel whose unshifted position falls outside the RoI reads the CPU's *fetched* image. That is 0 with an exact-RoI fetch, but real pixels when the cache holds a larger image from an earlier render. So the CPU's unprocessed channels at the window edge depend on cache state, while the GPU always reads 0. Tests bypass the cache and agree.
  - **Fix options, for a later call:** add the window to the RoI under pass-through, which changes the CPU fetch, or clip the CPU's reference read to the RoI. Not fixed here.
- 2026-10-23 — **The Reformat impulse/box failure was a tie-breaking difference, fixed in `1b73356be`** (consultant).
  - **Cause:** the CPU computes `fx = H00*(x+0.5) + H01*(y+0.5) + H02` in double without fma (`Resampler.cpp:705-707`), then floors (`:227`). The GPU used two-float, about 48 bits, exact only with fused fma.
  - **Why Reformat hits it:** its matrices put many samples exactly on integers. Fit 64x48→50x20 has `fx = 2.4(x+0.5) − 28`, so every x ≡ 2 mod 5 is a tie. The 0.98 worst error is a one-row flip in a stepped test channel.
  - **Fix:** `resample.slang` emulates exact IEEE double add and multiply-by-(k/2) with 32-bit integer operations. Every discontinuous decision uses the exact value: the impulse/box cell, the inside tests, box corners, the degenerate check, the floors and `outsideBounds`. `GpuResample` passes the matrix as `hBits[12]` (still 112 bytes).
  - **Evidence:** a C++ harness of the slang source against the real `Resampler.cpp` shows 0 mismatches over 10 matrices × 2 mipmaps × 10 filters × clamp/black-outside. The old kernel fails 12 of 20, impulse and box only. Transform's rotations match under both kernels.
  - **Open, perf:** every resample pixel now does integer-emulated double maths (Transform too), possibly several times slower at 24k, the user's target spatial-op workload. To do: bench the resample kernel old vs new at UHD/8K/24k on the GPU host once it's free. If it regressed, add a fast path that switches to exact maths only near a decision boundary.
- 2026-10-23 — **M110.P2.T4 code in (`d16793a24`), awaiting the phase build.**
  - **Route:** `ChannelRoute{sources[{inputNb, layer, nonFloat: ReadsZero|Converts}], components[Constant|Channel|ColorBit], cpuOnly}`. A colour-bit component is resolved against the layout the source is actually fetched in.
  - **Executors:** `runChannelRouteCpu` fetches each (input, layer) once before locking, then `applyChannelRoute` copies at byte, short or float. The GPU makes one `channelroute` dispatch per source plus one for constants, copying raw bits. Its push block is 80 bytes.
  - **Accepted deviations (PM):**
    - `channelRoute` is non-const, because the nodes' helpers aren't. The base's const GPU methods `const_cast`, as `StagedImageEffect::resolveGpuPlan` does.
    - `false` means the plane can't be described: the GPU refuses and the CPU render fails. "Stay on the CPU" is `route.cpuOnly`. **Binding on P3.T2/P3.T3:** a node returns `false` only where today's CPU render would fail, never where it succeeds.
  - **Mapping for P3:** in the implementer's report, kept for P3's briefs. Shuffle must set `cpuOnly` from each source's *stored* depth, because region slots are float.
  - **Test gap:** `ChannelRouteCpu_Test` drives `applyChannelRoute` on hand-built images. The fetch half is covered by the existing render suites once P3 moves the nodes.
- 2026-10-23 — **M110.P2.T3 code in (`1937f850e`); Phase 110.2 build of P2.T1–T4 dispatched (P2.T5 waits on M156).**
  - **Separability holds:** every old zero case is one-axis (border, or `blackOutside` with `dx <= 0`/`dy <= 0`), and the fade was already `wx * wy` in double.
  - **The GPU matte is two-float, not exact-double.** A (hi, lo) pair can't carry an arbitrary double weight, and the multiply is continuous in the weight, so partial pixels are held to `resampling()`. The zero and ≥ 1 cases are exact.
  - **Crop is bumped to plugin major 3 (hard cut).** With `blackOutside` off and softness above 0, a pixel centre exactly on the crop edge has a ramp weight of 0. That pixel is now *written* 0, where it used to be multiplied by 0, which gave NaN for a NaN source and -0 for a negative one. `NativeCrop_Test` never puts a centre on an edge, so it should be unaffected.
  - **Outside the brief:** `Engine/Nodes/README.md`'s offset-form paragraph is updated, and `GpuOffsetParams` grows 44 → 52 bytes with a weights binding at slot 2.
- 2026-10-23 — **M110.P3.T1 code in (`4734ee2fa`), committed locally and pushed after the Phase 110.2 build finishes.** Crop opts in with `kResamplesOnGpu`. The test runs a 19-case table, about 300 GPU renders: every extent type, reformat on and off, intersect, `blackOutside`, three softnesses, and fractional and half-pixel edges, at aspect ratio 1 and 2 and mipmap 0 and 1, plus tiles, a NaN border case and end-to-end. Risk: whether a Constant node passes NaN through. The test asserts NaN reaches the output, so it would fail loudly.
- 2026-10-23 — **M110.P2.T5 is built now, on M110's branch** (user). Whichever of M110 and M156 - GPU Download Throughput In Comps merges second rebases over the other's `GpuRegionTask.cpp` edits. M156's are profile/timestamp plumbing; M110's are `plan()`, members, input slots and `storeOutputs`. This replaces the earlier "P2.T5 waits for M156's edits to merge".
- 2026-10-24 — **Phase 110.2 build red** (remote `fast --tests` of `1937f850e`, run 40057906736, both legs). Failing: `GpuResidencyTest.MemberWithoutAGpuPathRendersTheRegionOnTheCpu`, `NativeEdgeDetectGpuTest.MatchesTheCpuRender`, three `NativePositionGpuTest` cases, `ChannelRouteCpuTest` (timeout), and `NativeTransformGpuTest` in one leg. A consultant is diagnosing and fixing them in the lane, without GPU runs, because M107.P6.T3's bench holds the host. P3.T1 (`4734ee2fa`) rides the same rebuild.
- 2026-10-24 — **Phase 110.2 fixes in (`f0f9d0c3c`, `9b8a816b1`), awaiting the rebuild** (consultant; tolerances unchanged).
  - **Transform was a real regression from `1b73356be`:** the exact-double emulation in every resample filter grew the kernel about 5× and made it 15–30× slower on lavapipe (motion-blur case over the 30 s submit timeout). Exact maths now runs only in new `resample_exact_main`/`resample_blur_exact_main` entries for impulse and box. Bilinear to notch return to the two-float code that passed in Phase 1. Motion-blur SPIR-V shrinks from 2.1 MB to 245 KB.
  - **Test bugs, code fine:** GpuResidency's CPU-only member is now Merge (EdgeDetect has a GPU path). Position's parity window uses the source's real pixel region (PAR ≠ 1), expects zeros where the CPU produces no plane, and sets channels through `channels`. `ChannelRouteCpu_Test`'s `fillTyped` deadlocked by calling `getBounds()` under its own write lock.
  - **EdgeDetect:** recursive filters with suppression after erosion or `expandRoD` flip pixels on near-ties within `iir()`'s tolerance, so those cases now use no erosion, or no suppression. Simple and box filters still cover suppression after erosion.
  - **Still owed:** impulse and box (draft mode forces impulse) still pay for the exact maths. The old-vs-new resample bench on the GPU host stands, after M156.P1.T2.
- 2026-10-24 — **Phase 110.2 build green for P2.T1–P2.T4** (remote `fast --tests` of `9b8a816b1`, run 40091898992, both legs; 342 of 343 entries pass). Transform, EdgeDetect, Position, ChannelRoute and GpuResidency all pass with the fixes. The one failure is P3.T1's new `NativeCropGpuTest` (`CropMatchesTheCpuRender`, `InputRegionIsTheNodesRegionOfInterest`, `NaNSourceOnTheBorderIsWrittenZero`, both legs), sent to a consultant. P2.T5 is being implemented in the lane.
- 2026-10-24 — **M110.P2.T5 code in (`dd776c096`), awaiting the build with the Crop fix.**
  - **One plane classifier:** `makeGpuTaskPlanes` sorts a task's planes into rendered, pass-through and absent. Placement and `plan()` share it, so they always agree.
  - **Per plane:** each rendered plane is a `Member`. A pass-through plane shares the in-region producer's slot, or, when the producer is outside the region, an external slot. Extra input layers are appended slots, with parallel input-number and layer vectors. Whole and strip paths fill `layerInputs` through one helper.
  - **Storing:** `storeOutputs` makes one `put` per task with every plane. A task goes all-RAM if any plane is downloaded.
  - **Cost:** kernel cost is split evenly per plane, and `memberNs` is summed back per task.
  - **Accepted deviations (PM):**
    - The test effect is built on `ChannelRouteEffect`, because `compositestep_halveadd` is not in the registry.
    - `Tests/Native/NativeGpuParity.cpp`'s `placeOnGpu` uses the new classifier, so parity tests can place multi-plane tasks.
    - `resolveInputs` no longer falls back silently to `components.front()`, only between colour layouts.
  - **`MaskInAnotherLayerThanItsProducersPlaneRendersOnTheCpu` keeps its expectation:** nothing asks the source for the producer's extra layer, so it can't be aliased. A new assertion pins that reasoning.
  - **Risk for P4.T1's acceptance chain:** the request pass does not ask upstream for planes a node passes through, so in Read → Grade → Shuffle (another layer) the Grade can't alias the plane and stays on the CPU. Fixing that means propagating pass-through planes in `Engine/ParallelRenderArgs.cpp`, outside P2.T5's files. Decide at P4.T1.
  - **Coverage gap:** the strip path with extra layer slots is untested, because the 64×48 parity source never strips.
- 2026-10-24 — **Crop GPU fix in (`cc4a0f96e`), building with P2.T5** (consultant).
  - **Cause:** Crop doesn't support multiple clip pixel aspect ratios, so its source clip reports the output's PAR. The CPU renders the source at the source's own PAR (`RenderScheduler.cpp:571-573`), then clips at the clip's PAR, keeping the overlap. The GPU converted only at the clip's PAR. `resampleGpuSourceRoI` and the source-RoD clip now use the source's own PAR, and `clipSourceToRoI` also clips at the clip's PAR. This changes nothing when the two PARs match, which is every node but Crop.
  - **Possible engine bug, follow-up:** the CPU drops those pixels because render and clip disagree on PAR. With several consumers, the result may depend on what else reads the source. The GPU matches the CPU reference. Candidate for M31 - Architectural Cleanup.
  - **Test fixes:** empty expected regions compare by emptiness only. The NaN case was really +inf, because a knob clamp turns NaN into `DBL_MAX`. It is now Constant(+inf) × Multiply(0), which covers both NaN and infinity written as 0.
- 2026-10-24 — **M110.P3.T2 code in (`6d6e9556b`), awaiting the phase build.**
  - **The CPU runs the route:** `Shuffle::render` and `fetchInputPlane` are gone. Per component, the route resolves exactly as the old render did. A source is a constant, a channel of a named layer, or a colour bit of the input's colour plane. Colour bits not written come from the main input. Missing inputs, layers and channels read 0. Every source is `eNonFloatReadsZero`. Read as bit-identical, so no plugin-major bump; the CPU suites at the build are the proof.
  - **Never returns false.** `cpuOnly` is set when `checkExtraChannelsPresent` fails, which the engine still reports, and when a read input is non-float at either its fetched or its stored depth. That is conservative.
  - **Test coverage gap:** ShuffleCopy with input 2 (the main input) disconnected has no GPU parity case.
- 2026-10-24 — **P2.T5 and P3.T1 done, so Phase 110.2's gate is green.** Remote `fast --tests` of `cc4a0f96e` (run 40094020594) passes all 344 entries in both legs. `GpuMultiPlaneRegionTest` and `NativeCropGpuTest` ran, with no skips in either suite.
- 2026-10-24 — **M110.P3.T3 code in (`6ce6924b8`); the Phase 110.3 build is dispatched.**
  - **Routes:** both nodes run on the CPU from their route. AddLayers: a widened colour plane is colour bits plus constant 0, an added layer is all 0, and layers already present pass through as identity members. RemoveLayers: Narrowed copies the kept bits, Unchanged and Dropped are never rendered, and non-colour layers pass through. Sources use `eNonFloatConverts`.
  - **Gates:** neither node ever returns false. `cpuOnly` is set for a non-float input, the same rule as Shuffle.
  - **CPU output:** expected to stay bit-identical, so no plugin-major bump.
  - **Risk:** a task whose only plane is a pass-through has to place as an identity member (the RemoveLayers → Grade end-to-end case).
  - **Gap:** the non-float `cpuOnly` path is untested for the whole family, because the parity source is float only.
- 2026-10-24 — **M110.P4.T4 inserted before P4.T1** (PM). P2.T5's report showed that the request pass never asks upstream for pass-through planes, so P4.T1's multi-plane chain case would split at the Grade. This is in scope under the user's "widen multi-plane regions to every node". `Engine/ParallelRenderArgs.cpp` overlaps only M64 - Tiled Rendering, which is parked; it rebases over these lines if it resumes, as already agreed for `RenderScheduler.cpp`.
- 2026-10-24 — **M110.P4.T4 code in (`909d7a465`), awaiting the build.**
  - **Mechanism:** `mergeRequestedComponents` works through a queue (node, time, view, mipmap, layers). For each layer newly requested of a non-identity node, `queuePassThroughPlanes` forwards the pass-through layers to the pass-through input, following `renderRoI`'s produced/pass-through rule. It forwards only along an edge the request already has, and only planes some consumer requested, so no task or edge is added.
  - **CPU and cache:** `componentsRequested` feeds only `task.components`, and hashes and cache keys never read it. The CPU now finds the plane in the frame store rather than pulling it unplanned, so the output is identical.
  - **Mask-layer test flipped,** as the Scope allows, and renamed `MaskInAnotherLayerThanItsProducersPlaneIsReadThroughItsPassThrough`. It now expects a GPU region matching the CPU. The expectation is unproven, and it reverts if the build shows a mismatch.
  - **New case:** `ShuffleReadsAnotherLayerThroughAGradeInOneRegion`.
  - **Risk:** an upstream node now carries pass-through planes, and if it can't resolve them it stays on the CPU, where before it went to the GPU colour-only.
- 2026-10-24 — **Phase 110.3 build (`6ce6924b8`, run 40095221631): P3.T2 done, P3.T3 red.** 345 of 346 entries pass in both legs, including `NativeShuffleGpuTest` and every CPU suite for the Shuffle and Layers families, so the CPU output is unchanged. `NativeLayersGpuTest` fails `AddLayersFollowsTimeVaryingLayers`, `RemoveLayersFollowsTimeVaryingLayers` and `RunsInARegionEndToEnd`. A consultant is fixing them against HEAD, which includes P4.T4.
- 2026-10-24 — **M110.P4.T1 code in (`08d60e522`), building with P4.T4 and the Layers fix.**
  - **Case 1:** `flat-rgba-pattern.exr` → Reformat (27x21, distort, keys) → Grade → Shuffle (R↔B, alpha 1).
  - **Case 2:** `flat-three-layers.exr`, with the Shuffle's green from `specular.B`. It needs P4.T4 to forward `specular` through two hops (the Reformat and the Grade).
  - **Assertions:** exactly one region of exactly those three nodes, the Read and the Write outside it, one GPU run each, and parity within `resampling()`.
- 2026-10-24 — **NativeLayersGpu failures diagnosed** (consultant).
  - **Time-varying cases: test fixed in `d40ab5123`.** A Switch's colour layout comes from metadata, which is the same at every frame and the widest of its inputs (`EffectInstance.cpp:6254-6305`). So only non-colour layers can vary per frame. The tests now vary spec only.
  - **RemoveLayers → Grade is a real request-pass gap.** A node that only passes a plane through, and needs nothing from that input, gets no request edge (`ParallelRenderArgs.cpp` `visitRequestPassItem`). P4.T4 can't forward along a missing edge, so the node stays on the CPU, which pulls the plane unplanned.
  - **Next:** a consultant is choosing between adding the edge when a requested plane resolves to a pass-through, and adding it lazily, then implementing the choice. Accepting CPU placement is the fallback only if both are unsound.
- 2026-10-24 — **Pass-through-only edges: lazy edge added (`5355cb3b9`), built with P4.T4, P4.T1 and the Layers test fix** (consultant).
  - **Mechanism:** `queuePassThroughPlanes` adds an edge to the pass-through input only once a consumer asks the node for a plane that resolves to a pass-through. Three conditions must hold: the input was walked at that time and view, the input's RoI contains the node's RoI, and the input's post-order is lower. `dfsPostOrder` is only a tie-break, and `buildGraph` rebuilds dependencies from the edges.
  - **Why not an eager edge:** it can't know which planes consumers will ask for at visit time, and adding it anyway would add a source task that renders every output component.
  - **Effect:** ordinary single-plane graphs gain no edge. Where an edge is added, it replaces an unplanned pull, so pull counts only go down and CPU output and cache keys are unchanged.
  - **New test:** `AddLayersPassesColourThroughBesideTheLayerItAddsInOneRegion`.
  - **Not covered:** a pass-through node as the render root stays an unplanned pull.
- 2026-10-24 — **A Switch must pass through only its active input's layers, colour layout included, at each frame** (user). Today it presents the widest colour layout of all its inputs at every frame, because metadata is time-invariant. That is a bug, not a constraint to test around. `d40ab5123`'s Layers tests, which vary only non-colour layers, are a stopgap. A consultant is scoping the fix: one M110 task, or its own milestone. Once it lands, the tests go back to varying the colour layout by frame.
- 2026-10-24 — **Node metadata must not be time-invariant** (user: "Node metadata should absolutely not be the same at every frame, there's no reason that should be the case"). This widens the Switch ruling: components and colour layout, PAR, fps, premult and the rest should be per frame for every node. It is architecture work beyond M110's scope, so the consultant is scoping it as its own milestone, with the Switch fix possibly riding on it. The new milestone goes to `/cat-plan` through the inbox.
- 2026-10-24 — **M110.P3.T4 inserted (PM, consultant-scoped)** to apply the user's Switch ruling now. Per-frame layers go through the components path, which is already keyed on (hash, time, view). This needs no time-varying `NodeMetadata`, so it ships ahead of the per-frame metadata milestone, which goes to `/cat-plan` through the inbox. Remaining gap until that milestone: the Switch's metadata, which the Viewer, Write and OFX clips read, still declares the widest input.
- 2026-10-24 — **Resumed by the gpu PM after a host crash (~09:16) and a reboot onto different hardware** (boot `d728f1eb`, RX 7900 XTX / Navi 31, ~193 GB available). The crash left the plan worktree's newest commit object empty locally; it had already been pushed, so the fetch restored it and nothing was lost. Rebased onto main `d82d560f6` (M159) as `23c4a5a83`; only `Engine/Nodes/README.md` is shared, and it merged cleanly.
- 2026-10-24 — **Remote `fast --tests` of `5355cb3b9` (run 40096925163): 346 of 347 entries pass in both legs, so P4.T4 and P4.T1 are done.** `NativeGpuChainTest` (both cases, one region between the Read and the Write), `ShuffleReadsAnotherLayerThroughAGradeInOneRegion`, the flipped `MaskInAnotherLayerThanItsProducersPlaneIsReadThroughItsPassThrough` and `NativeLayersGpuTest` pass. The one failure is the lazy-edge commit's new `GpuMultiPlaneRegionTest.AddLayersPassesColourThroughBesideTheLayerItAddsInOneRegion`: AddLayers and Shuffle are placed in a region, but it falls back to the CPU. A fixer is on it. P3.T3 closes with that fix.
- 2026-10-24 — **M110.P1.T1 checked off** (PM). Its verify has passed in every build since `9b8a816b1`. The owed resample bench for the exact-maths impulse/box entries moves into P4.T3's GPU-host run, so it isn't lost.

- 2026-10-24 — **AddLayers region fallback fixed in the engine (`cdc20e327`), awaiting the remote build.** The constant plane AddLayers adds names no source on its input, but its member still took a slot in that input's extra layer. The source never renders that layer, so the run's frame-store lookup failed and both nodes fell back. `NativeEffectBase::readsGpuInput(inputNb, plane, time, view)` (default true) is overridden by `ChannelRouteEffect` to say whether the plane's route reads that input, and `GpuRegionTask::planInputs` gives an unread input no slot (`-1`, handled like an unconnected mask). Placement, `canRenderOnGpu`, the CPU render and hashes are untouched; `ChannelRouteGpuTest.InputLayersFollowTheRoute` covers the new virtual.
- 2026-10-24 — **M110.P3.T4 code in (`fd155a8e5`), awaiting the build after `cdc20e327`'s.** `EffectInstance::presentsOnlyPassThroughLayers(time, view)` (default false) makes `getComponentsNeededAndProduced_public` take the disabled-node branch; `StagedImageEffect` answers `route(time, view).kind == Route::Kind::Input`, so an integer-`which` Dissolve is covered too, as the Scope's "route identity" already treats it (accepted, PM). `Route::Kind::Black` keeps the old behaviour. `d40ab5123`'s test narrowing is reverted, and `TimeVaryingLayersTest.SwitchPresentsRoutedInputColorLayout` checks rgb / rgba / no colour at f1–f3. Syntax-checked only. **Watch at the build:** tests that assumed a Switch's widest layout downstream; `RenderScheduler.cpp`'s `getOutputComponents` still sizes memory estimates from the widest metadata when nothing is produced.
- 2026-10-24 — **M110.P3.T3 done.** Remote `fast --tests` of `cdc20e327` (run 40104540364) passes all 348 entries in both legs, `GpuMultiPlaneRegionTest.AddLayersPassesColourThroughBesideTheLayerItAddsInOneRegion` and `ChannelRouteGpuTest.InputLayersFollowTheRoute` included. P3.T4 builds next.
- 2026-10-24 — **M110.P3.T4 done, so Phase 110.3's gate is green.** Remote `fast --tests` of `fd155a8e5` (run 40106776318) passes all 348 entries in both legs, with the colour-varying `NativeLayersGpuTest` cases restored from before `d40ab5123` and `TimeVaryingLayersTest.SwitchPresentsRoutedInputColorLayout`. No existing test relied on the Switch's widest layout. The viewer check of an RGB frame from an RGBA-metadata Switch rides the P4.T3 GPU-host run. Left: P4.T3 (RADV run and resample bench, after M107.P6.T3 and M156.P1.T2 free the host), then the gate.
- 2026-10-24 — **Rebased onto main `121a8289c` as `988231fc3`.** Conflicts in `GpuPlacement.{h,cpp}` and `NativeGpuCapability_Test.cpp` keep both sides: main's RAW capability IDs and `taskReadsRaw`, and its early return for full-scale-only effects at a downscale, which replaces M110's `rendersFullScale` fallback. Dispatched together: remote `fast --tests` (the gate's CI line), a remote release AppImage (P4.T3's profile and bench), and a local `NATRON_DEV_GPU=1` `fast --tests` build at 15 jobs for P4.T3's RADV run, instead of waiting on a third remote build.
- 2026-10-24 — **P4.T3's RADV half passes** (boot `1a6741d9`, RX 7900 XTX, natron-dev-gpu, `NATRON_TEST_REQUIRE_GPU=1`, local `fast --tests` of `988231fc3`). Full ctest: 415/416, 45 `gpu`-labelled tests run and not skipped. Every listed suite passes: `GpuLineFilter`, `StagedOffsetGpu`, `ChannelRouteGpu`, `GpuMultiPlaneRegion`, `NativeGpuChain`, every `Native<Node>Gpu` of the ported nodes, `NativeBlurGpu`, `NativeTransformGpu`, `GpuResidency`, `GpuPlacement` and `GpuRegionFallback`. The one failure, `NativeRawColorCalibrationGpuTest.EveryAdaptationVersionClipAndGamutMatchesTheCpu`, is main's (M80 - Native RAW Support): float 26 of 256 differs by ~1.5e-6 against a zero-mismatch expectation that only lavapipe meets. It is outside M110's diff and went to the inbox. Build and test wall times: a cold-ish local build in 6 min (39% ccache hits, 15 jobs) and the full ctest in 6 min, against ~35 min for a remote `fast --tests`. P4.T3's profile and resample bench wait for the remote release AppImages (HEAD and `216e3028f^`).
- 2026-10-24 — **P4.T3 acceptance-chain profile** (release AppImage of `988231fc3`, boot `1a6741d9`, ~192 GB available; `.local/m110/p4t3/partA/`). The chain is a UHD CheckerBoard EXR, then Reformat (downscale to 2881×1621, cubic), Grade, Shuffle (R↔B, alpha 1) and NativeWrite. `NATRON_GPU=auto` places **Reformat1+Grade1+Shuffle1 in one region** in all 3 fresh-process runs, with Read and Write on the CPU and no fallbacks. Medians: region wall 19.2 ms, kernels 1.3 ms, exposed transfer 16.5 ms (upload 5.8, download 10.7). The first-pass render takes 130 ms, against 182 ms with `NATRON_GPU=off`. **Switch check (headless proxy for the viewer check):** a Switch on an RGB source beside an RGBA one renders identically on GPU and CPU, and its A=0 shows the RGB input is active. The Write writes RGBA either way, so the channel count says nothing. The viewer itself wasn't checked. The resample before/after bench (`.local/m110/p4t3/resample_bench.sh`) waits for the `216e3028f^` release AppImage.
- 2026-10-24 — **Gate CI line passes on the rebased `988231fc3`** (remote `fast --tests`, run 40140497833, lavapipe): 416/416 in both scheduler legs, every new GPU suite included.
- 2026-10-24 — **P4.T3 resample before/after** (`.local/m110/p4t3/resample-1a6741d9/table.txt`; boot `1a6741d9`, release AppImages of `988231fc3` against `216e3028f^`, `NATRON_GPU=force`, 5 interleaved rounds, medians). Box regressed materially: kernel ms UHD 0.55→2.44 (Reformat) and 1.28→4.61 (Transform), 8K 1.54→8.98 and 7.13→18.44, 24k 12.4→55.9 and 50.2→113.1. Impulse is mixed, from equal to ~1.5×. Cubic is unchanged. Wall time is unchanged in every case (kernels are 1–3% of it). Per P4.T3's brief, **M110.P4.T5 is inserted** to add the boundary-only fast path. P4.T3 is otherwise complete.
- 2026-10-24 — **M110.P4.T5 code in (`86f4492b2`), awaiting the remote `fast --tests` and release builds.** `samplePixelFast` decides the impulse cell, the box inside/edge tests, the zero-Jacobian or degenerate cell, and corner collapse in two floats (Dekker products, since Vulkan has no guaranteed FMA). It falls back to `samplePixelExact` within E = 2^-42·M of a decision, and also when |x|,|y| ≥ 2^22. Matrix rows that the host proves exact in floats (`rowExactInFloats`, flag bits 8 and 16, and word 15 of each motion-blur matrix) get E = 0, so exact-scale ties don't fall back. The new `near-ties` geometry in `NativeTransformGpu_Test` (scale 1+2^-50, translate 0.5) runs through every filter. **Watch:** a box corner crossing a cell is treated as continuous, so its floor can differ from the double path's by less than E in weight. The tests decide whether that holds parity. SPIR-V grew ~13%. The build runs remotely so it stays off the host during M156's and M107's timed benches; the RADV run and the box/impulse re-bench follow M107's sweep.
- 2026-10-24 — **Remote `fast --tests` of `86f4492b2` (run 40145933243): 415/416 in both legs.** The one failure, `ResamplingMatchesTheCpuRender/7`, is Mitchell with clamp on under the new `near-ties` geometry: up to 0.055 off, a pre-existing CPU/GPU difference, not a fast-path bug. Mitchell does not pass through its centre samples, so the clamp range jumps at the `floor(fy - 0.5)` tap-cell boundary, which two floats can misplace by one ulp near a tie. **Fix `b9bcc4670`:** clamped Mitchell also takes the exact kernels, with the same boundary-only fallback on its tap floors; unclamped Mitchell and the other smooth filters are unchanged. The exact SPIR-V grows from ~687 KB to ~974 KB. Remote `fast --tests` and release re-dispatched. The re-bench adds clamped Mitchell beside box and impulse.
- 2026-10-24 — **Remote `fast --tests` of `b9bcc4670` (run 40149028930): 416/416 in both legs**, `near-ties` on every filter included. Still to come for P4.T5: the RADV run and the box/impulse/clamped-Mitchell re-bench on the release AppImage, after M107's sweep frees the GPU.
- 2026-10-24 — **P4.T5 re-bench** (release AppImages of `b9bcc4670` against `216e3028f^`, boot `1a6741d9`, 5 interleaved rounds; `.local/m110/p4t3/resample-t5-1a6741d9/table.txt`). Transform box and impulse, and Reformat impulse, are at parity with the pre-exact kernel at UHD/8K/24k: for example Transform 24k box 51.6 vs 52.5 ms, against 113 vs 50 before the fast path. **Reformat box stays 1.30–1.37× slower** (UHD 0.70 vs 0.54, 8K 2.09 vs 1.53, 24k 16.7 vs 12.5 ms), against the 15% target. The user accepted it as a known residual (≤ 4 ms on a 2.5 s render, wall time unchanged). RADV ctest of `b9bcc4670` (local `NATRON_DEV_GPU=1` build): 415/416; the only failure is main's `NativeRawColorCalibrationGpuTest` (inbox). A clamped-Mitchell before/after bench is running.
