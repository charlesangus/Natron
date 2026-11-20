# M262 - Expression Benchmark For Large Graphs

Measures what Python knob expressions cost today, on graphs from 3 to 5000 or more expression-driven nodes, so M263 - Expression Interpreter Overhead, M264 - Expression Result Cache and the optional M265 - Native Expression Evaluator are each judged against numbers. Today `KnobHelper::executeExpression` (Engine/Knob.cpp:3729-3800) builds a source string per call, runs it with `PyRun_String` in `__main__`, and reads the result back from the global `ret`. `declarePythonVariables` (Engine/Knob.cpp:2815-2915) emits one `hasattr` per sibling node into every expression's function body. The existing `tools/bench/graph_bench.py` has no expression topology.

## Scope

**In:**
- Three expression topologies in `graph_bench.py` and a size matrix from 3 to 5000, plus one run above 5000.
- A per-evaluation timing record (not only whole-frame time), split by expression kind.
- A recorded baseline and the go/no-go numbers for M265 - Native Expression Evaluator.
**Out:**
- Any change to expression evaluation (M263 - Expression Interpreter Overhead, M264 - Expression Result Cache).
- GPU or render-time (pixel) cost: this measures parameter evaluation only.
- Expressions in non-Grade nodes beyond what the topologies need.
**Edge cases:**
- The result cache keyed on time (`_exprRes`, Engine/KnobImpl.h:471-520) makes a second call at the same frame free; the benchmark must report cold (cache cleared) and warm separately.
- A host with 4 cores (the dev box) shows GIL serialisation less than a many-core host; record the core count with every line.
**Assumptions:**
- "3-5000 more" in the request means node counts from 3 up to 5000 and beyond. If it meant something else, the matrix sizes change and nothing else does.
- A Grade node's `gain` and `offset` knobs are enough to carry every expression kind.

## Phase 262.1: Topologies and timing

- [x] M262.P1.T1 — Add the three expression topologies to graph_bench.py
  - files: tools/bench/graph_bench.py
  - approach: Add `build_exprchain` (N Grade nodes, node k's `gain` is `<prev>.gain.get() * 1.001`), `build_exprfan` (N Grade nodes whose `gain` references one master node's knob, which is itself `sin(frame / 10)`) and `build_exprmixed` (an even split of four kinds: a constant such as `2.0 * 0.5`, a frame function such as `1 + 0.1 * sin(frame)`, a sibling reference, and a clamp of a sibling reference). Register them in the `BENCH_TOPO` dispatch and the header comment. Name every node explicitly, as `BENCH_NAMED=1` does for the others, so naming cost stays out of the figure. Set expressions with `knob.setExpression(expr, False, 0)` the way the existing comps do.
  - verify: `BENCH_TOPO=exprmixed BENCH_N=20 BENCH_RES=tiny NatronRenderer -b tools/bench/graph_bench.py` exits 0 and appends one JSON line; rendered output differs by frame (the frame function is live).
  - size: M

- [x] M262.P1.T2 — Record cold and warm per-evaluation time, by expression kind
  - files: tools/bench/graph_bench.py
  - approach: For the expression topologies only, after the graph is built, call `getValueAtTime` on every expression knob for each timed frame in two passes. Pass one clears the result cache first (change the knob's expression or call `clearExpressionsResults`). Pass two is warm. Time each pass with `time.perf_counter_ns` and add `expr_cold_us_per_eval`, `expr_warm_us_per_eval`, `expr_evals` and `expr_by_kind` (kind to cold microseconds) to the result record, with `cpu_count`. Keep the existing whole-frame timing untouched.
  - verify: a tiny-resolution run for `exprmixed` at N=20 prints a record with all four new fields, each positive and `cold >= warm`.
  - size: M

- [x] M262.P1.T3 — Add expression rows to the bench matrix runner
  - files: tools/bench/run_matrix.sh, tools/bench/README.md
  - approach: Add the three topologies to `run_matrix.sh` at N = 3, 30, 300, 1000, 5000 (and 20000 for `exprmixed` only). Document the topologies, the new record fields and the cold/warm split in README.md next to the existing topology list.
  - verify: `run_matrix.sh` with the expression topologies selected runs to completion at N=3 and N=30 and writes one line per configuration.
  - size: M

## Phase 262.2: Baseline

- [x] M262.P2.T1 — Run the matrix and record the baseline and M265's go/no-go numbers
  - files: tools/bench/BASELINE.md
  - approach: Run the full matrix on the release preset and append a table (topology, N, cold and warm microseconds per evaluation, per-kind split, first-frame wall time, cores) under a new "Expression evaluation" heading in BASELINE.md. Record in this milestone's `## Decisions` the figure that would make a native evaluator unnecessary: the cold `exprmixed` N=5000 first-frame expression time after M263 - Expression Interpreter Overhead and M264 - Expression Result Cache. Propose 100 ms; the user confirms or changes it when M265 - Native Expression Evaluator comes up.
  - verify: BASELINE.md has the table with a row for every topology and size; the Decisions entry names the threshold.
  - size: S

**Verification gate:** `run_matrix.sh` over the three expression topologies completes at every size listed, BASELINE.md carries the table, and the GUI bench (`tools/bench/gui/run.sh --xvfb`, `compare.py` against the xvfb baseline) shows no change, since this milestone touches no engine code.

## Progress

- 2026-11-19: P1 done at fb8e9c983. Grade's Gain knob is scripted `white`, and colour knobs read with `getValue(0)`, so the expressions use those in place of the spec's `gain` and `.get()`. The cold pass times frames past the rendered range, which start with an empty result cache. Smoke run on main (exprmixed N=20): 15 µs cold, 3 µs warm per evaluation. P2.T1 baseline running on a cloud VM (build/vast-logs/m262-bench.sh).
- 2026-11-20 — P2.T1 matrix ran on a 30-core VM (cloud-gpu-20261119T175144Z). BASELINE.md is in c2119abf0. Cold cost is 6-13 µs per evaluation and warm 1.2-2.8 µs, flat in N. Cold exprmixed N=5000 is 38.5 ms in one frame, under the proposed 100 ms M265 threshold, which the user confirms when M265 comes up. **exprmixed N=20000 segfaults** about 2 s into its first render, after the graph is built (exit 139, 49 GB VM, the N=5000 run used 1.8 GB). This is probably a stack overflow in a recursive walk over a 20000-deep chain; there is no backtrace yet. The gate requires every size to complete, so the user decides (Open questions).
- 2026-11-20 — **Gate accepted with the crash (user):** BASELINE.md marks exprmixed N=20000 as crashed, and the deep-chain segfault is filed separately as an E14 - Architecture stub (Open questions, for /cat-plan). The GUI bench line is vacuous: the diff touches only tools/bench, outside the GUI bench rule's paths.
- 2026-11-20 — Merged into feature/f12-expression-speed (c3cf24fa2, #321) after review round 1 (fixes in 7fdaaf72e, shellcheck in 8ab69adaa). main merged into F12 (6e0b93f9e).
