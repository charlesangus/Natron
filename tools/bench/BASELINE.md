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

## M63 start (2026-10-04, 20aa6f102)

Machine: 4-core Intel N100, 15 GB RAM; `build/release` at `20aa6f102` (code identical to `07349f257`; only comments and scripts differ) in the `natron-dev` container, one configuration at a time. Single-frame numbers here are about 1.4-1.5x slower than After M62 on every row, including single-threaded graph builds, in two full runs (the first is kept in `build/bench/contended/`). The host load average was 12-19 with nothing of ours running, so another tenant was using the CPUs. Treat these as the M63-start reference for this machine state and compare M63 results against them, not against After M62, only when the machine is similarly loaded; ratios within one run are unaffected.

Commands run from the repo root (`BENCH_TIMEOUT=3600` throughout):

```
tools/bench/run_matrix.sh m63start-hd hd 3 8 chain:30,100 wide:100 comp:100 mixed:100
tools/bench/run_matrix.sh m63start-tiny tiny 5 0 chain:1000,3000 wide:1000 comp:300
SAMPLER=states tools/bench/profile_run.sh m63start-comp100 120 0.5 BENCH_TOPO=comp BENCH_N=100 BENCH_FRAMES=20 BENCH_RES=hd
SAMPLER=states tools/bench/profile_run.sh m63start-wide100 120 0.5 BENCH_TOPO=wide BENCH_N=100 BENCH_FRAMES=20 BENCH_RES=hd
python3 tools/bench/compare.py build/bench/results-m62after-<hd|tiny>.jsonl build/bench/results-m63start-<hd|tiny>.jsonl
```

Results are in `build/bench/results-m63start-hd.jsonl` and `results-m63start-tiny.jsonl`. All 9 configurations exited 0; the whole run took 954 s.

| topo | n | res | named | settings | build (s) | median frame (s) | parallelism | range frames | range per-frame wall (s) | range_parallelism | RSS before render (MB) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| chain | 30 | hd | True | - | 0.1255 | 2.6371 | 1.95 | 8 | 2.172 | 2.32 | 129 |
| chain | 100 | hd | True | - | 0.4758 | 7.6613 | 2.01 | 8 | 7.162 | 2.21 | 173 |
| wide | 100 | hd | True | - | 0.6591 | 4.888 | 1.9 | 8 | 4.514 | 2.2 | 209 |
| comp | 100 | hd | True | - | 0.4844 | 9.508 | 2.08 | 8 | 8.923 | 2.26 | 195 |
| mixed | 100 | hd | True | - | 0.7435 | 17.3746 | 2.09 | 8 | 16.541 | 2.28 | 190 |
| chain | 1000 | tiny | True | - | 10.0627 | 0.4537 | 1.0 | - | - | - | 730 |
| chain | 3000 | tiny | True | - | 69.3674 | 1.529 | 1.0 | - | - | - | 1967 |
| wide | 1000 | tiny | True | - | 10.9216 | 1.0536 | 1.0 | - | - | - | 1109 |
| comp | 300 | tiny | True | - | 2.3371 | 0.4762 | 1.11 | - | - | - | 358 |

### Against After M62

`compare.py` output (the range columns are absent because the After M62 records have no range):

```
hd:
chain n=30 res=hd named=True       build_s 0.0903->0.1255 x1.39  frame_s 1.296->2.637 x2.04  parallelism 2.61->1.95 x0.75  rss_mb 130->129 x0.99  FLAGGED
chain n=100 res=hd named=True      build_s 0.3123->0.4758 x1.52  frame_s 5.374->7.661 x1.43  parallelism 2.67->2.01 x0.75  rss_mb 173->173 x1.00  FLAGGED
comp n=100 res=hd named=True       build_s 0.3753->0.4844 x1.29  frame_s 6.655->9.508 x1.43  parallelism 2.58->2.08 x0.81  rss_mb 195->195 x1.00
mixed n=100 res=hd named=True      build_s 0.3626->0.7435 x2.05  frame_s 12.24->17.37 x1.42  parallelism 2.54->2.09 x0.82  rss_mb 190->190 x1.00  FLAGGED
wide n=100 res=hd named=True       build_s 0.4378->0.6591 x1.51  frame_s 3.23->4.888 x1.51  parallelism 2.55->1.9 x0.75  rss_mb 209->209 x1.00  FLAGGED

tiny:
chain n=1000 res=tiny named=True   build_s 6.551->10.06 x1.54  frame_s 0.3203->0.4537 x1.42  parallelism 1->1 x1.00  rss_mb 730->730 x1.00  FLAGGED
chain n=3000 res=tiny named=True   build_s 47.01->69.37 x1.48  frame_s 1.123->1.529 x1.36  parallelism 1->1 x1.00  rss_mb 1967->1967 x1.00
comp n=300 res=tiny named=True     build_s 1.318->2.337 x1.77  frame_s 0.2676->0.4762 x1.78  parallelism 1.28->1.11 x0.87  rss_mb 359->358 x1.00  FLAGGED
wide n=1000 res=tiny named=True    build_s 7.05->10.92 x1.55  frame_s 0.6497->1.054 x1.62  parallelism 1->1 x1.00  rss_mb 1110->1109 x1.00  FLAGGED
FLAGGED missing from after: topo=chain n=100 res=tiny named=True settings=''
FLAGGED missing from after: topo=comp n=1000 res=tiny named=True settings=''
FLAGGED missing from after: topo=wide n=3000 res=tiny named=True settings=''
```

The "missing" rows are configurations After M62 ran that this baseline does not (chain 100, comp 1000, wide 3000 tiny). Every present row is 1.3-2.0x slower and many are flagged at 1.5, uniformly across build time, single-threaded tiny frames and HD frames, which points at the machine and not the code.

### Occupancy

`build/bench/states-m63start-comp100.txt` and `states-m63start-wide100.txt`: HD single-frame run (20 frames), 120 samples at 0.5 s (first 60 s of the run). Columns are samples, running threads, uninterruptible threads.

```
comp 100 hd:                 wide 100 hd:
     38 1 0                       1 0 0
     26 2 0                      42 1 0
     55 3 0                      15 2 0
                                 61 3 0
                                  1 4 0
```

Mean running threads: comp 2.14, wide 2.16. Share of samples at 4 running threads: comp 0%, wide 0.8%. Three threads were the ceiling in practice on both graphs.

## After M63 (2026-10-04, e0ae0a582)

Machine: 4-core Intel N100, 15 GB RAM; `build/release` at `e0ae0a582` in the `natron-dev` container, one configuration at a time. Every matrix, states and stack run was started only after `/proc/loadavg` (1-minute) read below 1.5; the value seen before each run is listed. Modes are selected with `BENCH_SETTINGS=renderSchedulerMode=0` (Legacy) and `=1` (Task graph); `compare.py` keys on `settings`, so the modes were compared by hand. The machine was quiet (unlike the invalid M63-start section), and Legacy matches After M62 within noise.

Load before each run: HD legacy 0.73, tiny legacy 0.91, HD taskgraph 1.05, tiny taskgraph 1.22, first occupancy set (the comp/wide runs of it were overwritten by a script naming bug and rerun, see below) 0.95-1.26, P=8 sweep 1.17, P=16 sweep 1.41, pixel renders 1.07 / 1.23 / 1.19 / 1.08, occupancy rerun legacy comp 0.14, wide 1.35, chain 1.26, taskgraph comp 1.03, wide 1.36, chain 1.10, stack profiles comp 1.20, wide 1.45.

Commands run from the repo root (`BENCH_TIMEOUT=3600` throughout; `<M>` is 0 for legacy and 1 for taskgraph):

```
BENCH_SETTINGS=renderSchedulerMode=<M> tools/bench/run_matrix.sh m63after-<legacy|taskgraph>-hd hd 3 8 chain:30,100 wide:100 comp:100 mixed:100
BENCH_SETTINGS=renderSchedulerMode=<M> tools/bench/run_matrix.sh m63after-<legacy|taskgraph>-tiny tiny 5 0 chain:1000,3000 wide:1000 comp:300
SAMPLER=states tools/bench/profile_run.sh m63after-<mode>-<comp|wide>100 120 0.5 BENCH_TOPO=<comp|wide> BENCH_N=100 BENCH_FRAMES=20 BENCH_RES=hd BENCH_SETTINGS=renderSchedulerMode=<M>
SAMPLER=states tools/bench/profile_run.sh m63after-<mode>-chain30r8 120 0.5 BENCH_TOPO=chain BENCH_N=30 BENCH_FRAMES=1 BENCH_RANGE=8 BENCH_RES=hd BENCH_SETTINGS=renderSchedulerMode=<M>
(same chain run again at interval 0.1 as ...-chain30r8-fine, because the 0.5 s run ends after 6-8 s)
BENCH_SETTINGS="renderSchedulerMode=1;noRenderThreads=<8|16>" tools/bench/run_matrix.sh m63after-taskgraph-p<8|16> hd 3 0 comp:100 wide:100
BENCH_FRAMES=0 BENCH_WORK=build/bench/pix/<legacy|tg4|tg8|tg16> graph_bench.py (comp 100 hd, frame 1 only) in each configuration, then cmp
tools/bench/profile_run.sh m63after-tg-<comp|wide>100 60 0.5 BENCH_TOPO=<comp|wide> BENCH_N=100 BENCH_FRAMES=20 BENCH_RES=hd BENCH_SETTINGS=renderSchedulerMode=1
```

Results: `build/bench/results-m63after-{legacy,taskgraph}-{hd,tiny}.jsonl`, `results-m63after-taskgraph-p{8,16}.jsonl`. All configurations exited 0. The whole run (first pass 2271 s plus the 876 s rerun) took about 53 minutes. The first occupancy and stack runs for comp and wide wrote to a shared file name (a `$c100` shell typo), so only wide survived; the rerun produced `states-m63after-<mode>-{comp,wide}100.txt` and `samples-m63after-tg-{comp,wide}100.txt`.

### HD (3 timed frames; range of 8 frames)

Ratio is taskgraph / legacy (below 1 is faster for taskgraph). Parallelism and RSS columns read legacy / taskgraph.

| topo | n | build s (L / T) | legacy frame s | taskgraph frame s | ratio | parallelism | range per-frame s (L / T) | range ratio | RSS before MB |
|---|---|---|---|---|---|---|---|---|---|
| chain | 30 | 0.093 / 0.092 | 0.9939 | 1.5114 | 1.52 | 3.17 / 2.62 | 0.847 / 1.610 | 1.90 | 129 / 129 |
| chain | 100 | 0.315 / 0.304 | 4.9201 | 5.2390 | 1.06 | 3.26 / 2.65 | 4.610 / 5.143 | 1.12 | 172 / 172 |
| wide | 100 | 0.476 / 0.465 | 3.1868 | 3.0666 | 0.96 | 2.91 / 3.30 | 2.907 / 2.880 | 0.99 | 208 / 208 |
| comp | 100 | 0.384 / 0.382 | 6.2120 | 6.2841 | 1.01 | 3.07 / 2.64 | 5.723 / 5.823 | 1.02 | 194 / 194 |
| mixed | 100 | 0.357 / 0.378 | 10.9764 | 12.0348 | 1.10 | 3.08 / 2.49 | 10.453 / 11.381 | 1.09 | 189 / 189 |

The chain 30 taskgraph row is noisy: the 1-frame rerun in the occupancy session gave 1.135 s frame and 0.995 s per range frame against 0.976 s and 0.886 s legacy (ratios 1.16 and 1.12). Three timed frames per config is a small sample; frame-to-frame variation inside the 20-frame runs is up to 2x on wide (see below).

### Tiny (5 timed frames, no range)

| topo | n | build s (L / T) | legacy frame s | taskgraph frame s | ratio | parallelism (L / T) | RSS before MB |
|---|---|---|---|---|---|---|---|
| chain | 1000 | 6.51 / 6.53 | 0.3320 | 0.3234 | 0.97 | 1.00 / 1.01 | 729 / 729 |
| chain | 3000 | 46.74 / 46.72 | 1.0712 | 1.0267 | 0.96 | 1.00 / 1.01 | 1966 / 1966 |
| wide | 1000 | 7.03 / 7.02 | 0.6805 | 0.5178 | 0.76 | 1.00 / 1.44 | 1108 / 1109 |
| comp | 300 | 1.32 / 1.36 | 0.2497 | 0.2169 | 0.87 | 1.26 / 1.55 | 357 / 357 |

### Legacy against After M62

`compare.py results-m62after-hd.jsonl results-m63after-legacy-hd.jsonl` matches nothing (After M62 rows have `settings=''`, these have `renderSchedulerMode=0`), so the median frame was compared by hand:

| config | After M62 s | M63 legacy s | ratio |
|---|---|---|---|
| chain 30 hd | 1.2958 | 0.9939 | 0.77 |
| chain 100 hd | 5.3744 | 4.9201 | 0.92 |
| wide 100 hd | 3.2296 | 3.1868 | 0.99 |
| comp 100 hd | 6.6549 | 6.2120 | 0.93 |
| mixed 100 hd | 12.2405 | 10.9764 | 0.90 |
| chain 1000 tiny | 0.3203 | 0.3320 | 1.04 |
| chain 3000 tiny | 1.1225 | 1.0712 | 0.95 |
| wide 1000 tiny | 0.6497 | 0.6805 | 1.05 |
| comp 300 tiny | 0.2676 | 0.2497 | 0.93 |

### Occupancy

`SAMPLER=states`, HD, single frame repeated 20 times (comp, wide) or one warm frame, one timed frame and an 8-frame range (chain 30). 120 samples at 0.5 s; runs that ended earlier have fewer samples. Histograms are samples by running-thread count; D-state threads were 0 in every sample. Mean includes any 0-1 thread samples at the ends of a run.

| run | samples | 1 | 2 | 3 | 4 | 5+ | mean running | share at 4 |
|---|---|---|---|---|---|---|---|---|
| legacy comp 100 | 120 | 43 | 7 | 3 | 67 | 0 | 2.78 | 55.8% |
| taskgraph comp 100 | 120 | 17 | 8 | 89 | 6 | 0 | 2.70 | 5.0% |
| legacy wide 100 | 105 | 44 | 6 | 2 | 53 | 0 | 2.61 | 50.5% |
| taskgraph wide 100 | 97 | 11 | 7 | 19 | 59 | 1 | 3.33 | 60.8% |
| legacy chain 30 range 8 (0.5 s) | 12 | 0 | 2 | 4 | 6 | 0 | 3.33 | 50.0% |
| taskgraph chain 30 range 8 (0.5 s) | 15 | 1 | 5 | 3 | 5 | 1 (10) | 3.33 | 33.3% |
| legacy chain 30 range 8 (0.1 s) | 51 | 0 | 8 | 5 | 34 | 4 | 3.73 | 66.7% |
| taskgraph chain 30 range 8 (0.1 s) | 63 | 3 | 22 | 14 | 23 | 1 | 2.95 | 36.5% |

Task graph keeps comp at exactly 3 running threads (89 of 120 samples) and rarely reaches 4; wide reaches 4 in 61% of samples. On chain 30 the Task graph is below legacy (2.95 against 3.73), the opposite of the intent. The 1-sample 10-thread reading in the 0.5 s taskgraph chain run is a transient. In the 20-frame wide runs frame time is bimodal (about 1.7 s for the first 4 frames, then about 3.2 s) in both modes' 20-frame runs and in the P=8/16 and stack runs; the 3-frame matrix rows are all in the slow mode.

### Pool-size sweep (Task graph, HD, 3 timed frames)

| P | comp frame s | comp rss_peak MB | wide frame s | wide rss_peak MB | pixels identical (comp, frame 1) |
|---|---|---|---|---|---|
| 4 (default) | 6.2841 | 3794 | 3.0666 | 1220 | yes (vs legacy) |
| 8 | 5.9640 | 1932 | 2.9715 | 1371 | yes |
| 16 | 3.8314 | 2395 | 2.9917 | 1926 | yes |

Pixel check: one frame of comp 100 HD rendered in Legacy and Task graph at P=4, 8 and 16 (uncompressed EXR, 294054 bytes each, under `build/bench/pix/`). `cmp -l` against legacy shows exactly 2 differing bytes per file, at offsets 67-70 inside the `capDate` header string; every pixel byte is identical. The comp peak RSS at P=4 (3794 MB, and 3655 MB legacy) is higher than at P=8/16, so peak RSS on comp is not monotonic in P; it varies by GB between runs.

### Gate checks

- FAIL: comp 100 HD Task graph mean running threads >= 3.2. 2.70 (89 of 120 samples at exactly 3; 5% at 4).
- PASS: wide 100 HD Task graph mean running threads >= 3.2. 3.33.
- FAIL: comp 100 HD single frame Task graph <= 0.8x Legacy. 6.2841 / 6.2120 = x1.01 (occupancy run: 6.2369 / 6.0039 = x1.04).
- FAIL: wide 100 HD single frame Task graph <= 0.8x Legacy. 3.0666 / 3.1868 = x0.96 (occupancy run: 2.9646 / 3.0774 = x0.96).
- FAIL: HD chain 30 Task graph <= 1.1x Legacy. 1.5114 / 0.9939 = x1.52 (noisy; occupancy rerun x1.16).
- PASS: HD chain 100 <= 1.1x. 5.2390 / 4.9201 = x1.06.
- PASS: tiny chain 1000 <= 1.1x. 0.3234 / 0.3320 = x0.97. PASS: tiny chain 3000 <= 1.1x. 1.0267 / 1.0712 = x0.96.
- FAIL: chain 30 HD range 8 per-frame wall Task graph <= 0.85x Legacy. 1.610 / 0.847 = x1.90 (occupancy rerun: 0.995 / 0.886 = x1.12).
- FAIL: mixed 100 HD Task graph <= 1.0x. 12.0348 / 10.9764 = x1.10.
- PASS: P=8/16 sweep pixels identical (only the `capDate` header seconds differ).
- PASS (with a note): peak RSS growth <= ~66 MB per added pool thread. Against P=4: wide +151 MB at P=8 (38 MB/thread), +706 MB at P=16 (59 MB/thread); comp falls (3794 -> 1932 -> 2395). Between P=8 and P=16 alone, wide grows 555 MB = 69 MB/thread and comp 463 MB = 58 MB/thread, so the wide step slightly exceeds 66.
- PASS: Legacy vs After M62, no row slower than 1.15x. Worst 1.05 (wide 1000 tiny); chain 1000 tiny 1.04.

### Input for M64

`samples-m63after-tg-comp100.txt` and `samples-m63after-tg-wide100.txt` (Task graph, HD, 20 frames, 60 eu-stack samples; eu-stack stops the process, so absolute concurrency is understated).

comp 100: 406 busy thread-samples. 65.5% of them (266) are `gomp_barrier_wait_end` in libgomp threads, i.e. OpenMP workers spinning in the CImg plugins' parallel regions; they are not Natron pool threads and are counted as running by `sample_states.sh` too, which inflates comp's 3-thread occupancy. Excluding them (140 busy samples): plugin render pixel work 79 (56%: Misc.ofx `ofxsMaskMixPix`, transform, filter, CImg), plugin other (begin/end/props and ImageProcessor glue) 47 (34%), engine per-image passes 14 (10%: `checkForNaNsAndFix`, alloc/fill, `copyUnProcessed`), engine other (host overhead: request pass, scheduler, metadata) about 0 in samples. Of the blocked samples, 60 of 63 are the writer waiting in `FrameFuture::wait`.

wide 100: 190 busy thread-samples. plugin render 149 (78.4%; `ofxsMaskMixPix` 24.7% self, other Misc.ofx frames 56%), engine image alloc/fill and NaN passes 22 (11.6%; `checkForNaNsAndFix` 6.8% self), plugin other 17 (8.9%), engine other (host overhead) 1 (0.5%), libc 1 (0.5%). 56 of 58 blocked samples are `FrameFuture::wait`.

So at HD host overhead is negligible in both graphs and the remaining levers are the plugin pixel loops (about 80-95% of non-spin busy time), the per-image NaN/copy passes (10-12%), and keeping the pool fed: comp sits at 3 running threads with an additional OpenMP spin pool competing for the 4 cores. Worth checking in M64 whether `OMP_WAIT_POLICY=passive` or `OMP_NUM_THREADS` coordination changes comp's occupancy and frame time.

## After admission fix (2026-10-04, 159da69d0)

Machine: 4-core Intel N100, 15 GB RAM; `build/release` at `159da69d0` in the `natron-dev` container, one configuration at a time, with the harness defaults: before each configuration the 1-minute load average must be below 0.5, then a 60 s sleep. The load seen at that point is in the table below (all 0.21-0.49); `cpu_khz_mean` is the mean over all four CPUs, sampled every 0.5 s while the configuration ran (including graph build, and for the HD matrix runs the extra stats render described below). Modes: `BENCH_SETTINGS=renderSchedulerMode=0` (Legacy) and `=1` (Task graph). The whole session took 4831 s (80.5 min), every configuration exited 0.

Commands (repo root; `BENCH_TIMEOUT=3600`; `<M>` is 0 for legacy and 1 for taskgraph; the HD and tiny matrices and the states runs had `BENCH_RENDER_STATS=1`):

```
BENCH_SETTINGS=renderSchedulerMode=<M> tools/bench/run_matrix.sh m63fix-<legacy|taskgraph>-hd hd 3 8 chain:30,100 wide:100 comp:100 mixed:100
BENCH_SETTINGS=renderSchedulerMode=<M> tools/bench/run_matrix.sh m63fix-<legacy|taskgraph>-tiny tiny 5 0 chain:1000 wide:1000 comp:300
SAMPLER=states tools/bench/profile_run.sh m63fix-<mode>-<comp|wide>100 120 0.5 BENCH_TOPO=<comp|wide> BENCH_N=100 BENCH_FRAMES=6 BENCH_RES=hd BENCH_SETTINGS=renderSchedulerMode=<M>
BENCH_SETTINGS="renderSchedulerMode=1;noRenderThreads=<8|16>" tools/bench/run_matrix.sh m63fix-taskgraph-p<8|16> hd 3 0 comp:100 wide:100
OMP_WAIT_POLICY=passive OMP_DISPLAY_ENV=true BENCH_SETTINGS=renderSchedulerMode=1 tools/bench/run_matrix.sh m63fix-omp-passive hd 3 0 comp:100
OMP_THREAD_LIMIT=1 BENCH_SETTINGS=renderSchedulerMode=<M> tools/bench/run_matrix.sh m63fix-omp-limit1-<legacy|taskgraph> hd 3 0 comp:100
BENCH_SETTINGS="renderSchedulerMode=1;nThreadsPerEffect=1" tools/bench/run_matrix.sh m63fix-tpe1 hd 3 0 comp:100
BENCH_SETTINGS="renderSchedulerMode=1;noRenderThreads=16;nThreadsPerEffect=4" tools/bench/run_matrix.sh m63fix-p16-tpe4 hd 3 0 comp:100
tools/bench/profile_run.sh m63fix-tg-comp100 60 0.5 BENCH_TOPO=comp BENCH_N=100 BENCH_FRAMES=6 BENCH_RES=hd BENCH_SETTINGS=renderSchedulerMode=1
python3 tools/bench/analyze_stacks.py build/bench/samples-m63fix-tg-comp100.txt
```

Results: `build/bench/results-m63fix-*.jsonl`, `states-m63fix-*.txt`, `samples-m63fix-tg-comp100.txt`, `freq-m63fix-*.txt`.

`max_concurrent_tasks` and `tasks_run` come from a new `BENCH_RENDER_STATS=1` option of `graph_bench.py`: `app.render` hard-codes `useRenderStats = false`, so after the timed frames the script saves the project and renders frame 2 of it in a second NatronRenderer process with `--render-stats -w`, then reads `Tasks run` and `Max concurrent tasks` from the `-stats.txt` file. It is a separate, cold, single-frame render (and extends the freq log of that configuration by a few seconds); Legacy records `0`. `run_matrix.sh` forwards `BENCH_RENDER_STATS` into the container. The HD frame times below are not affected, since the extra render follows them.

Load before each configuration: HD legacy chain 30 0.43, chain 100 0.49, wide 0.48, comp 0.49, mixed 0.47; HD taskgraph 0.47, 0.47, 0.47, 0.47, 0.46; tiny legacy 0.48, 0.47, 0.49; tiny taskgraph 0.33, 0.31, 0.39; occupancy legacy comp 0.21, taskgraph comp 0.49, legacy wide 0.48, taskgraph wide 0.47; P=8 comp 0.47, wide 0.48; P=16 comp 0.49, wide 0.49; OMP passive 0.48, limit1 legacy 0.48, limit1 taskgraph 0.49, tpe1 0.49, P16+tpe4 0.47; stack profile 0.46.

### HD (3 timed frames; range of 8 frames) and tiny (5 timed frames)

Ratio is taskgraph / legacy of the median frame (below 1 is faster for taskgraph). Parallelism, RSS and clock read legacy / taskgraph. `max concurrent tasks` is the Task graph value (chain and mixed are serial graphs, so 1 is the maximum possible).

| topo | n | build s (L / T) | legacy frame s | taskgraph frame s | ratio | parallelism (L / T) | range per-frame s (L / T) | range ratio | RSS before MB | max concurrent tasks (tasks run) | cpu_khz_mean MHz (L / T) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| chain | 30 hd | 0.097 / 0.096 | 1.0485 | 1.1217 | 1.07 | 3.14 / 2.56 | 0.907 / 0.987 | 1.09 | 130 / 130 | 1 (32) | 2610 / 2633 |
| chain | 100 hd | 0.308 / 0.317 | 3.0160 | 3.4686 | 1.15 | 3.29 / 2.66 | 4.609 / 4.934 | 1.07 | 174 / 173 | 1 (102) | 2020 / 2126 |
| wide | 100 hd | 0.463 / 0.498 | 1.9477 | 1.6945 | 0.87 | 2.94 / 3.30 | 2.318 / 2.272 | 0.98 | 210 / 209 | 4 (99) | 2201 / 2104 |
| comp | 100 hd | 0.389 / 0.383 | 3.9560 | 4.1545 | 1.05 | 3.09 / 2.66 | 5.730 / 5.846 | 1.02 | 195 / 195 | 4 (103) | 2002 / 2063 |
| mixed | 100 hd | 0.367 / 0.363 | 9.9440 | 11.7099 | 1.18 | 3.08 / 2.48 | 10.604 / 11.254 | 1.06 | 191 / 191 | 1 (102) | 1925 / 2100 |
| chain | 1000 tiny | 6.549 / 6.588 | 0.3272 | 0.3332 | 1.02 | 1.00 / 1.01 | - | - | 730 / 730 | 1 (1002) | 2607 / 2654 |
| wide | 1000 tiny | 6.976 / 7.025 | 0.6394 | 0.4526 | 0.71 | 1.00 / 1.48 | - | - | 1110 / 1110 | 4 (999) | 2647 / 2645 |
| comp | 300 tiny | 1.305 / 1.313 | 0.2503 | 0.2156 | 0.86 | 1.27 / 1.56 | - | - | 359 / 359 | 4 (304) | 2351 / 2291 |

No counterpart pair differs by more than 10% in `cpu_khz_mean` (largest: mixed HD, 1925 vs 2100 MHz, +9.1%). The HD rows still throttle inside a configuration: the three timed frames run mostly at full clock, the 8-frame range after them does not (mixed legacy frames 7.52, 9.94, 11.06 s show it), so the range columns are not comparable to the frame columns. These HD frame times are lower than After M63's because that section's configurations started hot (comp legacy 6.21 s there, 3.96 s here).

Task graph now reaches 4 concurrent tasks (the pool size) on wide and comp, up from the 3 seen in the occupancy runs of After M63; chain and mixed cannot go above 1.

### Legacy against After M62

Median frame of Legacy now against After M62 (`settings=''`; After M62 was not cooled down and has no clock log, so it ran partly throttled and the ratio mostly measures the cooldown):

| config | After M62 s | legacy now s | ratio |
|---|---|---|---|
| chain 30 hd | 1.2958 | 1.0485 | 0.81 |
| chain 100 hd | 5.3744 | 3.0160 | 0.56 |
| wide 100 hd | 3.2296 | 1.9477 | 0.60 |
| comp 100 hd | 6.6549 | 3.9560 | 0.59 |
| mixed 100 hd | 12.2405 | 9.9440 | 0.81 |
| chain 1000 tiny | 0.3203 | 0.3272 | 1.02 |
| wide 1000 tiny | 0.6497 | 0.6394 | 0.98 |
| comp 300 tiny | 0.2676 | 0.2503 | 0.94 |

### Occupancy

`SAMPLER=states`, HD, 6 timed frames of one frame number plus the warm frame, 120 samples requested at 0.5 s; the runs end before that (the sampler stops with the process), so sample counts are 16-66. Histograms are samples by running-thread count; D-state threads were 0 in every sample. Mean includes the 0-1 thread samples at the ends of the run.

| run | samples | 1 | 2 | 3 | 4 | mean running | share at 4 | frame s |
|---|---|---|---|---|---|---|---|---|
| legacy comp 100 | 64 | 17 | 1 | 2 | 44 | 3.14 | 68.8% | 3.96 4.00 4.00 6.38 6.42 6.89 |
| taskgraph comp 100 | 66 | 9 | 10 | 44 | 3 | 2.62 | 4.5% | 4.21 4.25 4.46 6.49 6.49 6.29 |
| legacy wide 100 | 22 | 15 | 2 | 0 | 5 | 1.77 | 22.7% | 1.98 1.99 1.94 2.00 1.96 1.97 |
| taskgraph wide 100 | 16 | 1 | 2 | 3 | 10 | 3.38 | 62.5% | 1.60 1.58 1.57 1.58 1.54 1.56 |

The comp runs exceed the ~12 s throttling window: frames 4-6 take about 6.4 s against about 4.0-4.4 s for frames 1-3, in both modes, so the samples mix full-clock and throttled seconds. The wide runs (11-12 s) stay inside it, but with 16-22 samples (the 1-thread samples are the graph-build tail and process exit) the legacy wide mean is dominated by those ends. Even with `max_concurrent_tasks` at 4, the comp occupancy stays at 3 running threads in 44 of 66 samples (4 only 3 times): the Task graph pool keeps 3-4 tasks admitted, but the sampled state is 3 running threads.

### Pool-size sweep (Task graph, HD, 3 timed frames, no range)

The P=4 rows are from the matrix above (which also ran a range afterwards). Pixel identity was not rechecked.

| P | comp frame s | ratio to P=4 | comp rss_peak MB | comp khz_mean MHz | wide frame s | ratio to P=4 | wide rss_peak MB | wide khz_mean MHz |
|---|---|---|---|---|---|---|---|---|
| 4 (default) | 4.1545 | 1.00 | 3631 | 2063 | 1.6945 | 1.00 | 1267 | 2104 |
| 8 | 3.7983 | 0.91 | 1809 | 2626 | 1.5573 | 0.92 | 1425 | 2377 |
| 16 | 3.7413 | 0.90 | 2307 | 2690 | 1.5495 | 0.91 | 2149 | 2413 |

FLAG: the P=4 rows' `cpu_khz_mean` is 13-30% below the P=8/16 rows' (the P=4 configurations also ran the 8-frame range and the stats render, which throttle the average); the frame times themselves come from the first, full-clock frames of each run, but the sweep is not a clean like-for-like comparison. The three timed frames of each sweep run are within 3% of each other.

### OpenMP and per-effect thread experiments (HD comp 100, 3 timed frames, no range)

`OMP_DISPLAY_ENV=true` printed the libgomp banner into the log (`OPENMP DISPLAY ENVIRONMENT BEGIN ... OMP_WAIT_POLICY = 'PASSIVE'`). Ratio is to the default Task graph row (4.1545 s, from the matrix, 2063 MHz); the limit1 legacy row is also given against Legacy (3.9560 s).

| config | frame s | ratio | frames | parallelism | khz_mean MHz |
|---|---|---|---|---|---|
| taskgraph default (P=4) | 4.1545 | 1.00 | 4.154 4.154 4.318 | 2.66 | 2063 (flag: has range) |
| taskgraph `OMP_WAIT_POLICY=passive` | 4.4176 | 1.06 | 4.263 4.418 6.114 | 2.58 | 2538 |
| legacy `OMP_THREAD_LIMIT=1` | 4.4209 | 1.06 (1.12 vs legacy 3.9560) | 4.417 4.422 4.421 | 2.64 | 2695 |
| taskgraph `OMP_THREAD_LIMIT=1` | 4.3493 | 1.05 | 4.349 4.287 4.671 | 2.53 | 2647 |
| taskgraph `nThreadsPerEffect=1` | 6.8435 | 1.65 | 6.844 6.818 7.940 | 1.45 | 2700 |
| taskgraph `noRenderThreads=16;nThreadsPerEffect=4` | 3.7072 | 0.89 | 3.707 3.682 3.744 | 3.21 | 2636 |

Reading: `OMP_WAIT_POLICY=passive` did not help (its third frame is a throttling outlier; the first two are 4.26 and 4.42 s). `OMP_THREAD_LIMIT=1` slows Legacy by 12% and the Task graph by 5%, so Legacy depends more on OpenMP threads than the Task graph does, but neither falls much, which means the libgomp threads were not what filled the cores. `nThreadsPerEffect=1` is much slower (parallelism 1.45): the pool then only helps where branches are independent. P=16 with 4 threads per effect is the fastest comp configuration measured (0.89x). The OMP and tpe rows ran at a higher mean clock than the default row (flag).

### Stack profile (Task graph, HD comp 100, 6 timed frames, 60 eu-stack samples)

`samples-m63fix-tg-comp100.txt`; load before 0.46, `cpu_khz_mean` 2244 MHz. 54 samples, 23.3 threads per sample (the process holds many parked libgomp and pool threads). gomp barrier frames count as idle now. Split of 1256 thread-samples: idle 85.0% (1068), busy 10.7% (134), blocked inside a render 4.3% (54; 53 of them the writer in `FrameFuture::wait`, 1 in `parallelForOnGlobalPool`). Mean busy threads per sample 2.48 (eu-stack stops the process, so this is understated). Busy activity: plugin render 51.5%, plugin other 38.8%, engine image alloc/fill 9.7%.

Top busy self time: unresolved `[Misc.ofx]` frames 29.9%; `ofxsMaskMixPix<float,4,1,true>` (Misc.ofx) 14.9%; `void [Misc.ofx]` 9.0%; `ofxsFilterInterpolate2D<float,4,...>` 8.2%; `CImg::_cimg_recursive_apply` (vanvliet blur) 6.7%; a `std::_Function_handler` in NatronRenderer 5.2%; `Natron::Image::checkForNaNsAndFix` 4.5%; `Transform3x3Processor` 3.7%; `OFX::Image::getPixelAddress` 3.7%; `CImgFilterPluginHelper` 3.0%; `ofxsPremult`/`ofxsUnPremult` 2.2% each. `ImageProcessor::multiThreadFunction` appears in 75.4% of busy samples, `PoolParallelForDetail::drain` in 64.9% and `RenderScheduler::runOneTask` in 57.5%; `gomp_thread_start` in only 4.5%, so the OpenMP spin that dominated the After M63 profile (65.5% of busy samples) is gone from the busy set.

### Gate checks

- FAIL: comp 100 HD Task graph mean running threads >= 3.2. 2.62 (44 of 66 samples at exactly 3; 4.5% at 4). Max concurrent tasks was 4.
- PASS: wide 100 HD Task graph mean running threads >= 3.2. 3.38 (16 samples).
- FAIL: comp 100 HD single frame Task graph <= 0.8x Legacy. 4.1545 / 3.9560 = x1.05.
- FAIL: wide 100 HD single frame Task graph <= 0.8x Legacy. 1.6945 / 1.9477 = x0.87 (tiny wide 1000 is x0.71).
- PASS: HD chain 30 Task graph <= 1.1x Legacy. 1.1217 / 1.0485 = x1.07.
- FAIL: HD chain 100 <= 1.1x. 3.4686 / 3.0160 = x1.15 (range per-frame x1.07).
- PASS: tiny chain 1000 <= 1.1x. 0.3332 / 0.3272 = x1.02.
- FAIL: chain 30 HD range 8 per-frame wall Task graph <= 0.85x Legacy. 0.987 / 0.907 = x1.09.
- FAIL: mixed 100 HD Task graph <= 1.0x. 11.7099 / 9.9440 = x1.18 (range per-frame x1.06).
- SKIPPED: P=8/16 sweep pixels identical (done in After M63).
- PASS (with a note): peak RSS growth <= ~66 MB per added pool thread. Wide against P=4: +158 MB at P=8 (40 MB/thread), +882 MB at P=16 (74 MB/thread; 90 MB/thread between P=8 and P=16, over the limit). Comp falls (3631 -> 1809 -> 2307).
- PASS: Legacy vs After M62, no row slower than 1.15x. Worst 1.02 (chain 1000 tiny); the HD rows are 0.56-0.81 because After M62 ran without cooldown.

## After thread budget (2026-10-04, 2db83845a)

Machine: 4-core Intel N100, 15 GB RAM; `build/release` at `2db83845a` in the `natron-dev` container, one configuration at a time, harness defaults (1-minute load average below 0.5, then a 60 s sleep). Modes: `BENCH_SETTINGS=renderSchedulerMode=0` (Legacy) and `=1` (Task graph); every configuration exited 0. The whole session took 5560 s (92.7 min), driven by `build/bench/m63final.sh`. Changes since After admission fix: a scheduler-owned per-task thread budget (a lone task gets the pool capped at cores, P tasks get 1 each; the OFX suite, host frame threading and CImg's OpenMP team obey it), feeders opened only while the ready set cannot fill the pool, helpers queued just above node tasks, and the hygiene fixes (stats off the hot path, same-key deferral across frames, failed tasks abort siblings).

Load seen at the end of each cooldown (all below 0.5): HD legacy chain 30 0.47, chain 100 0.47, wide 0.48, comp 0.49, mixed 0.48; HD taskgraph 0.46, 0.47, 0.47, 0.48, 0.49; tiny legacy 0.49, 0.36, 0.30; tiny taskgraph 0.23, 0.44, 0.44; occupancy legacy comp 0.33, wide 0.48, chain 0.48; taskgraph comp 0.46, wide 0.49, chain 0.48; sweep P=4 comp 0.47, wide 0.46; P=8 comp 0.47, wide 0.49; P=16 comp 0.49, wide 0.48; stack profile 0.46. `cpu_khz_mean` per run is in the tables (HD and tiny) and below (sweep, occupancy, stack).

Commands (repo root; `BENCH_TIMEOUT=3600`; `<M>` is 0 for legacy and 1 for taskgraph; `BENCH_RENDER_STATS=1` on the Task graph matrices only):

```
BENCH_SETTINGS=renderSchedulerMode=<M> tools/bench/run_matrix.sh m63final-<legacy|taskgraph>-hd hd 3 8 chain:30,100 wide:100 comp:100 mixed:100
BENCH_SETTINGS=renderSchedulerMode=<M> tools/bench/run_matrix.sh m63final-<legacy|taskgraph>-tiny tiny 5 0 chain:1000 wide:1000 comp:300
SAMPLER=states tools/bench/profile_run.sh m63final-<mode>-<comp|wide>100 120 0.5 BENCH_TOPO=<comp|wide> BENCH_N=100 BENCH_FRAMES=6 BENCH_RES=hd BENCH_SETTINGS=renderSchedulerMode=<M>
SAMPLER=states tools/bench/profile_run.sh m63final-<mode>-chain30 120 0.5 BENCH_TOPO=chain BENCH_N=30 BENCH_FRAMES=6 BENCH_RANGE=8 BENCH_RES=hd BENCH_SETTINGS=renderSchedulerMode=<M>
BENCH_SETTINGS=renderSchedulerMode=1 tools/bench/run_matrix.sh m63final-sweep4 hd 3 0 comp:100 wide:100
BENCH_SETTINGS="renderSchedulerMode=1;noRenderThreads=<8|16>" tools/bench/run_matrix.sh m63final-p<8|16> hd 3 0 comp:100 wide:100
tools/bench/profile_run.sh m63final-tg-comp100 60 0.5 BENCH_TOPO=comp BENCH_N=100 BENCH_FRAMES=6 BENCH_RES=hd BENCH_SETTINGS=renderSchedulerMode=1
python3 tools/bench/analyze_stacks.py build/bench/samples-m63final-tg-comp100.txt
```

Results: `build/bench/results-m63final-*.jsonl`, `states-m63final-*.txt`, `samples-m63final-tg-comp100.txt`, `freq-m63final-*.txt`.

### HD (3 timed frames; range of 8 frames) and tiny (5 timed frames)

Ratio is taskgraph / legacy of the median frame. Parallelism, RSS and clock read legacy / taskgraph. `max concurrent tasks` is the Task graph value from the separate stats pass.

| topo | n | build s (L / T) | legacy frame s | taskgraph frame s | ratio | parallelism (L / T) | range per-frame s (L / T) | range ratio | RSS before MB | max concurrent tasks (tasks run) | cpu_khz_mean MHz (L / T) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| chain | 30 hd | 0.095 / 0.094 | 1.0460 | 1.0125 | 0.97 | 3.03 / 3.09 | 0.890 / 0.864 | 0.97 | 130 / 130 | 1 (32) | 2560 / 2598 |
| chain | 100 hd | 0.309 / 0.309 | 3.3652 | 3.0829 | 0.92 | 2.95 / 3.24 | 4.751 / 4.581 | 0.96 | 173 / 173 | 1 (102) | 1985 / 2021 |
| wide | 100 hd | 0.463 / 0.464 | 1.9467 | 1.5406 | 0.79 | 2.94 / 3.57 | 2.395 / 2.175 | 0.91 | 209 / 209 | 4 (99) | 2186 / 2154 |
| comp | 100 hd | 0.387 / 0.387 | 3.9715 | 3.6881 | 0.93 | 3.10 / 3.23 | 5.706 / 5.516 | 0.97 | 195 / 195 | 4 (103) | 2004 / 1944 |
| mixed | 100 hd | 0.374 / 0.365 | 10.9741 | 10.4686 | 0.95 | 3.08 / 3.00 | 10.543 / 10.593 | 1.00 | 190 / 190 | 1 (102) | 1914 / 1918 |
| chain | 1000 tiny | 6.509 / 6.517 | 0.3254 | 0.3980 | 1.22 | 1.00 / 1.01 | - | - | 730 / 730 | 1 (1002) | 2480 / 2612 |
| wide | 1000 tiny | 6.979 / 7.001 | 0.6354 | 0.4952 | 0.78 | 1.00 / 1.57 | - | - | 1110 / 1110 | 4 (999) | 2604 / 2665 |
| comp | 300 tiny | 1.327 / 1.308 | 0.2535 | 0.2087 | 0.82 | 1.27 / 1.59 | - | - | 359 / 359 | 4 (304) | 2269 / 2355 |

No counterpart pair differs by more than 10% in `cpu_khz_mean` (largest: tiny chain 1000, +5.3%, Task graph higher). The frame columns are the median of three; for HD comp, wide and chain the three frames are within 5% of each other, for mixed the frames throttle (legacy 7.26, 10.97, 11.38 s; taskgraph 7.50, 10.47, 11.27 s), so the median is a throttled frame and the first-frame ratio is 1.03. First-frame ratios for the rest: chain 30 1.00, chain 100 0.94, wide 0.80, comp 0.93. Task graph tiny chain 1000 is slower than legacy in every frame (0.38-0.41 s against 0.32-0.33 s) and its warm frame too (0.406 against 0.343 s).

Against After admission fix (Task graph, what the thread budget changed): chain 30 HD 1.1217 -> 1.0125 s (ratio 1.07 -> 0.97); chain 100 HD 3.4686 -> 3.0829 s (1.15 -> 0.92); wide 100 HD 1.6945 -> 1.5406 s (0.87 -> 0.79, parallelism 3.30 -> 3.57); comp 100 HD 4.1545 -> 3.6881 s (1.05 -> 0.93, parallelism 2.66 -> 3.23); mixed 100 HD 11.7099 -> 10.4686 s (1.18 -> 0.95, ratio of the range 1.06 -> 1.00); range per-frame ratios chain 30 1.09 -> 0.97, chain 100 1.07 -> 0.96, wide 0.98 -> 0.91, comp 1.02 -> 0.97; tiny chain 1000 0.3332 -> 0.3980 s (1.02 -> 1.22, worse); tiny wide 1000 0.4526 -> 0.4952 s (0.71 -> 0.78, worse); tiny comp 300 0.2156 -> 0.2087 s (0.86 -> 0.82). Legacy did not move (comp 3.9560 -> 3.9715 s). The HD Task graph rows all gained 8-12% (the per-task thread budget keeps lone tasks on the whole pool); the tiny chain and wide rows lost 9-19%, so per-task budget bookkeeping costs on 1000-task serial graphs.

### Legacy against After M62

Median frame of Legacy now against After M62 (`settings=''`; After M62 had no cooldown, so the HD ratios mostly measure that):

| config | After M62 s | legacy now s | ratio |
|---|---|---|---|
| chain 30 hd | 1.2958 | 1.0460 | 0.81 |
| chain 100 hd | 5.3744 | 3.3652 | 0.63 |
| wide 100 hd | 3.2296 | 1.9467 | 0.60 |
| comp 100 hd | 6.6549 | 3.9715 | 0.60 |
| mixed 100 hd | 12.2405 | 10.9741 | 0.90 |
| chain 1000 tiny | 0.3203 | 0.3254 | 1.02 |
| wide 1000 tiny | 0.6497 | 0.6354 | 0.98 |
| comp 300 tiny | 0.2676 | 0.2535 | 0.95 |

### Occupancy

`SAMPLER=states`, HD, 6 timed frames plus the warm frame (chain 30 also an 8-frame range), 120 samples requested at 0.5 s; the runs end earlier, so sample counts are 17-73 and the 1-thread samples include the build tail and exit. D-state threads were not counted. Mean includes the 1-thread samples.

| run | samples | 1 | 2 | 3 | 4 | 5+ | mean running | share at 4 | cpu_khz_mean MHz | frame s |
|---|---|---|---|---|---|---|---|---|---|---|
| legacy comp 100 | 73 | 24 | 10 | 7 | 32 | 0 | 2.64 | 43.8% | 2200 | 4.62 5.25 7.41 6.53 6.16 6.25 |
| taskgraph comp 100 | 61 | 10 | 8 | 4 | 38 | 1 | 3.20 (3.197) | 63.9% | 2175 | 3.75 3.78 4.08 6.19 5.98 6.44 |
| legacy wide 100 | 22 | 13 | 1 | 0 | 8 | 0 | 2.14 | 36.4% | 2630 | 1.99 1.97 1.96 1.94 1.95 1.93 |
| taskgraph wide 100 | 17 | 2 | 1 | 0 | 14 | 0 | 3.53 | 82.4% | 2586 | 1.67 1.60 1.63 1.67 1.64 1.62 |
| legacy chain 30 range 8 | 23 | 7 | 1 | 1 | 14 | 0 | 2.96 | 60.9% | 2590 | 0.98 1.09 1.12 1.07 1.28 1.04 |
| taskgraph chain 30 range 8 | 22 | 5 | 0 | 4 | 13 | 0 | 3.14 | 59.1% | 2624 | 0.98 1.08 1.01 1.04 1.00 1.10 |

Against After admission fix: Task graph comp mean 2.62 -> 3.20 and share at 4 from 4.5% to 63.9% (the 44-of-66 samples stuck at exactly 3 are now 4 of 61); wide 3.38 -> 3.53 (62.5% -> 82.4%); chain 30 Task graph went from below legacy (After M63: 2.95 against 3.73) to above (3.14 against 2.96). The wide runs are 11-12 s long with 17-22 samples, so the legacy wide mean is dominated by build and exit samples. The comp runs exceed the ~12 s throttling window (frames 4-6 take about 6 s against 3.8-4.1 s).

### Pool-size sweep (Task graph, HD, 3 timed frames, no range, no stats pass)

The P=4 rows were rerun without range and stats (`m63final-sweep4`), so the clock state matches the P=8/16 rows. Median frame is the middle of three; the third comp frame at P=4 and P=8 is a throttling outlier (6.91 s and 6.95 s), so the first frame is listed too.

| P | comp frames s | comp median | comp rss_peak MB | comp khz_mean MHz | wide frames s | wide median | wide rss_peak MB | wide khz_mean MHz |
|---|---|---|---|---|---|---|---|---|
| 4 (default) | 3.77 4.32 6.91 | 4.3159 | 1688 | 2338 | 1.55 1.63 1.62 | 1.6248 | 981 | 2491 |
| 8 | 3.96 6.34 6.95 | 6.3439 | 1776 | 2197 | 1.67 1.72 1.63 | 1.6701 | 1447 | 2522 |
| 16 | 3.89 4.16 5.15 | 4.1597 | 2369 | 2521 | 1.58 1.66 1.70 | 1.6604 | 2076 | 2428 |

FLAG: comp `cpu_khz_mean` at P=8 (2197) is 14.7% below P=16 (2521) and 6% below P=4; the comp frames throttle in the second and third frame in all three runs, so comp medians here are throttle-dominated and only the first frame (3.77 / 3.96 / 3.89 s) is comparable. Wide is flat in P (first frames 1.55 / 1.67 / 1.58 s) and its clocks agree within 4%. Larger pools do not help any more because the per-task budget already puts the whole pool behind a lone task. The P=4 wide RSS (981 MB) is lower than the 1257 MB of the matrix row, which also ran the range and stats.

### Stack profile (Task graph, HD comp 100, 6 timed frames, 60 eu-stack samples)

`samples-m63final-tg-comp100.txt`; load before 0.46, `cpu_khz_mean` 2144 MHz. 48 samples, 22.9 threads per sample. Split of 1097 thread-samples: idle 82.5% (905), busy 11.7% (128), blocked inside a render 5.3% (58; 46 writer in `FrameFuture::wait`, 11 `parallelForOnGlobalPool`, 1 `renderAction`), unknown 0.5%. Mean busy threads per sample 2.67 (eu-stack stops the process, so this is understated). Busy activity: plugin other 52.3%, plugin render 37.5%, engine image alloc/fill 7.8%, libc alloc/copy 2.3%.

Top busy self time: unresolved `[Misc.ofx]` 35.2%; `ofxsMaskMixPix<float,4,1,true>` (Misc.ofx) 14.8%; `ofxsFilterInterpolate2D<float,4,...>` 10.9%; `CImg::_cimg_recursive_apply` (vanvliet blur) 7.8%; `void [Misc.ofx]` 4.7%; a `std::_Function_handler` in NatronRenderer 4.7%; `PixelCopierUnPremult` 3.1%; `Transform3x3Processor` 2.3%; `Natron::Image::checkForNaNsAndFix` 2.3%. Inclusive: `PoolParallelForDetail::drain` 81.2%, `ImageProcessor::multiThreadFunction` 72.7%, `RenderScheduler::runOneTask` 43.8%, `OfxHost::multiThread` 28.1%, `gomp_thread_start` 4.7%. Against After admission fix: `checkForNaNsAndFix` fell 4.5% -> 2.3% and engine alloc/fill 9.7% -> 7.8%; the busy set is now almost entirely pool threads running plugin code (OpenMP 4.5% -> 4.7%, unchanged).

### Gate checks

- FAIL (marginal): comp 100 HD Task graph mean running threads >= 3.2. 3.197 (rounds to 3.20; 38 of 61 samples at 4, up from 3 at 4.5%).
- PASS: wide 100 HD Task graph mean running threads >= 3.2. 3.53 (17 samples).
- FAIL: comp 100 HD single frame Task graph <= 0.8x Legacy. 3.6881 / 3.9715 = x0.93 (first frame x0.93; occupancy run first frame 3.75 / 4.62 = x0.81, but that Legacy frame ran at a lower clock, 2200 vs 2175 MHz, and its frames 2-3 are throttle outliers).
- PASS (borderline): wide 100 HD single frame Task graph <= 0.8x Legacy. 1.5406 / 1.9467 = x0.79 (first frame x0.80; occupancy run 1.67 / 1.99 = x0.84; tiny wide 1000 is x0.78).
- PASS: HD chain 30 <= 1.1x. 1.0125 / 1.0460 = x0.97. PASS: HD chain 100 <= 1.1x. 3.0829 / 3.3652 = x0.92.
- FAIL: tiny chain 1000 <= 1.1x. 0.3980 / 0.3254 = x1.22 (clocks within 5.3%, every frame slower; was x1.02 in After admission fix).
- FAIL: chain 30 HD range 8 per-frame wall Task graph <= 0.85x Legacy. 0.864 / 0.890 = x0.97 (was x1.09).
- PASS: mixed 100 HD Task graph <= 1.0x. 10.4686 / 10.9741 = x0.95 (first frame x1.03, range per-frame x1.00; the median is a throttled frame).
- FAIL: peak RSS growth <= ~66 MB per added pool thread. Wide: +466 MB at P=8 (116 MB/thread), +629 MB from 8 to 16 (79 MB/thread), +1095 MB from 4 to 16 (91 MB/thread); comp +88 MB at P=8 (22 MB/thread), +593 MB from 8 to 16 (74 MB/thread), +681 MB from 4 to 16 (57 MB/thread). The P=4 wide base (981 MB) is lower than other wide P=4 measurements (1220-1267 MB), which inflates the wide growth; against 1257 MB wide is +190 MB at P=8 and +819 MB at P=16 (68 MB/thread).
- PASS: Legacy vs After M62, no row slower than 1.15x. Worst 1.02 (chain 1000 tiny).
- FLAG: no pair of counterpart runs differs by more than 10% in `cpu_khz_mean` except the sweep's comp P=8 against P=16 (14.7%).

## Realistic workloads (2026-10-04, 2db83845a)

Machine and harness as in "After admission fix": `build/release` at `2db83845a`, one configuration at a time, default cooldowns (load before each configuration 0.46-0.49), clock logged. Modes: `BENCH_SETTINGS=renderSchedulerMode=0` (Legacy) and `=1` (Task graph, with `BENCH_RENDER_STATS=1`). Every configuration exited 0; the real set took 1910 s.

Plates (`make_plates.py`, `BENCH_PLATES_RES=all BENCH_PLATE_FRAMES=13`, about 55 s): `plate_hd_1..3` 9.4-9.5 MB per frame (122-124 MB per sequence, half RGBA; 1 and 2 ZIP, 3 PIZ), `plate_uhd_1..3` 37.3-37.7 MB per frame (485-490 MB per sequence), `deep_hd` 31.6 MB per frame (410 MB; one sample per pixel, R G B A Z ZBack float, ZIPS, Z 1-100), 2239 MB in all.

Commands (repo root; `<M>` 0 for legacy, 1 for taskgraph):

```
docker exec -e BENCH_PLATES_RES=all -e BENCH_PLATE_FRAMES=13 -e REPO="$PWD" -e OFX_PLUGIN_PATH="$PWD"/build/assets/Plugins natron-dev \
    bash -lc 'cd "$REPO" && xvfb-run --auto-servernum build/release/Renderer/NatronRenderer -b tools/bench/make_plates.py'
BENCH_TIMEOUT=3600 BENCH_SETTINGS=renderSchedulerMode=<M> tools/bench/run_matrix.sh t9-<legacy|taskgraph>-hd hd 3 8 readchain:30 footagecomp:32 deepcomp:8
BENCH_TIMEOUT=3600 BENCH_SETTINGS=renderSchedulerMode=<M> tools/bench/run_matrix.sh t9-<legacy|taskgraph>-uhd uhd 3 0 iobound:8 rambound:192
python3 tools/bench/classify.py build/bench/results-t9-*.jsonl
```

Results: `build/bench/results-t9-*.jsonl`, logs `build/bench/logs/t9-*`, clocks `build/bench/freq-t9-*.txt`.

Frame columns are the median of 3 timed frames; ratio is taskgraph / legacy. IO columns are the timed frames' totals (3 frames). Plate MB/frame is the plate file bytes the graph decodes per frame; output MB/frame is the written EXR (`none` compression).

| topo | n | nodes | legacy frame s | taskgraph frame s | ratio | range/frame s (L / T) | parallelism (L / T) | io_read / io_write MB (L / T) | rchar MB (L / T) | plate MB/frame | output MB/frame | rss_peak MB (L / T) | max concurrent tasks (tasks run) | cpu_khz_mean MHz (L / T) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| readchain | 30 hd | 31 | 1.1281 | 1.1260 | 1.00 | 0.917 / 0.902 | 3.01 / 3.01 | 5 / 50, 0 / 50 | 28 / 28 | 9.4 | 16.6 | 1248 / 1346 | 1 (32) | 2615 / 2595 |
| footagecomp | 32 hd | 38 | 1.5764 | 1.3040 | 0.83 | 1.655 / 1.296 | 2.76 / 3.27 | 19 / 51, 0 / 51 | 227 / 227 | 75.3 | 16.8 | 4581 / 4758 | 4 (39) | 2388 / 2485 |
| deepcomp | 8 hd | 10 | 2.0270 | 2.1573 | 1.06 | 1.604 / 1.585 | 1.85 / 1.67 | 0 / 50, 0 / 50 | 416 / 416 | 135.6 | 16.6 | 6129 / 4685 | 2 (4) | 2413 / 2550 |
| iobound | 8 uhd | 7 | 1.3458 | 0.9743 | 0.72 | - / - | 2.07 / 2.97 | 35 / 199, 0 / 199 | 451 / 451 | 149.8 | 66.4 | 3074 / 3079 | 4 (8) | 2440 / 2407 |
| rambound | 192 uhd | 191 | 27.0630 | 24.3242 | 0.90 | - / - | 2.92 / 3.65 | 0 / 199, 0 / 199 | 0 / 1 | 0.0 | 66.4 | 1735 / 2439 | 4 (192) | 1973 / 1739 |

Classifier verdicts (`classify.py`, legacy / taskgraph):

| topo | verdict (L / T) | numbers (taskgraph) |
|---|---|---|
| readchain 30 hd | CPU-bound / CPU-bound | parallelism 3.01 of 4; est. 1.83 GB/s moved; file bytes 26 MB/frame = 0.023 GB/s; 36 ms/node |
| footagecomp 32 hd | under-occupied / CPU-bound | parallelism 2.76 / 3.27; est. 2.11 GB/s moved; file bytes 92 MB/frame = 0.071 GB/s; rss 4.8 GB of a 7.9 GB budget |
| deepcomp 8 hd | under-occupied / under-occupied | parallelism 1.85 / 1.67; file bytes 152 MB/frame = 0.071 GB/s; rss 6.1 / 4.7 GB; 216 ms/node |
| iobound 8 uhd | under-occupied / CPU-bound | parallelism 2.07 / 2.97; file bytes 216 MB/frame = 0.22 GB/s (11% of the 2 GB/s disk threshold); block reads 35 MB legacy, 0 taskgraph |
| rambound 192 uhd | CPU-bound / CPU-bound | parallelism 2.92 / 3.65; rss_peak 1.7 / 2.4 GB of 7.9 GB; est. 2.43 GB/s moved; 127 ms/node |

- No family is IO-bound: with the plates in the page cache block reads are 0-35 MB over three frames, and even counted as disk traffic the plate and output bytes reach at most 0.22 GB/s (UHD iobound). Decode cost shows up as CPU (EXR decompression), not as IO wait.
- `rambound` is not RAM-bound: peak RSS is 1.7 GB (legacy) and 2.4 GB (taskgraph), not the 8.5 GB that 64 live UHD leaves would need, because each leaf's image is released once its merge has consumed it. Its rambound clocks differ by 12% (1973 vs 1739 MHz, taskgraph lower), so its x0.90 understates the taskgraph gain.
- Task graph wins where there are independent branches: UHD iobound x0.72 (4 concurrent tasks), footagecomp x0.83, rambound x0.90. readchain (a serial chain) is even at x1.00. deepcomp is x1.06 with 2 concurrent tasks and only 4 tasks run for 10 nodes, so most of the deep chain runs inside a single task.
- Pixel identity: in a 2-frame smoke (`BENCH_KEEP=1`, frames 1-3 of readchain 10, footagecomp 16, iobound 8, deepcomp 8 at HD and iobound 8, rambound 30 at UHD) every legacy and taskgraph EXR is byte-identical except for 2-3 bytes inside the `capDate` header.
- DeepRead does not report itself frame-varying, so within one process every frame of a `####` deep sequence after the first returned the first frame's samples: frame 2 rendered after frame 1 was byte-identical to frame 1 apart from `capDate`, while frame 2 rendered alone differed in 11.5M bytes. Both modes reproduce it. `graph_bench.py` works around it by keyframing each DeepRead's `disableNode`; the reader itself still needs the fix.

## M67 gate (contended) (2026-10-06)

Native Grade versus OFX Grade (`net.sf.openfx.GradePlugin` major 3 versus 2), release build, `tools/bench/native_vs_ofx.sh m67gate 3`: three ABAB rounds, each a tiny chain of 0 and 1000 Grades (5 frames) and an HD chain of 30 and 100 Grades (3 frames). Results: `build/bench/results-m67gate-r<1..3>-{ofx,native}.jsonl`. The host was quiet when the run started (load1 0.33), but 3 of the 24 configurations began at load1 0.54-0.70 (the previous configuration's tail), so the harness labels the run contended and the absolute numbers are not references. The ratios are, since each round is interleaved and the spread across rounds is at most 0.06.

| round | mem | build | tiny | hd | KB/node ofx -> native | HD ms/node ofx -> native |
|---|---|---|---|---|---|---|
| 1 | 0.216 | 0.886 | 0.503 | 1.731 | 634 -> 137 | 29.34 -> 50.79 |
| 2 | 0.216 | 0.884 | 0.511 | 1.672 | 634 -> 137 | 30.18 -> 50.45 |
| 3 | 0.216 | 0.874 | 0.497 | 1.697 | 634 -> 137 | 29.87 -> 50.69 |
| median | 0.216 | 0.884 | 0.503 | 1.697 | 634 -> 137 | 29.87 -> 50.69 |
| spread | 0.000 | 0.012 | 0.014 | 0.059 | | |

Gate: mem <= 0.50 passes (0.216), build <= 1.15 passes (0.884), tiny <= 1.15 passes (0.503), hd <= 1.05 fails (1.697). Verdict NO-GO.

Median frame wall (s), per config: tiny 1000 ofx 0.337 / native 0.170; hd 30 ofx 1.01 / native 1.62; hd 100 ofx 3.12 / native 5.17. RSS before the first frame (MB), tiny 0 / 1000: ofx 112 / 731, native 112 / 246.
