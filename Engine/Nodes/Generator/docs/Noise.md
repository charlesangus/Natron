# Noise

Generates 4D noise with no input. `type` picks the basis: Perlin, Simplex (OpenSimplex2S), Value,
Cellular (Voronoi) or White. `fractal` sums `octaves` octaves of it (at most 16; a larger request is
capped) as fBm, Turbulence (fBm over the absolute value), Ridged or Hybrid multifractal, each octave
`lacunarity` times the frequency and `gain` times the amplitude of the one before. `warpType` adds a
domain warp: each sample point is first moved by up to `warpAmount` noise cells along a Perlin or
Simplex field at `warpFrequency` times the noise's frequency. The kernels are ported from
FastNoise2 (MIT; see `libs/noise/`).

Cellular noise takes a `metric` (Euclidean, Manhattan, Chebyshev), an `output` (F1, F2, F2 - F1, Cell
Value, Distance to Edge, Smooth F1) and a `jitter`. Distance to Edge is exact for Euclidean and
(F2 - F1) / 2 for Manhattan and Chebyshev.

The pattern lives in canonical coordinates. A pixel centre goes through the inverse of `translate`,
`rotate` and `scale` about `center`, is divided by `noiseSize` (pixels per noise cell; the extent
knobs already own `size`), and the third and fourth axes are `zOffset` and `evolution`. So the
output does not depend on how the image is tiled, on its region of definition or on the render
scale. Coordinates are split into an integer cell and a fraction, so the pattern keeps its detail
far from the origin. Animating `evolution` changes the pattern continuously. With a positive
`loopPeriod`, `evolution` walks a circle of that circumference through the third and fourth axes
instead, so the animation repeats seamlessly every `loopPeriod` of evolution; the 2D pattern then
loops through 4D noise.

White noise is a random value per noise cell, so at `noiseSize` 1 every pixel has its own value.
It changes only with the seed and the position unless `reseedPerFrame` is on, which draws a new
pattern on every frame.

The noise value n, in [-1, 1], becomes t = (1 + `amplitude` n) / 2 + `offset`, and each channel is
`color0` + (`color1` - `color0`) t, unclamped. With `decorrelate`, each channel computes its own
noise from the seed plus a per-channel offset; otherwise one value fills every channel. The default
extent is the project format.

This node replaces openfx-misc Noise (Rand), CImg Noise and CImg Plasma; it is not a port of any of
them and has no parity reference.

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| Noise | exact | `kNoiseTileTolerance` | Tile, strip and region splits against a whole-frame render, on CPU. |
| Noise | 1e-4 absolute | `kNoiseScaleTolerance` | Render scale 1/2 against the full-scale pattern at the same canonical points, and rotate/translate/scale against the plain pattern moved by whole pixels: the mapping rounds its products differently. Not applied to white noise. |

GPU: held to the node's own CPU render (`Tests/Native/NativeNoiseGpu_Test.cpp`), at mipmap 0 and 1.
The core and the kernel use no fused multiply-add (`-fp-mode precise`), and the pixel-to-noise
mapping uses only additions, multiplications and floors, so white noise is bit-exact
(`ParityTolerance::exact()`), through a fractal too: the kernel corrects the device's quotient in the
fractal's normalising division so it rounds once, as the CPU's does. Perlin, Simplex, Value, the fractals and the warp are within 1e-5
absolute plus 1e-6 relative (`kNoiseGpuTolerance`): Vulkan rounds division and square root to a few
ulps. Cellular outputs that depend on which feature point is nearest (Cell Value, Distance to Edge)
may differ where two points tie; the test allows such pixels up to 0.5% of the image.
