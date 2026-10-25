# M171 - GPU Correctness Fixes

This milestone fixes two GPU problems found by later milestones. First, in a `-DNATRON_GPU=OFF` build, `StagedImageEffect::canRenderOnGpu` returns true for registry-kernel ops (found by M154 - Fresh Buffer Page Zeroing's first GPU-off CI build, run 39770415588). The cause is that `canRecordGpuOp` (`Engine/Nodes/Image/StagePlan.cpp:1032-1047`) accepts a Kind::Kernel op without checking `NATRON_HAVE_GPU`, although `recordStagesGpu` (`StagePlan.cpp:1050`) can only fail in that build. The effect is wrong and the test is right. The base contract (`Engine/Nodes/NativeEffectBase.h:290-299`) says canRenderOnGpu means "renderGpu() can produce this query". Every other GPU predicate already refuses when GPU is off: the RuntimeShader gate (`StagePlan.cpp:1023-1027`), `canRecordPassesGpu` (`PassExecutor.cpp:1122`) and `canRecordResampleGpu` (`ResampleExecutor.cpp:676`). Built-in nodes also return `GpuOpRef::none()` without GPU (for example `Engine/Nodes/Color/Clamp.cpp:216-231`). Only the test's injected kernel id (`Tests/Native/StagedImageEffectGpu_Test.cpp:502`) reaches the ungated branch. Run 39770415588 failed only this test, at :511, :528, :530 and :538. Second, `curvesnapshot.slang` computes double-float product errors with `fma(a, b, -p)` (found by M113 - Native Warp Nodes). Vulkan does not require fma to round once, and lavapipe rounds twice. This milestone applies the Dekker fix that M113 uses on its unmerged branch (31164fffe) and that `rawcolorcalibration.slang:100-113` already has on main. `offset.slang`'s `weightProduct`/`weigh` (:53, :69), merged with M110 - Remaining Native GPU Ports, has the same bug, which the original report missed. Filed 2026-10-24 from two inbox reports.

## Scope

**In:**
- Make the GPU-off answer of `canRecordGpuOp` false, and add a GPU-off CI leg. Neither `ci.yml` nor `nightly.yml` builds with GPU off; such a build only happens through a manual `remote-build.yml` dispatch with `cmake_args`.
- Replace the three fused error terms in `Engine/Gpu/Kernels/curvesnapshot.slang` (:71, :166, :196), and the two in `offset.slang` (:53, :69), with a Veltkamp/Dekker split under `precise`.
- Add a lavapipe probe test, plus a lint that stops new negated-addend `fma` error terms from appearing in `Engine/Gpu/Kernels/`.

**Out:**
- `Engine/Gpu/Kernels/resample.slang:129,168` (`ffMulF`, `ffMul`): M113 - Native Warp Nodes fixes these on `milestone/m113-native-warp-nodes` (31164fffe, `resamplecore.slang`), which hasn't merged to main yet. They stay allowlisted in the lint until it does.
- `gradeops.slang:6` `fma(a, v, b)`. This is a plain multiply-add, not an error term. Double rounding costs at most 1 ulp there, and the tolerance in `Tests/Gpu/GradeFixture.h:225` already covers it.
- `blur_lines.slang:132-150`, which already uses Dekker.

**Edge cases:**
- The Veltkamp split computes `a * 4097`, which overflows for |a| > ~8.3e34. When the split is not finite, the error term must be 0 (no extra precision), so no NaN reaches `lo` while `hi` is finite. The `!isfinite(r.hi)` fallback at `curvesnapshot.slang:109` must still apply.
- Denormal or flushed operands. The error term may be 0 and must not be NaN.

**Assumptions:**
- CI lavapipe still rounds `fma` twice, as M113 observed. The probe's canary checks this, so a fusing device cannot make the probe pass without testing anything.

## Phase 171.1: GPU-off StagedImageEffect test

- [ ] M171.P1.T1 — Refuse registry-kernel ops in `canRecordGpuOp` without `NATRON_HAVE_GPU`
  - files: `Engine/Nodes/Image/StagePlan.cpp`, `Engine/Nodes/Image/StagePlan.h`
  - approach: In `canRecordGpuOp` (`StagePlan.cpp:1032`), wrap the switch in `#ifdef NATRON_HAVE_GPU`. The `#else` branch uses `Q_UNUSED` and returns false, mirroring `StageGpuShaderGate::accept` (:1023-1027) and `canRecordPassesGpu`. Update the doc comment at `StagePlan.h:286-289` to say a GPU-off build accepts nothing. The fix covers both callers: `StagedImageEffect.cpp:178` and the render-time re-check at :249. Leave the test unchanged, because its `#ifdef NATRON_HAVE_GPU` expectation (:503-507) states the contract.
  - verify: PR CI (debug, asserts on, both legs; GPU on) passes `StagedImageEffectGpuTest`. The GPU-off check is one separate remote run, `tools/ci/remote/build.sh fast --tests --no-appimage --cmake-arg -DNATRON_GPU=OFF`, which must pass every suite including `StagedImageEffectGpuTest.HostAcceptsAPointOpOnlyWithAKernel`.
  - size: M

- [ ] M171.P1.T2 — Add a GPU-off build-and-test job to the nightly workflow
  - files: `.github/workflows/nightly.yml`
  - approach: Add a job `gpu-off-build-and-test` next to `nightly_release`. It uses the same container, checkout, test-asset and ccache steps (with a distinct ccache key suffix `-gpuoff`). The build step is `tools/ci/local/build.sh release --no-ccache-stats --cmake-arg -DNATRON_GPU=OFF`; `tools/ci/local/build.sh:35` documents the flag. The test step is `tools/ci/local/test.sh ctest release`, with `NATRON_TEST_REQUIRE_GPU` unset (see `remote-build.yml:294-301`). It gets its own "Check test results" step. Nightly runs on every push to `main` and daily, so a regression shows up within one merge without doubling the per-PR cost in `ci.yml`.
  - verify: actionlint passes in `checks.yml` lint-ci. A `workflow_dispatch` of Nightly on the branch shows the new job building with `NATRON_GPU:BOOL=OFF` in CMakeCache and ctest passing.
  - size: M

## Phase 171.2: curvesnapshot fma precision

- [ ] M171.P2.T1 — Replace the fused product-error terms in curvesnapshot.slang and offset.slang with Dekker's split, and add a lavapipe probe test
  - files: `Engine/Gpu/Kernels/curvesnapshot.slang`, `Engine/Gpu/Kernels/offset.slang`, `Tests/Gpu/kernels/curvesnapshot_probe.slang` (new), `Tests/Gpu/CurveSnapshotPrecision_Test.cpp` (new), `Tests/Gpu/CMakeLists.txt`
  - approach:
    - Add scalar `curveSplit` and `curveProductError(a, b, p)` to `curvesnapshot.slang`, copied from `rawcolorcalibration.slang:100-113`: `ffSplit` plus `e = ((ah*bh - p) + ah*bl + al*bh) + al*bl`, all under `precise`. `blur_lines.slang:134-150` has the same pattern in float4. Add the non-finite-split guard from Edge cases.
    - Use it in `curveMul` (:71), and in `CurveTableRange::at` for `pl` (:166) and `ql` (:196).
    - Apply the same fix in `offset.slang`'s `weightProduct` (:53) and `weigh` (:69), keeping `weigh`'s inf/nan/zero early return.
    - The probe kernel imports `curvesnapshot` and has two entries. `probe_mul` writes `curveMul(a, b)` hi/lo plus the product error, and `probe_fused` writes the raw `fma(a, b, -p)`. Register both like `fill` and `smoke` (`Tests/Gpu/CMakeLists.txt:19-21`).
    - The test feeds pairs whose float product has a nonzero error: curve-scale operands near 1, near 1e6 and near the split limit. It asserts `lo` equals the exact `(double)a*b - p`, and `hi + lo` matches the double product to 2^-44 relative. On lavapipe it also asserts that `probe_fused` loses the error for at least one pair, which is the canary.
  - verify: PR CI passes `CurveSnapshotPrecision*` on lavapipe, canary included. `NativeColorLookupGpuTest`, the ColorCorrect GPU parity suite, and the Crop, Position and StagedOffset GPU tests still pass at their current tolerances.
  - size: M

- [ ] M171.P2.T2 — Lint Engine/Gpu/Kernels for fused-fma error terms
  - files: `tools/ci/check_kernel_fma.sh` (new), `.github/workflows/checks.yml`
  - approach:
    - Write a shell check modelled on `tools/ci/check_serialization_layering.sh`. It fails on any `fma(<x>, <y>, -<z>)` in `Engine/Gpu/Kernels/*.slang`, and points at `rawcolorcalibration.slang:100` for the Dekker pattern.
    - Allowlist exactly `Engine/Gpu/Kernels/resample.slang`, with a comment naming M113, unless M113 has merged by the time this lands. The script also fails when an allowlisted file no longer contains a match, so the entry goes once M113's fix lands.
    - Add a lint-ci step, and shellcheck it alongside `checks.yml:173`.
  - verify: The script exits 0 on a tree with P2.T1 applied, and non-zero against `origin/main`'s `curvesnapshot.slang` and `offset.slang`. Lint-ci passes, and shellcheck is clean.
  - size: M

**Verification gate:** PR CI (debug, asserts on, both scheduler legs) is green, with `StagedImageEffectGpuTest` and `CurveSnapshotPrecision*` (canary included) passing on lavapipe. One separate remote `tools/ci/remote/build.sh fast --tests --no-appimage --cmake-arg -DNATRON_GPU=OFF` passes every suite, with `StagedImageEffectGpuTest` green; PR CI only builds with GPU on. `checks.yml` lint-ci passes with the new fma lint, and a dispatched Nightly run shows the GPU-off job green.

## Decisions

- 2026-10-25 — **Freshness check at claim, by the gpu PM** (consultant, against main `3c8553ec8`). P1.T1 and P1.T2 are current; their verify lines move to PR CI as the gate, with one separate GPU-off remote run. Caller lines are now `StagedImageEffect.cpp:178/249`. P2.T1's source commit `dfb04cdbe` doesn't exist. The Dekker template is now `rawcolorcalibration.slang:100-113` on main (31164fffe on M113's branch). `offset.slang:53,69` has the same fused `fma(…,-p)` and is added. P2.T2's allowlist moves to `resample.slang:129,168`, because M113 (earlier on the board, which owns that file) hasn't merged. **P3.T1 is dropped**, because its premise is gone. `NativeRawColorCalibrationGpu_Test.cpp:55` already uses `ParityTolerance::make(0., 1e-5)`, the test has passed on RADV twice on bases at or after `9d98c8202`, and the failures on 10-24 were on bases before M91 merged. Re-file it only from a fresh reproduction.
