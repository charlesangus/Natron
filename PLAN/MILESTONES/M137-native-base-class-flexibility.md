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
- Every point and composite GPU body becomes an `IPixelOp`/`IBinaryPixelOp` entry in the `pixelwrap.slang` style. There are no standalone kernels such as grade.slang's `grade_main`.
- Shared C++/Slang maths headers become the parity convention, and the Op names its header.
- `NativeImageEffect`, `NativeImageTraits` and `SpatialFilterSupport` are deleted. Merge is re-based as a Custom node that keeps its own `render()`.
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
- Merge stays on its own `render()` as a Custom node until M101 - Clean-Sheet Merge Node, so no second framework survives this milestone.
- The scene-to-image bridges (HydraRender, ProjectTexture) remain Custom image nodes with a scene input.
- An invalid spec, such as a Premult stage with no source input, a Route op with stages, or a mask role on a non-image input, is a compile error (constexpr spec plus `static_assert`/concepts). There is no runtime path for it.
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

- [ ] M137.P3.T2 — Approval stop
  - files: none (user decision); on rejection, the milestone branch
  - approach:
    - Share Xvfb screenshots of the Grade, Blur and Switch panels.
    - Set the row `blocked`, and add one `# Open questions` item asking the user to approve the design note and the spike.
    - **Approve:** record the decision, spot-check Phases 137.4–137.9 against the note per PLAN-FORMAT.md §5a, and continue on the same branch.
    - **Reject:** revert Phase 137.2's commits on the milestone branch, then hand back to `/cat-plan`, as the Scope edge case says.
  - verify: the user's answer is recorded under `## Decisions`.
  - size: S

## Phase 137.4: Every non-image native node onto `NodeSpec`

- [ ] M137.P4.T1 — Metadata nodes and `TypedPassthrough`
  - files: `Engine/Nodes/Metadata/{ModifyMetadata,CopyMetadata,RemoveMetadata,ViewMetadata}.{h,cpp}`, `Engine/Nodes/TypedPassthrough.{h,cpp}`
  - approach: replace each `getNativePluginDescription()` with a static `NodeSpec`, field for field. Pass-through identity goes through `route()` where the node is identity today.
  - verify: at the phase build, the metadata tests and `NativePluginList_Test` pass, and the `getPluginIDs()` dump is unchanged.
  - size: M

- [ ] M137.P4.T2 — Channel nodes: Shuffle, ShuffleCopy, AddLayers, RemoveLayers
  - files: `Engine/Nodes/Channel/{Shuffle,AddLayers,RemoveLayers}.{h,cpp}`, `Tests/Native/NativeChannelNodesSpec_Test.cpp` (new)
  - approach: as P4.T1. These stay multi-planar `NativeEffectBase` nodes and do not use the image pipeline. Because they have no native tests today, add a test of each one's ID, inputs and data kinds.
  - verify: at the phase build, the new test, the channel-set render tests and `NativePluginList_Test` pass.
  - size: M

- [ ] M137.P4.T3 — Deep I/O and bridges: DeepRead, DeepWrite, DeepMerge, DeepToImage, DeepFromImage
  - files: `Engine/Nodes/Deep/{DeepRead,DeepWrite,DeepMerge,DeepToImage,DeepFromImage}.{h,cpp}`
  - approach: as P4.T1, with input kinds from the spec. Do not touch the deep render paths.
  - verify: at the phase build, the deep tests pass.
  - size: M

- [ ] M137.P4.T4 — Remaining deep nodes: DeepRecolor, DeepCrop, DeepExpression, DeepReformat, DeepRemoveLayers, DeepAddLayers
  - files: `Engine/Nodes/Deep/{DeepRecolor,DeepCrop,DeepExpression,DeepReformat,DeepRemoveLayers,DeepAddLayers}.{h,cpp}`
  - approach: as P4.T3. DeepAddLayers keeps its `appendNodeContentHash` override.
  - verify: at the phase build, the deep tests pass.
  - size: M

- [ ] M137.P4.T5 — NativeRead, NativeWrite and MissingPluginNode
  - files: `Engine/Nodes/IO/NativeRead.{h,cpp}`, `Engine/Nodes/IO/NativeWrite.{h,cpp}`, `Engine/Nodes/MissingPluginNode.{h,cpp}`
  - approach:
    - As P4.T1. `isWriter` comes from the spec.
    - MissingPluginNode's description is built at runtime from the missing plug-in's record, so give it a runtime `NodeSpec` constructor. Keep that the only runtime-built spec, and document why in its header.
  - verify: at the phase build, the Read/Write and MissingPlugin tests pass.
  - size: M

## Phase 137.5: Point ops and generators

- [ ] M137.P5.T1 — Invert, Clamp and Saturation
  - files: `Engine/Nodes/Color/{Invert,Clamp,Saturation}.{h,cpp}`
  - approach: each becomes `ImageNode<…Op>` with `Stages{Premult, MaskMix}` and its current layer defaults. Use the existing `*_wrapped`/`invert_main` `IPixelOp` bodies, with `Params` equal to today's GPU params structs.
  - verify: at the phase build, the three nodes' goldens are bit-exact, their `Native*Gpu_Test` stay within their bounds, and their `Native*_Test` pass.
  - size: M

- [ ] M137.P5.T2 — ColorCorrect and ColorLookup
  - files: `Engine/Nodes/Color/{ColorCorrect,ColorLookup}.{h,cpp}`, `Engine/Nodes/Image/CurveSnapshot.h`
  - approach: as P5.T1. ColorLookup's curve LUT texture stays a `makeGpuShaderInputs`-style input carried by its `GpuOpRef`.
  - verify: as P5.T1, for both nodes.
  - size: M

- [ ] M137.P5.T3 — ColorMath (Add, Multiply, Gamma)
  - files: `Engine/Nodes/Color/ColorMathNode.{h,cpp}`, `Engine/AppManager.cpp`
  - approach: one `ImageNode<ColorMathOp>`. The three preset factories become three `NodeSpec`s sharing the op, with IDs unchanged.
  - verify: as P5.T1, for all three IDs.
  - size: M

- [ ] M137.P5.T4 — Generators: Constant, Solid, CheckerBoard
  - files: `Engine/Nodes/Generator/{Constant,CheckerBoard}.{h,cpp}`, `Engine/Nodes/Image/NativeGenerator.{h,cpp}`, `Engine/Nodes/Image/ExtentKnobs.{h,cpp}`
  - approach:
    - `NativeGenerator` becomes a helper used by generator ops, not a base class.
    - Each spec has `Layers::TargetSelect`, an optional Source input (`describeSourceInput`, `NativeGenerator.cpp:227`), no stages (they have none today), and a Point op with no sources.
    - Solid stays a spec variant of Constant.
  - verify: as P5.T1, plus `NativeCrop_Test`, which includes `NativeGenerator.h`.
  - size: L

## Phase 137.6: Remaining filters on the Passes kind

- [ ] M137.P6.T1 — ErodeDilate and Dilate
  - files: `Engine/Nodes/Filter/ErodeDilate.{h,cpp}`, `Tests/Native/NativeErodeDilateGpu_Test.cpp`
  - approach: Passes op over the `morph_direct`/`morph_wide` kernels with `Stages{MaskMix}`, and no Premult (as Blur in P2.T7). Delete its `render`/`renderGpu` overrides.
  - verify: at the phase build, the default-selector goldens are bit-exact, the premult cases are removed, and the GPU test is within its bound.
  - size: L

- [ ] M137.P6.T2 — EdgeDetect
  - files: `Engine/Nodes/Filter/EdgeDetect.{h,cpp}`
  - approach: as P6.T1, on the CPU only, since its GPU port is M110 - Remaining Native GPU Ports. The op's `gpu()` returns invalid, which keeps it on the CPU.
  - verify: at the phase build, the default-selector goldens are bit-exact and `NativeEdgeDetect_Test` passes.
  - size: M

- [ ] M137.P6.T3 — Delete `SpatialFilterSupport`
  - files: `Engine/Nodes/Filter/SpatialFilterSupport.{h,cpp}`, `Engine/CMakeLists.txt`
  - approach: delete the files once P2.T7, P6.T1 and P6.T2 have moved every caller onto the Passes executor. Move any helper still needed (`channelIndexForBit`, `processedBitsForImage`, `toPixelEnclosing`) into `StagePlan` or `PassExecutor`.
  - verify: at the phase build, a grep for `SpatialFilterSupport` finds nothing, and the filter tests pass.
  - size: M

## Phase 137.7: Resample kind and the Transform family

- [ ] M137.P7.T1 — Resample kind on CPU and GPU
  - files: `Engine/Nodes/Image/Op.h`, `Engine/Nodes/Image/ResampleExecutor.{h,cpp}` (new), `Engine/Nodes/Image/Resampler.h`, `Engine/Gpu/GpuResample.cpp`
  - approach:
    - A Resample op supplies `coords(params, x, y, J)` and an optional 3×3 for concatenation (`getTransform`), plus a write-coordinates flag for STMap-style outputs.
    - The CPU executor wraps `Resampler::buildSamplingTransforms`, `resampleRow`, `getRegionOfDefinition` and `getRegionOfInterest` (`Resampler.h:445-595`). The GPU executor wraps `resample.slang` through `GpuResample.cpp`.
    - MaskMix is applied by `StagePlan`, as for the other kinds.
  - verify: covered by P7.T2.
  - size: L

- [ ] M137.P7.T2 — Transform and TransformMasked
  - files: `Engine/Nodes/Transform/Transform.{h,cpp}`, `Tests/Native/NativeTransformGpu_Test.cpp`
  - approach: `ImageNode<TransformOp>` as Resample. TransformMasked is the MaskMix spec variant. Delete `render` (1077), `isIdentity` (1032) and the GPU overrides (1339-1371). Concatenation keeps working through the op's 3×3.
  - verify: at the phase build, the goldens are bit-exact, `NativeTransformGpu_Test` is within its bound, and `NativeTransform_Test` passes, including its concatenation cases.
  - size: L

- [ ] M137.P7.T3 — Reformat
  - files: `Engine/Nodes/Transform/Reformat.{h,cpp}`
  - approach: Resample op, CPU only (its GPU port is M110). `isIdentityOp` (646) becomes `Op::isIdentity`.
  - verify: at the phase build, the goldens are bit-exact and `NativeReformat_Test` passes.
  - size: M

- [ ] M137.P7.T4 — Position and Crop
  - files: `Engine/Nodes/Transform/{Position,Crop}.{h,cpp}`
  - approach:
    - Position becomes a Resample op with an integer offset, on the CPU only.
    - Crop becomes a Point op with a RoD override, using the `ExtentKnobs` it already has and softness in `row()`.
    - Delete their `render` overrides.
  - verify: at the phase build, the goldens are bit-exact, and `NativePosition_Test` and `NativeCrop_Test` pass.
  - size: M

## Phase 137.8: Composite kind, keyers and Merge

- [ ] M137.P8.T1 — Composite kind: n sources per row
  - files: `Engine/Nodes/Image/Op.h`, `Engine/Nodes/Image/StagePlan.{h,cpp}`, `Engine/Nodes/Image/PixelKernel.h`, `Tests/Native/StagedComposite_Test.cpp` (new)
  - approach:
    - The `StagePlan` fills `RowIO::src[i]` for every declared source, each with its own layer policy and a per-source "pixel present" span for rows outside that input's RoD. Sources are (input, time, weight).
    - Past `kPixelKernelMaxInputs` (`PixelKernel.h:41`), the op folds the remaining sources itself.
    - On the GPU, the plan binds n source views, and composite bodies are `IBinaryPixelOp` entries.
  - verify: at the phase build, `StagedComposite_Test` renders a two-source test op with an offset second input, a five-source fold, and mask with mix.
  - size: L

- [ ] M137.P8.T2 — Dissolve
  - files: `Engine/Nodes/Merge/Dissolve.{h,cpp}`
  - approach:
    - Composite op over 64 Variadic sources with `Stages{MaskMix}`. Drop its own `maskInvert` (`:262-269`) in favour of the stage.
    - At an integer `which`, `route()` answers that input.
    - `updateRange` stays in the node.
    - CPU only (its GPU port is M110).
  - verify: at the phase build, the goldens are bit-exact and `NativeDissolve_Test` passes.
  - size: M

- [ ] M137.P8.T3 — Keyer, with `keyer_main` as an `IPixelOp`
  - files: `Engine/Nodes/Keyer/Keyer.{h,cpp}`, `Engine/Gpu/Kernels/keyer.slang`, `Engine/Gpu/CMakeLists.txt`, `Tests/Native/NativeKeyerGpu_Test.cpp`
  - approach:
    - Point or Composite op, according to its inputs today. Move `keyer_main`'s body into an `IPixelOp` and delete the standalone entry.
    - Delete `render`, `isIdentity` and the GPU overrides.
    - Behaviour stays unchanged; M95 - Knob Cleanup And Keyer Redesign redesigns it later.
  - verify: at the phase build, the goldens are bit-exact and `NativeKeyerGpu_Test` is within its bound.
  - size: L

- [ ] M137.P8.T4 — ChromaKeyer, with `chromakeyer_main` as an `IPixelOp`
  - files: `Engine/Nodes/Keyer/ChromaKeyer.{h,cpp}`, `Engine/Gpu/Kernels/chromakeyer.slang`, `Engine/Gpu/CMakeLists.txt`, `Tests/Native/NativeChromaKeyerGpu_Test.cpp`
  - approach: as P8.T3. The custom `getGpuInputLayer` (`:1147`) becomes the input's layer policy in the spec.
  - verify: as P8.T3.
  - size: L

- [ ] M137.P8.T5 — Merge as a Custom node
  - files: `Engine/Nodes/Merge/Merge.{h,cpp}`
  - approach:
    - Re-base `MergeNode` onto `StagedImageEffect` with `OpKind::Custom` and a `NodeSpec` per preset (`MergePreset<P>`, `Merge.h:191`). It keeps its own `render()` (931) and `isIdentity` (861).
    - The mask/mix knobs come from the MaskMix stage, so `addMaskMixKnobs` (`:692`) goes.
    - M101 - Clean-Sheet Merge Node replaces the node later.
  - verify: at the phase build, `NativeMerge_Test` and `MergeOperators_Test` pass.
  - size: M

## Phase 137.9: Delete the old base and document

- [ ] M137.P9.T1 — Delete `NativeImageEffect`, `NativeImageTraits` and the description virtual
  - files: `Engine/Nodes/Image/NativeImageEffect.{h,cpp}`, `Engine/Nodes/NativeEffectBase.{h,cpp}`, `Engine/CMakeLists.txt`, `Engine/GpuPlacement.cpp`
  - approach:
    - Delete the files.
    - Remove `getNativePluginDescription()` and `NativePluginDescription`/`NativeInputDescription` (`NativeEffectBase.h:55-79,492`), leaving `NodeSpec` the only description.
    - Remove P2.T3's `NativeImageEffect` branch of `getGpuStageSummary`.
  - verify: at the phase build, a grep for `NativeImageEffect`, `NativeImageTraits` and `getNativePluginDescription` in `Engine/` finds nothing.
  - size: M

- [ ] M137.P9.T2 — Move the framework tests off the old base
  - files: `Tests/Native/NativeImageEffect_Test.cpp`, `Tests/Native/NativeImageEffectGpu_Test.cpp`, `Tests/Native/NativeRuntimeShaderGpu_Test.cpp`, `Tests/Native/NativeGpuCapability_Test.cpp`, `Tests/GpuResidency_Test.cpp`
  - approach:
    - Rewrite the test effects as spec'd `ImageNode<Op>`s. Rename the first two files to `StagedImageEffect*_Test.cpp`.
    - Keep each case's intent, and say in the report where an assertion changed.
  - verify: at the phase build, these tests pass.
  - size: M

- [ ] M137.P9.T3 — Move the remaining test includes
  - files: `Tests/Native/NativeGpuParity.h` and the per-node `Native*Gpu_Test.cpp` files that still include `NativeImageEffect.h` or `NativeGenerator.h`
  - approach: swap the includes to the node headers or `StagedImageEffect.h`. Make no assertion changes.
  - verify: at the phase build, a grep for `NativeImageEffect` in `Tests/` finds nothing, and the full ctest passes.
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

- [ ] M137.P10.T1 — After-timings
  - files: `tools/bench/BASELINE.md`
  - approach: on the same host and release build type as P1.T2, rerun the same cases, and record the after-table next to the before-table with the delta and spread.
  - verify: every case is within the before-run's spread, or within 3%, whichever is larger. A miss blocks the gate and goes to the user.
  - size: S

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
- 2026-10-19 — Elaborated with a spike and a baseline phase ahead of the approval stop (planner): golden renders and timings must be captured on unchanged code, so they come before any framework commit. During migration the engine reaches GPU virtuals through `NativeEffectBase`, so both bases coexist until Phase 137.9.

**Verification gate:**
- The full build and ctest pass, on both scheduler modes, including `NativeGolden_Test` bit-exact for every migrated node, every `Native*Gpu_Test` within its README bound on the remote GPU build, `NodeSpec_Test`, `NativeSwitch_Test`, `StageKnobFooter_Test` and `StagedComposite_Test`.
- Greps for `NativeImageEffect`, `NativeImageTraits`, `getNativePluginDescription`, `SpatialFilterSupport`, `hostUnPremult`, `grade_main`, `keyer_main` and `chromakeyer_main` find nothing in `Engine/` or `Tests/`.
- The `getPluginIDs()` dump equals the pre-milestone one, plus `Switch`.
- P10.T1's timings show no regression beyond noise.
- The user approved the design note (P3.T2) and the panel screenshots (P10.T2).
- CI is green.
