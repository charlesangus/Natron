# ToneEqualizer

Tolerance: against darktable's export of the same linear Rec. 709 source, the whole frame, relative
to the reference's mean level: a mean of 1e-3 and a max of 1e-2, with no pixel over the max except
under Mask Quantization, which may put 100 of 49152 pixels over it
(`Tests/Native/NativeToneEqualizer_Test.cpp`, `MatchesDarktable`). GPU against CPU: 1e-3 relative
for Result and Mask; Mask Exposure Histogram Zones a mean and max of 1e-3 with 16 pixels over
(`Tests/Native/NativeToneEqualizerGpu_Test.cpp`).

Port of darktable's tone equalizer (`src/iop/toneequal.c` and `src/common/luminance_mask.h`,
darktable `release-5.6.2`), with the guided filter and EIGF of the edge-aware filter library.

- A luminance mask is estimated from each pixel's colour, optionally refined by an edge-aware
  filter so that it follows edges but not texture, and each pixel is multiplied by a correction
  read from a smooth curve of the mask's exposure. The nine band knobs, **Blacks** (-8 EV) to
  **Speculars** (0 EV), set the curve's gain at each whole EV; a least-squares fit of eight
  gaussians passes within about 0.02 EV of them. The correction is clamped to [-2, +2] EV.
- **Curve Smoothing** is darktable's slider: the gaussians' width is sqrt(2)^(1 + value). A curve
  the fit cannot follow gets darktable's message as a node warning ("The interpolation is
  unstable, decrease the curve smoothing.").
- **Luminance Estimator** and **Preserve Details** are darktable's lists. The averaged filters take
  the geometric mean of the filtered and unfiltered mask. **Smoothing Diameter** is a percentage
  of the frame's larger side, **Edges Refinement** the filters' feathering, **Filter Diffusion**
  their iterations, **Mask Quantization** a step in EV the filtered mask snaps to.
- **Mask Exposure Compensation** scales the estimate; **Mask Contrast Compensation** pivots it
  around -4 EV, and only with Guided Filter or EIGF, as in darktable. **Auto Mask Exposure
  Compensation** and **Auto Mask Contrast Compensation** read the current frame's mask and place
  its 5th and 95th percentiles as darktable's buttons do.
- **Output** shows the corrected image, the mask as darktable displays it, or the mask rounded to
  the bands (**Mask Exposure Histogram Zones**).
- **Preset** and **Apply Preset** write every control of one of darktable's thirteen built-in
  presets.
- Premultiplied input is divided by alpha first and multiplied back after; alpha and any channel
  past the third pass through.
- The mask and its filter cover the whole frame, so the node asks for its input's whole region of
  definition whatever the render window. A mask whose settings are unchanged is kept, so a band
  edit only re-applies the curve.
- With the node's overlay shown, hovering the viewer writes the mask exposure under the cursor, its
  band and the correction there into **Cursor Readout**, and draws them next to the cursor. Natron's
  viewer has no wheel hook for overlays, so darktable's scroll-over-image band edit is not ported.
- GPU: the estimate and the correction are Slang kernels (`Engine/Gpu/Kernels/toneequal.slang`;
  darktable has no OpenCL tone equalizer), the filters those of the edge-aware library. A frame
  whose mask or pixels exceed 32-bit indices renders on the CPU.

## darktable controls

Every field of `dt_iop_toneequalizer_params_t` (version 2), its enum values, and darktable's
GUI-only controls, and where each is here.

| darktable item | Natron knob/option | darktable default | Natron default | status |
| --- | --- | --- | --- | --- |
| param noise "blacks" | blacks | 0.0 | 0.0 | mapped |
| param ultra_deep_blacks "deep shadows" | deepShadows | 0.0 | 0.0 | mapped |
| param deep_blacks "shadows" | shadows | 0.0 | 0.0 | mapped |
| param blacks "light shadows" | lightShadows | 0.0 | 0.0 | mapped |
| param shadows "mid-tones" | midtones | 0.0 | 0.0 | mapped |
| param midtones "dark highlights" | darkHighlights | 0.0 | 0.0 | mapped |
| param highlights "highlights" | highlights | 0.0 | 0.0 | mapped |
| param whites "whites" | whites | 0.0 | 0.0 | mapped |
| param speculars "speculars" | speculars | 0.0 | 0.0 | mapped |
| param blending "smoothing diameter" | smoothingDiameter | 5.0 | 5.0 | mapped |
| param smoothing (GUI "curve smoothing") | curveSmoothing | sqrt(2), slider 0.0 | 0.0 | mapped: the slider's value, as darktable's GUI |
| param feathering "edges refinement/feathering" | edgesRefinement | 1.0 | 1.0 | mapped |
| param quantization "mask quantization" | maskQuantization | 0.0 | 0.0 | mapped |
| param contrast_boost "mask contrast compensation" | maskContrastCompensation | 0.0 | 0.0 | mapped |
| param exposure_boost "mask exposure compensation" | maskExposureCompensation | 0.0 | 0.0 | mapped |
| param details "preserve details" | preserveDetails | DT_TONEEQ_EIGF | eigf | mapped |
| param method "luminance estimator" | luminanceEstimator | DT_TONEEQ_NORM_2 | norm2 | mapped |
| param iterations "filter diffusion" | filterDiffusion | 1 | 1 | mapped |
| enum DT_TONEEQ_NONE "no" | preserveDetails=no |  |  | mapped |
| enum DT_TONEEQ_AVG_GUIDED "averaged guided filter" | preserveDetails=averagedGuidedFilter |  |  | mapped |
| enum DT_TONEEQ_GUIDED "guided filter" | preserveDetails=guidedFilter |  |  | mapped |
| enum DT_TONEEQ_AVG_EIGF "averaged EIGF" | preserveDetails=averagedEigf |  |  | mapped |
| enum DT_TONEEQ_EIGF "EIGF" | preserveDetails=eigf | default |  | mapped |
| enum DT_TONEEQ_MEAN "RGB average" | luminanceEstimator=mean |  |  | mapped |
| enum DT_TONEEQ_LIGHTNESS "HSL lightness" | luminanceEstimator=lightness |  |  | mapped |
| enum DT_TONEEQ_VALUE "HSV value / RGB max" | luminanceEstimator=value |  |  | mapped |
| enum DT_TONEEQ_NORM_1 "RGB sum" | luminanceEstimator=norm1 |  |  | mapped |
| enum DT_TONEEQ_NORM_2 "RGB euclidean norm" | luminanceEstimator=norm2 | default |  | mapped |
| enum DT_TONEEQ_NORM_POWER "RGB power norm" | luminanceEstimator=powerNorm |  |  | mapped |
| enum DT_TONEEQ_GEOMEAN "RGB geometric mean" | luminanceEstimator=geomean |  |  | mapped |
| gui "auto-adjust the average exposure" | autoMaskExposureCompensation |  |  | mapped |
| gui "auto-adjust the contrast" | autoMaskContrastCompensation |  |  | mapped |
| gui "display exposure mask" | output=mask |  |  | mapped |
| gui mask histogram and its zones | output=zones |  |  | mapped: the zones drawn on the image rather than beside the histogram |
| gui presets (13) | preset, applyPreset |  |  | mapped |
| gui cursor exposure readout | cursorReadout, overlay |  |  | mapped |
| gui scroll over the image to edit the band under the cursor | N/A: the viewer has no wheel hook for overlays |  |  | DEFERRED |
| gui curve graph drag | N/A: the band knobs edit the curve |  |  | N/A |
| | output=result | | | EXTRA: darktable's output when the mask display is off |
