# libs/noise

Header-only 4D noise generators, `Noise4D.h` (namespace `noise4d`): Perlin, SuperSimplex,
value, white and cellular noise on a split coordinate (an integer cell and a fraction per axis).
Natron's noise node samples them through `Engine/Nodes/Generator/NoiseCore.h`, which adds the
fractal combiners and the domain warp; `Engine/Gpu/Kernels/noisecore.slang` mirrors both line for
line for the GPU.

## Upstream

A plain C++ port of [FastNoise2](https://github.com/Auburn/FastNoise2) at revision `721400f95`
(`721400f9560831be2fc9790be9b72c44bee7d33f`), files
`include/FastNoise/Generators/{Perlin,Simplex,Value,Cellular,BasicGenerators,Fractal,Utils}.inl`.
FastNoise2 and FastSIMD are not vendored: one FastSIMD lane becomes one scalar, following
FastSIMD's generic (SSE2/AVX2) code path. MIT, see `LICENSE` and `NOTICE`.

## Rules

- Keep the C-like subset (scalars, small POD structs by value, no references or library calls
  but `sqrt`) so the Slang mirror stays line for line. Change both files together.
- No fused multiply-add: `a * b + c` stays unfused, and the Slang kernel is compiled with
  `-fp-mode precise`. The CPU result is then the same on every x86-64 machine and bit-equal to
  the GPU wherever Vulkan rounds correctly.
- Float constants are spelled as bit patterns, with `static_assert`s against the expressions
  they round.
