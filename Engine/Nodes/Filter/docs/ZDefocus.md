# ZDefocus

Defocus the image by depth, as a lens focused at `focalPlane` does: every pixel spreads into a
bokeh whose size its depth sets. The bokeh shapes, the highlight boost and the gather are
Defocus's (see `Defocus.md`); the kernel library is `BokehKernel`.

1. **Depth.** The `depth` channel of the Source, in any of its layers, defaults to `depth.Z`. A
   value of 0 or below, or not a number, is infinitely far, as is a missing channel. With
   `depthUnits` Distance the value is the distance to the camera; with Inverse Distance (the
   default, as most renderers write it) it is one over the distance, larger nearer. `focalPlane`
   and `depthOfField` are in the same units.
2. **Circle of confusion.** Each depth gives a signed diameter in pixels, negative in front of
   the in-focus slice, positive behind it, 0 inside it and monotone in distance on each side. With
   `E` the nearest edge of the in-focus slice and `D` the distance, it is `a * |1/E - 1/D|`,
   capped at `maxSize`.
   - **Manual.** The slice of depths `depthOfField` wide around `focalPlane` is sharp, and `a` is
     chosen so that infinitely far behind the slice the diameter is `size`: `size` is the
     diameter at infinity. With a zero `size` nothing blurs.
   - **Lens.** A thin lens of `focalLength` (mm), `fStop` and `filmBackWidth` (mm), focused at
     `focalPlane`, with `a = f^2 / (N (S - f)) * S` for the focus distance `S`, converted from
     millimetres to pixels by the image's width over the film back. `sceneUnit` is the unit of the
     depth channel, so that distances compare with the focal length. Distances nearer than the
     focal length, and a focus nearer than it, give `maxSize`. The depth of field is the lens's,
     so `depthOfField` and `size` are not used, and doubling `fStop` halves the bokeh.
   The image's width is the width of the output format times its pixel aspect ratio.
3. **Slices.** The depths are cut into `layers` slices: one in focus, and the others spread evenly
   over the diameters in front of it and behind it, in proportion to those ranges and at least one
   for each side that blurs. A pixel falls between two slices and is shared between them by a tent
   weight. A pixel whose bokeh would be below half a pixel of radius is in focus.
4. **Gather.** Each slice gathers its layer and its coverage with its own kernel (radii from its
   diameter, `aspectRatio` and the pixel aspect ratio). The slices are laid over one another back
   to front, the farthest first, each normalised by its coverage, so a sharp foreground does not
   bleed into a blurred background, and a blurred foreground still spreads over the background
   behind it. The image is black outside its region of definition.

`output` Focal Plane shows the source with its colour half red in front of the focus, green in it
and blue behind it, at full size whatever the render scale, whose in-focus limit is a diameter of
under one pixel; Result is the defocus. The Mask and Mix limit the result as the standard knobs do.

The `focalPoint` overlay (a point in the image) sets `focalPlane` to the depth under it when the
user moves it; restoring a project or undoing a Focal Plane edit does not refocus.

| Parameter | Label | Default | Meaning |
|---|---|---|---|
| `depth` | Depth | `depth.Z` | The Source's channel that holds the depth |
| `depthUnits` | Depth Units | Inverse Distance | Distance or Inverse Distance |
| `mode` | Mode | Manual | Manual or Lens |
| `focalPlane` | Focal Plane | 1 | The depth in focus, in the Depth Units |
| `depthOfField` | Depth Of Field | 0 | Manual: the width of the sharp slice of depths around the focal plane, at least 0 |
| `size` | Size | 10 | Manual: the bokeh's diameter infinitely far behind the depth of field, 0 to 500 |
| `maxSize` | Max Size | 50 | The largest bokeh diameter whatever the depth, 0 to 500; the slices spread over the diameters up to it |
| `focalLength` | Focal Length | 50 | Lens: millimetres, 1 to 5000 |
| `fStop` | F Stop | 2.8 | Lens: the f-number, 0.5 to 128 |
| `filmBackWidth` | Film Back Width | 36 | Lens: millimetres the image's width spans, 0.1 to 1000 |
| `sceneUnit` | Scene Unit | Metres | Lens: the depth channel's unit (Millimetres, Centimetres, Metres, Inches, Feet) |
| `focalPoint` | Focal Point | (0.5, 0.5) | Normalised image point; moving it sets Focal Plane to the depth there; a viewer overlay |
| `layers` | Layers | 16 | Number of depth slices, 2 to 64; more are smoother and slower |
| `aspectRatio` | Aspect Ratio | 1 | The bokeh's width over its height; 0.1 to 10 |
| `kernelType` | Kernel Type | Disc | Disc, Blades or Image, each slice stretched over its own diameter |
| `blades` | Blades | 6 | Blades kernel: 3 to 16 |
| `rotation` | Rotation | 0 | Blades kernel: degrees counter-clockwise; at 0 a corner points up |
| `roundness` | Roundness | 0 | Blades kernel: 0 straight blades to 1 the disc |
| `kernelChannel` | Kernel Channel | `rgba.A` | Image kernel: the Kernel input's channel that holds the bokeh |
| `highlightThreshold` | Highlight Threshold | 1 | Values above it are highlights |
| `highlightGain` | Highlight Gain | 1 | Factor on the highlights' excess over the threshold, 1 to 1000 |
| `output` | Output | Result | Result or Focal Plane |

The standard channel, mask, mix and unprocessed-channel knobs follow. The inputs are Source (with
the depth), an optional Mask and an optional Kernel. An Image kernel is read as Defocus reads it
and posts the same warnings.

Departures from Nuke's ZDefocus:

- The Mask input is kept. `size` is the diameter of the bokeh infinitely far behind the depth of
  field in Manual mode; `maxSize` defaults to 50 and is at most 500.
- Lens mode has a `sceneUnit` knob, so that the depth channel's unit compares with the focal
  length.
- `layers` is 2 to 64 slices, each normalised by its coverage, always laid back to front.
- Highlight boost is new, and the Focal Plane output marks the regions in front of, in and behind
  the focus.
- No deep input, and no cat-eye, chromatic aberration or anamorphic bokeh (M133 - ZDefocus Lens
  Aberrations).

GPU: the Focal Plane output, and Disc and Blades kernels whose largest slice reaches at most 256
pixels, run on the GPU in float, reading the depth as an extra layer input of the pass (the
PassExecutor's GPU pass inputs carry extra layer inputs). A larger bokeh, an Image kernel and a
buffer beyond the kernels' 32-bit indices render on the CPU. The GPU holds the node's own CPU render
(`Tests/Native/NativeZDefocusGpu_Test.cpp`).

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| ZDefocus (ZDefocus) | GPU against CPU | `ParityTolerance::make(1e-4, 1e-3)` | 1e-4 absolute, 1e-3 relative; the depth and the slices' weights are evaluated in float |

The CPU properties (all at the focal plane is bit-identical, a constant depth matches Defocus, a
sharp foreground does not halo, the focal plane output, a stopped-down lens is sharper, the
disconnected Kernel, region independence and the golden) are in
`Tests/Native/NativeZDefocus_Test.cpp`.
