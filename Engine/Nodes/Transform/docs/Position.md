# Position

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| Position | exact | `kPositionTolerance` | |

Position holds its CPU class, exact, on the GPU: the offset-form resampler copies whole pixels, so
nothing is computed. A channel left out reads the source under the output pixel, unshifted, from the
region the device is given (the shifted window); a pixel whose unshifted position lies outside that
region reads zero there, as the CPU does when the source it fetched is exactly that region
(`Tests/Native/NativePositionGpu_Test.cpp`).
