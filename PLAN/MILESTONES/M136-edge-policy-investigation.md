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

- [ ] M136.P1.T1 — Survey every native node that samples outside its source and write the design note
  - files: new `Engine/Nodes/docs/edge-policy.md`; read-only survey of `Engine/Nodes/**`, `Engine/Gpu/Kernels/*.slang`
  - approach: Table every native node or kernel that reads outside its own position (start from Crop, Reformat, Transform, Blur, EdgeDetect, ErodeDilate, the Resampler family, and any warp, lens-distortion, Sharpen, Glow or Defocus node present now), with its CPU path, GPU kernel, current outside-source behaviour and the knob that controls it. Then state the model from `## Scope`/`## Decisions`: the `NodeMetadata` bit, source default, forwarding, tri-state knob, bit-driven sampling, OpenFX opacity. Note where the CPU (`PassExecutor` Boundary {Zero, Nearest}, `Resampler` clampCoord) and GPU (`resample.slang`, `blur_lines.slang`, `morphology.slang`) paths already differ and what each becomes. A surveyed node not covered by Phase 136.3 or 136.4 gets a new trailing task in Phase 136.4.
  - verify: the note exists, every surveyed node has a row, and the user approves it (the pm reports the note path and waits).
  - size: M

## Phase 136.2: The bit and the shared samplers

- [ ] M136.P2.T1 — Add the edge-mode field to `NodeMetadata`
  - files: `Engine/NodeMetadata.h`, `Engine/NodeMetadata.cpp`, `Tests/NodeMetadata_Test.cpp` (or the existing NodeMetadata test file)
  - approach: Add `enum class EdgeMode : unsigned char { Stretch, Black }` and an output-level field `edgeMode`, default `Stretch`, to `NodeMetadataPrivate`. Include it in the copy constructor, `operator=` and the equality comparison. Per-input edge modes are not stored; consumers read the input's output metadata. Follow whatever per-(time, view) shape M167 - Per-Frame Node Metadata left the struct in.
  - verify: a gtest that a default metadata is Stretch, copies keep the field, and two metadata differing only in `edgeMode` compare unequal.
  - size: M

- [ ] M136.P2.T2 — Propagate the bit through native nodes and make OpenFX opaque
  - files: `Engine/Nodes/NativeEffectBase.cpp`, `Engine/Nodes/NativeEffectBase.h`, `Engine/Nodes/NodeSpec.h`, `Engine/EffectInstance.cpp`
  - approach: In the native metadata derivation, a node with no override forwards the primary input's (B, else first) `edgeMode`; Switch and other identity nodes report the picked input's, per M167's identity rule. Sources (Read, generators) report Stretch. A node that sets the mode declares it in its spec or overrides a single hook the T2 default calls. OpenFX effects report Stretch regardless of inputs.
  - verify: gtest graph: Constant(Stretch) → Grade → Merge(B primary); an upstream node forced to Black reaches the output through pass-through nodes, Switch follows its picked input, and an OpenFX node resets to Stretch.
  - size: M

- [ ] M136.P2.T3 — One shared CPU out-of-bounds sampler
  - files: `Engine/Nodes/Image/PassExecutor.h`, `Engine/Nodes/Image/PassExecutor.cpp`, `Engine/Nodes/Image/Resampler.cpp`, `Engine/Nodes/Image/Resampler.h`, `Engine/Nodes/NodeSpec.h`
  - approach: Replace `InputBoundary {Zero, Nearest}` and `Resampler`'s `blackOutside` flag with the one `EdgeMode` type. The resample clamp-to-source and black-outside paths (`clampCoord`, `outsideBounds`) and the pass executor's boundary read share one helper that returns the nearest pixel for Stretch and zero for Black. No behaviour change at default settings beyond what Phase 136.3 and 136.4 specify; a node that has not moved yet passes its old behaviour as an explicit mode.
  - verify: gtest on the helper: reads at x=-3, x=width+3, corners and an empty-rect source for both modes; the existing `Resampler_Test` and `StagedResample_Test` pass unchanged.
  - size: M

- [ ] M136.P2.T4 — One shared GPU out-of-bounds sampler
  - files: `Engine/Gpu/Kernels/resample.slang`, `Engine/Gpu/Kernels/blur_lines.slang`, `Engine/Gpu/Kernels/morphology.slang`, a new `Engine/Gpu/Kernels/edgesample.slang`, `Engine/Gpu/GpuResample.cpp`, `Engine/Gpu/GpuResample.h`
  - approach: Add `edgesample.slang` with one function returning the clamped or zero tap for an edge-mode parameter, and include it from the three kernels. Replace `kFlagBlackOutside` packing with the shared mode. Keep each kernel's arithmetic otherwise unchanged. Fixes `morphology.slang` zero-outside against the CPU Neumann path by taking the mode as a parameter.
  - verify: the slang kernels compile under `tools/ci/local/syntax-check.py`; `Tests/Gpu/BlurGpu_Test.cpp`-style parity test samples both modes against the CPU helper from T3 on a small image with taps outside the source.
  - size: L

- [ ] M136.P2.T5 — Show the resolved edge state in the node metadata readout
  - files: the Gui or Python readout that lists a node's PAR and bit depth (locate with `git grep -n 'pixel aspect' Gui/ Engine/`), `Engine/NodeMetadata.h`
  - approach: Find where the existing per-node metadata is shown to the user; the survey found no `NodeMetadata` use under `Gui/`, so locate it first and say where in the commit. Add one line, "Edge: Stretch" or "Edge: Black". Add no new panel or graph badge.
  - verify: a unit or Python test of the readout string for both modes; Xvfb screenshot of the readout shared with the user.
  - size: M

## Phase 136.3: The tri-state knob on Crop, Reformat and Transform

- [ ] M136.P3.T1 — Shared tri-state Edge knob helper
  - files: `Engine/Nodes/Image/ExtentKnobs.h`, `Engine/Nodes/Image/ExtentKnobs.cpp`, `Engine/Nodes/Image/Resampler.h`
  - approach: Add a helper that creates the Input / Black / Stretch choice knob (name `edge_mode`, default Input) and a reader that resolves it against the input's `edgeMode` into an `EdgeMode`. Replace the shared `kResamplerParamFilterBlackOutside*` constants with it. The node sets its output `edgeMode` from the resolved value.
  - verify: gtest of the resolver for all three choices against both input modes.
  - size: M

- [ ] M136.P3.T2 — Move Transform onto the Edge knob
  - files: `Engine/Nodes/Transform/Transform.cpp`, `Engine/Nodes/Transform/Transform.h`, `Engine/Nodes/Image/ResampleExecutor.cpp`, `Engine/Nodes/Image/ResampleExecutor.h`, `Tests/Native/NativeTransformGpu_Test.cpp`
  - approach: Replace the boolean `black_outside` knob (default true) with the helper from T1 and drop `ResampleSettings::blackOutside` in favour of the mode. Update every test that set the old knob; no import of old projects.
  - verify: Transform translated by half the image: Stretch input gives stretched edges, Black input gives black, on CPU and GPU.
  - size: M

- [ ] M136.P3.T3 — Move Reformat onto the Edge knob
  - files: `Engine/Nodes/Transform/Reformat.cpp`, `Engine/Nodes/Transform/Reformat.h`, `Tests/Native/NativeReformat_Test.cpp`
  - approach: Same replacement as Transform; Reformat's output `edgeMode` is its resolved mode. Update the tests that set the old knob.
  - verify: Reformat to a larger format with each of the three choices and both input modes, CPU and GPU.
  - size: M

- [ ] M136.P3.T4 — Make Crop respect its source and move it onto the Edge knob
  - files: `Engine/Nodes/Transform/Crop.cpp`, `Engine/Nodes/Transform/Crop.h`, `Tests/Native/NativeCrop_Test.cpp`
  - approach: Replace the `Black Outside` boolean and its 1-pixel RoD growth (`Crop.cpp:182-220, 447`) with the helper. With the crop rectangle larger than the source, pixels beyond the source come from the shared sampler under the resolved mode. Outside the crop rectangle but inside the source, Black blacks them and Stretch repeats the crop rectangle's edge. Revisit the M112.P3.T2 behaviour: with Input selected, Crop larger than a stretch-edge source shows stretched edges.
  - verify: `NativeCrop_Test` cases: Crop larger than source for Stretch and Black inputs; Crop smaller than source for each knob choice; the `intersect` and `softness` cases still pass.
  - size: M

## Phase 136.4: Filters

- [ ] M136.P4.T1 — Blur edge mode
  - files: `Engine/Nodes/Filter/Blur.cpp`, `Engine/Nodes/Filter/Blur.h`, `Engine/Nodes/Filter/BlurKernels.h`, `Engine/Nodes/Filter/BlurKernels.cpp`, `Tests/Native/BlurFIRGaussian_Test.cpp`, `Tests/Gpu/BlurGpu_Test.cpp`
  - approach: Add the Edge knob (T1 of Phase 136.3). Replace the hard-wired Dirichlet zero with the shared sampler; the CPU `neumann` flag in `BlurKernels.h` becomes the mode. The RoD logic (`cropToFormat`, growth) is unchanged. Update the golden for the default; per the ulp rule, re-record rounding-only diffs.
  - verify: blur of a constant image with Stretch keeps its value to the border; Black darkens it; CPU matches GPU for both.
  - size: M

- [ ] M136.P4.T2 — EdgeDetect edge mode
  - files: `Engine/Nodes/Filter/EdgeDetect.cpp`, `Engine/Nodes/Filter/EdgeDetect.h`, `Tests/Native/` (EdgeDetect test file)
  - approach: Replace the fixed Neumann read (`EdgeDetect.cpp:519-542`) with the shared sampler and add the Edge knob. Default Input follows the source, so a Black source now produces an edge at its border, as it should.
  - verify: edge detect of a constant image shows no border response under Stretch and a border response under Black.
  - size: M

- [ ] M136.P4.T3 — Erode and Dilate edge mode
  - files: `Engine/Nodes/Filter/ErodeDilate.cpp`, `Engine/Nodes/Filter/ErodeDilate.h`, `Engine/Gpu/Kernels/morphology.slang`, `Tests/Native/` (ErodeDilate test file)
  - approach: Add the Edge knob and use the shared CPU and GPU samplers. The GPU kernel currently reads zero outside while the CPU path clamps; both follow the mode now.
  - verify: erode and dilate of an image with a bright border pixel, both modes, CPU == GPU.
  - size: M

## Phase 136.5: Matrix and acceptance

- [ ] M136.P5.T1 — Per-node CPU/GPU parity matrix over both bit values
  - files: `Tests/Native/EdgePolicy_Test.cpp` (new), `Tests/CMakeLists.txt` or the test list the other Native tests use
  - approach: One parameterised gtest over the nodes from the survey (Crop, Reformat, Transform, Blur, EdgeDetect, Erode, Dilate, plus any added to Phase 136.4): a small image with distinct border values, rendered with a taps-outside-source setting under input Stretch and input Black, CPU against GPU within ulps and against a hand-computed expectation. Register the test in the CMake list used by sibling tests.
  - verify: the matrix passes; a node added to the survey without a matrix row fails the build's test list review (the test enumerates the registered node ids it covers).
  - size: M

- [ ] M136.P5.T2 — Xvfb GUI check of the Edge knobs
  - files: `Tests/gui/` (new script beside the existing GUI scripts), `build/deeprepro/run-gui.sh` (read only)
  - approach: Under Xvfb, build Read-like Constant → Crop (larger than the source) and Transform, switch the Edge knob through Input / Black / Stretch, read the metadata readout line, and capture screenshots for each.
  - verify: the script exits zero, screenshots show stretched edges and black edges as expected, and the screenshots are sent to the user for sign-off.
  - size: M

**Verification gate:** `EdgePolicy_Test` passes in the release and debug builds; the existing Crop, Reformat, Transform, Blur, EdgeDetect, Erode/Dilate and Resampler tests pass; the Xvfb GUI check passes and the user has approved the design note and the screenshots; Crop larger than its source gives stretched edges for a Stretch input and black for a Black input, CPU and GPU.

## Decisions

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
