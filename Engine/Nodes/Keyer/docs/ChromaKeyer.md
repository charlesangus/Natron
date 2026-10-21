# ChromaKeyer

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| ChromaKeyer | transcendental / measured | `kChromaKeyerTolerance`, `kChromaKeyerUnpremultipliedTolerance` | the unpremultiplied output uses 1e-4 relative |

- **ChromaKeyer, unpremultiplied output: 1e-4 relative**
  (`ParityTolerance::make(0., 1e-4)`). The OFX plugin is built with `-Ofast`, so
  its key differs from the native one by a few ulps, and the unpremultiplied
  output divides the suppressed colour by a key alpha that can be near zero,
  magnifying them. 5e-5 fails; the other `show` outputs stay at the
  transcendental class.

ChromaKeyer holds its CPU class, transcendental, in the intermediate, premultiplied and composite modes, with every source alpha option, Bg and masks or not, and an RGB source; the host derives the key's per-frame constants as the CPU does and the device keys in float. The unpremultiplied mode holds 1e-3 relative: it divides by the key alpha, so a one-ulp difference in the float key the CPU rounds from double becomes a relative error of about 6e-8 over that alpha (a float model of the kernel measured 1.02e-4 on the parity source, where the alpha reaches 5.8e-4) (`Tests/Native/NativeChromaKeyerGpu_Test.cpp`).
