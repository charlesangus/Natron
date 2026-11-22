# AdjustBBox

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| AdjustBBox | exact | `ParityTolerance::exact()` | pixels are copied |

AdjustBBox grows the source's region by `numpixels` (in canonical units; negative values crop).
The Edge Mode knob (`edgeMode`) sets what fills the added border: Input takes the source's own
edge mode, Black fills it with zero, and Stretch repeats the nearest source pixel. Stretch reads a halo of the pad around the requested window, so a tile renders the same pixels
as the whole frame. The GPU copies pixels with the same clamp and is bit-exact
(`Tests/Native/NativeAdjustBBoxGpu_Test.cpp`).
