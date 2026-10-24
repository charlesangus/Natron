# RawTemperature

Tolerance: none recorded.

Port of the raw part of darktable's white balance module (`src/iop/temperature.c`, darktable
`release-5.6.2`). Its colour helpers are ported into `RawColorMath`. The CIE tables, the
blackbody and daylight spectra, and Little CMS's `cmsWhitePointFromTemp()` are included.

- Raw in, raw out, at the prepared stage. Any other stage is a persistent error.
- **Mode**, As Shot to Reference by default, darktable's default under its default
  `scene-referred (sigmoid)` workflow (`reload_defaults()` sets `DT_IOP_TEMP_D65_LATE` when
  `dt_is_scene_referred()`). darktable's `none` and display-referred workflows default to as shot;
  Natron has no workflow preference, so every RawTemperature, in Develop RAW or not, starts at As
  Shot to Reference.
  - **As Shot** takes `raw/wb_coeffs`, normalised to green 1. Without usable values it falls
    back as darktable's `_find_coeffs()` does: on the Camera Reference multipliers of the colour
    matrix, then on the camera's first preset normalised to green 1, then on (2, 1, 1.5).
  - **Camera Reference** makes D65 neutral through the inverse of `raw/cam_to_xyz`, as darktable's
    `_calculate_bogus_daylight_wb()` does. Without a matrix it takes the camera's daylight preset
    without tuning, as darktable's `reload_defaults()` does, then (2, 1, 1.5).
  - **Custom** uses the **Red**, **Green** and **Blue** knobs.
  - **Camera Preset** uses the camera's white balance setting named by **Preset**, at the
    **Finetune** tuning. A setting the source's camera does not have gives the As Shot multipliers.
  - **From Image Area** uses the **Red**, **Green** and **Blue** knobs as **Pick** last set them.
    Choosing the mode picks, as darktable's menu switches its picker on.
  - **As Shot to Reference** applies the As Shot multipliers and sets `raw/wb_late_correction` to
    the Camera Reference multipliers divided by the As Shot ones, per channel, as darktable's
    `_publish_chroma()` sets `late_correction`. RawHighlights and RawDemosaic pass it on;
    RawInputColor multiplies by it before its matrix. Any other mode removes the key.
    This mode white-balances the image only together with a chromatic adaptation after
    RawInputColor: RawInputColor ends at the camera's D65 reference, and the illuminant the photo
    was lit by is left for RawColorCalibration (which Develop RAW puts last) to adapt. Without a
    CAT the image keeps the scene's cast. RawColorCalibration warns when the white balance and its
    CAT disagree: "white balance applied twice" when it adapts after a white balance other than
    the reference (As Shot, say), as darktable's `_set_trouble_messages()` does.
- **Spot Bottom Left** and **Spot Size** are the area Pick reads, drawn as a rectangle in the
  viewer. While untouched they follow darktable's default picker box, 2% to 98% of the source's
  format.
- **Pick** reads the source over the area at full resolution and averages it per colour of the CFA
  over that colour's photosites (darktable's `_color_picker_bayer()` and
  `_color_picker_xtrans()`), per channel on linear data, and the one channel of a monochrome image
  as grey. As darktable's `color_picker_apply()` does, each multiplier is the reciprocal of its
  mean relative to green's, clamped to [0, 8], a mean at or under 0.001 counting as 1, and green
  is 1. It stores them in the knobs, sets the temperature and tint from them, and sets the mode to
  From Image Area, in one undo step.
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
  them. RawHighlights and RawDemosaic read both. In every mode it also records the Camera
  Reference multipliers, darktable's `D65coeffs`, in `raw/wb_d65_coeffs`.
- Differences from darktable in From Image Area:
  - darktable re-picks whenever its picker box moves while the picker is on; here the area is read
    only when Pick is pressed or the mode is chosen, and the result is stored.
  - darktable's default box is 2% to 98% of the final, cropped and distorted preview, transformed
    back to the module; here it is 2% to 98% of the source's format.
  - darktable accumulates the sums in float; here in double, then the mean is rounded to float.
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
`Tests/Native/NativeRawTemperatureSpot_Test.cpp` picks Bayer, X-Trans and linear mosaics with known
per-colour means. `Tests/Native/NativeRawLateCorrection_Test.cpp` checks As Shot to Reference
through RawHighlights, RawDemosaic and RawInputColor against Camera Reference, and
`raw/wb_d65_coeffs` in every mode. `Tests/Native/RawReferenceSceneDefault_Test.cpp` compares the
Develop RAW chain at its defaults, As Shot to Reference followed by RawColorCalibration, with
darktable's scene-referred default render of the CR2 and RAF crops.

GPU: `rawtemperature_wrapped` (`Engine/Gpu/Kernels/rawprepare.slang`), the op's `gpu()`, with the
CFA layout and the three multipliers in `RawTemperatureGpuParams`. One product per channel, as on
the CPU: bound `ParityTolerance::arithmetic()`. GPU tests: `Tests/Native/NativeRawPrepTempGpu_Test.cpp`,
every mode on the Bayer, X-Trans and linear fixtures.

## darktable controls

Generated by `python3 -I tools/raw/parity-audit.py --write-docs` from
`tools/raw/darktable-parity.json`: every control of darktable's `temperature` and where it is here.

| darktable item | Natron knob/option | darktable default | Natron default | status |
| --- | --- | --- | --- | --- |
| param red | red (shown from the image in every mode but custom) | from the image (as shot) | from the image | mapped |
| param green | green (shown from the image in every mode but custom) | from the image (as shot) | from the image | mapped |
| param blue | blue (shown from the image in every mode but custom) | from the image (as shot) | from the image | mapped |
| param various | N/A: 4-colour sensor |  |  | N/A |
| param preset | mode | DT_IOP_TEMP_D65_LATE (scene-referred, temperature.c:1518) | asShotToReference | mapped |
| gui temperature slider | temperature |  |  | mapped |
| gui tint slider | tint |  |  | mapped |
| gui as shot button | mode=asShot |  |  | mapped |
| gui user modified button | mode=custom |  |  | mapped |
| gui camera reference (D65) button | mode=cameraReference |  |  | mapped |
| gui D65 late button | mode=asShotToReference |  |  | mapped |
| gui camera white balance presets list | preset, mode=cameraPreset |  |  | mapped |
| gui finetune | finetune | 0 | 0 | mapped |
| gui spot area | spotBottomLeft, spotSize |  |  | mapped |
| gui spot picker | pick, mode=fromImageArea |  |  | mapped |
| | mode=cameraPreset | | | EXTRA: darktable selects a camera preset from its list and records user mode; here the mode says the preset knob applies |
