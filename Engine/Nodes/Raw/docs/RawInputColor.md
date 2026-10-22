# RawInputColor

Tolerance: none recorded.

Port of the matrix path of darktable's input colour module (`src/iop/colorin.c` and
`dt_colorspaces_create_xyzmatrix_profile()` in `src/common/colorspaces.c`, darktable
`release-5.6.2`). Its ICC path is not ported.

- Image in, image out, a Point op on the RGB channels. Alpha passes through.
- **Camera Matrix**:
  - **From File** reads `raw/cam_to_xyz` from the source's metadata, which RawDemosaic keeps. As
    darktable builds its input profile, the matrix becomes the RGB space whose primaries are the
    chromaticities of its columns and whose white, camera (1, 1, 1), is D65. A white-balanced
    neutral is therefore D65 white.
  - **Standard (Linear Rec. 709)** treats camera RGB as linear Rec. 709. It is also the fallback
    when the file has no matrix.
  - **Custom** takes the three rows below as the camera to XYZ (D65) matrix, as given.
- XYZ goes to the project's working space through the inverse of
  `ProjectColorManagement::getWorkingToXYZ()`. That matrix comes from the OpenColorIO config's
  `cie_xyz_d65_interchange` role, so no primaries are hard-coded. A working space with no such
  matrix, a log space for example, is treated as linear Rec. 709.
- The working space is part of every node's hash, so changing it renders again.

Tests: `Tests/Native/NativeRawInputColor_Test.cpp`, on a synthetic grey card and patches through
RawDemosaic. A viewer colour check on real raw files comes once real files exist.
