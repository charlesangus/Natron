# Difference

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| Difference | arithmetic | `ParityTolerance::arithmetic()` | |

Difference writes B's colour and, with `d = sum((A - B)^2)` over the red, green and blue
channels, an alpha of `gain * max(0, (d - blackPoint) / (whitePoint - blackPoint))`. Black Point
(default 0) is the difference that gives 0 and White Point (default 1) the one that gives 1, so
the defaults give `d` itself. When White Point is at or below Black Point the matte is a hard
threshold: Gain where `d > blackPoint`, 0 elsewhere. The matte is clamped to 0 to 1, so a negative
Gain gives 0 and a large Gain or difference gives 1. Without A, B is compared with itself, and
outside A's region A reads zero.

The alpha is clamped to 0 to 1 again after the mask and mix, so a partial mix or mask blending the
matte with a B whose alpha lies outside 0 to 1 still gives an alpha within it; a NaN gives 0. B's
alpha is left as it is wherever the node writes none: with the alpha channel not selected, with Mix
at 0 (the node passes B through), and where the mask is 0. The clamp is the `ClampAlpha` stage of
the node's pipeline, applied by the same blend on the CPU and the GPU (`pixelwrap.slang`).

The GPU kernel sums the squares in
the CPU order with `precise` arithmetic, multiplies by the same `1 / (whitePoint - blackPoint)` the
CPU computes, and holds arithmetic (1e-6 relative)
(`Tests/Native/NativeDifferenceGpu_Test.cpp`).
