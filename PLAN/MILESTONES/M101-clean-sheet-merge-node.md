# M101 - Clean-Sheet Merge Node

A new native Merge, `Merge`. It matches Nuke's Merge feature for feature, minus a few
pieces of legacy cruft, and it can merge several layers at once. Each side's alpha can be one
universal channel, each layer's own alpha, or a per-channel alpha layer. It ships with a CPU
row kernel and a native multi-input GPU kernel, using the placement and residency interface from
M84 - GPU Placement And Residency, so merges join resident regions instead of breaking them.
The current `MergeNode` is deleted with **no compatibility**. It was a faithful port of
openfx-misc Merge 2.0, built in M67 - Native Core Nodes. Old projects that use it load the
missing-plugin node (user, 2026-10-12).

**Framework (2026-10-19):** this milestone builds on M137 - Native Base Class Flexibility. Merge is a Composite op with `Stages{MaskMix}` declared through `NodeSpec`/`ImagePipelineSpec`. The `StagePlan` feeds every A input and B to the op, and the CPU and GPU executors apply mask and mix. M137 deletes `NativeImageEffect` and leaves today's `MergeNode` as a Custom-kind node with its own `render()`, which P5.T6 then deletes. Facts below that cite `NativeImageEffect` describe the code before M137; re-scout them at promotion.

Scouted facts (2026-10-12, `main` with M84 at its gate on `origin/milestone/m84-gpu-placement-and-residency` `8bf4a189a`):
- **Today's Merge** is `Engine/Nodes/Merge/Merge.{h,cpp}` (`MergeNode`) plus `MergeOperators.{h,cpp}`.
  - It keeps the OFX IDs `net.sf.openfx.MergePlugin` and the nine `net.sf.openfx.Merge<Preset>` presets, each registered as a separate plug-in.
  - Inputs are B=0, A=1, Mask=2, then A2..A64.
  - Channels are r/g/b/a bools for A, B and Output (`Merge.cpp:679-691`).
  - The `screenAlpha` "Alpha masking" knob is at `:667`.
  - Each plane in `args.outputLayers` is merged with the same plane on both sides (`:973-1018`). There is no separate A, B or output layer selection, and alpha always comes from the plane's own channel 3.
  - It has its own `render()` and mask/mix loop (`:1167-1187`), because `NativeImageEffect::render` fills only `src[0]` (`NativeImageEffect.cpp:466-469, 543-544`) even though `RowIO` holds `kPixelKernelMaxInputs = 4` sources (`PixelKernel.h:37,89`).
  - Several operators branch on or clamp against `maxValue = 1`: colour-dodge/burn, reflect, freeze, overlay and pin-light (`MergeOperators.cpp:150-200`).
- **Users of the old node:**
  - **RotoPaint internals** create Merges by `PLUGINID_OFX_MERGE` and set `operation` by name: `RotoContext.cpp:71,4674`, `RotoDrawableItem.cpp:65,261-268`, `RotoItem.cpp:57`, `RotoLayer.cpp:58`, `RotoStrokeItem.cpp:60`.
  - **Presets:** `AppInstance.cpp:1068`.
  - **Further references:** `AppManager.cpp`, `Node.cpp`, `ParallelRenderArgs.cpp`, `EffectInstance.h`, `NodeDocumentation.cpp` and `Engine/Nodes/README.md` (§148, §585, §615, the tolerance table at §658-676).
  - **PyPlugs:** `DropShadow`, `EdgeBlur`, `Fill`, `Glow`, `LightWrap`, `PIKColor` and `ZMask` under `Gui/Resources/PyPlugs/`.
  - **Tests:** `Tests/Native/NativeMerge_Test.cpp` and `MergeOperators_Test.cpp`. Merge is also used in `ColorViewsRender_Test`, `DefaultChannelSet_Test`, `GLScheduler_Test`, `LayerKnobs_Test`, `NativeMetadataPolicy_Test`, `NativeMetadata_Test`, `NodeCategory_Test`, `OfxMetadataBridge_Test`, `PoolSharing_Test`, `Native/NativePluginList_Test`, `Native/PyPlugNativeParams_Test` and `Native/BlurFIRGaussian_Test`.
- **Layers:**
  - A layer is an `ImageLayerDesc` with 1–4 channels.
  - **No layer has its own alpha.** `beauty.A` is just a channel named A. Only the colour plane has the `rgba`/`rgb`/`alpha`/`xy` views (`DESIGN/2026-09-26-rgba-rgb-alpha-layers.md`).
  - Missing channels read zero.
  - Layer knobs are declared with `Node::declareLayerKnob(knob, inputNb, role)` (`Node.h:1626`). Input-bound knobs list the input's layers, and Target knobs list the registry plus "New layer…". The registry doc names "Merge A/B selects" as input-bound and "Merge output" as a target (`DESIGN/2026-09-19-layer-registry.md` §3).
  - Planes come from `getComponentsNeededAndProduced` (Shuffle's is at `Shuffle.cpp:615-672`).
  - Mask and mix are in the base class (`NativeImageEffect.h:171-182`).
- **GPU interface (M84 - GPU Placement And Residency):**
  - `Engine/Nodes/Image/GpuRender.h` (not `Engine/Gpu/`) defines `GpuQuery`, `GpuImageView` and `GpuRenderArgs`; `GpuRenderArgs::inputs` is indexed by input number.
  - The virtuals on `NativeImageEffect` are `canRenderOnGpu`, `getGpuInputRoI` and `renderGpu`.
  - Kernels are Slang, compiled offline to SPIR-V and listed in `GpuKernelRegistry`. Storage buffers go in set 0 and push constants are at most 128 B.
  - M85 retired `pointops.slang`. Mask, mix, process channels and premult live in `Engine/Gpu/Kernels/pixelwrap.slang`, which reproduces `NativeImageEffect::render()`; `merge.slang` uses its helpers rather than its own blend.
  - Keyer and ChromaKeyer (M85) are multi-input GPU nodes: they override `renderGpu` and use `pixelwrap.slang`'s helpers, the pattern Merge follows. Regions already take several external inputs, each uploaded in its own layer.

## Design

- **Identity:**
  - ID `Merge` (a plain ID, as for every native node; `DECISIONS/2026-10-12-native-nodes-drop-ofx-legacy.md`), version 1, label "Merge", category Merge.
  - Inputs are **B = 0, A = 1, Mask = 2, then A2..A64**, Nuke's order and the old node's. A inputs fold in order like Nuke: A over B, then A2 over the result, and so on.
  - The presets (Plus, Matte, Multiply, In, Out, Screen, Max, Min, Difference) are **menu entries** that create `Merge` with `operation` set. They are not separate plug-ins.
- **Operators:**
  - All of Nuke's operators: atop, average, colour-burn, colour-dodge, conjoint-over, copy, difference, disjoint-over, divide, exclusion, from, geometric, hard-light, hypot, in, mask, matte, max, min, minus, multiply, out, over, overlay, plus, screen, soft-light, stencil, under and xor.
  - Plus the openfx-misc extras: hue, saturation, colour, luminosity, freeze, grain-extract, grain-merge, pin-light and reflect.
  - **Maths are Nuke's formulas, made HDR-safe** (user). Where a Nuke formula clamps, flips sign, divides by zero or jumps for values above 1 or below 0, the new node uses a continuous unbounded variant. Every such deviation is listed in the maths header and recorded under `## Decisions`. There is no legacy toggle.
- **Operator source, written once:**
  - The operators live in one header, `Engine/Nodes/Merge/MergeMath.h`, as scalar and `float4` functions in a C++/Slang common subset, behind a small compat shim.
  - The CPU row kernel and `merge.slang` both include it, so the two backends cannot drift.
  - If the shared subset becomes contorted, fall back to two sources plus a golden cross-backend test, and record that as a decision.
- **What is merged, the `layers` knob:**
  - **Single:** A layer → B layer → output layer. The output is a Target knob that may create a layer and defaults to the B layer.
  - **All matching:** every layer present on A and on B, paired by ID.
  - **List:** the chosen layers, Nuke's "also merge".
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
  - set bbox to (union, intersection, A, B)
  - metadata from (A, B)
  - frame range from (A, B)
  - mix
  - mask through the base class's mask channel, invert and mix
- **Dropped on purpose (user):** Nuke's mask fringe and inject, video colorspace, and **alpha masking** (`screenAlpha`). Blend operators process alpha like any other channel, and users mask or premult upstream.
- **Region awareness:**
  - `getRegionOfDefinition` follows the bbox mode.
  - On CPU and GPU, rows or strips outside every A input's RoD copy B, with no operator call and no dispatch.
  - `isIdentity` returns B when no A is connected and the operator is identity for a transparent A.
- **GPU:**
  - `merge.slang` defines `IBinaryPixelOp { float4 apply(float4 a, float aa, float4 b, float ba, uint2 xy); }` and one kernel that merges a layer list with in-kernel mask and mix.
  - `canRenderOnGpu` accepts every operator. Non-separable operators need 3 or more channels and are refused on GPU otherwise.
  - Multiple A inputs fold in one dispatch, up to the push-constant and binding limits, and in chained dispatches past that, keeping the result resident.
  - Hand-off to M87 - Point-Op Kernel Fusion: `IBinaryPixelOp` is the fusion hook for merges.

## Phase 101.1: Operator maths

- [ ] M101.P1.T1 — Write the single-source merge operator header
  - files: `Engine/Nodes/Merge/MergeMath.h` (new), `Engine/Nodes/Merge/MergeMathCompat.h` (new), `Tests/Native/MergeMath_Test.cpp` (new), `Tests/CMakeLists.txt` if the `Native/*_Test.cpp` glob does not pick it up
  - approach:
    - Implement every operator in the Design list as pure functions of premultiplied `(A, a, B, b)` per channel, plus `float4` forms for the four non-separable ones.
    - Use Nuke's documented formulas. Then make each HDR-safe where the formula clamps at 1, flips sign, divides by zero or jumps outside [0, 1]: for example, divide with B = 0 → 0, colour-dodge/burn without the `min(1, …)` clamp, and overlay/soft-light/hard-light branches that stay continuous above 1.
    - List every deviation from Nuke in a table in the header comment.
    - The compat shim maps `float4`, `min`/`max`/`abs`/`sqrt`/`pow` and the function qualifiers so the header compiles as both C++20 and Slang. Prove the Slang side compiles with `slangc` in the test's CMake, or in a build step, if `slangc` is available to the build (check how M84 compiles `Engine/Gpu/Kernels`).
    - Do not include or copy `MergeOperators.*`.
  - verify: `MergeMath_Test` has golden values per operator at in-range, above-1, negative and zero-alpha inputs, checked against Nuke's formula, or against the documented deviation where one applies. The header compiles under `slangc`. Both run at the milestone gate (one build per milestone).
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

- [ ] M101.P2.T2 — Add the new Merge node with a single layer and universal alpha
  - files: `Engine/Nodes/Merge/NativeMerge.{h,cpp}` (new), its registration (a `.reg.inc` fragment once M120 - Native Node Port Kit has merged, else `Engine/AppManager.cpp`), `Tests/Native/NativeMergeNode_Test.cpp` (new)
  - approach:
    - `Merge` v1 with the Design's inputs, operation, bbox, metadata from, frame range from, and the base class's mask and mix.
    - Use `layers = Single` only for now, with A/B/output layer knobs declared through `declareLayerKnob` (A and B input-bound, output a Target) and universal alpha channel selects per side.
    - A Composite op on M137's framework. Its row calls `MergeMath.h` over the `StagePlan`'s sources, folding A2..An in order inside the op past the source cap.
    - Override `getRegionOfDefinition` and `deriveOutputMetadata` per the Design. Identity comes through `Op::isIdentity` (B when no A is connected and the operator is identity for a transparent A). The framework composes it with mix and mask.
    - Rows outside every A's RoD copy B.
    - Name the class so it does not clash with `MergeNode` (deleted in P5) or the `Merge` namespace in `MergingEnum.h`.
  - verify: `NativeMergeNode_Test` covers over with an offset A, three A inputs, each bbox mode, mask and mix, identity with no A, a non-separable operator, and output-layer creation. It runs at the gate.
  - size: L

- [ ] M101.P2.T3 — Turn the old Merge presets into menu entries for the new node
  - files: `Engine/AppInstance.cpp` (`:1068` preset handling), `Engine/AppManager.cpp`, the node-menu or tab-menu preset source (find where presets are listed for M45 - Native Tab Menu), `Tests/Native/NativeMergeNode_Test.cpp`
  - approach:
    - Plus, Matte, Multiply, In, Out, Screen, Max, Min and Difference create `Merge` with `operation` set, and show the operator as the node's sub-label.
    - They are not separate plug-in IDs. Register the menu entries alongside the old presets until P5 deletes them.
  - verify: a test creates each preset entry and checks the plug-in ID and operation. Manual check: the tab menu lists them.
  - size: M

## Phase 101.3: Layers and alpha modes

- [ ] M101.P3.T1 — Add the `layers` modes: Single, All matching and List
  - files: `Engine/Nodes/Merge/NativeMerge.{h,cpp}`, `Tests/Native/NativeMergeNode_Test.cpp`
  - approach:
    - Add the `layers` choice.
    - All matching pairs the layers present on A and B by ID.
    - List uses a multi-layer knob, input-bound to B.
    - `getComponentsNeededAndProduced` asks each A input and B for the merged planes, produces those planes, and passes everything else through from B.
    - A layer missing on a side reads zero.
    - Follow the widen-on-write rules in `DESIGN/2026-09-26-rgba-rgb-alpha-layers.md` §3 for colour views.
  - verify: tests merge `rgba` + `beauty` + `spec` in All matching, a two-layer List, and a layer absent on A. All other layers on B are untouched.
  - size: M

- [ ] M101.P3.T2 — Add the per-side alpha source modes: Universal, Per-layer and Per-channel
  - files: `Engine/Nodes/Merge/NativeMerge.{h,cpp}`, `Tests/Native/NativeMergeNode_Test.cpp`
  - approach:
    - Add a per-side `alpha source` choice, plus the universal channel select and the per-channel alpha-layer select (input-bound).
    - Per-layer finds the layer's channel named `A`/`alpha`, case-insensitively, and falls back to the universal channel.
    - Per-channel requires the alpha layer's channel count to equal the merged layer's, and otherwise errors with both names.
    - Output alpha follows the Design.
    - The kernel takes an alpha span per channel. Universal and per-layer broadcast one span, and per-channel binds one per channel, so all three modes share one row path.
  - verify: tests check each mode on each side, including A per-layer over B universal, and per-channel with `alphaRGB` against a hand-computed result. The per-layer fallback and the mismatch error are covered.
  - size: L

## Phase 101.4: GPU path

- [ ] M101.P4.T1 — Write the Merge Slang kernel and register it
  - files: `Engine/Gpu/Kernels/merge.slang` (new, includes `MergeMath.h`), `Engine/Gpu/GpuKernelRegistry.cpp`, `Engine/Gpu/CMakeLists.txt`, `Engine/Nodes/Merge/NativeMerge.{h,cpp}`
  - approach:
    - Add `IBinaryPixelOp` as an op body that M137's `recordStagesGpu` wraps; no standalone kernel with its own mask/mix. Use one body parameterised by operator: a specialisation constant, or one entry point per operator if that is how the registry works.
    - The kernel takes a layer list, per-channel alpha bindings, mask, mix, and up to N A inputs folded in one dispatch. Past N, chain dispatches and keep the result resident.
    - `canRenderOnGpu` per the Design. `getGpuInputRoI` returns the output rect, as for a point op.
    - Strips outside every A's RoD copy B, with no operator dispatch.
    - Check M84's merged `GpuRender.h` first. Its interface may have shifted at the gate.
  - verify: covered by P4.T2's parity test.
  - size: L

- [ ] M101.P4.T2 — Test CPU/GPU parity and region chaining for Merge
  - files: `Tests/Gpu/GpuMerge_Test.cpp` (new), `Tests/CMakeLists.txt` or `Tests/Gpu/CMakeLists.txt`
  - approach:
    - On lavapipe, compare the GPU and CPU output for every operator × alpha mode, with two A inputs, mask and mix. Use exact tolerance for Porter-Duff operators and transcendental for blend modes, per `Engine/Nodes/README.md`'s tolerance table.
    - Placement test: Grade → Merge(A = Blur(B), B) → Grade is one resident region with one upload per external input. This is the region-to-region case M84 could not build.
  - verify: the test runs in CI's lavapipe job and at the gate.
  - size: M

## Phase 101.5: Replace the old Merge

- [ ] M101.P5.T1 — Move RotoPaint's internal merges to the new node
  - files: `Engine/RotoContext.cpp`, `Engine/RotoDrawableItem.cpp`, `Engine/RotoItem.cpp`, `Engine/RotoLayer.cpp`, `Engine/RotoStrokeItem.cpp`
  - approach:
    - Create `Merge` instead of `PLUGINID_OFX_MERGE`.
    - Map each knob set by name (`operation` and any channel or bbox knobs) to the new knobs. Input indices are unchanged (B = 0, A = 1, Mask = 2).
  - verify: the existing Roto/RotoPaint tests, plus a manual paint-stroke check in the GUI, at the gate.
  - size: M

- [ ] M101.P5.T2 — Move the bundled PyPlugs to the new node (Glow, LightWrap, DropShadow, EdgeBlur)
  - files: `Gui/Resources/PyPlugs/Glow.py`, `LightWrap.py`, `DropShadow.py`, `EdgeBlur.py`
  - approach: replace the old Merge IDs with `Merge` and set `operation` explicitly where a preset ID was used. Input indices are unchanged. Map any `AChannels`/`BChannels`/`OutputChannels`/`screenAlpha` settings to the new layer and alpha knobs. `screenAlpha` has no equivalent, so note any PyPlug whose output changes.
  - verify: `PyPlugNativeParams_Test` (updated in P5.T4) and rendering each PyPlug on a fixture at the gate.
  - size: M

- [ ] M101.P5.T3 — Move the bundled PyPlugs to the new node (Fill, PIKColor, ZMask)
  - files: `Gui/Resources/PyPlugs/Fill.py`, `PIKColor.py`, `ZMask.py`
  - approach: as P5.T2.
  - verify: as P5.T2.
  - size: M

- [ ] M101.P5.T4 — Point the tests that use Merge incidentally at the new node
  - files: `Tests/ColorViewsRender_Test.cpp`, `Tests/DefaultChannelSet_Test.cpp`, `Tests/GLScheduler_Test.cpp`, `Tests/LayerKnobs_Test.cpp`, `Tests/PoolSharing_Test.cpp`
  - approach: swap the ID and the knob names. Keep each test's intent. Where a test asserted old-node behaviour that no longer exists (alpha masking, OFX defaults), rewrite the assertion for the new behaviour and say so in the report.
  - verify: the five tests pass at the gate.
  - size: M

- [ ] M101.P5.T5 — Point the metadata, catalogue and PyPlug tests at the new node
  - files: `Tests/NativeMetadataPolicy_Test.cpp`, `Tests/NativeMetadata_Test.cpp`, `Tests/NodeCategory_Test.cpp`, `Tests/OfxMetadataBridge_Test.cpp`, `Tests/Native/NativePluginList_Test.cpp` (and `Tests/Native/PyPlugNativeParams_Test.cpp`, `Tests/Native/BlurFIRGaussian_Test.cpp` if they reference Merge)
  - approach: as P5.T4. `NativePluginList_Test` must expect `Merge` and no `net.sf.openfx.Merge*` IDs.
  - verify: these tests pass at the gate.
  - size: M

- [ ] M101.P5.T6 — Delete the old Merge
  - files: `Engine/Nodes/Merge/Merge.{h,cpp}`, `Engine/Nodes/Merge/MergeOperators.{h,cpp}`, `Tests/Native/NativeMerge_Test.cpp`, `Tests/Native/MergeOperators_Test.cpp`, plus the registration and references in `AppManager.cpp`, `AppInstance.cpp`, `Node.cpp`, `ParallelRenderArgs.cpp`, `EffectInstance.h` and `NodeDocumentation.cpp`
  - approach:
    - Delete the files and every reference: `git grep` for `PLUGINID_NATRON_MERGE`, `PLUGINID_OFX_MERGE`, `net.sf.openfx.Merge`, `MergeNode` and `MergeOperators` until nothing remains outside `docs/` history.
    - Optionally rename `NativeMerge.*` to `Merge.*` now that the name is free.
    - Confirm openfx-misc's Merge stays retired from the bundle (M67 - Native Core Nodes' fork PR), so the old IDs resolve to nothing. Old projects then load the missing-plugin node, which is M22 - Missing Plugin Placeholder, `todo`.
  - verify: the grep is empty and the full build and test suite pass at the gate.
  - size: M

## Phase 101.6: Docs and UAT

- [ ] M101.P6.T1 — Document the new Merge
  - files: `Engine/Nodes/README.md` (§148, §585, §615 and the tolerance table §658-676), the node's description string in `NativeMerge.cpp`, `docs/` user docs for Merge if present
  - approach:
    - Describe the inputs, operators with formulas, layer modes, alpha sources and what was dropped and why.
    - Put the HDR-safe deviation table in the user docs.
    - Remove the old Merge's README entries.
  - verify: the docs build (if the docs pipeline from M6 - Documentation Pass has a check). Review the text against the Design.
  - size: M

- [ ] M101.P6.T2 — UAT
  - files: none (user acceptance)
  - approach: hand the user a build plus a checklist:
    - over/screen/multiply on a CG multipass, in All matching and in List
    - per-layer alpha on beauty AOVs
    - per-channel alpha with an `alphaRGB` layer
    - three A inputs
    - mask and mix
    - each bbox mode
    - a GPU-resident comp, with the profile showing one region through the Merge
    - RotoPaint strokes
    - each bundled PyPlug
  - verify: the user signs off. Record the findings under `## Decisions`.
  - size: S

**Verification gate:** in one build, the full test suite passes in both scheduler modes:
- `MergeMath_Test`, `NativeMergeNode_Test` and `GpuMerge_Test` (lavapipe) pass.
- `git grep` finds no reference to the old Merge IDs or classes.
- RotoPaint and every bundled PyPlug render.
- CI is green.
- The user signs off the UAT.

## Decisions

- 2026-10-19 — **Built on M137 - Native Base Class Flexibility's framework** (user, M137 grill): M137 does not migrate today's Merge, since this milestone rewrites it, and leaves it as a Custom node. P2.T1 is cancelled because the Composite kind supersedes it. This milestone gains a dep on M137.
- 2026-10-12 — **No compatibility with the old Merge** (user): old Merge IDs are deleted outright. Old projects show the missing-plugin node (M22 - Missing Plugin Placeholder).
- 2026-10-12 — **Maths are Nuke's formulas, made HDR-safe, with no legacy toggle** (user).
- 2026-10-12 — **Dropped** (user): Nuke's mask fringe and inject, video colorspace, and alpha masking (`screenAlpha`). Masking is just the base class's mask channel, invert and mix.
- 2026-10-12 — **Per-channel alpha comes from an alpha layer** whose channel *i* is the alpha of merged channel *i* (user). This fits the 4-channel layer cap.
- 2026-10-12 — **Placed after M84 - GPU Placement And Residency and before M85 - Native GPU Kernels** (user). M85 no longer ports Merge.
- 2026-10-12 — **Plain ID `Merge`, not a reverse-domain ID** (user): native nodes drop reverse-domain IDs. The operation presets stay as menu entries; Merge is the one exception to "no duplicate menu entries".

## Hand-off from M85 - Native GPU Kernels

- `slang_add_kernel` has no include path, so `merge.slang` cannot `import pixelwrap` as things stand. Either add an include path to `cmake/SlangKernels.cmake` (P4.T1's first step), or put the Merge op inside `pixelwrap.slang`, as M85 did for Grade.
- Op parameters go in a storage buffer and wrap geometry in push constants (`WrapParams`, 116 B of the 128 B limit). A per-channel alpha binding table belongs in the buffer.
- "(Un)premult by" runs on the GPU only when the divisor is in the task's plane, otherwise the node renders on the CPU. Masks may come from any layer.
- Seed a GPU cost row by measuring a lone node's region `kernelMs` at UHD (`render-profile-report.py --emit-cost-table`). A node without a row only gets the class default.
