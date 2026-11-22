# M506 - Deep Expression Chain Crash

NatronRenderer segfaults (exit 139) about 2 s into the first render of `BENCH_TOPO=exprmixed BENCH_N=20000` (`tools/bench/graph_bench.py`, F12 branch; BASELINE.md "exprmixed 20000 crashed"). N=5000 works. `exprmixed` cycles constant/frame/sibling/clamp, so an expression chain is at most about 2 deep. That points to a recursive per-node graph walk (RoD, frames needed, hash, metadata) rather than expression evaluation. Capture the backtrace, then make the offending walk(s) iterative.

## Scope

**In:**
- Backtrace; iterative rewrite of every walk the crash reaches; a small-stack regression test.

**Out:**
- Expression speed (F12 - Expression Speed's other milestones); O(N²) costs that do not crash.

**Edge cases:**
- More than one recursive walk: fix each walk that overflows at the test depth, not only the first frame in the backtrace.

**Assumptions:**
- glibc threads take their default stack from `ulimit -s`, so a smaller stack reproduces the crash at a small N.
- M506 stays standalone and waits for F12 - Expression Speed to land on `main`: the `exprmixed` topology in `tools/bench/graph_bench.py` exists only on `feature/f12-expression-speed`.

## Phase 506.1: Backtrace

- [ ] M506.P1.T1 — Capture the crash backtrace at a sane size
  - files: read-only; notes in Decisions
  - approach: in natron-dev (release build), first try `ulimit -s 1024` with `BENCH_TOPO=exprmixed BENCH_N=2000 BENCH_RES=tiny BENCH_FRAMES=1`, then plain `BENCH_TOPO=chain` at the same N to split graph from expression. Run under `gdb -batch -ex run -ex "thread apply all bt 40"`; if gdb fails, use `docker exec -u root --privileged natron-dev eu-stack`. Only go to N=20000 at the default stack if memory allows. Each run stays well under 30 min. Name the recursive function(s) and their file:line.
  - verify: a Decisions entry with the repeating frames, the topology/N/stack that reproduces it, and whether `chain` also crashes
  - size: L

## Phase 506.2: Iterative walks

- [ ] M506.P2.T1 — Make the walk(s) P1.T1 named iterative
  - files: per P1.T1 (likely `Engine/EffectInstance*.cpp`, `Engine/Node*.cpp`, `Engine/RenderGraph*`)
  - approach: an explicit stack/worklist with the same visit order and memoisation; results must stay bit-identical.
  - verify: the P1.T1 repro at `ulimit -s 1024` completes; existing ctest passes at the phase build
  - size: L
- [ ] M506.P2.T2 — Small-stack deep-chain regression test
  - files: `Tests/` (new `DeepChainRender_Test.cpp`, registered in `Tests/CMakeLists.txt`)
  - approach: build about 3000 Grade nodes, using the `CreateNodeArgsPropNodeInitialName` path to skip auto-naming, each with an `exprmixed`-style expression. Render one tiny frame on a `QThread` with `setStackSize(512 KiB)` and check the pixel value. Old gtest rules (no `GTEST_SKIP`, no `ASSERT_TRUE(ptr)`).
  - verify: crashes with P2.T1 reverted (scratch build); passes with it
  - size: M

**Verification gate:** P2.T2 passes; `exprmixed` N=20000 completes on a host with the memory for it (recorded in BASELINE.md); if `Engine/Knob*` was touched, the GUI bench rule; full debug PR CI green.
