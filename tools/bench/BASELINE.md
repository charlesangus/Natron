# Baseline 2026-09-25 at b0e212d5a

Machine: 4-core Intel N100, 15 GB RAM; release build running in the `natron-dev` container.

Run-to-run noise is up to 2x, so compare ratios between rows, not absolute values.

Commands run from the repo root. The stack samples in `build/bench/samples-*.txt` came from `tools/bench/profile_run.sh` on chain:1000 tiny, comp:300 tiny and chain:30 hd; the sample count and interval were not recorded. Records with `named` absent (the tiny chain rows) predate that field, so whether node names were explicit is not recorded for them.

Commands:

```
tools/bench/run_matrix.sh tiny tiny 5 0 chain:0,10,30,100,300,1000,3000 wide:10,30,100,300,1000,3000 comp:10,30,100,300
tools/bench/run_matrix.sh hd hd 3 0 chain:0,10,30,100,300 mixed:10,30,100,300 wide:10,30,100,300 comp:10,30,100,300
tools/bench/run_matrix.sh hdrange hd 1 8 chain:100 comp:100 wide:100
```

Frames per record: `frames` is the length of `frame_wall_s`; `range` is `range_frames`. Fields recorded per record: topo, n, nodes, res, named, build_s, warm_wall_s, warm_cpu_s, frame_wall_s, frame_wall_med_s, frame_cpu_s, parallelism, range_frames, range_wall_s, range_cpu_s, rss_before_mb, rss_peak_mb, counts.

## results-tiny.jsonl

| topo | n | res | named | frames | range frames | build (s) | median frame (s) | parallelism | range wall (s) | RSS before render (MB) |
|---|---|---|---|---|---|---|---|---|---|---|
| chain | 0 | tiny | absent | 5 | 0 | 0.03 | 0.0017 | 1.12 |  | 109 |
| chain | 10 | tiny | absent | 5 | 0 | 0.04 | 0.007 | 0.93 |  | 116 |
| chain | 30 | tiny | absent | 5 | 0 | 0.20 | 0.0168 | 1.00 |  | 128 |
| chain | 100 | tiny | absent | 5 | 0 | 0.72 | 0.0558 | 0.97 |  | 171 |
| chain | 300 | tiny | absent | 5 | 0 | 3.59 | 0.2673 | 0.96 |  | 295 |
| chain | 1000 | tiny | absent | 5 | 0 | 41.01 | 1.67 | 0.99 |  | 727 |
| chain | 3000 | tiny | absent | 5 | 0 | 914.37 | 16.42 | 0.99 |  | 1962 |
| wide | 10 | tiny | True | 5 | 0 | 0.06 | 0.0094 | 0.99 |  | 118 |
| wide | 30 | tiny | True | 5 | 0 | 0.18 | 0.0216 | 1.01 |  | 139 |
| wide | 100 | tiny | True | 5 | 0 | 0.83 | 0.0957 | 0.97 |  | 207 |
| wide | 300 | tiny | True | 5 | 0 | 2.35 | 0.3114 | 0.97 |  | 408 |
| wide | 1000 | tiny | True | 5 | 0 | 13.02 | 0.9599 | 0.98 |  | 1107 |
| wide | 3000 | tiny | True | 5 | 0 | 68.53 | 3.386 | 1.00 |  | 3106 |
| comp | 10 | tiny | True | 5 | 0 | 0.08 | 0.0131 | 0.95 |  | 123 |
| comp | 30 | tiny | True | 5 | 0 | 0.20 | 0.0288 | 0.98 |  | 139 |
| comp | 100 | tiny | True | 5 | 0 | 0.76 | 0.1205 | 1.02 |  | 193 |
| comp | 300 | tiny | True | 5 | 0 | 3.57 | 1.267 | 1.16 |  | 357 |

## results-hd.jsonl

| topo | n | res | named | frames | range frames | build (s) | median frame (s) | parallelism | range wall (s) | RSS before render (MB) |
|---|---|---|---|---|---|---|---|---|---|---|
| chain | 0 | hd | True | 3 | 0 | 0.01 | 0.1751 | 1.35 |  | 109 |
| chain | 10 | hd | True | 3 | 0 | 0.04 | 1.448 | 1.41 |  | 115 |
| chain | 30 | hd | True | 3 | 0 | 0.19 | 3.814 | 1.40 |  | 128 |
| chain | 100 | hd | True | 3 | 0 | 0.77 | 19.61 | 1.03 |  | 171 |
| chain | 300 | hd | True | 3 | 0 | 4.51 | 33.94 | 1.50 |  | 294 |
| mixed | 10 | hd | True | 3 | 0 | 0.07 | 2.274 | 1.60 |  | 120 |
| mixed | 30 | hd | True | 3 | 0 | 0.22 | 6.279 | 1.72 |  | 135 |
| mixed | 100 | hd | True | 3 | 0 | 0.95 | 23.88 | 1.66 |  | 188 |
| mixed | 300 | hd | True | 3 | 0 | 4.03 | 94.58 | 1.76 |  | 338 |
| wide | 10 | hd | True | 3 | 0 | 0.05 | 0.744 | 1.27 |  | 117 |
| wide | 30 | hd | True | 3 | 0 | 0.20 | 2.118 | 1.52 |  | 138 |
| wide | 100 | hd | True | 3 | 0 | 0.79 | 6.473 | 1.58 |  | 208 |
| wide | 300 | hd | True | 3 | 0 | 3.01 | 20.81 | 1.59 |  | 408 |
| comp | 10 | hd | True | 3 | 0 | 0.08 | 1.573 | 1.68 |  | 122 |
| comp | 30 | hd | True | 3 | 0 | 0.17 | 3.379 | 1.61 |  | 139 |
| comp | 100 | hd | True | 3 | 0 | 0.82 | 11.71 | 1.74 |  | 193 |
| comp | 300 | hd | True | 3 | 0 | 3.67 | 39.24 | 1.70 |  | 357 |

## results-hdrange.jsonl

| topo | n | res | named | frames | range frames | build (s) | median frame (s) | parallelism | range wall (s) | RSS before render (MB) |
|---|---|---|---|---|---|---|---|---|---|---|
| chain | 100 | hd | True | 1 | 8 | 0.58 | 9.269 | 1.73 | 66.7 | 171 |
| comp | 100 | hd | True | 1 | 8 | 0.78 | 11.34 | 1.78 | 74.3 | 193 |
| wide | 100 | hd | True | 1 | 8 | 0.69 | 5.399 | 1.77 | 36.5 | 208 |
