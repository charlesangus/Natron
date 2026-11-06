# IErodeDilate

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| IErodeDilate | exact | `ParityTolerance::exact()` (GPU) | no OFX reference; a white Map with the square shape is held bit for bit to ErodeDilate |

The variable-size ErodeDilate, `IErodeDilate`, ErodeDilate's I-variant in the I-variant registry; every knob they
share has the same name.

- **Map.** As IBlur's ([IBlur.md](IBlur.md)): an aux Map input from `MapInput`, its channel (alpha by default)
  inverted, clamped to [0, 1] and remapped by Map Gamma and Map Response, a black pixel outside the Map's image (inverted and remapped like any other), the standard Mask and Mix
  still limiting the result. The signed radius of an axis at a pixel is `lerp(sizeAtBlack, size, map)`, computed in
  float and clamped between the two, so it is monotonic in the Map and may cross 0 where Size and Size At Black have
  opposite signs, switching that pixel from eroding to dilating. A disconnected Map filters at Size everywhere and
  shows a warning badge.
- **Element.** Square: the (2 rx + 1) x (2 ry + 1) rectangle, as ErodeDilate's. Disc: the pixels of the ellipse of half
  axes rx and ry, whose row half-widths are computed exactly in integers. The element is clipped to the source
  image, as ErodeDilate clamps its window. A fractional radius blends the results of its floor and its ceiling,
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
- **Sizes.** -1000 to 1000 on each axis, fractional, default 1, scaled by the render scale and, as ErodeDilate's,
  not divided by the pixel aspect ratio. A positive radius erodes and a negative one dilates.
- **Stages.** Up to four stages run in turn, each reading the previous one's result: 0 erodes and 1 dilates with
  the element of each pixel's positive (or, for 1, negated negative) radii, and 2 (erode) and 3 (dilate) filter Y
  alone. A pixel whose axes have the same sign takes its 2D element in stage 0 or 1; one whose axes have opposite
  signs filters X in stage 0 or 1 and then Y in stage 2 or 3, its element degenerating to two lines. Only the stages
  the Map's range of radii can need run, so non-negative sizes run stage 0 alone. A pixel near a crossing reads its
  neighbours' results from the earlier stages, so a dilating pixel next to eroding ones grows the eroded values.
- **Region.** The region of interest grows by the sum of the stages' largest radii, since a tile's Map is not known
  when it is requested. The Map is read over the render window when one stage runs, and over the window grown by
  the later stages' radii when more do. The region of definition grows by the largest dilating radius unless
  Expand RoD is off. A tile whose Map gives radius 0 throughout is a pass-through copy.
- **Measured exceptions.** A white Map with the square shape at a whole Size matches ErodeDilate bit for bit,
  except for ErodeDilate's own quirk on lines of length halfWidth + 2, and the sign of a zero.

Tests: `Tests/Native/NativeIErodeDilate_Test.cpp`. The row queries, the exact ellipse half-widths and the whole
filter match brute force; the bounded-memory tile matches the definition bit for bit by both strategies and any
batching, infinities and signed zeros included (and NaNs, as NaNs), and a radius of 1000 on a 256 x 256 tile matches brute force; a black Map is bit-identical to the input; a white Map with the square shape matches
ErodeDilate at whole sizes of either sign with or without a Map; the disc matches a brute-force disc; a Map ramp gives a monotonic
result; a disconnected Map warns. The goldens (`Golden`: square, disc, Size At Black, a Map remap of gamma 0.5 with
the Smooth response, and mask and mix) are compared bit for bit.

GPU: `ierodedilate.slang` builds the same tables in global memory per channel, then runs each pixel's queries, stage
by stage through a domain-sized buffer, and holds `ParityTolerance::exact()` against the CPU, a Map ramp whose radius
crosses 0 included; the Map's remap is `mapremap.slang`, the CPU's float operations in
the same order, gamma included. A radius over 255 pixels on either axis stays on the CPU, which
keeps the ellipse arithmetic in 32-bit integers, as do byte, short and half images
(`Tests/Native/NativeIErodeDilateGpu_Test.cpp`).
