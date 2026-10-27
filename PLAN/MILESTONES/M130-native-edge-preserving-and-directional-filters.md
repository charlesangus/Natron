# M130 - Native Edge-Preserving And Directional Filters

Native, redesigned non-separable filters:
- **`Median`**, with a threshold that is safe for scene-linear values.
- **`Bilateral`**, with Guided as a guide-input mode. The range sigma is relative or log-space, not CImg's 0.3 on a [0,1] range.
- **`DirBlur`**, an accumulation blur with Directional, Radial and Zoom modes. It also takes over GodRays as a radial mode.

Every node follows the native conventions:
- plain ID at major 1;
- scene-linear maths;
- host mask, mix and (un)premult from the base class;
- no compatibility with the OFX versions.

Every node ships a CPU kernel, a GPU kernel, a native test, a GPU parity test, an icon and a size-scaled `gpuWeight()` where cost grows with the kernel size. It registers through M120 - Native Node Port Kit's per-node fragment files, and retires through one `Engine/OfxRetired/<ofx-plugin-id>` marker per retired ID.

Deps: M120 - Native Node Port Kit.

Scouted facts (scouted 2026-10-13 on `main` `338d9324a`; M85 - Native GPU Kernels facts from `origin/milestone/m85-native-gpu-kernels`; re-check at promotion):
- **CImg Median** (`build/assets/plugin-src/openfx-misc/CImg/Median/CImgMedian.cpp:142`): `blur_median(2*size+1, threshold)`, size 1, threshold 1. The threshold is in intensity units, so a default of 1 means almost nothing on [0,1] data and is meaningless on HDR.
- **CImg Bilateral** (`CImg/Bilateral/CImgBilateral.cpp:174`): sigma_s 10, sigma_r 0.3, 2 iterations, plus `CImgBilateralGuided` with a Guide clip. The cost is O(r²). sigma_r assumes a [0,1] range.
- **CImg Guided** (`CImgGuided.cpp:163`): radius 5, epsilon 0.2 (squared), 1 iteration. It is built on box filters. Epsilon assumes [0,1].
- **CImg RollingGuidance** (`CImgRollingGuidance.cpp:172`): an iterated bilateral, with sigma_s 10, sigma_r 0.1 and 4 iterations.
- **openfx-misc DirBlur** is a Transform3x3 variant (`openfx-misc/Transform/Transform.cpp:47-51`, ID `net.sf.openfx.DirBlur`). Its knobs are `amount`, `centered`, `fading` and the transform knobs. It accumulates samples along the transform path.
- **GodRays** (`openfx-misc/GodRays/GodRays.cpp:47`): fromColor, toColor, gamma (all default 1), steps 5 (2^steps scaled copies), max. Its gamma knob is a display-space hack.
- **References:**
  - `Engine/OfxEffectInstance.cpp`: `processAllLayers` lists DirBlur (`:1278`) and GodRays (`:1330`); `:1311-1329` lists the CImg IDs.
  - `Tests/gui/m45_tab_menu.py:214-219` searches the tab menu for "DirBlur".
  - `Engine/Settings.cpp:1319-1323` mentions DirBlur in help text.
- **Native resampling for DirBlur:**
  - `Engine/Nodes/Image/Resampler.h` provides `BlurSettings` (`:391`), `buildSamplingTransforms` (`:445`), which returns matrices plus weights and already supports directional blur, and `resampleRow` and `motionBlurPixel` (stratified van der Corput sampling).
  - M85.P5.T2 ports `motionBlurPixel` to the GPU, and M85.P5.T1 creates `resample.slang`.
- **Native spatial infrastructure:** `Engine/Nodes/Filter/SpatialFilterSupport.h` (`PlaneJob`, `MaskInput`, `WindowLayout`, `writeWindow`). Blur's GPU path is at `Blur.cpp:894-990`. Its gate refusing masks and mix at `:897` must not be copied; use M85 - Native GPU Kernels's `pixelwrap.slang` finish pass.

## Scope

**In:**
- Native `Median`, `Bilateral` (with a Guided mode) and `DirBlur` (Directional, Radial and Zoom modes, plus a GodRays-style radial mode). Each has CPU and GPU kernels and parity tests.
- Retirement of CImg Median, Bilateral, BilateralGuided, Guided and RollingGuidance, and of openfx-misc DirBlur and GodRays.

**Out:**
- Sharpen, Bloom, Glow, EdgeExtend and EdgeBlur: M123 - Native Sharpen And Glow.
- Defocus, ZDefocus, IBlur and IDefocus: M131 - Native Defocus Family.
- Denoise, Smooth and the shock filters stay on OFX. No milestone owns them.
- Vector (motion-vector) blur. No milestone owns it.

**Edge cases:**
- **Median threshold:** it is relative, `|in − median| / max(|median|, ε)`. A pixel is replaced only when it deviates by more than the threshold, and threshold 0 always replaces. This behaves the same on HDR and on [0,1] data.
- **Bilateral range sigma:** it is measured in log2 stops of luminance difference. A sigma of 1 stop treats 0.1 vs 0.2 the same as 10 vs 20. Negatives and zeros use a small floor before the log.
- **Guided mode without a guide connected:** it guides by the source itself (self-guided).
- **DirBlur Radial and Zoom centres:** they sit in canonical coordinates with a host position overlay. Samples outside the source read 0, the same as the resampler's black-outside.

**Assumptions:**
- Unconfirmed: RollingGuidance is retired, covered by Bilateral's `iterations` knob.
- Unconfirmed: GodRays becomes DirBlur's Radial mode plus a `fromColor`/`toColor` tint ramp. Its gamma knob is dropped.
- Unconfirmed: M85.P5.T2's GPU `motionBlurPixel` is reusable for DirBlur's directional mode. If it isn't, DirBlur writes its own accumulation kernel on `resample.slang`.

## Phase 129.1: Median

- [ ] M130.P1.T1 — Write the native Median node (CPU)
  - files: `Engine/Nodes/Filter/Median.h`, `Engine/Nodes/Filter/Median.cpp`, `Engine/Nodes/Filter/Median.reg.inc`
  - approach:
    - Knobs: `size` (radius, default 1) and `threshold` (relative, default 0).
    - Implementation:
      - Square window.
      - Partial selection (`std::nth_element`) on a reused per-thread buffer.
      - Plane jobs as in `SpatialFilterSupport.h`.
      - RoI expanded by `size`.
    - The relative threshold rule is in `## Scope`.
  - verify: at the phase build, `ctest -R NativeMedian` (T2) passes.
  - size: M

- [ ] M130.P1.T2 — Test Median on CPU
  - files: `Tests/Native/NativeMedian_Test.cpp`
  - approach:
    - Salt-and-pepper impulses vanish at size 1.
    - A step edge is preserved.
    - Size 0 is identity.
    - The threshold behaves the same at 1× and 100× scale.
    - Record regression fixtures.
  - verify: at the phase build, `ctest -R NativeMedian` passes.
  - size: M

- [ ] M130.P1.T3 — Median on the GPU
  - files: `Engine/Gpu/Kernels/median.slang`, `Engine/Gpu/Kernels/median.cmake`, `Engine/Nodes/Filter/Median.cpp`, `Tests/Native/NativeMedianGpu_Test.cpp`
  - approach:
    - Radius ≤ 2 uses a sorting network on registers.
    - Larger radii stage a shared-memory tile and select per pixel. Above a stated cap, for example radius 16, the GPU path refuses and the CPU path runs.
    - Results must be exact, since a median is a selection.
  - verify: at the phase build, `ctest -R NativeMedianGpu` passes on RADV with exact equality.
  - size: L

## Phase 129.2: Bilateral and Guided

- [ ] M130.P2.T1 — Write the native Bilateral node with a Guided mode (CPU)
  - files: `Engine/Nodes/Filter/Bilateral.h`, `Engine/Nodes/Filter/Bilateral.cpp`, `Engine/Nodes/Filter/Bilateral.reg.inc`
  - approach:
    - Inputs: Source, plus an optional Guide.
    - Knobs: `mode` (Bilateral, Guided), `size` (spatial sigma), `rangeSigma` (stops of luminance, default 1), `iterations` (1).
    - Bilateral weights each tap as `exp(-d²/2σs²) · exp(-Δstops²/2σr²)`.
    - Guided is He et al.'s guided filter on box means. Its ε is applied in log-luminance so it is scale-invariant.
  - verify: at the phase build, `ctest -R NativeBilateral` (T2) passes.
  - size: L

- [ ] M130.P2.T2 — Test Bilateral and Guided on CPU
  - files: `Tests/Native/NativeBilateral_Test.cpp`
  - approach:
    - A flat region is smoothed.
    - A step edge whose stop difference is well above rangeSigma survives.
    - Scaling the input by 100 scales the output by 100 (scale invariance).
    - A self-guided Guided result is close to Bilateral on an edge fixture.
    - Record regression fixtures.
  - verify: at the phase build, `ctest -R NativeBilateral` passes.
  - size: M

- [ ] M130.P2.T3 — Bilateral and Guided on the GPU
  - files: `Engine/Gpu/Kernels/bilateral.slang`, `Engine/Gpu/Kernels/bilateral.cmake`, `Engine/Nodes/Filter/Bilateral.cpp`, `Tests/Native/NativeBilateralGpu_Test.cpp`
  - approach:
    - Bilateral uses a tiled direct gather. Guided uses separable box means through `blur.slang`'s box filter plus point ops.
    - Iterations stay resident.
  - verify: at the phase build, `ctest -R NativeBilateralGpu` passes on RADV within 1e-4 relative.
  - size: L

## Phase 129.3: DirBlur

- [ ] M130.P3.T1 — Write the native DirBlur node with Directional, Radial and Zoom modes (CPU)
  - files: `Engine/Nodes/Filter/DirBlur.h`, `Engine/Nodes/Filter/DirBlur.cpp`, `Engine/Nodes/Filter/DirBlur.reg.inc`
  - approach:
    - Knobs: `mode`, `length` (pixels or angle for Radial), `center` (position overlay), `direction`, `samples`, `centered`, `falloff`, and `fromColor`/`toColor` for a GodRays-style tint along the accumulation.
    - Build the per-sample matrices with `buildSamplingTransforms` (`Resampler.h:445`) and accumulate through `resampleRow`, the same route Transform's motion blur takes.
  - verify: at the phase build, `ctest -R NativeDirBlur` (T2) passes.
  - size: L

- [ ] M130.P3.T2 — Test DirBlur on CPU
  - files: `Tests/Native/NativeDirBlur_Test.cpp`
  - approach:
    - Length 0 is identity.
    - A point source smears along the direction, around the centre (Radial) or towards the centre (Zoom).
    - Energy is conserved within tolerance.
    - Record regression fixtures.
  - verify: at the phase build, `ctest -R NativeDirBlur` passes.
  - size: M

- [ ] M130.P3.T3 — DirBlur on the GPU
  - files: `Engine/Gpu/Kernels/dirblur.slang`, `Engine/Gpu/Kernels/dirblur.cmake`, `Engine/Nodes/Filter/DirBlur.cpp`, `Tests/Native/NativeDirBlurGpu_Test.cpp`
  - approach: import M85 - Native GPU Kernels's `resample.slang` and accumulate samples per pixel with the same matrices and weights the CPU uses. Reuse M85.P5.T2's motion-blur port if its interface fits.
  - verify: at the phase build, `ctest -R NativeDirBlurGpu` passes on RADV within 1e-4 relative.
  - size: L

## Phase 129.4: Retire the OFX versions

- [ ] M130.P4.T1 — Retire the OFX and CImg versions and fix their references
  - files: `Engine/OfxRetired/<ofx-plugin-id>` (one per retired ID), `Engine/OfxEffectInstance.cpp`, `Engine/Settings.cpp`, `Tests/gui/m45_tab_menu.py`
  - approach:
    - Add an `Engine/OfxRetired/<ofx-plugin-id>` marker for each of: CImg Median, Bilateral, BilateralGuided, Guided and RollingGuidance; `net.sf.openfx.DirBlur`; GodRays.
    - Drop them from `processAllLayers` (`OfxEffectInstance.cpp:1278, 1330`) and from the CImg list (`:1311-1329`).
    - Update the `Settings.cpp:1319-1323` help text and the tab-menu GUI test (`m45_tab_menu.py:214-219`) to find the native DirBlur.
  - verify: at the phase build, `ctest` passes. The m45 tab-menu GUI script passes under Xvfb. Retired IDs load as missing-plugin nodes.
  - size: M

- [ ] M130.P4.T2 — Document the nodes
  - files: `Engine/Nodes/Filter/docs/{Median,Bilateral,DirBlur}.md`
  - approach: one doc file per node (M120 - Native Node Port Kit's scheme): knobs, maths, GPU caps and tolerances.
  - verify: reviewed at the gate against the node code.
  - size: S

## Decisions

- 2026-10-13 — **Family in scope (user, /cat-plan grill):** Median, Bilateral with Guided, and DirBlur, with GodRays folded in as a radial mode.
- 2026-10-13 — **Scene-linear range metrics:** thresholds and range sigmas are relative or in stops, never on [0,1]. This follows from the redesign rule (`DECISIONS/2026-10-12-native-nodes-drop-ofx-legacy.md`).
- 2026-10-13 — **GPU ships with each port (user).**

**Verification gate:** the full build and `ctest` pass. Every `Native*Gpu` parity test in this milestone passes on RADV. Under Xvfb, Median, Bilateral (both modes) and DirBlur (all modes) are rendered with a mask and mix ≠ 1, and the DirBlur centre overlay is dragged. Screenshots are shared with the user before sign-off. The retired IDs load as missing-plugin nodes. An AppImage built with `package.sh` is handed over for UAT.
