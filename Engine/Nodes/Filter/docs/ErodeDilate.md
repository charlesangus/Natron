# ErodeDilate

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| ErodeDilate | exact | `kTolerance` | rectangular min and max are exact in float |

One node, `ErodeDilate`, with a signed Size per axis, as Nuke's FilterErode: a positive size erodes (the minimum
over the rectangle of 2*|size|+1 pixels), a negative size dilates (the maximum), 0 leaves the axis unchanged. The
range is -1000 to 1000, scaled by the render scale and rounded down in magnitude, so a size whose scaled magnitude is
under 1 on both axes is an identity whichever its sign. Axes of the same sign take the rectangle's extremum; axes of
opposite signs filter X and then Y, each with its own operator. Expand RoD grows the region of definition by |size|
on each dilating axis only.

ErodeDilate holds exact, bit for bit on NaNs, signed zeros and denormals (`Tests/Native/NativeErodeDilateGpu_Test.cpp`).
The parity references (`Tests/fixtures/native-parity/ErodeDilate`) were recorded from CImgErode and CImgDilate, the
dilating cases at the negated size. `Tests/Native/NativeErodeDilate_Test.cpp` covers erode, dilate, size 0 as an
identity, and opposite signs matching an X-only node followed by a Y-only node.
