# RawTemperature

Tolerance: none recorded.

Port of the raw part of darktable's white balance module (`src/iop/temperature.c`, darktable
`release-5.6.2`). Its colour helpers are ported into `RawColorMath`. The CIE tables, the
blackbody and daylight spectra, and Little CMS's `cmsWhitePointFromTemp()` are included.

- Raw in, raw out, at the prepared stage. Any other stage is a persistent error.
- **Mode**:
  - **As Shot** takes `raw/wb_coeffs`, normalised to green 1. Without usable values it falls
    back as darktable's `_find_coeffs()` does: on the Camera Reference multipliers of the colour
    matrix, then on the camera's first preset normalised to green 1, then on (2, 1, 1.5).
  - **Camera Reference** makes D65 neutral through the inverse of `raw/cam_to_xyz`, as darktable's
    `_calculate_bogus_daylight_wb()` does. Without a matrix it takes the camera's daylight preset
    without tuning, as darktable's `reload_defaults()` does, then (2, 1, 1.5).
  - **Custom** uses the **Red**, **Green** and **Blue** knobs.
  - **Camera Preset** uses the camera's white balance setting named by **Preset**, at the
    **Finetune** tuning. A setting the source's camera does not have gives the As Shot multipliers.
- **Preset** lists the settings of darktable's presets database (`data/wb_presets.json`, installed
  in `Resources/darktable/`) for the source's `raw/make` and `raw/model`, rawspeed's canonical
  names, which darktable matches exactly. Names are lower-cased and at most 50 are listed, as in
  darktable. The menu is refilled when the source changes; the project saves the setting by name.
  Choosing a setting, or a tuning, switches the mode to Camera Preset.
- **Finetune** shows only for a setting with tunings, and its range is the lowest to the highest.
  A tuning the camera gives is used as is; one between two is interpolated as darktable's
  `dt_wb_preset_interpolate()` does, linearly in the reciprocal multipliers. A saved value outside
  the setting's range is clamped to it.
- Editing **Temperature** or **Tint** sets the three multipliers through the camera matrix (sRGB
  when there is none) and switches the mode to Custom. Editing a multiplier sets the temperature
  and tint back, as darktable's sliders do. Choosing another mode shows its multipliers in the
  knobs.
- Multipliers apply per CFA colour on a mosaic and per channel on three-channel linear data. A
  one-channel image without a CFA is left alone.
- The output records the multipliers in `raw/wb_applied` and multiplies `raw/processed_maximum` by
  them. RawHighlights and RawDemosaic read both.
- darktable's "as shot to reference" and "from image area" modes are not ported.
- Differences from darktable in the presets:
  - darktable's `reload_defaults()` looks for "Daylight" and "DirectSunlight", which the
    lower-cased names never equal, so it never finds the daylight preset. This looks for
    "daylight" and "direct sunlight", as it means to.
  - darktable's scan for a setting's tunings stops only at another name, so it can run into the
    next camera's rows or past the end of the database; this one stops at the camera's last row.
  - Where darktable finds no tuning to use, it keeps the multipliers it had. Clamping makes that
    unreachable here, short of malformed data, which gets the setting's row without tuning.
  - darktable exits when the presets file is malformed. Here the database is empty, so the menu
    shows nothing and the fallbacks skip the preset steps.
- It renders at full resolution only.

Tests: `Tests/Native/NativeRawTemperature_Test.cpp`, on synthetic mosaics. No real camera files yet.
`Tests/Native/NativeRawTemperaturePresets_Test.cpp` checks the presets against `wb_presets.json`:
the Canon EOS 5D Mark III daylight rows, a between-tunings interpolation, the Fujifilm X-Pro2 menu,
a camera without presets, and the fallbacks without as-shot multipliers or a matrix.

GPU: `rawtemperature_wrapped` (`Engine/Gpu/Kernels/rawprepare.slang`), the op's `gpu()`, with the
CFA layout and the three multipliers in `RawTemperatureGpuParams`. One product per channel, as on
the CPU: bound `ParityTolerance::arithmetic()`. GPU tests: `Tests/Native/NativeRawPrepTempGpu_Test.cpp`,
every mode on the Bayer, X-Trans and linear fixtures.
