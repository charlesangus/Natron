# M117 - GUI Perf Harness And Baseline

Stand up a repeatable GUI benchmark for large node graphs and record its first baseline before any fix moves the numbers. It does three things. A main-thread stall monitor shows when the UI hangs. A seeded generator builds 300–3000+ node graphs with heavy expressions, labels, backdrops and groups inside the running app. A driver times node graph operations against those graphs and compares them with a stored per-host baseline. M118 - Large Graph GUI Responsiveness does the fixing. This milestone only measures, and gives every later GUI- or Engine-touching milestone a regression check.

The engine already has `tools/bench/` (NatronRenderer, render only, `BASELINE.md`), but nothing measures the GUI. `Tests/gui/` scripts (`guitest.py`, run by `Tests/gui/run-gui-test.sh` under a GLX-capable Xvfb against `build/release/App/Natron`) already drive the real app with synthetic events. `m55_node_graph.py` is the template for driving `NodeGraph`. The GuiTests binary has no full `Gui`/`NodeGraph`, so the benchmark must drive the real app.

Two kinds of numbers, never mixed:
- **Xvfb (dev container).** Directionally right but much slower than a real display. It's used only to catch regressions against its own baseline.
- **Real host.** The stripped release AppImage on the host's own display, run by the user on whatever machine they test on. Only these numbers are judged against M118's budgets.

## Scope

**In:**
- An event-loop stall monitor, in every build, off by default, enabled by `NATRON_STALL_LOG=<path>`, and zero-cost when off.
- A seeded in-app graph generator: chain, wide, diamond-comp and mixed topologies at N = 300/1000/3000/5000. About 20% of knobs carry expressions, there are multi-line HTML labels, and backdrops and groups.
- A bench driver timing interaction ops (load, fit, zoom, pan, hover, select, rubber-band, drag-move) and edit ops (create, paste, delete, undo/redo, set expression, label edit, knob change with expression dependents, frame step, idle cost).
- A runner with `--xvfb` and `--host` modes, a compare script with a CI-run self-test, and a README.
- A committed Xvfb baseline with measured noise, and the user's real-host baseline.
- A per-op scaling exponent that ranks superlinear ops as M118's target list.

**Out:**
- Any performance fix (M118 - Large Graph GUI Responsiveness).
- A CI timing gate: GitHub runners are too noisy. Only the compare script's deterministic self-test runs in CI.
- Committed `.ntp` fixtures: graphs are generated in-process, so M16 - Project Format Redesign can't break them.
- Curve editor, dope sheet and properties-bin performance.

**Edge cases:**
- Project load is measured by saving each generated graph to a temp file in the current format and reloading it in the same run.
- Generation uses explicit node names, so setup doesn't pay naming costs. Naming is timed separately by the create and paste ops, which use default names.
- An op that doesn't finish within its per-op timeout is recorded as `timeout` rather than aborting the run, so one hang doesn't hide the rest.
- The first-launch update dialog blocks scripts on a fresh HOME. The runner pre-seeds `checkForUpdates=false`.

**Assumptions:**
- Native core nodes (M67/M73) are present in the release build, so fixtures don't depend on the OFX bundles.

## Phase 117.1: Instrumentation

- [x] M117.P1.T1 — Add a main-thread stall monitor, off by default
  - files: `Gui/StallMonitor.h` (new), `Gui/StallMonitor.cpp` (new), `Gui/GuiApplicationManager.cpp`, `Gui/CMakeLists.txt` (or the Gui sources list), `Tests/StallMonitor_Test.cpp` (new) + `Tests/CMakeLists.txt` GuiTests sources
  - approach: when `NATRON_STALL_LOG=<path>` is set at startup (`NATRON_STALL_THRESHOLD_MS` overrides the 50 ms default), start a watcher thread and a 10 ms main-thread `QTimer` that stamps an atomic heartbeat. When the heartbeat is older than the threshold, the watcher opens a stall. When it advances again, it appends one JSON line: `{start_ms, duration_ms}`. `start_ms` is on `CLOCK_MONOTONIC` (`std::chrono::steady_clock` on Linux), the same clock as Python's `time.monotonic()`, so the bench attributes stalls to ops by its own timestamps and needs no Python binding. When the variable is unset, nothing is created.
  - verify: a GuiTests gtest starts the monitor with a temp path, blocks the main thread for 200 ms, spins the event loop, and checks that exactly one stall of 180 ms or more was written, with `start_ms` within the blocked window. With the variable unset, no thread exists.
  - size: M
- [x] M117.P1.T2 — Seeded large-graph generator that runs inside the app
  - files: `tools/bench/gui/gen_graph.py` (new), `tools/bench/gui/test_gen_graph.py` (new)
  - approach: `build(app, topology, n, seed, expr_frac=0.2, labels=True, backdrops=True, groups=True)` creates N native processing nodes (Grade, Blur, Merge, Transform, ColorCorrect) through `app.createNode` with explicit names. Topologies: `chain`; `wide` (one source fanning into N); `comp` (diamond merges, the shape that went exponential in M62); `mixed` (comp plus wide branches). It places nodes on a grid sized like a real comp. About `expr_frac` of the numeric knobs get expressions that reference another node's knob (`Blur12.size.get() * 0.5`) or `frame`. Every tenth node gets a 3–5 line HTML label. A backdrop wraps every 50 nodes, and about 5% of nodes sit inside 3–4 groups. It is deterministic: the same seed gives the same names, connections and expressions. It returns a summary dict (node count, edge count, expression count).
  - verify: `test_gen_graph.py` runs under NatronRenderer (`NatronRenderer -t`) for each topology at N=300. It checks node/edge/expression counts and that two runs with one seed produce identical summaries plus a hash of (name, inputs, expressions).
  - size: M
- [x] M117.P1.T3 — Bench driver: interaction ops
  - files: `tools/bench/gui/gui_bench.py` (new)
  - approach: built on `Tests/gui/guitest.py`'s generator `run()`. For each (topology, N) it generates the graph, saves it to a temp `.ntp`, reloads it, and times `load`. It then times `fit_all`, `zoom_step` (10 wheel steps in and out), `pan` (a 40-event middle-drag), `hover` (200 idle mouse moves), `click_select`, `select_all`, `rubber_band` (a 40-event drag over about 10% of the nodes), and `drag_move` with 1 and 50 nodes selected. Interaction ops force a synchronous `viewport().repaint()` per event, so paint cost lands inside the timing. Each op records its `time.monotonic()` start and end, runs R reps (default 5), and reports median, p95 and per-event max. The stalls in the stall log that fall inside the op's window are attributed to it (count, max, total). Output is one JSON line per op, headed by a host record: CPU model, cores, Qt platform, git SHA, build type, and AppImage or not. A per-op timeout (default 120 s) records `timeout` and moves on.
  - verify: runs under `Tests/gui/run-gui-test.sh` at N=300 `mixed` in the container. Every op emits a record with no `timeout`. A deliberately injected `time.sleep(0.3)` op, behind a `--self-check` flag, is attributed one stall.
  - size: M
- [x] M117.P1.T4 — Bench driver: edit ops
  - files: `tools/bench/gui/gui_bench.py`
  - approach: add ops to the same driver. `create_one` (default name, so naming is timed), `paste_100` and `paste_1000` (copy and paste a selection), `duplicate_100`, `delete_1000`, `undo` and `redo` of the delete, `set_expression` (on a knob in the middle of the graph), `edit_label` (rewrite a 5-line HTML label), `knob_change_fanout` (change a knob that 50 expressions reference), `connect` (rewire one input mid-graph, which triggers the invalid-expression recheck), `frame_step` (10 timeline steps), and `idle` (process CPU time over 5 s with no input, which catches the 300 ms whole-graph timer). Each op restores the graph state for the next rep, by undo or regeneration.
  - verify: as P1.T3, at N=300 `mixed`. Every edit op emits a record, the node count after the run equals the count before it, and `idle` reports a non-zero CPU time.
  - size: M
- [x] M117.P1.T5 — Runner, compare script and README
  - files: `tools/bench/gui/run.sh` (new), `tools/bench/gui/compare.py` (new), `tools/bench/gui/README.md` (new), `Tests/CMakeLists.txt` (self-test registration)
  - approach: `run.sh --xvfb|--host [--sizes 300,1000,3000] [--topologies ...] [--reps R] [--out DIR]`:
    - `--xvfb` reuses `run-gui-test.sh`'s Xvfb/GL setup against `build/release`, in the container, launched detached with a done marker, one run at a time.
    - `--host` runs a given AppImage on the current display.
    - Both pre-seed `checkForUpdates=false`, set `NATRON_STALL_LOG`, and write `build/bench/gui/<UTC>-<hostname>-<mode>.jsonl`.
    - `compare.py BASELINE RUN [--tol 0.2]` reports per-op median ratios, flags any op slower than the tolerance, any new `timeout`, and any stall over 250 ms not in the baseline. It exits non-zero on a flag.
    - `compare.py --scaling RUN` fits a log-log slope of median against N per (op, topology). It ranks ops by slope, marking anything above 1.2 as superlinear.
    - `--self-test` checks synthetic inputs with known answers. It is registered as ctest `GuiBenchCompareSelfTest`, the same way `RenderProfileReportSelfTest` is.
    - The README covers both modes, their meaning (Xvfb for regressions only, host for budgets), and how to re-baseline.
  - verify: `ctest -R GuiBenchCompareSelfTest` passes, and `shellcheck run.sh` is clean (CI's checks.yml already shellchecks `tools/bench`). Running `compare.py` on two runs from P1.T3/T4 prints a table and exits 0.
  - size: M

## Phase 117.2: Baseline

- [ ] M117.P2.T1 — Record the Xvfb baseline and its noise
  - files: `tools/bench/gui/baselines/xvfb.jsonl` (new), `tools/bench/gui/BASELINE.md` (new)
  - approach: on a release build at this milestone's HEAD, in the container with nothing else running, run all four topologies at N=300/1000/3000, three times. If an op times out at 3000, record that, and add a 2000 point for it so a slope still exists. Commit the median-of-runs file as the baseline. In `BASELINE.md`, record per-op run-to-run spread, the compare tolerance chosen from it (at least twice the worst spread for that op class), the `--scaling` table, and the ranked superlinear list. That list is M118's target list.
  - verify: `compare.py baselines/xvfb.jsonl <each of the three runs>` exits 0 with the chosen tolerance. `BASELINE.md` names every op with a slope above 1.2.
  - size: M
- [x] M117.P2.T2 — Package the AppImage and collect the user's real-host baseline
  - files: `tools/bench/gui/baselines/host-<hostname>.jsonl` (new, the user's run), `tools/bench/gui/BASELINE.md`
  - approach: run `package.sh` for the stripped release AppImage. Hand the user the AppImage and the command `tools/bench/gui/run.sh --host --appimage <path>` at N=300/1000/3000 `mixed` and `comp`. Commit the file they return and add its summary to `BASELINE.md`, next to M118's budgets with each op marked over or under.
  - verify: the user returns a run file; `compare.py --scaling` on it succeeds; `BASELINE.md` shows the host table.
  - size: S

- [x] M117.P2.T3 — Remove the "default appearance changed" startup prompt and keep benches free of modal dialogs
  - files: `Gui/GuiAppInstance.cpp` (~398-405), `Engine/Settings.{h,cpp}` (`isDefaultAppearanceOutdated`/`restoreDefaultAppearance` if unused after), `tools/bench/gui/run.sh`, `tools/bench/gui/gui_bench.py`
  - approach: delete the upstream "The default appearance of Natron changed since last version" question at startup (user, 2026-11-10: "should probably be removed"; no backward compatibility in this fork) and any settings code only it uses. In the bench, pre-seed first-run flags in the throwaway HOME, and have the driver dismiss any modal dialog that appears during an op, logging its text in the results.
  - verify: a fresh-HOME launch shows no appearance prompt; a host bench run completes without manual clicks.
  - size: M

## Phase 117.3: Minutes-long gate bench

The user (2026-11-10): benches take hours; get them down to minutes. The consultant's design follows. The default **gate** preset runs comp at 300 and 3000 nodes, with cached graphs, 1 rep and adaptive repeats, and per-op timeouts derived from budgets or the baseline, clamped to 2–30 s. Wedged ops are recorded as censored `over_budget` with no long wait. The occasional **full** preset covers four topologies at 300, 1000 and 3000. Xvfb stays the milestone gate; the host is only for M118-style budget checks.

- [x] M117.P3.T1 — Graph cache, budget-derived timeouts, a timed delete/undo/redo cycle, and per-event stats
  - files: `tools/bench/gui/gui_bench.py`
  - approach:
    - `GUI_BENCH_GRAPH_CACHE` with `.ntpz` files keyed `<topo>-<n>-s<seed>-<gen_graph sha>`, plus a summary sidecar. A miss generates the graph and saves it; a hit loads it. The first load is timed as `load`, and restarts always load.
    - An `OP_BUDGET_MS` table and `GUI_BENCH_BASELINE`. Each op gets its own timeout and guard: `clamp(5 × max(budget×events, baseline median), 2 s, 30 s)`. Status `over_budget` carries `censored_ms`.
    - undo/redo go not_run after delete_1000 wedges.
    - delete, undo and redo merge into one timed cycle per rep, keeping the three op names.
    - `GUI_BENCH_REPS=auto` repeats single-event ops until 5 samples or 3 s.
    - Emit `step_wall_p50/p90/p95` and a rounded sample list.
  - verify: `py_compile`; one Xvfb gate run on the M117 AppImage in natron-dev finishes in minutes and records over_budget instead of 120 s timeouts.
  - size: L
- [x] M117.P3.T2 — `run.sh` presets, compare.py censored and confirm handling, run-bench.sh and docs
  - files: `tools/bench/gui/run.sh`, `tools/bench/gui/compare.py`, `tools/bench/gui/README.md`, `tools/bench/gui/BASELINE.md`, `build/appimages/run-bench.sh` (untracked)
  - approach:
    - `run.sh --preset gate|full` (default gate) sets the matrix and the cache, baseline and timeout variables. The fallback op timeout drops to 30 s. Add `--rerun OPS --reps 5`.
    - compare.py: censored values; a "confirm" verdict when there are fewer than 5 samples, printing the re-run command; and a `--budgets` host mode at 16/100 ms. Extend the self-test.
    - run-bench.sh: m117 and m117xvfb run one gate run each, the m118 cap drops to 300 s, and m319xvfb inherits the preset.
    - Docs: the bench rule becomes the gate at every milestone gate and the full preset periodically.
  - verify: shellcheck 0.10 clean; `compare.py --self-test`; a gate run's compare prints sensible verdicts.
  - size: M
- [x] M117.P3.T3 — Re-record the baselines in gate mode
  - files: `tools/bench/gui/baselines/` (archive the old files), `BASELINE.md`
  - approach: one Xvfb gate run in natron-dev, a few minutes on the dev host. One host gate run on the GPU host, about 5 min, by the user. The old jsonl files move to `baselines/archive/`.
  - verify: compare against itself is clean, and BASELINE.md tables are regenerated.
  - size: M

**Verification gate:**
- One build. Full ctest is green, including `StallMonitor` and `GuiBenchCompareSelfTest`, and CI is green.
- `test_gen_graph.py` passes under NatronRenderer.
- The Xvfb baseline and its noise are committed, along with the superlinear target list.
- The user's real-host baseline is committed.
- The stall monitor is off by default: no thread when `NATRON_STALL_LOG` is unset (code inspection plus the gtest).

## Decisions

- 2026-11-02 — Audit: P1 and P2.T1 are committed and pushed (4abfe234a, 3c2182152). P2.T1's baseline is still the provisional one (run 1 full, run 2 partial, no run 3), so its verify (compare.py against three runs) isn't met yet. P2.T2 waits on the user's real-host run. No build/ctest gate is recorded. The branch is 29 commits behind main.
- 2026-10-28 — Provisional Xvfb baseline (run 1 full, run 2 partial; median spread ~6%, tol 1.0). Full baseline needs run 2 finished + run 3 in a quiet window (~6 h). Findings for M118: load/duplicate_100/paste_100 superlinear (slopes 1.4–1.8) and time out at N≥2000; hover times out at 2000–3000; **delete_1000 and undo/redo hang from ~60 nodes** (20 deletes pass, 30 hang) — a real hang, not slowness.
- 2026-10-13 — Regression checks use local timings only, with no CI timing gate (user): GitHub runners are too noisy for wall-clock numbers. The bench runs at the gates of GUI/Engine-touching milestones per the board rule. The full rule is in `DECISIONS/2026-10-13-gui-bench-gate-rule.md`.
- 2026-10-13 — Two kinds of numbers (user). Xvfb in the container is directionally right but much slower, so it's used only to catch regressions against its own baseline. Budgets are judged on the real host the user tests on, which the milestone does not name.
- 2026-10-13 — Fixtures are synthetic and built in-process (user), with no committed `.ntp`, so M16 - Project Format Redesign can't break them. Load is measured by save-and-reload within a run.
- 2026-10-13 — Split from the fixes (user): the harness and baseline ship first, so the baseline is recorded before any fix changes it. M118 - Large Graph GUI Responsiveness does the fixes.
- 2026-10-13 — The stall monitor ships in every build, off by default and zero-cost when off (user agreed to the recommendation).
- 2026-11-10 — Main merged in (0a1a61cfe; main already carried M117's first harness commit through M224, plus a roto driver, both kept). Release gate green: run 42769498480, full ctest including StallMonitor and GuiBenchCompareSelfTest. The real-host baseline runs through the GPU-host session harness (user): `run-bench.sh` gained `BENCHES=m117` (sizes 300,1000,3000; mixed and comp; 3 reps; capped at 90 min) and `SHUTDOWN=1`, which powers the host off only after the copy-back succeeds. The AppImage and steps are in `build/appimages/Natron-m117-gui-perf-harness.{AppImage,md}`. This sandbox can't resolve `maxsettings`, so the user runs `push-bench.sh`.
- 2026-11-10 — Second GPU-host run (`results-20261110T151416Z`) got through mixed 300 and 1000 before exiting 3 at a hung `delete_1000`. Click-select, drag and undo ops errored, and the user had to click through modal popups: "The default appearance changed" and link-deletion confirmations. The harness fixes from M118 are being ported; P2.T3 removes the appearance prompt (user) and makes the bench dismiss dialogs.
- 2026-11-10 — P2.T3: both upstream startup prompts are removed, the appearance one and the matching shortcuts-reset one, with their settings bookkeeping (9a757f381). The bench keeps its version seeds, because the baseline AppImage predates this removal.
- 2026-11-10 — P2.T2: the real-host baseline is committed (640bc5efa, `baselines/host-ms-E2087B87C9C8.jsonl`). It is the GPU-host run `results-20261110T160016Z`: AI PRO R9700, 62 GB, a different GPU and RAM from the same hostname's earlier runs. It covers mixed and comp at 300, 1000 and 3000. delete_1000, undo, redo and comp's knob_change_fanout wedge on this pre-M118 build, and paste_1000 and load time out at 3000; all are recorded as not measurable. compare.py --scaling flags 11 superlinear ops. The gate's build is running (run 42849400659).
- 2026-11-10 — PR #256 review round 1. The Xvfb baseline predates the current harness (OFX IDs, different graph hashes, older op definitions) and P2.T1's three-run verify was never met, so P2.T1 is unticked and the file is marked stale for regression use. The harness's wedged guard was per op rather than per step, and click_select and drag_move_1 timed their framing work. Both are being fixed, which makes the host baseline's affected rows provisional until a re-run.
- 2026-11-10 — The user found the benches far too slow: about 3 h per Xvfb run, about 80 min per host run, about 45 min for M118. Two-thirds of that is 120 s timeouts plus restarts that regenerate the graph. Phase 117.3 adds a gate preset expected to take about 2–3 min healthy and 6–10 min on a pre-M118 build, and a full preset of about 15 min. Xvfb stays the gate and the host is only for budget checks (consistent with the no-GPU-host-timing-gates rule). The old baselines are censored and pre-M118, so they are archived rather than gated on.
- 2026-11-10 — P3.T1: an Xvfb smoke of comp-300 in natron-dev took about 2 min, against about 10 min before. The cached load is 4.8 s, the undo wedge is caught at 12 s, and slow ops are recorded as over_budget. Interaction events take 100–150 ms each on Xvfb, so Xvfb timeouts need the gate baseline.
- 2026-11-11 — P3.T2, T3 done (21a499e1f, a8651e8bc, 9212a1b76). Gate runs in practice: Xvfb on the dev host about 9 min per side (the 262 s load at 3000 on an N100 dominates); GPU host session for m117+m118 7 min. Baselines are gate recordings of the pre-M118 build: Xvfb dev host N100 and host 7800X3D/RX 7900 XTX; old files archived. Review round 2 (P3 diff) running.
