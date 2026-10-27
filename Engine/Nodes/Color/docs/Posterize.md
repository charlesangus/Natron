# Posterize

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| Posterize | exact | `ParityTolerance::exact()` | the GPU kernel is bit-exact with the CPU for every dither: the matrices are the same tables, and the random dither uses the same integer hash |
| Posterize, wrapped render | arithmetic | `ParityTolerance::arithmetic()` | the mask and mix blend after the op |

The node replaces OpenFX's Quantize with every control and its defaults, plus Range Min and
Range Max (`Tests/Native/NativePosterize_Test.cpp`).

Maths, per processed channel, for a value `v`, `d = v - rangeMin` and `N` levels:

- **No dither.** `level = floor(t)` when `t = d·N / range` is at or below 0, else `ceil(t - 1)`:
  Quantize's rounding, whose bins are half open at the bottom, so a value on a bin's upper edge
  stays on that level.
- **Dithered.** With `l = d·(N - 1) / range`, the level is `floor(l)`, or one above it when the
  fraction `l - floor(l)` is above a threshold: the matrix entry at the pixel, wrapped, for the
  Bayer 2x2, 4x4 and 8x8 and the void-and-cluster 14x14 and 25x25 blue-noise matrices, or a
  hash of the pixel, the channel and the seed for Random.
- **Output** `level·range / (N - 1) + rangeMin`.

- **Levels** (default 16) need not be whole. Below 2, or with Range Max at or below Range Min,
  the node is an identity.
- **Outside the range** values step on at the same spacing; nothing is clamped.
- **Seed and Static Seed.** The random dither's seed is hashed with the frame unless Static Seed
  is on, as Quantize did, so a given seed and frame give the same noise every render.

The kernel reads the same threshold tables and hash with `precise` arithmetic
(`Tests/Native/NativePosterizeGpu_Test.cpp`).
