# M495 - KeyMix GPU Edge Read Fix

`Rows/EdgePolicyMatrixTest.ReadsPastTheBoxByItsMode/32` ("KeyMix A smaller than B, input over a black box", `Tests/Native/EdgePolicy_Test.cpp`, the GPU-against-CPU check near :915) intermittently got 416 GPU-vs-CPU mismatches, worst 5.79e36, under parallel ctest on M336 - Heavy Scene Acceptance's PR CI (run 44940733888, task-graph leg), and passed on a serial rerun. A value like 5.79e36 is garbage memory, not rounding: the GPU KeyMix path most likely reads texels of A outside A's region that were never written (a pooled or recycled texture not cleared to the edge mode's value), and only a busy pool hands it a dirty texture. Find the read, fix it in the product, and prove it with a repeat under load.

## Scope

**In:**
- Diagnosing which GPU read sees unwritten memory: `StepKeyMix` in `Engine/Gpu/Kernels/routing.slang`, KeyMix's GPU op (`Engine/Nodes/Merge/KeyMix.cpp`, `KeyMixOp::gpu`), and how the GPU path fills an input's out-of-region texels for the edge mode (`Engine/Nodes/docs/edge-policy.md`).
- The product fix, wherever the read is (kernel, edge fill, or texture pool clear).
- A deterministic regression test that hands KeyMix a dirty texture.

**Out:**
- MoonRay and other scene-test flakes seen in the same run (M496 - MoonRay Test Stability).
- Raising tolerances or skipping the row: the mismatch is a real read of uninitialised memory.

**Assumptions:**
- The same dirty-read can affect other two-input GPU ops sharing the edge-fill path; if the diagnosis shows the bug is in the shared fill or pool, the fix lands there and covers them.

## Phase 495.1: Diagnose

- [ ] M495.P1.T1 — Find the GPU read of unwritten texels in KeyMix's A-smaller-than-B case
  - files: read-only: `Engine/Gpu/Kernels/routing.slang` (`StepKeyMix`), `Engine/Nodes/Merge/KeyMix.cpp`, the GPU edge-fill and texture-pool code it calls into (`Engine/Gpu/`), `Tests/Native/EdgePolicy_Test.cpp` (the "KeyMix A smaller than B" row, :714)
  - approach: consultant (opus) traces how A's texture is sized and filled when A's region is smaller than B's and the edge mode is black: is the texture allocated at the output window, and are texels outside A's region written (cleared to 0 or the edge value) before `StepKeyMix` samples them? Check pool reuse: does a recycled texture keep its last contents? Name the exact read and the code that should have written it, and say whether other ops share the path.
  - verify: a written diagnosis naming file:line of the unguarded read and of the missing write, recorded as a milestone decision
  - size: L

## Phase 495.2: Fix

- [ ] M495.P2.T1 — Make the GPU path never read unwritten texels of a smaller input
  - files: per P1.T1 — likely `Engine/Gpu/Kernels/routing.slang`, `Engine/Nodes/Merge/KeyMix.cpp`, or the shared GPU edge-fill/pool code under `Engine/Gpu/`
  - approach: fix at the point P1.T1 named: write the out-of-region texels for the edge mode (or bound the kernel's reads to the input's region and apply the edge mode there), matching the CPU path's result. If the bug is in the shared fill or pool, fix it there rather than in KeyMix.
  - verify: `ctest -R EdgePolicyMatrix` passes at the phase build, GPU and CPU
  - size: M
- [ ] M495.P2.T2 — A regression test that hands the GPU path a dirty texture
  - files: `Tests/Native/EdgePolicy_Test.cpp` (or the GPU pool's test file if P1.T1 puts the bug in the pool)
  - approach: before the KeyMix render, fill the GPU texture pool with textures holding a sentinel (e.g. NaN or 1e30) and release them, so the render reuses dirty memory deterministically; then compare GPU against CPU. Bundled gtest is old: no `GTEST_SKIP`, use `INSTANTIATE_TEST_CASE_P`, no `ASSERT_TRUE(ptr)`.
  - verify: the test fails on the pre-fix code (checked by reverting P2.T1 locally in review reasoning or a scratch build) and passes with it
  - size: M

## Phase 495.3: Gate (PM runs)

- [ ] M495.P3.T1 — Repeat under parallel load
  - files: none
  - approach: `tools/ci/remote/build.sh debug --tests --repeat-test EdgePolicyMatrix:20`, both schedulers
  - verify: 20/20 per scheduler, no GPU-vs-CPU mismatch
  - size: S

**Verification gate:** P3.T1 green on both schedulers; the P2.T2 regression test passes; full debug PR CI green.

## Decisions
- 2026-11-22 — Gate on 7ce580607 (diagnostics only): full debug ctest green on both schedulers (933/933); the EdgePolicyMatrix ×20 repeat timed out (exit 124) in iteration 2 on both schedulers, with no mismatch in the iterations that ran (19 and 36 rows OK). The hang's stacks (build/debug/Testing/repeat/Rows_EdgePolicyMatrixTest/iter-2.stacks) aren't uploaded as artifacts. Next: upload the repeat stacks/logs from remote-build.yml, rerun with fewer parallel copies or a longer per-iteration timeout to tell a hang from a slow debug run. Parked behind F4 (user priority, 2026-11-22).
