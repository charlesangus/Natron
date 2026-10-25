# ColorMatrix

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| ColorMatrix | arithmetic, plus an absolute bound | `kMatrixTolerance` | single-precision products summed in a fixed order; against a double reference the error follows the terms, not the result, so 5e-6 absolute covers cancellation |

- **Clamp Black defaults off.** The OpenFX plug-in defaulted it on; the native node starts as
  an exact identity.
- **Missing channels.** An RGB or XY image reads alpha as 1; an Alpha image reads R, G and B
  as 0, as the OpenFX plug-in did.

The kernel sums in the CPU's order with `precise` arithmetic
(`Tests/Native/NativeColourUtilGpu_Test.cpp`).
