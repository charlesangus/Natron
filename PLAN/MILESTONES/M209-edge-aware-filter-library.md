# M209 - Edge-Aware Filter Library

A shared CPU and GPU library of the edge-aware building blocks that darktable's scene-referred modules are made of, ported from darktable `src/common/` at the `DARKTABLE_REF` pinned in `tools/ci/local/fetch-assets.sh`:
- `box_filters.cc`
- `guided_filter.c`
- `fast_guided_filter.h`
- `eigf.h`, the exposure-invariant guided filter
- `dwt.c`, à-trous B-spline wavelet decomposition and recomposition
- `heal.c`, Poisson healing

Each one is a function over float buffers, not a node. M210 - Profiled Denoise, M211 - Wavelet Retouch, M212 - Tone Equalizer, M213 - Contrast And Texture and M215 - Colour Equalizer call them, and M130 - Native Edge-Preserving And Directional Filters' guided mode may.

The raw-only fast guided filter already in `Engine/Nodes/Raw/RawHighlightsLaplacian.cpp` (its header cites `fast_guided_filter.h`) moves into this library, and RawHighlights calls the shared copy.

See `DECISIONS/2026-10-25-darktable-ai-and-image-tool-ports.md`.

## Scope

**In:**
- **`Engine/Nodes/Filter/EdgeAware/`**, holding:
  - `BoxFilter` (mean and variance, separable, any radius, O(1) per pixel)
  - `GuidedFilter`, plain and fast (subsampled), with a guide and a target
  - `Eigf`, exposure-invariant, on log luminance
  - `Wavelets`, à-trous B-spline: decompose into N scales plus a residual, recompose, with darktable's scale-to-sigma mapping
  - `PoissonHeal`, darktable's multigrid/SOR heal with a mask
- A Slang GPU kernel for each, under `Engine/Gpu/Kernels/`, with CPU/GPU parity tests.
- RawHighlights' Laplacian mode switched to the shared guided filter, bit-exact or within its current tolerance.
- A README section.

**Out:**
- Bilateral grid and the permutohedral lattice: M130 - Native Edge-Preserving And Directional Filters owns Bilateral.
- Local Laplacian (`locallaplacian.c`): its consumer, bilat, is excluded as display-referred.
- Any node: M210–M215.

**Edge cases:**
- **Borders:** darktable's mirrored-edge convention throughout, documented per function.
- **A radius larger than the image:** clamped, with the same result as darktable.
- **Non-finite input:** treated as 0 in the statistics and passed through at that pixel.
- **Very small images** (smaller than the subsampling factor): the fast guided filter falls back to the plain one.

**Assumptions:**
- darktable's `src/common/` files are self-contained enough to port per file, without the pixelpipe. P1.T1 confirms this.

## Phase 209.1: CPU library

- [ ] M209.P1.T1 — Box filter and guided filters
  - files: new `Engine/Nodes/Filter/EdgeAware/BoxFilter.{h,cpp}`, new `Engine/Nodes/Filter/EdgeAware/GuidedFilter.{h,cpp}`, `Engine/CMakeLists.txt`
  - approach: port `box_filters.cc`, `guided_filter.c` and `fast_guided_filter.h`, threaded over rows the way existing Filter kernels are (`Engine/Nodes/Filter/BlurKernels.cpp`). Keep darktable's parameter meanings: radius, epsilon and subsampling.
  - verify: P1.T4's tests pass.
  - size: L

- [ ] M209.P1.T2 — EIGF and wavelets
  - files: new `Engine/Nodes/Filter/EdgeAware/Eigf.{h,cpp}`, new `Engine/Nodes/Filter/EdgeAware/Wavelets.{h,cpp}`
  - approach: port `eigf.h`, and `dwt.c`'s decompose and recompose with its B-spline kernel and scale bookkeeping (`dwt_get_max_scale` and the like). Keep the API scale-indexed, so retouch can edit one scale.
  - verify: P1.T4's tests pass.
  - size: L

- [ ] M209.P1.T3 — Poisson heal, and move RawHighlights onto the library
  - files: new `Engine/Nodes/Filter/EdgeAware/PoissonHeal.{h,cpp}`, `Engine/Nodes/Raw/RawHighlightsLaplacian.cpp`
  - approach:
    - Port `heal.c`.
    - Replace the private fast guided filter in `RawHighlightsLaplacian.cpp` with a call to `GuidedFilter` and delete the duplicate code.
  - verify: P1.T4's tests pass, and `ctest -R NativeRawHighlights` is unchanged.
  - size: M

- [ ] M209.P1.T4 — CPU tests
  - files: new `Tests/Native/EdgeAware_Test.cpp`
  - approach:
    - **Box filter:** matches a brute-force mean within 1e-6.
    - **Guided filter:** with `eps` → 0 and guide = target it is identity; it matches a direct per-window formula on 64² images.
    - **Fast guided filter:** within its documented error of the plain one.
    - **EIGF:** scaling the input by k scales the output by k (exposure invariance).
    - **Wavelets:** decompose then recompose is identity within 1e-6, and the energy per scale for white noise follows darktable's ratios.
    - **Poisson heal:** a masked hole in a linear ramp is filled with the ramp within 1e-3.
  - verify: `ctest -R EdgeAware` passes.
  - size: M

## Phase 209.2: GPU kernels

- [ ] M209.P2.T1 — Box filter and guided filter kernels
  - files: new `Engine/Gpu/Kernels/boxfilter.slang`, `boxfilter.cmake`, `guidedfilter.slang`, `guidedfilter.cmake`, new `Engine/Nodes/Filter/EdgeAware/EdgeAwareGpu.{h,cpp}`
  - approach:
    - Separable passes with shared-memory row sums, using darktable's `data/kernels/guided_filter.cl` as the reference.
    - Host entry points mirror the CPU API over `GpuBuffer` and are recorded in a `GpuRecordContext` (`Engine/Gpu/GpuRecord.h:29`), as `Engine/Gpu/GpuBlur.*` is.
  - verify: P2.T3's parity tests pass.
  - size: L

- [ ] M209.P2.T2 — EIGF, wavelet and heal kernels
  - files: new `Engine/Gpu/Kernels/eigf.slang`, `eigf.cmake`, `wavelets.slang`, `wavelets.cmake`, `heal.slang`, `heal.cmake`, `Engine/Nodes/Filter/EdgeAware/EdgeAwareGpu.cpp`
  - approach: port from darktable's `.cl` kernels where they exist (`data/kernels/`). Heal runs its SOR iterations as repeated dispatches.
  - verify: P2.T3's parity tests pass.
  - size: L

- [ ] M209.P2.T3 — GPU parity tests and README
  - files: new `Tests/Native/EdgeAwareGpu_Test.cpp`, `Engine/Nodes/README.md`
  - approach:
    - Each function's GPU output matches its CPU output on random and edge-case images within 1e-4 relative, and Heal within 1e-3.
    - Add an "Edge-aware library" section to the README listing the functions and their darktable origins.
  - verify: `ctest -R EdgeAwareGpu` passes on lavapipe.
  - size: M

**Verification gate:** the full build and ctest pass, including `EdgeAware` and `EdgeAwareGpu` on lavapipe and RADV, with RawHighlights tests unchanged.

## Decisions

- 2026-10-27 — **darktable's behaviour over the plan's wording (bosley-native-nodes, PR #136 review):** borders follow each darktable function (box/guided shrinking windows, B-spline clamp, hat-transform mirror, gaussian edge start) rather than the plan's "mirrored everywhere", because the library exists to port darktable modules faithfully; EIGF works on linear values with normalised variance; heal is red/black SOR (darktable has no multigrid). P1.T3: RawHighlightsLaplacian never had a private fast guided filter (plan error); only its B-spline decomposition and sigma-per-step helper moved to the library, results unchanged. Pinned darktable golden vectors (Codex finding) are deferred to the first module port that needs them (M210 reference parity).

Shipped 2026-10-27: PR #136, squash a08602f38 on main.
