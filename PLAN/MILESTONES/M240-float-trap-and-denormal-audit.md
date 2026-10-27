# M240 - Float Trap And Denormal Audit

M184 - Variable-Size Filter Kit's RADV run (2026-10-26) found two classes of bug. This milestone checks every other native node and GPU parity test for them and fixes what it finds.

1. **Knob double→float overflow under debug traps.** A colour or double knob ranging over ±DBL_MAX stores ±inf as ±DBL_MAX (`Knob<T>::clampToMinMax`, `Engine/KnobImpl.h`). A node that then does `(float)value` overflows, and debug render threads trap overflow (`Engine/RenderScheduler.cpp`, about line 170), so the render dies with SIGFPE. Linux labels the signal with the highest-priority stale MXCSR flag, so it often reads "invalid". M184 added `knobValueAsFloat(double)` (`Engine/Nodes/KnobReader.h`) and fixed Constant and CheckerBoard. Grade, ColorCorrect, Clamp, Keyer, Transform and others likely have the same cast.
2. **Exact CPU/GPU comparisons that real GPUs fail on subnormals.** RADV, and NVIDIA (see the M169 - Pipelined Strip Member Timing inbox note on `CurveSnapshotPrecisionTest`), flush subnormal results to zero where the CPU and lavapipe keep them. M184 fixed `MapRemapGpu_Test` with a local `flushedBitsOf()` helper. Other parity tests that compare bits or ULPs can fail the same way on hardware while passing PR CI.

## Scope

**In:**
- An audit of native-node knob reads that convert to float, and of GPU parity comparisons (`Tests/Native/*Gpu_Test.cpp`, `Tests/Native/NativeGpuParity.h`, `Tests/Gpu/Tolerance.h`, `Tests/Gpu/*`).
- `knobValueAsFloat` applied at every overflowing site.
- One shared flush-aware comparison in the parity helpers, used wherever a test can produce subnormals.
- A regression sweep: render every registered native node with ±inf and ±huge values in its float knobs under debug traps, and fail on SIGFPE.

**Out:**
- Changing knob ranges or how knobs store inf.
- OFX plug-ins (they convert in their own code).
- Fixing kernels to keep subnormals. Flushing is accepted hardware behaviour, so tests adapt.

**Edge cases:**
- `knobValueAsFloat` lives on M184's branch until that merges. P2 work rebases onto main after M184 merges. Until then, only the P1 audit runs.
- A NaN knob value passes through unchanged and must not trap. Only overflow is in scope.
- Sweep nodes that need inputs get a small constant source. Nodes whose render legitimately fails on non-finite knobs (for example a size of inf) are expected failures, but must not crash.

## Phase 240.1: Audit

- [x] M240.P1.T1 — List overflowing knob-to-float conversions
  - files: `Engine/Nodes/**` (read only), this milestone file
  - approach: Find every native-node site that narrows a knob value (double) to float: `(float)`, `static_cast<float>` and implicit narrowing into float params in `bake`/`readColor`/param structs. Keep the sites whose knob range allows values beyond FLT_MAX (±DBL_MAX or unbounded). Record the list (file:line, knob, range) under this file's `

- [ ] M240.P2.T4 — knobValueAsFloat in keyers, filters and OCIO
  - files: `Engine/Nodes/Keyer/Keyer.cpp`, `Keyer/ChromaKeyer.cpp`, `Filter/EdgeDetect.cpp`, `OCIO/OCIODisplay.cpp`, `OCIO/OcioNodeBase.cpp`
  - approach: As P2.T2, for the sites listed in Decisions, including ChromaKeyer's `keyGain` products and OCIODisplay's `1./gamma`.
  - verify: the debug build passes, and the P2.T3 sweep passes for these nodes.
  - size: M

- [ ] M240.P2.T5 — knobValueAsFloat in the warps and lens nodes
  - files: `Engine/Nodes/Transform/IDistort.cpp`, `Transform/STMap.cpp`, `Transform/WarpGpu.h`, `Transform/LensDistortion.cpp`, `Transform/LensModelsGpu.h`
  - approach: As P2.T2. `setWarpGpuDouble` and `setLensModelGpuDouble` split a double into a float pair (hi, lo). Make the split non-trapping for values beyond FLT_MAX (hi = ±inf, lo = 0) so the GPU sees the same as the CPU.
  - verify: the debug build passes; the P2.T3 sweep and `ctest -R "STMap|IDistort|LensDistortion"` pass.
  - size: M

- [ ] M240.P2.T6 — knobValueAsFloat in the deep and scene nodes
  - files: `Engine/Nodes/Deep/DeepCrop.cpp`, `Deep/DeepFromImage.cpp`, `Deep/DeepExpressionEvaluator.cpp`, `Scene/SceneNodeAuthoring.cpp`, `Scene/USDCamera.cpp`, `Scene/USDLight.cpp`, `Scene/USDMaterial.cpp`
  - approach: As P2.T2 (seven files, but each is a one-to-two-line mechanical swap). The deep expression parser narrows `strtod` literals, so use the same helper there.
  - verify: the debug build passes, and `ctest -R "Deep|USD|Scene"` passes.
  - size: M

- [ ] M240.P2.T7 — knobValueAsFloat in the raw and write nodes
  - files: `Engine/Nodes/Raw/RawColorCalibration.cpp`, `Raw/RawPrepare.cpp`, `Raw/RawInputColor.cpp`, `Raw/RawColorCalibrationCheckerNode.cpp`, `IO/OiioWriteSupport.cpp`, `IO/NativeWrite.cpp`
  - approach: As P2.T2. First settle the audit's unsettled raw sites (RawInputColor `row0..2` through the matrix inversion; checker `corner`; spot `bottomLeft`/`size` in RawColorCalibrationSpot and RawTemperature) and fix those that narrow a ±DBL_MAX value.
  - verify: the debug build passes; the P2.T3 sweep and `ctest -R "Raw|Write"` pass.
  - size: M

## Decisions`.
  - verify: the list is recorded, with each entry's knob range checked against its knob definition.
  - size: M

- [x] M240.P1.T2 — List parity comparisons exposed to subnormal flush
  - files: `Tests/Native/*Gpu_Test.cpp`, `Tests/Native/NativeGpuParity.h`, `Tests/Gpu/Tolerance.h`, `Tests/Gpu/*` (read only), this milestone file
  - approach: Find comparisons that are bit-exact or tight in ULPs between CPU (or a double reference) and GPU output, and judge whether the tested inputs can produce subnormal results or intermediates (powers, exponentials, products of small values, Dekker splits). Record each exposed test with its reason under `## Decisions`.
  - verify: the list is recorded; each entry names the comparison and the input range that reaches subnormals.
  - size: M

## Phase 240.2: Fixes

- [x] M240.P2.T1 — Shared flush-aware comparison
  - files: `Tests/Native/NativeGpuParity.h`, `Tests/Gpu/Tolerance.h`, `Tests/Native/MapRemapGpu_Test.cpp`, the tests from P1.T2
  - approach: Move `flushedBitsOf()` from `MapRemapGpu_Test.cpp` into the shared parity helpers as an option of the image comparison (|x| < FLT_MIN counts as signed zero on both sides; everything else exact or under the existing tolerance). Make the tests listed in P1.T2 use it, and add the matching treatment to `tol::` for the double-reference tests.
  - verify: the touched suites pass on lavapipe in the build, and on RADV with `NATRON_DEV_GPU=1 tools/ci/local/test.sh ctest debug`.
  - size: M

- [ ] M240.P2.T2 — knobValueAsFloat in the colour nodes
  - files: `Engine/Nodes/Color/Grade.cpp`, `Color/Clamp.cpp`, `Color/ColorMathNode.cpp`, `Color/Saturation.cpp`, `Color/ColorCorrect.cpp`
  - approach: Replace each overflowing narrow listed in Decisions (P1.T1 audit) with `knobValueAsFloat` from `Engine/Nodes/KnobReader.h`. Where a double expression can itself overflow or divide by zero before narrowing (Grade's `mul*(wh-bl)/d` and `1./gamma` at gamma 0, ColorCorrect's `invGamma`, Saturation's row blend), make the expression non-trapping with the same result the CPU would give without traps (±inf), without changing finite results.
  - verify: the debug build passes, and the P2.T3 sweep passes for these nodes.
  - size: M

- [ ] M240.P2.T3 — Non-finite knob sweep under debug traps
  - files: `Tests/Native/NativeKnobNonFiniteSweep_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: For every registered native node (the registry `NativePluginList_Test` enumerates), set each float/double/colour knob in turn to +inf, -inf and ±1e300, render a small window from a constant source on the CPU in a debug build (traps enabled as on render threads), and assert the process doesn't die. A render error is allowed; a signal is not. Run it as one suite that prints the node and knob per case.
  - verify: `ctest -R NativeKnobNonFiniteSweep` passes in the debug build. Reverting one P2.T2 site makes it fail.
  - size: L

## Decisions

- 2026-10-26 — **Started alongside M184 (user):** the user asked for it to be picked up in parallel. P1 is read-only and runs now. P2 starts once M184 - Variable-Size Filter Kit has merged (it brings `knobValueAsFloat`), on a branch rebased onto main.

- 2026-10-26 — **RADV failures inherited from main (gpu, seen on M184 - Variable-Size Filter Kit's branch rebased onto main, which touches neither):** `NativeNoiseGpuTest.MatchesTheCpuRender` has 1-ulp CPU/GPU mismatches on RADV (white per pixel / rotated cells, worst 5.96e-08), and its exact comparison is in P1.T2's scope. `NativeGpuCapabilityTest.OnlyAllowlistedNativeNodesRenderOnGpu` reports OCIODisplay and OCIOLookTransform as GPU-capable but not allowlisted. Both pass PR CI on lavapipe. Also in scope: in debug builds, Merge `plus` (`MergeOperators::separable<31>`) traps FE_INVALID on +inf + -inf, so any pixel op that legitimately makes NaN from non-finite input kills a debug render. Decide whether pixel kernels should run with invalid held, or the trap stays and tests avoid it.

- 2026-10-26 — **P1.T2 audit result (subnormal exposure, main at 52d58e736):**
  - Every non-exact `ParityTolerance` (`Tests/NativeParity.cpp:631`, bound = abs + rel·max(1,|ref|), abs ≥ 1e-6) already absorbs flushed subnormals.
  - Latent exact sites, safe with their current inputs: Crop softness 0 (`NativeCropGpu_Test.cpp:493,643`), Saturation defaults (`:305,321`), StagedImageEffectGpu point-op (`:689,781`), RuntimeShaderGpu (`:337`), and the divide bounds in `StagedImageEffectGpu_Test.cpp:341-353` and `PixelWrap_Test.cpp:440-443` (relative only, no floor). Grade memcmp outside the mask (`NativeGradeGpu_Test.cpp:323,698`) sweeps denorm_min through a copy path; it should pass but is unverified on hardware.
  - Local flush helpers already exist (`NativeColorMathGpu_Test.cpp:237` `allowFlushedToZero`, `NativeColorLookupGpu_Test.cpp:258` `isDenormal`), and `CurveSnapshotPrecision_Test.cpp:36` has a FLT_MIN·2^48 floor.
  - Recommended home: an opt-in `ParityTolerance::flushSubnormals` (`exact().flushing()`) applied in `accepts()`, plus `ErrorStats::flushSubnormals` in `Tests/Gpu/Tolerance.h`, retiring the local helpers. P2.T1 follows this. Flushed *inputs* that change a result (subnormal divisor, negative-exponent pow) still need skip-or-bound handling.
  - **Contradiction:** the audit judged Noise safe at 1e-5, but `NativeNoiseGpu_Test.cpp:344` uses `ParityTolerance::exact()` for `Match::Exact` cases, and those fail on RADV by 1 ulp of normal values (5.96e-08), which is not a subnormal issue. P2 investigates it as a separate exact-match failure on hardware: the CPU and GPU hash/float paths differ by rounding.

- 2026-10-26 — **P1.T1 audit result (overflowing knob→float narrows, main at 52d58e736; table in this session's audit):**
  - About 28 files narrow a ±DBL_MAX (or [0,DBL_MAX]) knob to float on a render path.
  - **Colour:** Grade, Clamp, ColorMathNode (Add/Multiply/Gamma), Saturation, ColorCorrect.
  - **Generator:** Constant and CheckerBoard, both fixed in M184.
  - **Keyers, filters and OCIO:** Keyer and ChromaKeyer `keyColor`/`keyGain`, EdgeDetect `blurSize`, OCIODisplay `gain`/`gamma` and OcioNodeBase.
  - **Warps:** IDistort and STMap `uvOffset`/`uvScale` through `WarpGpu.h`, and LensDistortion through `LensModelsGpu.h`.
  - **Deep:** DeepCrop `zRange`, DeepFromImage `depth`, and DeepExpressionEvaluator literals.
  - **Scene:** SceneNodeAuthoring, USDCamera, USDLight, USDMaterial.
  - **Raw:** RawColorCalibration `x`/`y` (±1e300) and RawPrepare black/white levels. RawInputColor and the checker/spot knobs are unsettled.
  - **IO (indirect):** OiioWriteSupport metadata doubles and NativeWrite `formatPar`.
  - **Adjacent traps:** `1./gamma` at gamma 0 in Grade, ColorCorrect and OCIODisplay; Grade's `wh-bl` overflow.
  - Noise already guards with `boundedFloat()` (clamp to ±FLT_MAX, NaN→0). The fixes use M184's `knobValueAsFloat` instead, which keeps ±inf as the user typed it. P2.T2 was split by family into P2.T2, P2.T4, P2.T5, P2.T6 and P2.T7.

**Verification gate:** the full debug build and ctest pass on lavapipe (PR CI) and on RADV (`NATRON_DEV_GPU=1 tools/ci/local/test.sh ctest debug`), including the new sweep.
