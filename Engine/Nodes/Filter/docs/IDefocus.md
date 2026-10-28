# IDefocus

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| IDefocus | measured | `kIDefocusTolerance` (GPU) | no OFX reference; the CPU is held to Defocus at a constant Map (1e-5) and to its goldens, the GPU to the CPU at 1e-5 absolute, 1e-4 relative |

The variable-size Defocus: Defocus's bokeh, its size set per pixel by a Map input. Defocus is its base node in the
I-variant registry (`IDefocus.reg.inc`); every knob they share has the same name, and the bokeh is Defocus's
`BokehKernel` (disc, blades or the Kernel input's image) with the same highlight boost.

- **Map.** The Map input, from `MapInput` (`MapInput.h`), as IBlur reads it: Map Channel, Invert Map, Map Gamma and
  Map Response give a value in [0, 1], black outside the Map's image, and the size at a pixel is
  `lerp(sizeAtBlack, size, map)`. Size At Black is 1-D, as Size is, and both stop at 500.
- **No Map.** With the Map disconnected the node defocuses at Size everywhere, as Defocus does, and shows a warning
  badge. An Image kernel with no Kernel input leaves the image unchanged and warns about the Kernel instead.
- **Scatter, not gather.** Each source pixel spreads into the bokeh of its own size, and each output pixel is the sum
  of what reaches it over the sum of those weights. A sharp pixel beside an out-of-focus highlight receives the
  highlight's bloom, up to the highlight's radius and no further; IBlur gathers over the output pixel's own size
  instead, so its sharp pixels stay sharp next to any blur. Pixels that nothing but themselves reaches with the
  single tap keep the source bit for bit, so a black Map with Size At Black 0 is the identity.
- **Levels.** The Map value is rounded to one of N + 1 levels, N being four per pixel of radius between Size At Black
  and Size (at most 1024), so the radius steps by at most a quarter pixel. Level 0 has exactly Size At Black and
  level N exactly Size, so a constant white Map matches Defocus at Size. Each level's kernel is built once, on first
  use, and kept until a knob, the scale or the Kernel image changes.
- **Algorithm.** For each output row, every source pixel within the largest halo that the Map reaches in the tile
  adds its kernel's row at that offset, run by run, to running difference arrays of weight and weighted value, in
  double; a prefix sum gives each pixel's totals. A row is skipped when no pixel in it reaches that far. Cost grows
  with the area of the largest bokeh.
- **NaN and infinity.** A NaN or infinite source counts as 0 in the sums, on the CPU and the GPU alike.
- **Region.** The region of interest, of the source and of the Map, grows by the halo at the larger of Size and
  Size At Black, since the Map is not known when the region is requested. The source reads black outside its image,
  and the region of definition grows by the same halo. Sizes scale with the render scale and the pixel aspect ratio,
  as Defocus's do.
- **Knobs.** Size At Black, Size, Aspect Ratio, Kernel Type, Blades, Rotation, Roundness, Kernel Channel, Highlight
  Threshold, Highlight Gain, the Map knobs (Map Channel, Invert Map, Map Response, Map Gamma) and the standard
  channel, mask and mix knobs. No premultiply knob, as for every filter.

Tests: `Tests/Native/NativeIDefocus_Test.cpp`. A constant Map, white, grey with a Size At Black, or absent, matches
Defocus within 1e-5 for the disc and a stretched aperture; a black Map is bit-identical to the input; a highlight on
the white side of a half Map blooms onto the black side no further than its radius; a constant input stays constant
with and without the boost; sub-windows match the full render; a disconnected Map warns. The goldens (`Golden`:
default, Size At Black inverted, blades, boost, a Map remap, the Image kernel, and mask and mix) are compared bit
for bit.

GPU: `idefocus.slang` loads each pixel's level, by `mapremap.slang` exactly as the CPU remaps it, and its boosted
source, then gathers every output pixel over its apron from the levels' weight table, in float. The GPU holds
`kIDefocusTolerance` against the CPU (`Tests/Native/NativeIDefocusGpu_Test.cpp`). A halo over 64 pixels and the
Image kernel stay on the CPU, as do byte, short and half images. `gpuCostHint()` reports the bokeh's diameter as a
size-scaled sigma (2 r / 2.4 at the larger radius), as Defocus does.
