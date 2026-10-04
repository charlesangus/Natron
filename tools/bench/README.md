# Render-scaling benchmarks

Scripts for measuring how Natron's graph construction and rendering scale with node count, and
for finding where the time goes. Everything runs in the `natron-dev` container against
`build/release`.

| File | Purpose |
|---|---|
| `graph_bench.py` | Builds one synthetic graph in NatronRenderer, renders it, appends a JSON result line. |
| `run_matrix.sh` | Runs `graph_bench.py` over topologies and sizes, one process per configuration. |
| `profile_run.sh` | Runs one configuration and samples its stacks with eu-stack once the graph is built. |
| `sample_stacks.sh` | The eu-stack sampler (needs `docker exec -u root --privileged` for ptrace). |
| `sample_states.sh` | Counts running threads from `/proc` without stopping the process; use it for concurrency. |
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
