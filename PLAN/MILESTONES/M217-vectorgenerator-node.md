# M217 - VectorGenerator Node

A native `VectorGenerator` node computes dense optical flow from its Source. It writes the existing Forward (t→t+1) and Backward (t→t−1) motion layers, in pixels, plus a 0..1 confidence layer, and passes RGBA through. It offers two classical methods:
- **DIS:** dense inverse search with variational refinement (Kroeger et al., ECCV 2016). Fast, and the default.
- **TV-L1:** coarse-to-fine TV-L1 (Zach, Pock and Bischof 2007, with Wedel et al.'s improvements). Slower, and higher quality.

An optional Matte input separates foreground and background so motion doesn't bleed across the matte edge. A temporal-smoothing knob, off by default, regularises the vectors over t±2. This milestone is CPU-only. M218 - VectorGenerator GPU ports it, M221 - Neural Optical Flow adds a third method, and M219 - Curves Follow Vectors is the first consumer. See `DECISIONS/2026-10-25-motion-vectors-epic.md`.

Deps:
- M124 - Native Time Remaps: the native multi-time fetch (`getFramesNeeded` plus fetching the input at another time).
- M120 - Native Node Port Kit (done): the per-node `.reg.inc`, and doc fragments.
- M137 - Native Base Class Flexibility (done): the Custom kind.

Scouted facts (2026-10-25, `origin/main` at `3c8553ec`; re-check at promotion):
- **No flow code exists anywhere in the tree.** There is no optical flow, DIS, TV-L1, KLT or pyramid code, and OpenCV isn't linked. Eigen is vendored at `libs/Eigen3`, but only `libs/ceres` puts it on an include path.
- **Motion layers already exist:**
  - `kNatronForwardMotionVectorsLayerID` and `kNatronBackwardMotionVectorsLayerID` are in `Engine/ImageLayerDesc.h:67-71`, with the label `kNatronMotionComponentsLabel` "Motion" at `:80`.
  - `getForwardMotionComponents()` and `getBackwardMotionComponents()` (`:252-253`) are 2-channel (`ImageLayerDesc.cpp:234,242`), and their OFX mapping is at `ImageLayerDesc.cpp:665-757`.
  - No native node produces or consumes them yet.
- **Producing new layers:** `getComponentsNeededAndProduced` puts produced layers in `(*comps)[-1]` and input needs in `(*comps)[0]`. `Engine/Nodes/Channel/AddLayers.cpp:181-199` is the model.
- **Multi-frame reads:** `Engine/Nodes/README.md:1119-1124` requires extra times to be declared through `getFramesNeeded()`. `Engine/Nodes/Merge/Dissolve.cpp:278` overrides it, as do `StagedImageEffect.cpp:574` and `IO/NativeWrite.cpp:1229`. The native base is `Engine/Nodes/Image/StagedImageEffect.{h,cpp}`. M124 adds the fetch-at-time helper. Read its milestone file at promotion for the final name.
- **Time nodes live in `Engine/Nodes/Time/`**, created by M124.
- **Reusable CPU pieces:** `Engine/Nodes/Image/Resampler.*` provides bilinear taps, and `Engine/Nodes/Filter/BlurKernels.*` provides Gaussian pre-smoothing.

## Scope

**In:**
- An `Engine/Flow/` library with no Qt dependency:
  - an image pyramid and gradients;
  - DIS with variational refinement;
  - TV-L1;
  - a forward/backward consistency confidence;
  - matte-separated estimation;
  - optional temporal smoothing.
- The `VectorGenerator` node: plain ID at major 1, Time grouping. It takes a Source and an optional Matte.
- Knobs:
  - `method` (DIS, TV-L1; default DIS);
  - `quality` (Fast, Medium, Fine; default Medium), mapped to each method's pyramid depth, patch size and iterations;
  - `flowChannels` (Luminance, RGB; default Luminance);
  - `matteChannel` (default the Matte's alpha);
  - `temporalSmoothing` (bool, default off) and `smoothingStrength`;
  - `confidenceThreshold`, used only for the viewer display;
  - `outputs` (Forward+Backward, Forward, Backward; default both).
- Outputs:
  - the Forward and Backward motion layers, in pixels at the render scale;
  - a new single-channel `vectorConfidence` layer, 0..1, where 1 means forward and backward agree and the pixel is not occluded;
  - RGBA passed through unchanged.
- Tests on synthetic sequences with known ground-truth flow, an icon and a doc.

**Out:**
- GPU kernels → M218 - VectorGenerator GPU.
- The neural method → M221 - Neural Optical Flow.
- Moving curves along the vectors → M219 - Curves Follow Vectors.
- Vector-driven image warps (VectorDistort or SmartVector-style paint propagation), and flow-interpolated retiming (Kronos/OFlow-style) → none.
- Consuming the vectors for motion blur → M191 - Native VectorBlur, which reads these layers unchanged.
- Refining precomputed (CG) vectors through a vectors input → none (user).

**Edge cases:**
- **First and last frame of the Source's time domain:** the missing neighbour's vectors are zero and their confidence is 0. The other direction is computed normally.
- **Identical neighbour frames:** zero vectors, within the test tolerance.
- **The neighbour frame's format differs** (a resolution change mid-sequence): render fails with a node error naming both frames.
- **Non-finite source pixels** are treated as 0 before the pyramid is built.
- **Proxy and render scale:** flow is computed at the render scale and the vectors are in pixels at that scale, so they scale with it.
- **Matte connected but empty:** identical to no matte.
- **Region of interest:** flow is global, so every render reads the Source's full RoD at t−1, t and t+1 (t±2 with smoothing) and computes the whole frame once. The cache serves later tiles. The RoD equals the Source's.
- **Large motion:** pyramid depth is derived from image size, with the coarsest level about 32 px wide, so motion up to about a quarter of the width is reachable.
- **Abort:** checked between pyramid levels and between iteration batches.

**Assumptions:**
- M124's fetch-at-time helper can return the Source's full RoD at a neighbour time, independent of the requested tile.
- Unconfirmed: a Matte splits estimation into two passes, inside and outside, each with a masked data term, and the results are composited by the matte. The confidence is computed per pass.
- Unconfirmed: temporal smoothing is a confidence-weighted blend of the t's vectors with t±1's vectors warped to t, with no joint solve.

## Phase 217.1: Flow library (CPU)

- [ ] M217.P1.T1 — Flow field type, pyramid and gradients
  - files: `Engine/Flow/FlowField.h` (new), `Engine/Flow/FlowPyramid.{h,cpp}` (new), `Engine/CMakeLists.txt`, `Tests/Flow/FlowPyramid_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach:
    - `FlowField` is a W×H float u/v plus confidence, with bilinear sampling and a warp-by-flow helper.
    - `FlowPyramid` builds a grey pyramid from a float image: Luminance (Rec.709 weights), or RGB stacked as three channels. Each level is a Gaussian pre-blur followed by a 2× downsample. It also computes per-level x/y gradients (Sobel or central differences).
    - Depth comes from the image size (the coarsest level about 32 px wide), unless overridden.
    - Put `libs/Eigen3` on the Engine include path for later tasks.
  - verify: `ctest -R FlowPyramid` passes. The test covers level sizes for odd dimensions, a constant image giving zero gradients, and a ramp giving the expected gradient.
  - size: M

- [ ] M217.P1.T2 — DIS patch inverse search and densification
  - files: `Engine/Flow/DisFlow.{h,cpp}` (new), `Tests/Flow/DisFlow_Test.cpp` (new)
  - approach:
    - Follow Kroeger et al. 2016. At each level, coarse to fine, run an inverse-compositional Gauss-Newton search on overlapping patches (patch size and stride from `quality`), initialised from the upsampled coarser flow.
    - Densify by weighting each patch's displacement by its photometric error.
    - Expose a parameter struct with presets for Fast, Medium and Fine.
  - verify: `ctest -R DisFlow` passes. On a textured synthetic plate (fixed-seed noise), the mean endpoint error (EPE) stays within the thresholds the test records for a 3.5 px translation, a 2° rotation about the centre and a 20 px translation (the pyramid case). Record those thresholds in the test.
  - size: L

- [ ] M217.P1.T3 — Variational refinement
  - files: `Engine/Flow/VariationalRefine.{h,cpp}` (new), `Engine/Flow/DisFlow.cpp`, `Tests/Flow/DisFlow_Test.cpp`
  - approach:
    - Implement the DIS paper's per-level refinement: a brightness-constancy data term, a gradient-constancy data term and a smoothness term, with robust penalisers.
    - Solve with fixed-point SOR iterations. Iteration counts come from `quality`.
    - DIS calls it after densification at each level.
  - verify: `ctest -R DisFlow` passes. EPE improves on the T2 cases by the margin the test records, and a piecewise-constant two-region motion keeps a sharp boundary.
  - size: L

- [ ] M217.P1.T4 — TV-L1 flow
  - files: `Engine/Flow/TvL1Flow.{h,cpp}` (new), `Tests/Flow/TvL1Flow_Test.cpp` (new)
  - approach:
    - Use the coarse-to-fine TV-L1 duality scheme of Zach, Pock and Bischof (Wedel et al.'s variant).
    - Run several warps per level, each with thresholding and dual updates for a fixed number of iterations, and median-filter the flow between warps.
    - Parameters (λ, θ, τ, warps, iterations) come from `quality` presets.
  - verify: `ctest -R TvL1Flow` passes. It runs the same synthetic cases as DisFlow, with its own recorded EPE thresholds, and must at least match DIS on the rotation case.
  - size: L

- [ ] M217.P1.T5 — Consistency confidence and matte-separated estimation
  - files: `Engine/Flow/FlowConfidence.{h,cpp}` (new), `Engine/Flow/FlowEstimate.{h,cpp}` (new), `Tests/Flow/FlowConfidence_Test.cpp` (new)
  - approach:
    - `FlowEstimate` is the single entry point. It takes the frames, the method, the parameters and an optional matte, and returns the forward and backward fields.
    - With a matte it runs two masked passes, each with its data term weighted by matte or 1−matte, and composites them by the matte.
    - Confidence comes from the forward/backward round trip: `exp(-|f(x)+b(x+f(x))|² / σ²)`, zeroed where x+f(x) leaves the frame.
  - verify: `ctest -R FlowConfidence` passes. A static background with a moving square has confidence below 0.2 in the strip the square uncovers and above 0.9 elsewhere. With a matte on the square, background EPE in a 4 px band around it improves on the matte-less run by the margin the test records.
  - size: M

- [ ] M217.P1.T6 — Temporal smoothing
  - files: `Engine/Flow/FlowTemporal.{h,cpp}` (new), `Engine/Flow/FlowEstimate.cpp`, `Tests/Flow/FlowTemporal_Test.cpp` (new)
  - approach:
    - Warp the t±1 fields to t through the t fields, then blend them confidence-weighted with the t fields by `smoothingStrength`.
    - `FlowEstimate` gains a variant that takes the five frames t−2..t+2.
  - verify: `ctest -R FlowTemporal` passes. On a constant-velocity sequence with added noise, frame-to-frame vector variance drops by the recorded margin while EPE does not rise.
  - size: M

## Phase 217.2: VectorGenerator node

- [ ] M217.P2.T1 — Write the VectorGenerator node
  - files: `Engine/Nodes/Time/VectorGenerator.{h,cpp}` (new), `Engine/Nodes/Time/VectorGenerator.reg.inc` (new), `Engine/CMakeLists.txt`
  - approach:
    - Read `Engine/Nodes/README.md` first.
    - Build on `StagedImageEffect`'s Custom kind, with the knobs from `## Scope`.
    - `getFramesNeeded` asks the Source for t−1, t and t+1, or t−2..t+2 with smoothing, clamped to its time domain, and the Matte for the same times. `getComponentsNeededAndProduced` produces Forward, Backward and `vectorConfidence` (define its `ImageLayerDesc` in the node), following `AddLayers.cpp:181-199`.
    - The RoI is the full Source RoD at each time.
    - Render calls `FlowEstimate` on the whole frame, then writes the requested window. Edge cases follow `## Scope`.
  - verify: at the phase build, `ctest -R NativeVectorGenerator` (T2) passes.
  - size: L

- [ ] M217.P2.T2 — Test the VectorGenerator node
  - files: `Tests/Native/NativeVectorGenerator_Test.cpp` (new)
  - approach:
    - Build graphs over a generated moving-texture sequence and check:
      - the Forward and Backward layers match ground truth within the library's thresholds;
      - RGBA passes through bit-exact;
      - the first and last frames give zero vectors with confidence 0 on the missing side;
      - render scale 0.5 halves the vectors;
      - a mismatched-format neighbour errors;
      - `outputs` = Forward skips the backward layer;
      - the Matte path runs.
    - Record regression fixtures.
  - verify: `ctest -R NativeVectorGenerator` passes.
  - size: M

- [ ] M217.P2.T3 — Icon and doc
  - files: `Gui/Resources/Images/NativeNodes/` (icon), `Engine/Nodes/Time/docs/VectorGenerator.md` (new)
  - approach:
    - Follow the M120 fragment conventions.
    - The doc covers the methods, quality presets, the matte, temporal smoothing, the confidence layer, the vector convention (pixels; forward is t→t+1) and the full-frame cost.
    - Declare `gpuWeight` Heavy: whole-frame multi-time work.
  - verify: the node shows its icon in the Time menu, and the doc renders in the node's help. Both are checked in the gate's Xvfb pass.
  - size: M

**Verification gate:**
- The build and the full ctest pass, including `FlowPyramid`, `DisFlow`, `TvL1Flow`, `FlowConfidence`, `FlowTemporal` and `NativeVectorGenerator`.
- Xvfb GUI check on a fixture plate under `build/`, with screenshots shared before sign-off:
  - the Forward layer viewed through the viewer's layer menu;
  - the `vectorConfidence` layer showing the occlusion strip;
  - the method switched between DIS and TV-L1.
- Timings for DIS and TV-L1 at Medium on a 2K frame, on the release AppImage on the target host, recorded in `## Decisions`.
- An AppImage built with `package.sh` for UAT.

## Decisions

- 2026-10-25 — **Classical methods first, neural later (user):** DIS and TV-L1 both ship as `method` choices. The neural method is M221 - Neural Optical Flow.
- 2026-10-25 — **Forward and backward layers, in pixels (user):** this matches the OFX/Nuke convention and M191 - Native VectorBlur's input.
- 2026-10-25 — **A confidence layer ships (user):** M219 - Curves Follow Vectors uses it to detect occluded points.
- 2026-10-25 — **An optional Matte input (user),** as in Nuke's VectorGenerator. There is no precomputed-vectors input.
- 2026-10-25 — **Frames independent by default, with temporal smoothing as a knob (user).**
- 2026-10-25 — **The node is named `VectorGenerator` (user),** after Nuke's.
