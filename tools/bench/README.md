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
| `make_plates.py` | Writes the synthetic EXR plates and the deep sequence the plate-fed families read. |
| `classify.py` | Prints a boundedness verdict (CPU, bandwidth, IO, RAM, host overhead) per result record. |

Topologies: `chain` (N Grades in series), `mixed` (Grade/Blur/Transform/ColorCorrect in series),
`wide` (sources merged by a balanced tree), `comp` (seeded random DAG with fan-out and merges).
Per-node chains isolate one node's cost: `ccchain` and `blurchain` (N ColorCorrects or Blurs),
`xfchain` (N Transform + Grade pairs, since Transforms in series concatenate), `mergechain` (N
Merges, each with the same second source) and `mergesrcchain` (each Merge with a fresh
CheckerBoard). Take the marginal cost between two sizes, and subtract `chain` from `xfchain` and
`mergechain` from `mergesrcchain`.
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
