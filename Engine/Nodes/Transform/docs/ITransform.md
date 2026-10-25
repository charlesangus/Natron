# ITransform

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| ITransform | measured | `kWarpTolerance` (1e-4, CPU) | no OFX reference; a white or disconnected Map and a constant Map of 0.5 against Transform at Amount 1 and 0.5; a black Map within 1e-6 of the source |
| ITransform | resampling | `kWarpTolerance` = `ParityTolerance::resampling()` (GPU) | the GPU against the CPU, Map connected, disconnected and outside its image, masked and mixed |

The variable Transform: Transform's translate, rotate, scale, skew and centre, applied per output pixel by an
amount the Map input gives. Transform (`net.sf.openfx.TransformPlugin`) is its base node in the I-variant registry
(`ITransform.reg.inc`); every knob they share has the same name.

- **Map.** The Map input, from `MapInput` (`MapInput.h`), is an aux input: the host does not blend it, and the
  standard Mask and Mix still limit the result. Map Channel picks one channel of any of its layers (alpha by
  default) and Invert Map inverts it; the value is clamped to [0, 1] and remapped by Map Gamma and Map Response
  exactly as in IBlur (see `Engine/Nodes/Filter/docs/IBlur.md`). The Map is sampled in output space, at the pixel
  being written, not where that pixel comes from in the source. Outside the Map's image the raw value is 0.
- **Interpolation.** Each output pixel samples the source once, through the transform at its Map value m: the
  identity at 0 and Transform's matrix at 1, translate, rotate and skew linear in m and scale geometric from 1, the
  centre fixed, as Transform's Amount blends. The matrix comes from a `TransformInterp::MatrixCurve`: exact matrices
  at evenly spaced knots of m (0, 1/2 and 1 among them), joined by cubic Hermite interpolation. The transform is
  interpolated, never the images of two transforms, so a soft Map bends the image instead of mixing two copies.
- **Tearing.** A Map edge between black and white tears the image along it when the transform is large: the pixels
  on either side sample the source far apart. Soften the Map to bend the image instead.
- **Filtering.** The filter footprint is the Jacobian of the sample point, including the change of the matrix with
  the Map's gradient, so a steep Map does not alias. The Map is read one pixel past the render window for it.
- **No Map.** With the Map disconnected it reads 1: the whole transform applies everywhere, as in Transform, and the
  node shows a warning badge.
- **Concatenation.** ITransform is a per-pixel warp and never offers a matrix to concatenate: it ends a chain of
  transforms, so a Transform above it and one below it each resample the image.
- **Region.** The region of definition is the source's pushed forward through the transforms at every Map value the
  render may see (only the full transform with the Map disconnected), grown by a pixel with Black Outside, and
  unioned with the source's under a mask or a mix. The source is read over the window pulled back the same way.
- **Motion blur.** Motion blur averages the warp over the shutter, and Directional Blur over the transform from none
  to the pixel's, with the Monte Carlo sampling of `Resampler::resampleRow()`. It renders on the CPU only.
- **Knobs.** Transform's knobs except Amount, which the Map replaces, the Map knobs (Map Channel, Invert Map, Map
  Response, Map Gamma) and the standard channel, mask and mix knobs.

Tests: `Tests/Native/NativeIWarp_Test.cpp`. A black Map is the source; a white Map and no Map render as Transform;
a constant Map of 0.5 renders as Transform at Amount 0.5; a vertical Map ramp moving a stripe sideways bends it into
one stripe per row, never two faint copies; a disconnected Map warns; motion blur and directional blur with no Map
average within 1e-3 RMS of Transform's. The goldens (`GoldenITransform`: default, a Map remap of gamma 2 with the
Smooth response inverted, motion blur, and mask and mix) are compared bit for bit.

GPU: `iwarp.slang` evaluates the same matrix curve from the same knots and reads the Map as an extra input, remapping
it with `mapremap.slang` as the CPU does. Float images only, with or without a mask, never with motion or
directional blur (`Tests/Native/NativeIWarpGpu_Test.cpp`).

Cost seed: pending measurement; there is no `ITransform.costseed.inc` yet.
