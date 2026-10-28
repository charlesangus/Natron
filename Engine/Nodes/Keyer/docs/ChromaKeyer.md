# ChromaKeyer

ChromaKeyer keys in the scene-linear working space. The key colour and the
Source are split into luma and chroma (Y'PbPr) with the working space's luma
weights (`ProjectColorManagement::getWorkingLuma`, AP1 under ACEScg). There is
no transfer encoding, no colourspace choice and no clamp. Foreground values
above 1 or below 0 survive the suppression, and the key alpha follows the lift
and gain ramp past 0 and 1.

- Inputs: Source, Bg (composited under the key in Composite mode), Mask (the
  host mask).
- The host footer limits the result. Mask and Mix blend the keyed output
  against the Source.
- Source alpha "Inside Mask" keeps the key below one minus the Source's own
  alpha. "Normal" composites with it. An RGB Source has no alpha.
- The output is always RGBA, with the foreground key as alpha.

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| ChromaKeyer | transcendental | `kChromaKeyerTolerance`, `kChromaKeyerUnpremultipliedTolerance` | the unpremultiplied output uses 1e-3 relative on the GPU |

The GPU holds the CPU's class, transcendental, in the intermediate,
premultiplied and composite modes, for every source alpha option, with or
without Bg and the host mask, and for an RGB source. The host derives the
per-frame constants as the CPU does, and the device keys in float.

The unpremultiplied mode holds 1e-3 relative. It divides by the key alpha, so a
one-ulp difference in the float key, which the CPU rounds from double, grows by
the reciprocal of that alpha (`Tests/Native/NativeChromaKeyerGpu_Test.cpp`).
