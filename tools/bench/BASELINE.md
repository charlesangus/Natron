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

## Baseline at M62 start (2026-10-03, ab2b06c90)

Machine: 4-core Intel N100, 15 GB RAM; `build/release` at `ab2b06c90` running in the `natron-dev` container. Run-to-run noise is up to 2x, so compare ratios between rows. `run_matrix.sh` now forwards `BENCH_NAMED` (default 1) to the container.

Commands run from the repo root:

```
tools/bench/run_matrix.sh m62base-tiny tiny 5 0 chain:100,1000 wide:1000 comp:300
BENCH_NAMED=0 BENCH_TIMEOUT=3600 tools/bench/run_matrix.sh m62base-tiny-unnamed tiny 5 0 chain:100,1000
tools/bench/run_matrix.sh m62base-hd hd 3 0 chain:30
tools/bench/profile_run.sh m62base-chain1000 60 0.5 BENCH_TOPO=chain BENCH_N=1000 BENCH_FRAMES=40 BENCH_RES=tiny
tools/bench/profile_run.sh m62base-hdchain30 60 0.5 BENCH_TOPO=chain BENCH_N=30 BENCH_FRAMES=20 BENCH_RES=hd
python3 tools/bench/analyze_stacks.py build/bench/samples-m62base-chain1000.txt
python3 tools/bench/analyze_stacks.py build/bench/samples-m62base-hdchain30.txt
python3 tools/bench/compare.py build/bench/results-tiny.jsonl build/bench/results-m62base-tiny.jsonl
python3 tools/bench/compare.py build/bench/results-hd.jsonl build/bench/results-m62base-hd.jsonl
```

Results are in `build/bench/results-m62base-tiny.jsonl`, `results-m62base-tiny-unnamed.jsonl` and `results-m62base-hd.jsonl`.

| topo | n | res | named | build (s) | median frame (s) | parallelism | RSS before render (MB) |
|---|---|---|---|---|---|---|---|
| chain | 100 | tiny | True | 0.40 | 0.0332 | 1.0 | 173 |
| chain | 1000 | tiny | True | 16.77 | 0.7266 | 1.0 | 731 |
| chain | 100 | tiny | False | 0.44 | 0.0344 | 1.0 | 173 |
| chain | 1000 | tiny | False | 23.07 | 0.7253 | 1.0 | 731 |
| wide | 1000 | tiny | True | 7.12 | 0.6223 | 1.0 | 1111 |
| comp | 300 | tiny | True | 2.11 | 0.8374 | 1.42 | 359 |
| chain | 30 | hd | True | 0.095 | 1.752 | 2.38 | 130 |

### Against the 2026-09-25 baseline

`compare.py` output (matching records only; the "only in before" notes are trimmed):

```
comp n=300 res=tiny named=True     build_s 3.568->2.105 x0.59  frame_s 1.267->0.8374 x0.66  parallelism 1.16->1.42 x1.22  rss_mb 357->359 x1.01
wide n=1000 res=tiny named=True    build_s 13.02->7.119 x0.55  frame_s 0.9599->0.6223 x0.65  parallelism 0.98->1 x1.02  rss_mb 1107->1111 x1.00
chain n=30 res=hd named=True       build_s 0.1873->0.095 x0.51  frame_s 3.814->1.752 x0.46  parallelism 1.4->2.38 x1.70  rss_mb 128->130 x1.02
```

The September tiny chain rows have `named` absent, so `compare.py` finds no match for the new chain records (`named` True or False). Compared by hand against the September rows (build_s, median frame_s):

| chain tiny n | September | M62 start, unnamed | build ratio | frame ratio |
|---|---|---|---|---|
| 100 | 0.72 s, 0.0558 s | 0.436 s, 0.0344 s | x0.61 | x0.62 |
| 1000 | 41.01 s, 1.67 s | 23.07 s, 0.7253 s | x0.56 | x0.43 |

Every configuration measured is faster than September; none is slower. Median frame time ratios: chain:100 tiny x0.62, chain:1000 tiny x0.43, wide:1000 tiny x0.65, comp:300 tiny x0.66, chain:30 hd x0.46. Build time ratios: x0.51 to x0.61. RSS is unchanged (x1.00 to x1.02). Chain:1000 tiny with explicit names builds in 16.8 s against 23.1 s with default names. chain:30 hd parallelism went from 1.40 to 2.38.

### Profiles

Stack samples: `build/bench/samples-m62base-chain1000.txt` (60 samples, 0.5 s interval, chain:1000 tiny, 40 frames) and `build/bench/samples-m62base-hdchain30.txt` (chain:30 hd, 20 frames). Percentages are of busy thread-samples, which were few (50 and 109) because eu-stack stops the process while it walks stacks; chain:1000 had 0.94 busy threads per sample and chain:30 hd had 1.88.

Top 10 self-time functions, chain:1000 tiny (50 busy thread-samples):

| % | function |
|---|---|
| 10.0 | `unlink_chunk.constprop.0` (libc) |
| 8.0 | `__dynamic_cast` (libstdc++) |
| 8.0 | `__strcmp_avx2` (libc) |
| 8.0 | `std::_Sp_counted_base<` (NatronRenderer) |
| 6.0 | `Natron::Node::getEffectInstance` |
| 6.0 | `Natron::NodeMetadata::getIsFrameVarying` |
| 4.0 | `Natron::KnobHolder::getHasAnimation` |
| 4.0 | `void` (Misc.ofx) |
| 4.0 | `Natron::applyNodeRedirectionsUpstream` |
| 4.0 | `__memcmp_avx2_movbe` (libc) |

Top inclusive functions, chain:1000 tiny: `Natron::isFrameVaryingOrAnimated_impl` 56%, `Natron::EffectInstance::treeRecurseFunctor` 54%, `Natron::EffectInstance::getInput` 46%, `Natron::EffectInstance::renderRoI` 42%, `Natron::EffectInstance::renderInputImagesForRoI` 42%, `Natron::Node::getInput` 40%, `Natron::Node::getInputInternal` 40%, `Natron::EffectInstance::renderRoIInternal` 32%, `Natron::applyNodeRedirectionsUpstream` 30%, `OFX::Host::ImageEffect::Instance::renderAction` 28%. Activity: engine:other 52%, plugin:render 26%, engine:identity 10%, engine:metadata 6%.

Top 10 self-time functions, chain:30 hd (109 busy thread-samples):

| % | function |
|---|---|
| 21.1 | (unnamed frame) (Misc.ofx) |
| 17.4 | `void` (Misc.ofx) |
| 11.0 | `void OFX::ofxsMaskMixPix<float, 4, 1, true>` (Misc.ofx) |
| 10.1 | `void Natron::Image::copyUnProcessedChannelsForComponents<float, 1, 4, 4>` |
| 6.4 | `Natron::Image::pixelAt` |
| 6.4 | `deflate_compress_greedy` (libdeflate) |
| 4.6 | `Natron::Image::checkForNaNsAndFix` |
| 4.6 | `__memcpy_avx_unaligned_erms` (libc) |
| 4.6 | `std::__fill_a1<unsigned char>` (libOpenImageIO) |
| 3.7 | `void OFX::ofxsPremult<float, 4, 1>` (Misc.ofx) |

Activity, chain:30 hd: plugin:other 58.7%, engine:image alloc/fill 21.1%, engine:other 11.0%, plugin:render 7.3%. 25 thread-samples were blocked, 24 of them in `Natron::OfxHost::multiThread`.
