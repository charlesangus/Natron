# M171 - GPU Correctness Fixes

This milestone fixes two GPU problems found by later milestones. First, in a `-DNATRON_GPU=OFF` build, `StagedImageEffect::canRenderOnGpu` returns true for registry-kernel ops (found by M154 - Fresh Buffer Page Zeroing's first GPU-off CI build, run 39770415588). The cause is that `canRecordGpuOp` (`Engine/Nodes/Image/StagePlan.cpp:1032-1047`) accepts a Kind::Kernel op without checking `NATRON_HAVE_GPU`, although `recordStagesGpu` (`StagePlan.cpp:1050`) can only fail in that build. The effect is wrong and the test is right. The base contract (`Engine/Nodes/NativeEffectBase.h:290-299`) says canRenderOnGpu means "renderGpu() can produce this query". Every other GPU predicate already refuses when GPU is off: the RuntimeShader gate (`StagePlan.cpp:1023-1027`), `canRecordPassesGpu` (`PassExecutor.cpp:1122`) and `canRecordResampleGpu` (`ResampleExecutor.cpp:676`). Built-in nodes also return `GpuOpRef::none()` without GPU (for example `Engine/Nodes/Color/Clamp.cpp:216-231`). Only the test's injected kernel id (`Tests/Native/StagedImageEffectGpu_Test.cpp:502`) reaches the ungated branch. Run 39770415588 failed only this test, at :511, :528, :530 and :538. Second, `curvesnapshot.slang` computes double-float product errors with `fma(a, b, -p)` (found by M113 - Native Warp Nodes). Vulkan does not require fma to round once, and lavapipe rounds twice. This milestone applies the Dekker fix that M113 used in dfb04cdbe. Filed 2026-10-24 from two inbox reports.

## Scope

**In:**
- Make the GPU-off answer of `canRecordGpuOp` false, and add a GPU-off CI leg. Neither `ci.yml` nor `nightly.yml` builds with GPU off; such a build only happens through a manual `remote-build.yml` dispatch with `cmake_args`.
- Replace the three fused error terms in `Engine/Gpu/Kernels/curvesnapshot.slang` (:71, :166, :196) with a Veltkamp/Dekker split under `precise`.
- Add a lavapipe probe test, plus a lint that stops new negated-addend `fma` error terms from appearing in `Engine/Gpu/Kernels/`.

**Out:**
- `Engine/Gpu/Kernels/resample.slang:117,135`. M113 - Native Warp Nodes owns this file on `milestone/m113-native-warp-nodes`, where it is renamed `resamplecore.slang` and already fixed (dfb04cdbe). It is not on `origin/main` yet.
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
  - approach: In `canRecordGpuOp` (`StagePlan.cpp:1032`), wrap the switch in `#ifdef NATRON_HAVE_GPU`. The `#else` branch uses `Q_UNUSED` and returns false, mirroring `StageGpuShaderGate::accept` (:1023-1027) and `canRecordPassesGpu`. Update the doc comment at `StagePlan.h:286-289` to say a GPU-off build accepts nothing. The fix covers both callers: `StagedImageEffect.cpp:169` and the render-time re-check at :237. Leave the test unchanged, because its `#ifdef NATRON_HAVE_GPU` expectation (:503-507) states the contract.
  - verify: `tools/ci/remote/build.sh fast --tests --no-appimage --cmake-arg -DNATRON_GPU=OFF` passes all suites, including `StagedImageEffectGpuTest.HostAcceptsAPointOpOnlyWithAKernel`. The GPU-on `tools/ci/remote/build.sh fast --tests --no-appimage` still passes `StagedImageEffectGpuTest` on lavapipe.
  - size: M

- [ ] M171.P1.T2 — Add a GPU-off build-and-test job to the nightly workflow
  - files: `.github/workflows/nightly.yml`
  - approach: Add a job `gpu-off-build-and-test` next to `nightly_release`. It uses the same container, checkout, test-asset and ccache steps (with a distinct ccache key suffix `-gpuoff`). The build step is `tools/ci/local/build.sh release --no-ccache-stats --cmake-arg -DNATRON_GPU=OFF`; `tools/ci/local/build.sh:35` documents the flag. The test step is `tools/ci/local/test.sh ctest release`, with `NATRON_TEST_REQUIRE_GPU` unset (see `remote-build.yml:294-301`). It gets its own "Check test results" step. Nightly runs on every push to `main` and daily, so a regression shows up within one merge without doubling the per-PR cost in `ci.yml`.
  - verify: actionlint passes in `checks.yml` lint-ci. A `workflow_dispatch` of Nightly on the branch shows the new job building with `NATRON_GPU:BOOL=OFF` in CMakeCache and ctest passing.
  - size: M

## Phase 171.2: curvesnapshot fma precision

- [ ] M171.P2.T1 — Replace the fused product-error terms in curvesnapshot.slang with Dekker's split, and add a lavapipe probe test
  - files: `Engine/Gpu/Kernels/curvesnapshot.slang`, `Tests/Gpu/kernels/curvesnapshot_probe.slang` (new), `Tests/Gpu/CurveSnapshotPrecision_Test.cpp` (new), `Tests/Gpu/CMakeLists.txt`
  - approach: Add scalar `curveSplit` and `curveProductError(a, b, p)` to `curvesnapshot.slang`, copied from M113's `resamplecore.slang` (`git show dfb04cdbe -- Engine/Gpu/Kernels/resamplecore.slang`: `ffSplit` with `* 4097.0` under `precise`, and `e = ((ah*bh - p) + ah*bl + al*bh) + al*bl`). Add the non-finite-split guard from Edge cases. Use it in `curveMul` (:71), in `CurveTableRange::at` for `pl` (:166) and for `ql` (:196). The probe kernel imports `curvesnapshot` and has two entries. `probe_mul` writes `curveMul(a, b)` hi/lo and the product error for operand pairs from a buffer. `probe_fused` writes the raw `fma(a, b, -p)`. Register both like `fill`/`smoke` (`Tests/Gpu/CMakeLists.txt:19-21`) and add the test source to `GpuTests_SOURCES`. The test feeds pairs whose float product has a nonzero error, including curve-scale operands near 1, near 1e6 and near the split limit. It asserts `lo` equals the exact `(double)a*b - p` and `hi + lo` matches the double product to 2^-44 relative. On lavapipe (device name from `Tests/Gpu/GpuTestDevice.h`), it asserts that `probe_fused` loses the error for at least one pair. That canary shows the fused form would have failed this test.
  - verify: `tools/ci/remote/build.sh fast --tests --no-appimage` passes `CurveSnapshotPrecision*` on lavapipe, with the canary asserting there. `NativeColorLookupGpuTest` and the ColorCorrect GPU parity suite (both import curvesnapshot) still pass at their current tolerances.
  - size: M

- [ ] M171.P2.T2 — Lint Engine/Gpu/Kernels for fused-fma error terms
  - files: `tools/ci/check_kernel_fma.sh` (new), `.github/workflows/checks.yml`
  - approach: Write a shell check modelled on `tools/ci/check_serialization_layering.sh`. It fails on any `fma(<x>, <y>, -<z>)` in `Engine/Gpu/Kernels/*.slang`, pointing at `blur_lines.slang:132` for the Dekker pattern. Allowlist exactly `Engine/Gpu/Kernels/resample.slang`, with a comment naming M113. The script also fails if an allowlisted path no longer exists, which forces the entry to be removed when M113's rename lands. Add a lint-ci step, and shellcheck it alongside :173.
  - verify: The script exits 0 on a tree with P2.T1 applied, and exits non-zero when run against `origin/main`'s `curvesnapshot.slang`. Lint-ci passes, and shellcheck is clean.
  - size: M

**Verification gate:** `tools/ci/remote/build.sh fast --tests --no-appimage` and `tools/ci/remote/build.sh fast --tests --no-appimage --cmake-arg -DNATRON_GPU=OFF` both pass all suites, with `StagedImageEffectGpuTest` green in both and `CurveSnapshotPrecision*` (canary included) green on lavapipe. `checks.yml` lint-ci passes with the new fma lint, and a dispatched Nightly run shows the GPU-off job green.
