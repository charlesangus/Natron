# GUI benchmark baseline

Two gate recordings, both made on 2026-11-12 with `run.sh --preset gate` (comp topology at N = 300 and 3000, adaptive
reps, per-rep timeouts of 2-30 s), `Natron-m118-large-graph-gui-responsiveness.AppImage` (sha256 2dc2f095258091b7, build
4aebea572). The Xvfb run used harness 5fdfc5707; the host run used harness c9a9a6e89. They follow M118 - Large Graph GUI
Responsiveness, so no op wedges Natron any more: every op runs at both sizes, and only paste_1000 at N=3000 on Xvfb is
censored. The pre-M118 gate recordings (a8651e8bc, `Natron-m117-gui-perf-harness.AppImage`) are in `baselines/archive/` as
`gate-pre-m118-*.jsonl`, with the older pre-gate recordings (120 s and 300 s censored timeouts), kept for reference, not
for gating. The before and after comparison is at the end of this file.

How the baselines are used:

- Milestones touching the bench-rule paths (GUI or Engine) compare one Xvfb gate run against `baselines/xvfb.jsonl`:
  `tools/bench/gui/run.sh --xvfb --preset gate --baseline tools/bench/gui/baselines/xvfb.jsonl`, then
  `compare.py baselines/xvfb.jsonl RUN.jsonl`. `--baseline` takes each op's timeout from the baseline's medians.
  `CONFIRM` rows are re-run with the printed `run.sh --rerun ... --reps 5` line before they count.
- The full preset (four topologies at 300, 1000 and 3000) runs periodically, or when a `--scaling` slope from the
  gate's two sizes looks suspect.
- Budgets (16 ms per event for interaction, 100 ms for edits) are judged only on a host run, with `compare.py --budgets`.
- `over_budget` cells are censored: `censored_ms` is how far the rep got before its timeout, a lower bound. Over on both
  sides is "still over" and judged neither way.

## Xvfb

`baselines/xvfb.jsonl`: single gate run, 2026-11-12. Host: Intel N100, 4 cores, Xvfb with the xcb platform (natron-dev
container), harness 5fdfc5707. Preset gate, comp topology, N = 300 and 3000, seed 1.

Noise, measured on the pre-M118 build (the numbers describe the harness): the pre-M118 gate run and two more gate runs of the same AppImage (same day, same host) give the spread below, (max - min) /
min over the three, for the ops that were `ok` in all three. Wall and per-event CPU (`step_cpu_median_ms`) are equally
noisy, so wall stays judged. Run 2 was also a quiet period: at N=300 six ops that were over budget in the other two
were `ok` in it.

| Metric | Spread, N=300 | Spread, N=3000 |
|---|---|---|
| rep median | 12-5700% (fit_all, select_all, click_select 260-440%) | 11-150% |
| per-event p50 | 10-16000% (absolute 5-140 ms) | 19-93% |
| per-event p95 | 10-24000% (absolute 5-100 ms) | 17-104% |
| per-event CPU median | 8-14800% (absolute 3-120 ms) | 22-60% |
| longest stall, `ok` ops | up to 2.0 s (load) | up to 6.2 s (frame_step) |

Tolerance 3.0 (a 4x slowdown), minimum change 200 ms, long stall 3000 ms, chosen by `compare.py` for any `xvfb`
file: the largest same-build forward comparison was +239% (rubber_band at N=300, 1 rep of 40 events, 1.1 s then 3.8 s),
and the small ops swing by large ratios over tens of milliseconds, which the 200 ms floor ignores. 1.0 or 2.0 still flags
rubber_band; 0.5 also flags fit_all at N=3000. Host mode keeps 0.2 / 5 ms / 250 ms. With these, the three same-build
pairs report 0 regressions.

Median is the per-rep time of the whole event sequence; per-event max is the worst single event. "over (x s)" is a
censored cell, how far it got before its timeout.

| Op | median 300 / 3000 (ms) | per-event max 300 / 3000 (ms) | Reps 300 / 3000 | Status 300 / 3000 |
|---|---|---|---|---|
| click_select | 19.4 / 61.5 | 26.1 / 1.0 | 5 / 5 | ok / ok |
| connect | 21.6 / 155.4 | 10.6 / 100.5 | 5 / 5 | ok / ok |
| create_one | 33.2 / 237.4 | 4.2 / 26.6 | 5 / 5 | ok / ok |
| delete_1000 | 73.9 / 457.8 | 63.5 / 339.6 | 5 / 2 | ok / ok |
| drag_move_1 | 687.2 / 1702.9 | 24.2 / 47.5 | 1 / 1 | ok / ok |
| drag_move_50 | 1242.4 / 2977.8 | 29.2 / 64.2 | 1 / 1 | ok / ok |
| duplicate_100 | 437.6 / 795.8 | 421.3 / 548.7 | 5 / 3 | ok / ok |
| edit_label | 13.1 / 50.6 | 5.0 / 5.6 | 5 / 5 | ok / ok |
| fit_all | 14.0 / 44.2 | 15.5 / 55.5 | 5 / 5 | ok / ok |
| frame_step | 419.2 / 4018.4 | 33.5 / 433.5 | 1 / 1 | ok / ok |
| hover | 1360.2 / 2691.4 | 22.0 / 34.6 | 1 / 1 | ok / ok |
| idle | 5173.6 / 5250.1 | 0.1 / 0.1 | 1 / 1 | ok / ok |
| knob_change_fanout | 29.4 / 63.9 | 7.2 / 19.5 | 5 / 5 | ok / ok |
| load | 1825.7 / 20923.3 | 1832.6 / 20922.7 | 2 / 1 | ok / ok |
| pan | 674.5 / 1054.9 | 25.0 / 38.8 | 1 / 1 | ok / ok |
| paste_100 | 445.1 / 843.9 | 398.5 / 801.5 | 5 / 3 | ok / ok |
| paste_1000 | 629.0 / over (6.2 s) | 587.3 / over (6.2 s) | 4 / - | ok / over_budget |
| redo | 49.9 / 392.8 | 24.4 / 239.8 | 5 / 2 | ok / ok |
| rubber_band | 306.3 / 579.4 | 24.9 / 58.7 | 1 / 1 | ok / ok |
| select_all | 31.0 / 252.9 | 34.2 / 236.3 | 5 / 5 | ok / ok |
| set_expression | 14.1 / 46.7 | 1.7 / 3.8 | 5 / 5 | ok / ok |
| undo | 66.7 / 537.6 | 37.4 / 324.1 | 5 / 2 | ok / ok |
| zoom_step | 536.0 / 864.3 | 39.7 / 63.6 | 1 / 1 | ok / ok |

Scaling (`compare.py --scaling baselines/xvfb.jsonl`, N=300 and 3000; all 22 ops that completed at both sizes): load 1.06,
frame_step 0.98, select_all 0.91, undo 0.91, redo 0.90, connect 0.86, create_one 0.85, delete_1000 0.79, edit_label 0.59,
set_expression 0.52, click_select 0.50, fit_all 0.50, drag_move_1 0.39, drag_move_50 0.38, knob_change_fanout 0.34, hover 0.30,
paste_100 0.28, rubber_band 0.28, duplicate_100 0.26, zoom_step 0.21, pan 0.19, idle 0.01. No op is superlinear.
Medians are step wall time. paste_1000 at N=3000 is `over_budget` (censored, 6.2 s reached) and so has no slope.

## Real host

`baselines/host-ms-E2087B87C9C8.jsonl`: single gate run, 2026-11-12, same preset as above, harness c9a9a6e89.

- Host: ms-E2087B87C9C8, AMD Ryzen 7 9800X3D (16 threads), AMD Radeon RX 7900 XTX, real display, xcb. The CPU was a
  7800X3D when the pre-M118 host baseline was recorded; the hostname and GPU are unchanged. Only the before.jsonl of the
  same sitting is comparable to older host runs.
- Per-host baseline: compare it only against runs from the same machine.

| Op | median 300 / 3000 (ms) | per-event max 300 / 3000 (ms) | Reps 300 / 3000 | Status 300 / 3000 |
|---|---|---|---|---|
| click_select | 16.7 / 38.0 | 0.2 / 7.3 | 5 / 5 | ok / ok |
| connect | 8.3 / 43.1 | 2.8 / 30.6 | 5 / 5 | ok / ok |
| create_one | 16.5 / 90.8 | 1.6 / 4.9 | 5 / 5 | ok / ok |
| delete_1000 | 16.7 / 153.7 | 18.1 / 84.0 | 5 / 5 | ok / ok |
| drag_move_1 | 325.9 / 368.3 | 0.2 / 6.7 | 1 / 1 | ok / ok |
| drag_move_50 | 641.6 / 369.3 | 8.4 / 4.0 | 1 / 1 | ok / ok |
| duplicate_100 | 148.1 / 265.4 | 172.2 / 174.7 | 5 / 5 | ok / ok |
| edit_label | 8.3 / 11.9 | 0.9 / 1.4 | 5 / 5 | ok / ok |
| fit_all | 8.3 / 9.0 | 2.8 / 11.4 | 5 / 5 | ok / ok |
| frame_step | 143.5 / 1612.8 | 19.6 / 175.1 | 1 / 1 | ok / ok |
| hover | 823.3 / 822.3 | 1.5 / 4.0 | 1 / 1 | ok / ok |
| idle | 4849.4 / 5250.2 | 0.0 / 0.0 | 1 / 1 | ok / ok |
| knob_change_fanout | 10.3 / 19.1 | 3.4 / 7.6 | 5 / 5 | ok / ok |
| load | 594.6 / 6972.6 | 605.0 / 6941.2 | 2 / 1 | ok / ok |
| pan | 316.9 / 322.7 | 2.9 / 5.8 | 1 / 1 | ok / ok |
| paste_100 | 150.1 / 265.5 | 145.0 / 142.3 | 5 / 5 | ok / ok |
| paste_1000 | 220.4 / 1761.0 | 214.1 / 1574.3 | 5 / 2 | ok / ok |
| redo | 10.2 / 127.6 | 6.5 / 63.4 | 5 / 5 | ok / ok |
| rubber_band | 174.2 / 193.2 | 1.3 / 16.4 | 1 / 1 | ok / ok |
| select_all | 8.0 / 87.2 | 7.1 / 84.2 | 5 / 5 | ok / ok |
| set_expression | 16.7 / 16.9 | 0.5 / 0.8 | 5 / 5 | ok / ok |
| undo | 16.1 / 201.6 | 15.5 / 122.0 | 5 / 5 | ok / ok |
| zoom_step | 165.5 / 221.4 | 2.6 / 19.9 | 1 / 1 | ok / ok |

Scaling (`compare.py --scaling baselines/host-ms-E2087B87C9C8.jsonl`, N=300 and 3000; all 23 ops): undo 1.10, redo 1.10,
load 1.07, frame_step 1.05, select_all 1.04, delete_1000 0.96, paste_1000 0.90, create_one 0.74, connect 0.72, click_select
0.36, knob_change_fanout 0.27, duplicate_100 0.25, paste_100 0.25, edit_label 0.15, zoom_step 0.13, drag_move_1 0.05,
rubber_band 0.04, fit_all 0.03, idle 0.03, pan 0.01, set_expression 0.00, hover 0.00, drag_move_50 -0.24. Slopes above 1
(undo, redo, load, frame_step, select_all) sit at or just over the linear line; `compare.py` reports 0 superlinear ops.
Step medians on the host are quantised to the 8 ms frame, so slopes of the small ops (set_expression, hover, pan) are noise.

## M118 before and after

M118 - Large Graph GUI Responsiveness, comp topology, N=3000, rep medians in ms. Xvfb compares the pre-M118 gate
baseline (`archive/gate-pre-m118-xvfb.jsonl`) with the new run (`compare.py` reports 0 regressions). Host compares
`before.jsonl` (build of the old base, `Natron-m118-base-0553d5c3a.AppImage`) with `after.jsonl`, both from the 2026-11-12
sitting on the Ryzen 7 9800X3D. "over (x s)" is censored and "not run" means Natron had wedged on an earlier op.

| Op | Xvfb before -> after | Host before -> after |
|---|---|---|
| click_select | 535.2 -> 61.5 | 46.8 -> 38.0 |
| connect | 489.1 -> 155.4 | 147.3 -> 43.1 |
| create_one | 584.0 -> 237.4 | 177.5 -> 90.8 |
| delete_1000 | not run -> 457.8 | not run -> 153.7 |
| drag_move_1 | over (3.9 s) -> 1702.9 | over (3.2 s) -> 368.3 |
| drag_move_50 | over (4.3 s) -> 2977.8 | over (3.4 s) -> 369.3 |
| duplicate_100 | over (12.1 s) -> 795.8 | over (12.2 s) -> 265.5 |
| edit_label | 364.4 -> 50.6 | 23.8 -> 11.9 |
| fit_all | 402.1 -> 44.2 | 7.2 -> 9.0 |
| frame_step | 27753.5 -> 4018.4 | 4542.1 -> 1612.8 |
| hover | over (16.1 s) -> 2691.4 | 14657.7 -> 822.3 |
| knob_change_fanout | 1058.0 -> 63.9 | 161.6 -> 19.1 |
| load | over (262.0 s) -> 20923.3 | not run -> 6972.6 |
| pan | over (4.4 s) -> 1054.9 | over (3.4 s) -> 322.7 |
| paste_100 | not run -> 843.9 | not run -> 265.5 |
| paste_1000 | not run -> over (6.2 s) | not run -> 1761.0 |
| redo | not run -> 392.8 | not run -> 127.6 |
| rubber_band | over (3.7 s) -> 579.4 | 1001.4 -> 193.2 |
| select_all | 714.8 -> 252.9 | 121.8 -> 87.2 |
| set_expression | 581.2 -> 46.7 | 81.2 -> 16.9 |
| undo | over (12.1 s at N=300) -> 537.6 | not run -> 201.7 |
| zoom_step | over (2.6 s) -> 864.3 | over (2.1 s) -> 221.4 |

The host comparison reports 3 regressions, all at N=300: click_select (8.7 -> 16.7 ms), fit_all (0.4 -> 8.3 ms) and
set_expression (8.3 -> 16.7 ms). They are frame quantisation of the step wall time (8 ms to 16 ms); per-event CPU is
lower after than before.

### Host budgets

`compare.py --budgets baselines/host-ms-E2087B87C9C8.jsonl` (N=3000): 15 of 20 ops pass. The user waived the five that
fail on 2026-11-12, as they stand, to revisit if real use needs it. They are, in ms per event against the budget:

| Op | p50 | Budget |
|---|---|---|
| select_all | 68.6 | 16 |
| undo | 108 | 100 |
| duplicate_100 | 142 | 100 |
| paste_100 | 140 | 100 |
| paste_1000 | 1545 | 100 |
