# IBlur

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| IBlur | measured | `kIBlurTolerance` (GPU) | no OFX reference; the CPU is held to its goldens, the GPU to the CPU at 1e-4 relative |

The variable-size Blur: Blur's size, scaled per pixel by a Map input. Blur (`net.sf.cimg.CImgBlur`) is its base
node in the I-variant registry (`IBlur.reg.inc`); every knob they share has the same name.

- **Map.** The Map input, from `MapInput` (`MapInput.h`), is an aux input: the host does not blend it, and the
  standard Mask and Mix still limit the result. Map Channel picks one channel of any of its layers (alpha by
  default), Invert Map inverts it, and Map Black and Map White remap it to [0, 1], clamped. Outside the Map's image
  the raw value is 0, so the remap and invert still apply there. The size at a pixel is
  `lerp(sizeAtBlack, size, map)` per axis.
- **No Map.** With the Map disconnected the node blurs at Size everywhere and shows a warning badge.
- **Exact zero.** Where the size is 0 on both axes the output is the input bit for bit, with no ringing from the
  blurred pixels around it (`Size At Black` defaults to 0, so a black Map leaves its pixels unchanged).
- **Algorithm.** Three passes of a 2-D box of the pixel's radii (`VariableBlurKernels.h`). Each pass sums the
  previous one's output into a summed-area table per plane (double precision, tile-local origin) and reads every
  box from it in O(1), so the cost per pixel does not depend on the size. The radius gives the three boxes the
  variance of Blur's Gaussian of standard deviation size / 2.4, horizontally divided by the pixel aspect ratio as
  Blur does; a fractional radius covers the box's end pixels in part. At a constant size the result is within
  about 2% RMS of Blur's FIR Gaussian (accepted in the milestone's decisions).
- **Region.** The region of interest, of the source and of the Map, grows by the three boxes' reach at the larger
  of Size and Size At Black, since the Map is not known when the region is requested. The source reads black
  outside its image, and the region of definition grows by the same reach unless Crop To Format clips it, as
  Blur's does. Sizes scale with the render scale.
- **Knobs.** Size (2-D, 0 to 1000), Crop To Format, the Map knobs (Map Channel, Invert Map, Map Black, Map White,
  Size At Black, with no maximum) and the standard channel, mask and mix knobs. Blur's filter choice has no
  counterpart: IBlur has the one filter.

Tests: `Tests/Native/NativeIBlur_Test.cpp`. A black Map is bit-identical to the input; a white Map is within 2% RMS of
Blur at sizes 4 and 12; a half-black Map leaves the black half untouched right up to the edge of the white; a constant
input stays constant; a disconnected Map warns. The goldens (`Golden`: default, Size At Black, a Map remap, and mask
and mix) are compared bit for bit.

GPU: `iblur.slang` runs the same three boxes through the pass runner, reading the Map as an extra input. Each plane's
summed-area table is summed exactly in 64-bit integers after a power-of-two scale, and the box sums are weighted in
two-float arithmetic, so the GPU holds `kIBlurTolerance`, 1e-4 relative, against the CPU without fp64; detail over an
offset of 1000 holds `ParityTolerance::make(5e-4, 1e-6)`. A box radius over 512 pixels stays on the CPU: Size stops
at 1000, a radius near 240, so only a large Size At Black reaches it. Byte, short and half images stay on the CPU
(`Tests/Native/NativeIBlurGpu_Test.cpp`).
