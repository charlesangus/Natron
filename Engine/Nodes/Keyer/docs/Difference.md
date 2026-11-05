# Difference

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| Difference | arithmetic | `ParityTolerance::arithmetic()` | |

Difference writes B's colour and, with `d = sum((A - B)^2)` over the red, green and blue
channels, an alpha of `gain * max(0, (d - blackPoint) / (whitePoint - blackPoint))`. Black Point
(default 0) is the difference that gives 0 and White Point (default 1) the one that gives 1, so
the defaults give `d` itself. When White Point is at or below Black Point the matte is a hard
threshold: Gain where `d > blackPoint`, 0 elsewhere. The alpha is then clamped to 0 to 1, so a negative Gain gives 0 and a large Gain or
difference gives 1. Without A,
B is compared with itself, and outside A's region A reads zero. The GPU kernel sums the squares in
the CPU order with `precise` arithmetic, multiplies by the same `1 / (whitePoint - blackPoint)` the
CPU computes, and holds arithmetic (1e-6 relative)
(`Tests/Native/NativeDifferenceGpu_Test.cpp`).
