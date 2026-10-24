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

- **GPU.** Every setting renders on the GPU unless the erosion radius exceeds
  the morphology kernel's 1024 pixels, and holds `iir()`, 1e-4 absolute,
  against the CPU render (`Tests/Native/NativeEdgeDetectGpu_Test.cpp`, which the
  build runs on lavapipe; no measured exception is recorded). The passes run
  over the CPU's buffer, the window grown by the halo and clipped to the region
  of definition: the source fill and the finite-difference stencil match it bit
  for bit, the derivative and pre-blur line filters are Blur's, with their state
  in two floats where the CPU keeps it in double, and the normalisation,
  channel combination and non-maxima suppression run in two floats where the CPU
  works in double, since a float rounding flips the suppression on near ties.
  Ties that the CPU itself decides by a rounding still flip: the parity sweep
  uses the parity source's noise, and keeps suppression with a blur size above 0
  to channels without the alpha bands' symmetric steps.
