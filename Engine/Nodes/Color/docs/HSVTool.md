# HSVTool

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| HSVTool | arithmetic | `ParityTolerance::arithmetic()` | the hue, the coefficients and the conversion back run statement for statement on both sides; Vulkan division is within 2.5 ulp |

The node replaces OpenFX's HSVTool (`net.sf.openfx.HSVToolPlugin`) with every control, under
the same names and defaults, except where listed below
(`Tests/Native/NativeHSVTool_Test.cpp`).

- **Scene-linear.** Saturation is the chroma over the largest of R, G and B, 0 when that is not
  positive, and neither saturation nor brightness is clamped, so HDR and negative values are
  selected and adjusted. A colour whose largest channel is not positive keeps its chroma when
  adjusted.
- **No Upper Limit** (`brightnessRangeOpen`, new, on by default): the brightness range is open
  above, so values over 1 are selected. The upper end still sets the centre for the gain.
  Setting Src Color or Set Src from Rectangle turns it off, as both measure a closed range.
- **Clamp Black is off by default.** OpenFX defaulted it on.
- **Alpha follows the Channels knob.** Output Alpha writes the coefficients only when the
  Channels knob processes A (it does not by default), so the default is an identity. Alpha is
  blended by mask and mix like the other channels, and the output is not forced to RGBA or to
  unpremultiplied.
- **Hue in degrees on the knobs, turns inside.** The knobs keep OpenFX's degrees.
- **No colour picker overlay or HiDPI toggle.** The analysis rectangle uses the host's
  rectangle interact; the picker and the overlay draw of the selected range are gone.

The kernel follows the CPU statement for statement with `precise` arithmetic
(`Tests/Native/NativeHueGpu_Test.cpp`).
