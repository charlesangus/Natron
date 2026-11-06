# OCIODisplay

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| OCIODisplay | 1e-4 | `kDisplayTolerance` | the reference asks OpenColorIO for the display processor another way |

`OCIODisplay` (`OCIODisplay.h`, plug-in ID `OCIODisplay`) renders RGB as a display of the
project's config shows it, the way the viewer does: `gain` on the input, the `channelSelector`
view, the display and view transform from `inputColorspace` with the context variables of the
`ocioContext` group, then `gamma`. Alpha is left as it is. The `inputColorspace` menu's first
entry, `projectDefault`, is the project's working space, its label showing the space it stands
for.

- **Menus.** `display` and `view` list the config's displays and the chosen display's views,
  each after a `projectDefault` entry: the config's default display, and that display's default
  view. Both are the defaults of a new node, follow the config and the display live, and are
  labelled with what they stand for ("Project default (sRGB - Display)"). A display or view the
  config or the display lacks after a change returns to `projectDefault`.
- **Channel view.** RGB, R, G, B, A (1 for an RGB input) or the luminance with the working
  space's weights, shown in R, G and B.
- **Errors.** A transform OpenColorIO cannot build, such as from a space the config lacks, is a
  persistent error, and the input passes through.
- **Retired.** It replaces openfx-io's OCIODisplay, whose ID `Engine/OfxRetired/` retires.
- **GPU.** None yet: the node renders on the CPU.

`Tests/Native/NativeOCIODisplay_Test.cpp` holds the bound, absolute below 1 and relative above.
