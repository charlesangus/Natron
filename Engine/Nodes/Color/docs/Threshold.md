# Threshold

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| Threshold | arithmetic | `ParityTolerance::arithmetic()` | the ramp is one subtraction and one division |

- **Maths.** Per channel, with `lo = level - softness` and `hi = level + softness`: 1 at or
  above `hi`, else 0 at or below `lo`, else `(v - lo) / (hi - lo)`.
- **Deliberate change.** At softness 0 a value equal to the level gives 1. The OpenFX plug-in
  tested `value <= level - softness` first and gave 0 there.

The kernel compares and divides in the CPU's order (`Tests/Native/NativeColourUtilGpu_Test.cpp`).
