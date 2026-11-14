# Render-scaling benchmarks

Scripts for measuring how Natron's graph construction and rendering scale with node count, and
for finding where the time goes. Everything runs in the `natron-dev` container against
`build/release`.

| File | Purpose |
|---|---|
| `graph_bench.py` | Builds one synthetic graph in NatronRenderer, renders it, appends a JSON result line. |
| `run_matrix.sh` | Runs `graph_bench.py` over topologies and sizes, one process per configuration. |
| `profile_run.sh` | Runs one configuration and samples its stacks with eu-stack once the graph is built; `SAMPLER=states` samples thread states instead. |
| `sample_stacks.sh` | The eu-stack sampler (needs `docker exec -u root --privileged` for ptrace). |
| `sample_states.sh` | Counts running threads from `/proc` without stopping the process; use it for concurrency. |
| `lib.sh` | Sourced by `run_matrix.sh` and `profile_run.sh`: cooldown, CPU frequency logging, and writing the clock into the result record. |
| `analyze_stacks.py` | Summarises eu-stack samples: busy/blocked/idle, activity, self and inclusive functions. |
| `compare.py` | Compares two result files per (topology, n, resolution, named); exits non-zero on a regression past `--threshold`. |
| `stream_bench.c` | Node-at-a-time vs tiled vs fused cost of a chain of point ops on this machine. |
| `page_fault_bench.c` | First-touch cost, huge-page coverage and THP counters of a fresh buffer allocated like `HostImageMemory`, with optional competing memcpy load. |
| `make_plates.py` | Writes the synthetic EXR plates and the deep sequence the plate-fed families read. |
| `gpu_region_bench.sh` | GPU Off/Auto comparison of spatial nodes (alone and between Grades) and the M82 comps from a Natron AppImage, as first-frame, re-render, jump and playback sessions; keeps each run's render profile. |
| `gpu_region_summary.py` | Splits the GPU region records of those profiles into per-phase times per region and per node. |
| `gpu-region-profile-sample.jsonl` | Region records in the current format, used by `gpu_region_summary.py --self-test`. |
| `raw_large_bench.sh`, `raw_large_bench.py` | Renders a camera raw file through the whole Develop RAW chain from a Natron AppImage at 1, 2 and 4 render threads (`BENCH_THREADS`) and prints time, speedup and peak RSS per count. |
| `reformat_filter_bench.sh`, `reformat_filter_bench.py` | CPU cost of a 4x Reformat downscale with Cubic and Lanczos 3, a main AppImage against a branch AppImage. |
| `classify.py` | Prints a boundedness verdict (CPU, bandwidth, IO, RAM, host overhead) per result record. |
| `bezier_keying_bench.py` | Keys Roto Beziers at 1..K frames in NatronRenderer and prints the keying time per K, the median per-edit cost and the log-log slope against K. |

Topologies: `chain` (N Grades in series), `mixed` (Grade/Blur/Transform/ColorCorrect in series),
`wide` (sources merged by a balanced tree), `comp` (seeded random DAG with fan-out and merges).
Per-node chains isolate one node's cost: `ccchain` and `blurchain` (N ColorCorrects or Blurs),
`xfchain` (N Transform + Grade pairs, since Transforms in series concatenate), `mergechain` (N
Merges, each with the same second source) and `mergesrcchain` (each Merge with a fresh
CheckerBoard). Take the marginal cost between two sizes, and subtract `chain` from `xfchain` and
`mergechain` from `mergesrcchain`.
Two fixed graphs measure GPU regions on spatial nodes: `gtg` (Grade -> Transform with motion blur
`BENCH_MOTION_BLUR`, default 1 -> Grade) and `gebg` (Grade -> Erode `BENCH_ERODE_SIZE`, default 6 ->
Blur -> Grade); `BENCH_N` does not apply to them.
Resolutions: `tiny` (32x32, isolates per-node overhead), `hd`, `uhd`. Sources are animated so
nothing is cached as frame-invariant.

```
tools/bench/run_matrix.sh tiny tiny 5 0 chain:10,100,1000 wide:10,100,1000
tools/bench/profile_run.sh chain1000 60 0.5 BENCH_TOPO=chain BENCH_N=1000 BENCH_FRAMES=40
python3 tools/bench/analyze_stacks.py build/bench/samples-chain1000.txt
python3 tools/bench/compare.py build/bench/results-before.jsonl build/bench/results-after.jsonl --threshold 1.5
```

Results land in `build/bench/results-<tag>.jsonl`, logs in `build/bench/logs/`.

## Realistic workloads

The generator-fed families above decode nothing and keep every image far below the cache budget,
so they cannot show IO or memory limits. These families read plates from disk:

| `BENCH_TOPO` | graph | nodes |
|---|---|---|
| `readchain` | Read (plate 1) -> N Grades -> Write | N + 1 |
| `footagecomp` | K = N/4 Reads cycling the three plates, each -> Grade -> Transform (translate drifting per frame) -> CImgBlur on every other branch, merged pairwise; the first half of the merges (the leaf level first) masked by a Roto node holding one ellipse or rectangle | about 4.5 K |
| `iobound` | K = N/2 Reads -> one Merge chain -> Write (writer compression `none`) | about N |
| `rambound` | the `wide` tree (CheckerBoard -> Grade leaves, Merge tree), meant for `BENCH_RES=uhd`: at N=192 its 64 leaf images of 133 MB (float RGBA) exceed the 7.5 GB cache budget of this 15 GB machine | about N |
| `deepcomp` | K = N/2 DeepReads of the deep sequence -> DeepMerge chain -> DeepToImage -> Merge over HD plate 1 -> Write | about N |

`BENCH_RES` (`hd` or `uhd`) picks the plate resolution and project format of `readchain`,
`footagecomp` and `iobound`; `deepcomp` always uses HD. Reads loop their 8-frame sequence (`before`
and `after` set to `loop`), so every timed frame decodes a different frame. DeepRead cannot loop:
`deepcomp` needs `1 + BENCH_FRAMES + BENCH_RANGE + 1` frames on disk and fails early otherwise, so
use `BENCH_FRAMES=3 BENCH_RANGE=3` with the default plates or regenerate them with a larger
`BENCH_PLATE_FRAMES`. The deep chain's sample count grows by one per merge (K samples per pixel at
the end, about 50 MB of samples per layer at HD), so keep `deepcomp` at N <= 16 on this machine.
The Roto shapes are not animated, so each Roto matte is rendered once per frame like any other
input.

Plates: `make_plates.py` writes, per resolution, `plate_<hd|uhd>_<1|2|3>.####.exr` (frames 1-8,
half-float RGBA; plates 1 and 2 ZIP, plate 3 PIZ), and `deep_hd.####.exr` (DeepFromImage of the
HD plate 1 image with a Z ramp from 1 to 100, written by DeepWrite). Each image is a checkerboard
mixed with full-frame noise reseeded per frame, so frames differ and compress as poorly as grainy
footage. Existing complete sequences are skipped and every sequence's size is logged.
`BENCH_PLATES_RES=hd|uhd|all` (default `all`), `BENCH_PLATES_DIR` (default `build/bench/fixtures`,
gitignored), `BENCH_PLATE_FRAMES` (default 8).

```
docker exec -e BENCH_PLATES_RES=all -e REPO="$PWD" -e OFX_PLUGIN_PATH="$PWD"/build/assets/Plugins natron-dev \
    bash -lc 'cd "$REPO" && xvfb-run --auto-servernum build/release/Renderer/NatronRenderer -b tools/bench/make_plates.py'
tools/bench/run_matrix.sh real-hd hd 3 0 readchain:30 footagecomp:64 iobound:16 deepcomp:16
tools/bench/run_matrix.sh real-uhd uhd 3 0 iobound:16 rambound:192
python3 tools/bench/classify.py build/bench/results-real-hd.jsonl build/bench/results-real-uhd.jsonl
```

`BENCH_KEEP=1 tools/bench/run_matrix.sh ...` copies each configuration's written frames to
`build/bench/keep/<tag>/` before deleting them, e.g. to compare the two scheduler modes byte for
byte (the EXR `capDate` header differs between runs).

IO counters: around every timed phase `graph_bench.py` reads `/proc/self/io` and records the
deltas. The top-level `io_rchar`, `io_wchar` (bytes passed through read/write calls, page-cache hits
included), `io_read_bytes`, `io_write_bytes` (block IO) and `majflt` (major page faults) cover the
timed frames; `io_phases` holds the same for `warm`, `frames` and `range`. `plate_bytes` is the plate
file bytes the graph decodes per frame (the mean frame size of each Read's sequence, summed over
Read and DeepRead nodes), `output_bytes` and `output_frames` the size and count of the frames the
writer produced, measured before `run_matrix.sh` deletes them. `compare.py` prints `io_r`/`io_w`
(block IO MB of the timed frames) with their ratio when both files have them; they are never
flagged. Plates just written sit in the page cache, so block reads are usually 0 unless the cache is
dropped first (`sync; echo 3 > /proc/sys/vm/drop_caches` as root on the host).

`classify.py <results.jsonl>...` prints one line per record: the verdicts and the numbers behind
them. The thresholds are heuristics (see the script's docstring): RAM-bound when `rss_peak_mb` is
at least 80% of the cache budget (half of total RAM, or `BENCH_CACHE_MB`) or the timed frames took
major page faults; IO-bound when block IO or the plate plus output file bytes per frame reach half
of `BENCH_DISK_GBPS` (default 2); bandwidth-bound when the estimated bytes moved per frame (pixels
x 16 x (2 x nodes + merges)) reach half of `BENCH_STREAM_GBPS` (default 7); host-overhead-bound
when the frame takes under 1 ms per node at parallelism <= 1.3; CPU-bound when parallelism is at
least 70% of the cores with no other signal; otherwise under-occupied. Natron prints nothing when
its cache evicts, so there is no purge count.

`BENCH_DRY_RUN=1 BENCH_TOPO=<topo> BENCH_N=<n> python3 tools/bench/graph_bench.py` builds a graph
against stand-in objects on the host and prints its node counts, without Natron.

eu-stack stops the process while it walks stacks, so samples in which no thread looks busy are
an artifact; take concurrency from `sample_states.sh` instead. perf is not installed in the
container and the container has no package network.

`BENCH_NAMED=1` (the default) gives every node an explicit name, which skips the default
unique-name search in `NodeCollection::checkNodeName`; set it to 0 to measure the default
`app.createNode()` path.

`BENCH_SETTINGS="name=value;name=value"` sets Natron settings for a run. `run_matrix.sh` and
`profile_run.sh` expand it into repeated `--setting name=value` arguments for NatronRenderer, and
`graph_bench.py` records the string as `settings` (empty when unset). `compare.py` only matches
records with the same `settings`, and records without the field count as empty.
To compare runs made under different settings, pass `--pair-settings BEFORE_SETTINGS AFTER_SETTINGS`:
rows of the first file whose `settings` equal the first value pair with rows of the second file whose
`settings` equal the second, and `default` selects rows with empty `settings`. For example
`compare.py --pair-settings renderSchedulerMode=0 renderSchedulerMode=1 legacy.jsonl taskgraph.jsonl`
prints the Task graph / Legacy ratios.

`BENCH_SETTINGS=renderSchedulerMode=0|1` selects the render scheduler: 1 (Task graph) is the
default when unset, 0 selects Legacy pull. A run with empty `settings` therefore measures the
Task graph scheduler.

With `BENCH_RANGE` > 0 the record carries `range_frames`, `range_wall_s`, `range_cpu_s` and
`range_parallelism` (`range_cpu_s / range_wall_s`, null without a range). `compare.py` prints the
range per-frame wall time (`range_wall_s / range_frames`) and `range_parallelism` as extra columns
when both files have them.

`SAMPLER=states tools/bench/profile_run.sh <name> <count> <interval> VAR=...` writes the
running-thread histogram to `build/bench/states-<name>.txt`.

## Methodology

This host (Intel N100) drops its CPU clock after roughly 10-16 s of full load. Runs of different
length or thermal state are therefore not comparable: 3-frame HD rows run mostly at full clock,
while 20-frame runs throttle. Compare only runs of equal length that started from the same state.

- Cooldown: before each configuration `run_matrix.sh` and `profile_run.sh` wait until the 1-minute
  load average is below `BENCH_MAX_LOAD` (default 0.5, at most `BENCH_COOLDOWN_TIMEOUT` seconds,
  default 600), then sleep `BENCH_COOLDOWN` seconds (default 60).
- Clock log: while a configuration runs, `/sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq`
  is sampled every 0.5 s into `build/bench/freq-<tag>-<topo>-<n>.txt` (`freq-<name>.txt` for
  `profile_run.sh`), one line of `epoch_seconds khz_cpu0 khz_cpu1 ...`. `run_matrix.sh` adds the
  mean and minimum over all CPUs to the result record as `cpu_khz_mean` and `cpu_khz_min`.
  `compare.py` prints `khz_mean` for both sides and prints a `note:` when they differ by more
  than 10%.
- OpenMP: `OMP_WAIT_POLICY`, `GOMP_SPINCOUNT`, `OMP_THREAD_LIMIT` and `OMP_DISPLAY_ENV` are
  forwarded into the container when set on the host, e.g.
  `OMP_WAIT_POLICY=passive tools/bench/run_matrix.sh ...`.
- `analyze_stacks.py` counts threads parked in `gomp_barrier_wait_end`,
  `gomp_team_barrier_wait_end` or `gomp_thread_start` as idle (or blocked when inside a render),
  not busy.
- The writer's compression is set to `none` and read back as `[bench] writer compression=<value>`
  in the log. Earlier records were written with an invalid option (`No Compression`) that was
  silently ignored, so all of them wrote ZIP-compressed half-float EXR.
- Render stats: `BENCH_RENDER_STATS=1` makes `graph_bench.py` save the project after the timed frames
  and render frame 2 once more with `NatronRenderer --render-stats -w` (`app.render` cannot enable
  stats), recording `tasks_run` and `max_concurrent_tasks` from the `-stats.txt` file. It is a separate
  cold render; Legacy reports 0.

## GPU region bench

`tools/bench/gpu_region_bench.sh <Natron.AppImage> [out-dir]` (default `build/bench/gpu-region`) runs each
case with `NATRON_GPU=off` and `NATRON_GPU=auto` under `NATRON_RENDER_SCHEDULER=taskgraph` and
`NATRON_RENDER_PROFILE`, interleaved, the order flipping each round (`BENCH_ROUNDS`, default 3). Nothing is
warmed up: pipeline compile, host import and pinning, pool warm-up and first-frame placement are part of
every number. Kinds (`BENCH_KINDS`, run in this order):

| kind | what runs |
| --- | --- |
| `a-cold` | one frame in a fresh process, with `XDG_CACHE_HOME` pointing at an empty temp dir (the pipeline disk cache lives in `$XDG_CACHE_HOME/Natron/gpu-pipelines`) |
| `a-warm` | the same, with the case's cache dir primed by an untimed run the first time it is needed |
| `bc` | one session on the primed cache: the frame (`first`), `BENCH_RERENDERS` (default 4) re-renders of it after a small knob change (`b`), then a jump to `BENCH_JUMP_FRAME` (`c`) |
| `playback` | `BENCH_PLAYBACK_FRAMES` (default 4) frames as one range in a fresh process; secondary |

Cases: each of `BENCH_NODES` (default `erode40 blur100 ck xf`; `xfmb blur25 iir25 iir100 erode6` also
exist) alone (`node`) and between two Grades (`gxg`) at every size in `BENCH_SIZES` (default `uhd 7680x4320
16384x16384 24576x24576`), then the comps in `BENCH_COMPS` (default the three in `comps/`) at
`BENCH_COMP_SIZES` (default `uhd 7680x4320`). The 8K comps add a project format in-process; the comps'
own coordinates (ramp points, box sizes) stay UHD-sized. The knob that changes between re-renders is the
node's own (Transform scale, Blur size, Erode size, ChromaKeyer acceptanceAngle), or the first Grade's
multiply if the node lacks it; for comps it is `fg_grade1`, `glow_thresh` or `plate_grade`.

- The AppImage runs under `env -u LD_LIBRARY_PATH` with `--appimage-extract-and-run`
  (`BENCH_APPIMAGE_ARGS`), because a host Qt on that path shadows the image's and the host has no FUSE.
  `BENCH_APP_ARGS` (default `-b`) goes before the script.
- Before every run the script reads `MemAvailable` and skips the case, logging the reason to
  `<out-dir>/skips.log`, when its estimate exceeds it: `BENCH_NEED_NODE` / `BENCH_NEED_GXG` (default 6 / 9)
  float RGBA frames, doubled for playback; `BENCH_COMP_NEED_GB` (default 4) per UHD comp, scaled by pixel
  count. Before each run it waits for CPUs under 5% busy and `Dirty` + `Writeback` under 256 MB after a `sync`.
- Each run writes into its own directory under `BENCH_RENDER_BASE` (default `<out-dir>/render`, absolute),
  created by the driver and removed after the run; `graph_bench.py` deletes each render's files and syncs
  outside the timed interval. Render times are taken in-process around the render call; point
  `BENCH_RENDER_BASE` at tmpfs to keep write-back off disk altogether.
- Per run: `<tag>.log`, `<tag>.prof.*`, `<tag>.marks.jsonl` (each render's label, frame and time),
  `results.jsonl` and `driver.jsonl` (process wall and exit status). The tag is
  `<case>-<size>-<gpu mode>-<kind>-r<round>`. A run whose log already holds its result is not repeated.
- `BENCH_DRY_RUN=1` prints the planned and skipped cases with the memory probe and renders nothing.

`graph_bench.py` runs one session itself with `BENCH_SESSION=a|bc|playback` (and `BENCH_TOPO=ntp
BENCH_COMP=<.ntpz>` for a comp); see its header for the variables.

`python3 -I tools/bench/gpu_region_summary.py [--skip-frames N] [--import-rate GBPS] <out-dir>/<tag>.prof...`
reads each run (a file, or a prefix: the profile appends a suffix) and prints its case, GPU mode and kind;
with a `.marks.jsonl` beside it, a per-render table (render time, Write task time, difference) and the
regions grouped by render label (`first`, `b`, `c`, ...). Per region (members joined by `+`) it prints the
mean per run of: plan, wall, kernel, exposed and hidden transfer, upload, download, record, acquire,
submit/wait, prepare wait, other, pipelined floor, imports and pool misses, fallbacks and retries; then per
member node its kernel time and share of the region's wall. With marks the first frame is kept, since it
carries the one-time costs; without them it is skipped as before. `--skip-frames` overrides either.

The pipelined floor is `max(0, T - K*(n-1)/n)` with T the exposed plus hidden transfer ms, K the kernel ms
and n the strips (1 when unstripped). `--import-rate` takes a measured GpuBench host-import rate in GB/s and
computes T as `(uploadBytes + downloadBytes) / rate` instead.

Which fields lie in `wallMs`: `kernelMs`, `transferExposedMs`, `recordMs`, `acquireMs` and
`submitWaitMs` are inside it; `planMs` is outside (a region costs `planMs + wallMs`);
`transferHiddenMs` overlaps kernels, and `uploadMs`/`downloadMs` are transfer totals of which only the
exposed part is in the wall. `other` = `wallMs` minus the five inside parts. Pipelined regions
overlap recording and waiting with kernels, so `other` can go negative there. A node's kernel time is
its task record's `wallNs` (device timestamps). `pred xfer` is the record's `predictedTransferMs`, the
transfer time placement's transfer model predicts for the region's `uploadBytes` and `downloadBytes`, to
compare with `uploadMs + downloadMs`; it reads `n/a` when the model knows no bandwidth (`null`).
`--self-test` checks the breakdown, the floor and the labelling on `gpu-region-profile-sample.jsonl`.

## Page-fault bench

`page_fault_bench.c` faults a fresh anonymous buffer and reports what the kernel did, with no Natron
involved. Build and run on the host; it is plain C.

```
gcc -O2 -pthread -o page_fault_bench tools/bench/page_fault_bench.c
page_fault_bench [-s GiB] [-a mmap|aligned|mmap2m] [-t write|rw|populate] [-n THREADS] [-l LOAD_THREADS] [-j]
```

- `-a`: `mmap` is a plain small-page mapping; `aligned` is `aligned_alloc(2 MiB)` plus `MADV_HUGEPAGE`, as
  `HostImageMemory::rawAlloc` does for large buffers; `mmap2m` is an `mmap` aligned to 2 MiB plus
  `MADV_HUGEPAGE`.
- `-t`: `write` stores one byte per 4 KiB; `rw` does `p[off] = p[off]` per 4 KiB, as `GpuImportPreparer::touch`
  does; `populate` uses `MADV_POPULATE_WRITE` (kernel 5.14+, else it exits with a message).
- `-n` threads (default: all online CPUs) each fault a contiguous slice of whole 2 MiB pages. `-l` starts that
  many threads streaming `memcpy` over their own 256 MiB buffers to compete for memory bandwidth.
- One line per run: fault GB/s, minor faults (`getrusage`), `AnonHugePages` of the buffer's VMA, the
  `thp_fault_alloc`, `thp_fault_fallback` and `thp_split_pmd` deltas, the time to free the buffer, and the
  THP `enabled`, `defrag` and `use_zero_page` settings. `-j` prints the same fields as one JSON object.
- It refuses a buffer (plus load buffers) above half of `MemAvailable`. The vmstat deltas are system-wide, so
  run it on an otherwise quiet host.

## Raw GPU bench

`raw_gpu_bench.sh` times the first frame of the raw Develop chain (Read, RawPrepare, RawHotPixels,
RawDenoise, RawTemperature, RawHighlights in Clip mode, RawDemosaic, RawInputColor, Write to EXR) with
`NATRON_GPU=off` and `auto`, one fresh process per render, no warm-up and an empty `XDG_CACHE_HOME`.

```
tools/bench/raw_gpu_bench.sh --app <Natron.AppImage> [--rounds 3] [--out DIR] [--big FILE] [--dry-run]
```

- Cases: the RAF with Markesteijn 1-pass and 3-pass, the CR2 with AMaZE, plus `--big FILE` (or a raw of
  60 MB or more found in `build/assets` or `Tests/fixtures`). Off and auto alternate per case, the order
  flipping each round.
- `DIR/results.jsonl` has one line per render: case, mode, round, first-frame ms, GPU region count (from the
  `NATRON_RENDER_PROFILE` records, so region membership can be checked in `DIR/<tag>.prof*`), exit status,
  boot id and `MemAvailable`. `DIR/table.txt` has the median first-frame ms off and auto and the speed-up.
- A case is skipped, with a line in `DIR/skips.log`, when `MemAvailable` is below the raw file size times
  `BENCH_NEED_FACTOR` (default 40, at least 4 GiB). `--dry-run` lists the cases and the memory probe.

## Scene edit-to-frame latency

`scene_edit_bench.py` times how long an edit takes to reach the scene viewer: from setting a knob
to the first finished frame drawn after the viewer synced the edit. It runs inside Natron under
Xvfb through the GUI test runner, configured by environment variables (see the script's
docstring), and appends one JSON line per run to `$NATRON_GUI_TEST_OUT/scene_edit_bench.jsonl`.

```
# Once: a 1M-prim file (the natron-dev image's python3 has pxr).
docker exec natron-dev python3 tools/bench/scene_edit_bench.py make-file build/bench/prims1m.usdc --prims 1000000

# A 30-node chain, editing the head (needs a scene op such as USDEditTransform; without one it chains
# Dots and edits USDRead's frame offset).
docker exec -e BENCH_SCENE=chain -e BENCH_N=30 -e BENCH_LABEL=after natron-dev \
    Tests/gui/run-gui-test.sh tools/bench/scene_edit_bench.py build/release/App/Natron

# The 1M-prim file, editing USDRead's frame offset.
docker exec -e GUI_TEST_TIMEOUT=1800 -e BENCH_SCENE=file -e BENCH_FILE=build/bench/prims1m.usdc -e BENCH_LABEL=after natron-dev \
    Tests/gui/run-gui-test.sh tools/bench/scene_edit_bench.py build/release/App/Natron
```

Run each configuration with a build from before the long-lived scene (`BENCH_LABEL=before`) and
one from after, one at a time in the container. Builds from before lack the
`natronSceneSyncedAtMs` and `natronSceneLastPaintMs` properties, so their latencies are rounded to
the 50 ms poll and marked `"approximate": true`; their open times are exact. Compare
`edit_ms_median` against `open_ms`: an upstream edit on the 1M-prim file should cost far less than
opening it. `engines_after` should equal `engines_before` (1) after the phase. The record keeps
nothing about the machine: note the host with the numbers.

The USDRender half of the gate (edit to rendered frame on a USDRender node) needs USDRender's
per-node long-lived stage and is not covered here yet.

## Bezier keying cost

`bezier_keying_bench.py` builds finished feathered Beziers in a Roto node and keys every control
and feather point at frames 1..K through the Python roto API, auto-keying on. It prints the keying
wall time per K, the median cost of one point edit at the largest K and the least-squares slope of
log(time) against log(K). A slope near 2 means each edit costs time proportional to the shape's
keyframe count; near 1 means a constant cost per edit. Presets are environment variables, since
NatronRenderer passes no arguments to the script (see the docstring).

```
# 1 shape x 24 points x 50, 100 and 200 keys.
tools/ci/local/devshell.sh bash -lc "cd $PWD && BENCH_LABEL=before BENCH_OUT=build/bench/bezier-keying.jsonl \
    build/release/Renderer/NatronRenderer -b tools/bench/bezier_keying_bench.py"

# Heavy: 30 shapes x 24 points x 200 keys.
tools/ci/local/devshell.sh bash -lc "cd $PWD && BENCH_BEZIER_HEAVY=1 BENCH_LABEL=before BENCH_OUT=build/bench/bezier-keying.jsonl \
    build/release/Renderer/NatronRenderer -b tools/bench/bezier_keying_bench.py"
```

Run both with a build from before the incremental GUI copy (`BENCH_LABEL=before`) and one from
after (`BENCH_LABEL=after`). The record keeps nothing about the machine: note the host with the
numbers. `Tests/BezierEditScaling_Test.cpp` prints the same single-shape figures from the engine
directly, without the Python binding's overhead, and fails a release build when keying stops
scaling linearly with the keyframe count.

## Heavy scene fixtures

`heavy_scene_make.py` generates the two scenes the scene-viewer budgets run against, under
`build/bench`: a payload city (about 300 blocks of buildings, `/World/BlockN/BM`, one `.usdc` payload
per building, about 1M prims once loaded and a few thousand before, `extentsHint` authored on nine
in ten buildings) and a 1M-prim flat file. It needs the pxr module, so run it in the natron-dev
container. The fixtures are never committed; only the generator is.

```
docker exec natron-dev bash -lc "cd $PWD && python3 tools/bench/heavy_scene_make.py"
```

`heavy_scene_gpu_host.sh fixtures` makes them on a host without pxr, through the dev image, under
`build/bench-gpu`.

Any other scene runs with `BENCH_FIXTURE=scene` and `BENCH_FILE`. `heavy_scene_rows.py` picks the rows
the actions use from its composed stage, in a process of its own under the AppImage's `natron-python`,
and the bench caches them under `BENCH_ROWS_DIR`. An action with no row to act on gets a `skipped`
record.

## Heavy scene budgets

`heavy_scene_bench.py` runs one action per scenario against a heavy scene inside Natron and prints
one JSON line per scenario: the longest main-thread gap (10 ms heartbeat), the layers the action
loaded, and the action's own numbers, each marked `pass`, `fail`, `not_valid` or `not_applicable`
against the scene-graph budgets. Frame rate and time to first frame need a real GPU: without
`BENCH_GPU=1`, or on a software renderer, they are measured but marked `not_valid`. Stall and
unrequested-payload checks are valid under llvmpipe. The scenarios are the harness's `ACTIONS` table
(`open`, `edit`, `orbit`, `load`, `twirl`, `node_switch`, `workflow`, and the draw state scenarios below); `BENCH_SCENARIOS` picks
and orders them.

`open` reports `first_frame_ms`, from setting the file to the first frame finished after the scene
reached the viewer.

`load`, after `open`, asks the viewer to load `BENCH_LOAD_PATHS` (one building by default;
`/World/BlockN` with `BENCH_LOAD_DESCENDANTS=1` is a larger load, and the paths can name several)
while moving the mouse in a tumble until the result is on screen. It reports `load_ms`,
`load_mouse_moves`, `load_move_gap_ms` (the longest wait between two moves), `load_hold_ms` (how long
the last batch held the viewer lock), `load_edits` and `load_failures`; the stall budget covers the
whole load, and the requested payload files do not count as unrequested.

`twirl`, after `open`, docks the Scene Graph panel beside the viewer and opens an unloaded payload row
in its tree (`BENCH_TWIRL_PATH`; `/World/Block0/B1` on the city), tumbling the view until the row is
loaded and the result is on screen. The stall budget covers the time from the twirl on, not the
docking. It reports `twirl_ms`, `twirl_rows`, `twirl_mouse_moves`, `twirl_move_gap_ms` and the same
`load_hold_ms`, `load_edits` and `load_failures`.

`node_switch`, after `open` (on the flat file for the design's budget), docks the Scene Graph panel,
sets it to follow the node selection and selects `BENCH_SWITCH_NODES` (default 4) USDEditTransforms
off USDRead in turn, each read into a stage of the panel's own, then the first again, whose stage the
panel keeps. The stall budget covers the switching, not the docking or the node creation. It reports
`switch_ms` per switch (each includes the selection's 150 ms settle), `switch_max_ms`,
`cached_switch_ms` and `cached_switch_composes`, which is 0 when the kept stage is reused.

The draw state scenarios drive the viewer's `setDrawState`, `clearDrawState` and `setDrawRevealBatch`
and act on rows `heavy_scene_rows.py` picks, or the fixture's own (`BENCH_ROOT`, `BENCH_BRANCH`,
`BENCH_SIBLING_ROW`, `BENCH_WIDE_ROW`, `BENCH_REVEAL_BRANCH` override them). Each carries `filter_bound`, judged: a viewer
whose engine lost the filter fails rather than reporting a cheap nothing.

| Scenario | What it does | Reports |
| --- | --- | --- |
| `populate_full`, `populate_pruned` | Open the file, in place of `open`; pruned sets `bbox` on the root first | `first_frame_ms`, `prims_added_upstream`, `prims_added_ms`, `hidden_sent` |
| `load_all` | Loads the root with descendants; `BENCH_LOAD_ALL=1` picks rows from the loaded scene | `load_ms`, `prims_added_upstream`, `load_hold_ms` |
| `prune_restore` | `bbox` then clear on the branch | `prune_*` and `restore_*`: `apply_ms`, `frame_ms`, `total_ms`; hidden and shown notices |
| `sibling_state` | The same on one row among many, twice | `sibling_*`; `sibling_resynced_paths` budget 0 |
| `reveal` | Row `bbox`, then `full` at `BENCH_REVEAL_BATCH` prims per frame, up to `BENCH_REVEAL_MAX_S` | `reveal_prims`, `reveal_frames`, `reveal_fps`, `reveal_worst_frame_ms`, `reveal_complete` |
| `reveal_cancel` | Starts a reveal at 50 prims per frame, then `bbox` after three frames' worth | `cancel_total_ms`, `cancel_was_active`, `cancel_stopped` |
| `noedit` | Sets, clears, reveals and cancels on the branch | `stage_edits`, budget 0, counting stage change notices and edit counts |
| `sibling_shot` | Screen grabs around one change on the sibling row, the branch and the root as control | `*_shot_changed_*`, `*_shot_frame_coverage`, `shot_reading`; `control_changed_pixels` at least 1 |
| `orbit10k`, `boxes_shot` | `children` (orbit10k) or `bbox` (boxes_shot) on the root, then an orbit or a grab with only boxes drawn | `orbit_fps`, `boxes_first_drawn_ms`; skipped until the viewer publishes `natronSceneDrawBoxes` |
| `draw_budget` | `bbox` then clear on the sibling row and on the widest row, `BENCH_BUDGET_REPEATS` times (default 2), then once more each with a trace | `draw_changes`, one entry per change; `sibling_picture_ms_max`, `wide_picture_ms_max`; `traced_stall_ms` |

`frame_ms` is `total_ms` less `apply_ms`: from the call to the image that shows the change, without the
render thread's time sending notices. `total_ms` is polled every 50 ms, so it bounds the time rather
than measuring it. Frame figures are valid only on a real GPU, like the rest.

The 100 ms budget covers both the main-thread stall and the time from a change to the image that
shows it. `time_to_picture_ms` is judged on the worst untraced change of `prune_restore`,
`sibling_state` and `draw_budget` (and per row of `draw_budget`, as `time_to_picture_<row>_ms`); it is
valid only with `NATRON_DRAW_STATE_TIMING=1` on a real GPU. With that variable set the viewer keeps a
record per frame that takes in a change, from the slot call to the paint that presents it, and
publishes the last 256 as `natronSceneDrawTimings`; without it the viewer does none of this. Each
change's timings then gain the parts of `picture_ms`, each starting where the one before ends:

| Field | From, to |
| --- | --- |
| `queue_ms` | the slot call, the render thread's apply |
| `apply_ms` | the filter's notices to the render index (its Hydra prim inserts and removals included) |
| `wait_frame_ms` | the apply, the start of the frame's render calls |
| `render_ms` | the engine's render calls: Hydra sync, draw item regather, batch rebuild, GL submission |
| `gpu_tail_ms` | the GPU finishing after the last call; the render thread waits for it only in this mode |
| `handoff_ms` | the frame's arrival on the main thread |
| `present_ms` | the paint that shows it |

with `render_passes`, `rendered_on_paint` (the frame was rendered by the paint itself, so there is no
GPU tail or handoff), `coalesced_changes` (changes taken in by the same frame), `hidden_sent` and
`shown_sent`. A traced change runs `TraceCollector` and `HdPerfLog` from its apply to the end of its
frame and adds `stages_ms` and `stage_counts` (render-thread wall time and calls per scope:
`sync` for `HdRenderIndex::SyncAll`, `regather` for `HdSt_RenderPass::_UpdateDrawItems`, `batchRebuild`
for `HdStCommandBuffer::_RebuildDrawBatches`, `executeDraw` for `HdStCommandBuffer::ExecuteDraw`, and
the others in `Gui/SceneDrawTiming.cpp`'s table; nested scopes are counted in both), `counter_deltas` and
`counter_values` (HdPerfLog's `rebuildBatches`, `drawItemsFetched` and the like), `trace_collect_ms`,
and `trace_thread_found` (false: the scopes were summed over every thread). The viewer reads a trace on
a thread of its own after the frame, so `trace_collect_ms` is in none of the other parts; the record
says `tracePending` until then, and `draw_budget` waits for it before the next change. Tracing slows
the change, so traced changes and their stall are never judged.
`sibling_shot` skips while payloads are unloaded, as nothing is drawn for the control to change.

Every record carries `gl_renderer`, `gl_vendor`, `peak_rss_mb`, `peak_rss_gb` and
`mem_available_min_gb`. `heavy_scene_watchdog.sh` watches the run from outside Natron, so a hung run
still leaves a record: `timed_out` past the wall-clock limit (`BENCH_SCENARIO_LIMIT_S`,
`BENCH_OPEN_LIMIT_S`), `out_of_memory` when its memory or swap guard ends the process, `aborted` when
the process ends on its own, and `skipped` for the scenarios left in that process.

```
docker exec -e BENCH_FIXTURE=flat -e BENCH_SCENARIOS=open,orbit -e GUI_TEST_TIMEOUT=1800 natron-dev \
    Tests/gui/run-gui-test.sh tools/bench/heavy_scene_bench.py build/release/Natron.AppImage
```

On a host with a real GPU, from a checkout of the branch under test:

```
DISPLAY=:0 XAUTHORITY=$HOME/.Xauthority tools/bench/heavy_scene_gpu_host.sh gate /path/to/Natron.AppImage
```

`gate` runs, one fresh process each: flat open; city open with a one-building load, a one-block load
with descendants, and 30 blocks with descendants at once, the mouse moving throughout; a city twirl;
the draw state scenarios on flat, on the fully loaded city and on Lobby, with `populate_pruned` runs of
their own on flat and the city; then Lobby's building, block and twirl runs, and Island's building and twirl runs last, skipped under
`ISLAND_MIN_RAM_GB`. Each scene's draw run ends with `draw_budget` and runs with
`NATRON_DRAW_STATE_TIMING=1`, so the gate holds the budget records and its `prune_restore` and
`sibling_state` times to picture are valid; the timing costs a `glFinish` on every frame that takes in
a change, reveal steps included. The results go to `build/bench-gpu/heavy-scene-gate.jsonl`.

```
DISPLAY=:0 XAUTHORITY=$HOME/.Xauthority tools/bench/heavy_scene_gpu_host.sh budget /path/to/Natron.AppImage
```

`budget` runs only `draw_budget`, with `NATRON_DRAW_STATE_TIMING=1`, for a budget table without the gate: on flat after `populate_full`, on
the city and Lobby after `populate_full` and `load_all`, and on Island after `populate_full`, under its
RAM guard. Both modes end with `heavy_scene_budget_table.py`'s table: each scene's stall and time to
picture against 100 ms, one line per change with the parts above, and each traced change's stages and
counters. The results go to `build/bench-gpu/heavy-scene-budget.jsonl`. The script's header
lists its environment; `SCENE_DIR`, `LOBBY_DIR` and `ISLAND_DIR` say where the scenes are, and Docker
is only needed while the fixtures are missing.

### End-to-end budget run

`workflow` chains what a user does on one scene in one process: `open`, a twirl into a payload,
Full on one asset, Full + Load on a block, a pattern node (a USDEditTransform with a `primPattern`,
`BENCH_PATTERN` or the branch's descendants) inserted below USDRead and viewed, a switch to that node
with the Scene Graph panel following the selection, a prim selected in the inspector, a project save,
and a reset and reopen of the saved project. Each step has its own `ms`, `stall_ms` and numbers under
`workflow_steps`, and its own budgets in the record: `<step>_stall_ms` (100 ms, the setup before a
step not counted), `full_asset_picture_ms` (100 ms, valid with `NATRON_DRAW_STATE_TIMING=1` on a real
GPU), `full_load_block_complete` and `full_load_block_worst_frame_ms` (the reveal finishes with no
gap over 100 ms between painted frames), `reopen_first_frame_ms` (2 s, on a scene with payloads), and
the first-frame and unrequested-payload budgets over the whole run. The save is judged as a stall: a
`saveProjectAs` call that blocks the main thread over 100 ms fails. A scene with nothing for a step
(flat has no payloads to twirl or load) gets `not_applicable` there. The reopen leaves the process's
nodes gone, so `workflow` has to be the last scenario of its run; scenarios after it are skipped.

On a GPU host, from a checkout of the branch under test, one command runs it on both fixtures, Lobby
and Island, with the loaded reveal, cancel and orbit scenarios beside it:

```
DISPLAY=:0 XAUTHORITY=$HOME/.Xauthority tools/bench/heavy_scene_gpu_host.sh acceptance /path/to/Natron.AppImage
```

`acceptance` runs one fresh process per line: `workflow` on flat, the city, Lobby and Island; on the
loaded city, Lobby and Island `populate_full`, `load_all`, `reveal_wide`, `reveal_cancel_paced` and
`orbit10k`; and `reveal_load_all` on the unloaded city. Island (119,220 meshes, 30.9M instances) is
looked for by name under `ISLAND_DIR` or `SCENE_DIR` (the main checkout's `.local` by default) and
skipped with a message and a `skipped` record when it is absent or when less than `ISLAND_MIN_RAM_GB`
is available; its loaded run raises `BENCH_PICK_LIMIT_S` to an hour, as picking rows from the fully
loaded scene is slow. The fixtures are made through the dev image when `build/bench-gpu` lacks them.
The results go to `build/bench-gpu/heavy-scene-acceptance.jsonl`, with the table and the pass, fail and
did-not-finish counts at the end. A single scene runs the same way through `ONLY_LABELS`
(`accept-city`, `accept-island`, ...), or by hand:

```
BENCH_FIXTURE=city BENCH_SCENARIOS=workflow NATRON_DRAW_STATE_TIMING=1 BENCH_GPU=1 \
    Tests/gui/run-gui-test.sh tools/bench/heavy_scene_bench.py /path/to/Natron.AppImage
```
