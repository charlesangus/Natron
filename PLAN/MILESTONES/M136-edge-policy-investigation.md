# M136 - Edge Policy Investigation

Decide, in one principled place, what an image does outside its bounds: black outside versus stretched edge. Today each node decides for itself: Transform defaults `black_outside` on, Reformat and Crop default it off, Blur is hard-wired to zero outside, EdgeDetect clamps to the nearest pixel, and ErodeDilate's GPU kernel reads zero where its CPU path clamps. M112 - Native Node Behaviour Fixes (`M112.P3.T2`) made Crop read black outside its source whatever the input is. This milestone gives the image one black-outside bit in `NodeMetadata`, sampled by one CPU helper and one GPU sampler, and moves the surveyed nodes onto it.

Deps: M167 - Per-Frame Node Metadata (the bit is a `NodeMetadata` field and rides its per-(time, view) propagation).

Acceptance sketch:
- A design note the user approves: where the black-outside state lives, how it propagates, and which nodes read or set it.
- Crop larger than its source gives stretched edges for a stretch-edge input and black for a black-outside input.
- The same rule holds on the CPU and GPU paths, and for every node in the survey.

## Scope

**In:**
- A design note the user approves, then the implementation across the surveyed native nodes (Crop, Reformat, Transform, the warp nodes, Blur and the other filters), CPU and GPU.
- A per-image black-outside bit, stored as a `NodeMetadata` field (rides M167 - Per-Frame Node Metadata's per-(time, view) propagation and hashing). Values: black outside or stretch edge.
- Sources (Read, generators) default to stretch edge. Nodes that do not touch the bit forward their primary input's bit.
- Crop, Reformat and Transform replace their boolean Black Outside knob with a tri-state: Input (default), Black, Stretch. The output bit follows the choice. Blur and the other filters get the same per-filter knob.
- Consumers read outside an input's RoD by bit-driven sampling: clamp to the nearest pixel for stretch, zero for black. One shared CPU helper and one shared GPU sampler. No materialised 1-pixel black border in the RoD.
- The resolved edge state shown in the node's metadata/info readout.
- Crop larger than its source respects the source's bit.

**Out:**
- Mirror, repeat and other edge modes: none (the bit is binary).
- Roto, shape and tracker black-outside code (`Engine/Roto*`, `Bezier.cpp`, `TrackerContext*`): none.
- OpenFX plugin changes or an OFX extension: none.
- Deep images and deep nodes: M21 - Deep Tier-2 And Bridges.
- A node-graph badge or any other new graph UI.

**Edge cases:**
- Multi-input nodes (Merge, Keymix, Copy): the output bit is the primary input's bit (B, or the first input).
- Switch, Dissolve and other identity nodes: follow the picked input's bit, per M167's identity rule.
- OpenFX nodes are opaque: they ignore the input bit and their output is the source default (stretch).
- Crop smaller than its source: the Crop's own tri-state governs what lies outside the crop rectangle.

**Assumptions:**
- Unconfirmed: the bit is hashed as part of `NodeMetadata`, so it invalidates caches without separate handling.
- Unconfirmed: the Nuke-style behaviour the user describes is a metadata bit plus a virtual black pixel; the design note confirms it only as far as needed, since sampling is bit-driven here.
- Unconfirmed: the filter list for the per-filter knob is every native node that samples outside its source RoD, found by the survey.

## Phase 136.1: Survey and design note

- [x] M136.P1.T1 — Survey every native node that samples outside its source and write the design note
  - files: new `Engine/Nodes/docs/edge-policy.md`; read-only survey of `Engine/Nodes/**`, `Engine/Gpu/Kernels/*.slang`
  - approach: Table every native node or kernel that reads outside its own position (start from Crop, Reformat, Transform, Blur, EdgeDetect, ErodeDilate, the Resampler family, and any warp, lens-distortion, Sharpen, Glow or Defocus node present now), with its CPU path, GPU kernel, current outside-source behaviour and the knob that controls it. Then state the model from `## Scope`/`## Decisions`: the `NodeMetadata` bit, source default, forwarding, tri-state knob, bit-driven sampling, OpenFX opacity. Note where the CPU (`PassExecutor` Boundary {Zero, Nearest}, `Resampler` clampCoord) and GPU (`resample.slang`, `blur_lines.slang`, `morphology.slang`) paths already differ and what each becomes. A surveyed node not covered by Phase 136.3 or 136.4 gets a new trailing task in Phase 136.4.
  - verify: the note exists, every surveyed node has a row, and the user approves it (the pm reports the note path and waits).
  - size: M


- [x] M136.P1.T2 — Refresh the design note to current main and the approved answers
  - files: `Engine/Nodes/docs/edge-policy.md`
  - approach: mark it approved 2026-11-13 and refresh every line reference to current main (`getDefaultMetadataFromInputs`, `applyIdentityRule`, the OFX metadata path, `computeHashInternal`). Add rows for nodes landed since the survey: DirBlur, GodRays, LensCorrect, SplineWarp, AutoStraighten (own `black_outside` knobs), AdjustBBox, Median, Bilateral, DenoiseProfiled, ToneEqualizer, ColorEqualizer (fixed fills), Defocus, IDefocus (with a Map input), ZDefocus, VectorBlur, KeyMix, FrameBlend; and check whether ContrastTexture, LensAberration, Retouch, SlitScan and the time nodes read off-position. Move Merge, Dissolve, KeyMix, ShuffleCopy and FrameBlend into a new §1.4a "Same-position reads" listing every mask, composite-source, ChannelRoute and Map/UV read site and its current zero rule. §2.3: the knob is `edgeMode` / "Edge Mode"; Q3's rule (a forced value governs the whole output and becomes the output bit); the default changes (a Read used as Merge A repeats its edge across a larger B). §2.4: Stretch reads need the RoI clamped into the input's RoD; the mask-misses-window shortcut and Merge's transparent-A skip hold only under Black; transform concatenation stops at a node whose resolved mode differs. §3: replace the questions with the answers; Q1 reads "every input follows its own edge bit… to keep an element's edge from smearing across a merge, set its upstream Crop or Transform to Black". Remove every reference to other applications and the `.local/docs` link.
  - verify: every native node under `Engine/Nodes/{Filter,Transform,Merge,Channel,Time,Color}` that reads off-position has a row and a task in this file; no other application is named.
  - size: M

## Phase 136.2: The bit and the shared samplers

- [x] M136.P2.T1 — Add the edge-mode field to `NodeMetadata`
  - files: `Engine/NodeMetadata.h`, `Engine/NodeMetadata.cpp`, `Tests/PerFrameMetadata_Test.cpp`
  - approach: Add `enum class EdgeMode : unsigned char { Stretch, Black }` and an output-level field `edgeMode`, default `Stretch`, to `NodeMetadataPrivate`. Include it in the copy constructor, `operator=` and the equality comparison. Per-input edge modes are not stored; consumers read the input's output metadata. Follow whatever per-(time, view) shape M167 - Per-Frame Node Metadata left the struct in.
  - verify: a gtest that a default metadata is Stretch, copies keep the field, and two metadata differing only in `edgeMode` compare unequal.
  - size: M

- [x] M136.P2.T2 — Propagate the bit through native nodes and make OpenFX opaque
  - files: `Engine/EffectInstance.cpp` (`getDefaultMetadataFromInputs`, `applyIdentityRule`), `Engine/OfxEffectInstance.cpp` (metadata path), `Engine/Nodes/Image/StagedImageEffect.{h,cpp}` (`getPreferredMetadataAt`), `Tests/PerFrameMetadata_Test.cpp`
  - approach: a node with no override forwards the primary input's (B, else first) `edgeMode`; Switch and other identity nodes report the picked input's, per M167's identity rule. Sources (Read, generators) report Stretch. A node that sets the mode does so in `getPreferredMetadataAt`. OpenFX effects report Stretch regardless of inputs.
  - verify: gtest graph: Constant (Stretch) → Grade → Merge (B primary); an upstream node forced to Black reaches the output through pass-through nodes, Switch follows its picked input, and an OpenFX node resets to Stretch.
  - size: M

- [x] M136.P2.T3 — One shared CPU out-of-bounds sampler
  - files: `Engine/Nodes/Image/PassExecutor.h`, `Engine/Nodes/Image/PassExecutor.cpp`, `Engine/Nodes/Image/Resampler.cpp`, `Engine/Nodes/Image/Resampler.h`, `Engine/Nodes/NodeSpec.h`
  - approach: Replace `InputBoundary {Zero, Nearest}` and `Resampler`'s `blackOutside` flag with the one `EdgeMode` type. The resample clamp-to-source and black-outside paths (`clampCoord`, `outsideBounds`) and the pass executor's boundary read share one helper that returns the nearest pixel for Stretch and zero for Black. No behaviour change at default settings beyond what Phase 136.3 and 136.4 specify; a node that has not moved yet passes its old behaviour as an explicit mode.
  - verify: gtest on the helper: reads at x=-3, x=width+3, corners and an empty-rect source for both modes; `PassExec::readChannelRow` (`PassExecutor.cpp`) takes the mode instead of a fill value; `Tests/Native/NodeSpec_Test.cpp` moves off `InputBoundary`; the existing `Resampler_Test` and `StagedResample_Test` pass unchanged.
  - size: M

- [x] M136.P2.T4 — One shared GPU out-of-bounds sampler
  - files: new `Engine/Gpu/Kernels/edgesample.slang`, `Engine/Gpu/Kernels/resamplecore.slang`, `Engine/Gpu/Kernels/pixelwrap.slang`, `Engine/Gpu/GpuResample.{h,cpp}`, `Engine/Nodes/Transform/WarpGpu.h`
  - approach: `edgesample.slang` holds one function returning the clamped or zero tap for an edge-mode parameter. One mode constant replaces every `k*BlackOutside` flag in these shared paths; per-kernel buffer fills move with their node tasks in Phase 136.4. Each kernel's arithmetic is otherwise unchanged. (CPU and GPU morphology already agree; nothing to fix there.)
  - verify: the kernels compile under `tools/ci/local/syntax-check.py`; a parity test samples both modes against the T3 CPU helper on a small image with taps outside the source.
  - size: L

- [x] M136.P2.T5 — Show the resolved edge state in the node info readout
  - files: `Engine/Node.cpp` (`makeInfoForInput`, beside the "Pixel aspect ratio" line), a gtest file for the readout
  - approach: add an "Edge Mode:" line for each input and for the output, reading Stretch or Black. No new panel or graph badge.
  - verify: gtest of `makeInfoForInput`'s string for both modes; an Xvfb screenshot of the readout goes to the user (P5.T2).
  - size: M


- [x] M136.P2.T6 — Stretch-aware region of interest
  - files: `Engine/Nodes/Image/StagedImageEffect.cpp`, `Engine/Nodes/Image/StagePlan.cpp`, `Engine/Nodes/Image/EdgeModeKnob.h`
  - approach: `EffectInstance` clips an input's RoI to its RoD and fetches nothing when that is empty, so a Stretch read wholly outside the input would get no pixels to repeat. Under Stretch, clamp each input's RoI into that input's RoD as a band at least one pixel wide.
  - verify: gtest — a window wholly outside a Stretch input still fetches its image and repeats the edge; under Black the input isn't fetched.
  - size: M

## Phase 136.3: The tri-state knob on Crop, Reformat and Transform

- [x] M136.P3.T1 — The Edge Mode pipeline stage
  - files: new `Engine/Nodes/Image/EdgeModeKnob.{h,cpp}`, `Engine/Nodes/Image/ImagePipelineSpec.h` (`Stage::EdgeMode`), `Engine/Nodes/Image/StagedImageEffect.cpp`, `Engine/Nodes/Image/StagePlan.{h,cpp}` (`plan.edgeMode`), `Engine/Nodes/Image/Resampler.h`
  - approach: most affected nodes are built on `ImageNode<Op>`/`StagedImageEffect`, so the knob is a pipeline stage: it adds an Input / Black / Stretch choice (script name `edgeMode`, label "Edge Mode", default Input), resolves it against the input's `edgeMode`, sets the output bit (a forced value overrides the input's and is passed on, per Q3) and hands the mode to PassList, Resample and IWarp. It replaces the `kResamplerParamFilterBlackOutside*` constants. Nodes not on the staged pipeline use the same helper directly.
  - verify: gtest of the resolver for all three choices against both input modes, including the output bit.
  - size: M

- [x] M136.P3.T2 — Move Transform onto the Edge knob
  - files: `Engine/Nodes/Transform/Transform.cpp`, `Engine/Nodes/Transform/Transform.h`, `Engine/Nodes/Image/ResampleExecutor.cpp`, `Engine/Nodes/Image/ResampleExecutor.h`, `Tests/Native/NativeTransformGpu_Test.cpp`
  - approach: Replace the boolean `black_outside` knob (default true; shared with ITransform) with the Edge Mode stage and drop `ResampleSettings::blackOutside` in favour of the mode. Concatenation resamples the whole chain under one mode, so it uses the resolved mode and stops at a node whose resolved mode differs. Update every test that set the old knob (`NativeTransform_Test`, `NativeTransformGpu_Test`); no import of old projects.
  - verify: Transform translated by half the image: Stretch input gives stretched edges, Black input gives black; forced Stretch on a Black input stretches and the output bit is Stretch; a concatenated chain with differing modes stops concatenating; CPU and GPU.
  - size: M

- [x] M136.P3.T3 — Move Reformat onto the Edge knob
  - files: `Engine/Nodes/Transform/Reformat.cpp`, `Engine/Nodes/Transform/Reformat.h`, `Tests/Native/NativeReformat_Test.cpp`
  - approach: Same replacement as Transform; Reformat's output `edgeMode` is its resolved mode, and a forced mode also governs the band between the source and the format (Q3). Update the tests that set the old knob; add `NativeReformatGpu_Test` cases.
  - verify: Reformat to a larger format with each of the three choices and both input modes, CPU and GPU.
  - size: M

- [x] M136.P3.T4 — Make Crop respect its source and move it onto the Edge knob
  - files: `Engine/Nodes/Transform/Crop.cpp`, `Engine/Nodes/Transform/Crop.h`, `Tests/Native/NativeCrop_Test.cpp`
  - approach: Replace the `Black Outside` boolean, its 1-pixel RoD growth and Reformat's coupling that toggles it with the Edge Mode stage. Under Input, the band between the source and the crop rectangle follows the input's bit. A forced Black or Stretch governs the whole output, that band included, and becomes the output bit (Q3). Outside the crop rectangle but inside the source, Black blacks and Stretch repeats the crop rectangle's edge.
  - verify: `NativeCrop_Test` and `NativeCropGpu_Test` cases: Crop larger than source for Stretch and Black inputs under Input; forced Stretch over a Black source and forced Black over a Stretch source; Crop smaller than source for each knob choice; the `intersect` and `softness` cases still pass.
  - size: M

## Phase 136.4: Filters

- [x] M136.P4.T1 — Blur edge mode
  - files: `Engine/Nodes/Filter/Blur.cpp`, `Engine/Nodes/Filter/Blur.h`, `Engine/Nodes/Filter/BlurKernels.h`, `Engine/Nodes/Filter/BlurKernels.cpp`, `Tests/Native/BlurFIRGaussian_Test.cpp`, `Tests/Gpu/BlurGpu_Test.cpp`
  - approach: Add the Edge knob (T1 of Phase 136.3). Replace the hard-wired Dirichlet zero with the shared sampler; the CPU `neumann` flag in `BlurKernels.h` becomes the mode. The RoD logic (`cropToFormat`, growth) is unchanged. Update the golden for the default; per the ulp rule, re-record rounding-only diffs.
  - verify: blur of a constant image with Stretch keeps its value to the border; Black darkens it; CPU matches GPU for both.
  - size: M

- [x] M136.P4.T2 — EdgeDetect edge mode
  - files: `Engine/Nodes/Filter/EdgeDetect.cpp`, `Engine/Nodes/Filter/EdgeDetect.h`, `Tests/Native/` (EdgeDetect test file)
  - approach: Replace the fixed Neumann read (`EdgeDetect.cpp:519-542`) with the shared sampler and add the Edge knob. Default Input follows the source, so a Black source now produces an edge at its border, as it should.
  - verify: edge detect of a constant image shows no border response under Stretch and a border response under Black.
  - size: M

- [x] M136.P4.T3 — Erode and Dilate edge mode
  - files: `Engine/Nodes/Filter/ErodeDilate.cpp`, `Engine/Nodes/Filter/ErodeDilate.h`, `Engine/Gpu/Kernels/morphology.slang`, `Tests/Native/` (ErodeDilate test file)
  - approach: Add the Edge Mode stage; the fill beyond the source follows the mode while the window clamp stays (CPU and GPU already agree, `NativeErodeDilateGpu_Test`).
  - verify: erode and dilate of an image with a bright border pixel, both modes, CPU == GPU.
  - size: M

- [x] M136.P4.T4 — Warp nodes edge mode
  - files: `Engine/Nodes/Transform/CornerPin.cpp`, `CornerPin.h`, `CornerPinKnobs.h`, `ITransform.cpp`, `ICornerPin.cpp`, `IWarp.cpp`, `IWarp.h`, `IDistort.cpp`, `IDistort.h`, `STMap.cpp`, `STMap.h`, `LensDistortion.cpp`, `LensDistortion.h`, `Mirror.cpp`, `WarpGpu.h`; their `Tests/Native/` tests
  - approach: Replace each `black_outside`/`blackOutside` boolean with the Phase 136.3 Edge knob; ITransform and ICornerPin follow Transform's and CornerPin's knob. `WarpSettings::blackOutside` and the `kWarpGpuBlackOutside`/`kIWarpGpuBlackOutside`/`kLensDistortionGpuBlackOutside` flags become the shared mode. STMap's Wrap U/V stay (the mode applies only under Wrap None). IDistort's and STMap's UV inputs are P4.T12. Mirror drops its hard-wired flag and forwards the bit. `expandRoD` and LensDistortion's margin key off the resolved mode. See `Engine/Nodes/docs/edge-policy.md` §1.2.
  - verify: each node, a setting with taps outside the source under Stretch and Black inputs, CPU == GPU.
  - size: M

- [x] M136.P4.T5 — Sharpen and EdgeExtend edge mode
  - files: `Engine/Nodes/Filter/Sharpen.cpp`, `Sharpen.h`, `EdgeExtend.cpp`, `EdgeExtend.h`, `Engine/Gpu/Kernels/sharpen.slang`, `edgeextend.slang`; their `Tests/Native/` tests
  - approach: Add the Edge knob; replace the fixed Nearest boundary and Neumann line ends (`Sharpen.cpp:252, 503`, `EdgeExtend.cpp:511, 516`) and the kernels' own clamped fills with the shared samplers.
  - verify: a constant image shows no border response under Stretch and one under Black, CPU == GPU.
  - size: M

- [x] M136.P4.T6 — Glow and Bloom edge mode
  - files: `Engine/Nodes/Filter/Glow.cpp`, `Glow.h`, `Bloom.cpp`, `Bloom.h`, `BloomKernels.cpp`, `BloomKernels.h`, `Engine/Gpu/Kernels/glow.slang`, `glow_frame.slang`, `bloom.slang`; their `Tests/Native/` tests
  - approach: Add the Edge knob; Glow's fixed Zero (`Glow.cpp:1016`) and Bloom's fixed Nearest (`Bloom.cpp:75`, `BloomKernels.cpp:224`) follow the resolved mode through the shared samplers.
  - verify: a constant image keeps its value to the border under Stretch and darkens under Black, CPU == GPU.
  - size: M

- [x] M136.P4.T7 — IBlur, IErodeDilate and EdgeBlur edge mode
  - files: `Engine/Nodes/Filter/IBlur.cpp`, `IBlur.h`, `IErodeDilate.cpp`, `IErodeDilate.h`, `EdgeBlur.cpp`, `EdgeBlur.h`, `VariableBlurKernels.cpp`, `VariableBlurKernels.h`, `Engine/Gpu/Kernels/iblur.slang`, `ierodedilate.slang`; their `Tests/Native/` tests
  - approach: Add the Edge knob for the source input; the fixed Zero source fill and the summed-area table's zero outside follow the resolved mode. Map inputs are P4.T11. EdgeBlur's matte keeps EdgeDetect's rule.
  - verify: a constant image keeps its value to the border under Stretch and darkens under Black, CPU == GPU.
  - size: M


- [x] M136.P4.T8 — Mask inputs and the MaskMix stage
  - files: `Engine/Nodes/Image/PassExecutor.cpp`, `Engine/Nodes/Image/StagePlan.cpp`, `Engine/Nodes/Image/ResampleExecutor.cpp`, `Engine/Gpu/Kernels/pixelwrap.slang`, `Engine/Gpu/Kernels/compositestep.slang`, `Engine/Nodes/Image/StagedImageEffect.cpp` (the mask-misses-window shortcut)
  - approach: every input follows its own edge bit (Q1): masks read through the shared samplers by the mask input's bit; the mask-misses-window identity shortcut applies only under Black.
  - verify: Blur and Merge with a small mask under Stretch and Black, CPU == GPU.
  - size: M

- [x] M136.P4.T9 — Composite sources: Merge, KeyMix, Dissolve
  - files: `Engine/Nodes/Image/StagePlan.cpp` (the composite fold), `Engine/Nodes/Merge/Merge.{h,cpp}`, `KeyMix.{h,cpp}`, `Dissolve.{h,cpp}`, `Engine/Gpu/Kernels/merge.slang`, `dissolve.slang`, `compositestep.slang`, `pixelwrap.slang`; their Native and Gpu tests
  - approach: each source and its alpha read by its own bit; Merge's transparent-A skip runs only under Black. Remove references to other applications from Merge's tooltips.
  - verify: A smaller than B, both modes, for each node, CPU == GPU.
  - size: L

- [x] M136.P4.T10 — ShuffleCopy channel routes
  - files: `Engine/Nodes/Channel/ChannelCopy.{h,cpp}`, `Engine/Nodes/Channel/Shuffle.cpp`, `Engine/Gpu/Kernels/channelroute.slang`, `Tests/Native/NativeShuffleGpu_Test.cpp`
  - approach: each routed input reads by its own bit.
  - verify: input 1 smaller than input 2, both modes, CPU == GPU.
  - size: M

- [x] M136.P4.T11 — Map inputs of IBlur, IErodeDilate, IDefocus and EdgeBlur
  - files: `Engine/Nodes/Image/MapInput.{h,cpp}` (`MapPlane::outside`, `tileRange`), `Engine/Gpu/Kernels/mapremap.slang`, `IBlur.cpp`, `IErodeDilate.cpp`, `IDefocus.cpp`, `EdgeBlur.cpp`
  - approach: Map planes follow the map input's bit; Stretch clamps.
  - verify: a Map smaller than the source, both modes, CPU == GPU.
  - size: M

- [x] M136.P4.T12 — IDistort and STMap UV inputs
  - files: `Engine/Nodes/Transform/IDistort.cpp`, `STMap.cpp`, `Engine/Gpu/Kernels/coordmap.slang`, `stmap.slang`
  - approach: as T11 for UV reads.
  - verify: a UV input smaller than the source, both modes, CPU == GPU.
  - size: M

- [x] M136.P4.T13 — LensCorrect, SplineWarp and AutoStraighten onto the Edge Mode knob
  - files: their `.{h,cpp}`, `Engine/Gpu/Kernels/lenscorrect.slang`, `splinewarp.slang`
  - approach: replace each `black_outside` knob (default on today) with the stage; LensCorrect's edge fade, its outside shortcut and its source-region bound key off the resolved mode.
  - verify: as P4.T4.
  - size: M

- [x] M136.P4.T14 — DirBlur and GodRays
  - files: `Engine/Nodes/Filter/DirBlur.{h,cpp}`, `GodRays.{h,cpp}`, `Engine/Gpu/Kernels/dirblur.slang`
  - approach: replace their `black_outside` knobs and GPU flag with the stage; `DirBlur::sourceRegion` keys off the mode.
  - verify: as P4.T4.
  - size: M

- [x] M136.P4.T15 — AdjustBBox onto the Edge Mode knob
  - files: `Engine/Nodes/Transform/AdjustBBox.cpp`, its GPU kernel and tests
  - approach: AdjustBBox grows the RoD and has its own Black/Nearest choice; replace it with the stage. Median and Bilateral clip their windows to the RoD and never read past it, so they get no knob (PM, from the refreshed survey).
  - verify: a grown box shows the input's edge repeated under Stretch and black under Black, CPU == GPU.
  - size: M

- [x] M136.P4.T16 — Defocus, IDefocus, ZDefocus and VectorBlur
  - files: their `.{h,cpp}` and GPU kernels; their Native and Gpu tests
  - approach: their fixed-black source reads follow the mode through the shared samplers.
  - verify: as P4.T15.
  - size: L


- [x] M136.P4.T18 — Depth, UV and time inputs of ZDefocus, VectorBlur and the time nodes
  - files: `Engine/Nodes/Filter/ZDefocus.cpp` (Depth input), `VectorBlur.cpp` (UV and Depth inputs), `Engine/Nodes/Time/FrameBlend.cpp`, `TimeBlur.cpp`, `SlitScan.cpp`, their GPU kernels and tests
  - approach: every input follows its own edge bit (Q1): these same-position reads of other inputs follow each input's bit through the shared samplers, as P4.T8–T12 do for masks, composites and maps.
  - verify: an auxiliary input smaller than the source, both modes, CPU == GPU, for each node.
  - size: L

- [x] M136.P4.T19 — NeuralUpscale onto the Edge Mode knob
  - files: `Engine/Nodes/Filter/NeuralUpscale.cpp` (or its location), its tests
  - approach: its taps clamp to the source frame with no knob; add the stage so Black reads zero past the source.
  - verify: an upscale of a small image shows repeated edges under Stretch and black under Black.
  - size: M

- [ ] ~~M136.P4.T17 — DenoiseProfiled, ToneEqualizer and ColorEqualizer fills~~ (cancelled: the refreshed survey found they read only inside their own region)
  - files: none
  - approach: none
  - verify: none
  - size: S

## Phase 136.5: Matrix and acceptance

- [x] M136.P5.T1 — Per-node CPU/GPU parity matrix over both bit values
  - files: `Tests/Native/EdgePolicy_Test.cpp` (new; `Tests/Native` is globbed)
  - approach: One parameterised gtest over every node with an Edge Mode knob plus Merge, KeyMix, Dissolve, ShuffleCopy, the mask path and the Map/UV inputs: a small image with distinct border values, rendered with a taps-outside-source setting under input Stretch and input Black and the three knob choices, CPU against GPU within ulps and against a hand-computed expectation.
  - verify: the matrix passes; a node added to the survey without a matrix row fails the build's test list review (the test enumerates the registered node ids it covers).
  - size: M

- [x] M136.P5.T2 — Xvfb GUI check of the Edge knobs
  - files: a new script beside `Tests/gui/guitest.py`, run with `tools/ci/remote/build.sh --from-run <id> --gui`
  - approach: Under Xvfb, build Read-like Constant → Crop (larger than the source) and Transform, and a Merge with A smaller than B; switch the Edge Mode knob through Input / Black / Stretch, read the info readout line, and capture a screenshot for each.
  - verify: the script exits zero, screenshots show stretched edges and black edges as expected, and the screenshots are sent to the user for sign-off.
  - size: M

**Verification gate:** `EdgePolicy_Test` passes in the release and debug PR CI legs; the matrix covers every node with an Edge Mode knob plus Merge, KeyMix, Dissolve, ShuffleCopy, the mask path and the Map and UV inputs, under both bit values and the three knob choices, CPU against GPU; the existing tests of each touched node plus `Resampler_Test` and the `Staged*` tests pass; the remote `--gui` check passes and the user approves the screenshots; Crop larger than its source stretches for a Stretch input under Input, is black for a Black input, and follows a forced setting over either, CPU and GPU.

## Decisions

- 2026-11-02: audit (plan state caught up with the branch). The design note is committed (4db8e9062, Engine/Nodes/docs/edge-policy.md, with its own Q1-Q4) and pushed. P1.T1 stays unticked until the user approves the note; no later phase has started.
- 2026-10-25 — Deliverable is a design note followed by implementation: the acceptance sketch already requires working CPU and GPU behaviour.
- 2026-10-25 — State is a per-image bit, Nuke-style: one principled place that propagates through the graph, and the only way Crop can respect its input.
- 2026-10-25 — Sources default to stretch edge and pass-through nodes forward the input's bit: the user's choice, so a plain Read stretches when transformed.
- 2026-10-25 — Crop, Reformat and Transform get a tri-state Input / Black / Stretch knob: Input follows the incoming bit and the user can force either; the fork has no backward compatibility, so the knob type changes freely.
- 2026-10-25 — Blur and the other filters get a per-filter knob like Transform, rather than a fixed rule: the user wants control per filter.
- 2026-10-25 — The bit is a `NodeMetadata` field and M136 depends on M167 - Per-Frame Node Metadata: this reuses its propagation and hashing and avoids two metadata mechanisms.
- 2026-10-25 — Consumers sample by bit with no materialised border: keeps the RoD the real pixel extent and avoids the RoI and cache interactions of a 1-pixel RoD growth.
- 2026-10-25 — OpenFX nodes are opaque and reset the chain to the default: OFX clip preferences have no slot for the bit.
- 2026-10-25 — Multi-input nodes take the primary input's bit: matches how PAR and format already resolve.
- 2026-10-25 — Verification gate is a per-node CPU/GPU parity matrix over both bit values, plus a Crop-larger-than-source case and an Xvfb GUI check of the tri-state knobs with screenshots.
- 2026-10-25 — Edge state is visible only in the metadata/info readout: no new graph decoration.
- 2026-10-25 — Elaborated after a code survey: the warp, lens-distortion, Sharpen, Glow and Defocus nodes were not found in this tree at planning time, so the survey task re-lists the nodes that sample outside their source when the milestone is claimed, and each gets a task then.
- 2026-11-02 — Q4: filters default to Input (they follow the input's edge bit); goldens re-recorded once (user).
- 2026-11-13 — **Design note approved (user)**, with answers: **Q1 — every input follows the edge bit** (user chose this over the recommendation): same-position reads of other inputs (Merge's A, Dissolve, Copy, every mask input, the Map inputs of IBlur, IErodeDilate, IDistort and STMap) repeat their edge too, which brings Merge, Dissolve, Copy and mask mixing into scope. **Q2 — Raw nodes stay out**: they forward the bit and keep their darktable borders. **Q3 — a forced mode overrides the input bit and passes the forced setting on**: Crop, Reformat and Transform forced to Black or Stretch govern the whole output (the band between the source region and the crop rectangle included) and set the output bit to the forced value. **Q4 — filters default to Input** (2026-11-02). Phases 2–5 to be re-checked against current main and extended for Q1 before implementation.
- 2026-11-13 — Re-planned after approval (consultant freshness check against b59ff3fdf): five premises were stale (P2.T2, P2.T4, P3.T1, P4.T3, P5.T2) and rewritten; the knob is delivered as a `Stage::EdgeMode` pipeline stage, since most affected nodes are `StagedImageEffect`s; added P1.T2 (note refresh), P2.T6 (Stretch-aware RoI), P4.T8–T17 (Q1's same-position inputs and nodes landed since the survey). "Copy" is ShuffleCopy (no native Copy; the OpenFX Copy is out of scope). KeyMix, FrameBlend's MaskMix and IDefocus's Map are in scope because every input follows the bit (Q1).
- 2026-11-13 — P1.T2 done (1b218d710): the note is refreshed and contains no references to other applications. Survey follow-ups: P4.T15 narrowed to AdjustBBox (Median and Bilateral never read past their region, so they get no knob); P4.T17 cancelled (those nodes read only inside their region); P4.T18 added (ZDefocus Depth, VectorBlur UV and Depth, and the FrameBlend, TimeBlur and SlitScan time reads, all in scope under Q1); P4.T19 added (NeuralUpscale clamps with no knob). Raw and the edge-aware filters stay out (Q2 and their whole-frame borders).\n- 2026-11-13 — P2.T3 scoping (PM): fill-value reads (the divisor's 1.f, STMap's alpha 1.f, mask and UV 0.f) keep a constant-fill reader for now, and only image-edge reads use the mode helper; masks and UV move to the bit in P4.T8, P4.T11 and P4.T12. `PassExec::Boundary` and `InputBoundary` are replaced by `EdgeMode` (Zero → Black, Nearest → Stretch), and every Resampler `blackOutside` caller is converted mechanically now; the GPU flag is translated in one line until P2.T4.
- 2026-11-13 — P2.T3 implemented (3dc34c091): `Engine/Nodes/Image/EdgeSample.h`. The NatronGpu library can't include `NodeMetadata.h`, so `GpuResample::blackOutside` stays a bool until P2.T4, which moves `EdgeMode` into a light header both libraries include.
- 2026-11-13 — P2.T5 and P2.T6 implemented (9dc00592c). Stretch RoI runs as a post-hook (`adjustInputRegionsOfInterest`) after `getRegionsOfInterest`, because staged nodes adjust the base result. Open points: the GPU input RoI gets no band yet; under concatenation `transformInputRois` runs after the hook. Phase 2 gate running.
- 2026-11-13 — Phase 3 implemented (42f35f04a). Transform's default changes from Black to Input (stretches over Stretch sources). The Stretch band is now computed after concatenation, and GPU input reads get it too. The Transform and Crop soft goldens may need re-recording. The Phase 3 gate replaces the Phase 2 one.
- 2026-11-13 — P4.T1–T7 implemented (e7e9b31dd). EdgeBlur's GPU read the matte as black past its edge while the CPU used the mode; fixed. The EdgeBlur and EdgeDetect matte always repeats its edge (PM: a black border would read as an edge), and only the blurred source follows the knob. Input resolves to Stretch for most sources, so border goldens change. Gating with `--record-goldens`, replacing the Phase 3 gate.
- 2026-11-13 — P4.T8–T10 implemented (ce6d616fc). Merge's transparent-A skip stays under both modes, since it checks A's actual pixels. Mask and route inputs get the Stretch RoI band on GPU too.
- 2026-11-13 — P4.T11–T13 implemented (5062b6c65). The Map reads of IWarp, IDistort and STMap follow the Map's mode through the shared `MapInput`. LensCorrect, SplineWarp and AutoStraighten now default to Input instead of Black. To check under T14 to T19: SplineWarp's mask/mix reference for a concatenated plane is still hard-coded Black.
- 2026-11-13 — P4.T14–T16 implemented (5863bc857); these nodes now default to Input. SplineWarp's concatenated reference follows the source's resolved mode, but its non-concatenated `readSourceRow` still reads black past the source. That is fixed with T18/T19.
- 2026-11-13 — P4.T18, P4.T19 and the SplineWarp read are implemented (828db9396). Phase 4 is done. PM calls: Depth and UV are layers of the Source, so they follow its mode. NeuralUpscale's model tiles stay inside the source, and only its alpha filter follows the mode. A `StagePlan::blendSourceFollowsEdgeMode` opt-in serves SplineWarp's GPU reference. The Stretch edge is the source image's own data window.
- 2026-11-13 — Phase 5 written (78a22c0f8, main merged in). It adds `EdgePolicy_Test`, which fails when a plugin has the knob but no matrix row, and `m136_edge_mode.py`. Merge has no knob, so the GUI script switches the Crop feeding A. Full gate running with `--record-goldens` and the GUI script; the screenshots go to the user for sign-off.
- 2026-11-14 — Gate on 78a22c0f8: NativeIWarpTest's four parity cases and two `m136_edge_mode.py` Transform checks failed; both test-side (d41057e2e). Under Stretch a Map-connected I-node's region covers its untransformed source, which the base node's doesn't, so the parity cases compare under Black and a new case compares under Stretch inside the base node's region; the script sampled the viewer's dashed region outline. Main (with M268 - Edge-Aware Region Independence) merged in (0ef51c2cd): ToneEqualizer keeps main's whole-frame job with `EdgeMode::Black` fills, and DenoiseProfiled's new variance fill moves to `EdgeMode::Black`. Re-gating with `--record-goldens`.
