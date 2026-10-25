# HueKeyer

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| HueKeyer | arithmetic | `ParityTolerance::arithmetic()` | the hue, saturation and both trapezoids run statement for statement on both sides; Vulkan division is within 2.5 ulp |

A clean-sheet keyer (`Tests/Native/NativeHueKeyer_Test.cpp`). It does not port OpenFX's curve-based
HueKeyer (`net.sf.openfx.HueKeyer`), whose amount and saturation-threshold curves it replaces
with two trapezoids:

- **Hue** (`hueLow`, `hueLowFull`, `hueHighFull`, `hueHigh`), in turns from red at 0, measured
  going up the hue circle from `hueLow`, so the trapezoid may wrap across red. The defaults key
  greens: 0.25, 0.30, 0.37, 0.42.
- **Saturation** (`satLow`, `satLowFull`, `satHighFull`, `satHigh`), chroma over the largest of
  R, G and B, 0 when that is not positive, and not clamped. Points out of order are pushed up to
  the one before. Defaults 0.1, 0.3, 1, 1: grey keys 0 unless `satLow` is at most 0, and
  saturation above 1 (negative channels) keys 0 unless `satHigh` is raised.
- **Alpha** is the product of the two, or one minus it with `invert`. Only alpha is written; RGB
  passes through, and the output is RGBA whatever the source. Mask and mix blend the key with
  the source alpha.

The kernel follows the CPU statement for statement with `precise` arithmetic
(`Tests/Native/NativeHueGpu_Test.cpp`).
