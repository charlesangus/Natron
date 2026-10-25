# Colorspace

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| Colorspace | transcendental | `ParityTolerance::transcendental()` | the cube root, arctangent and sine run as fixed polynomial and Newton steps on both sides, so only rounding differs; Vulkan division is within 2.5 ulp |
| Colorspace, from xyY | transcendental, plus an absolute bound | `toleranceFor()` | x·Y/y magnifies the rounding of the division by Y/y before the matrix to RGB cancels terms, so 1e-4 absolute |

Round trips through every model hold to 1e-5 relative in ACEScg and in Linear Rec.709
(`Tests/Native/NativeColorspace_Test.cpp`).

The node replaces the OpenFX RGBToHSV, HSVToRGB, RGBToHSL, HSLToRGB, RGBToHSI, HSIToRGB,
RGBToYCbCr601/709, YCbCrToRGB601/709, RGBToYPbPr601/709, YPbPrToRGB601/709, RGBToYUV601/709,
YUVToRGB601/709, RGB709ToXYZ, XYZToRGB709, RGB709ToLab, LabToRGB709, XYZToLab, LabToXYZ,
XYZToxyY and xyYToXYZ plug-ins with one In and one Out choice.

- **Working space, not Rec. 709.** XYZ, xyY and L\*a\*b\* use the project's working space
  primaries and white (ACEScg when the project offers none). The OpenFX plug-ins always used
  the Rec. 709 primaries.
- **L\*a\*b\* is unscaled.** L\* runs from 0 to 100; the OpenFX plug-ins divided by 100.
- **No transfer function.** The Y'CbCr, YPbPr and YUV models apply the luma coefficients to
  the linear RGB as given. Apply an OETF first when encoded values are wanted.
- **Hue in turns.** HSV, HSL and HSI hue runs from 0 to 1.
- **Works on the values as they are.** The node does not unpremultiply or premultiply: the
  RGB it receives is converted as stored and alpha is left unchanged, and there is no
  "(Un)premult by" control. To convert unpremultiplied colour, put the node between an
  Unpremult and a Premult. The OpenFX plug-ins unpremultiplied an RGB input and premultiplied
  an RGB output when asked to.
- **Defaults to RGB to RGB,** an identity.
- **No YUV for Rec. 2020,** as before: analog YUV has no Rec. 2020 definition.

The kernel follows the CPU statement for statement with `precise` arithmetic
(`Tests/Native/NativeColorspaceGpu_Test.cpp`).
