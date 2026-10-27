# EdgeBlur

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| EdgeBlur (EdgeBlur) | IIR | `kEdgeBlurTolerance` (GPU) | GPU against CPU, 2e-4 absolute plus 1e-4 relative: EdgeDetect's IIR kernels, whose roundings the edge weight's power steepens near 0, and IBlur's quantised summed-area tables |

Blurs the image where the matte has edges. The matte is the alpha of the source, or with External
Matte the alpha of the Matte input. On a layer without alpha there are no edges and the image is
unchanged.

- **Edges.** EdgeDetect's gradient magnitude of the matte (`EdgeDetectOp`), with the Filter knob's
  filter at Size, no erosion and no non-maximum suppression, on EdgeDetect's buffer: the matte's
  nearest pixel repeated, the region grown by the detection's reach and cropped to the matte's format.
  A sharp step of the matte from 0 to 1 gives an edge of about 1.
- **Weight.** `w = min(1, pow(edge, 1 / edgeMult))`, 0 where the edge is not positive
  (`EdgeBlur::edgeWeight()`, IBlur's Map remap with that exponent). A larger Edge Mult (default 2)
  widens the blurred band.
- **Blur.** IBlur's variable blur with the weight as its Map: each pixel is blurred by `Size * w`
  (Size defaults to 3), three boxes of that pixel's radius approximating the Gaussian of standard
  deviation `Size * w / 2.4`, on the processed channels, black outside the image. A pixel whose `w`
  is 0 is the input bit for bit. The Filter knob picks the edge detection's filter only.
- **Why not a mix.** Mixing one fixed blur in by `w` adds the blurred edge, spread over the blur's
  whole reach, to a fading copy of the sharp edge, which shows as a second step (a ghost) where `w`
  is between 0 and 1. A blur whose size falls with `w` keeps a single, monotonic transition.
- **Identity.** Size 0, or External Matte with no Matte connected, leaves the input unchanged.
- **Region.** The source is read over the blur's reach plus, when it is the matte, the edge detection's;
  the Matte over the blur's reach plus the edge detection's. The weight is computed over every pixel the
  blur reads. The region of definition grows by the blur's reach unless
  Crop To Format clips it, as Blur's does. Sizes scale with the render scale.
- **Knobs.** External Matte, Size, Filter (EdgeDetect's eight filters, Gaussian by default), Crop To
  Format, Edge Mult, and the standard channel, mask and mix knobs. Size is the blur at full edge
  weight; Edge Mult shapes how fast the blur size falls off the edge. Mask, mix and "(Un)premult by" are
  the host's stages.

EdgeBlur replaces the `fr.inria.EdgeBlur` PyPlug, every control kept under its name: the PyPlug's
Blur1channels, Blur1mix and Merge1maskInvert are the standard channel, mix and mask-invert knobs.
The PyPlug raised the edges to 1 / Edge Mult with a Gamma node and fed them to its Blur as a mask;
here the weight is clamped to [0, 1] and sets the blur size per pixel, so an edge above 1 does not push
the result past the blur, and no sharp copy of the edge shows through the blurred one.

Tests: `Tests/Native/NativeEdgeBlur_Test.cpp`. The edge weight; a transparent Matte, a missing Matte
and Size 0 are bit-identical to the input; with a finite edge filter the blur stays on a box matte's
border, from the source's alpha and from the Matte's; a larger Edge Mult blurs at least as many
pixels; a blurred step is monotonic, with a single peak in its slope (no ghost). The goldens (`Golden`: default, External Matte, Edge Mult 4 with Sobel uncropped, and mask and
mix) are compared bit for bit.

GPU: EdgeDetect's GPU recorder writes the edges into a buffer over the pixels the blur reads, and
IBlur's recorder (`IBlur::recordPassesGpu()`) reads that buffer as its Map; the Matte is the pass
runner's extra input. A box radius above IBlur's GPU cap stays on the CPU.
Byte, short and half images stay on the CPU (`Tests/Native/NativeEdgeBlurGpu_Test.cpp`).
