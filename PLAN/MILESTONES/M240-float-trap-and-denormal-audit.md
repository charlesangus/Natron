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

- [ ] M240.P1.T1 — List overflowing knob-to-float conversions
  - files: `Engine/Nodes/**` (read only), this milestone file
  - approach: Find every native-node site that narrows a knob value (double) to float: `(float)`, `static_cast<float>` and implicit narrowing into float params in `bake`/`readColor`/param structs. Keep the sites whose knob range allows values beyond FLT_MAX (±DBL_MAX or unbounded). Record the list (file:line, knob, range) under this file's `## Decisions`.
  - verify: the list is recorded, with each entry's knob range checked against its knob definition.
  - size: M

- [ ] M240.P1.T2 — List parity comparisons exposed to subnormal flush
  - files: `Tests/Native/*Gpu_Test.cpp`, `Tests/Native/NativeGpuParity.h`, `Tests/Gpu/Tolerance.h`, `Tests/Gpu/*` (read only), this milestone file
  - approach: Find comparisons that are bit-exact or tight in ULPs between CPU (or a double reference) and GPU output, and judge whether the tested inputs can produce subnormal results or intermediates (powers, exponentials, products of small values, Dekker splits). Record each exposed test with its reason under `## Decisions`.
  - verify: the list is recorded; each entry names the comparison and the input range that reaches subnormals.
  - size: M

## Phase 240.2: Fixes

- [ ] M240.P2.T1 — Shared flush-aware comparison
  - files: `Tests/Native/NativeGpuParity.h`, `Tests/Gpu/Tolerance.h`, `Tests/Native/MapRemapGpu_Test.cpp`, the tests from P1.T2
  - approach: Move `flushedBitsOf()` from `MapRemapGpu_Test.cpp` into the shared parity helpers as an option of the image comparison (|x| < FLT_MIN counts as signed zero on both sides; everything else exact or under the existing tolerance). Make the tests listed in P1.T2 use it, and add the matching treatment to `tol::` for the double-reference tests.
  - verify: the touched suites pass on lavapipe in the build, and on RADV with `NATRON_DEV_GPU=1 tools/ci/local/test.sh ctest debug`.
  - size: M

- [ ] M240.P2.T2 — Use knobValueAsFloat at the audited sites
  - files: the sites listed in P1.T1 (split into a second task by node family if the list exceeds about five files)
  - approach: Replace each overflowing narrow with `knobValueAsFloat` from `Engine/Nodes/KnobReader.h`. No behaviour change beyond not trapping: values still round to nearest, overflow to ±inf, and NaN passes through.
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

**Verification gate:** the full debug build and ctest pass on lavapipe (PR CI) and on RADV (`NATRON_DEV_GPU=1 tools/ci/local/test.sh ctest debug`), including the new sweep.
