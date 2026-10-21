# Saturation

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| Saturation | transcendental | `kSaturationTolerance` | the ACES AP1 option is exempt: native uses the real AP1 coefficients, where the OFX plugin falls through to CCIR 601 |

- **Deliberate fix.** Saturation's ACES AP1 coefficients above.

Saturation holds its CPU class, transcendental, for every luminance option; the kernel takes the
weights from the CPU's `ColorMath::luminance` (`Tests/Native/NativeSaturationGpu_Test.cpp`).
