# M244 - NVIDIA Test Fixes

Runs of M168 - NVIDIA Viewer Hand-off Validation and M169 - Pipelined Strip Member Timing on bosley-faux (RTX 3070 Ti, NVIDIA GL and Vulkan) left a set of test and tooling findings that passed lavapipe and CI. This milestone gathers them so the NVIDIA host runs green: a GUI test that cannot tell an unpainted viewer from a picture, a GPU timing test that flaked once in ten runs, a subnormal-sensitive precision test, a scheduler test that fails consistently on the host, and two host-setup traps that look like code failures.

It is written against the post-M239 code (branch `milestone/m239-weight-based-gpu-placement`, not yet merged). M239 - Weight-Based GPU Placement deletes the measured cost model, so nothing here touches `GpuCostModel`. The file paths below exist on both main and that branch unless noted.

## Scope

**In:**
- `Tests/gui/m107_viewer_handoff.py`: "the viewer shows the comp" uses `looks_blank`, so an unpainted viewer fails. The UAT docstring and the header of `Tests/gui/run-gui-test.sh` say a host-display run needs a CPU reference taken on the same display. `tools/gpu-spike/desktop-handoff-check.sh` stops warning "GL renderer is not radeonsi" on NVIDIA.
- `GpuResidencyTest.RegionMembersAreTimedOnTheDevice`: the Blur-outweighs-Grade check is robust against a first-region cold start.
- `CurveSnapshotPrecisionTest.CurveMulKeepsTheProductError`: the guard excludes inputs whose Dekker partial products are subnormal.
- `RenderSchedulerTest.CachedOutputsOfTwoFramesAtOnce`: diagnosed, then fixed, on bosley-faux.
- A preflight that fails clearly on Git LFS pointer fixtures, and documentation of the host-setup traps.

**Out:**
- Spike-robust GPU calibration of `GpuCostModel`'s EMA (suggested by M169). VOID: M239 - Weight-Based GPU Placement deletes the measured cost model, the EMA and `observeGpu`.
- The knob double-to-float overflow audit, the flush-aware comparison helper in `Tests/Gpu/Tolerance.h` and `NativeGpuParity.h`, and the regression sweep. M240 - Float Trap And Denormal Audit owns them. This milestone's curve test fix is local to `CurveSnapshotPrecision_Test.cpp` and must not add a shared helper; if M240 later moves the test onto its helper, that is M240's change.
- The unpainted startup window itself (M243 - Startup Pane Paint On NVIDIA).
- Changing the Dekker kernels or the pipelined-strip engine. Flushing subnormals is accepted hardware behaviour and the timing code is correct.
- Platforms other than Linux x86-64.

**Edge cases:**
- `looks_blank` samples the middle quarter of the image at a stride and needs 8 distinct byte values, so a flat-colour comp would trip it. The comp in this test is a gradient-bearing generated image, so it does not. P1.T1 confirms on the Xvfb run.
- A weight or warm-up for the timing test must not change what the test measures: members' device time is per member and the two add up to the region's compute. The warm-up region is not counted in the region-record assertion (`3 * kTimedRuns`).
- The curve-test fix must keep lavapipe's coverage: pairs whose partial products are normal still get the exact check.
- `CachedOutputsOfTwoFramesAtOnce` fails on bosley-faux with and without the GPU and passes on CI, so the cause is host-dependent (core count, timing, memory) and not GPU code. The fix may be to the test or to the scheduler, and which is decided by P3.T1.

**Assumptions:**
- Unverified: `CachedOutputsOfTwoFramesAtOnce` on bosley-faux fails the same way after the host-setup traps are fixed. It could be a stale-plug-in or LFS symptom; P3.T1 reruns it first.
- Unverified: the failing curve pairs are exactly the `BelowTiny` pairs `8e-31 x [0.5, 1]`. The test on main already has a `kExactFloor` (FLT_MIN * 2^48) that routes products below about 3.3e-24 to a loose bound of 4 * FLT_MIN, and the inbox report's numbers (`|hi+lo-exact|` 5.06e-39 against a limit 3.07e-44) fit the exact branch's `2^-44` relative limit and not the loose one. P2.T1 first reproduces on NVIDIA to find which branch fails before changing anything.
- Unverified: the timing test's spike was cold-start cost in the first region of the process (Grade 0.260 ms against Blur 0.209 ms, no transfer overlap). Main and the M239 branch already take the minimum of three timed runs for each member, so a single spike is already filtered; the failure may need a warm-up run or a more robust comparison.

## Phase 244.1: Hand-off GUI test and spike script

- [ ] M244.P1.T1 — The comp check fails on an unpainted viewer
  - files: `Tests/gui/m107_viewer_handoff.py`
  - approach: in the "wire the comp" step, replace the predicate `(gt.viewer_centre_colour() or (0, 0, 0)) != (0, 0, 0)` with one that grabs the viewer the way `shot_of_viewer` / the later cases do and requires `not looks_blank(img)`. `looks_blank` is defined in the same function (about line 181); use it where it is in scope, moving the definition up if the order needs it. Keep the same timeout and the `require(ok, "the viewer shows the comp")` message.
  - verify: under Xvfb (`Tests/gui/run-gui-test.sh Tests/gui/m107_viewer_handoff.py`, CPU reference run) the case still passes. Temporarily forcing the predicate's image to a flat #202020 makes it fail; do not commit that.
  - size: M

- [ ] M244.P1.T2 — Document the same-display CPU reference rule
  - files: `Tests/gui/m107_viewer_handoff.py` (docstring only), `Tests/gui/run-gui-test.sh` (header comment only)
  - approach: in the UAT docstring, after the sentence that says every run but the CPU reference compares with `$NATRON_GUI_TEST_OUT/cpu/`, add that a host-display run (`GUI_TEST_HOST_DISPLAY`) must compare with a CPU reference taken on the same display, because the display's colour handling and GL differ from Xvfb's. In the `run-gui-test.sh` header paragraph on `GUI_TEST_HOST_DISPLAY`, add one sentence saying the same.
  - verify: `bash -n Tests/gui/run-gui-test.sh` and `python3 -I -m py_compile Tests/gui/m107_viewer_handoff.py` succeed; the diff is comments only.
  - size: S

- [ ] M244.P1.T3 — Do not warn about radeonsi on NVIDIA
  - files: `tools/gpu-spike/desktop-handoff-check.sh`
  - approach: the script prints "GL renderer is not radeonsi" for any other renderer. Make the warning name the renderer it found and only warn about radeonsi when the renderer string contains neither `NVIDIA` nor `radeonsi`; for NVIDIA print an info line saying the NVIDIA renderer was found. Keep the exit status unchanged.
  - verify: `bash -n` passes; run the script with a stubbed `glxinfo` on PATH that prints `OpenGL renderer string: NVIDIA GeForce RTX 3070 Ti/PCIe/SSE2`, and with one that prints `llvmpipe`: the first has no warning, the second still warns.
  - size: M

## Phase 244.2: GPU test robustness

- [ ] M244.P2.T1 — Make the member-timing order check robust
  - files: `Tests/GpuResidency_Test.cpp`
  - approach: in `RegionMembersAreTimedOnTheDevice` (about line 1907 on main, 2051 on the M239 branch), run one untimed warm-up of the Blur and Grade region before the mode loop, so the first region of the process does not carry pipeline creation and first-use costs into the minimum, and keep `EXPECT_GT(minBlurNs, minGradeNs)` on the minimum of `kTimedRuns` runs. Make the warm-up not count in the profile: it runs before `setProfilePath`, so `EXPECT_EQ(3 * kTimedRuns, regionRecords)` is unchanged. Also raise the Blur radius or the frame size if needed so the Blur's device time is at least about 3x the Grade's on NVIDIA (the recorded failure had 0.209 ms against 0.260 ms, so the kernels are close at 512 px and sigma 60). Change the doc comment to say why the warm-up is there. Leave the 5 % sum check for whole and serial modes and the nesting check for pipelined mode as they are.
  - verify: the build passes; on lavapipe `ctest -R GpuResidency` passes; on bosley-faux the test passes 30 runs in a row (`--gtest_filter=GpuResidencyTest.RegionMembersAreTimedOnTheDevice --gtest_repeat=30`), in both schedulers.
  - size: M

- [ ] M244.P2.T2 — Reproduce the curve-product failure on NVIDIA and name the failing branch
  - files: `Tests/Gpu/CurveSnapshotPrecision_Test.cpp` (a temporary diagnostic print, not committed)
  - approach: on bosley-faux run `ctest -R CurveSnapshotPrecision -V` against main's test. For each failing pair print a, b, p, `|hi + lo - exact|`, which branch (exact at or above `kExactFloor`, loose below) failed and the three Dekker partial products' magnitudes (compute `ah*bh`, `ah*bl + al*bh`, `al*bl` on the host in double). Record in `## Decisions` the branch that fails, the smallest partial product that is subnormal, and the threshold that separates passing from failing pairs. Remove the print.
  - verify: the Decisions entry gives the failing branch and threshold with the excerpt of the failing output; the working tree has no diagnostic code.
  - size: M

- [ ] M244.P2.T3 — Exclude subnormal-partial-product inputs from the exact check
  - files: `Tests/Gpu/CurveSnapshotPrecision_Test.cpp`
  - approach: apply the P2.T2 finding. Add a local predicate `partialProductsRepresentable(a, b)` that splits both operands with the Veltkamp split the kernel uses (`x * 4097`), computes `al*bl` (the smallest partial product) and the cross terms in double, and is false when any nonzero one is below FLT_MIN. Use it in place of, or in addition to, the `|p| >= kExactFloor` guard so that pairs whose partial products a flushing device drops go to the loose branch (the bound `4 * kFloatMin` on the error and on `|hi + lo - exact|`) and the rest keep the exact check. If P2.T2 shows the loose branch itself fails, widen its bound to the flushed partial products' total and say so in the comment. Do not add a shared helper (M240 owns that). Keep `exactChecked > pairs.size() / 2` true, and keep `Category::BelowTiny` in the tested categories.
  - verify: the build passes; `ctest -R CurveSnapshotPrecision` passes on lavapipe (including `FusedErrorTermCanary`, which must still see the fused bug there) and on bosley-faux (NVIDIA).
  - size: M

## Phase 244.3: Scheduler test and host setup

- [ ] M244.P3.T1 — Diagnose CachedOutputsOfTwoFramesAtOnce on bosley-faux
  - files: `Tests/RenderScheduler_Test.cpp` (temporary diagnostics, not committed), this milestone file (`## Decisions`)
  - approach: first fix the host traps with P3.T3's checks in place (LFS fixtures, `build/assets`) and rerun the test, because a stale plug-in could be the cause. If it still fails, run `--gtest_filter=RenderSchedulerTest.CachedOutputsOfTwoFramesAtOnce` with `NATRON_GPU=off` under both schedulers, with `--gtest_repeat=20`, and print `CountingTestRegistry` counts per leaf and per pool size (1, 4, 16) to see which leaves render twice and in which frame. Compare the host's core count and pool defaults with CI. The test expects each caching node to render once across two frames that share cached outputs (`sharesCachedOutput`); leaves rendering twice means the second frame's task did not wait on, or did not find, the first frame's cached image. Check whether `appPTR->clearAllCaches()` keeping referenced images, the cache size limit on a host with a different memory setting, or the cache's hash for the leaf values explains it. Record the cause in `## Decisions`.
  - verify: the Decisions entry names the cause with the counts that show it, and says whether the fix is in the test or in the scheduler or cache.
  - size: L

- [ ] M244.P3.T2 — Fix the cause of the double render
  - files: set by the P3.T1 finding, one of `Tests/RenderScheduler_Test.cpp`, `Engine/RenderScheduler.cpp`, `Engine/RenderSchedulerTaskGraph.cpp` or the image-cache code. The brief is rewritten with the real list before this task runs (PLAN-FORMAT section 5a).
  - approach: fix what P3.T1 found. If the test is host-dependent (for example it assumes a thread count or a cache limit), make it set what it assumes. If the scheduler or cache is wrong, fix the engine and keep the test unchanged, adding a case if the existing one cannot isolate it.
  - verify: `ctest -R RenderScheduler` passes on bosley-faux under both schedulers and with `--gtest_repeat=20` for this case, and still passes in the dev container.
  - size: L

- [ ] M244.P3.T3 — Preflight for LFS pointer fixtures and stale plug-ins, and document host setup
  - files: `tools/ci/local/test.sh`, `tools/ci/local/README.md`, `Tests/fixtures/raw/README.md`
  - approach: `test.sh` already runs `fetch-assets.sh --check-pins` before ctest, which refuses stale `build/assets/Plugins`; add a second preflight beside it for ctest only. For each file under `Tests/fixtures/raw/` that `.gitattributes` marks `filter=lfs`, read its first line; if it starts with `version https://git-lfs.github.com/spec`, print the file names and "run `git lfs install && git lfs pull`" and exit 1. A host without `git lfs` on PATH gets that named in the message. Allow `NATRON_TEST_SKIP_LFS_CHECK=1` for runs that skip the raw suites. In the README's Prerequisites section list git-lfs as a host requirement, say what the `--check-pins` failure means and that `fetch-assets.sh` fixes it, and add the same pointer to the "Two gotchas" section. In the raw fixtures README add the preflight's name next to the existing sentence about pointer files.
  - verify: `bash -n tools/ci/local/test.sh`; with a pointer file substituted for a fixture in a scratch copy, `test.sh ctest release` exits 1 with the message before starting ctest, and with real files it proceeds. The README diff mentions both the LFS and the pins checks.
  - size: M

**Verification gate:** remote `fast --tests` CI green and the release gate passes (`ctest` in the dev container under both schedulers). On bosley-faux (RTX 3070 Ti) with git-lfs installed and `build/assets` re-fetched: `ctest -L gpu` passes in full, `CurveSnapshotPrecisionTest`, `GpuResidencyTest.RegionMembersAreTimedOnTheDevice` (30 repeats) and `RenderSchedulerTest.CachedOutputsOfTwoFramesAtOnce` pass, and `GUI_TEST_HOST_DISPLAY=:0 Tests/gui/run-gui-test.sh Tests/gui/m107_viewer_handoff.py` passes against a CPU reference taken on the same display. Run `ctest` there with `NATRON_GPU=force` for both schedulers, and rerun on lavapipe CI to confirm the curve test still catches the fused bug.
