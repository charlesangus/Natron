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

Topologies: `chain` (N Grades in series), `mixed` (Grade/Blur/Transform/ColorCorrect in series),
`wide` (sources merged by a balanced tree), `comp` (seeded random DAG with fan-out and merges).
Resolutions: `tiny` (32x32, isolates per-node overhead), `hd`, `uhd`. Sources are animated so
nothing is cached as frame-invariant.

```
tools/bench/run_matrix.sh tiny tiny 5 0 chain:10,100,1000 wide:10,100,1000
tools/bench/profile_run.sh chain1000 60 0.5 BENCH_TOPO=chain BENCH_N=1000 BENCH_FRAMES=40
python3 tools/bench/analyze_stacks.py build/bench/samples-chain1000.txt
python3 tools/bench/compare.py build/bench/results-before.jsonl build/bench/results-after.jsonl --threshold 1.5
```

Results land in `build/bench/results-<tag>.jsonl`, logs in `build/bench/logs/`.

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
