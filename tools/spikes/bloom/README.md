# Bloom spike

This spike decides how a native Bloom node stays GPU-resident. Bloom is the sum of `count` Gaussian blurs of size `size * ratio^i`, divided by `count`, with sigma = size / 2.4. The CImg reference is `CImgBlur.cpp`, the Bloom branch. The spike compares two candidates:

- **Pyramid.** An adaptive downsample pyramid. Blur i runs at level k, decimated by d = 2^k. k is the largest value with sigma_i / d >= sigma_min (2.5 by default). Level sample j covers the absolute pixels [d·j, d·j + d), so level origins are anchored to multiples of d. The blur at level k has sigma_eff² = (sigma_i / d)² − var_down(k) − var_up(d), and the result is upsampled and accumulated at full resolution. It comes in four variants:
  - downsample: [1 3 3 1]/8 or box [1 1]/2
  - upsample: bilinear or Catmull-Rom bicubic
- **D.** `count` full-resolution quasi-Gaussian (IIR) blurs through the engine's line IIR (`recordLineBlur`), each added to the output by a small kernel.

Nothing here is linked into Natron.

## Build and run

Build in the natron-dev container, from the repository root:

    cmake -S tools/spikes/bloom -B build/spikes/bloom-build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build/spikes/bloom-build

Run spike 1 (CPU) only. This works anywhere:

    build/spikes/bloom-build/BloomSpikeBench --cpu-only

Run both spikes on the host GPU, since the container only sees lavapipe:

    GPU_SPIKE_BUILD=build/spikes/bloom-build tools/gpu-spike/run-host.sh BloomSpikeBench

Notes on running:

- A software device is refused unless `GPU_SPIKE_ALLOW_SOFTWARE=1`. On lavapipe only the GPU check table (2a) means anything, so use `--skip-gpu-timing` there.
- `--help` lists the options for narrowing a run: sizes, counts, variants, sigma_min, resolutions and so on.
- Every table is also appended as CSV under `build/spikes/bloom/` (change with `--out`).

## What it prints

**1a Quality.** A 960x540 single-channel image, black outside, for each input (point, step edge, HDR plate 0.01..50 with noise and highlights), size 3/25/100/400 and count 1/3/5/8.

- Every pyramid variant is compared against two references:
  - **FIR reference:** the sum of full-resolution FIR Gaussians (`LineFilter::firGaussian`).
  - **IIR reference:** the sum of quasi-Gaussians (`LineFilter::forFilter(eFilterQuasiGaussian)`, which is what D computes).
- The row `iir(D)` measures D itself against the FIR reference.
- `levels` lists the level k of each blur. A case counts as decimated when any of its blurs has k > 0.
- The table ends with each method's worst case over the decimated cases.

**1b Seams.** A 32768x64 plate. Windows 1024 wide at x = base and x = base + 37 (37 is a non-multiple of every d > 1), for base 0 and mid-image.

- Each window is rendered from only its source cone clipped to the image.
- **overlap** counts floats where the two windows differ bitwise.
- **full** counts floats where either window differs from the full-frame render.
- Both must be 0.

**1c CPU timing.** HD and UHD, 4 channels, size 50 and 200, count 5, on one thread and on all threads.

- **ref-fir and ref-iir(D):** timed per stage (copy, rows, columns, accumulate, divide).
- **Pyramid variants:** timed over the whole frame, median of `--iters`.
- A value prefixed with `~` was extrapolated. The FIR's cost grows with its radius, and at size 200 its widest blur has a radius of several thousand pixels. A stage that would exceed `--budget-ms` is therefore timed on its leading lines and scaled up.

**2a GPU check.** A 480x270 RGBA plate.

- D on the GPU is compared against the CPU IIR sum.
- Each GPU pyramid is compared against the CPU pyramid.
- Pass is max rel <= 1e-4.

**2b GPU timing.** HD and UHD, size 50 and 200, count 5.

- The figure is kernel ms between two timestamps around the whole recorded frame: median of `--iters` after one warm-up.
- The pyramid is: downsample per level, then `recordSeparableBlur` per blur, then upsample-accumulate.
- D is `recordFilterWindow`, then per blur `recordLineBlur` into one reused temporary plus an accumulate dispatch.
- Uploads and downloads are excluded.

## Metrics

All errors are normalised by the reference's peak |ref| over the image:

- `max` = max |got − ref| / peak
- `rms` = sqrt(mean (got − ref)²) / peak
- `px` = max |got − ref| / (|ref| + 1e-3·peak), a per-pixel relative error that stops dividing by near-zero tails.

## Details that affect the numbers

**Variance correction.** var_down(k) is the variance the downsample chain adds, in level-k pixels²: v·(1 − 4^−k)/3, where v = 0.75 for [1 3 3 1]/8 and 0.25 for the box. var_up is the mean over output phases of f(1 − f) for bilinear. Catmull-Rom has none, since it reproduces quadratics. A blur at k = 0 uses sigma unchanged.

**Truncated FIR reference.** When the FIR radius ceil(3σ) reaches past a whole line, the quality reference uses a double-precision direct convolution with the same normalised weights. This happens at the larger sizes and counts. Under the black-outside boundary it is the same filter, computed at O(n²) per line instead of O(n·r). The timing rows always use the engine's `LineFilter`.

**Bit-exact seams.** Each sample's arithmetic does not depend on the region being rendered. A region is the dependency cone of the output, clipped to where the data is non-zero, and level origins are anchored in absolute coordinates.

**Not implemented.** The optional row-transposed IIR variant of D is not in the spike.

## Decision rule

Choose the pyramid if its relative RMS error vs reference stays under ~1e-3 at decimated levels and the seam test is exact, and D costs above ~40 ms at UHD on the XTX; otherwise ship D.
