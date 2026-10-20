# M137 - Native Base Class Flexibility

Replace `NativeImageEffect`, the single base class that special-cases native image nodes through `NativeImageTraits` flags, with a composed framework. It has three parts:
- A declarative spec says what a node *is*.
- An `Op` says what it *computes*.
- One `StagePlan` with a CPU and a GPU executor says how the host *runs* it. Nodes cannot override that part.

Under it, a kind-agnostic layer in `NativeEffectBase` gives every native node, deep and metadata nodes included, one way to declare itself: `NodeSpec`, a route/identity hook, and knob baking. The motivating defect is that Blur, EdgeDetect and ErodeDilate expose "(Un)premult by" only because a trait is on, and `render()`, `canRenderOnGpu()` and `isIdentity()` all branch on it. The design came out of a Fable design pass and a grill session on 2026-10-19 (see `## Decisions`).

The milestone runs in this order:
1. Capture pre-migration goldens and timings.
2. Build a spike: the framework plus Grade, Blur and a new Switch.
3. Write the design note and stop for the user's approval.
4. Migrate every native node, then delete the old base.

Scouted facts (2026-10-19, `main` at `d521f111a`; re-check line numbers at claim):
- **Node description:**
  - `NativePluginDescription` is at `Engine/Nodes/NativeEffectBase.h:79` and `NativeInputDescription` at `:55`. They are returned by value from the pure virtual `getNativePluginDescription()` (`:492`) and rebuilt on every query (`NativeEffectBase.h:231-277`, `.cpp:196-262`). Dissolve rebuilds 65 inputs on each call.
  - There are 43 overrides across Color, Generator, Transform, Keyer, Filter, Merge, Channel, Metadata, IO, Deep (11), `TypedPassthrough` and `MissingPluginNode`.
  - Registration is in `AppManager::loadBuiltinNodePlugins()`, at `Engine/AppManager.cpp:1620-1697`, through `registerBuiltInPlugin<T>(icon, deprecated, internal)`. Preset factories (`ColorMathAdd`/`Multiply`/`Gamma` at `ColorMathNode.h:107-115`, and `MergePreset<P>` at `Merge.h:191`) are structs with a static `BuildEffect`.
- **Engine casts to `NativeImageEffect`:**
  - `GpuCostModel.cpp:247`, and `:250`, which also casts to `Blur`
  - `RenderScheduler.cpp:351` (`isPointOp`)
  - `GpuPlacement.cpp:488,564`
  - `GpuRegionTask.cpp:583,587,623,630,1095,1177,1455`
  - Tests: `GpuResidency_Test.cpp:384,470`
  - `GpuPlacement.cpp:518-519` re-reads `mix` by name, and `:539-547` re-reads the unpremult selector.
- **Includes:** `NativeImageEffect.h` is included by 46 files (26 Engine, 20 Tests), `NativeGenerator.h` by 10, and `SpatialFilterSupport.h` by 5 (Blur, EdgeDetect, ErodeDilate and itself).
- **Kernels:**
  - Every kernel is listed in `Engine/Gpu/CMakeLists.txt:23-44`. The `GpuKernelId` enum is generated (`GpuKernelRegistry.h:20-25`).
  - Already `IPixelOp` entries over `pixelwrap.slang` (`:57`, `runWrapped` `:185`): invert, constant, checkerboard, clamp, colorlookup, colorcorrect, saturation, add, multiply, gamma, and `grade_wrapped` (`pixelwrap.slang:333`).
  - Standalone: `grade_main` (`grade.slang:32`), `keyer_main` (`keyer.slang:167`) and `chromakeyer_main` (`chromakeyer.slang:360`), plus the spatial kernels `blur`, `blur_lines`, `morph_*`, `resample*` and `wrap_divide`/`wrap_finish`.
- **Overrides today:**
  - Transform: `render` 1077, `isIdentity` 1032, GPU 1339-1371.
  - Crop: `render` 570.
  - Reformat: `render` 1121, `isIdentityOp` 646.
  - Position: `render` 349, `isIdentity` 296.
  - Dissolve: `render` 471, `isIdentity` 371, its own `maskInvert` at 262-269.
  - Keyer: `render` 1116, `isIdentity` 1070, GPU 1287/1316.
  - ChromaKeyer: `render` 910, `isIdentity` 870, GPU 1124-1198.
  - Merge: `render` 931, `isIdentity` 861.
- **Knob footer:**
  - `createUnPremultSelector` is at `Node.cpp:2875`. It is gated by `wantsHostUnPremultSelector()` at `:2890`.
  - `createMaskSelectors` is at `:2967`.
  - In `initializeDefaultKnobs` (`:3240`), mask invert and mix are re-added at the end at `:3302-3351`, and `advancedSep` is added at `:3353-3376`.
  - `addMaskMixKnobs` is at `NativeImageEffect.cpp:851`. It has 12 node callers and 3 test callers.
- **Tests:**
  - There is no golden-render harness for nodes. `NativeParity` (`Tests/NativeParity.h`) replays recorded `.f32` references in its "NPAR" format (`:236-310`, recorded through `NATRON_PARITY_RECORD_DIR`, `NativeParity.cpp:234`). Its tolerances are at `:143-151`.
  - The GPU parity harness is `Tests/Native/NativeGpuParity.{h,cpp}`.
  - `Tests/Native/*_Test.cpp` is globbed (`Tests/CMakeLists.txt:132`).
  - There are no Shuffle, AddLayers or RemoveLayers tests.
- **Bench:**
  - `tools/bench/node_profile.{py,sh}` runs single-node and N-chain UHD renders in natron-dev against `build/release` (cpu/serial) or `build/debug` (gpu). Its `NODES` table (`node_profile.py:48-76`) has no blur, dissolve, crop, reformat or position.
  - `graph_bench.py` has `blurchain`, `xfchain`, `ccchain` and `mergechain`.
- **Hashing (verified):** `Node::computeContentHash` (`Node.cpp:~942-1010`) hashes the plug-in ID and version, the binary stamp, knob names and values, inputs, and `appendNodeContentHash` (overridden only by AddLayers and DeepAddLayers). No class trait or `NativeImageTraits` is hashed.

## Scope

**In:**
- A spike first: `NodeSpec`, `StagePlan` and both executors, with Grade (Point), Blur (Passes) and a new Switch (Route) on CPU and GPU. Then a design note, written from what the spike found, as a decision file. Then a stop for the user's approval of both. The spike's code lands on the milestone branch and is kept if the design is approved, or reverted if it is not.
- A kind-agnostic shared layer in `NativeEffectBase`, which deep, scene and geometry nodes adopt later instead of forking:
  - `NodeSpec` replaces `NativePluginDescription`. It holds identity, grouping and RFV reads, plus inputs with kind, role (Source, Aux, Mask, Variadic), optional, time, boundary and layer policy.
  - One route/identity hook, returning `{None | Input(i, t) | Black}`. The scheduler and GPU placement read it.
  - A common `KnobReader` and `bake()` into a trivially-copyable `Params`. Knobs are read once per evaluation.
- The image framework:
  - An `ImagePipelineSpec` with stages (Premult, MaskMix, PassThroughUnprocessed), a layer policy (Color with defaults, All, TargetSelect, None) and an op kind (Point, Composite, Route, Passes, Resample, Custom).
  - The `Op` concept, one `StagePlan`, and the two executors `runStagesCpu` and `recordStagesGpu`.
  - Identity is composed and never overridden.
  - GPU feasibility is a property of the plan, which placement reads instead of re-reading knobs by name (`GpuPlacement.cpp:518,539`).
- Every native node moves onto `NodeSpec`, including Shuffle, AddLayers, RemoveLayers, the metadata nodes, NativeRead/Write, DeepToImage and MissingPluginNode.
- Every image node moves onto the image framework: the 7 colour point-ops, the generators, Blur, EdgeDetect, ErodeDilate, the Transform family (as Resample), Crop, Reformat, Position, Dissolve, Keyer and ChromaKeyer.
- Switch, a new native Route node, built here as the Route proof. Its brief moved from M121 - Native Routing And Mix (M121.P1.T1, cancelled):
  - 64 optional image inputs named "0".."63", modelled on Dissolve. `which` is an animatable int, and its display range follows the highest connected input (`Dissolve.cpp:277-280`). There is no `automatic` knob (user, 2026-10-13).
  - The route hook returns the clamped `which` input at the same time and view. RoD is the chosen input's. It never renders.
  - It forwards `getTransform` from the chosen input if the framework supports concatenation; otherwise concatenation is left out.
  - It registers through today's manifest; M120 - Native Node Port Kit moves it onto a fragment with every other node. Moving the PyPlugs to it and retiring `switchPlugin` stay with M121.
- Blur, EdgeDetect and ErodeDilate lose "(Un)premult by" and filter premultiplied data as stored.
- The framework creates the premult, mask and mix stage knobs and places them together below the line: [separator] → (Un)premult by → mask channel and Invert mask → Mix. That is M95 - Knob Cleanup And Keyer Redesign's agreed footer order without the clamp row, which M95 adds above "(Un)premult by" and extends to OFX nodes.
- Every point and composite GPU body becomes an `IPixelOp` entry, one-source or n-source (P8.T6), in the `pixelwrap.slang` style. There are no standalone kernels such as grade.slang's `grade_main`.
- Shared C++/Slang maths headers become the parity convention, and the Op names its header.
- `NativeImageEffect`, `NativeImageTraits` and `SpatialFilterSupport` are deleted. Merge is re-based as a Custom node that renders through `renderOp`/`isOpIdentity`.
- The gate that proves done has three parts:
  - Every migrated node, wherever behaviour is not meant to change, is bit-exact on the CPU against golden renders captured before the migration. GPU output stays within the README parity tolerances.
  - `tools/bench` in-graph timings for Grade, Blur, Transform and Dissolve on the release build show no regression beyond noise, measured on the same host before and after.
  - Xvfb screenshots of the stage-knob panels are shared with the user before sign-off.

**Out:**
- The clean-sheet Merge rewrite. Merge goes Custom here. Owner: M101 - Clean-Sheet Merge Node.
- Switch's siblings and the rest of routing/mix. Owner: M121 - Native Routing And Mix.
- New GPU ports for EdgeDetect, Reformat, Crop, Position, Dissolve and the Shuffle family, plus multi-plane placement. Owner: M110 - Remaining Native GPU Ports.
- Per-node registration fragments, built on `NodeSpec`. Owner: M120 - Native Node Port Kit.
- Optimising the Passes executor for Blur's in-graph overhead. Owner: M139 - Blur In-Graph Overhead.
- Plain IDs and sibling merges; IDs carry over unchanged here. Owner: M111 - Native Node Identity Cleanup.
- The Keyer trapezoid redesign, clamp black/white placement, and the hidpi/interactive knob cleanup. Owner: M95 - Knob Cleanup And Keyer Redesign.
- Fusing point-op regions. Owner: M87 - Point-Op Kernel Fusion.
- Deep per-sample plan types. Owner: M21 - Deep Tier-2 And Bridges.
- Geometry plan types. Owner: M54 - SOPs-Style Geometry.
- Scene authoring. Owner: M19 - USD/Hydra Foundation.
- Multi-plane outputs, such as Despill's spill map. No owner.
- A declarative knob table that generates `initializeKnobs()`. No owner.
- A single-source CPU/GPU kernel JIT. No owner.

**Edge cases:**
- Switch with `which` beyond the last connected input clamps to the highest connected input. With no inputs connected it outputs black and an empty RoD.
- Switch with an animated `which` re-routes per frame with no cache poisoning, because the routed input is part of the frame's hash.
- A filter at default settings stays bit-exact when "(Un)premult by" goes away, because the selector defaults to None (`Node.cpp:2909`). Only renders that set it change, and their GPU parity cases are removed with the knob.
- Merge is a Custom node, rendering through `renderOp`/`isOpIdentity` with the composed stage identities, until M101 - Clean-Sheet Merge Node, so no second framework survives this milestone.
- Constant, Solid and CheckerBoard lose their optional input (user, 2026-10-19). They no longer pass channels through, and unwritten channels of the target layer are zero.
- Crop rounds its box to whole pixels at the render scale, and softness is a matte (user, 2026-10-19). Its goldens may differ only at the edge pixels of fractional-box cases.
- The scene-to-image bridges (HydraRender, ProjectTexture) remain Custom image nodes with a scene input.
- An invalid spec, such as a Premult stage with no source input, a Route op with stages, or a mask role on a non-image input, is a compile error (constexpr spec plus `static_assert`/concepts). There is no runtime path for it, except MissingPluginNode's runtime spec, which `isValidNodeSpec()` checks at construction (P4.T5).
- GPU refusal falls back to the CPU, and a runtime shader that does not compile is reported once and never rebuilt, as `NativeImageEffect` does today.
- Knob script names (`mix`, `maskInvert`, per-node knobs) are unchanged, so content hashes and the reordering at `Node.cpp:3303` keep working. The hasher reads no class traits (verified, see Scouted facts), so replacing `NativeImageTraits` with a spec leaves hashes unchanged.
- The design is rejected at the approval stop: P3.T2 reverts Phase 137.2's commits on the milestone branch, the row goes back to `todo` with this file returned to a stub, and `/cat-plan` re-elaborates it from the rejection's reasons.
- During migration both bases coexist. The engine reaches GPU virtuals through `NativeEffectBase` (P2.T3), so a node on either base works until P9 deletes the old one.

**Assumptions:**
- Unconfirmed: Switch keeps 64 fixed optional inputs like Dissolve, rather than growing inputs dynamically.
- `canRenderOnGpu` and the plan stay deterministic between placement and record time (`GpuRegionTask.cpp:587`).
- Unconfirmed: GPU-side changes compile and run only on the remote build until a local slangc tree exists. The spike's GPU half is verified there.
- Unconfirmed: the Despill spill-map mode in M122 - Native Colour Utilities is refused for the GPU, or stays Custom, until multi-plane outputs exist.

## Phase 137.1: Baselines before any change

- [x] M137.P1.T1 — Golden-render harness and pre-migration CPU goldens
  - files: `Tests/Native/NativeGolden.{h,cpp}` (new), `Tests/Native/NativeGolden_Test.cpp` (new), `Tests/fixtures/native-golden/` (new), `Tests/CMakeLists.txt`
  - approach:
    - Add a harness that renders a native node on fixed inputs and compares the result bit-exactly with a stored reference. Reuse `NativeParity`'s "NPAR" `.f32` writer and reader (`NativeParity.h:236-310`) rather than a new format.
    - Recording runs when `NATRON_GOLDEN_RECORD_DIR` is set; otherwise the harness replays.
    - Define cases for every image node M137 migrates: the 7 colour point-ops, Constant, Solid, CheckerBoard, Blur, EdgeDetect, ErodeDilate, Dilate, Transform, TransformMasked, Crop, Reformat, Position, Dissolve, Keyer and ChromaKeyer.
    - Each case has default knobs plus 2–3 non-default settings, with mask and mix on, an RGBA and a non-colour layer, and render scale 1 and 0.5. Include "(Un)premult by" = A for the colour nodes only.
    - Record the goldens from this phase's build, before any framework code exists.
  - verify: at the phase build, the recorded fixtures are committed, and `NativeGolden_Test` replays them green on the unchanged nodes.
  - size: L

- [x] M137.P1.T2 — Bench coverage and before-timings
  - files: `tools/bench/node_profile.py`, `tools/bench/BASELINE.md`
  - approach:
    - Add `blur`, `dissolve`, `crop`, `reformat` and `position` entries to the `NODES` table (`node_profile.py:48-76`), following the existing entries.
    - After the phase build, run `node_profile.sh cpu 8` for grade, blur, transform and dissolve, plus `graph_bench.py` `blurchain`, `xfchain` and `ccchain`, on the release build of this host.
    - Record the numbers, the host and the SHA in `BASELINE.md` under an M137 heading.
  - verify: `BASELINE.md` has the M137 before-table, with three rounds per case and their spread.
  - size: M

## Phase 137.2: Spike — framework, Grade, Blur, Switch

- [x] M137.P2.T1 — `NodeSpec`, the route hook and `KnobReader`/`bake` in `NativeEffectBase`
  - files: `Engine/Nodes/NodeSpec.h` (new), `Engine/Nodes/KnobReader.{h,cpp}` (new), `Engine/Nodes/NativeEffectBase.{h,cpp}`, `Tests/Native/NodeSpec_Test.cpp` (new)
  - approach:
    - `NodeSpec` is a constexpr struct holding identity (ID, label, description, grouping, versions, isWriter), output kind, RFV reads, and a span of `InputSpec`. Each `InputSpec` has label, kind, role (Source, Aux, Mask, Variadic(n)), optional, a time policy, a boundary (Zero, Nearest) and a layer policy (Plane, Select(knob), PairWith(i)).
    - `NativeEffectBase` answers every description query (`NativeEffectBase.h:231-277`, `.cpp:196-262`) from a node's static spec when it has one, and falls back to `getNativePluginDescription()` until P9 removes it.
    - Add the route hook `virtual Route route(double time, ViewIdx view)`, returning `{None | Input(i, t) | Black}`. Implement `isIdentity` on top of it for spec'd nodes.
    - `KnobReader` reads knobs at a time and view, with RFV context from TLS. `bake()` returns a trivially-copyable `Params`.
    - Validate the spec with `static_assert` and concepts: a mask role on a non-image input, or a Variadic input that isn't last, fails to compile.
  - verify: at the phase build, `NodeSpec_Test` checks description equivalence for a spec'd test effect against the same `NativePluginDescription`, all three route answers, and that `bake` reads each knob once. A `static_assert` negative case sits in a commented compile-fail block, documented in the test.
  - size: L

- [x] M137.P2.T2 — Image pipeline spec, `Op` concept, `StagePlan` and the CPU executor
  - files: `Engine/Nodes/Image/ImagePipelineSpec.h` (new), `Engine/Nodes/Image/Op.h` (new), `Engine/Nodes/Image/StagePlan.{h,cpp}` (new), `Engine/Nodes/Image/StagedImageEffect.{h,cpp}` (new)
  - approach:
    - `ImagePipelineSpec` holds a `StageSet` (Premult, MaskMix, PassThroughUnprocessed), a layer policy (Color{defaults}, All, TargetSelect, None) and an `OpKind`.
    - `Op` concepts per kind: Point first. Point has `bake`, `isIdentity(Params)`, a pure `row(Params, RowIO)` (`PixelKernel.h:92-103`), and a `gpu(Params)` returning a compiled kernel ID or a runtime shader.
    - `StagePlan` resolves the divisor, mask, invert, mix, channels and sources once per render. `runStagesCpu` is today's `NativeImageEffect::render` loop (`.cpp:965-1178`), moved rather than rewritten, so output stays bit-exact.
    - `StagedImageEffect` on `NativeEffectBase` composes identity as mix 0, or `Op::isIdentity`, or a non-inverted mask missing the window. It answers `wantsHostUnPremultSelector`, `getLayerKnobSpec`, `defaultProcessesAllLayers` and `isHostChannelSelectorSupported` from the spec.
    - `ImageNode<Op>` is the thin class template over it.
  - verify: covered by P2.T5's Grade goldens; at the phase build, it compiles and the existing native tests stay green.
  - size: L

- [x] M137.P2.T3 — GPU virtuals on `NativeEffectBase`, and placement reads the plan
  - files: `Engine/Nodes/NativeEffectBase.h`, `Engine/GpuPlacement.cpp`, `Engine/GpuRegionTask.cpp`, `Engine/GpuCostModel.cpp`, `Engine/RenderScheduler.cpp`
  - approach:
    - Move `canRenderOnGpu`, `getGpuInputRoI`, `getGpuInputLayer`, `renderGpu` and an `opKind()` query (replacing `isPointOp`) up to `NativeEffectBase`, with refusing defaults. `NativeImageEffect` and `StagedImageEffect` both override them.
    - Retarget the casts at `GpuPlacement.cpp:488,564`, `GpuRegionTask.cpp:583-630,1095,1177,1455`, `GpuCostModel.cpp:247` and `RenderScheduler.cpp:351` to `NativeEffectBase`.
    - Replace `GpuPlacement.cpp:514-547`'s by-name reads of mix and the unpremult selector with a virtual `getGpuStageSummary(time, view)`. `StagedImageEffect` fills it from its `StagePlan`; `NativeImageEffect` fills it from its knobs.
    - Leave the Blur cast at `GpuCostModel.cpp:250` until P2.T6.
  - verify: at the phase build, `GpuResidency_Test`, `GpuPlacement_Test` and every `Native*Gpu_Test` pass unchanged.
  - size: L

- [x] M137.P2.T4 — GPU executor for Point ops
  - files: `Engine/Nodes/Image/StagePlan.cpp`, `Engine/Nodes/Image/StagedImageEffect.cpp`, `Engine/Gpu/GpuWrap.{h,cpp}`
  - approach:
    - `recordStagesGpu` records a Point op as one wrapped dispatch, through the existing `GpuWrap` and `pixelwrap.slang` `runWrapped<Op>` path. Move it from `NativeImageEffect::makeGpuWrap`/`renderGpu` (`.cpp:697-806`), and fold the runtime-shader path (`.cpp:390-575`) in as a second `GpuOpRef` form.
    - Mask, mix, invert, divisor and skip channel are read from the `StagePlan` only.
    - GPU feasibility is a plan property: float, 1–4 channels, and the divisor inside the plane.
  - verify: covered by P2.T5's GPU parity test; at the phase build, `NativeRuntimeShaderGpu_Test` passes against a staged test effect.
  - size: L

- [x] M137.P2.T5 — Grade on the framework, and `grade_main` retired
  - files: `Engine/Nodes/Color/Grade.{h,cpp}`, `Engine/Gpu/Kernels/grade.slang`, `Engine/Gpu/Kernels/pixelwrap.slang`, `Engine/Gpu/CMakeLists.txt`, `Tests/Native/NativeGradeGpu_Test.cpp`
  - approach:
    - Grade becomes `ImageNode<GradeOp>` with `Stages{Premult, MaskMix}`, `Layers::Color{1,1,1,0}` and `OpKind::Point`. `GradeOp::Params` is today's `GradeGpuParams` (`Grade.cpp:394-404`).
    - Its GPU body is `grade_wrapped` (`pixelwrap.slang:333`). Delete `grade_main` and its `slang_add_kernel` line.
    - The Normalize button and `knobChanged` stay in the node.
  - verify: at the phase build, Grade's `NativeGolden_Test` cases are bit-exact, `NativeGradeGpu_Test` stays within the README Grade bound, and `NativeGrade_Test` passes.
  - size: M

- [x] M137.P2.T6 — Passes kind: pass list over scratch, on CPU and GPU
  - files: `Engine/Nodes/Image/Op.h`, `Engine/Nodes/Image/PassExecutor.{h,cpp}` (new), `Engine/Nodes/Image/StagePlan.cpp`, `Engine/GpuCostModel.cpp`
  - approach:
    - A Passes op returns a pass list (line X, line Y, gather with apron, point, resample, downsample) over scratch buffers.
    - Halo, RoI and RoD growth are the sum over passes.
    - The CPU executor ports `SpatialFilterSupport`'s `fillBuffer`, `ProcessedPlanes`, `forEachLineChunk` and `writeWindow` with the same arithmetic order. Merge `forEachLineChunk` and `makeRowBands` into one band helper.
    - The GPU executor records divide (only when the Premult stage is on), the passes into `ctx.scratch`, then finish. Move it from `recordGpuDivide`/`recordGpuFinish`.
    - Replace the Blur cast at `GpuCostModel.cpp:250` with an op-provided cost hint.
  - verify: covered by P2.T7; at the phase build, it compiles and the existing filter tests stay green.
  - size: L

- [x] M137.P2.T7 — Blur on the Passes kind, without "(Un)premult by"
  - files: `Engine/Nodes/Filter/Blur.{h,cpp}`, `Engine/Nodes/Filter/BlurKernels.{h,cpp}`, `Tests/Native/NativeBlurGpu_Test.cpp`
  - approach:
    - Blur becomes `ImageNode<BlurOp>` with `Stages{MaskMix}`, `Layers::All` and `OpKind::Passes`. The FIR/IIR lines in `BlurKernels` become its passes.
    - Delete `Blur::render` (`:480`) and its own selector read (`:533-566`).
    - Remove the GPU parity cases that set "(Un)premult by".
  - verify: at the phase build, Blur's golden cases at the default (None) selector are bit-exact, the cases that set it are deleted from the goldens, `NativeBlurGpu_Test` stays within its bound, and `NativeBlur_Test` passes.
  - size: L

- [x] M137.P2.T8 — Native Switch as the Route proof
  - files: `Engine/Nodes/Merge/Switch.{h,cpp}` (new), `Engine/AppManager.cpp`, `Tests/Native/NativeSwitch_Test.cpp` (new), `Gui/Resources/Images/NativeNodes/Switch.png` (new, or reuse the OFX icon)
  - approach:
    - Follow the Switch brief under `## Scope`: a spec'd node with `Inputs{Variadic(64)}`, `OpKind::Route` and no stages.
    - `route()` returns the clamped `which` input. RoD is the routed input's, or empty with none connected.
    - `which` display range follows the highest connected input, as `Dissolve::updateRange` does (`Dissolve.cpp:281-299`).
    - Register next to Dissolve (`AppManager.cpp:1669`).
  - verify: at the phase build, `NativeSwitch_Test` covers `which` 0/1/beyond-range, an animated `which` changing output per frame, the no-input case, and exactly one upstream render per frame (`CountingTestEffect`).
  - size: M

- [x] M137.P2.T9 — Stage-knob footer for spec'd nodes
  - files: `Engine/Node.cpp`, `Engine/Nodes/Image/StagedImageEffect.cpp`, `Tests/Native/StageKnobFooter_Test.cpp` (new)
  - approach:
    - The framework creates `maskInvert` and `mix` when the spec has MaskMix, keeping the same script names. `createUnPremultSelector` (`Node.cpp:2875`) runs when the spec has Premult.
    - In `initializeDefaultKnobs` (`:3302-3376`), a spec'd node gets: separator → (Un)premult by → mask channel(s) → Invert mask → Mix. Leave a slot for M95's clamp row.
    - Nodes on the old base keep today's order until they migrate.
  - verify: at the phase build, `StageKnobFooter_Test` checks the main-page order for Grade (with premult) and Blur (without).
  - size: M

## Phase 137.3: Design note and approval

- [x] M137.P3.T1 — Write the design note from the spike
  - files: `PLAN/DECISIONS/<date>-native-node-framework.md` (new), `PLAN/DECISIONS/INDEX.md`
  - approach:
    - Write it as the project-wide decision file. It covers:
      - the three layers (`NodeSpec`, `ImagePipelineSpec` with `Op`, `StagePlan` with its executors) and their final signatures as merged in Phase 137.2
      - how each op kind and each node maps
      - what the spike changed from the Fable design, and why
      - the Phase 137.2 golden, parity and bench results against Phase 137.1
      - the risks left for Phases 137.4–137.9
      - the deep, scene and geometry boundary (who adopts `NodeSpec` later)
    - Name each departure from this file's Scope plainly, so the user can veto it.
  - verify: the note exists, its INDEX line is appended, and it cites the Phase 137.2 commit SHAs.
  - size: L

- [x] M137.P3.T2 — Approval stop
  - files: none (user decision); on rejection, the milestone branch
  - approach:
    - Share Xvfb screenshots of the Grade, Blur and Switch panels.
    - Set the row `blocked`, and add one `# Open questions` item asking the user to approve the design note and the spike.
    - **Approve:** record the decision, spot-check Phases 137.4–137.9 against the note per PLAN-FORMAT.md §5a, and continue on the same branch.
    - **Reject:** revert Phase 137.2's commits on the milestone branch, then hand back to `/cat-plan`, as the Scope edge case says.
  - verify: the user's answer is recorded under `## Decisions`.
  - size: S

## Phase 137.4: Every non-image native node onto `NodeSpec`

- [x] M137.P4.T1 — Metadata nodes and `TypedPassthrough`
  - files: `Engine/Nodes/Metadata/{ModifyMetadata,CopyMetadata,RemoveMetadata,ViewMetadata}.{h,cpp}`, `Engine/Nodes/TypedPassthrough.{h,cpp}`
  - approach:
    - Replace each `getNativePluginDescription()` with a `static constexpr NodeSpec kNodeSpec` over an `inline constexpr InputSpec[]`, field for field, and a `nodeSpec()` returning `staticNodeSpec<kNodeSpec>()`, as `Switch.h` does. Descriptions become plain `string_view`s (approved). Keep the class names; tests cast to them.
    - Identity rule for every P4 task: delete an `isIdentity` override whose answer reads only time, view, knobs and connections, and implement `route()` instead (`Route::input(0, time)` for these five). Keep `isIdentity` where it reads scale or RoI, which `route()` lacks (DeepCrop).
  - verify: at the phase build, `NativeCopyMetadata_Test`, `NativeModifyMetadata_Test`, `NativeRemoveMetadata_Test`, `NativeViewMetadata_Test` and `NativePluginList_Test` pass, and the `getPluginIDs()` dump is unchanged.
  - size: M

- [x] M137.P4.T2 — Channel nodes: Shuffle, ShuffleCopy, AddLayers, RemoveLayers
  - files: `Engine/Nodes/Channel/{Shuffle,AddLayers,RemoveLayers}.{h,cpp}`, `Tests/Native/NativeChannelNodesSpec_Test.cpp` (new)
  - approach: as P4.T1. These stay multi-planar `NativeEffectBase` nodes and do not use the image pipeline. Because they have no native tests today, add a test of each one's ID, inputs and data kinds.
  - verify: at the phase build, the new test, the channel-set render tests and `NativePluginList_Test` pass.
  - size: M

- [x] M137.P4.T3 — Deep I/O and bridges: DeepRead, DeepWrite, DeepMerge, DeepToImage, DeepFromImage
  - files: `Engine/Nodes/Deep/{DeepRead,DeepWrite,DeepMerge,DeepToImage,DeepFromImage}.{h,cpp}`
  - approach: as P4.T1, with input kinds from the spec. Do not touch the deep render paths.
  - verify: at the phase build, the deep tests pass.
  - size: M

- [x] M137.P4.T4 — Remaining deep nodes: DeepRecolor, DeepCrop, DeepExpression, DeepReformat, DeepRemoveLayers, DeepAddLayers
  - files: `Engine/Nodes/Deep/{DeepRecolor,DeepCrop,DeepExpression,DeepReformat,DeepRemoveLayers,DeepAddLayers}.{h,cpp}`
  - approach: as P4.T3. DeepAddLayers keeps its `appendNodeContentHash` override.
  - verify: at the phase build, the deep tests pass.
  - size: M

- [x] M137.P4.T5 — NativeRead, NativeWrite and MissingPluginNode
  - files: `Engine/Nodes/IO/NativeRead.{h,cpp}`, `Engine/Nodes/IO/NativeWrite.{h,cpp}`, `Engine/Nodes/MissingPluginNode.{h,cpp}`
  - approach:
    - NativeRead/NativeWrite as P4.T1; `isWriter` comes from the spec.
    - `NodeSpec` is an aggregate of `string_view`s and spans with no runtime constructor. MissingPluginNode builds its spec at construction from the missing plug-in's record: it owns the strings and a `std::vector<InputSpec>` as members (every string built before any view is taken), and `nodeSpec()` returns a member `NodeSpec` viewing them. It is the only runtime spec; say why in its header.
    - No `static_assert` covers it: call `isValidNodeSpec()` at construction and fail loudly (log, refuse the inputs), not with `assert` (NDEBUG builds).
  - verify: at the phase build, the `NativeRead*_Test`/`NativeWrite*_Test` suites and `Tests/Serialization/MissingPlugin*_Test` pass, and the `getPluginIDs()` dump is unchanged.
  - size: M

- [x] M137.P4.T6 — Merge golden cases, recorded before Merge is re-based
  - files: `Tests/Native/NativeGolden_Test.cpp`, `Tests/fixtures/native-golden/` (new Merge fixtures only)
  - approach:
    - P1.T1 recorded no Merge cases, yet P8.T5 moves Merge's mask/mix onto the `StagePlan`. Add Merge cases in P1.T1's style: over and plus, one preset ID, A2 connected, mask with mix 0.5 and inverted, an A input outside the window, scale 1 and 0.5.
    - Record at this phase's remote build (`--record-goldens`), while Merge is still on the old base, and commit only the new Merge fixtures. Every other fixture stays the e349888f0 recording.
  - verify: at the phase build, the Merge fixtures carry the recording STAMP, `NativeGolden_Test` replays green, and git shows no change to the other fixtures.
  - size: M

- [x] M137.P4.T7 — Shared test effects onto `NodeSpec`
  - files: `Tests/CountingTestEffect.h`, `Tests/DataKindTestEffect.h`, `Tests/DeepRenderTestEffect.h`, `Tests/{InputChangedFetch,Multiplanar,NativeMetadata,PassThroughRoD}TestEffect.h`
  - approach: P9.T1 deletes `getNativePluginDescription()`, which the test effects in these headers override. Replace each with a constexpr `NodeSpec` and `nodeSpec()`, field for field, as P4.T1. Keep their own `isIdentity` overrides, and make no assertion changes.
  - verify: at the phase build, the full ctest passes on both scheduler modes.
  - size: M

- [x] M137.P4.T8 — Remaining test-file effects onto `NodeSpec`
  - files: `Tests/NativeParity.{h,cpp}`, `Tests/Native/EngineHooks_Test.cpp`, `Tests/Native/NativeDirectOutput_Test.cpp`, `Tests/Native/RectangleOverlay_Test.cpp`, `Tests/Native/NodeSpec_Test.cpp`
  - approach:
    - As P4.T7, for `ParitySourceTestEffect` and the effects in the three test files.
    - Replace `NodeSpec_Test`'s equivalence case against a `NativePluginDescription` with direct checks of what `NativeEffectBase` answers: ID, label, grouping, versions, input count, Variadic labels, optional, kinds, mask. Keep its other cases.
    - The framework test effects stay for P9.T2.
  - verify: at the phase build, these suites pass, and a grep for `getNativePluginDescription` in `Tests/` lists only `NativeImageEffect_Test.cpp`, `NativeImageEffectGpu_Test.cpp` and `NativeRuntimeShaderGpu_Test.cpp`.
  - size: M

## Phase 137.5: Point ops and generators

- [x] M137.P5.T1 — Invert, Clamp and Saturation
  - files: `Engine/Nodes/Color/{Invert,Clamp,Saturation}.{h,cpp}`, `Tests/Native/{NativeInvertGpu,NativeClampGpu,NativeSaturationGpu}_Test.cpp`
  - approach:
    - Each becomes a `…Op` with `kNodeSpec`/`kPipeline`, under a node class of its current name deriving from `ImageNode<…Op>`, as Grade does. Pipeline: `{Premult, MaskMix, PassThroughUnprocessed}` (the old base's set with `hostUnPremult`), current layer defaults, Point.
    - `Params` hold what the CPU kernel reads today at its precision (doubles where `*Values` are doubles). `gpu()` builds the existing float block (`*GpuParams`) from them for the existing kernel (`invert`, `clamp_wrapped`, `saturation_wrapped`), as `GradeOp` does. `isIdentityOp` becomes `isIdentity(params)`. Delete `makeKernel`, `gpuKernelId`, `makeGpuKernel`, `isPointOp` and the `addMaskMixKnobs` calls.
    - Retarget the GPU tests' `NativeImageEffect*` casts to `StagedImageEffect*`; the parity harness has both overloads.
  - verify: at the phase build, the three nodes' goldens are bit-exact, their `Native*Gpu_Test` stay within their bounds, and their `Native*_Test` pass.
  - size: M

- [x] M137.P5.T2 — ColorCorrect, and curve tables in `Params`
  - files: `Engine/Nodes/Color/ColorCorrect.{h,cpp}`, `Engine/Nodes/Image/CurveSnapshot.h`, `Engine/Nodes/Image/Op.h`, `Engine/Nodes/KnobReader.h`, `Tests/Native/NativeColorCorrectGpu_Test.cpp`
  - approach:
    - As P5.T1, kernel `colorcorrect_wrapped`.
    - ColorCorrect's CPU kernel evaluates two `CurveSnapshot`s whose keys are an unbounded `std::vector`, and its GPU block is packed words; neither fits a trivially-copyable `Params`. Approved by the user: relax `bakeParams`/`PointOp` to `std::copyable` Params, which hold a `std::shared_ptr` to immutable tables built in `bake()` and cached in the op by the curve knobs' hash, so per-query bakes do not rebuild them. Add `GpuOpRef::packed(kernelId, std::vector<std::uint32_t>)` for blocks an op packs itself.
    - CPU arithmetic is unchanged; only where the tables live moves.
  - verify: at the phase build, ColorCorrect's goldens are bit-exact, `NativeColorCorrectGpu_Test` stays within its bound, and `NativeColorCorrect_Test` passes.
  - size: L

- [x] M137.P5.T5 — ColorLookup
  - files: `Engine/Nodes/Color/ColorLookup.{h,cpp}`, `Engine/Nodes/Image/Op.h`, `Engine/Nodes/Image/StagedImageEffect.cpp`, `Tests/Native/NativeColorLookupGpu_Test.cpp`
  - approach:
    - As P5.T2, with its shared-table Params: the tables hold the four 1024-entry LUTs and the `CurveSnapshot`s, because the CPU evaluates the curves directly outside [0,1] (`ColorLookup.cpp:~262`). `gpu()` returns `GpuOpRef::packed` with the words `packGpu` builds today, or none when a curve has more keys than the block holds.
    - ColorLookup refuses two-channel pixels on the GPU (`:713`), but `canRenderOnGpu` is FINAL and `gpu(params)` sees no query. Add an accepted-channel-count field to `GpuOpRef` (default 1–4), checked by `StagedImageEffect::canRenderOnGpu`, and set it to {1,3,4} here.
    - Keep the class name (`ParametricBackground_Test` casts to it) and its histogram/`knobChanged` code.
  - verify: as P5.T1, plus `ParametricBackground_Test`.
  - size: L

- [x] M137.P5.T3 — ColorMath (Add, Multiply, Gamma)
  - files: `Engine/Nodes/Color/ColorMathNode.{h,cpp}`, `Engine/AppManager.cpp`, `Tests/Native/NativeColorMathGpu_Test.cpp`
  - approach:
    - One op templated on the operation, `ColorMathOp<Kind>`, each instantiation with its own `kNodeSpec` (IDs unchanged) and the shared pipeline `{Premult, MaskMix, PassThroughUnprocessed}`, Point. `ImageNode` default-constructs its op and passes it nothing from the spec, so the kind is a template argument.
    - `gpu()` picks `add_wrapped`/`multiply_wrapped`/`gamma_wrapped`. Params as P5.T1.
    - The `ColorMathAdd`/`Multiply`/`Gamma` factory structs go. Register the three `ImageNode<ColorMathOp<…>>` (or thin subclasses) in `loadBuiltinNodePlugins()`. Retarget the GPU test's casts.
  - verify: as P5.T1 for all three IDs, and `NativePluginList_Test` passes.
  - size: M

- [x] M137.P5.T6 — Point ops that need the render scale
  - files: `Engine/Nodes/Image/Op.h`, `Engine/Nodes/Image/StagedImageEffect.{h,cpp}`, `Tests/Native/StagedPointContext_Test.cpp` (new)
  - approach:
    - A Point op's `row(params, io)` and `gpu(params)`, and `StagedImageEffect::gpuOp(time, view)`, see no render scale. CheckerBoard (`CheckerBoard.cpp:412,476`) and Crop (P7.T4: whole-pixel box, softness matte) need it.
    - Mirror `PassesOp`: an optional scale-aware form `isIdentity(params, ctx)`, `row(params, ctx, io)`, `gpu(params, ctx)` with `ctx` the existing `PassContext`. `ImageNode` selects it by requires-expression, as for `costHint`. `gpuOp` takes the query's scale. Ops without the form are unchanged.
    - Listed before P5.T4, which uses it.
  - verify: at the phase build, `StagedPointContext_Test` renders a test op that writes its scale at scale 1 and 0.5 on the CPU (and the GPU where available), and Grade's goldens stay bit-exact.
  - size: M

- [x] M137.P5.T4 — Generators: Constant, Solid, CheckerBoard, without inputs
  - files: `Engine/Nodes/Generator/{Constant,CheckerBoard}.{h,cpp}`, `Engine/Nodes/Image/NativeGenerator.{h,cpp}`, `Tests/Native/{NativeConstant,NativeConstantGpu,NativeCheckerBoardGpu}_Test.cpp`, `Tests/fixtures/native-golden/` (generator fixtures only)
  - approach:
    - Deliberate behaviour change (user): the generators lose their optional input entirely, with no Source input, no stages and `sourceInput` -1, and no longer pass channels through. Delete `describeSourceInput` (`NativeGenerator.cpp:227`); unwritten channels of the target layer are zero.
    - `NativeGenerator` becomes a helper the nodes hold: extent knobs, RoD, preferred metadata, frame range, `knobChanged`/`onKnobsLoaded`, `deriveOutputMetadata`. Each node derives from `ImageNode<…Op>` and forwards those overrides (none is FINAL), plus `isGenerator()`. Spec: no inputs, `LayerPolicy::targetSelect()`, no stages, Point.
    - CheckerBoard uses P5.T6's scale-aware form. Solid is a second `kNodeSpec` over Constant's op. Kernels stay `constant`/`checkerboard`. Retarget the GPU tests' casts.
    - Remove the cases that wire an input into a generator in this task's tests (`NativeConstant_Test.cpp:~402`, the `connected` cases in `NativeConstantGpu_Test.cpp:~177`). P5.T7 handles the other test files.
    - The golden harness wires no source into a generator (`NativeGolden.h:81`), so the generator goldens are expected to replay unchanged. Re-record only the Constant, Solid and CheckerBoard fixtures through the remote `--record-goldens` build, commit only those, and report any fixture that changed.
  - verify: at the phase build, the re-recorded generator fixtures replay green and git shows no change to any other fixture; `NativeConstantGpu_Test`/`NativeCheckerBoardGpu_Test` are within their bounds; `NativeConstant_Test`, `NativeCheckerBoard_Test` and `NativeCrop_Test` (which includes `NativeGenerator.h`) pass.
  - size: L

- [x] M137.P5.T7 — Tests that wire an input into a generator
  - files: `Tests/GeneratorLayer_Test.cpp`, `Tests/NativeMetadataPolicy_Test.cpp`
  - approach: P5.T4 removes the generators' input. Rewrite or delete the cases that connect a node into a generator (`GeneratorLayer_Test.cpp:285,367`, `NativeMetadataPolicy_Test.cpp:~215`), keeping each case's intent where it still applies, such as metadata with no upstream. Grep `Tests/` for any other `connectNodes(x, <generator>, …)` and fix those here too, naming them in the report.
  - verify: at the phase build, these suites pass, and a grep finds no test connecting an input to a Constant, Solid or CheckerBoard node.
  - size: M

## Phase 137.6: Remaining filters on the Passes kind

- [x] M137.P6.T1 — ErodeDilate and Dilate
  - files: `Engine/Nodes/Filter/ErodeDilate.{h,cpp}`, `Tests/Native/NativeErodeDilateGpu_Test.cpp`, `Tests/Native/NativeErodeDilate_Test.cpp`
  - approach:
    - `ImageNode<ErodeDilateOp>`, Passes, `{MaskMix, PassThroughUnprocessed}`, `LayerPolicy::all()`, no Premult (as `Blur.h:123`), so it loses "(Un)premult by".
    - The morphology runs as LineX/LineY or Gather passes. The `morph_direct`/`morph_wide` dispatches go in the list's single `PassList::gpu` recorder (a `PassesOp` has no `gpu()`). The RoD override becomes `clipRegionOfDefinition`. Delete the RoI override, `render`, `canRenderOnGpu`, `getGpuInputRoI`, `renderGpu` and `isIdentityOp` (now `isIdentity(params, ctx)`).
    - Dilate is a second `kNodeSpec` with the mode as an op template argument. `NativeErodeDilateGpu_Test:115` casts both to `ErodeDilate*`: keep that valid or retarget it to `StagedImageEffect*`.
    - Delete the GPU parity cases that set "(Un)premult by" (`:159-162`, `:267`), and flip `NativeErodeDilate_Test:217`'s `hostUnPremultBy` check to absence. The goldens never recorded filter premult cases.
  - verify: at the phase build, the ErodeDilate/Dilate goldens are bit-exact, `NativeErodeDilateGpu_Test` is within its bound, and `NativeErodeDilate_Test` and `ErodeDilateKernels_Test` pass.
  - size: L

- [x] M137.P6.T2 — EdgeDetect
  - files: `Engine/Nodes/Filter/EdgeDetect.{h,cpp}`, `Tests/Native/NativeEdgeDetect_Test.cpp`
  - approach:
    - Passes op on the CPU only (GPU port: M110 - Remaining Native GPU Ports): leave `PassList::gpu` empty.
    - `{MaskMix, PassThroughUnprocessed}`, `LayerPolicy::color(true, true, true, false)` (its traits today), no Premult. The operator is a Gather pass. The RoD override becomes `clipRegionOfDefinition`. Delete `render`, plus the RoI and `addAcceptedComponents` overrides where the framework answers the same.
    - Flip `NativeEdgeDetect_Test:180`'s `hostUnPremultBy` check to absence.
  - verify: at the phase build, EdgeDetect's goldens are bit-exact and `NativeEdgeDetect_Test` passes.
  - size: M

- [x] M137.P6.T3 — Delete `SpatialFilterSupport.h`
  - files: `Engine/Nodes/Filter/SpatialFilterSupport.h`
  - approach: the helpers already live in `PassExec` and the `.cpp` is gone. The header is a `using` shim whose last callers P6.T1/P6.T2 remove. Delete it; no CMake file lists it.
  - verify: at the phase build, greps for `SpatialFilterSupport` and `SpatialFilter::` in `Engine/` and `Tests/` find nothing, and the filter tests pass.
  - size: S

## Phase 137.7: Resample kind and the Transform family

- [ ] M137.P7.T1 — Resample kind on the CPU
  - files: `Engine/Nodes/Image/Op.h`, `Engine/Nodes/Image/ResampleExecutor.{h,cpp}` (new), `Engine/Nodes/Image/StagedImageEffect.{h,cpp}`, `Tests/Native/StagedResample_Test.cpp` (new)
  - approach:
    - Add a `ResampleOp` concept and admit it in `OpOfKind`: `bake`, `isIdentity(params, ctx)`, and either a 3×3 (`transform(params, ctx)`) or an integer offset (Position), plus the filter, clamp and black-outside settings `Resampler` takes. `ctx` is `PassContext`.
    - The CPU executor wraps `Resampler::buildSamplingTransforms`, `resampleRow` and the region helpers (`Resampler.h:445-595`). Prefer a row function under `runStagesCpu` that samples the source image, so mask, mix and pass-through stay the moved loop. Use a separate loop only if the goldens require it.
    - Add the Resample branches to `StagedImageEffect`'s RoD and RoI, which handle only Route and Passes today. `ImageNode` answers `getTransform`/`getCanTransform`/`getInputsHoldingTransform` from the op's 3×3, so concatenation goes through it.
    - Leave out the STMap write-coordinates flag; no node here needs it.
  - verify: at the phase build, `StagedResample_Test` renders a translate+scale test op bit-exact against `Resampler` called directly, at scale 1 and 0.5, with mask and mix 0.5, and checks its RoD, RoI and `getTransform`.
  - size: L

- [ ] M137.P7.T2 — Transform and TransformMasked on the CPU
  - files: `Engine/Nodes/Transform/Transform.{h,cpp}`
  - approach:
    - `TransformNode` derives from `ImageNode<TransformOp>`; class names stay (tests cast to `TransformNode*`). Transform has no stages (`rendersUnprocessedChannels()` is false today and it has no mix). TransformMasked has `{MaskMix}`, replacing its `addMaskMixKnobs` call (`:563`). Both use `LayerPolicy::all()` and Resample.
    - Delete `render` (1077), `isIdentity` (1032), the RoD/RoI overrides, `rendersUnprocessedChannels` (FINAL in the base) and the GPU overrides (1339-1371). Transform renders on the CPU until P7.T5, in the same phase. `getCanTransform` stays `!masked`.
  - verify: at the phase build, the Transform and TransformMasked goldens are bit-exact and `NativeTransform_Test` passes, including its concatenation cases.
  - size: L

- [ ] M137.P7.T3 — Reformat
  - files: `Engine/Nodes/Transform/Reformat.{h,cpp}`
  - approach: `Reformat` (class name kept; `NativeReformat_Test:411` casts to it) derives from `ImageNode<ReformatOp>`: Resample, CPU only (M110), no stages, `LayerPolicy::all()`. Its `rendersUnprocessedChannels` override goes because it is FINAL in the base. `isIdentityOp` (646) becomes `isIdentity(params, ctx)` and `render` (1121) goes. The output-format and metadata overrides stay.
  - verify: at the phase build, the Reformat goldens are bit-exact and `NativeReformat_Test` passes.
  - size: M

- [ ] M137.P7.T4 — Position, and Crop as a rounding point op
  - files: `Engine/Nodes/Transform/{Position,Crop}.{h,cpp}`
  - approach:
    - Position: Resample op with an integer offset, CPU only, `LayerPolicy::all()`. `isIdentity` (296) becomes the op's. Delete `render` and the RoD/RoI overrides the Resample branch now answers.
    - Crop is a pure Point op (user), not Resample or Custom. Its box rounds to whole pixels internally at the render scale, since a fractional-pixel crop is meaningless. Softness and black-outside form a matte the row multiplies by, using P5.T6's scale-aware form. `getRegionOfDefinition` (not FINAL) returns the rounded box from the `ExtentKnobs` it has. No stages (`rendersUnprocessedChannels()` is false today; the override goes).
    - `reformat` shifts the output origin, and today the render reads a shifted source (`Crop.cpp:~590`, `tx`/`ty`). Express it without that read: move the output RoD/format and treat the shift as an integer translation the engine already handles (e.g. through `getTransform` concatenation or the output format's offset). If that truly cannot keep Crop a point op, stop and report; do not pick another kind.
    - Delete both `render` overrides.
    - Crop's goldens may change where the old code cut at fractional edges. Any diff must be confined to edge pixels of fractional-box cases. Report each diff instead of re-recording.
  - verify: at the phase build, the Position goldens are bit-exact; the Crop goldens are bit-exact except reported edge-pixel diffs in fractional-box cases; and `NativePosition_Test` and `NativeCrop_Test` pass.
  - size: L

- [ ] M137.P7.T5 — Resample on the GPU, Transform's GPU path, and the motion-blur cost hint
  - files: `Engine/Nodes/Image/StagedImageEffect.{h,cpp}`, `Engine/Gpu/GpuResample.cpp`, `Engine/Nodes/NativeEffectBase.h`, `Engine/GpuCostModel.cpp`, `Tests/Native/NativeTransformGpu_Test.cpp`
  - approach:
    - `canRenderOnGpu`/`renderGpu` gain a Resample branch over `resample.slang` through `GpuResample.cpp`. It is ported from P7.T2's deleted `TransformNode` overrides, with mask and mix from the `StagePlan`. Ops without a GPU form (Reformat, Position) stay refused.
    - `GpuCostModel.cpp:254` still casts to `TransformNode` for motion blur. Add `motionBlur` to `GpuCostHint`, let `ImageNode::gpuCostHint` ask Resample ops for a hint, and have the Transform op report it. Delete the cast and the `Transform.h` include.
    - Retarget `NativeTransformGpu_Test`'s casts where needed.
  - verify: at the phase build, `NativeTransformGpu_Test` is within its bound, `GpuPlacement_Test` passes, and a grep for `TransformNode` in `Engine/GpuCostModel.cpp` finds nothing.
  - size: L

## Phase 137.8: Composite kind, keyers and Merge

- [ ] M137.P8.T1 — Composite kind on the CPU
  - files: `Engine/Nodes/Image/Op.h`, `Engine/Nodes/Image/StagePlan.{h,cpp}`, `Engine/Nodes/Image/StagedImageEffect.{h,cpp}`, `Tests/Native/StagedComposite_Test.cpp` (new)
  - approach:
    - Add a `CompositeOp` concept and admit it in `OpOfKind`: `bake`, `isIdentity(params)`, `sources(params, RouteContext)` returning this evaluation's active (input, time) list, and `row(params, io)` over `io.src[0..n)`. At most `kPixelKernelMaxInputs` (4) sources and no fold: no node here reads more (Dissolve 2, Keyer/ChromaKeyer 4).
    - `StagePlan` carries the source list. `runStagesCpu` fills `RowIO::src[i]`, reading each input through its `InputSpec::layer` (the first executor that reads it). A Mask-role source is read as the one channel its mask selector names. Rows outside an input's RoD read zero.
    - The stages blend against a blend source the op names per evaluation (default: the first Source). The composed identities in `StagedImageEffect::isIdentity` resolve to that input, not to `getSourceInput()`.
    - `ImageNode::route()` calls an op's `route(params, ctx)` for any kind when the op has one; today it does so only for Route.
    - The GPU side is P8.T6.
  - verify: at the phase build, `StagedComposite_Test` renders a two-source test op with an offset second input under mask and mix, a blend source other than input 0, a four-source op, and a Point op whose `route()` answers identity.
  - size: L

- [ ] M137.P8.T6 — Composite kind on the GPU
  - files: `Engine/Gpu/Kernels/pixelwrap.slang`, `Engine/Gpu/GpuWrap.{h,cpp}`, `Engine/Nodes/Image/StagePlan.cpp`, `Engine/Nodes/Image/StagedImageEffect.cpp`, `Tests/Gpu/PixelWrap_Test.cpp`
  - approach:
    - `IBinaryPixelOp` does not exist, and Keyer/ChromaKeyer read four inputs. Add an n-source op interface beside `IPixelOp` (`pixelwrap.slang:57`), with a `runWrapped` that binds up to `kPixelKernelMaxInputs` source views and applies the same divide, mask, mix and finish as the one-source path.
    - `recordStagesGpu` binds the plan's sources. `canRenderOnGpu` admits Composite when `canRunStagesGpu` holds and every source is a usable float view. `getGpuInputLayer` answers each source from its `InputSpec::layer`.
    - Listed directly after P8.T1; P8.T3 and P8.T4 need it.
  - verify: at the phase build, on the remote GPU build, `PixelWrap_Test` checks a two-source and a four-source op against a CPU transcription.
  - size: L

- [ ] M137.P8.T2 — Dissolve
  - files: `Engine/Nodes/Merge/Dissolve.{h,cpp}`
  - approach:
    - Composite op, CPU only (M110), `LayerPolicy::all()`. Inputs keep their numbering: `{"0" Source, "1" Source, "Mask" Mask, Variadic count 62 firstNumber 2}`; the mask is input 2 (`kDissolveMaskInput`).
    - `sources()` returns floor and ceil of `which`. At an integer `which`, `route()` answers that input. The blend source is the lower input (floor of `which`), as its mask does today (`:398-407`).
    - `{MaskMix, PassThroughUnprocessed}` replaces its own `maskInvert` (`:262-269`; same script name) and gives Dissolve a `mix` knob (approved by the user).
    - `updateRange` and `getFramesNeeded` stay in the node.
  - verify: at the phase build, the Dissolve goldens are bit-exact (mix at its default 1) and `NativeDissolve_Test` passes.
  - size: M

- [ ] M137.P8.T3 — Keyer, with `keyer_main` as an n-source op
  - files: `Engine/Nodes/Keyer/Keyer.{h,cpp}`, `Engine/Gpu/Kernels/keyer.slang`, `Engine/Gpu/CMakeLists.txt`, `Tests/Native/NativeKeyerGpu_Test.cpp`
  - approach:
    - Composite op over Source, InM, OutM and Bg, with InM/OutM as Mask-role sources read through their selectors (P8.T1). No MaskMix (no mix knob today). Unprocessed channels as today; the goldens decide on PassThroughUnprocessed. `isIdentity` stays false.
    - Move `keyer_main`'s body into a P8.T6 n-source entry, and delete the standalone entry and its `slang_add_kernel` line. Delete `render`, `isIdentity` and the GPU overrides. Retarget the test's casts.
    - Behaviour stays unchanged; M95 - Knob Cleanup And Keyer Redesign redesigns it later.
  - verify: at the phase build, the Keyer goldens are bit-exact, `NativeKeyerGpu_Test` is within its bound, and `NativeKeyer_Test` passes.
  - size: L

- [ ] M137.P8.T4 — ChromaKeyer, with `chromakeyer_main` as an n-source op
  - files: `Engine/Nodes/Keyer/ChromaKeyer.{h,cpp}`, `Engine/Gpu/Kernels/chromakeyer.slang`, `Engine/Gpu/CMakeLists.txt`, `Engine/Nodes/NodeSpec.h`, `Tests/Native/NativeChromaKeyerGpu_Test.cpp`
  - approach:
    - As P8.T3.
    - Its `getGpuInputLayer` (`:1147`) and its CPU path read Source and Bg in their resolved colour layer, whatever plane renders. `InputLayer` has only `plane`/`select`/`pairWith`. Add the kind this needs (e.g. `colorLayer()`) and declare it on Source and Bg, so the executors' per-source layer read replaces the override. `getGpuReadableInput` goes with the GPU overrides.
  - verify: as P8.T3, with `NativeChromaKeyer_Test`.
  - size: L

- [ ] M137.P8.T5 — Merge as a Custom node
  - files: `Engine/Nodes/Merge/Merge.{h,cpp}`
  - approach:
    - Re-base `MergeNode` onto `StagedImageEffect`, `OpKind::Custom`, with a `NodeSpec` per preset (`MergePreset<P>`, `Merge.h:191`): B (input 0) as the first Source, then A, Mask, and a Variadic run for A2… (`firstNumber` 2). Stages: MaskMix, plus PassThroughUnprocessed if its render copies unprocessed channels today.
    - `render`/`isIdentity` are FINAL. `render` (931) becomes `renderOp(args, plan)`. `isIdentity` (861) becomes `isOpIdentity`, keeping only its own rule (no A input meets the window). Mix 0 and mask-miss come from the composition and must agree with the old answers.
    - The blend reads mask, invert and mix from the `StagePlan`. `addMaskMixKnobs` (`:692`) goes; the stage creates the same `mix`/`maskInvert`. GPU stays refused. M101 - Clean-Sheet Merge Node replaces the node later.
  - verify: at the phase build, P4.T6's Merge goldens are bit-exact, and `NativeMerge_Test` and `MergeOperators_Test` pass.
  - size: L

## Phase 137.9: Delete the old base and document

- [ ] M137.P9.T1 — Delete `NativeImageEffect`, `NativeImageTraits` and the description virtual
  - files: `Engine/Nodes/Image/NativeImageEffect.{h,cpp}`, `Engine/Nodes/NativeEffectBase.{h,cpp}`, `Engine/Nodes/Image/PixelKernel.h`, `Engine/Gpu/GpuWrap.h`, `Engine/Nodes/Image/Resampler.h`
  - approach:
    - Delete the files; no CMake file lists them. The old base's `getGpuStageSummary` goes with them.
    - Remove `getNativePluginDescription()`, `NativePluginDescription`/`NativeInputDescription` and the fallback branches at `NativeEffectBase.cpp`'s query sites. `NodeSpec` becomes the only description, and `nodeSpec()` becomes pure virtual.
    - Reword the comments naming `NativeImageEffect` in `PixelKernel.h`, `GpuWrap.h`, `Resampler.h` and `pixelwrap.slang`. Drop `PixelKernel`/`KernelContext` if nothing uses them.
  - verify: at the phase build, a grep for `NativeImageEffect`, `NativeImageTraits`, `NativePluginDescription` and `getNativePluginDescription` in `Engine/` (README aside, P9.T4) finds nothing, and the full ctest passes.
  - size: M

- [ ] M137.P9.T2 — Move the framework tests off the old base
  - files: `Tests/Native/NativeImageEffect_Test.cpp`, `Tests/Native/NativeImageEffectGpu_Test.cpp`, `Tests/Native/NativeRuntimeShaderGpu_Test.cpp`, `Tests/SerialTests.cmake`, `Tests/CMakeLists.txt`
  - approach:
    - Rewrite the test effects as spec'd `ImageNode<Op>`s. Rename the first two files and their suites to `StagedImageEffect*`, and update `SerialTests.cmake:13` and the kernel registration at `Tests/CMakeLists.txt:150` to match.
    - `NativeRuntimeShaderGpu_Test` keeps its two staged-op cases and drops its old-base effect.
    - Keep each case's intent; say in the report where an assertion changed.
  - verify: at the phase build, these suites pass, including the serial one.
  - size: M

- [ ] M137.P9.T3 — Remove the remaining old-base references in tests
  - files: `Tests/Native/NativeGpuParity.{h,cpp}`, `Tests/Gpu/PixelWrap_Test.cpp`
  - approach: drop `NativeGpuParity`'s `NativeImageEffect&` overload (P5–P8 retargeted every per-node GPU test) and reword `PixelWrap_Test.cpp:315`'s comment. Make no assertion changes.
  - verify: at the phase build, a grep for `NativeImageEffect`, `NativeImageTraits` and `getNativePluginDescription` in `Tests/` finds nothing, and the full ctest passes.
  - size: S

- [ ] M137.P9.T4 — Rewrite the node-authoring README sections
  - files: `Engine/Nodes/README.md`
  - approach:
    - Rewrite "Writing a node, start to finish" (`:385-458`) and "Flat image nodes" (`:658-797`) for `NodeSpec`, `ImagePipelineSpec`, the `Op` kinds, stages and footer, and shared C++/Slang maths headers.
    - Update "Single hierarchy" (`:30-45`) to say `StagedImageEffect` is a convenience layer on `NativeEffectBase`, not a parallel hierarchy.
    - Point to the design-note decision file.
  - verify: review against the design note; no stale `NativeImageEffect` references remain.
  - size: M

## Phase 137.10: Gate evidence

- [ ] M137.P10.T1 — After-timings, interleaved with the before build
  - files: `tools/bench/BASELINE.md`
  - approach:
    - P1.T2's table spreads 19–50% and cannot judge the gate. On a quiet host with no agents or builds running, run the before build (the e349888f0 release AppImage under `build/m137-before/`) and this branch's release AppImage back-to-back, interleaved per round, three rounds each.
    - Cases: `node_profile.sh cpu 8` for grade, blur, transform and dissolve, plus `graph_bench.py` `blurchain`, `xfchain` and `ccchain`.
    - Record the paired table beside the before-table, with host, SHAs, per-case deltas and spreads.
  - verify: every case's after-median is within the pair's spread or within 3%, whichever is larger. A miss blocks the gate and goes to the user, even for Passes overhead that M139 - Blur In-Graph Overhead owns.
  - size: M

- [ ] M137.P10.T2 — Panel screenshots and AppImage
  - files: `Tests/gui/m137_panels.py` (new), `build/appimages/M137-<sha>.AppImage`
  - approach:
    - Under Xvfb, open Grade, Saturation, Blur, ErodeDilate, Transform, Dissolve and Switch.
    - Screenshot each main page, showing the footer order and filters without "(Un)premult by", and share the shots with the user.
    - Package the AppImage with `tools/ci/local/package.sh fast`.
  - verify: the user approves the shots.
  - size: M

## Decisions

- 2026-10-19 — Claimed despite the file-overlap test (user override): M137 shares `Engine/GpuRegionTask.cpp`, `GpuCostModel.cpp`, `RenderScheduler.cpp` and `Tests/GpuResidency_Test.cpp` with M109 - GPU Region Engine Hardening, `GpuCostModel.cpp` with M107 - GPU Viewer Hand-off, `AppManager.cpp`/`Tests/CMakeLists.txt` with the parked M64 - Tiled Rendering, and `Engine/Nodes/README.md` with M78 - Metadata In Expressions. The user accepted rebasing over M109/M107 and resolving the conflicts in the scheduler and cost-model files.
- 2026-10-19 — Goldens carry a build-config `STAMP`: a replay is bit-exact only in the configuration that recorded them, and uses the `NativeParity` CPU tolerances elsewhere (PR CI debug vs remote `fast` at -O1 can differ by ulps). They are recorded remotely through a `record_goldens` input on `remote-build.yml` (`tools/ci/remote/build.sh --record-goldens`), since this host builds remotely. The migration gate compares in the recording configuration.

- 2026-10-19 — Scope is a blue-sky redesign, not a trait fix. The Fable design has a declarative spec, an `Op` concept, and one `StagePlan` with CPU and GPU executors (user): mixins and per-feature flags leave `render()` overridable, which is where the triplicated blend and identity rules came from.
- 2026-10-19 — The milestone covers every image op kind (Point, Composite, Route, Passes, Resample, Custom) and migrates every image node (user): one milestone rather than a split, accepting that it serialises the E3 ports while it runs.
- 2026-10-19 — Build the kind-agnostic shared layer (`NodeSpec`, the route hook, `KnobReader`/`bake`) now (user): the image framework proves it, so deep, scene and geometry adopt it rather than fork it.
- 2026-10-19 — Every native node moves onto `NodeSpec`, non-image ones included (user): no second way to declare a node survives.
- 2026-10-19 — This milestone runs before M120 - Native Node Port Kit, whose fragments are generated from `NodeSpec` (user): registration is designed once. M120 gains a dep on M137.
- 2026-10-19 — Merge is not migrated. It goes Custom and M101 - Clean-Sheet Merge Node builds on the framework (user): migrating a node that is about to be rewritten is wasted work. M101 gains a dep on M137.
- 2026-10-19 — Delete `NativeImageEffect` at the end of this milestone (user): no second framework survives.
- 2026-10-19 — This milestone runs before M110 - Remaining Native GPU Ports (user): those ports become GPU bodies for `recordStagesGpu`, so nothing is hand-wrapped and then migrated. M110 gains a dep on M137.
- 2026-10-19 — This milestone owns stage-knob placement below the line, taken from M95 - Knob Cleanup And Keyer Redesign (user): the framework creates those knobs, so it places them.
- 2026-10-19 — Order of work is spike, then design note, then one approval stop (user). The spike covers Grade, Blur and Switch on CPU and GPU, and its code is kept if approved.
- 2026-10-19 — Switch moves from M121 - Native Routing And Mix into this milestone as the Route proof (user). No native Route node exists yet.
- 2026-10-19 — Gate: bit-exact on the CPU, README tolerance on the GPU (user): the CPU maths is unchanged, so any ulp change is a bug.
- 2026-10-19 — Gate: no performance regression beyond noise (user): the framework must not tax every node.
- 2026-10-19 — M139 - Blur In-Graph Overhead runs after this milestone (user): it then optimises the shared Passes executor for every node that uses it. M139 gains a dep on M137.
- 2026-10-19 — IDs carry over unchanged; renames stay with M111 - Native Node Identity Cleanup (user): this milestone is behaviour-preserving.
- 2026-10-19 — An invalid spec is caught at compile time (user).
- 2026-10-19 — Keyer and ChromaKeyer are migrated as they are. M95 - Knob Cleanup And Keyer Redesign's redesign lands on the framework (user). M95 gains a dep on M137.
- 2026-10-19 — Point and composite GPU bodies are converted to `IPixelOp`/`IBinaryPixelOp` here (user): a standalone kernel would be a second GPU path, and it is also the precondition for M87 - Point-Op Kernel Fusion.
- 2026-10-19 — P2.T1 shapes (code a5f0efc72): `nodeSpec()` is a virtual returning a pointer (not CRTP), so runtime specs (MissingPluginNode, presets) work; nodes return `staticNodeSpec<kSpec>()`, which carries the `static_assert`s. `Variadic` is a role plus `count`, last only, labels `label + (firstNumber + k)`. `Route::black()` maps to identity with input -1, which the render paths already treat as empty. **Open for P2.T8 (Switch with no inputs):** `EffectInstance::isIdentity_public`'s cache (`EffectInstance.cpp:~4176-4180`) reads a cached -1 back as "not identity", so a cached Black would render; fix the cache encoding there. `route()` has no scale/RoI, so window-dependent identity (mask missing the window) composes in `StagedImageEffect::isIdentity`. Spec text is untranslated `string_view`.
- 2026-10-19 — P2.T2 shapes (code 1523db758): `NativeImageEffect::render` and `StagedImageEffect` share one `runStagesCpu` (the loop moved verbatim; mask layer/channel now resolve before the divisor's layers are read, arithmetic unchanged). `StagedImageEffect::isIdentity`/`render` are FINAL; nodes implement `isOpIdentity`/`renderOp`. **For P2.T5:** Grade needs `Stage::PassThroughUnprocessed` in its `StageSet`, or the host copies the unprocessed channels instead (bit-exactness risk). **For P8.T5:** Merge as a Custom node goes through `renderOp`/`isOpIdentity`, since `render`/`isIdentity` are FINAL.
- 2026-10-19 — P1.T2 before-timings are recorded in `tools/bench/BASELINE.md` but are noisy: per-node spread is 19–50%, from a loaded 4-core host, while graph chains were within 5% apart from xfchain. The rig for repeating them is the release AppImage extracted under `build/m137-before/`. **For P10.T1:** re-measure the before build (`e349888f0` AppImage) and the after build back-to-back, interleaved, on a quiet host with no agents running, and judge the gate on that pair. P2.T4 makes `NativeImageEffect::canRenderOnGpu` resolve a stage plan per call, so watch placement cost.
- 2026-10-19 — Design note and spike approved by the user at the P3.T2 stop, departures included (`DECISIONS/2026-10-19-native-node-framework.md`); migration continues on the same branch after a §5a spot-check of Phases 137.4–137.9.
- 2026-10-20 — Crop keeps its current behaviour exactly, including `reformat` translating the box to the origin (user, superseding the earlier point-op/rounding and framing-only answers). It goes on the Resample kind's integer-offset form with no new rounding; softness/black-outside need a small per-pixel weight hook in the Resample executor. All Crop goldens must stay bit-exact.
- 2026-10-20 — The Fill PyPlug is deleted (user: not useful). Its inner Solid relied on a source input that native generators no longer have.
- 2026-10-20 — PIKColor's two Dilate nodes stop setting "(Un)premult by"; their input has R/G/B at 0, so the output is unchanged. The channel-set unpremult test uses an OFX Ramp as its pass-through generator.
- 2026-10-20 — The Resample kind runs its own loop (`runResampleCpu`), not a row function under `runStagesCpu`: it must fetch through the concatenated input transform and sample the whole source per plane. Its mask/mix blend is a copy of `runStagesCpu`'s, and Premult is refused for Resample ops. `transform()` takes `amount`/`invert` for motion and directional blur; `ResampleSettings::transformIsIdentity` is the region identity, separate from the node identity.
- 2026-10-20 — Position keeps `Stage::PassThroughUnprocessed`, since its old render copied unprocessed channels. TransformMasked is no longer a `TransformNode` (`TransformImageNode<MASKED>`); P7.T5 must also cover it where `GpuCostModel` and `NativeTransformGpu_Test` cast to `TransformNode`.
- 2026-10-20 — Phase 4 and Phases 5–6 builds re-recorded every golden bit-identically (generators included); Merge goldens recorded at the Phase 4 build.
- 2026-10-19 — Elaborated with a spike and a baseline phase ahead of the approval stop (planner): golden renders and timings must be captured on unchanged code, so they come before any framework commit. During migration the engine reaches GPU virtuals through `NativeEffectBase`, so both bases coexist until Phase 137.9.
- 2026-10-19 — §5a spot-check after approval: 22 of the 27 unchecked tasks in Phases 137.4–137.10 were rewritten, 5 passed, and 8 were added (P4.T6–T8, P5.T5–T7, P7.T5, P8.T6). Every `files` path existed, but the briefs predated the spike's final shapes. Fixes:
  - the FINAL `render`/`isIdentity`/`rendersUnprocessedChannels`, with `renderOp`/`isOpIdentity` instead;
  - `PassList::gpu` rather than an op `gpu()` for Passes;
  - the old base's `{MaskMix, PassThroughUnprocessed}` stage set;
  - the unbuilt Resample and Composite kinds;
  - per-node GPU tests that cast to `NativeImageEffect*`;
  - about 15 test effects overriding `getNativePluginDescription`;
  - Merge, which had no goldens;
  - stale greps (`hostUnPremult` matches the kept `hostUnPremultBy`).
  Oversized tasks split (P5.T2, P7.T1, P8.T1).
- 2026-10-19 — Curve-driven Point ops (ColorCorrect, ColorLookup) relax `Params` to `std::copyable`, holding a `std::shared_ptr` to immutable tables cached by the curve knobs' hash, and `GpuOpRef::packed` carries their packed GPU words (user): their curves have unbounded keys and the CPU evaluates them outside the LUT, so a trivially-copyable inline snapshot cannot stay bit-exact.
- 2026-10-19 — Constant, Solid and CheckerBoard lose their optional input: no Source input, no stages, `sourceInput` -1, no pass-through (user). This is a deliberate behaviour change; P5.T4 re-records only the generator goldens.
- 2026-10-19 — Crop is a pure Point op whose box rounds to whole pixels internally, with softness a matte the row multiplies by; `reformat` is expressed as an output RoD/format move and an integer translation, never a shifted source read, and P7.T4 stops and reports if that fails (user): a crop to a fractional pixel makes no sense. Golden diffs are allowed only at the edge pixels of fractional-box cases, and are reported.
- 2026-10-19 — Dissolve gains a `mix` knob through the MaskMix stage, replacing its own mask handling (user).
- 2026-10-20 — Dissolve (code 1193cf0c8): rounding-only diffs from the op/stage split are accepted and its goldens re-recorded (user: don't revisit architectural decisions to fix rounding errors). Masked and one-input pixels can move by a few ulps, and unprocessed channels now always come from the lower input. The same rule applies to any later node whose conversion moves results only by rounding.
- 2026-10-20 — Composite (code 74836e031) refuses the Premult stage at compile time: no single divisor applies to several inputs.
- 2026-10-20 — Merge (code 6b81168b2) keeps its own early mask-miss identity return in `isOpIdentity` beside the framework's check, so the old answers hold when the mask region fails or touches the window edge. One old answer can't be kept: with a B channel off under an enabled output channel and a non-inverted mask missing the window, the framework now answers identity to B. No golden or test covers that case.
- 2026-10-19 — Untranslated `NodeSpec` text needs no owner: the tree has no `.ts`/`.qm` translation files, so the `tr()` calls it replaces were no-ops and nothing regresses.

**Verification gate:**
- The full build and ctest pass on both scheduler modes. This includes:
  - `NativeGolden_Test` bit-exact, in the recording configuration (STAMP), for every recorded case of every migrated node. Exceptions: the generator fixtures were re-recorded by P5.T4 after the generators lost their input. Crop's goldens, `Crop_reformat_*` included, stay bit-exact (P7.T4). Merge's cases come from P4.T6. Rounding-only diffs from a conversion (Dissolve, P8.T2) are re-recorded, not fixed.
  - Every `Native*Gpu_Test` within its README bound on the remote GPU build. The Blur and ErodeDilate parity cases that set "(Un)premult by" were removed with the knob.
  - `NodeSpec_Test`, `NativeSwitch_Test`, `StageKnobFooter_Test`, `StagedPointContext_Test`, `StagedResample_Test`, `StagedComposite_Test` and `NativeChannelNodesSpec_Test`.
- Greps for `NativeImageEffect`, `NativeImageTraits`, `NativePluginDescription`, `getNativePluginDescription`, `SpatialFilterSupport`, `grade_main`, `keyer_main` and `chromakeyer_main`, and a whole-word grep (`grep -w`) for `hostUnPremult`, find nothing in `Engine/` or `Tests/`. The knob script name `hostUnPremultBy` stays. `Engine/GpuCostModel.cpp` casts to no node class.
- The `getPluginIDs()` dump equals the pre-milestone one, plus `fr.natron.Switch`.
- P10.T1's interleaved before/after timings show no regression beyond noise.
- The user approved:
  - the design note (P3.T2);
  - the post-approval departures: curve Params copyable with shared tables (P5.T2/P5.T5), generators losing their inputs (P5.T4), and Dissolve gaining mix (P8.T2);
  - the panel screenshots (P10.T2).
- CI is green.
