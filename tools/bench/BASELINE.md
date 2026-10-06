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

## After M62 (2026-10-04, 07349f257)

Machine: 4-core Intel N100, 15 GB RAM; `build/release` at `07349f257` running in the `natron-dev` container, one configuration at a time. Run-to-run noise is up to 2x, so compare ratios between rows. The "before" for every ratio is the M62-start baseline above (`results-m62base-*.jsonl`); rows without an m62base counterpart are compared by hand against the September `results-tiny.jsonl` / `results-hd.jsonl`.

Commands run from the repo root (`BENCH_TIMEOUT=3600` throughout):

```
tools/bench/run_matrix.sh m62after-tiny tiny 5 0 chain:100,1000,3000 wide:1000,3000 comp:300,1000
BENCH_NAMED=0 tools/bench/run_matrix.sh m62after-tiny-unnamed tiny 5 0 chain:100,1000
tools/bench/run_matrix.sh m62after-hd hd 3 0 chain:30,100 mixed:100 wide:100 comp:100
tools/bench/profile_run.sh m62after-chain1000 60 0.5 BENCH_TOPO=chain BENCH_N=1000 BENCH_FRAMES=40 BENCH_RES=tiny
tools/bench/profile_run.sh m62after-comp300 60 0.5 BENCH_TOPO=comp BENCH_N=300 BENCH_FRAMES=40 BENCH_RES=tiny
tools/bench/profile_run.sh m62after-hdchain30 60 0.5 BENCH_TOPO=chain BENCH_N=30 BENCH_FRAMES=20 BENCH_RES=hd
python3 tools/bench/analyze_stacks.py build/bench/samples-m62after-<name>.txt --top 15
tools/bench/sample_states.sh <pid> 600 0.1     # on a live chain:30 hd, 20 frames run; output in build/bench/states-hdchain30.txt
python3 tools/bench/compare.py build/bench/results-m62base-<tiny|tiny-unnamed|hd>.jsonl build/bench/results-m62after-<...>.jsonl
```

Results are in `build/bench/results-m62after-tiny.jsonl`, `results-m62after-tiny-unnamed.jsonl` and `results-m62after-hd.jsonl`. All 14 configurations exited 0; the whole matrix, profiles and states run took about 8 minutes.

| topo | n | res | named | build (s) | median frame (s) | parallelism | RSS before render (MB) |
|---|---|---|---|---|---|---|---|
| chain | 100 | tiny | True | 0.34 | 0.0306 | 1.0 | 173 |
| chain | 1000 | tiny | True | 6.55 | 0.3203 | 1.0 | 730 |
| chain | 3000 | tiny | True | 47.01 | 1.1225 | 1.0 | 1967 |
| wide | 1000 | tiny | True | 7.05 | 0.6497 | 1.0 | 1110 |
| wide | 3000 | tiny | True | 41.52 | 2.0576 | 1.0 | 3111 |
| comp | 300 | tiny | True | 1.32 | 0.2676 | 1.28 | 359 |
| comp | 1000 | tiny | True | 6.81 | 3.4466 | 2.09 | 921 |
| chain | 100 | tiny | False | 0.31 | 0.0301 | 1.0 | 173 |
| chain | 1000 | tiny | False | 6.93 | 0.3159 | 1.0 | 730 |
| chain | 30 | hd | True | 0.09 | 1.2958 | 2.61 | 130 |
| chain | 100 | hd | True | 0.31 | 5.3744 | 2.67 | 173 |
| mixed | 100 | hd | True | 0.36 | 12.2405 | 2.54 | 190 |
| wide | 100 | hd | True | 0.44 | 3.2296 | 2.55 | 209 |
| comp | 100 | hd | True | 0.38 | 6.6549 | 2.58 | 195 |

### Against the M62-start baseline

`compare.py` output (matching records; "only in after" notes trimmed to the rows handled by hand below):

```
tiny:
chain n=100 res=tiny named=True    build_s 0.4031->0.3426 x0.85  frame_s 0.0332->0.0306 x0.92  parallelism 1->1 x1.00  rss_mb 173->173 x1.00
chain n=1000 res=tiny named=True   build_s 16.77->6.551 x0.39  frame_s 0.7266->0.3203 x0.44  parallelism 1->1 x1.00  rss_mb 731->730 x1.00
comp n=300 res=tiny named=True     build_s 2.105->1.318 x0.63  frame_s 0.8374->0.2676 x0.32  parallelism 1.42->1.28 x0.90  rss_mb 359->359 x1.00
wide n=1000 res=tiny named=True    build_s 7.119->7.05 x0.99  frame_s 0.6223->0.6497 x1.04  parallelism 1->1 x1.00  rss_mb 1111->1110 x1.00

tiny-unnamed:
chain n=100 res=tiny named=False   build_s 0.4361->0.3059 x0.70  frame_s 0.0344->0.0301 x0.88  parallelism 1->1 x1.00  rss_mb 173->173 x1.00
chain n=1000 res=tiny named=False  build_s 23.07->6.932 x0.30  frame_s 0.7253->0.3159 x0.44  parallelism 1->1 x1.00  rss_mb 731->730 x1.00

hd:
chain n=30 res=hd named=True       build_s 0.095->0.0903 x0.95  frame_s 1.752->1.296 x0.74  parallelism 2.38->2.61 x1.10  rss_mb 130->130 x1.00
```

No row is flagged at the 1.5 threshold. Rows with no m62base record, compared by hand against September (build s, median frame s):

| config | September | After M62 | build ratio | frame ratio |
|---|---|---|---|---|
| chain 3000 tiny | 914.37, 16.42 | 47.01, 1.1225 | x0.051 | x0.068 |
| wide 3000 tiny | 68.53, 3.386 | 41.52, 2.0576 | x0.61 | x0.61 |
| comp 1000 tiny | never finished building | 6.81, 3.4466 | | |
| chain 100 hd | 0.77, 19.61 | 0.31, 5.3744 | x0.40 | x0.27 |
| mixed 100 hd | 0.95, 23.88 | 0.36, 12.2405 | x0.38 | x0.51 |
| wide 100 hd | 0.79, 6.473 | 0.44, 3.2296 | x0.55 | x0.50 |
| comp 100 hd | 0.82, 11.71 | 0.38, 6.6549 | x0.46 | x0.57 |

Chain tiny frame time per node: 0.306 ms (n=100), 0.320 ms (n=1000), 0.374 ms (n=3000). wide:1000 tiny did not change against m62base (build x0.99, frame x1.04).

### Profiles

Stack samples: `build/bench/samples-m62after-chain1000.txt`, `samples-m62after-comp300.txt`, `samples-m62after-hdchain30.txt`. The runs finished before the 60 samples were taken (render is much faster now), so the sample counts are 22, 16 and 47 and the busy thread-sample counts are 15, 19 and 114. The chain:1000 and comp:300 percentages rest on 15 and 19 thread-samples and are low confidence. Mean busy threads per sample: 0.68, 1.19, 2.43 (eu-stack stops the process while walking, so these understate concurrency).

chain:1000 tiny, top 10 self time (15 busy thread-samples; activity: plugin:render 46.7%, engine:other 26.7%, libc alloc/copy 13.3%, plugin:beginEnd 6.7%, engine:components 6.7%):

| % | function |
|---|---|
| 20.0 | `_int_malloc` (libc) |
| 13.3 | `unlink_chunk.constprop.0` (libc) |
| 6.7 | `Natron::OfxBooleanInstance::get` |
| 6.7 | `OFX::Image::getPixelAddress` (Misc.ofx) |
| 6.7 | `OFX::PropertySet::propGetString` (Misc.ofx) |
| 6.7 | `Natron::KnobHelper::getAllExpressionDependenciesRecursive` |
| 6.7 | `Natron::Node::setNodeIsRenderingInternal` |
| 6.7 | `void` (Misc.ofx) |
| 6.7 | `free` (libc) |
| 6.7 | `OFX::Host::Property::PropertyTemplate<IntValue>::getValue` |

Top inclusive, chain:1000: `renderRoI`, `treeRecurseFunctor`, `renderInputImagesForRoI` 86.7%; `renderRoIInternal` 66.7%; `OfxEffectInstance::render`, `renderHandler`, `OFX::Host::ImageEffect::Instance::mainEntry`, `renderAction`, `tiledRenderingFunctor`, `render_public` 53.3%; `_int_malloc` 33.3%; `operator new` 26.7%; `OFX::Host::Property::Set::Set` / `createProperty` 20.0%. `isFrameVaryingOrAnimated_impl`, `getInput`, `applyNodeRedirectionsUpstream` (56%, 46%, 30% at m62base) no longer appear. `copyTLSFromSpawnerThreadInternal` and `cleanupTLSForThread`: 0 samples in any of the three files.

comp:300 tiny, top 10 self time (19 busy thread-samples; activity: plugin:other 47.4%, plugin:render 36.8%, engine:other 10.5%, engine:image alloc/fill 5.3%; 5 blocked samples, all in `OfxHost::multiThread`):

| % | function |
|---|---|
| 26.3 | (unnamed frame) (Misc.ofx) |
| 10.5 | `ofxsMaskMixPix<float, 4, 1, true>` (Misc.ofx) |
| 10.5 | `CImg<float>::_cimg_recursive_apply` (CImg.ofx) |
| 5.3 | `double` (Misc.ofx) |
| 5.3 | `Image::checkForNaNsAndFix` |
| 5.3 | `_Sp_counted_ptr_inplace<std::map<int, std::map<ViewIdx, vector<OfxRangeD>>>>` |
| 5.3 | `OFX::Host::Property::Set::fetchProperty` |
| 5.3 | `QObject::thread` |
| 5.3 | `PixelCopierPremultMaskMix<float,4,1,float,4,1>::multiThreadProcessImages` (CImg.ofx) |
| 5.3 | `EffectInstance::aborted` |

Top inclusive, comp:300: `OFX::ImageProcessor::multiThreadFunction` 57.9%, `QThreadPoolThread::run` 52.6%, `renderInputImagesForRoI`/`renderRoI`/`treeRecurseFunctor` 47.4%, `renderHandler`/`renderRoIInternal`/`tiledRenderingFunctor` 42.1%, `OfxEffectInstance::render` 36.8%, `DefaultRenderFrameRunnable::renderFrame` 31.6%.

chain:30 hd, top 10 self time (114 busy thread-samples; activity: plugin:other 80.7%, engine:image alloc/fill 12.3%, engine:other 4.4%, plugin:render 2.6%; 36 blocked samples: 27 in `OfxHost::multiThread`, 6 in `Natron::` (unnamed), 3 in `OpenEXROutput::write_scanlines`):

| % | function |
|---|---|
| 36.8 | (unnamed frame) (Misc.ofx) |
| 23.7 | `void` (Misc.ofx) |
| 8.8 | `ofxsPremult<float, 4, 1>` (Misc.ofx) |
| 7.9 | `Image::copyUnProcessedChannelsForChannels<float,1,4,4,false,false,false,true>` row lambda (NatronRenderer) |
| 7.0 | `ofxsMaskMixPix<float, 4, 1, true>` (Misc.ofx) |
| 4.4 | `Image::checkForNaNsAndFix` |
| 2.6 | `OFX::Image::getPixelAddress` (Misc.ofx) |
| 1.8 | `internal_exr_apply_zip` (OpenEXR) |
| 1.8 | `deflate_compress_greedy` (libdeflate) |
| 0.9 | `__memcpy_avx_unaligned_erms` (libc) |

Top inclusive, chain:30 hd: `QtConcurrent::ThreadEngineBase::run` 88.6%, `OFX::ImageProcessor::multiThreadFunction` 79.8%, `MapKernel<RectI>` / `forEachCopyUnProcessedRowBand` 7.9%, `tiledRenderingFunctor` / `renderRoIInternal` / `renderRoI` 7.0%, `DefaultRenderFrameRunnable::renderFrame` 5.3%, `Image::checkForNaNsAndFix` 4.4%, `internal_exr_apply_zip` 3.5%. `Image::pixelAt` does not appear (m62base: 6.4%).

hdchain30 thread-state histogram (`sample_states.sh`, 279 samples at 0.1 s plus overhead over one live chain:30 hd, 20-frame run; count of samples by running-thread count, D-state count was 0 in every sample):

| running threads | samples | share |
|---|---|---|
| 0 | 2 | 0.7% |
| 1 | 33 | 11.8% |
| 2 | 6 | 2.2% |
| 3 | 233 | 83.5% |
| 4 | 5 | 1.8% |

Mean running threads 2.74. The window includes about 2 s before rendering starts and the process exit.

### Gate checks

- FAIL: tiny chain 1000 frame >=3x faster than m62base. 0.7266 s -> 0.3203 s, x2.27 faster (needed <=0.242 s).
- PASS: tiny chain 3000 frame under 3 s. 1.1225 s.
- PASS: chain 3000 frame <= ~3.5x chain 1000 frame. 1.1225 / 0.3203 = x3.50.
- PASS: comp 1000 builds and renders. Build 6.81 s, median frame 3.4466 s (September: never finished building).
- PASS: `BENCH_NAMED=0` chain 1000 build within 1.2x of named. 6.932 s / 6.551 s = x1.06 (m62base: x1.38).
- FAIL: chain 1000 build (named) >=5x faster than m62base. 16.77 s -> 6.551 s, x2.56 faster (needed <=3.35 s).
- PASS: HD chain 30 frame improved vs m62base 1.752 s: 1.2958 s (x0.74). PASS: `copyUnProcessedChannels*` below 10% of busy self time: 7.9% (m62base 10.1%), `pixelAt` absent (m62base 6.4%).
- PASS: `copyTLSFromSpawnerThreadInternal` / `cleanupTLSForThread` absent or <2% self time in the chain1000 profile: 0 samples (15 busy thread-samples, so the bound is coarse).
- PASS: tiny wide 1000 and HD chain 30 not slower than 1.5x m62base. wide 1000 x1.04 (0.6223 -> 0.6497 s); HD chain 30 x0.74.
- PASS: no configuration more than 1.5x slower than m62base. `compare.py` flagged nothing; unmatched rows are all faster than September (table above).

### Input for M63 (task-graph scheduler)

HD chain 30 (20 frames, 1.2958 s median frame, parallelism 2.61 from the matrix run): in the live thread-state histogram 274 of 279 samples (98.2%) had fewer than 4 threads running; 83.5% had exactly 3, 11.8% had 1 and 1.8% had 4. A frame never keeps the 4 cores fully busy; the dominant state is 3 running threads.

Split of the per-frame cost in the hdchain30 profile (114 busy thread-samples, share of busy self time): plugin pixel work (Misc.ofx frames: unnamed 36.8%, `void` 23.7%, `ofxsPremult` 8.8%, `ofxsMaskMixPix` 7.0%, `getPixelAddress` 2.6%) is 78.9%. Engine per-image work (row-based unprocessed-channel copy 7.9%, `checkForNaNsAndFix` 4.4%, `aborted` 0.9%, memcpy/fill 0.9%) is about 14%. The EXR writer (zip, deflate, convert) is about 5%. The per-node serial host work (request pass, render-args setup, metadata, identity, cache, TLS) does not register as a self-time function at HD; `engine:other` is 4.4% of busy samples.

Per-node serial host overhead measured directly from the tiny chain, where plugin pixel work is negligible: 0.32 ms per node per frame (chain 1000: 0.3203 s; chain 3000: 0.374 ms per node). HD chain 30 costs 1.2958 s / 30 = 43.2 ms per node per frame, so the host overhead is at most about 0.7% of the HD per-node cost; at HD the remaining per-frame cost is plugin pixel work and image-sized engine passes. In the chain:1000 tiny profile (15 busy thread-samples) plugin render and begin/end frames are 53% and engine/libc frames 47%, dominated by property-set construction (`Property::Set::Set`, 20% inclusive) and malloc/free (33% inclusive).
