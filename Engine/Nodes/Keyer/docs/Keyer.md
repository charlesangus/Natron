# Keyer

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| Keyer | transcendental | `kKeyerTolerance` | |

- **Deliberate fix.** Keyer and ChromaKeyer with an RGB source treat alpha as 1 for Normal and add nothing for "Add to Inside Mask", where OFX reads out of bounds, and neither mirrors OFX's output-premult preference.

Keyer holds its CPU class, transcendental, in every key, output and source alpha mode, with each of InM, OutM and Bg or none, partly covering the window, an RGB source and a channel subset; the host decides every threshold comparison and derives the key colour terms in double, and the device takes the distance to the key axis from the orthogonal part of the colour rather than the CPU's squared-norm difference, which float cannot hold near the axis (`Tests/Native/NativeKeyerGpu_Test.cpp`).
