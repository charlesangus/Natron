# GUI benchmark

Times node graph interaction and edit operations on generated large graphs inside the Natron
GUI, and records main-thread stalls while they run. A second driver times roto and paint
interaction: pen-to-pixel latency of brush strokes and the cost of shape edits (see
[Roto driver](#roto-driver)).

| File | Role |
|---|---|
| `gen_graph.py` | Builds a seeded graph (`chain`, `wide`, `comp`, `mixed`) of N processing nodes with groups, backdrops, labels and about 20% expression-driven knobs. `test_gen_graph.py` checks it under `NatronRenderer -t`. |
| `gui_bench.py` | The node graph driver. Runs inside the GUI and writes one JSON line per op. |
| `roto_bench.py` | The roto driver (`run.sh --driver roto`). Same record format, roto and paint ops. |
| `run.sh` | Launches a driver under Xvfb in the container, or from an AppImage on your own display. |
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

## Roto driver

```
tools/bench/gui/run.sh --xvfb --driver roto --reps 5
tools/bench/gui/run.sh --host --driver roto --appimage Natron-x86_64.AppImage
```

The result is `<UTC>-<host>-<mode>-roto.jsonl`; its directory also holds `latency.jsonl`, the
latency probe's log. `run.sh` sets `NATRON_ROTO_LATENCY_LOG` for this driver only, and passes
every `ROTO_BENCH_*` variable into the container. `--sizes`, `--topologies` and `--self-check`
don't apply.

| Op | Topology, n | What it does |
|---|---|---|
| `stroke_<B>px` | `roto-2k`/`roto-4k`, upstream nodes | A paced brush stroke on a RotoPaint node of brush size B (5, 50, 300 image pixels), with no input or with a 300-node `comp` graph upstream. |
| `pen_up` | as above | A short 50 px stroke; times the release, which waits for the stroke's final render on the main thread. |
| `drag_point`, `drag_tangent`, `drag_feather`, `transform_box` | `roto-shapes`, shapes | Drags the top point, its right tangent, its feather bar or the top-left handle of the transform box of one shape on a Roto node holding 1, 30 or 100 shapes of 24 points keyed on 200 frames. Each drag is undone through the Edit menu and checked. |
| `hover` | `roto-shapes`, shapes | The cursor crosses the viewer over every shape with no button down. |

`median_ms` and `p95_ms` hold the op's headline cost, which is what `compare.py` diffs; the
record's `headline` field names it:

- `latency` (stroke ops): pen-to-pixel time, from the input event to the end of the viewer paint
  that first showed the point, over every stroke point of every rep;
- `values` (`pen_up`): the release;
- `rep` (shape ops, and stroke ops without the probe): the per-rep wall time. Each event forces a
  synchronous viewer repaint, so overlay cost lands in the event that caused it.

Every record also carries `rep_median_ms`, `latency_p50_ms`/`latency_p95_ms`, `latency_points`,
`latency_stages_ms` (the median time between successive probe stages), and
`overlay_p50_ms`/`overlay_p95_ms` (timed synchronous viewer repaints: during the drag for shape
ops, over the finished stroke for stroke ops). Shape drags add no stroke points, so their latency
fields are empty. `setup` records give the time to build the upstream graph and the shapes.

Strokes are paced at `ROTO_BENCH_PEN_INTERVAL_MS` (default 8 ms, a 125 Hz tablet). After each
stroke the driver waits until the latency log stops growing, then deletes the stroke. Other
knobs: `ROTO_BENCH_BRUSHES`, `ROTO_BENCH_FORMATS`, `ROTO_BENCH_UPSTREAM`, `ROTO_BENCH_SHAPES`,
`ROTO_BENCH_POINTS`, `ROTO_BENCH_KEYS`, `ROTO_BENCH_STROKE_EVENTS`, `ROTO_BENCH_DRAG_EVENTS`.
`--ops stroke` selects every stroke op.

### The latency probe

When `NATRON_ROTO_LATENCY_LOG` names a file, Natron appends one line per stroke point once a
viewer paint has shown it:

```
{"seq": 12, "event_ms": ..., "append_ms": ..., "request_ms": ..., "render_start_ms": ...,
 "render_end_ms": ..., "upload_ms": ..., "paint_ms": ...}
```

The stamps are milliseconds on `steady_clock`, the clock of the stall log and of Python's
`time.monotonic()`: the mouse or tablet event, the point joining the stroke, the viewer render
being requested, starting and finishing, the frame's texture upload, and the end of the viewer's
`paintGL`. A render covers every point added before it was requested. A stage a point never went
through is `null`. Without the variable the probe costs one branch per stage.

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
