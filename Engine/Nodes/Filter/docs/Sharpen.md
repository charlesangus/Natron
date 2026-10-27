# Sharpen

An unsharp mask. The detail is the input minus its blur, `d = in - blur`. The mode says what is
done with it:

- **Sharpen** writes `in + amount * w * d`.
- **Soften** writes `in - amount * w * d`.
- **Laplacian** writes `amount * d`.

`w` is the threshold's weight. It reads the detail relative to the local blurred value,
`r = |d| / max(|blur|, 1e-6)`. At threshold 0, the default, `w` is 1. Otherwise `w` is 0 up to
`r = threshold` and rises linearly to 1 at `r = 2 * threshold`. Because `r` is relative, the
threshold treats a plate scaled by any factor the same way. Laplacian ignores it.

The blur is one of Blur's filters (size and filter knobs, Gaussian (FIR) by default, size 3). It
repeats the nearest pixel outside the image, so the region of definition is the source's. Nothing
is clamped: negative and over-range values go through. Mask, mix and "(Un)premult by" are the
host's stages.

Sharpen replaces the OFX CImg Sharpen, Soften and Laplacian plug-ins as one node. Their IDs
(`net.sf.cimg.CImgSharpen`, `net.sf.cimg.CImgSoften`, `net.sf.cimg.CImgLaplacian`) are retired
(`Engine/OfxRetired/`): a project naming one gets the missing-plug-in node.

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| Sharpen (Sharpen) | resampling | `kFirTolerance` | GPU against CPU, FIR Gaussian, 1e-5 absolute |
| Sharpen (Sharpen) | IIR | `ParityTolerance::iir()` | GPU against CPU, the recursive and box filters, as Blur's |

The CPU properties (amount 0 is the identity, Laplacian of a constant is 0, Sharpen is the input
plus the Laplacian, the threshold is scale invariant, negatives go through) are in
`Tests/Native/NativeSharpen_Test.cpp`; the GPU parity is in
`Tests/Native/NativeSharpenGpu_Test.cpp`.
