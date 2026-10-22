# RawTemperature

Tolerance: none recorded.

Port of the raw part of darktable's white balance module (`src/iop/temperature.c`, darktable
`release-5.6.2`). Its colour helpers are ported into `RawColorMath`. The CIE tables, the
blackbody and daylight spectra, and Little CMS's `cmsWhitePointFromTemp()` are included.

- Raw in, raw out, at the prepared stage. Any other stage is a persistent error.
- **Mode**:
  - **As Shot** takes `raw/wb_coeffs`, normalised to green 1. Without usable values it falls
    back to Camera Reference.
  - **Camera Reference** makes D65 neutral through the inverse of `raw/cam_to_xyz`, as darktable's
    `_calculate_bogus_daylight_wb()` does. Without a matrix it falls back to (2, 1, 1.5).
  - **Custom** uses the **Red**, **Green** and **Blue** knobs.
- Editing **Temperature** or **Tint** sets the three multipliers through the camera matrix (sRGB
  when there is none) and switches the mode to Custom. Editing a multiplier sets the temperature
  and tint back, as darktable's sliders do. Choosing another mode shows its multipliers in the
  knobs.
- Multipliers apply per CFA colour on a mosaic and per channel on three-channel linear data. A
  one-channel image without a CFA is left alone.
- The output records the multipliers in `raw/wb_applied` and multiplies `raw/processed_maximum` by
  them. RawHighlights and RawDemosaic read both.
- darktable's "as shot to reference" mode and its presets database are not ported.
- It renders at full resolution only.

Tests: `Tests/Native/NativeRawTemperature_Test.cpp`, on synthetic mosaics. No real camera files yet.
