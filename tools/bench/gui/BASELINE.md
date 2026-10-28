# GUI benchmark baseline (Xvfb)

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
