# Glow

Bloom applied to the highlights of the image, then added or screened over it.

1. **Map.** The luminance `L` of a pixel is taken with the project's working-space luma weights,
   restricted to the processed colour channels and renormalised so that a grey of value `v` has
   luminance `v`. With no colour channel processed, a processed alpha is its own luminance. The
   kept luminance is `c = softKnee(L - threshold, knee)`: 0 below the threshold,
   `x^2 / (2 knee)` over the knee, and `x - knee / 2` above it, which keeps the curve and its slope
   continuous. The map is the pixel scaled by `c / L`, then multiplied by the mask (1 without
   one) and, with `isolateHue`, by the hue key: `max(0, 1 - d / 50)`, where `d` is the distance in
   degrees around the hue circle between the pixel's hue and `pickHue`'s, as the PyPlug's HSVTool
   keyed it. Hues are HSV hues of the processed colour channels (an unprocessed one reads 0); a
   grey or black pixel has hue 0.
2. **Grade.** Each colour channel `m` of the map becomes
   `brightness * tint * (c + saturation * (m - c))`, saturation taken about the map's luminance.
   A processed alpha is scaled by `brightness` only.
3. **Bloom.** The graded map goes through Bloom's kernel: `count` blurs of sizes
   `size * ratio^i`, horizontally `size`, vertically `size * (1 - stretch)`, with Blur's filter
   (Gaussian (FIR) by default, on the pyramid). Outside the image it is black. With a stretch,
   `rotate` turns the bloom: the stretch's long axis turns clockwise by `rotate` degrees, as the
   PyPlug's transforms turned it. A multiple of 180 degrees changes nothing and 90 or 270 swaps the
   axes exactly; any other angle resamples the map bilinearly into a frame turned
   counter-clockwise by `rotate` (in square pixels), blooms it there, and resamples the bloom back.
4. **Combine.** `addInput` adds the graded map to its bloom. A gamma other than 1 raises the
   positive glow to `1 / gamma`. The glow is then added to the source, screened over it
   (`a + g - a g` where either is at most 1, else the larger), or output alone (`glowOnly`). A
   processed alpha is clamped to [0, 1].

`preview` outputs the ungraded map (step 1) instead. `expandRoD` grows the region of definition by
the bloom's reach; off, the glow is cropped to the source's region. Alpha is not processed by
default. "(Un)premult by" is the host's stage. The mask is not a host mix: it multiplies the map
inside the node (step 1), so the glow of a masked-out highlight does not spill into the mask's
area, and there is no mix or mask-invert control.

The map's luma weights are read from the project's working space when the parameters are baked.
Every node's render hash folds in the project's content digest, which covers the working space,
so switching it re-renders Glow.

| Parameter | Default | Meaning |
|---|---|---|
| `threshold` | 0.25 | Luminance below which nothing glows |
| `knee` | 0.5 | Width of the soft knee above the threshold; 0 is a hard threshold |
| `isolateHue` | off | Key the map by `pickHue`'s hue |
| `pickHue` | (0, 0, 0) | The colour whose hue `isolateHue` keeps (black: hue 0, red) |
| `preview` | off | Output the map (step 1) |
| `size` | 3 | The first blur's size, in canonical pixels |
| `stretch` | 0 | Shrinks the vertical (turned: the short) size by `1 - stretch` |
| `rotate` | 0 | Degrees the stretch turns clockwise; no effect without a stretch |
| `ratio` | 2 | Growth of the size from one blur to the next |
| `count` | 7 | Number of blurs |
| `filter` | Gaussian (FIR) | Blur's filter |
| `tint` | (1, 1, 1) | Per-channel gain of the map |
| `brightness` | 1 | Gain of the map |
| `saturation` | 1 | Saturation of the map about its luminance |
| `gamma` | 1 | The positive glow is raised to `1 / gamma` |
| `screen` | off | Screen the glow over the source instead of adding it |
| `addInput` | off | Add the graded map to its bloom |
| `glowOnly` | off | Output the glow alone |
| `expandRoD` | off | Grow the region of definition by the bloom's reach |

The Glow PyPlug's controls map as follows. "isolate highlights" is `threshold` (plus `knee`,
which is new; 0 is a hard threshold). "Isolate Hue", "pick Hue" (`pickHue`), "Rotate", "size",
"stretch", "ratio", "iterations" (`count`), "filter", "screen", "add input", "glow only" and
"expand RoD" keep their meaning and defaults. Its post-grade gain is `tint` times `brightness`, and
its gamma and saturation are single values. Its "alpha" checkbox is the channel set's A. Its mask
multiplied the map in the rotated frame without turning the mask; here the mask multiplies the map
in the image, where it lies on the source. The preview is the map in the image, not turned.

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| Glow (Glow) | resampling | `ParityTolerance::make(0., 1e-4)` | GPU against CPU, FIR Gaussian on the pyramid, 1e-4 relative |
| Glow (Glow) | IIR | `ParityTolerance::iir()` | GPU against CPU, the recursive and box filters, as Blur's |

The CPU properties (a threshold above the image is the identity, the glow alone plus the source
is the output, screening, the soft-kneed preview, saturation and tint, the expanded region of
definition, tiles, rotation, hue isolation, the mask and the working space) and the GPU parity
(including rotated and hue-isolated cases) are in `Tests/Native/NativeGlow_Test.cpp`.
