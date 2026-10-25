# M101 - Clean-Sheet Merge Node

A new native Merge, `Merge`. It matches Nuke's Merge feature for feature, minus a few
pieces of legacy cruft, and it can merge several layers at once. Each side's alpha can be one
universal channel, each layer's own alpha, or a per-channel alpha layer. It is a Composite op on
M137 - Native Base Class Flexibility's framework, which this milestone first extends so a folded
input can be read in a selected layer plus an alpha plane, on the CPU and on the GPU. It ships with
a CPU path and a GPU op, so merges join resident regions instead of breaking them. The operator
presets become Python items in the node menu from M158 - Node Toolbar As Menu.
The current `MergeNode` is deleted with **no compatibility**. It was a faithful port of
openfx-misc Merge 2.0, built in M67 - Native Core Nodes. Old projects that use it load the
missing-plugin node (user, 2026-10-12).

Scouted facts (2026-10-22, `main` `1eed87752`; re-scouted at the grill, so the earlier `NativeImageEffect` facts are gone):
- **Framework (M137 - Native Base Class Flexibility):**
  - The bases are `NativeEffectBase` (`Engine/Nodes/NativeEffectBase.h`) and `StagedImageEffect`/`ImageNode<OP>` (`Engine/Nodes/Image/StagedImageEffect.h`).
  - The specs are `NodeSpec`/`InputSpec` (`Engine/Nodes/NodeSpec.h:96,115`, with roles `Source, Aux, Mask, Variadic`, and `InputLayer` `plane()/select(knob)/pairWith(n)`) and `ImagePipelineSpec{stages, layers, op}` (`Engine/Nodes/Image/ImagePipelineSpec.h:141`).
  - A Composite op (`Engine/Nodes/Image/Op.h:246-300`) has `bake`, `isIdentity`, `inputs(params, ctx) → CompositeInputs{base, further, blendInput}` and `step(params, RowIO)`. `step` folds `io.src[0]` (the result so far) with `io.src[1]` (the next input). Mask and mix apply once, to the result, and there is no Premult stage.
  - **`CompositeInput` is just `{inputNb}`.** Each input is read in exactly one layer, its `InputSpec::layer`.
  - **The GPU fold accepts only inputs read in the plane being rendered.** A `Select` layer sends the render to the CPU, and a folded input can't be the mask input (`StagePlan.h:236-241`, `docs/decisions/2026-10-19-native-node-framework.md`).
  - Executors: `runStagesCpu` (`StagePlan.h:186`), and `recordStagesGpu`/`recordStagesGpuFinish` (`:306,325`). On the GPU, a Composite op needs a registry `Kernel` op built with `runCompositeStep` (`Engine/Gpu/Kernels/compositestep.slang`), dispatched once per further input with ping-pong scratch.
  - The region engine is in `Engine/GpuRegionTask.{h,cpp}` and `Engine/GpuPlacement.cpp`.
  - Copy from `DissolveOp` (`Engine/Nodes/Merge/Dissolve.h`, Composite, Variadic, no GPU) and `Tests/Native/StagedComposite_Test.cpp`.
- **Today's Merge** is `MergeNode` (`Engine/Nodes/Merge/Merge.{h,cpp}`, `MergeOperators.{h,cpp}`):
  - An `OpKind::Custom` node with `renderOp`/`isOpIdentity`, inputs B=0, A=1, Mask=2, then Variadic A2..A64.
  - It registers 10 IDs, `net.sf.openfx.MergePlugin` and `net.sf.openfx.Merge<Plus|Matte|Multiply|In|Out|Screen|Max|Min|Difference>`, one `.reg.inc` each, with docs under `Merge/docs/` and `Merge.costseed.inc`.
- **Port kit (M120 - Native Node Port Kit):**
  - Per-node `.reg.inc`, `.costseed.inc`, `docs/<Node>.md` with a `Tolerance:` line, `Tests/Native/Native<Node>_Test.cpp` with `expectNativeIdentity`, a `<kernel>.cmake` fragment and an icon. The checklist is in `Engine/Nodes/README.md:373-413`.
  - Retiring an ID is an `Engine/OfxRetired/<ofx-id>` marker. Only an ID some loaded OFX bundle still provides may have one (`OfxRetired_Test`). An unknown ID in a project loads `MissingPluginNode`.
- **GPU:**
  - Kernels are `Engine/Gpu/Kernels/*.slang`, built offline by `slang_add_kernel` (`cmake/SlangKernels.cmake`). `import` works by same-directory lookup (`compositestep` → `pixelwrap` → `gradeops`).
  - `slang_add_kernel(... CPP)` builds a kernel as C++ for oracle tests (`Tests/Gpu/CMakeLists.txt`).
  - Push constants are at most 128 B, and `GpuWrapParams` uses 112. Op parameters go in a buffer of up to 64 KB (`GpuOpRef`).
  - The wrap uses bindings 0–3, and `compositestep` adds 4.
- **Layer knobs:** `Node::declareLayerKnob(knob, inputNb, eRoleInputBound|eRoleTarget)` (`Engine/Node.h:1754`). `KnobLayerSelect` selects one layer, and `KnobChannelSet` selects several (used by `AddLayers.cpp:74-101`). Planes still come from `getComponentsNeededAndProduced`.
- **Users of the old node:**
  - Roto sets only `operation` and `maskInvert`: `RotoContext.{cpp,h}`, `RotoContextPrivate.h`, `RotoDrawableItem.{cpp,h}`, `RotoPaint.cpp`, `RotoStrokeItem.cpp`.
  - PyPlugs: `DropShadow`, `EdgeBlur`, `Glow`, `LightWrap`, `PIKColor`, `ZMask`.
  - Engine and GUI hooks:
    - `AppInstance.cpp:1043-1062` (inspector list)
    - `GuiApplicationManager.cpp:1102` (M shortcut)
    - `NodeGui.cpp:449,3533` (operator sub-label icon)
    - `Node.cpp`, `ParallelRenderArgs.cpp`, `EffectInstance.h:52` (`PLUGINID_OFX_MERGE`), `NodeDocumentation.cpp:334-370`
  - Tools: `tools/genStaticDocs.sh`, `tools/release/check-startup.sh`, `tools/bench/*`.
  - Golden fixtures: `Tests/fixtures/golden-ntp/{cg_multipass,keying_grade,defocus_retime}.ntp`, plus the copies under `tools/bench/comps/`.
  - Tests: `NativeMerge_Test`, `MergeOperators_Test`, `PyPlugNativeParams_Test`, `NativeGolden_Test`, `NativeMetadataPolicy_Test`, `GpuPlacement_Test`, `GpuResidency_Test`, `FrameGraphBuild_Test`, `GraphScaling_Test` and `Tests/gui/m69_cleanup.py`. Others have one hit each; run the grep.
- **Layers:** a layer is an `ImageLayerDesc` with 1–4 channels. No layer has its own alpha: `beauty.A` is just a channel named A. Missing channels read zero.

## Design

- **Identity:**
  - ID `Merge` (a plain ID, as for every native node; `DECISIONS/2026-10-12-native-nodes-drop-ofx-legacy.md`), version 1, label "Merge", category Merge.
  - `OpKind::Composite` with `Stages{MaskMix, PassThroughUnprocessed}`.
  - Inputs are **B = 0, A = 1, Mask = 2, then A2..A64** (Variadic), Nuke's order and the old node's. `inputs()` returns base B, then A, A2… in order, so A goes over B, then A2 over the result, and so on.
  - In the inspector list, and bound to the M shortcut.
- **Presets:** Plus, Matte, Multiply, In, Out, Screen, Max, Min and Difference are **Python items in the node menu** (M158 - Node Toolbar As Menu). Each one creates `Merge` and sets `operation`, and the tab menu lists them because they are menu items. They are not plug-ins.
- **Operators:**
  - All of Nuke's operators, **with Nuke's option names** (`color-dodge`, `conjoint-over`, `hard-light`…): atop, average, color-burn, color-dodge, conjoint-over, copy, difference, disjoint-over, divide, exclusion, from, geometric, hard-light, hypot, in, mask, matte, max, min, minus, multiply, out, over, overlay, plus, screen, soft-light, stencil, under and xor.
  - Plus the openfx-misc extras under their current names: hue, saturation, color, luminosity, freeze, grain-extract, grain-merge, pinlight and reflect.
  - **Maths are Nuke's formulas, made HDR-safe** (user). Where a Nuke formula clamps, flips sign, divides by zero or jumps for values above 1 or below 0, the new node uses a continuous unbounded variant. Every such deviation is listed in the Slang module's header and the node doc, and recorded under `## Decisions`. There is no legacy toggle.
- **Operator source, Grade's pattern:**
  - `Engine/Gpu/Kernels/mergeops.slang` is canonical.
  - The C++ CPU operators (`Engine/Nodes/Merge/MergeOps.{h,cpp}`) are written alongside it.
  - An oracle test compares every operator with the `CPP`-compiled Slang.
- **What is merged, the `layers` knob:**
  - **Single:** A layer → B layer → output layer. A and B are input-bound `KnobLayerSelect`s, and the output is a Target that may create a layer and defaults to the B layer.
  - **All matching:** every layer present on A and on B, paired by ID.
  - **List:** a `KnobChannelSet` input-bound to B, Nuke's "also merge".
  - In every mode, all other B layers pass through, and a layer missing on a side reads as zero.
- **Alpha source, per side (A and B separately):**
  - **Universal:** one channel, default `rgba.A`, is that side's alpha for every merged layer. `none` means alpha 0.
  - **Per-layer:** each layer's own channel named `A` or `alpha` (case-insensitive) is its alpha. A layer without one falls back to the universal channel.
  - **Per-channel:** an alpha **layer** whose channel *i* is the alpha of merged channel *i*, e.g. `alphaRGB` for `beauty.RGB`. A channel-count mismatch is a render error naming both layers.
  - **Output alpha:**
    - With Universal, the universal alpha channel is merged with the alpha rule when it is in the output.
    - With Per-layer, each layer's alpha channel is merged as part of that layer.
    - With Per-channel, the alpha layer itself is merged per channel and written to B's alpha layer.
- **Other knobs:**
  - set bbox to (union, intersection, A, B), where "A" is the union of every connected A input
  - metadata from (A, B), where "A" is input 1
  - frame range from (A, B)
  - mix
  - mask through the `MaskMix` stage's mask channel, invert and mix
- **Dropped on purpose (user):** Nuke's mask fringe and inject, video colorspace, and **alpha masking** (`screenAlpha`). Blend operators process alpha like any other channel, and users mask or premult upstream.
- **Region awareness:**
  - `getRegionOfDefinition` follows the bbox mode.
  - On CPU and GPU, rows or strips outside every A input's RoD copy B, with no operator call and no dispatch.
  - `isIdentity` returns B when no A is connected and the operator is identity for a transparent A.
- **GPU:**
  - `mergeops.slang` provides an `IBinaryPixelOp { float4 apply(float4 a, float aa, float4 b, float ba, uint2 xy); }` body, wrapped by `runCompositeStep`, with the operator chosen by a parameter.
  - One dispatch per further A input, the framework's fold. The result stays resident.
  - `canRenderOnGpu` accepts every operator. Non-separable operators need 3 or more channels and are refused on GPU otherwise.
  - Hand-off to M87 - Point-Op Kernel Fusion: `IBinaryPixelOp` is the fusion hook for merges.

## Scope

**In:**
- The Composite framework extension: per-input alpha sources and `Select`-layer inputs, on CPU and GPU, region engine included.
- The new `Merge` node with every Design feature, its CPU path and GPU op, presets as node-menu Python items, docs and a cost seed.
- Every caller moved to the new node, and the old node and its 10 IDs deleted.
- The golden fixtures and bench comps rebuilt and re-recorded.

**Out:**
- The node menu and Python menu items themselves: M158 - Node Toolbar As Menu.
- Folding several A inputs in one GPU dispatch: none, or M87 - Point-Op Kernel Fusion.
- KeyMix, Copy and other routing nodes: M121 - Native Routing And Mix.
- Any compatibility with old Merge IDs or knob names: none (no backward compat).
- Mask fringe, inject, video colorspace and alpha masking: none (dropped).

**Edge cases:**
- No A connected: identity to B, through `isIdentity`.
- No B connected: B reads zero, as in Nuke.
- A layer missing on one side reads zero, and B's other layers pass through.
- A per-channel alpha layer whose channel count differs from the merged layer's: render error naming both.
- A per-layer alpha with no `A`/`alpha` channel falls back to the universal channel.
- A folded A input that is also the mask input: CPU fallback, the framework's existing rule.
- A non-separable operator on fewer than 3 channels is refused on the GPU and renders on the CPU.
- Bbox "A" with several A inputs is the union of their RoDs. "Metadata from A" reads input 1.
- More than 64 A inputs is impossible: Variadic `count` caps them.
- Golden diffs: expected only where the HDR-safe deviations apply. Re-record them, and investigate any other diff.

**Assumptions:**
- The region engine can hand one input over in more than one layer without changing its residency accounting beyond one buffer per layer.
- Roto's results are unchanged, since it uses only `operation` and `maskInvert` and never `screenAlpha`.

## Phase 101.1: Operator maths

- [x] M101.P1.T1 — Write the canonical Slang operator module
  - files: `Engine/Gpu/Kernels/mergeops.slang` (new), `Tests/Gpu/CMakeLists.txt` (a `CPP` oracle target), `Tests/Gpu/MergeOpsOracle_Test.cpp` (new)
  - approach:
    - Implement every operator in the Design list as pure functions of premultiplied `(A, a, B, b)` per channel, plus `float4` forms for the four non-separable ones (hue, saturation, color, luminosity).
    - Use Nuke's documented formulas and option names. Then make each HDR-safe where the formula clamps at 1, flips sign, divides by zero or jumps outside [0, 1]: for example, divide with B = 0 → 0, color-dodge/burn without the `min(1, …)` clamp, and overlay/soft-light/hard-light branches that stay continuous above 1.
    - Put a table of every deviation from Nuke in the module's header comment.
    - Build it with `slang_add_kernel(... CPP)` as Grade's `gradeops` is built for tests. Do not include or copy `MergeOperators.*`.
  - verify: `MergeOpsOracle_Test` checks golden values per operator, through the CPP-compiled module, at in-range, above-1, negative and zero-alpha inputs. The values are checked against Nuke's formula, or against the documented deviation where one applies. Runs at the milestone gate.
  - size: L

- [x] M101.P1.T2 — Write the C++ CPU operators and test them against the Slang oracle
  - files: `Engine/Nodes/Merge/MergeOps.{h,cpp}` (new), `Tests/Native/MergeOps_Test.cpp` (new), `Tests/Gpu/MergeOpsOracle_Test.cpp`
  - approach:
    - The same operators as row functions over `nComps`-interleaved floats, with a per-channel alpha span, stride 0 for a broadcast alpha and 1 for a per-channel layer.
    - Write them to vectorise. Use an unroll pragma where it helps, as Blur does.
    - The operator enum and option names come from one list both files use.
  - verify: `MergeOps_Test` reuses P1.T1's golden table, and the oracle test compares every operator's CPU row with the CPP-compiled Slang over a random HDR sweep, within the tolerance class for that operator (`Engine/Nodes/README.md` "Parity tolerances").
  - size: M

## Phase 101.7: Composite framework — alpha and layer sources

- [x] M101.P7.T1 — Add per-input layer and alpha sources to Composite ops on the CPU
  - files: `Engine/Nodes/Image/Op.h`, `Engine/Nodes/Image/StagePlan.{h,cpp}`, `Engine/Nodes/Image/PixelKernel.h`, `Tests/Native/StagedComposite_Test.cpp`
  - approach:
    - `CompositeInput`, and the base, gain an optional read layer, which overrides the `InputSpec::layer` resolution, and an optional alpha source: none, one channel of a layer, or an alpha layer read per channel.
    - `runStagesCpu` fetches the extra plane rows and hands `step()` an alpha span per source in `RowIO`, with a stride of 0 (broadcast) or 1 (per channel).
    - A missing layer reads zero, and a channel-count mismatch on a per-channel alpha layer is a render error naming both layers.
    - Existing Composite ops, Dissolve among them, set none of this and stay bit-identical.
  - verify: new `StagedComposite_Test` cases use a test op that folds A in a selected layer with a universal alpha from `rgba.A`, and with a per-channel alpha layer, both against hand-computed rows. They also cover the mismatch error. `NativeDissolve_Test` is unchanged.
  - size: L

- [x] M101.P7.T2 — Let the GPU region engine hand an input over in several layers
  - files: `Engine/GpuRegionTask.{h,cpp}`, `Engine/GpuPlacement.cpp`, `Engine/Nodes/Image/StagePlan.cpp` (`stageGpuInputLayer`), `Tests/GpuResidency_Test.cpp`
  - approach:
    - An external or resident input may be requested in more than one layer: the plane rendered, a selected layer, and an alpha layer. Each is uploaded or kept resident as its own buffer.
    - Residency and cost accounting count each layer.
    - The region-to-region case keeps an upstream node's result resident in every layer requested.
  - verify: a new `GpuResidency_Test` case runs a region whose consumer reads one upstream input in two layers. It checks one upload per layer per external input, and that nothing breaks the region. The existing placement and residency tests pass.
  - size: L

- [x] M101.P7.T3 — Fold Select-layer inputs and alpha sources on the GPU
  - files: `Engine/Gpu/Kernels/compositestep.slang`, `Engine/Nodes/Image/StagePlan.{h,cpp}` (`canRunStageSourcesGpu`, `recordStagesGpu`), `Tests/Native/StagedImageEffectGpu_Test.cpp`
  - approach:
    - `runCompositeStep` gains bindings for the result's alpha and the step input's alpha, plus a stride, in the op's parameter buffer or the 16 spare push-constant bytes.
    - `canRunStageSourcesGpu` accepts inputs whose read layer is a `Select` and inputs with alpha sources, using P7.T2's multi-layer hand-over.
    - The folded-input-is-mask rule is unchanged.
  - verify: `StagedImageEffectGpu_Test` runs P7.T1's test op on lavapipe in each mode, matching the CPU exactly. A `Select`-layer fold stays on the GPU (`canRenderOnGpu` is true).
  - size: L

## Phase 101.2: Node on the CPU

- [ ] ~~M101.P2.T1 — Feed multiple inputs to point-op kernels in NativeImageEffect~~ (cancelled 2026-10-19: M137 - Native Base Class Flexibility's Composite kind feeds every declared input to the op)
  - files: `Engine/Nodes/Image/NativeImageEffect.{h,cpp}`, `Engine/Nodes/Image/PixelKernel.h`, `Tests/Native/NativeImageEffect_Test.cpp`
  - approach:
    - Add a trait or virtual that lists the point-op source inputs (default `{0}`). `render()` then fetches each listed input's plane for the row, fills `src[i]`/`nSrc`, and passes a per-source "pixel present" span for rows outside an input's RoD, as `MergeOperators::RowSide::present` does today.
    - Raise `kPixelKernelMaxInputs` if needed. A nodes beyond the limit fold in a loop inside the kernel's caller, so do not raise it to 66.
    - The base class's unpremult, kernel, premult and mask × mix order is unchanged.
    - Existing single-input nodes must be bit-identical.
  - verify: a new `NativeImageEffect_Test` case renders a two-input test kernel with an offset second input. All existing native node tests still pass at the gate.
  - size: M

- [x] M101.P2.T2 — Add the new Merge node with a single layer and universal alpha
  - files: `Engine/Nodes/Merge/NativeMerge.{h,cpp}` (new), `Engine/Nodes/Merge/NativeMerge.reg.inc` (new), `Tests/Native/NativeMergeNode_Test.cpp` (new)
  - approach:
    - A Composite `MergeOp` on `ImageNode<OP>`, with `NodeSpec` ID `Merge` v1 and the Design's inputs (A2..A64 Variadic), `operation` (option names from `MergeOps.h`), bbox, metadata from, frame range from, and the `MaskMix` stage's mask and mix.
    - `layers = Single` only for now. A and B are input-bound `KnobLayerSelect`s, the output is a Target, and there is a universal alpha channel select per side. They feed P7.T1's read layer and alpha source per input.
    - `inputs()` returns base B and the connected A inputs in order. `step()` calls `MergeOps`.
    - Override `getRegionOfDefinition` and the metadata derivation per the Design. Identity comes through `Op::isIdentity`.
    - Rows outside every A's RoD copy B.
    - Name the class so it does not clash with `MergeNode` (deleted in P5) or the `Merge` namespace in `MergingEnum.h`. The test calls `expectNativeIdentity`.
  - verify: `NativeMergeNode_Test` covers over with an offset A, three A inputs, each bbox mode, mask and mix, identity with no A, a non-separable operator, A `spec` over B `rgba`, and output-layer creation. Runs at the gate.
  - size: L

- [x] M101.P2.T3 — Add the Merge presets as node-menu Python items
  - files: the bundled `initGui.py` (new, in the bundled PyPlugs directory `Gui/Resources/PyPlugs/`; check that `tools/release/stage-bundle.sh` and the install rules ship it to `Plugins/PyPlugs`), `Tests/gui/` (new test script)
  - approach:
    - Using M158 - Node Toolbar As Menu's `natron.nodesMenu()`, add `QAction`s under `Merge/` labelled Plus, Matte, Multiply, In, Out, Screen, Max, Min and Difference.
    - Each action runs a function that creates `Merge` and sets `operation`, so the tab menu's creation context places it at the cursor.
    - The node's sub-label shows the operator (P2.T4).
  - verify: a GUI test under Xvfb triggers each action and checks the node's plug-in ID and `operation`, and checks the tab menu lists `Merge/Plus`.
  - size: M

- [x] M101.P2.T4 — Wire the new Merge into the inspector list, the M shortcut and the operator sub-label
  - files: `Engine/AppInstance.cpp` (`isEntitledForInspector`, `:1043-1062`), `Gui/GuiApplicationManager.cpp` (`:1102`), `Gui/NodeGui.cpp` (`:449`, `:3533`), `Tests/Native/NativeMergeNode_Test.cpp`
  - approach: `Merge` becomes inspector-entitled in place of the old IDs, M creates `Merge`, and the sub-label icon reads the new node's `operation` choice. Drop the old IDs from these three places in P5.T6, not here.
  - verify: a test checks `Merge` is inspector-entitled. The GUI bench runs at the gate, because `Gui/NodeGui*` is touched.
  - size: M

## Phase 101.3: Layers and alpha modes

- [x] M101.P3.T1 — Add the `layers` modes: Single, All matching and List
  - files: `Engine/Nodes/Merge/NativeMerge.{h,cpp}`, `Tests/Native/NativeMergeNode_Test.cpp`
  - approach:
    - Add the `layers` choice.
    - All matching pairs the layers present on A and B by ID.
    - List uses a `KnobChannelSet` declared input-bound to B, as `AddLayers` does.
    - `getComponentsNeededAndProduced` asks each A input and B for the merged planes, produces those planes, and passes everything else through from B.
    - A layer missing on a side reads zero.
    - Follow the widen-on-write rules in `DESIGN/2026-09-26-rgba-rgb-alpha-layers.md` §3 for colour views.
  - verify: tests merge `rgba` + `beauty` + `spec` in All matching, a two-layer List, and a layer absent on A. All other layers on B are untouched.
  - size: M

- [x] M101.P3.T2 — Add the per-side alpha source modes: Universal, Per-layer and Per-channel
  - files: `Engine/Nodes/Merge/NativeMerge.{h,cpp}`, `Tests/Native/NativeMergeNode_Test.cpp`
  - approach:
    - Add a per-side `alpha source` choice, plus the universal channel select and the per-channel alpha-layer select (input-bound).
    - Each mode maps onto P7.T1's alpha source per input:
      - Universal is one channel, broadcast.
      - Per-layer is the layer's `A`/`alpha` channel, case-insensitive, falling back to the universal channel.
      - Per-channel is an alpha layer read per channel. The framework raises the mismatch error.
    - Output alpha follows the Design.
  - verify: tests check each mode on each side, including A per-layer over B universal, and per-channel with `alphaRGB` against a hand-computed result. The per-layer fallback and the mismatch error are covered.
  - size: L

## Phase 101.4: GPU path

- [x] M101.P4.T1 — Add the Merge GPU op and register it
  - files: `Engine/Gpu/Kernels/mergeops.slang`, `Engine/Gpu/Kernels/mergeops.cmake` (new), `Engine/Nodes/Merge/NativeMerge.{h,cpp}`
  - approach:
    - Add the `IBinaryPixelOp` body and a `runCompositeStep` entry point to `mergeops.slang`, registered in the GPU registry through its `.cmake` fragment, as `compositestep_halveadd` is.
    - The operator index and per-channel alpha stride go in the op's parameter buffer.
    - `MergeOp::gpu(params)` returns it.
    - `canRenderOnGpu` follows the Design, and P7.T3 lets every layer and alpha mode stay on the GPU.
    - Strips outside every A's RoD copy B, with no dispatch.
  - verify: covered by P4.T2's parity test.
  - size: L

- [x] M101.P4.T2 — Test CPU/GPU parity and region chaining for Merge
  - files: `Tests/Gpu/GpuMerge_Test.cpp` (new), `Tests/Gpu/CMakeLists.txt`
  - approach:
    - On lavapipe, compare the GPU and CPU output for every operator × alpha mode × layer mode (including A `spec` over B `rgba`), with two A inputs, mask and mix. Use exact tolerance for Porter-Duff operators and transcendental for blend modes, per the "Parity tolerances" table.
    - Placement test: Grade → Merge(A = Blur(B), B) → Grade is one resident region, with one upload per external input layer.
  - verify: the test runs in CI's lavapipe job and at the gate.
  - size: M

- [ ] M101.P4.T3 — Seed Merge's GPU and CPU cost rows
  - files: `Engine/Nodes/Merge/NativeMerge.costseed.inc` (new)
  - approach: measure a lone Merge's region `kernelMs` at UHD and its CPU row cost, and generate the rows with `tools/bench/render-profile-report.py ... --emit-cost-table` rather than writing them by hand (`Engine/Nodes/README.md` checklist).
  - verify: the seed rows load (the cost-model tests pass), and placement of the P4.T2 comp keeps Merge on the GPU.
  - size: M

## Phase 101.5: Replace the old Merge

- [x] M101.P5.T1 — Move RotoPaint's internal merges to the new node
  - files: `Engine/RotoContext.{cpp,h}`, `Engine/RotoContextPrivate.h`, `Engine/RotoDrawableItem.{cpp,h}`, `Engine/RotoPaint.cpp`, `Engine/RotoStrokeItem.cpp`
  - approach:
    - Create `Merge` instead of `PLUGINID_OFX_MERGE`.
    - `operation` and `maskInvert` are the only knobs Roto sets. Map the operator names onto Nuke's option names. Input indices are unchanged (B = 0, A = 1, Mask = 2).
    - The edits are mechanical ID and name swaps across these files.
  - verify: the existing Roto/RotoPaint tests, plus a manual paint-stroke check in the GUI, at the gate.
  - size: M

- [x] M101.P5.T2 — Move the bundled PyPlugs to the new node (Glow, LightWrap, DropShadow, EdgeBlur)
  - files: `Gui/Resources/PyPlugs/Glow.py`, `LightWrap.py`, `DropShadow.py`, `EdgeBlur.py`
  - approach: replace the old Merge IDs with `Merge` and set `operation` explicitly where a preset ID was used. Input indices are unchanged. Map any `AChannels`/`BChannels`/`OutputChannels`/`screenAlpha` settings to the new layer and alpha knobs. `screenAlpha` has no equivalent, so note any PyPlug whose output changes.
  - verify: `PyPlugNativeParams_Test` (updated in P5.T5) and rendering each PyPlug on a fixture at the gate.
  - size: M

- [x] M101.P5.T3 — Move the bundled PyPlugs to the new node (PIKColor, ZMask)
  - files: `Gui/Resources/PyPlugs/PIKColor.py`, `ZMask.py`
  - approach: as P5.T2. `Fill.py` no longer references Merge.
  - verify: as P5.T2.
  - size: M

- [x] M101.P5.T4 — Point the CPU tests that use Merge incidentally at the new node
  - files: `Tests/ColorViewsRender_Test.cpp`, `Tests/DefaultChannelSet_Test.cpp`, `Tests/LayerKnobs_Test.cpp`, `Tests/PoolSharing_Test.cpp`, `Tests/gui/m69_cleanup.py`
  - approach: swap the ID and the knob names. Keep each test's intent. Where a test asserted old-node behaviour that no longer exists (alpha masking, OFX defaults), rewrite the assertion for the new behaviour and say so in the report.
  - verify: these tests pass at the gate.
  - size: M

- [x] M101.P5.T5 — Point the metadata, catalogue and PyPlug tests at the new node
  - files: `Tests/NativeMetadataPolicy_Test.cpp`, `Tests/NativeMetadata_Test.cpp`, `Tests/NodeCategory_Test.cpp`, `Tests/OfxMetadataBridge_Test.cpp`, `Tests/Native/NativePluginList_Test.cpp` (and `Tests/Native/PyPlugNativeParams_Test.cpp`, `Tests/Native/BlurFIRGaussian_Test.cpp` if they reference Merge)
  - approach: as P5.T4. `NativePluginList_Test` must expect `Merge` and no `net.sf.openfx.Merge*` IDs.
  - verify: these tests pass at the gate.
  - size: M

- [x] M101.P5.T6 — Delete the old Merge
  - files: `Engine/Nodes/Merge/Merge.{h,cpp}`, `MergeOperators.{h,cpp}`, the 10 old `.reg.inc` fragments, `Merge.costseed.inc` and `docs/Merge*.md`, `Tests/Native/NativeMerge_Test.cpp`, `Tests/Native/MergeOperators_Test.cpp`, plus the references in `AppInstance.cpp`, `GuiApplicationManager.cpp`, `NodeGui.cpp`, `Node.cpp`, `ParallelRenderArgs.cpp`, `EffectInstance.h`, `NodeDocumentation.cpp`, `tools/genStaticDocs.sh`, `tools/release/check-startup.sh` and `tools/bench/*`
  - approach:
    - Delete the files and every reference: `git grep` for `PLUGINID_NATRON_MERGE`, `PLUGINID_OFX_MERGE`, `net.sf.openfx.Merge`, `MergeNode` and `MergeOperators` until nothing remains outside `docs/decisions/` history.
    - Rename `NativeMerge.*` to `Merge.*` and `NativeMergeNode_Test.cpp` to `NativeMerge_Test.cpp`, now that the names are free.
    - Add an `Engine/OfxRetired/` marker for an old ID only if a loaded OFX bundle still provides it. `OfxRetired_Test` decides; openfx-misc's Merge left the bundle in M67 - Native Core Nodes. Otherwise the IDs resolve to nothing, and old projects load `MissingPluginNode`.
  - verify: the grep is empty, and the full build and test suite pass at the gate.
  - size: M

- [x] M101.P5.T7 — Point the GPU and scheduler tests that use Merge at the new node
  - files: `Tests/GLScheduler_Test.cpp`, `Tests/GpuPlacement_Test.cpp`, `Tests/GpuResidency_Test.cpp`, `Tests/FrameGraphBuild_Test.cpp`, `Tests/GraphScaling_Test.cpp`
  - approach: as P5.T4. Where a placement test expected the old Custom Merge to break a region, expect the new one to join it, and say so.
  - verify: these tests pass at the gate.
  - size: M

- [ ] M101.P5.T8 — Rebuild the golden fixtures and bench comps with the new Merge and re-record them
  - files: `Tests/fixtures/golden-ntp/{cg_multipass,keying_grade,defocus_retime}.ntp`, their copies under `tools/bench/comps/`, the goldens `Tests/Native/NativeGolden_Test.cpp` reads, `Tests/Native/NativeGolden_Test.cpp`
  - approach:
    - Replace each old Merge with `Merge`, using the same operator and settings.
    - Re-record the goldens and the bench baselines these comps feed.
    - Check every diff lies where a documented HDR-safe deviation applies. Investigate any other diff as a bug.
  - verify: `NativeGolden_Test` passes on the re-recorded goldens. The report lists every changed golden and the deviation that explains it.
  - size: M

## Phase 101.6: Docs and UAT

- [ ] M101.P6.T1 — Document the new Merge
  - files: `Engine/Nodes/Merge/docs/NativeMerge.md` (new, renamed with the node in P5.T6), `Engine/Nodes/README.md` ("Parity tolerances" `:1019` and any Merge entries), the node's `NodeSpec` description
  - approach:
    - Describe the inputs, operators with formulas, layer modes, alpha sources, the node-menu presets, and what was dropped and why.
    - Include the HDR-safe deviation table and a `Tolerance:` line per the port-kit checklist.
    - Update the README's Merge tolerance rows.
  - verify: the docs build (if the docs pipeline from M6 - Documentation Pass has a check). Review the text against the Design.
  - size: M

- [ ] M101.P6.T2 — UAT
  - files: none (user acceptance)
  - approach: hand the user an AppImage plus a checklist:
    - over/screen/multiply on a CG multipass, in All matching and in List
    - A `spec` over B `rgba` in Single mode
    - per-layer alpha on beauty AOVs
    - per-channel alpha with an `alphaRGB` layer
    - three A inputs
    - mask and mix
    - each bbox mode
    - a GPU-resident comp, with the profile showing one region through the Merge
    - the presets from the toolbar and the tab menu
    - RotoPaint strokes
    - each bundled PyPlug
  - verify: the user signs off. Record the findings under `## Decisions`.
  - size: S

**Verification gate:** in one build, the full test suite passes in both scheduler modes:
- `MergeOpsOracle_Test`, `MergeOps_Test`, `NativeMerge_Test`, the new `StagedComposite_Test`/`StagedImageEffectGpu_Test`/`GpuResidency_Test` cases and `GpuMerge_Test` (lavapipe) pass.
- `NativeGolden_Test` passes on the re-recorded goldens.
- `git grep` finds no reference to the old Merge IDs or classes.
- RotoPaint and every bundled PyPlug render.
- The GUI bench runs (`tools/bench/gui/run.sh --xvfb`, `compare.py` against `baselines/xvfb.jsonl`), and its result is recorded here.
- CI is green.
- The user signs off the UAT.

## Decisions

- 2026-10-25 — **Merge metadata comes from B by default (user):** B, the background/main input, is the default metadata source, not A. This replaces M74's documented "metadata from A" policy for Merge; the policy doc and its test change to B. Range From stays B.
- 2026-10-25 — **Per-plane limits to check at the P3 gate (bosley-native-nodes):** (1) a per-channel B alpha with A on Universal/PerLayer reads A's same-named layer in the alpha plane, not A's broadcast alpha; (2) layers pair by ID but the input request matches channels, so a same-ID layer with different channels on A reads black; (3) every produced plane is requested from every non-mask input on any render of the node, which may fetch planes the rendered plane doesn't need — measure, and narrow to the rendered plane if the host requests them eagerly; (4) the extra layers follow the layer knob's channel mask. All-matching takes A as the union of the connected A inputs; an empty List merges only the output layer.
- 2026-10-25 — **Layer modes stay per-plane (bosley-native-nodes):** Merge stays non-multiplanar. A "produced planes" hook consulted by `getComponentsNeededDefault` declares the planes All-matching and List modes produce, and each plane renders separately with A and B read in that plane; Per-channel output alpha is one of those planes (B's alpha layer). The GPU path keeps one plane per query, so P4 holds. Rejected: making Merge multiplanar, which breaks the one-layer GPU path. Non-multiplanar Staged nodes now declare their input layers through `EffectInstance::addInputLayersNeeded`.
- 2026-10-25 — **Phase 101.2 calls (bosley-native-nodes):** identity goes through `route()` (an op can't see which inputs are connected): with no A, operators that keep B under a transparent A pass B through. "Rows outside every A's RoD copy B" is a per-chunk check that A and its alpha are zero, for those operators only. Metadata-from and range-from default to B; editing B's layer moves the output layer to match. Presets live in `Gui/Resources/PyPlugs/initGui.py`. P3.T2 must still turn non-separable operators with a per-channel alpha layer into a render error (2026-10-25 decision); 101.2 uses the first channel's alpha meanwhile.
- 2026-10-25 — **Install rule (bosley-native-nodes):** `cmake/NatronBundleAssets.cmake` installs PyPlugs to `Plugins/` instead of `Plugins/PyPlugs`, so an install-tree Natron misses `initGui.py`; `stage-bundle.sh` (AppImages) is right. P5 fixes the DESTINATION. Until P5 the tab menu lists both Merge nodes and both Plus presets.
- 2026-10-25 — **`pinlight`, not `pin-light` (bosley-native-nodes):** the openfx-misc extras keep their current names, and openfx-misc calls it `pinlight`.
- 2026-10-25 — **Plain AB and A/B (bosley-native-nodes):** multiply and divide drop Nuke's two-negatives jump; two negatives give a positive, the continuous HDR-safe variant. Geometric is 0 when A+B is 0; hard-light and overlay use the plain multiply and the new screen; soft-light is Nuke's formula. All listed in `mergeops.slang`'s deviation table; Phase 101.6 mirrors it into the node doc.
- 2026-10-25 — **Non-separable operators with a per-channel alpha layer (bosley-native-nodes):** hue, saturation, color and luminosity with a per-channel alpha layer are a render error naming the operator, rather than picking one channel's alpha (P3.T2).
- 2026-10-25 — **Freshness (bosley-native-nodes):** the GPU test binary doesn't link the Engine, so `MergeOps.cpp` (Macros.h only) compiles into it; the oracle entry is its own `mergeops_oracle.slang` with no registry fragment. P4.T1's GPU entry goes in `merge.slang` + `merge.cmake` importing `mergeops`. The README "Parity tolerances" section is at ~:1221. The formulas were checked against openfx-misc and Foundry search snippets; Foundry's pages couldn't be fetched from the sandbox.
- 2026-10-22 — **Extend the Composite framework for alpha and layer sources** (user, M101 grill): `CompositeInput` reads only one layer per input, so cross-layer Universal and Per-channel alpha had no path. A per-input read layer and alpha source, on CPU and GPU, is the principled fix, and other nodes can reuse it. It is Phase 101.7, inside this milestone (user).
- 2026-10-22 — **The GPU fold also reads `Select`-layer inputs** (user): Single mode with A's layer ≠ B's would otherwise fall back to CPU and break the region. It uses the same multi-layer hand-over as alpha.
- 2026-10-22 — **One GPU dispatch per further A input** (user): the framework's fold. Comps rarely have more than 2–3 A inputs. Batching is left to M87 - Point-Op Kernel Fusion.
- 2026-10-22 — **Presets are Python items in the node menu, from M158 - Node Toolbar As Menu** (user): no preset plug-ins and no new preset API. The tab menu lists them because everything in the node menu is a menu item. M158 is a new dependency.
- 2026-10-22 — **Operator source follows Grade's pattern** (user): canonical `mergeops.slang`, C++ CPU operators alongside it, and an oracle test against the CPP-compiled Slang. No shared C++/Slang header exists, and the shim would be new, unproven ground. This replaces `MergeMath.h`.
- 2026-10-22 — **Golden fixtures and bench comps are rebuilt with the new Merge and re-recorded** (user): diffs are reviewed only against the documented HDR-safe deviations. No backward compatibility.
- 2026-10-22 — **Operator option names are Nuke's exactly** (user): `color-dodge`, `conjoint-over`… so ported scripts and Roto's by-name settings match. The openfx-misc extras keep their current names.
- 2026-10-19 — **Built on M137 - Native Base Class Flexibility's framework** (user, M137 grill): M137 does not migrate today's Merge, since this milestone rewrites it, and leaves it as a Custom node. P2.T1 is cancelled because the Composite kind supersedes it. This milestone gains a dep on M137.
- 2026-10-12 — **No compatibility with the old Merge** (user): old Merge IDs are deleted outright. Old projects show the missing-plugin node (M22 - Missing Plugin Placeholder).
- 2026-10-12 — **Maths are Nuke's formulas, made HDR-safe, with no legacy toggle** (user).
- 2026-10-12 — **Dropped** (user): Nuke's mask fringe and inject, video colorspace, and alpha masking (`screenAlpha`). Masking is just the base class's mask channel, invert and mix.
- 2026-10-12 — **Per-channel alpha comes from an alpha layer** whose channel *i* is the alpha of merged channel *i* (user). This fits the 4-channel layer cap.
- 2026-10-12 — **Placed after M84 - GPU Placement And Residency and before M85 - Native GPU Kernels** (user). M85 no longer ports Merge.
- 2026-10-12 — **Plain ID `Merge`, not a reverse-domain ID** (user): native nodes drop reverse-domain IDs. ~~The operation presets stay as menu entries; Merge is the one exception to "no duplicate menu entries".~~ Superseded 2026-10-22: the presets are node-menu Python items.

## Hand-off from M85 - Native GPU Kernels

- ~~`slang_add_kernel` has no include path, so `merge.slang` cannot `import pixelwrap` as things stand.~~ Resolved: `import` works by same-directory lookup (`compositestep` → `pixelwrap` → `gradeops`).
- Op parameters go in a storage buffer and wrap geometry in push constants (`GpuWrapParams`, 112 B of the 128 B limit). A per-channel alpha binding table belongs in the buffer.
- Masks may come from any layer.
- Seed a GPU cost row by measuring a lone node's region `kernelMs` at UHD (`render-profile-report.py --emit-cost-table`), as P4.T3 does. A node without a row only gets the class default.
