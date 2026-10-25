# Glow

Bloom applied to the highlights of the image, then added or screened over it.

1. **Map.** The luminance `L` of a pixel is taken with the project's working-space luma weights,
   restricted to the processed colour channels and renormalised so that a grey of value `v` has
   luminance `v`. With no colour channel processed, a processed alpha is its own luminance. The
   kept luminance is `c = softKnee(L - threshold, knee)`: 0 below the threshold,
   `x^2 / (2 knee)` over the knee, and `x - knee / 2` above it, which keeps the curve and its slope
   continuous. The map is the pixel scaled by `c / L`.
2. **Grade.** Each colour channel `m` of the map becomes
   `brightness * tint * (c + saturation * (m - c))`, saturation taken about the map's luminance.
   A processed alpha is scaled by `brightness` only.
3. **Bloom.** The graded map goes through Bloom's kernel: `count` blurs of sizes
   `size * ratio^i`, horizontally `size`, vertically `size * (1 - stretch)`, with Blur's filter
   (Gaussian (FIR) by default, on the pyramid). Outside the image it is black.
4. **Combine.** `addInput` adds the graded map to its bloom. A gamma other than 1 raises the
   positive glow to `1 / gamma`. The glow is then added to the source, screened over it
   (`a + g - a g` where either is at most 1, else the larger), or output alone (`glowOnly`). A
   processed alpha is clamped to [0, 1].

`preview` outputs the ungraded map (step 1) instead. `expandRoD` grows the region of definition by
the bloom's reach; off, the glow is cropped to the source's region. Alpha is not processed by
default. Mask, mix and "(Un)premult by" are the host's stages: the mask mixes the output with the
source rather than masking the map.

The Glow PyPlug's controls map as follows. "isolate highlights" is `threshold` (plus `knee`,
which is new; 0 is a hard threshold). Its hue isolation is not ported. "size", "stretch", "ratio",
"iterations" (`count`), "filter", "screen", "add input", "glow only" and "expand RoD" keep their
meaning. Its post-grade gain is `tint` times `brightness`, and its gamma and saturation are single
values. Its "alpha" checkbox is the channel set's A. Its rotation is not ported.

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| Glow (Glow) | resampling | `ParityTolerance::make(0., 1e-4)` | GPU against CPU, FIR Gaussian on the pyramid, 1e-4 relative |
| Glow (Glow) | IIR | `ParityTolerance::iir()` | GPU against CPU, the recursive and box filters, as Blur's |

The CPU properties (a threshold above the image is the identity, the glow alone plus the source
is the output, screening, the soft-kneed preview, saturation and tint, the expanded region of
definition, tiles) and the GPU parity are in `Tests/Native/NativeGlow_Test.cpp`.
