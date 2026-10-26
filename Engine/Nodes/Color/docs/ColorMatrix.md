# ColorMatrix

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| ColorMatrix | arithmetic, plus an absolute bound | `kMatrixTolerance` | single-precision products summed in a fixed order; against a double reference the error follows the terms, not the result, so 5e-6 absolute covers cancellation |

- **Knobs.** `matrixSize` (label "Matrix") picks 3x3 (the default) or 4x4. `matrix` (label
  "Coefficients") is one 16-dimension double knob, rows R, G, B, A over columns r, g, b, a:
  dimension `row * 4 + column`, named `rr`, `rg`, ... `aa`, identity by default. The panel shows
  it as a grid of plain numeric fields headed R G B A across and down, with no slider; each field
  keeps its own animation, expression, link and right-click menu.
- **3x3.** The panel shows the top-left 3x3 block. R, G and B are `m0·R + m1·G + m2·B`, the
  alpha column unread, and alpha passes through untouched, unclamped, whatever the stored alpha
  row and column hold. Switching size never changes a stored coefficient, so going back to 4x4
  restores the alpha terms. The node is an identity when the 3x3 block is and no clamp is on.
- **4x4.** Each output channel is its row dotted with the source RGBA,
  `out = m0·R + m1·G + m2·B + m3·A`, summed in that order.
- **Clamps.** Clamp Black raises a negative result to 0, else Clamp White lowers one above 1 to
  1. Both default off, so the native node starts as an exact identity; the OpenFX plug-in
  defaulted Clamp Black on.
- **Missing channels.** At 4x4 an RGB or XY image reads alpha as 1; an Alpha image reads R, G
  and B as 0, as the OpenFX plug-in did. At 3x3 an Alpha image passes through.

The kernel sums in the CPU's order with `precise` arithmetic and takes the size as flag bit 4
(`Tests/Native/NativeColourUtilGpu_Test.cpp` checks both sizes against the CPU row).

The display is a generic `KnobDouble::setMatrixDisplay(rows, columns, rowLabels, columnLabels)`
hint, with `setMatrixVisibleExtent(rows, columns)` hiding the fields outside a top-left block;
`KnobGuiValue` lays such a knob out as a headed grid (`Tests/gui/m122_colormatrix.py`).
