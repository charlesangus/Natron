# OCIOLookTransform

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| OCIOLookTransform | transcendental | `expectRender()`'s default, 1e-5 | against OpenColorIO's `LookTransform` |

`OCIOLookTransform` (`OCIOLookTransform.h`, plug-in ID `OCIOLookTransform`) applies looks of the
project's config to RGB on the way from `in_colorspace` to `out_colorspace`, with the Context
variables; empty spaces are the working space. `direction` inverse undoes the looks from the
output back to the input. Alpha is left as it is.

- **Looks.** With `single_look` checked, the `look` menu (the config's looks) is the one applied.
  Otherwise `look_combination` is: looks in order, separated by commas, a `-` prefix inverting
  one and `|` separating alternatives. `append` adds the selected look to its end.
- **Errors.** A transform OpenColorIO cannot build, such as from a space or look the config
  lacks, is a persistent error, and the input passes through.
- **Retired.** It replaces openfx-io's OCIOLookTransform, whose ID `Engine/OfxRetired/`
  retires.
- **GPU.** None yet: the node renders on the CPU.

The tolerance is the `expectRender()` default in `Tests/Native/OcioNodeTestSupport.h`
(`Tests/Native/NativeOCIOLookTransform_Test.cpp`).
