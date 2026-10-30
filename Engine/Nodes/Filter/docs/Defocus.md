# Defocus

Defocus the image as a lens out of focus does: every pixel spreads into a bokeh, a disc, an
aperture of straight or curved blades, or the image connected to the Kernel input. The kernel
library, `BokehKernel` (`Engine/Nodes/Filter/BokehKernel.{h,cpp}`), is engine-free and is shared
with ZDefocus.

1. **Highlight boost.** With a `highlightGain` above 1, a value `v` above `highlightThreshold` `t`
   becomes `t + (v - t) * gain` before the gather and a value above `t` after it becomes
   `t + (v - t) / gain`, so bright out-of-focus highlights bloom into the bokeh's shape while the
   rest of the image keeps its brightness. Gain 1 skips both.
2. **Kernel.** `size` is the bokeh's diameter, in pixels at the render scale. Its radii are
   `size * aspectRatio / 2` across (divided by the pixel aspect ratio) and `size / 2` up. Below half
   a pixel on both the node is the identity, bit for bit. A weight is the fraction of its pixel
   the shape covers, supersampled along the shape's edge, and the weights are normalised to sum
   to 1, so a constant image stays constant.
   - **Disc.** The ellipse filling the radii.
   - **Blades.** An aperture of `blades` (3 to 16) corners. At `rotation` 0 a corner points up and
     the rotation is counter-clockwise, in degrees. `roundness` 0 gives straight blades and 1 the
     disc.
   - **Image.** The Kernel input's `kernelChannel`, in any of its layers, read over the input's whole
     region of definition (a Kernel larger than 16 Mpixels is read over its image only), with
     its longer side stretched over the diameter and centred. Each weight is the area average of the
     values its pixel covers. Negative and non-finite values count as 0, and the weights are
     normalised. A Kernel input that is disconnected, or whose channel is empty or black, gives the
     single tap, so the node is the identity, and posts a warning.
3. **Gather.** Each output pixel is the sum of the source under the kernel. The CPU builds a prefix
   table per row, in double, and sums the kernel's runs of equal weight from it, so the gather is
   exact whatever the size; a larger size costs only the kernel's rows. The image is black
   outside its region of definition. The Mask and Mix limit the result as the standard knobs do:
   each processed channel of the output is `in + mix * (out - in)`.

| Parameter | Label | Default | Meaning |
|---|---|---|---|
| `size` | Size | 10 | Bokeh diameter in pixels; 0 to 500; below one pixel the node is the identity |
| `aspectRatio` | Aspect Ratio | 1 | The bokeh's width over its height; 0.1 to 10 |
| `kernelType` | Kernel Type | Disc | Disc, Blades (`blades`) or Image (`image`) |
| `blades` | Blades | 6 | Blades kernel: number of blades, 3 to 16 |
| `rotation` | Rotation | 0 | Blades kernel: degrees counter-clockwise; at 0 a corner points up |
| `roundness` | Roundness | 0 | Blades kernel: 0 straight blades to 1 the disc |
| `kernelChannel` | Kernel Channel | `rgba.A` | Image kernel: the Kernel input's channel that holds the bokeh; a channel it lacks, or None, reads 0 |
| `highlightThreshold` | Highlight Threshold | 1 | Values above it are highlights, at least 0 |
| `highlightGain` | Highlight Gain | 1 | Factor on the highlights' excess over the threshold, 1 to 1000; 1 leaves them as they are |

The standard channel, mask, mix and unprocessed-channel knobs follow. The inputs are Source, an
optional Mask and an optional Kernel. Every layer of the source is defocused.

Departures from Nuke's Defocus:

- `size` is the bokeh's diameter, and its default is 10 where Nuke's is 1 (open to the user).
- `aspectRatio` replaces Nuke's "ratio".
- There are no "scale", "quality" or "method" knobs: the gather is exact, so there is no
  quality to trade.
- Highlight boost (`highlightThreshold`, `highlightGain`) and the blade `roundness` are new.

GPU: Disc and Blades kernels whose reach is at most 256 pixels from the centre on either axis run
on the GPU as a gather over the same runs, in float. A larger size, an Image kernel (the Kernel
input is never read on the GPU) and a buffer beyond the kernels' 32-bit indices render on the CPU.
The GPU holds the node's own CPU render (`Tests/Native/NativeDefocusGpu_Test.cpp`).

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| Defocus (Defocus) | GPU against CPU | `ParityTolerance::make(1e-5, 1e-4)` | 1e-5 absolute, 1e-4 relative; the CPU sums in double, the GPU in float |

The CPU properties (size 0 is bit-identical, a point source reproduces the kernel, a constant is
preserved, the highlight boost, the Image kernel and its warnings, region independence and the
golden) are in `Tests/Native/NativeDefocus_Test.cpp`.
