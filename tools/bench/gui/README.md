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
| `merge.py` | Merges repeated runs into a median-of-runs baseline and prints each op's run-to-run spread. |
| `baselines/` | Committed reference runs. |

## Presets and when to run them

`run.sh --preset gate` (the default) is the milestone gate: `comp` at 300 and 3000 nodes, adaptive
reps (`--reps auto`), graphs from the cache, per-rep timeouts of 2–30 s, and no restarts after a
wedge (`--restarts 0`). It takes a few minutes on a healthy build, and stays in minutes on one whose
ops wedge. `--preset full` covers `chain`, `wide`, `comp` and `mixed` at 300, 1000 and 3000 with the
same settings but restarts Natron after every wedge (`--restarts unlimited`), in about 15 minutes on
a healthy build. `--sizes`, `--topologies`, `--reps` and `--restarts` override a preset's choice.
`--preset create` is not a matrix of the standing ops: it runs only the node-creation ops (see
[Node-creation latency](#node-creation-latency)).

The bench rule: run the gate at every milestone gate of a GUI- or Engine-touching milestone,
against the committed gate baseline (`--baseline` sets the timeouts from it, and `compare.py`
judges the result). Run the full preset periodically, or when a `--scaling` slope from the gate's
two sizes looks suspect.

Generated graphs are kept in `build/bench/gui/graph-cache` (`--graph-cache DIR`), keyed by
topology, n, seed, `gen_graph.py`'s hash and the build (the AppImage's sha256, else Natron's version
and the binary's size and mtime), so a repeat run on the same build opens them instead of generating
them. A project saved by one build need not load the same in another, and the sidecar's load times
belong to the build that measured them, so each new build generates its graphs once: about 2 minutes
at 3000 nodes on the GPU host and about 6 on the N100. Comparing two builds therefore generates once
per build. Entries for builds no longer in use are safe to delete. `--fresh-graphs` uses an empty cache in the run's own directory, so every graph is generated
again; use it when generation's own scaling is what is being measured.

`--rerun OPS` re-runs only those ops, with `--reps 5` unless `--reps` is given, to confirm a
suspected regression; `compare.py` prints the exact line when it wants one. With a fixed rep count an
op that goes over budget still runs every rep, so a re-run gathers 5 samples of it; with `--reps auto`
it stops at the first rep that goes over. A cycle step that goes over still ends its rep's cycle,
since the steps after it depend on it, but the next rep starts the cycle again.

## Node-creation latency

`run.sh --preset create` runs four ops on `mixed` at 0, 300 and 3000 nodes (the empty graph is
the generator's single `Source`) and nothing else:

```
tools/bench/gui/run.sh --xvfb --preset create [--stages] [--appimage PATH]
```

The ops run only when named, by the preset or by `--ops`; the `gate` and `full` presets do not
include them.

| Op | What it does |
|---|---|
| `create_by_shortcut` | With a node selected, sends Blur's plugin shortcut (`B`) to the graph. |
| `tab_menu_open` | Sends Tab to the graph with a node selected and waits for the menu to be painted with items in its list. |
| `tab_menu_create` | With the menu open and `blur` typed (setup, untimed), sends Enter on the Blur row. |
| `create_then_key` | Sends `B` as `create_by_shortcut` does, then Tab as soon as the new node has painted, and waits for the menu's paint. The sample runs from the `B` to the menu's paint, so the work creation defers past the node's first paint (the properties panel's build), which the next key waits on, is counted. |

Each rep brings the project's tab forward, activates the window, focuses the graph and puts the
pointer over its viewport, because the graph only takes the create-node key when it has focus or
the pointer (`NodeGraph::isNodeCreationShortcutTarget`). The sample runs from just before the key
event is posted to the first `QEvent::Paint` that shows the result, caught by an event filter: on
the graph viewport for the creating ops, once the new node is selected or exists, and on the tab
menu's popup, its input, list and list viewport for `tab_menu_open` and the Tab of
`create_then_key`, while its list holds rows. The filter sits on the whole application only for a
Tab that comes before the popup is built. The sample then runs on to the next idle: it ends at the
end of the event-loop pass that delivered the last paint to those widgets before 150 ms went by
without one, so a repaint a creation queues behind its first paint (the whole-widget one about
60 ms later) counts in `create_by_shortcut` as it does in `tab_menu_create`. Settling stops 1 s
after the first paint. The `B` of `create_then_key` does not settle: its Tab follows the node's
first paint, as a person's next key does, and the Tab's sample settles instead. A pass can include
other queued events, so the figure is an upper bound on the last paint's own end. A paint taken
inside the key call does not count, and no first paint within 2 s fails the run.

A creating rep checks that exactly one new Blur appeared, then undoes it through the Edit menu and
checks the root is as it was. The undo history keeps an undone entry until the next push and
destroys its node then, which would put the node's teardown (its properties panel among it) inside
the next rep's sample. So the rep then pushes a label edit on the selected node as an undo step of
its own ("Bench Drain"), which drops the undone creation, and undoes that edit through the Edit
menu, checking that the label is back. The undone label edit is what the next creation drops, at
the cost of one knob-value command. Before each key, and after each creating rep's undo, the bench
drains what is left queued: posted events, deferred deletes and due timers, until the loop has
been idle for 20 ms and nothing in the application has painted for 150 ms, 2 s at most.
`create_then_key` does not drain between its two keys.

The records are in the usual format, with 20 reps each and `median_ms`, `p95_ms`, `min_ms` and
`max_ms` taken from the latencies (the key-to-paint time, not the rep's wall time), plus
`headline: "latency"`, `budget_ms` (50, 33, 50 and 50), `latency_ms` (every sample) and
`latency_reps`. `create_then_key` adds `node_paint_ms`, the median and max of its `B`-to-node-paint
part. `--reps` does not apply. The session's first-ever Tab is a sample of its own:
`tab_menu_open` runs one extra rep when it is the process's first Tab, keeps it out of the 20 and
records it as `first_open_ms`; `tab_menu_create` records it the same way when it is the one that
opened the menu first, and `create_then_key`, when its Tab is the first, runs one extra rep and records that rep's whole
span as `first_open_ms`. The run opens a Natron process per size, so each size has its own first
open.

### The stage split

`--stages` (or `NATRON_NODE_CREATION_STAGE_LOG=FILE` for a run by hand) makes Natron append one
line per stage span to FILE, and adds three fields to each record: `latency_stages_ms`, the median
and max over reps of each stage's time between the key and the paint, `deferred_stages_ms`, the
same for stages that started in that span and ended after the paint (for `create_then_key`, the
span runs to the menu's paint, so `panelBuild` lands in `latency_stages_ms`), and `first_open_stages_ms`
(`tab_menu_open`, first open only). A stage a rep never ran counts as 0 in that rep. Stages nest, so
their times overlap and do not add up to the latency.

```
{"stage": "createNodeGui", "start_ms": ..., "end_ms": ...}
```

The stamps are milliseconds on `steady_clock`, the clock of the stall log and of Python's
`time.monotonic()`. Without the variable each stage site costs one relaxed atomic load.

| Stage | Span |
|---|---|
| `engineCreateNode` | `Node::load`, in `AppInstance::createNodeInternal`; includes `pythonCallbacks`. |
| `createNodeGui` | `GuiAppInstance::createNodeGui`, which holds the stages up to `positioning`. |
| `panelBuild` | `NodeGui::ensurePanelCreated`, the properties panel. A node made from the nodes menu, a plug-in shortcut or the tab menu builds it after the graph's first paint, so it lands in `deferred_stages_ms`. |
| `nameAndEdges` | Laying out the new node's name and edges, which creation collects and runs once before `positioning`. |
| `preview` | `computePreviewImage` for the new node. |
| `autosave` | `triggerAutoSave` for the new node. |
| `positioning` | Placing the node: auto-connect and `moveNodesForIdealPosition`. |
| `pythonCallbacks` | The `onNodeCreated` callbacks and the node's Python variable declaration. |
| `tabMenuLaunch` | `tabtabtab_natron.launch`, the whole Python side of Tab. |
| `tabMenuShow` | The popup's `show()`. |
| `tabMenuGetItems` | `get_items()`, wherever it is called from (inside `tabMenuRefresh`). |
| `tabMenuRefresh` | The popup's refresh of its items, inside `tabMenuShow`. |

The `tabMenu*` stages are timed by wrapping the Python functions from the bench itself, so they
need no Natron support and only exist while the op's setup has run. `launch`'s time does not
include the C++ side of `TabMenuBridge::launch`: building the context and calling it.

## The two modes

**`--xvfb`: regressions only.** This runs `build/release/App/Natron`, or `--appimage PATH`, in the
natron-dev container under Xvfb with software GL (llvmpipe). Paint costs there are nothing like a
real GPU's, and the container shares the machine with whatever else is running. So the numbers
mean something only next to another `--xvfb` run from the same machine. Use it to catch a change
that makes an op slower or makes it stall, not to judge whether the GUI feels fast.

```
tools/bench/gui/run.sh --xvfb --preset gate --baseline tools/bench/gui/baselines/xvfb.jsonl
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
tools/bench/gui/run.sh --host --appimage Natron-x86_64.AppImage --preset gate
```

Natron takes over the screen while it runs. Leave the mouse and keyboard alone. The terminal
prints the result path first, then each result line as it lands; a run that writes no results
ends with the tail of `natron.log`. On a session whose screen X clients cannot read back
(XWayland), the run notes it and carries on; the bench itself reads no pixels.

Both modes run Natron under a throwaway HOME seeded with `checkForUpdates=false`, so your settings
are neither read nor changed. The HOME also carries current `appearanceVersion` and
`NATRON_SHORTCUTS_DEFAULT_VERSION`, which matter only to AppImages built before commit 9a757f381:
those ask on every restart whether to take a new default appearance or new shortcuts, and the
seeds stop them asking. Later builds never ask. Each run writes:

- `build/bench/gui/<UTC>-<host>-<mode>.jsonl`, the results;
- `build/bench/gui/<UTC>-<host>-<mode>/<topology>-<n>/`, holding `natron.log`, `results.txt` and
  `stalls.jsonl`.

The node graph driver runs each topology and size in its own Natron process, so no size runs on a
project another size reset. An op slower than its timeout is recorded as `over_budget` and the run
goes on; one that hangs Natron is recorded as a wedged `over_budget` and Natron restarts or the run
moves to the next size (see below). Results already written stay in the one `.jsonl`, and the exit
code is the last non-zero one.

Ops run in an order that keeps the most signal when one wedges: the read-only interactions first
(`fit_all`, `zoom_step`, `pan`, `hover`, `click_select`, `select_all`, `rubber_band`,
`drag_move_1`, `drag_move_50`), then the cheap edits (`create_one`, `connect`, `edit_label`,
`set_expression`, `knob_change_fanout`, `frame_step`, `idle`), then the heavy edits cheapest first
(`duplicate_100`, `paste_100`, the `delete_1000`/`undo`/`redo` cycle, `paste_1000`).

Opening the cached graph is timed as the `load` op, but it is the setup every other op needs, so its
wedge guard is the largest of 60 s, 3 times the slowest load the cache's sidecar records and the
generation time it records, not an op's guard. A load past the op timeout is still
recorded as `over_budget`; the record carries `guard_s`.

The other options are `--ops` (a subset of ops), `--out` and `--self-check`. Under `--xvfb`,
`--baseline`, `--graph-cache` and `--out` must be inside the checkout, the only directory the
container mounts; `run.sh` refuses any other path.
`--self-check` adds an op that blocks the main thread for 300 ms. It fails unless exactly one
stall is attributed to it, which checks the stall monitor and the attribution end to end.
`GUI_BENCH_OP_TIMEOUT_S` (default 30) caps every per-rep timeout and is the timeout of an op with
neither a budget nor a baseline. `GUI_BENCH_TIMEOUT` (default 6 h) caps the whole run.
`GUI_BENCH_GRAPH_CACHE`, `GUI_BENCH_BASELINE` and `GUI_BENCH_REPS` set the cache, the baseline and
the reps when the matching option is not given; both modes pass them to the driver.

## What a record holds

The first line is a `host` record: CPU, cores, Qt platform, git SHA, build type and the AppImage.
The git SHA is the harness checkout, not the build: an AppImage carries no commit, so the record
names it by file name, size, mtime and sha256 (`appimage_*`), and `compare.py` prints those.
Each graph then gets a `graph` record (its counts and content hash) followed by one `op` record
per op. An `op` record holds:

- `median_ms`, `p95_ms`, `min_ms`, `max_ms` of the per-rep wall time;
- `per_event_max_ms`, the longest single event;
- `step_wall_median_ms`/`step_wall_max_ms`, `step_cpu_median_ms`/`step_cpu_max_ms` and
  `loop_pass_median_ms`/`loop_pass_max_ms`, over every event of every rep (see below);
- `step_wall_p50`/`step_wall_p90`/`step_wall_p95`, the per-event percentiles, and `samples_ms`, the
  per-event times themselves (0.1 ms, at most 200, thinned evenly; `samples_total` when thinned);
- `stalls`: the count, max and total of the stalls whose midpoint falls inside a timed rep;
- `status`: `ok`, `over_budget`, `not_run` or `error` (`timeout` in older results), with
  `timeout_s`, the rep's timeout (see below), and for `over_budget` `censored_ms` and `over_reps`.
- `dialogs`, when a modal dialog came up during the op: each one's title, text, the answer the
  bench gave and the op step it came up in.

An op runs its events one at a time on the Qt event loop, so timers and deferred paints land
between events, as they would for a user. Interaction events force a synchronous viewport
repaint. Setup and restore steps are not timed.

The per-rep wall time folds three things together, so each event is also timed in parts:

- `step_wall_*`: wall time of the event's own step, including its forced repaint;
- `step_cpu_*`: main-thread CPU time of the same step (`time.thread_time()`); a gap to
  `step_wall_*` is waiting, not work;
- `loop_pass_*`: wall time from the step returning to the next step starting, which is the Qt
  event-loop pass (`singleShot(0)`) that runs deferred paints and timers. Only gaps between two
  consecutive plain events count; deliberate sleeps and waits are excluded.

The split fields are absent from older results. `compare.py` prints them under an op only when
both sides have them.

`--no-forced-repaint` (`GUI_BENCH_FORCED_REPAINT=0`) swaps the forced `viewport().repaint()` after
each interaction event for `QApplication.processEvents()` inside the step. Paints then happen as
Qt schedules them (coalesced), which is closer to what a user sees; the host record carries
`forced_repaint`. Do not compare a run made with it against one made without.

## Profiling one op

`profile_ops.sh` does all of this for a list of ops, in the container or with `--host` on an
AppImage (sampling with `eu-stack`, or `sample_stacks_gdb.sh` when only gdb is installed), and
prints why the sampler could not attach (missing tool, `kernel.yama.ptrace_scope`) before the first
op. It prints `== profile <op> start|end` around each op. Run it with `--help` for its options. Each
rep is still cut at its per-rep timeout, so a long op is sampled only up to that point. The steps
below are the manual path.

`perf` is not available in the container, so profile with wall-clock stack samples. Use an Xvfb
run of a release build on the `comp` topology at N=3000, one rep, one op:

```
tools/bench/gui/run.sh --xvfb --sizes 3000 --reps 1 --ops pan --out build/bench/profile
```

That prints the result path and detaches. While the op runs, find the Natron PID and sample it
from inside the container as root (ptrace needs `-u root --privileged`):

```
docker exec -u root --privileged natron-dev tools/bench/sample_stacks.sh PID 200 0.25 build/bench/profile/pan.stacks
```

`sample_stacks.sh PID COUNT INTERVAL OUT` runs `eu-stack` on every thread COUNT times, INTERVAL
seconds apart, and stops when the process exits. Sample only the op: size COUNT x INTERVAL to the
op's duration from an earlier run's `median_ms`, and start sampling once the log says the op has
begun (the graph is generated and loaded first). Summarise the top frames with
`tools/bench/analyze_stacks.py`. Read `step_cpu_*` against `step_wall_*` first: a step that is
mostly wall time is waiting, not computing.

`GUI_BENCH_HOLD_FILE=PATH` makes each op report `BEGIN op topology n` after its setup and wait (up to
`GUI_BENCH_HOLD_S`, default 120 s) until PATH exists, so a sampler can attach before a short op
starts. On a host with `kernel.yama.ptrace_scope` 1, only an ancestor may attach to Natron;
`GUI_BENCH_PTRACER_ANY=1` makes the bench allow any process of the same user to attach. The samplers
exit non-zero with the attach error and the remedy when their first sample gets no stacks.

A stall is any stretch where the main thread doesn't service a 10 ms heartbeat timer for over
50 ms (`NATRON_STALL_THRESHOLD_MS`). The stall monitor runs only when `NATRON_STALL_LOG` names a
file, and `run.sh` sets it.

Each rep has its own timeout: 5 times the larger of the op's budget (16 ms per event for
interaction ops, 100 ms for edits, `OP_BUDGET_MS`) and its baseline median, clamped to 2–30 s. It is
checked before each event and once the last event returns. A rep past it is abandoned, and the op is
recorded as `over_budget` with `censored_ms`, how far the rep got (the smallest such value when
several reps went over), and `over_reps`, how many did: the value is censored, a lower bound rather
than a time.
Under Xvfb, events cost 100–150 ms each, so without a baseline most interaction ops go over budget;
the gate baseline is what makes Xvfb timeouts meaningful.

A single step that doesn't return within its timeout plus 10 s (untimed setup and cleanup get at
least 60 s) wedges Natron: the bench writes an `over_budget` record with `wedged` and exits 3. With
no stalls logged over that time, the step is waiting in a nested event loop, not computing.

With restarts left (`--restarts N`, the full preset's default is unlimited), `run.sh` then restarts
Natron for that topology and size (logs under `<topology>-<n>-restart<k>/`) with
`GUI_BENCH_RESUME=1`, which skips the ops already in the result file and opens the graph from the
cache. With none left (the gate's default), the wedge also records every op not yet run at that size
as `not_run` with the reason `Natron wedged on <op>`, and the run goes on to the next size: a
restart reopens the graph, which on a slow build costs more than the ops it saves. An op that
wedged is recorded as a not-run `over_budget` (with `wedged_at_n`) at every larger size of the
topology instead of being run again, except the cached `load`, which every size needs; set
`GUI_BENCH_SKIP_WEDGED=0` to run them anyway. `run.sh` still exits 3 when anything wedged.

`delete_1000`, `undo` and `redo` run as one cycle per rep, each step timed and recorded under its own
name. When one is not ok, the ones after it are recorded as `not_run` with a `reason` naming it.

The bench answers any modal dialog within 100 ms (every 1 ms around a delete): Yes, OK or Continue
when the dialog offers one, otherwise it rejects it. Each answer is printed as a `NOTE dialog`
line and kept in the op's `dialogs` field, so a dialog cannot hold up a run unseen.

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
tools/bench/gui/compare.py tools/bench/gui/baselines/xvfb.jsonl build/bench/gui/<run>.jsonl
tools/bench/gui/compare.py --scaling build/bench/gui/<run>.jsonl
tools/bench/gui/compare.py --budgets build/bench/gui/<host run>.jsonl
```

Baselines recorded before the gate preset (the per-rep timeouts, the graph cache and the delete
cycle) are censored at 120 s timeouts and come from builds before the gate preset; they move to
`baselines/archive/` and are re-recorded in gate mode, so do not gate on them. `compare.py` warns on stderr whenever a graph
that both files generated for the same topology, n and seed has a different hash, because the rows
for that size then time different graphs.

Comparison flags an op when any of the following holds, and then exits 1:

- its per-event p50, per-event p95 or rep median grew by more than `--tol` and by more than
  `--min-ms` (default 5 ms); the p95 is judged only with 20 or more events on both sides, since
  below that it is the slowest event, and the rep median only with 3 or more reps on both sides or
  more than one event per rep, since a single-event op's median of one or two reps is a draw or two
  and its per-event p50 already judges the same event;
- its status fell from `ok` (over budget, wedged or an error), or from `over_budget` to an error;
- it is missing from the run, or `not_run` for a reason of its own;
- it stalled for more than `--long-stall-ms` (default 250 ms) where the baseline did not;
- it was `over_budget` on both sides and the run wedged Natron where the baseline did not and had got
  less than half way to the 12 s wedge guard; a baseline already past half way is the same op on the
  other side of a timing edge.

The defaults depend on the `mode` in the host records: when either file is an `xvfb` run, `--tol` is 3.0,
`--min-ms` 200 and `--long-stall-ms` 3000, otherwise 0.2, 5 and 250. Three recordings of one build under
Xvfb on the 4-core dev host differ by several times, in both directions, in wall time and in per-event
main-thread CPU alike (`BASELINE.md` has the table), so an Xvfb gate only catches an op that got more
than four times slower and 200 ms slower, or that stalls for seconds. Passing any of the three flags
overrides its default in either mode.

`over_budget` (and `timeout`) values are censored. Over on both sides prints as `still over`, with
each side's `censored_ms`, and is judged neither way unless the run newly wedged Natron, which is a
regression; over in the baseline only is an improvement. An error in the baseline leaves nothing to
judge the run by, and prints as such.
An op `not_run` because the cycle step before it failed prints its reason and is not counted; the
step that failed is. An op `not_run` because another op wedged Natron (`Natron wedged on <op>`) is
`not measured` when the baseline has no `ok` record of it either, and `REGRESSED: missing after
wedge` when it does. When the graph at a size was never set up, the run carries a `note` record with
`setup_failed`: the bench writes one when opening the cached graph wedges Natron, and `run.sh` writes
one when a Natron process for a size exits non-zero without recording any op (it died or hung
generating or loading the graph, or the run's cap ended it). `compare.py` prints the note in the
header and reports that size's ops as `setup wedged` rather than `missing`.

A slowdown that rests on fewer than 5 samples on either side (per-event samples, else reps) is
`CONFIRM`, not a regression, and does not fail the comparison. A fall from `ok` to `over_budget`
without a wedge is judged on the baseline's samples only, since the run's value is a lower bound: it
is a regression when the baseline has 5 or more, and otherwise `CONFIRM` until a re-run goes over on
5 reps with `censored_ms` past the baseline median by more than `--tol` and `--min-ms`. The output ends with the `run.sh --rerun <ops> --reps 5 ...` line that re-runs those
ops against the same baseline; `--appimage PATH` puts the AppImage's path in it, otherwise it names
the run's AppImage file.

`--budgets` judges a host run: each budgeted op at the largest n of each topology, on its
per-event p50, against 16 ms (interaction) or 100 ms (edit). Over budget, censored or not run
fails, and it exits 1 on any failure. Xvfb runs are for regressions only, never budgets.

`--scaling` fits the log-log slope of the median against N for each (op, topology) and ranks them
steepest first. A slope above 1.2 is marked superlinear: that op's cost grows faster than the
graph does.

When the two files name the same AppImage, `compare.py` warns; `merge.py` warns when
the files it merges come from different builds.

`compare.py --self-test` runs as ctest `GuiBenchCompareSelfTest`.

## Re-baselining

Re-baseline after a deliberate performance change lands, or when the container image or CI
machine changes. Don't re-baseline to make a regression go away.

1. Build release at the commit to be the baseline, and leave the machine otherwise idle.
2. Run the gate preset with `--xvfb --wait` and no `--baseline`.
3. Check the run against itself is clean, and against a second gate run at the chosen tolerance;
   the spread between them sets the tolerance (at least twice the worst spread for that kind of
   op). `merge.py --spread` prints the spread table.
4. Commit the result as `baselines/xvfb.jsonl`, moving the old one to `baselines/archive/`. Record
   the spread, the tolerance and the `--scaling` table in `BASELINE.md`.

Host baselines are per machine: `baselines/host-<hostname>.jsonl`. Compare them only against
runs from the same machine.
