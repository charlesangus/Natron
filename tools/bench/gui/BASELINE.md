# GUI benchmark baseline

Two gate recordings, both made on 2026-11-11 with `run.sh --preset gate` (comp topology at N = 300 and 3000, adaptive
reps, per-rep timeouts of 2-30 s), harness commit a8651e8bc, `Natron-m117-gui-perf-harness.AppImage` (sha256
eb4f4cfb512333ac). This build predates the large-graph responsiveness fixes, so its wedges are part of the baseline: undo wedges Natron at N=300 (redo
and paste_1000 are then not run) and duplicate_100 wedges Natron at N=3000 (every later op at that size is not run).
The pre-gate recordings with 120 s and 300 s censored timeouts are in `baselines/archive/`, kept for reference, not for gating.

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

`baselines/xvfb.jsonl`: single gate run, 2026-11-11. Host: Intel N100, 4 cores, Xvfb with the xcb platform (natron-dev
container), harness a8651e8bc. Preset gate, comp topology, N = 300 and 3000, seed 1. Run-to-run spread has not been
measured for this file; judge changes with `CONFIRM` re-runs.

Median is the per-rep time of the whole event sequence; per-event max is the worst single event. "over (x s)" is a
censored cell, how far it got before its timeout.

| Op | median 300 / 3000 (ms) | per-event max 300 / 3000 (ms) | Reps 300 / 3000 | Status 300 / 3000 |
|---|---|---|---|---|
| click_select | 112.7 / 535.2 | 63.1 / 293.5 | 5 / 2 | ok / ok |
| connect | 73.7 / 489.1 | 24.8 / 189.6 | 5 / 4 | ok / ok |
| create_one | 127.1 / 584.0 | 25.4 / 97.2 | 5 / 5 | ok / ok |
| delete_1000 | 1648.0 / not run | 1548.6 / not run | 1 / - | ok / not_run |
| drag_move_1 | over (3.3 s) / over (3.9 s) | over (3.3 s) / over (3.9 s) | - / - | over_budget / over_budget |
| drag_move_50 | over (3.3 s) / over (4.3 s) | over (3.3 s) / over (4.3 s) | - / - | over_budget / over_budget |
| duplicate_100 | over (3.8 s) / over (12.1 s) | over (3.8 s) / over (12.1 s) | - / - | over_budget / over_budget |
| edit_label | 37.0 / 364.4 | 8.5 / 17.7 | 5 / 5 | ok / ok |
| fit_all | 94.4 / 402.1 | 94.2 / 470.3 | 5 / 5 | ok / ok |
| frame_step | 1238.9 / 27753.5 | 104.6 / 3050.3 | 1 / 1 | ok / ok |
| hover | over (16.0 s) / over (16.1 s) | over (16.0 s) / over (16.1 s) | - / - | over_budget / over_budget |
| idle | 4995.9 / 5511.3 | 0.0 / 0.0 | 1 / 1 | ok / ok |
| knob_change_fanout | 93.1 / 1058.0 | 50.6 / 669.4 | 5 / 3 | ok / ok |
| load | 4648.8 / over (262.0 s) | 4647.8 / over (262.0 s) | 1 / - | ok / over_budget |
| pan | over (3.4 s) / over (4.4 s) | over (3.4 s) / over (4.4 s) | - / - | over_budget / over_budget |
| paste_100 | over (3.5 s) / not run | over (3.5 s) / not run | - / - | over_budget / not_run |
| paste_1000 | not run / not run | not run / not run | - / - | not_run / not_run |
| redo | not run / not run | not run / not run | - / - | not_run / not_run |
| rubber_band | 1123.1 / over (3.7 s) | 50.7 / over (3.7 s) | 1 / - | ok / over_budget |
| select_all | 144.7 / 714.8 | 98.4 / 396.9 | 5 / 3 | ok / ok |
| set_expression | 42.5 / 581.2 | 20.8 / 250.4 | 5 / 4 | ok / ok |
| undo | over (12.1 s) / over | over (12.1 s) / over | - / - | over_budget / over_budget |
| zoom_step | over (2.1 s) / over (2.6 s) | over (2.1 s) / over (2.6 s) | - / - | over_budget / over_budget |

Scaling (`compare.py --scaling baselines/xvfb.jsonl`, N=300 and 3000; only ops that completed at both sizes): frame_step
1.35 (superlinear), set_expression 1.14, knob_change_fanout 1.06, edit_label 0.99, connect 0.82, select_all 0.69,
click_select 0.68, create_one 0.66, fit_all 0.63, idle 0.04.

## Real host

`baselines/host-ms-E2087B87C9C8.jsonl`: single gate run, 2026-11-11, same preset and harness as above.

- Host: ms-E2087B87C9C8, AMD Ryzen 7 7800X3D (16 threads), AMD Radeon RX 7900 XTX, real display, xcb.
- Per-host baseline: compare it only against runs from the same machine.

| Op | median 300 / 3000 (ms) | per-event max 300 / 3000 (ms) | Reps 300 / 3000 | Status 300 / 3000 |
|---|---|---|---|---|
| click_select | 8.1 / 62.5 | 8.3 / 54.8 | 5 / 5 | ok / ok |
| connect | 16.7 / 152.7 | 2.8 / 55.7 | 5 / 5 | ok / ok |
| create_one | 20.8 / 205.6 | 3.9 / 20.5 | 5 / 5 | ok / ok |
| delete_1000 | 277.9 / not run | 260.3 / not run | 1 / - | ok / not_run |
| drag_move_1 | 641.8 / over (3.3 s) | 3.3 / over (3.3 s) | 1 / - | ok / over_budget |
| drag_move_50 | 872.7 / over (3.3 s) | 17.2 / over (3.3 s) | 1 / - | ok / over_budget |
| duplicate_100 | 815.6 / over (12.2 s) | 857.9 / over (12.2 s) | 3 / - | ok / over_budget |
| edit_label | 16.7 / 29.3 | 1.3 / 2.4 | 5 / 5 | ok / ok |
| fit_all | 0.6 / 109.2 | 9.5 / 109.1 | 5 / 5 | ok / ok |
| frame_step | 216.6 / 5776.4 | 24.0 / 522.6 | 1 / 1 | ok / ok |
| hover | 915.0 / over (16.1 s) | 7.2 / over (16.1 s) | 1 / - | ok / over_budget |
| idle | 5024.3 / 4964.7 | 0.0 / 0.0 | 1 / 1 | ok / ok |
| knob_change_fanout | 18.4 / 197.5 | 7.8 / 120.5 | 5 / 5 | ok / ok |
| load | 1256.3 / not run | 1249.2 / not run | 1 / - | ok / not_run |
| pan | 688.9 / over (3.4 s) | 7.4 / over (3.4 s) | 1 / - | ok / over_budget |
| paste_100 | 808.7 / not run | 789.8 / not run | 3 / - | ok / not_run |
| paste_1000 | 1157.6 / not run | 1230.0 / not run | 3 / - | ok / not_run |
| redo | not run / not run | not run / not run | - / - | not_run / not_run |
| rubber_band | 179.8 / 1355.9 | 6.5 / 61.6 | 1 / 1 | ok / ok |
| select_all | 8.3 / 149.6 | 6.8 / 77.6 | 5 / 5 | ok / ok |
| set_expression | 8.1 / 98.1 | 6.1 / 67.3 | 5 / 5 | ok / ok |
| undo | over (3.9 s) / not run | over (3.9 s) / not run | - / - | over_budget / not_run |
| zoom_step | 216.6 / over (2.2 s) | 13.5 / over (2.2 s) | 1 / - | ok / over_budget |

Scaling (`compare.py --scaling baselines/host-ms-E2087B87C9C8.jsonl`, N=300 and 3000): fit_all 2.26 (superlinear),
frame_step 1.43 (superlinear), select_all 1.25 (superlinear), set_expression 1.09, knob_change_fanout 1.03,
create_one 1.00, connect 0.96, click_select 0.89, rubber_band 0.88, edit_label 0.24, idle -0.01.
fit_all's 300 median (0.6 ms) is near timer resolution, which inflates its slope.
