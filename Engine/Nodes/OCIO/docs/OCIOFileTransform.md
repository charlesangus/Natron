# OCIOFileTransform

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| OCIOFileTransform | transcendental | `expectRender()`'s default, 1e-5 | against OpenColorIO's `FileTransform` |

`OCIOFileTransform` (`OCIOFileTransform.h`, plug-in ID `OCIOFileTransform`) applies a transform
OpenColorIO reads from `file` (a LUT, a matrix, a CDL, a CLF...) to RGB, `direction` forward or
inverse, with `interpolation` (nearest, linear, tetrahedral or best) and the Context variables.
Alpha is left as it is. No file is an identity.

- **CCC id.** For a `.ccc` or `.cdl`, `cccid` picks the correction; `cccid_menu` lists the
  file's ids.
- **Reload.** `reload` reads the file again through the project's colour management, so a file
  changed on disk is what the node applies.
- **Errors.** A file that cannot be read is a persistent error, and the input passes through.
- **Retired.** It replaces openfx-io's OCIOFileTransform, whose ID `Engine/OfxRetired/`
  retires.
- **GPU.** None yet: the node renders on the CPU.

The tolerance is the `expectRender()` default in `Tests/Native/OcioNodeTestSupport.h`
(`Tests/Native/NativeOCIOFileTransforms_Test.cpp`).
