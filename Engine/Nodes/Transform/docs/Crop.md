# Crop

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| Crop | exact / resampling | `kExactTolerance`, `kSoftTolerance` | softness 0 copies pixels; a soft edge is a double smoothstep rounded to float |

Crop runs on the GPU in the offset form with its separable matte. At softness 0 the GPU is exact,
and so is every pixel whose fade weight is 0 (written 0) or at least 1 (the source pixel kept).
A partial weight multiplies in two-float on the device and is held to `resampling()`. A source
pixel that is NaN or infinite on a zero-weight edge or border comes out 0 on both sides, which is
why Crop is major 3. The GPU source region is Crop's own region of interest
(`Tests/Native/NativeCropGpu_Test.cpp`).
