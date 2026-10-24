# RawInputColor

Tolerance: none recorded.

Port of the matrix path of darktable's input colour module (`src/iop/colorin.c`, the matrix
profiles of `src/common/colorspaces.c` and the tables of `src/common/colormatrices.c`, darktable
`release-5.6.2`), with its built-in generic profiles, gamut clipping and working profile. Its ICC
path (embedded and file profiles, the rendering intent) is not ported, nor the legacy
`blue_mapping` parameter, which darktable no longer shows.

- Image in, image out, a Point op on the RGB channels. Alpha passes through.
- **Input Profile** (`inputProfile`), in darktable's order. A project saves the option's id, so
  options added later do not move a saved choice.
  - **Embedded Matrix** (`embeddedMatrix`): `raw/dng_d65_matrix`, the XYZ to camera matrix a
    DNG's colour tags give for D65, inverted (`dt_colorspaces_create_xyzimatrix_profile()`).
  - **Standard Color Matrix** (`standardMatrix`): `raw/cam_to_xyz`, the decoder's camera matrix,
    which RawDemosaic keeps.
  - For both, as darktable builds its input profile, the matrix becomes the RGB space whose
    primaries are the chromaticities of its columns and whose white, camera (1, 1, 1), is D65. A
    white-balanced neutral is therefore D65 white.
  - **Enhanced Color Matrix** (`enhancedMatrix`), **Vendor Color Matrix** (`vendorMatrix`),
    **Alternate Color Matrix** (`alternateMatrix`): darktable's `dt_profiled_colormatrices`,
    `dt_vendor_colormatrices` and `dt_alternate_colormatrices` (`RawColorMatrices`), looked up by
    `raw/make` + " " + `raw/model` (darktable's `camera_makermodel`); the enhanced table ignores
    case, the other two do not, as darktable's lookups do. An entry gives the XYZ of three
    primaries and a white. As `cmsCreateRGBProfile()` does, the primaries are scaled so camera
    white is the entry's white at Y = 1, then adapted to Little CMS's D50 with its linear
    Bradford; darktable's working profile is adapted the same way, so the matrix is taken from
    D50 to D65 with Bradford again (`RawColorMath::profileCameraToXyz()`). Little CMS is not
    linked: the Bradford maths is ported.
  - **Generic profiles** (`RawColorProfiles`), darktable's built-in input profiles in its order:
    sRGB (`srgb`), Adobe RGB (compatible) (`adobeRgb`), Linear Rec709 RGB (`linearRec709`),
    Rec709 RGB (`rec709`), Linear Rec2020 RGB (`linearRec2020`), PQ Rec2020 RGB (`pqRec2020`),
    HLG Rec2020 RGB (`hlgRec2020`), PQ P3 RGB (`pqP3`), HLG P3 RGB (`hlgP3`), Display P3 RGB
    (`displayP3`), Linear ProPhoto RGB (`linearProPhoto`), Linear XYZ (`linearXyz`), Lab (`lab`),
    Linear Infrared BGR (`linearInfraredBgr`) and BRG (for testing) (`brg`).
    - Matrices from the primaries and white as `cmsCreateRGBProfile()` builds them, through D50
      and back to D65 as the table matrices are. Linear Rec709 keeps
      `RawColorMath::srgbToXyz()`, within 1e-4 of the profile-derived one. Linear XYZ is XYZ
      relative to the D50 connection space, adapted to D65 with Bradford; darktable reaches the
      same through a Little CMS transform.
    - Tone curves: sRGB (and Display P3, BRG) and Rec709 are Little CMS parametric type 4, Adobe
      RGB gamma 2.19921875, PQ and HLG 4096-point tables clipped to 1, as `colorspaces.c` builds
      them. As colorin's `_apply_tone_curves()`, a curve is read from a 0x10000-entry table below
      1 (negatives read its first entry) and above 1 extrapolated with the power law
      `dt_iop_estimate_exp()` fits at 0.7, 0.8, 0.9 and 1, not clamped. The late correction
      multiplies before the curve.
    - **Lab**: L from 0 to 100, a and b unscaled, to XYZ D50 (`dt_Lab_to_XYZ()`), Bradford to D65,
      then the working space. As darktable, which disables colorin for it, it gets no late
      correction and no gamut clipping.
  - **Custom** (`custom`), Natron's, takes the three rows below as the camera to XYZ (D65)
    matrix, as given. Not in darktable.
- **Fallback** (colorin `commit_params()`): a table matrix the camera has not falls back to the
  embedded matrix (darktable tries the embedded ICC profile first, which Natron does not read),
  the embedded matrix to the standard matrix, the standard matrix to linear Rec709. The node
  warns "input profile: X not available for this camera. falling back to Y." while it does.
- **Override Input Profile** (`inputProfileOverride`), off by default. While it is off the choice
  is disabled and shows the file-derived default, which a render uses, as RawHighlights' clipping
  threshold does: darktable's `reload_defaults()`, the embedded matrix when the source has
  `raw/dng_d65_matrix`, the standard matrix when it has `raw/cam_to_xyz`, linear Rec709 for an
  image with neither (darktable's HDR case). The embedded ICC, 4Bayer, monochrome, sRGB/Adobe RGB
  and LDR branches before those have no counterpart in the source's metadata yet.
- XYZ goes to the project's working space through the inverse of
  `ProjectColorManagement::getWorkingToXYZ()`. That matrix comes from the OpenColorIO config's
  `cie_xyz_d65_interchange` role, so no primaries are hard-coded. A working space with no such
  matrix, a log space for example, is treated as linear Rec. 709.
- The working space is part of every node's hash, so changing it renders again.
- **Late correction**: when the source carries `raw/wb_late_correction` (RawTemperature in As Shot
  to Reference), camera RGB is multiplied by it, `D65coeffs / as_shot` per channel, before the
  matrix, as darktable's `process_cmatrix()` does. The product is folded into the matrix's
  columns, or for a non-linear profile applied before its tone curve. It is skipped for a Lab input
profile, as darktable's `process()` does, and so is the metadata below. The output
  then records `raw/wb_d65_coeffs` as `raw/wb_applied` and multiplies `raw/processed_maximum` by
  the correction, as darktable's `process()` does; the key itself is left for later nodes.
- **Gamut Clipping** (`gamutClipping`), colorin's `normalize`: Off (`off`, the default), sRGB
  (`srgb`), Adobe RGB (compatible) (`adobeRgb`), Linear Rec709 RGB (`linearRec709`), Linear Rec2020
  RGB (`linearRec2020`). As `process_cmatrix_proper()`, the input goes to the gamut's linear RGB
  (`nmatrix`), is clipped to [0, 1] (`dt_vector_clip()`), then goes on to the output space
  (`lmatrix`). Only the matrices are used, so sRGB and Linear Rec709 clip alike.
- **Working Profile** (`workingProfile`): Project Working Space (`project`, the default), then
  darktable's working profiles sRGB (web-safe), Adobe RGB (compatible), Linear Rec709 RGB,
  Rec709 RGB, Linear Rec2020 RGB, PQ Rec2020 RGB, HLG Rec2020 RGB, PQ P3 RGB, HLG P3 RGB, Display
  P3 RGB and Linear ProPhoto RGB, with the generic profiles' ids. Another choice writes that
  profile instead of the project's working space: its matrix from XYZ, then its tone curve
  reversed (Little CMS's analytic inverse for the parametric curves, the exact inverse of the
  4096-point table for PQ and HLG, where darktable reverses it with `cmsReverseToneCurveEx()`),
  tabulated and extrapolated above 1 as the input curves are. The node says "output: X, not the
  project's working space." while it does. darktable's default is Linear Rec2020; the project's
  working space is the default here so the node fits the project's colour management.

Tests: `Tests/Native/NativeRawInputColor_Test.cpp`, on a synthetic grey card and patches through
RawDemosaic. `Tests/Native/NativeRawInputColorProfiles_Test.cpp` checks the default on the CR2
and linear DNG fixtures, the table matrices against a hand computation from their entries, the
fallbacks, and the late correction with a table profile. `Tests/Native/NativeRawLateCorrection_Test.cpp`
checks the late correction through RawTemperature. `Tests/Native/NativeRawInputColorGeneric_Test.cpp`
round-trips every generic profile against its standard's curve and primaries
(`Tests/Native/RawColorReference.h`), checks the extrapolation above 1, Linear XYZ, Lab, and that
Lab skips the late correction. `Tests/Native/NativeRawInputColorClipping_Test.cpp` checks that each
gamut clips what is outside it and leaves the rest, and that a working profile converts the
output by the expected matrix and curve and posts its message. A viewer colour check on real raw files comes once real files exist.

GPU: `rawinputcolor_wrapped` (`Engine/Gpu/Kernels/rawinputcolor.slang`), the op's `gpu()`, with
the baked camera to working space matrix, the OCIO matrix and the late correction included, in
`RawInputColorGpuParams`; the kernel is unchanged by the late correction. A non-linear or Lab
input profile, gamut clipping or a working profile other than the project's has no GPU path: the
op's `gpu()` returns none and the region renders on the CPU.
The products are summed in the CPU's order without contraction: bound
`ParityTolerance::arithmetic()`. GPU tests: `Tests/Native/NativeRawChainGpu_Test.cpp`, which also
renders the whole Develop chain with RCD as one GPU region, one upload and one download.
