# M555 - Dissolve GPU Smaller-Input Mismatch

`NativeEdgeInputsGpuTest.DissolveSmallerInput` failed once on the task-graph leg of run 45179223309 (`Tests/Native/NativeEdgeInputsGpu_Test.cpp`, check ~:270, test at ~:352: 688 mismatches, "Dissolve black: first at float 3920, worst 0.5709780") and passed on the in-job rerun. A 0.57 difference is not rounding: it looks like a read outside the smaller input, or of uninitialised memory, in black edge mode on the GPU path. Possibly the same class as M495 - KeyMix GPU Edge Read Fix, so this starts from that milestone's diagnosis and fix.

## Scope

**In:**
- Deciding whether M495 - KeyMix GPU Edge Read Fix's fix already covers Dissolve; if not, finding and fixing Dissolve's own dirty read.
- A deterministic regression test that hands Dissolve a dirty texture.

**Out:**
- Raising tolerances or disabling the test: the mismatch is a real wrong read.
- KeyMix itself (M495 - KeyMix GPU Edge Read Fix).

**Assumptions:**
- The failure needs a busy GPU texture pool to show, like M495's, so a plain rerun will not reproduce it.

## Phase 555.1: Diagnose

- [ ] M555.P1.T1 — Find the GPU read that makes Dissolve's smaller-input case wrong
  - files: read-only: `Tests/Native/NativeEdgeInputsGpu_Test.cpp` (~:270, ~:352), Dissolve's node and GPU op (`Engine/Nodes/Merge/`), its Slang kernel (`Engine/Gpu/Kernels/`), the GPU edge-fill and texture-pool code (`Engine/Gpu/`), and M495 - KeyMix GPU Edge Read Fix's `## Decisions` and merged fix
  - approach: consultant (opus) checks first whether M495's fix (shared edge fill or pool clear) covers Dissolve's path; if it does, say so and name the test that proves it. Otherwise trace how the smaller input's texture is sized and filled in black edge mode and whether Dissolve samples texels never written, naming the read and the missing write.
  - verify: a written diagnosis with file:line, recorded as a milestone decision
  - size: L

## Phase 555.2: Fix

- [ ] M555.P2.T1 — Make Dissolve's GPU path never read unwritten texels
  - files: per P1.T1 — Dissolve's GPU op or kernel, or the shared GPU edge-fill/pool code under `Engine/Gpu/`
  - approach: fix at the point P1.T1 named, matching the CPU result for the edge mode; prefer the shared fill or pool when the bug lives there. If P1.T1 found M495's fix already covers it, this task is the regression test only (P2.T2) and is struck through.
  - verify: `ctest -R NativeEdgeInputsGpu` passes at the phase build
  - size: M
- [ ] M555.P2.T2 — A regression test that hands Dissolve a dirty texture
  - files: `Tests/Native/NativeEdgeInputsGpu_Test.cpp`
  - approach: before the Dissolve render, fill the GPU texture pool with textures holding a sentinel (NaN or 1e30) and release them so the render reuses dirty memory deterministically, then compare GPU against CPU — the same technique as M495's regression test; reuse its helper if one exists. Bundled gtest is old: no `GTEST_SKIP`, `INSTANTIATE_TEST_CASE_P`, no `ASSERT_TRUE(ptr)`.
  - verify: the test passes with the fix and its reasoning shows it would fail without it
  - size: M

## Phase 555.3: Gate (PM runs)

- [ ] M555.P3.T1 — Repeat under parallel load
  - files: none
  - approach: `tools/ci/remote/build.sh debug --tests --repeat-test NativeEdgeInputsGpu:20`, both schedulers
  - verify: 20/20 per scheduler, no GPU-vs-CPU mismatch
  - size: S

**Verification gate:** P3.T1 green on both schedulers; the P2.T2 regression test passes; full debug PR CI green.

## Decisions
- 2026-11-23 — Standalone milestone after M495 - KeyMix GPU Edge Read Fix (user, via inbox drain): its fix may be the same class, so it goes first.
- 2026-11-23 — Claimed ahead of its Deps at the user's request; works on top of the dependency's branch.
- 2026-11-23 — Diagnosis (opus consultant): no unguarded GPU read found on the KeyMix/Dissolve composite path (bounded reads in pixelwrap.slang/compositestep.slang; working buffers fully written). Dissolve's bad pixel (20,15) lies inside the small input's box, which points at buffer reuse/race, not an edge read. The ×20 repeat timeout looks like overload (≈8 lavapipe debug processes, 300 s ctest TIMEOUT) rather than a hang. Next: pool-poison test option + shared helper used by KeyMix, Merge and Dissolve rows; upload repeat logs/stacks; repeat parallel/timeout knobs. Poisoned failure → crop inputs to region in makeStageGpuInput/makeStageGpuWrap (covers both); poisoned pass → trace strips and RAM-upload path.
