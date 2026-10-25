# ICornerPin

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| ICornerPin | measured | `kWarpTolerance` (1e-4, CPU) | no OFX reference; a white or disconnected Map and a constant Map of 0.5 against CornerPin at Amount 1 and 0.5; a black Map within 1e-6 of the source |
| ICornerPin | resampling | `kWarpTolerance` = `ParityTolerance::resampling()` (GPU) | the GPU against the CPU, with and without Black Outside |

The variable CornerPin: CornerPin's pin, from the "from" points to the "to" points, applied per output pixel by an
amount the Map input gives. CornerPin (`CornerPin.reg.inc`) is its base node in the I-variant registry
(`ICornerPin.reg.inc`); every knob they share has the same name.

- **Map.** The Map input, from `MapInput` (`MapInput.h`), is an aux input: the host does not blend it, and the
  standard Mask and Mix still limit the result. Map Channel picks one channel of any of its layers (alpha by
  default) and Invert Map inverts it; the value is clamped to [0, 1] and remapped by Map Gamma and Map Response
  exactly as in IBlur (see `Engine/Nodes/Filter/docs/IBlur.md`). The Map is sampled in output space, at the pixel
  being written, not where that pixel comes from in the source. Outside the Map's image the raw value is 0.
- **Interpolation.** Each output pixel samples the source once, through the pin at its Map value m: the "to" points
  blended from the "from" points by m, as CornerPin's Amount blends them, and the homography solved at that blend.
  The extra matrix applies everywhere. The matrix comes from a `TransformInterp::MatrixCurve`: exact homographies at
  evenly spaced knots of m (0, 1/2 and 1 among them), joined by cubic Hermite interpolation. The pin is
  interpolated, never the images of two pins, so a soft Map bends the image instead of mixing two copies.
- **Tearing.** A Map edge between black and white tears the image along it when the pin moves the image far: the
  pixels on either side sample the source far apart. Soften the Map to bend the image instead.
- **Filtering.** The filter footprint is the Jacobian of the sample point, including the change of the homography
  with the Map's gradient, so a steep Map does not alias. The Map is read one pixel past the render window for it.
- **No Map.** With the Map disconnected it reads 1: the whole pin applies everywhere, as in CornerPin, and the node
  shows a warning badge.
- **Concatenation.** ICornerPin is a per-pixel warp and never offers a matrix to concatenate: it ends a chain of
  transforms, so a transform above it and one below it each resample the image.
- **Region.** As ITransform: the source's region pushed forward through the pins at every Map value the render may
  see, grown by a pixel with Black Outside, and unioned with the source's under a mask or a mix.
- **Motion blur.** Motion blur averages the warp over the shutter with the Monte Carlo sampling of
  `Resampler::resampleRow()`. It renders on the CPU only.
- **Knobs.** CornerPin's knobs except Amount, which the Map replaces, the Map knobs (Map Channel, Invert Map, Map
  Response, Map Gamma) and the standard channel, mask and mix knobs.

Tests: `Tests/Native/NativeIWarp_Test.cpp`. A black Map is the source; a white Map and no Map render as CornerPin; a
constant Map of 0.5 renders as CornerPin at Amount 0.5; a disconnected Map warns. The goldens (`GoldenICornerPin`:
default, impulse, and mask and mix) are compared bit for bit.

GPU: `iwarp.slang` evaluates the same matrix curve from the same knots and reads the Map as an extra input, remapping
it with `mapremap.slang` as the CPU does. Float images only, with or without a mask, never with motion blur
(`Tests/Native/NativeIWarpGpu_Test.cpp`).

Cost seed: pending measurement; there is no `ICornerPin.costseed.inc` yet.
