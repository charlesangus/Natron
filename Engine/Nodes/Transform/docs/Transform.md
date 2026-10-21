# Transform

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| Transform | resampling / IIR | `kResamplingTolerance`, `kMotionBlurTolerance` | motion blur averages up to hundreds of samples |

Transform and TransformMasked hold their CPU class, `resampling()`, for every filter with clamp and black outside on and off, with mask and mix, at mipmap 0 and 1; sample positions travel in two floats so the filters pick the CPU's taps. Motion blur at quality 0.5, 1 and 4 over every shutter offset, and directional blur, hold `iir()`: each pixel draws the CPU's van der Corput sequence over the same host-built matrices and stops on the same variance test, its sums kept in two floats. The device filters each sample in float where the CPU uses double, a few ulps apart, so a pixel whose variance quotient lies that close to an integer takes one sample more or fewer and moves by about `motionBlurMaxError()` / sqrt(samples); up to one pixel in 250 of a window (at least 2) may therefore miss `iir()`, by no more than `motionBlurMaxError()` (quality / 1000), the standard error the sampler itself targets. Matching the CPU's sample counts exactly would need double-precision filtering on the device. A transform concatenated from upstream stays on the CPU (`Tests/Native/NativeTransformGpu_Test.cpp`).
