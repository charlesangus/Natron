# EdgeExtend

Extends the colour at a matte's edge outwards, into the area the matte leaves clear. Opaque pixels
keep their colour.

1. With Premultiply Input on (the default), the colour is multiplied by the alpha.
2. Each of the `slices` slices (default 5) blurs the running result and merges the result over
   its blur, `a + b * (1 - alpha a)`, on all four channels. Slice `i` of `n` blurs at `i` times
   `size / sqrt(n (n + 1) (n + 0.5) / 3)`, so the slices' blurs add up to one of `size` (default
   10).
3. With Unpremultiply Output on (the default), the colour is divided by the grown alpha where
   that exceeds 1e-6 and the alpha is the source's: the extended colour under the original matte.
   Without it the output is premultiplied, with the grown alpha.

The blurs are FIR Gaussians (Blur's "Gaussian (FIR)", standard deviation size / 2.4) that repeat
the nearest pixel outside the image, so the region of definition is the source's and nothing
spreads past the sum of the slices' radii. The node works on the four channels at once: with
fewer processed it passes the input through, as the OFX CImg EdgeExtend does. Mask and mix are
the host's stages; the node does its own premultiplication. Nothing is clamped.

EdgeExtend replaces the OFX CImg EdgeExtend, whose default IIR filter it does not offer: that
filter's tails spread colour indefinitely, and its documentation recommends FIR.

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| EdgeExtend (EdgeExtend) | resampling | `kTolerance` | GPU against CPU, FIR Gaussian slices |

The CPU properties (opaque pixels and alpha unchanged, the edge colour outside the box, the
kernels' reach, the premultiplied output, size 0 is the identity) are in
`Tests/Native/NativeEdgeExtend_Test.cpp`; the GPU parity is in
`Tests/Native/NativeEdgeExtendGpu_Test.cpp`.
