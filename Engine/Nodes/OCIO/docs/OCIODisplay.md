# OCIODisplay

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| OCIODisplay | 1e-4 | `kDisplayTolerance` | the reference asks OpenColorIO for the display processor another way |

`OCIODisplay` (`OCIODisplay.h`, plug-in ID `OCIODisplay`) renders RGB as a display of the
project's config shows it, the way the viewer does: `gain` on the input, the `channel_selector`
view, the display and view transform from `in_colorspace` with the Context variables, then
`gamma`. Alpha is left as it is. An empty input space is the working space.

- **Menus.** `display` and `view` list the config's displays and the chosen display's views; a
  new node takes the config's default display and view, and changing the display takes its
  default view when the current one is not in it.
- **Channel view.** RGB, R, G, B, A (1 for an RGB input) or the luminance with the working
  space's weights, shown in R, G and B.
- **Errors.** A transform OpenColorIO cannot build, such as from a space the config lacks, is a
  persistent error, and the input passes through.
- **Retired.** It replaces openfx-io's OCIODisplay, whose ID `Engine/OfxRetired/` retires.
- **GPU.** None yet: the node renders on the CPU.

`Tests/Native/NativeOCIODisplay_Test.cpp` holds the bound, absolute below 1 and relative above.
