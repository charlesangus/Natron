# EdgeDetect

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| EdgeDetect | IIR | `kEdgeDetectTolerance` | see the exceptions below |

- **EdgeDetect, RGBA processing.** In this host the OFX EdgeDetect always feeds
  alpha into the rms, max and tensor modes: `Node.cpp` keeps the plugin's own
  RGBA switches on and applies the channel set afterwards. The native node
  combines only the processed channels, as upstream Natron's plugin does. Parity
  pairs therefore set the channel set to process RGBA on both sides; with fewer
  channels the two legitimately differ.

- **`-Ofast`.** The OFX plugins are compiled with `-Ofast`, whose ulp
  differences flip tied pixels under Sobel plus non-maxima suppression, because
  erosion leaves flat plateaus that suppression compares exactly. The recorded
  EdgeDetect case uses the Gaussian gradients, which are bit-exact. Do not read a
  Sobel-plus-suppression mismatch as a native bug without checking for ties.
