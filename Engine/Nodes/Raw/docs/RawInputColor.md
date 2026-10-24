# RawInputColor

Tolerance: none recorded.

Port of the matrix path of darktable's input colour module (`src/iop/colorin.c`, the matrix
profiles of `src/common/colorspaces.c` and the tables of `src/common/colormatrices.c`, darktable
`release-5.6.2`). Its ICC path is not ported.

- Image in, image out, a Point op on the RGB channels. Alpha passes through.
- **Input Profile** (`inputProfile`), in darktable's order. A project saves the option's id, so
  options added later (T16's generic profiles) do not move a saved choice.
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
  - **Linear Rec709 RGB** (`linearRec709`) treats camera RGB as linear Rec. 709.
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
  columns. darktable skips it for a Lab input profile, which this node does not offer. The output
  then records `raw/wb_d65_coeffs` as `raw/wb_applied` and multiplies `raw/processed_maximum` by
  the correction, as darktable's `process()` does; the key itself is left for later nodes.

Tests: `Tests/Native/NativeRawInputColor_Test.cpp`, on a synthetic grey card and patches through
RawDemosaic. `Tests/Native/NativeRawInputColorProfiles_Test.cpp` checks the default on the CR2
and linear DNG fixtures, the table matrices against a hand computation from their entries, the
fallbacks, and the late correction with a table profile. `Tests/Native/NativeRawLateCorrection_Test.cpp`
checks the late correction through RawTemperature. A viewer colour check on real raw files comes once real files exist.

GPU: `rawinputcolor_wrapped` (`Engine/Gpu/Kernels/rawinputcolor.slang`), the op's `gpu()`, with
the baked camera to working space matrix, the OCIO matrix and the late correction included, in
`RawInputColorGpuParams`; the kernel is unchanged by the late correction.
The products are summed in the CPU's order without contraction: bound
`ParityTolerance::arithmetic()`. GPU tests: `Tests/Native/NativeRawChainGpu_Test.cpp`, which also
renders the whole Develop chain with RCD as one GPU region, one upload and one download.
