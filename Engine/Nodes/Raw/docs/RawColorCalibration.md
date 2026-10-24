# RawColorCalibration

Tolerance: the darktable comparisons use the class of the file's default demosaic, RCD for the
CR2 and Markesteijn for the RAF (`Tests/Native/NativeRawColorCalibration_Test.cpp`).

Port of darktable's color calibration module (`src/iop/channelmixerrgb.c`, with
`src/common/illuminants.h`, `chromatic_adaptation.h` and `colorspaces_inline_conversions.h`,
darktable `release-5.6.2`): the chromatic adaptation, the R, G and B mix, gamut compression,
colorfulness, brightness and gray. The pixel maths are in `RawColorCalibrationMath`
(`makeKernel()` for `commit_params()`, `processPixel()` for `_loop_switch()`), plain data a GPU
kernel can take as it is. Its colour checker, its white balance detection (AI) modes and the
spot picker are not ported, nor the camera illuminant yet.

- Image in, image out, a Point op on the RGB channels. Alpha passes through.
- Works on the project's working space. Working RGB goes to XYZ through the project's matrix to XYZ
  (D65) and a Bradford adaptation to D50, as darktable's linear working profile is relative to
  the D50 connection space; linear Rec. 709 when the project has no matrix. darktable's default
  working space is linear Rec2020: only adaptation none, which mixes in the working RGB, depends
  on which one.
- A project saves choice ids, so options added later do not move a saved choice.

## Knobs

| darktable param | Knob | Notes |
| --- | --- | --- |
| `adaptation` (CAT16) | **Adaptation** (`adaptation`): `linearBradford`, `cat16`, `nonLinearBradford`, `xyz`, `none` | CAT tab |
| | **CCT** label (`approxCct`) | `_update_approx_cct()`'s text; not saved |
| `illuminant` (D) | **Illuminant** (`illuminant`): `pipe`, `a`, `d`, `e`, `f`, `led`, `blackBody`, `custom` | no camera illuminant yet |
| `illum_fluo` (F3) | **F Source** (`illumFluo`): `f1` to `f12` | shown for F |
| `illum_led` (B5) | **LED Source** (`illumLed`): `b1` to `b5`, `bh1`, `rgb1`, `v1`, `v2` | shown for LED |
| `temperature` (5003) | **Temperature** (`temperature`), 1667 to 25000 K, slider 3000 to 7000 | shown for D and black body |
| | **Hue** (`illumHue`), 0 to 360, and **Chroma** (`illumChroma`), 0 to 300, slider 0 to 100 | darktable's `illum_x`/`illum_y` sliders: the Luv hue and chroma of x, y; not saved; shown for custom |
| `x`, `y` (0.333) | `illumX`, `illumY`, hidden | what a project saves for the custom illuminant |
| `gamut` (1) | **Gamut Compression** (`gamut`), 0 to 12, slider 0 to 4 | |
| `clip` (on) | **Clip Negative RGB from Gamut** (`clip`) | |
| `red`, `green`, `blue` (identity) | **Input R/G/B** (`redR` ... `blueB`), -2 to 2, on the R, G and B tabs | |
| `normalize_R/G/B` (off) | **Normalize Channels** (`redNormalize`, `greenNormalize`, `blueNormalize`) | |
| `saturation` (0) | **Input R/G/B** (`saturationR` ...), slider -1 to 1, Colorfulness tab | |
| `normalize_sat` (off) | **Normalize Channels** (`saturationNormalize`) | |
| `version` (v3) | **Saturation Algorithm** (`saturationVersion`): `v1`, `v2`, `v3` | as darktable, shown only when a project saved v1 or v2 |
| `lightness` (0) | **Input R/G/B** (`lightnessR` ...), slider -1 to 1, Brightness tab | |
| `normalize_light` (off) | **Normalize Channels** (`lightnessNormalize`) | |
| `grey` (0) | **Input R/G/B** (`greyR` ...), slider 0 to 1, Gray tab | |
| `normalize_grey` (off) | **Normalize Channels** (`greyNormalize`) | |

## Interface behaviour

- `_update_illuminants()`: adaptation none hides the illuminant and its controls; each illuminant
  shows only its own controls.
- `gui_changed()`: choosing D or black body restores the last temperature used with it; editing
  the illuminant, a source or the temperature sets x and y from it, and, for an illuminant that
  is neither D nor black body, the temperature to the nearest CCT. `_illum_xy_callback()`:
  editing hue or chroma sets x, y and the temperature. The CCT label follows each of them.
- A gray mix whose weights sum to 0 with Normalize Channels on warns "color calibration: the sum
  of the gray channel parameters is zero, normalization will be disabled.", as darktable does.

## Differences from darktable

- With adaptation none, an identity mix, no gamut compression, clipping off and no colorfulness,
  brightness or gray, the node passes its input through. darktable still runs the pixel loop,
  whose xyY round trip only rounds.
- Animation is off for the choices, toggles and the illuminant knobs, which sync each other; the
  sliders animate.
- As darktable leaves it, the gray output stays in the adaptation's LMS or XYZ space.
