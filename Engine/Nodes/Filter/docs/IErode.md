# IErode

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| IErode, IDilate | exact | `ParityTolerance::exact()` (GPU) | no OFX reference; a white Map with the square shape is held bit for bit to Erode and Dilate |

The variable-size Erode and Dilate: one template, `IErodeDilateNode<ErodeDilateMode>`, registered twice
(`IErode.reg.inc`, `IDilate.reg.inc`) with Erode (`net.sf.cimg.CImgErode`) and Dilate (`net.sf.cimg.CImgDilate`) as
their base nodes in the I-variant registry; every knob they share has the same name.

- **Map.** As IBlur's ([IBlur.md](IBlur.md)): an aux Map input from `MapInput`, its channel (alpha by default)
  inverted, clamped to [0, 1] and remapped by Map Gamma and Map Response, 0 outside the Map's image (neither inverted nor remapped), the standard Mask and Mix
  still limiting the result. The radius of an axis at a pixel is `lerp(sizeAtBlack, size, map)`, computed in float
  and clamped between the two, so it is monotonic in the Map. A disconnected Map filters at Size everywhere and
  shows a warning badge.
- **Element.** Square: the (2 rx + 1) x (2 ry + 1) rectangle, as Erode's. Disc: the pixels of the ellipse of half
  axes rx and ry, whose row half-widths are computed exactly in integers. The element is clipped to the source
  image, as Erode clamps its window. A fractional radius blends the results of its floor and its ceiling,
  bilinearly over the two axes, so the result moves smoothly with the Map; a whole radius takes the element's
  extremum exactly. A pixel of radius 0 on both axes is the input bit for bit.
- **Algorithm.** The result is defined row by row: each row's span is two lookups in the row's sparse table (level k
  the minimum or maximum of 2^k values), and the rows reduce from the top. The CPU computes that definition bit
  for bit with bounded memory (`IErodeDilateKernels::TilePlan`), by the cheaper of two strategies. Rows, for any
  shape: the row tables are built a batch of rows at a time and each pixel folds every batch into its elements, so
  a pixel costs O(ry) per element and the tables never exceed a fixed budget per plane (16 MB), whatever the
  radius. Columns, for the square: for each output column and each horizontal radius it uses, the row spans are
  written down a column and a NaN-skipping sparse table down that column answers any vertical radius in O(1), so a
  pixel's query cost does not depend on its radius; the columns are batched to the same budget. At radius 1000 on
  a 256 x 256 tile, the square takes about 0.16 s per plane instead of 8 s, and the tables 16 MB instead of 194 MB
  per plane.
- **Sizes.** 0 to 1000 on each axis, fractional, default 1, scaled by the render scale and, as Erode's, not divided
  by the pixel aspect ratio. Unlike Erode's they are never negative: IErode only erodes and IDilate only dilates,
  so converting a base node with a negative size gives the opposite I-node, and a base node with one positive and
  one negative axis has no I-variant equivalent.
- **Region.** The region of interest grows by the larger radius of Size and Size At Black, since a tile's Map is
  not known when it is requested; the Map itself is read over the render window only. IDilate's region of
  definition grows by as much unless Expand RoD is off; IErode's never grows. A tile whose Map gives radius 0
  throughout is a pass-through copy.
- **Measured exceptions.** A white Map with the square shape at a whole Size matches Erode and Dilate bit for bit,
  except for Erode's own quirk on lines of length halfWidth + 2, and the sign of a zero.

Tests: `Tests/Native/NativeIErodeDilate_Test.cpp`. The row queries, the exact ellipse half-widths and the whole
filter match brute force; the bounded-memory tile matches the definition bit for bit by both strategies and any
batching, infinities and signed zeros included (and NaNs, as NaNs), and a radius of 1000 on a 256 x 256 tile matches brute force; a black Map is bit-identical to the input; a white Map with the square shape matches Erode
and Dilate at whole sizes with or without a Map; the disc matches a brute-force disc; a Map ramp gives a monotonic
result; a disconnected Map warns. The goldens (`Golden`: square, disc, Size At Black, a Map remap of gamma 0.5 with
the Smooth response, and mask and mix) are compared bit for bit.

GPU: `ierodedilate.slang` builds the same tables in global memory per channel, then runs each pixel's queries, and
holds `ParityTolerance::exact()` against the CPU; the Map's remap is `mapremap.slang`, the CPU's float operations in
the same order, gamma included. A radius over 255 pixels on either axis stays on the CPU, which
keeps the ellipse arithmetic in 32-bit integers, as do byte, short and half images
(`Tests/Native/NativeIErodeDilateGpu_Test.cpp`).
