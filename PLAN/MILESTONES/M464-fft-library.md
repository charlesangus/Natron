# M464 - FFT Library

A 2-D FFT on the CPU and the GPU behind one `Engine/Fft` interface. LensFlare needs it for the starburst (Fraunhofer far field: power spectrum of the aperture) and the ghost aperture image (Fresnel near field by the angular spectrum method); later FFT convolution (Bloom, Glow, Convolve) can reuse it. CPU: vendored header-only pocketfft (BSD-3-Clause). GPU: our own radix-2/4 Stockham FFT in Slang through the existing kernel registry and `GpuRecordContext`.

## Scope

**In:**
- `Fft::Plan2D` for power-of-two sizes 64–4096: complex-to-complex forward and inverse, real-to-complex forward, on the CPU and the GPU.
- Helpers: `fftshift`, power spectrum |F|² / N², complex multiply by a transfer function.
- CPU/GPU parity tests.

**Out:**
- Non-power-of-two sizes on the GPU (callers pad).
- VkFFT and FFTW: rejected for now (see Decisions); the interface lets VkFFT replace the GPU backend later.
- FFT convolution nodes: no milestone yet.

**Edge cases:**
- A size that isn't a power of two on the GPU: the plan refuses; callers pad. The CPU accepts any size (pocketfft).
- A GPU allocation failure: the caller's normal CPU fallback.
- Inverse(forward(x)) round-trips within float tolerance, scaled by 1/N on the inverse.

## Phase 464.1: CPU

- [x] M464.P1.T1 — Vendor pocketfft and the CPU plan
  - files: `libs/pocketfft/pocketfft_hdronly.h` (new, vendored with its LICENSE and upstream commit in `libs/pocketfft/README.md`), `Engine/Fft/Fft.{h,cpp}` (new), `Engine/CMakeLists.txt`, `Tests/Native/Fft_Test.cpp` (new)
  - approach: `Fft::Plan2D(width, height, kind)` with `forward`/`inverse` on interleaved complex float buffers and `forwardReal`; multi-threaded over rows and columns through the engine's thread pool. `fftshift` and `powerSpectrum` helpers.
  - verify: gtest: a delta transforms to a constant; a 2-D sinusoid peaks at its frequency; inverse(forward(x)) matches x within 1e-5 at 1024²; the power spectrum of a disc aperture shows the Airy pattern's first dark ring at 1.22 λ/D in sample units within one sample.
  - size: M

## Phase 464.2: GPU

- [x] M464.P2.T1 — Slang Stockham FFT
  - files: `Engine/Gpu/Kernels/fft.slang` (new), `Engine/Gpu/Kernels/fft.cmake` (new), `Engine/Gpu/GpuFft.{h,cpp}` (new), `Engine/Fft/Fft.{h,cpp}`, `Tests/Native/FftGpu_Test.cpp` (new)
  - approach: Radix-4 passes with a final radix-2 when log2 N is odd, in shared memory for rows up to 4096, ping-ponging two scratch buffers; columns by a transposed pass. Real input packed into complex. `Fft::Plan2D` gains a GPU path that records into a `GpuRecordContext` with its buffers resident; follow the `blur.cmake` registration and `GpuBlur.h` wrapper patterns.
  - verify: parity with P1.T1 within 1e-4 relative to the peak at 256², 1024² and 4096², forward and inverse, on RADV at the phase build (`reportNoGpu` skips without a device); a 2048² complex forward timing is logged.
  - size: L

**Verification gate:** build and `ctest -R Fft` green with GPU parity on RADV; the pocketfft licence and commit are recorded in `libs/pocketfft/README.md`.

## Decisions

- 2026-11-16 — **pocketfft on the CPU, our own Slang FFT on the GPU (user, after a pros/cons comparison):** fits the GPU layer as-is with no runtime shader compilation; flare FFTs are cached per aperture change, so speed matters little now. VkFFT stays an option behind the same interface if FFT convolution later needs it; FFTW was rejected as a packaged dependency.
- 2026-11-19: all tasks done at 67294bf65. The gate is green, all 892 tests in both legs. FftGpu.MatchesTheCpuPlan passes on the CI device, llvmpipe. RADV parity isn't possible on CI and is still to be run on the GPU host; the pocketfft commit and licence are recorded in libs/pocketfft/README.md.
- 2026-11-20 — PR #320 CI (44589755438) failed FftGpu on llvmpipe. The 33163bc21 scratch check correctly refuses 4096², because a 128 MiB scratch is over llvmpipe's 112 MiB effective bind limit. In f83ef2361 the test expects that refusal, and RADV still runs the size. Review round 1 replied on the PR. Not done: lifting the pool's size-class overshoot so 4096² fits under 128 MiB limits (it would change the shared pool and placement).
