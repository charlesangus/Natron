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

- [ ] M110.P1.T1 — Run Reformat on the GPU through the matrix-form resampler
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

- [ ] M110.P2.T1 — Record EdgeDetect's pass list on the GPU
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

- [ ] M110.P2.T2 — Run Position on the offset-form GPU executor
  - files: `Engine/Nodes/Transform/Position.h`, `Engine/Nodes/Transform/docs/Position.md`, `Tests/Native/NativePositionGpu_Test.cpp` (new), `Tests/Native/NativeGpuCapability_Test.cpp`
  - approach:
    - Add `kResamplesOnGpu = true` and the P1.T4 concept's `static_assert`.
    - Test integer and fractional translations (rounded as `offset()` does) at mipmap 0 and 1, a non-square pixel aspect ratio, a channel subset (PassThroughUnprocessed reads the unshifted source), and a translation that moves the source out of the window. All are `exact()`.
    - Add `PLUGINID_NATRON_POSITION` to the allowlist. Record the GPU bound (`exact()`) in `Position.md`.
  - verify: at the phase build, `NativePositionGpu_Test`, `NativePosition_Test` and `NativeGpuCapability_Test` pass on lavapipe.
  - size: M

- [ ] M110.P2.T3 — Replace the row matte with a separable matte both executors read
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

- [ ] M110.P2.T4 — Add a channel-route description, base class and device kernel for the channel nodes
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

- [ ] M110.P2.T5 — Let multi-plane tasks join regions as one member per plane
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

- [ ] M110.P3.T1 — Run Crop, softness included, on the GPU
  - files: `Engine/Nodes/Transform/Crop.h`, `Engine/Nodes/Transform/docs/Crop.md`, `Tests/Native/NativeCropGpu_Test.cpp` (new), `Tests/Native/NativeGpuCapability_Test.cpp`
  - approach:
    - Add `kResamplesOnGpu = true` and the offset concept's `static_assert`.
    - Test every extent type, `reformat` on and off, `intersect`, `blackOutside`, softness 0 (`exact()`) and above 0 (`resampling()`), a NaN source pixel on the border, a crop rectangle larger than the source, mipmap 0 and 1, a non-square pixel aspect ratio, and tiles.
    - Check that the GPU source region equals Crop's own CPU region of interest.
    - Add `PLUGINID_NATRON_CROP` to the allowlist. Add the GPU bound to `Crop.md`'s row.
  - verify: at the phase build, `NativeCropGpu_Test`, `NativeCrop_Test` and `NativeGpuCapability_Test` pass on lavapipe.
  - size: M

- [ ] M110.P3.T2 — Move Shuffle and ShuffleCopy onto the channel route
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

- [ ] M110.P3.T3 — Move AddLayers and RemoveLayers onto the channel route
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

**Phase gate:** a single `tools/ci/remote/build.sh fast --tests` passes, including the three tasks' tests on lavapipe.

## Phase 110.4: Acceptance chain, docs and the GPU host

- [ ] M110.P4.T1 — Test the Read → Reformat → Grade → Shuffle → Write chain as one region
  - files: `Tests/Native/NativeGpuChain_Test.cpp` (new)
  - approach:
    - Build a NativeRead of `Tests/fixtures/flat-rgba-pattern.exr` (or a multi-layer fixture), then a Reformat with a non-identity resize and filter, a Grade with non-neutral gain, a Shuffle with a non-identity mapping (R↔B, alpha from 1, so it is not a route identity), and a NativeWrite.
    - Run `renderEndToEnd` with the Shuffle as root, as the Write pulls it.
    - Assert exactly one region, holding the Reformat, Grade and Shuffle tasks and neither Read nor Write (`ranInRegion`, `regionNodes`). Assert the GPU image matches the CPU one within `resampling()`, since Reformat's bound dominates.
    - Add a second case where the Shuffle reads in2 from another layer of the Read (multi-plane input).
    - Follow `NativeTransformGpu_Test.cpp:935`.
  - verify: at the phase build, `NativeGpuChain_Test` passes on lavapipe.
  - size: M

- [ ] M110.P4.T2 — Update the native-node README for the new GPU paths
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

- [ ] M110.P4.T3 — Verify on RADV in the natron-dev container and record the results
  - files: none (results go to this file's `## Decisions`)
  - approach:
    - On the GPU host, `NATRON_DEV_GPU=1`, in the natron-dev container, build `fast --tests`. Run with `NATRON_TEST_REQUIRE_GPU=1`:
      - every new test of this milestone (`GpuLineFilter_Test`, `StagedOffsetGpu_Test`, `ChannelRouteGpu_Test`, `GpuMultiPlaneRegion_Test`, `NativeGpuChain_Test`);
      - every `Native<Node>Gpu_Test` of the ported nodes;
      - `NativeBlurGpu_Test`, `NativeTransformGpu_Test`, `GpuResidency_Test`, `GpuPlacement_Test` and `GpuRegionFallback_Test`.
    - Then build the stripped `release` AppImage. Render a `.ntp` of the acceptance chain at UHD with `env -u LD_LIBRARY_PATH NATRON_GPU=auto NATRON_RENDER_PROFILE=<file>`.
    - Summarise the profile with `tools/bench/gpu_region_summary.py` (read-only). Record whether the cost model placed Reformat, Grade and Shuffle in one region, and the region's wall and kernel ms.
    - Compare only runs from the same boot.
  - verify: every listed test passes on RADV, and a Decisions line records the Auto-mode outcome and the numbers.
  - size: M

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
