# Keyer

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| Keyer | transcendental | `keyerTolerance()` | |

- **Redesign, no migration.** The key value is the source's luminance (by default the working space's luma weights) or its projection on the key colour; the alpha is the trapezoid A <= B <= C <= D over it, RGB passes through unpremultiplied, and the host mask and mix replace the InM, OutM and Bg inputs. Despill, the output and source-alpha modes and the Screen and None key modes are gone; projects saved with the old Keyer do not load its settings.

Keyer holds its CPU class, transcendental, for every key source and luminance formula, hard and soft edges, the host mask, inverted mask and mix, and the alpha channel alone; the CPU and the device compute the key value and the ramps in float with the same roundings, and only the device's division differs (`Tests/Native/NativeKeyerGpu_Test.cpp`).
