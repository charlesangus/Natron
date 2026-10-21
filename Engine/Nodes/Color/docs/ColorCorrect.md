# ColorCorrect

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| ColorCorrect | transcendental | `kColorCorrectTolerance` | tone curves evaluated lock-free |

ColorCorrect holds its CPU class, transcendental, in every group and luminance option, with the tone curves read from the CPU's table inside [0, 1] and evaluated from their keys outside it, as ColorLookup does, and the tone weights clamped to [0, 1] on both sides whatever the output clamps are (shared through `CurveSnapshot::packGpu` and `Kernels/curvesnapshot.slang`, so in two floats past a steep key between two floats too); a curve of more than 128 keys, or one `packGpu` cannot bound within 1e-6 of the CPU, stays on the CPU (`Tests/Native/NativeColorCorrectGpu_Test.cpp`).
