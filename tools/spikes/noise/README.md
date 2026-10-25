# Noise library spike

This spike compares ways of getting FastNoise2's 4D Perlin, SuperSimplex and Cellular F1
(Euclidean), each alone and under FBm, onto the CPU and the GPU. It stands alone like
`tools/gpu-spike`, and nothing here is linked into Natron.

| Implementation | What it is |
|---|---|
| `a_base`, `a_base_fma`, `a_v3`, `a_v3_fma` | `fn2port/`: a line-for-line plain C++ port of the FastNoise2 generators. The row loop is structure-of-arrays so GCC autovectorises it at `-O2`. It is built for `-march=x86-64` and `x86-64-v3`, once unfused and once with explicit `fma`. |
| `gpu`, `gpu_fma` | `fn2port/fn2port4d.slang`: the same port in Slang, run through the Vulkan fixture from `Engine/Gpu`. |
| `b_strict`, `b_relaxed` | `fn2lib/`: FastNoise2 and FastSIMD themselves, built strict (`-ffp-contract=off`, no FMA) and relaxed (`-ffast-math`, FMA). Each build is a shared object that exports only the `fn2shim.h` C functions. Rendering calls `GenPositionArray4D` once per row. |
| `fnl` | `fnl/`: FastNoiseLite's scalar 3D Perlin, OpenSimplex2S and Cellular, with z only. It is the speed floor, and its images are not comparable to the others. |

## Vendored sources (no network fetch)

| Project | Commit | Size | Licence |
|---|---|---|---|
| Auburn/FastNoise2 | `721400f9560831be2fc9790be9b72c44bee7d33f` | 530 KB, include/ and src/ only | MIT, `fn2lib/FastNoise2/LICENSE` |
| Auburn/FastSIMD | `f7e9ce6fca475731c0a3c115ec29dcfd36886b2a` (the commit FastNoise2 pins) | 305 KB | MIT |
| Auburn/FastNoiseLite | `785f37a9ad76e283586a379675085f2063ae03f7` | 111 KB, `FastNoiseLite.h` | MIT |

The FastNoise2 and FastSIMD files together come to 82 files. The port is a derived work of
FastNoise2 and keeps its MIT notice in `fn2port/LICENSE`.

## Build

Build in the natron-dev container, from the repository root. The container is single-tenant, so
cap the build at `-j4` when anything else is running.

    cmake -S tools/spikes/noise -B build/spikes/noise-build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build/spikes/noise-build -j4

The vectoriser report for each port variant (`-fopt-info-vec-optimized-missed`) is written to
`build/spikes/noise-build/vec-report/<variant>.txt`. Each chunk loop in `fn2port_rows.cpp`
should show `loop vectorized`. The `base_fma` variant is the exception: at the x86-64 baseline,
`std::fma` is a libm call, so its fused loops stay scalar.

## Run

On the CPU only, which works anywhere, including inside the container:

    build/spikes/noise-build/NoiseSpikeBench --no-gpu --out build/spikes/noise

On the GPU host:

    GPU_SPIKE_BUILD=build/spikes/noise-build tools/gpu-spike/run-host.sh NoiseSpikeBench --out build/spikes/noise

GPU timings are meaningful only on the RX 7900 XTX host, run through
`tools/gpu-spike/run-host.sh`. The container sees only lavapipe, and the bench refuses lavapipe
unless `GPU_SPIKE_ALLOW_SOFTWARE=1` is set. Even then the numbers measure llvmpipe, not a GPU.

### Defaults

- Image: 2048x2048. Pixel (x, y) maps to (origin + x + 0.5, origin + y + 0.5, 23.5, 41.25) / 64.
- Origins: 0 and 1e5.
- FBm: 4 octaves, lacunarity 2, gain 0.5.
- Seed: 1337.

### Options

| Option | Effect |
|---|---|
| `--size N\|WxH` | Image size. |
| `--algos` | `perlin`, `supersimplex`, `cellular_f1`, or any of these with `_fbm` appended. |
| `--impls` | Implementations to run, from the table above. `a_base` always runs because it is the reference. |
| `--threads N` | Threads for the all-cores timing. |
| `--band N` | Rows in the one-thread timing. Default 128. |
| `--iters N` | Timed GPU dispatches. The median is reported. |
| `--scale S` | Feature size in pixels. |
| `--octaves N` | Octave count for the `_fbm` algorithms. |
| `--origins a,b` | Origins to render. |
| `--fn2-simd max\|scalar\|sse2\|sse41\|avx2\|avx512` | Caps FastNoise2's SIMD level. |
| `--exr-all` | Writes every EXR (see Output). |
| `--no-gpu` | Skips the GPU implementations. |

### Output

The bench prints one table row per (algorithm, origin, implementation) and appends the same data
to `<out>/results.csv`. Each row has:

- **Timings:** CPU ms/Mpx on one thread and on all threads, and GPU kernel ms/Mpx.
- **Differences:** the max absolute difference against `a_base`, `gpu` and `gpu_fma`. The CSV
  also has the mean.
- **`self`:** for the port variants only, the count of pixels where the vectorised row loop
  differs from per-pixel `fbm()`, checked on every eighth row. It must be 0.

EXRs go to `<out>` (single Y channel). By default the bench writes `a_base`, `b_strict`, `fnl`
and `gpu`, plus the `gpu` and `b_strict` differences against `a_base`, for the first origin only.
`--exr-all` writes every implementation and difference at every origin.

## Expected differences

- **`a_*` vs `gpu`:** GPU `sqrt`, division and reciprocal-sqrt are not correctly rounded in
  Vulkan, so the unfused GPU kernel can differ by a few ulps. Cellular is most affected, through
  `sqrt`. `fma` variants should be compared with `fma` variants.
- **`a_*` vs `b_*`:**
  - FastNoise2's AVX512 path and `-ffast-math` (relaxed) differ by ulps.
  - FastNoise2's cellular also takes the square root of its second distance. That does not
    change F1.
- **`fnl`:** 3D noise with its own gradients, so it is a timing floor only.
