# Difference

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| Difference | arithmetic | `ParityTolerance::arithmetic()` | |

Difference writes B's colour and an alpha of `max(0, gain * sum((A - B)^2) - offset)` over the
red, green and blue channels. The matte is not clamped above 1. Without A, B is compared with
itself, and outside A's region A reads zero. The GPU kernel sums the squares in the CPU order
with `precise` arithmetic and holds arithmetic (1e-6 relative)
(`Tests/Native/NativeDifferenceGpu_Test.cpp`).
