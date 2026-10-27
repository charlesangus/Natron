# GUI benchmark

Times node graph interaction and edit operations on generated large graphs inside the Natron
GUI, and records main-thread stalls while they run.

| File | Role |
|---|---|
| `gen_graph.py` | Builds a seeded graph (`chain`, `wide`, `comp`, `mixed`) of N processing nodes with groups, backdrops, labels and about 20% expression-driven knobs. `test_gen_graph.py` checks it under `NatronRenderer -t`. |
| `gui_bench.py` | The driver. Runs inside the GUI and writes one JSON line per op. |
| `run.sh` | Launches the driver under Xvfb in the container, or from an AppImage on your own display. |
| `compare.py` | Compares two runs, or fits how each op scales with N. |
| `baselines/` | Committed reference runs. |

## The two modes

**`--xvfb`: regressions only.** This runs `build/release/App/Natron`, or `--appimage PATH`, in the
natron-dev container under Xvfb with software GL (llvmpipe). Paint costs there are nothing like a
real GPU's, and the container shares the machine with whatever else is running. So the numbers
mean something only next to another `--xvfb` run from the same machine. Use it to catch a change
that makes an op slower or makes it stall, not to judge whether the GUI feels fast.

```
tools/bench/gui/run.sh --xvfb --sizes 300,1000,3000 --topologies chain,wide,comp,mixed --reps 5
```

It starts detached and prints the result path. When it finishes, the exit code is written to
`<result>.done`. Wait on that file; don't poll for the process. Only one `--xvfb` run happens at
a time: a second one waits on `build/bench/gui/.xvfb.lock`. Add `--wait` to run in the
foreground. The AppImage path must be visible inside the container (for example under `build/`).

**`--host`: budgets.** This runs a packaged AppImage (`tools/ci/local/package.sh`) directly on
`$DISPLAY`, with no container, so it measures a real machine with its own GPU and window manager.
These are the numbers to hold against interaction budgets. It needs no Docker and no PySide6 on
the host.

```
tools/bench/gui/run.sh --host --appimage Natron-x86_64.AppImage --sizes 300,1000,3000 --topologies mixed,comp
```

Natron takes over the screen while it runs. Leave the mouse and keyboard alone.

Both modes run Natron under a throwaway HOME seeded with `checkForUpdates=false`, so your
settings are neither read nor changed. Each run writes:

- `build/bench/gui/<UTC>-<host>-<mode>.jsonl`, the results;
- `build/bench/gui/<UTC>-<host>-<mode>/`, holding `natron.log`, `results.txt` and
  `stalls.jsonl`.

The other options are `--ops` (a subset of ops), `--reps`, `--out` and `--self-check`.
`--self-check` adds an op that blocks the main thread for 300 ms. It fails unless exactly one
stall is attributed to it, which checks the stall monitor and the attribution end to end.
`GUI_BENCH_OP_TIMEOUT_S` (default 120) is the per-op budget. `GUI_BENCH_TIMEOUT` (default 6 h)
caps the whole run.

## What a record holds

The first line is a `host` record: CPU, cores, Qt platform, git SHA, build type and the AppImage.
Each graph then gets a `graph` record (its counts and content hash) followed by one `op` record
per op. An `op` record holds:

- `median_ms`, `p95_ms`, `min_ms`, `max_ms` of the per-rep wall time;
- `per_event_max_ms`, the longest single event;
- `stalls`: the count, max and total of the stalls whose midpoint falls inside a timed rep;
- `status`: `ok`, `timeout` or `error`.

An op runs its events one at a time on the Qt event loop, so timers and deferred paints land
between events, as they would for a user. Interaction events force a synchronous viewport
repaint. Setup and restore steps are not timed.

A stall is any stretch where the main thread doesn't service a 10 ms heartbeat timer for over
50 ms (`NATRON_STALL_THRESHOLD_MS`). The stall monitor runs only when `NATRON_STALL_LOG` names a
file, and `run.sh` sets it.

The per-op timeout is checked between events. A single event that hangs is caught only by the
whole-run timeout.

Ops whose count is larger than the graph are capped at N/2: at N=300, `paste_1000` and
`delete_1000` move 150 nodes. The record's `count` field gives the real figure.

## Comparing

```
tools/bench/gui/compare.py tools/bench/gui/baselines/xvfb.jsonl build/bench/gui/<run>.jsonl --tol 0.2
tools/bench/gui/compare.py --scaling build/bench/gui/<run>.jsonl
```

Comparison flags an op when any of the following holds, and then exits 1:

- its median grew by more than `--tol` and by more than `--min-ms` (default 5 ms);
- its status fell from `ok`;
- it is missing from the run;
- it stalled for more than `--long-stall-ms` (default 250 ms) where the baseline did not.

`--scaling` fits the log-log slope of the median against N for each (op, topology) and ranks them
steepest first. A slope above 1.2 is marked superlinear: that op's cost grows faster than the
graph does.

`compare.py --self-test` runs as ctest `GuiBenchCompareSelfTest`.

## Re-baselining

Re-baseline after a deliberate performance change lands, or when the container image or CI
machine changes. Don't re-baseline to make a regression go away.

1. Build release at the commit to be the baseline, and leave the machine otherwise idle.
2. Run the full matrix three times with `--xvfb --wait`.
3. Check that each run against the others passes `compare.py` at the chosen tolerance. The spread
   between them sets the tolerance: use at least twice the worst spread for that kind of op.
4. Commit the median-of-runs file as `baselines/xvfb.jsonl`. Record the spread, the tolerance and
   the `--scaling` table in `BASELINE.md`.

Host baselines are per machine: `baselines/host-<hostname>.jsonl`. Compare them only against
runs from the same machine.
