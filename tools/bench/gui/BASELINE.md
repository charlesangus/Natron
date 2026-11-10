# GUI benchmark baseline

Two baselines: Xvfb on an Intel N100 (below) and a real host ("Real host" section at the end).

## Xvfb

Provisional. The file is built from two runs, not three, and most cells have one sample. See "Samples".

Host: Intel N100, 4 cores, Xvfb :79 with the xcb platform, release AppImage at git 4abfe234a. Other agents
share the container, so some cells carry contention noise.

## Samples

Each run is the full matrix of four topologies at N = 300, 1000, 2000 and 3000, 3 reps per op. `baselines/xvfb.jsonl`
is `merge.py` over run 1 (all 16 cells) and run 2 (chain, wide, comp, mixed at N=300 and chain at N=1000: 5 cells).
Cells outside run 2 are single samples. A full baseline needs run 2 finished and a run 3: about 11 more cell runs for
run 2 (about 2 h) and a full run 3 (about 4 h), in a quiet window. Runs that overlapped another agent's Natron or
NatronRenderer process were discarded and redone; cc1plus syntax checks from other agents were not discarded.

Ops left out of the matrix: delete_1000, undo and redo hang from about 60 nodes (20 deletes pass, 30 hang), so they are
not in the baseline. paste_1000 is also left out. Both are for M118.

## Run-to-run spread

Spread is (max - min) / median of the per-run medians, over the 5 cells with two samples (95 op rows).

| Class | Median spread | 90th percentile | Worst |
|---|---|---|---|
| interaction | 5.5% | 49% | 98% (fit_all chain 300) |
| edit | 6.2% | 37% | 115% (create_one chain 300) |

Outliers (above 50%) are all in run 2 chain-300 or mixed-300 paste_100, which ran while another agent's jobs were active.
The typical op is within about 6%. With two samples the 90th percentile is itself noisy.

## Compare tolerance

`compare.py baselines/xvfb.jsonl RUN --tol 1.0 --min-ms 5`. This is a 100% change, which is under the 2x-worst-spread rule
(that would be over 200%, and meaningless). It is the smallest round value at which both runs pass against the merged
baseline. Treat it as catching only gross regressions (2x) until a third run tightens it. Verified: run 1 against the
baseline exits 0; run 2 against the baseline restricted to its 5 cells exits 0 (tol 0.5 gives 1 regression; compare.py
reports cells absent from a partial run as regressions, so run 2 cannot be compared against the full file).

## Timeouts

Per-op timeout is 120 s. Ops that timed out are recorded with status timeout and so are excluded from the slopes:

- load: every topology at N=2000 and 3000
- hover: wide, comp, mixed at N=2000 and 3000; chain at 3000
- paste_100: chain, comp, mixed at N=2000 and 3000; wide at 3000
- duplicate_100: chain, comp, mixed at N=2000 and 3000

## Scaling: superlinear ops (slope above 1.2, M118 targets), steepest first

Slope of log(median) on log(N). Entries with only N=300,1000 lost their 2000 and 3000 points to timeouts, so their real slope is at least this.

| Op | Topology | N points | Slope |
|---|---|---|---|
| load | wide | 300,1000 | 1.80 |
| load | chain | 300,1000 | 1.76 |
| load | mixed | 300,1000 | 1.71 |
| duplicate_100 | comp | 300,1000 | 1.71 |
| paste_100 | comp | 300,1000 | 1.69 |
| load | comp | 300,1000 | 1.66 |
| duplicate_100 | chain | 300,1000 | 1.60 |
| edit_label | mixed | 300,1000,2000,3000 | 1.55 |
| duplicate_100 | mixed | 300,1000 | 1.52 |
| set_expression | mixed | 300,1000,2000,3000 | 1.47 |
| paste_100 | mixed | 300,1000 | 1.38 |
| duplicate_100 | wide | 300,1000,2000,3000 | 1.36 |
| knob_change_fanout | mixed | 300,1000,2000,3000 | 1.26 |
| set_expression | comp | 300,1000,2000,3000 | 1.24 |

The full table: `compare.py --scaling baselines/xvfb.jsonl`.

Hover is the slowest interaction at 1000 nodes (24 s median on chain).

## Real host

`baselines/host-ms-E2087B87C9C8.jsonl` is one run, unmerged (a single sample per cell), 2026-11-10.

- Host: ms-E2087B87C9C8, AMD Ryzen 7 9800X3D (16 cores), AMD Radeon AI PRO R9700 (radeonsi/RADV, Mesa 26.2.3), X11 Cinnamon, 62 GB RAM, xcb.
- Build: `Natron-m117-gui-perf-harness.AppImage` (M117 with main merged at 0a1a61cfe), sha256 eb4f4cfb512333ac, harness commit
  9a757f381 (records c48618e85 and later).
- Matrix: mixed and comp at N = 300, 1000 and 3000, 3 reps, 300 s step timeout. `compare.py --scaling baselines/host-ms-E2087B87C9C8.jsonl`
  reproduces the slope table.
- 15 delete-link confirmation dialogs ("... Continue anyway?") were auto-accepted and logged during delete_1000, undo and redo.

### Not measurable on this build

Wedged Natron for over 300 s, recorded as timeouts, larger sizes skipped:

- delete_1000, undo, redo: mixed at N=300 (all N skipped); comp at N=1000 (N=3000 skipped). comp at N=300 completed.
- knob_change_fanout: comp at N=300 (N=1000 and 3000 skipped). Mixed completed.

Timed out at the 120 s limit: paste_1000 and load, mixed and comp, at N=3000.

### Per-op results

Median is the per-rep time of the whole event sequence; per-event max is the worst single event, the figure M118's budgets
apply to. "-" is a timeout or a skipped cell. The budget is 16 ms per event for pan, zoom, hover and drag_move, and 100 ms for
click_select, select_all, rubber_band, set_expression, edit_label and knob_change_fanout; other ops have none. "At largest n"
compares the per-event max at the largest size that ran. pan mixed at N=3000 reads 1.0 ms against 16.6 ms at N=1000 and is
a measurement outlier, not a pass.

| Op | Topology | median 300 / 1000 / 3000 (ms) | per-event max 300 / 1000 / 3000 (ms) | Budget (ms) | At largest n |
|---|---|---|---|---|---|
| click_select | comp | 25.1 / 53.6 / 203.7 | 18.1 / 51.8 / 195.3 | 100 | over (195.3 at n=3000) |
| click_select | mixed | 25.0 / 52.3 / 171.1 | 17.7 / 49.3 / 166.1 | 100 | over (166.1 at n=3000) |
| connect | comp | 16.7 / 17.5 / 153.4 | 3.1 / 12.7 / 45.1 | - | none |
| connect | mixed | 16.7 / 27.3 / 65.9 | 2.0 / 11.0 / 36.2 | - | none |
| create_one | comp | 15.7 / 51.4 / 174.6 | 3.6 / 6.8 / 17.5 | - | none |
| create_one | mixed | 16.2 / 47.6 / 161.1 | 3.5 / 6.5 / 15.8 | - | none |
| delete_1000 | comp | 240.7 / - / - | 227.9 / - / - | - | none |
| drag_move_1 | comp | 658.5 / 1318.1 / 4760.2 | 19.3 / 52.1 / 198.0 | 16 | over (198.0 at n=3000) |
| drag_move_1 | mixed | 657.9 / 1308.7 / 4257.1 | 17.9 / 47.8 / 182.9 | 16 | over (182.9 at n=3000) |
| drag_move_50 | comp | 892.0 / 2747.7 / 6938.9 | 14.4 / 59.5 / 132.3 | 16 | over (132.3 at n=3000) |
| drag_move_50 | mixed | 1166.9 / 2419.1 / 6196.9 | 18.0 / 53.4 / 120.3 | 16 | over (120.3 at n=3000) |
| duplicate_100 | comp | 692.8 / 3803.2 / 25773.6 | 673.8 / 3746.2 / 26037.3 | - | none |
| duplicate_100 | mixed | 651.4 / 3550.0 / 24739.9 | 636.4 / 3506.8 / 24738.7 | - | none |
| edit_label | comp | 16.7 / 16.7 / 26.1 | 1.5 / 1.2 / 2.4 | 100 | under (2.4 at n=3000) |
| edit_label | mixed | 16.7 / 16.7 / 40.0 | 1.1 / 1.3 / 5.4 | 100 | under (5.4 at n=3000) |
| fit_all | comp | 8.3 / 1.5 / 6.4 | 8.3 / 28.6 / 7.1 | - | none |
| fit_all | mixed | 0.4 / 1.5 / 6.4 | 8.0 / 25.6 / 6.7 | - | none |
| frame_step | comp | 99.4 / 113.7 / 184.6 | 17.9 / 80.1 / 396.0 | - | none |
| frame_step | mixed | 84.2 / 83.4 / 174.5 | 19.0 / 84.7 / 384.9 | - | none |
| hover | comp | 857.2 / 2538.9 / 14338.8 | 6.1 / 20.7 / 66.7 | 16 | over (66.7 at n=3000) |
| hover | mixed | 852.9 / 2477.3 / 14029.8 | 6.0 / 22.5 / 68.3 | 16 | over (68.3 at n=3000) |
| idle | comp | 4898.9 / 5055.7 / 4999.4 | 0.0 / 0.0 / 0.0 | - | none |
| idle | mixed | 4999.8 / 5001.0 / 4930.8 | 0.0 / 0.0 / 0.0 | - | none |
| knob_change_fanout | mixed | 13.5 / 36.2 / 136.7 | 5.9 / 19.4 / 80.6 | 100 | under (80.6 at n=3000) |
| load | comp | 914.5 / 7074.6 / - | 921.5 / 7079.7 / - | - | none |
| load | mixed | 953.0 / 7686.5 / - | 956.9 / 7651.9 / - | - | none |
| pan | comp | 633.5 / 1841.6 / 6202.6 | 6.0 / 16.2 / 45.9 | 16 | over (45.9 at n=3000) |
| pan | mixed | 637.7 / 1649.8 / 5644.5 | 6.1 / 16.6 / 1.0 | 16 | under (1.0 at n=3000) |
| paste_100 | comp | 675.1 / 3700.4 / 25293.4 | 654.3 / 3661.4 / 25156.7 | - | none |
| paste_100 | mixed | 640.4 / 3448.0 / 24388.0 | 619.0 / 3395.5 / 24247.4 | - | none |
| paste_1000 | comp | 964.6 / 9841.8 / - | 939.7 / 9771.4 / - | - | none |
| paste_1000 | mixed | 902.1 / 9693.2 / - | 884.1 / 9691.1 / - | - | none |
| redo | comp | 228.4 / - / - | 209.5 / - / - | - | none |
| rubber_band | comp | 179.9 / 312.6 / 1011.5 | 5.7 / 18.1 / 56.0 | 100 | under (56.0 at n=3000) |
| rubber_band | mixed | 180.4 / 304.5 / 990.0 | 5.7 / 17.9 / 53.3 | 100 | under (53.3 at n=3000) |
| select_all | comp | 8.0 / 22.2 / 118.8 | 6.6 / 21.6 / 67.3 | 100 | under (67.3 at n=3000) |
| select_all | mixed | 8.4 / 20.3 / 115.3 | 7.9 / 19.9 / 63.1 | 100 | under (63.1 at n=3000) |
| set_expression | comp | 8.3 / 20.4 / 85.4 | 5.1 / 16.9 / 58.2 | 100 | under (58.2 at n=3000) |
| set_expression | mixed | 8.2 / 20.6 / 81.5 | 4.9 / 17.0 / 56.4 | 100 | under (56.4 at n=3000) |
| undo | comp | 3136.6 / - / - | 3120.7 / - / - | - | none |
| zoom_step | comp | 216.4 / 565.3 / 2095.7 | 12.3 / 48.7 / 174.0 | 16 | over (174.0 at n=3000) |
| zoom_step | mixed | 158.4 / 525.2 / 1787.3 | 11.7 / 46.8 / 157.6 | 16 | over (157.6 at n=3000) |

Over budget at N=3000: pan comp, zoom_step, hover, drag_move_1 and drag_move_50 (both topologies), and click_select (both
topologies, 166 and 195 ms). Under budget: edit_label, knob_change_fanout (mixed), rubber_band, select_all, set_expression, and pan mixed (outlier).

### Scaling

Slope of log(median) on log(N), steepest first; above 1.2 is superlinear. Slopes use N=300, 1000 and 3000 except where noted.

| Op | Topology | N points | Slope |
|---|---|---|---|
| paste_1000 | mixed | 300,1000 | 1.97 |
| paste_1000 | comp | 300,1000 | 1.93 |
| load | mixed | 300,1000 | 1.73 |
| load | comp | 300,1000 | 1.70 |
| paste_100 | mixed | 300,1000,3000 | 1.58 |
| duplicate_100 | mixed | 300,1000,3000 | 1.58 |
| paste_100 | comp | 300,1000,3000 | 1.57 |
| duplicate_100 | comp | 300,1000,3000 | 1.57 |
| fit_all | mixed | 300,1000,3000 | 1.23 |
| hover | comp | 300,1000,3000 | 1.22 |
| hover | mixed | 300,1000,3000 | 1.21 |

Below 1.2: select_all 1.17 and 1.13, zoom_step mixed 1.05, create_one comp 1.04; every other op is at or below 1.01.
