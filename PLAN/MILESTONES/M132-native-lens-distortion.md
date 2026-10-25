# M132 - Native Lens Distortion

A redesigned native `LensDistortion` node. It replaces openfx-misc LensDistortion and covers every lens model the OFX node has:
- Nuke;
- the five 3DE4 models;
- PanoTools;
- PFBarrel, if cheap.

It distorts and undistorts, deriving one direction from the other with a Newton solver. Unlike the OFX node (identity Jacobian, a TODO), it computes a proper Jacobian, so filtering is correct under strong distortion. It can output an STMap instead of the image, and it handles overscan. Every model gets a GPU kernel on M113 - Native Warp Nodes' per-pixel-coordinate sampler. It uses a plain ID at major 1, maths is scene-linear, the host provides mask and mix, and there is no compatibility with the OFX node.

Deps: M113 - Native Warp Nodes (the shared coordinate sampler and `coordmap.slang`).

Scouted facts (scouted 2026-10-13 on main `338d9324a`; M85 - Native GPU Kernels facts from `origin/milestone/m85-native-gpu-kernels`; re-check at promotion). OFX sources are under `build/assets/plugin-src/openfx-misc/Distortion/`.

**OFX LensDistortion** (`Distortion.cpp`, versions 2 to 4):
- **Models** (`model`, `:3910`): Nuke, PFBarrel, 3DE Classic, 3DE4 Anamorphic Degree 6, 3DE4 Radial Fisheye Degree 8, 3DE4 Radial Standard Degree 4, 3DE4 Anamorphic Standard Degree 4, PanoTools.
- **Common knobs:**
  - `direction` (Distort/Undistort, default Distort).
  - `outputMode` (Image/STMap).
  - The Format group.
- **Per-model parameters:**
  - Nuke: `k1`, `k2`, `center`, `anamorphicSqueeze`, `asymmetricDistortion`.
  - PFBarrel: `pfFile`, `pfC3`, `pfC5`, `pfSqueeze`, `pfP`.
  - 3DE models: `tde4_*`.
  - PanoTools: `pt_*`.
- **Model maths** (`DistortionModel.{h,cpp}`):
  - Each model implements `distort` and `undistort`.
  - Base classes derive the missing direction by a Newton solve (`DistortionModelUndistort`/`DistortionModelDistort`, `DistortionModel.h:50-70`).
  - The model is evaluated at the pixel centre.
  - The Distort direction calls `undistort()` to find the source point (`:1049`).
  - The Nuke model is centred on the format and normalized by `max(fx, fy)` (`DistortionModel.cpp:187`).
- **Jacobian:** a TODO, fixed at identity (`Distortion.cpp:1060`).
- **STMap output** (`:1115`) writes `u = (sx-srcx1)/(srcx2-srcx1)`, `v` likewise, and alpha 1.
- **References:** `Engine/OfxEffectInstance.cpp:1279-1287` (`defaultProcessesAllLayers`). There are no PyPlug, menu or tracker references.

**Native pieces from M113 - Native Warp Nodes:**
- `resampleRowFromCoords` and the Jacobian helper in `Engine/Nodes/Image/Resampler.{h,cpp}`.
- `Engine/Gpu/Kernels/coordmap.slang`.
- Per-node fragments `Engine/Nodes/Transform/LensDistortion.{reg.inc,costseed.inc}`, `docs/LensDistortion.md`, `Engine/Gpu/Kernels/lensdistortion.cmake`, and the `Engine/OfxRetired/net.sf.openfx.LensDistortion` marker.

## Scope

**In:**
- Native `LensDistortion` with the Nuke, 3DE Classic, the four 3DE4 models and PanoTools models, plus PFBarrel if P1.T3 finds it cheap.
- Distort and Undistort for every model, with an analytic Jacobian where the model allows it and a finite-difference Jacobian otherwise.
- STMap output mode.
- Overscan: the output format and RoD can grow to hold the distorted image.
- A CPU kernel, a GPU kernel and parity tests per model.
- Retiring the OFX LensDistortion.

**Out:**
- Importing 3DE4 lens files and Nuke `.nk` LensDistortion settings. Owned by no milestone (Unconfirmed).
- Lens-grid solving or calibration: estimating parameters from a grid image. No milestone.
- Card3D and 3D camera projection → E9 - USD And 3D.
- Chromatic aberration and per-channel distortion → M131 - ZDefocus Lens Aberrations covers the defocus side. Per-channel lens distortion has no milestone.

**Edge cases:**
- Newton solve fails to converge, or leaves the model's valid domain (e.g. fisheye beyond 180°): the pixel is black (or alpha 0 in STMap mode), never NaN.
- Distort → Undistort round trip: it must return the input within the solver tolerance across the format.
- Render scale below 1: the model is evaluated in canonical coordinates, so a proxy render matches a downscaled full render within filter tolerance.
- Non-square pixel aspect and anamorphic squeeze: the model works in a pixel-aspect-corrected space, as the OFX node does.
- STMap output for a format with an offset origin: coordinates normalize against the source format box.

**Assumptions:**
- Unconfirmed: 3DE4 lens-file and Nuke `.nk` import are Out.
- Unconfirmed: overscan is a single "output format: input / input + overscan %" choice, plus an explicit-format option, not a free box knob.
- Unconfirmed: the node exposes only the selected model's parameters, with a per-model page or visibility switch, and keeps the 3DE4 parameter names so values copy across from 3DE.
- The 3DE4 model maths in openfx-misc matches the published 3DE4 lens-distortion SDK. P1.T2 checks this against the SDK formulas.

## Phase 131.1: Models

- [x] M132.P1.T1 — Port the lens-model library with Newton inversion and analytic Jacobians
  - files: `Engine/Nodes/Transform/LensModels.h` (new), `Engine/Nodes/Transform/LensModels.cpp` (new), `Tests/Native/LensModels_Test.cpp` (new)
  - approach:
    - An engine-free library: an abstract `LensModel { distort(p), undistort(p), jacobian(p, direction) }` over normalized lens coordinates, with a generic Newton inverse (damped, bounded iterations, a convergence flag) and a finite-difference Jacobian fallback.
    - Port the Nuke and PanoTools models from openfx-misc `DistortionModel.cpp`, with redesigned parameter structs (plain doubles, no OFX types).
    - Write the code in a C-like subset (no virtual calls in the inner evaluation, POD params) so P3 can mirror it in Slang line for line.
  - verify: the test covers round trips (distort∘undistort) within 1e-6 over a grid for both models, analytic Jacobian against finite differences, and reference values computed from the openfx-misc formulas. It runs at the phase build.
  - size: L

- [x] M132.P1.T2 — Add the 3DE Classic and four 3DE4 models
  - files: `Engine/Nodes/Transform/LensModels3DE.h` (new), `Engine/Nodes/Transform/LensModels3DE.cpp` (new), `Tests/Native/LensModels3DE_Test.cpp` (new)
  - approach:
    - Port 3DE Classic, Anamorphic Degree 6, Radial Fisheye Degree 8, Radial Standard Degree 4 and Anamorphic Standard Degree 4 onto P1.T1's interface.
    - Check each formula against the public 3DE4 lens-distortion plugin SDK's model definitions, and note any openfx-misc deviation under `## Decisions`.
    - Keep 3DE's parameter names.
  - verify: round-trip and Jacobian tests as P1.T1 for all five, plus SDK reference points where published. It runs at the phase build.
  - size: L

- [x] M132.P1.T3 — Decide PFBarrel and port it if cheap
  - files: `Engine/Nodes/Transform/LensModels.cpp`, `Tests/Native/LensModels_Test.cpp`, this milestone file (`## Decisions`)
  - approach:
    - Read openfx-misc's PFBarrel (`pfC3`, `pfC5`, squeeze, `pfP`, and the `.pfb` file reader).
    - If the model is a few lines on P1.T1's interface and the file reader is small, port both.
    - Otherwise record PFBarrel as Out, with the reason, under `## Decisions`, and the plan move.
  - verify: either PFBarrel round-trip tests pass at the phase build, or a dated decision explains the cut.
  - size: M

## Phase 131.2: Node on the CPU

- [x] M132.P2.T1 — Add the native LensDistortion node
  - files: `Engine/Nodes/Transform/LensDistortion.{h,cpp}` (new), `Engine/Nodes/Transform/LensDistortion.reg.inc`, `Tests/Native/NativeLensDistortion_Test.cpp` (new)
  - approach:
    - `LensDistortion` v1. Source input plus the host mask.
    - Knobs: `model`, `direction` (Distort/Undistort), `output` (Image/STMap), overscan/format, and the selected model's parameters only.
    - Per output pixel, compute the source coordinate (Distort uses the model's undistort, and vice versa) and its Jacobian. Sample with `resampleRowFromCoords`. Unconverged pixels read black.
    - STMap mode writes normalized u, v and alpha = converged.
    - `getRegionOfDefinition` grows by overscan.
  - verify: the test covers Distort then Undistort chained giving the input within filter tolerance, STMap output fed to M113 - Native Warp Nodes's STMap matching Image output, a non-convergent fisheye edge giving black with no NaN, and a render scale of 0.5 matching a downscaled full render. It runs at the phase build.
  - size: L

## Phase 131.3: GPU

- [ ] M132.P3.T1 — Mirror the lens models in Slang
  - files: `Engine/Gpu/Kernels/lensmodels.slang` (new), `Engine/Gpu/Kernels/lensmodels.cmake`, `Tests/Native/LensModelsGpu_Test.cpp` (new)
  - approach:
    - Line-for-line Slang ports of P1's models and the Newton inverse, using the same iteration cap and tolerance.
    - A test-only entry evaluates `distort`/`undistort`/`jacobian` over a coordinate buffer.
  - verify: per-model parity of coordinates and Jacobians, CPU against GPU, tolerance 1e-5 relative. It runs at the phase build on RADV.
  - size: L

- [ ] M132.P3.T2 — Render LensDistortion on the GPU
  - files: `Engine/Gpu/Kernels/lensdistortion.slang` (new), `Engine/Nodes/Transform/LensDistortion.cpp`, `Engine/Gpu/Kernels/lensdistortion.cmake`, `Tests/Native/NativeLensDistortionGpu_Test.cpp` (new)
  - approach:
    - A compute entry: lens model → source coordinate and Jacobian → `coordmap.slang` sample, wrapped by `pixelwrap.slang`. STMap mode writes the coordinates.
    - Pass the model parameters as a small storage buffer if they exceed the 128 B push-constant limit.
    - Add the cost seed.
  - verify: image and STMap output parity for every model, tolerance 1e-5 absolute. It runs at the phase build on RADV.
  - size: L

## Phase 131.4: Retire the OFX LensDistortion

- [ ] M132.P4.T1 — Retire the OFX LensDistortion
  - files: `Engine/OfxRetired/net.sf.openfx.LensDistortion`, `Engine/OfxEffectInstance.cpp` (`:1279-1287` entry), `Tests/DefaultChannelSet_Test.cpp`
  - approach: add the `Engine/OfxRetired/net.sf.openfx.LensDistortion` marker, drop its `defaultProcessesAllLayers` entry, and move or remove test cases that name it.
  - verify: a test asserts the OFX ID no longer loads and `LensDistortion` does. The full ctest passes at the gate.
  - size: M

**Verification gate:**
- The build and the full ctest pass, including the model, node and GPU parity tests on RADV.
- Xvfb GUI check, with screenshots shared before sign-off: each model family, Distort/Undistort round trip on a grid fixture, and STMap output.
- An AppImage built with `package.sh` for UAT.
- The OFX LensDistortion no longer loads.

## Decisions

- 2026-10-25 — **GPU precision: double-float, not fp64 (bosley-native-nodes):** GpuDevice doesn't enable shaderFloat64 and SPIR-V has no fp64 transcendentals, so the GPU models carry each double as a hi/lo float pair (as M113's stmap/resamplecore do), with the CPU's Newton settings unchanged and the fisheye rewritten without trig on the GPU. Measured on the host: worst 5e-12 relative on positions, solve success identical to the CPU at every test point; the model parity bound is 1e-9 relative. LensDistortion stays on the CPU in production until a measured cost seed exists (CPU profiles + an RX 7900 XTX kernel time).
- 2026-10-25 — **Output size (bosley-native-nodes):** the extent knobs (as on Crop) pick the reference format (Source by default), an `overscan` percentage grows it evenly on all sides, and the region of definition is the Source edges mapped through the lens plus a margin, with Crop To Format applied. The input region is the whole Source, as STMap does. Loading a `.pfb` also sets Squeeze; the output-mode knob keeps the OFX name `outputMode`. The cost seed is generated, so it moves to P3.
- 2026-10-25 — **Departures from openfx-misc (bosley-native-nodes):** 3DE and PFBarrel honour a format whose origin isn't (0,0); Fisheye8 drops ldpk's clamps and reports points at or past 90° as invalid (black); the PFBarrel reload bug is fixed (each key keeps its own values); the `.pfb` parser is strict; the inverse is a damped 2x2 Newton with analytic Jacobians (1e-12 relative) instead of a diagonal finite-difference step; PFBarrel is ported. Strong Nuke lenses on anamorphic formats have no distort solution near the corners, which go black; the node doc says so. Phase 3 must settle GPU precision (fp64 or a float tolerance with its own CPU/GPU bound).
- 2026-10-13 — **Split from M113 - Native Warp Nodes, with every model (user, native-node port grill):** LensDistortion is large enough for its own milestone. It covers Nuke, the 3DE models and PanoTools, plus PFBarrel if cheap. It reuses M113 - Native Warp Nodes's coordinate sampler and gets a GPU kernel per model.
- 2026-10-13 — **A real Jacobian:** the OFX node's identity Jacobian (`Distortion.cpp:1060`) under-filters strong distortion. The native node computes analytic or finite-difference Jacobians.
