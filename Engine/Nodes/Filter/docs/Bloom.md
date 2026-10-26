# Bloom

The mean of `count` blurs of the input, of sizes `size * ratio^i` for `i = 0` to `count - 1`. Each
blur is one of Blur's filters, Gaussian (FIR) by default. As in CImg Bloom, `count` is at least 1;
a ratio of at most 1 gives one blur, and one blur has ratio 1. `count` is at most 20 and `ratio`
at most 8. Each blur's size, in pixels at the render scale, is capped at 65536, wider than any
image, so the halo and the pyramid stay bounded whatever the settings. Size 0, the default, is the
identity, as is any setting whose largest blur is Blur's identity.

The Gaussian (FIR) blurs run on a decimated pyramid. Each level halves the previous one along one
axis with the `[1 3 3 1] / 8` kernel. A blur of standard deviation `sigma` along an axis runs on
the deepest level `k` along it where `sigma / 2^k` is still at least 2.5. There its standard
deviation is `sqrt((sigma / 2^k)^2 - (1 - 4^-k) / 4)`, the downsampling's variance taken out, so
every FIR kernel reads at most 15 samples on each side. The blurred level is brought back with
Catmull-Rom (bicubic) upsampling and added in. Below sigma 5 (size 12) a blur runs at full
resolution and is exactly Blur's FIR Gaussian. The level samples sit at fixed positions in pixel
coordinates, so a tile matches the full frame. The other filters run every blur at full
resolution, as Blur runs them.

Outside the image the blurs repeat the nearest pixel, so the region of definition is the source's.
Nothing is clamped: negative and over-range values go through. Mask and mix are the host's stages.
There is no "(Un)premult by": a blur of a premultiplied image is the premultiplied blur, so the
source is bloomed as it comes. Glow uses the same bloom on its thresholded highlights.

Bloom replaces the OFX CImg Bloom, whose ID, `net.sf.cimg.CImgBloom`, is retired
(`Engine/OfxRetired/`): a project naming it gets the missing-plug-in node.

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| Bloom (Bloom) | resampling | `ParityTolerance::make(0., 1e-4)` | GPU against CPU, FIR Gaussian on the pyramid, 1e-4 relative |
| Bloom (Bloom) | IIR | `ParityTolerance::iir()` | GPU against CPU, the recursive and box filters, as Blur's |

The CPU properties (size 0 is the identity, a constant is preserved, a point keeps its energy,
count 1 is Blur's FIR Gaussian below size 12, negatives go through, a tile matches the full
frame, extreme settings render finite) are in `Tests/Native/NativeBloom_Test.cpp`; the GPU parity is in
`Tests/Native/NativeBloomGpu_Test.cpp`.
