# OCIOCDLTransform

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| OCIOCDLTransform | transcendental | `expectRender()`'s default, 1e-5 | against OpenColorIO's `CDLTransform` |

`OCIOCDLTransform` (`OCIOCDLTransform.h`, plug-in ID `OCIOCDLTransform`) applies an ASC CDL to
RGB, `direction` forward or inverse. Alpha is left as it is. The default values are an identity,
with no processor.

- **Values.** `slope`, `offset`, `power` and `saturation`, or, with `read_from_file`, the CDL of
  the `.cc`, `.ccc` or `.cdl` in `file`, picked by `cccid` (the `cccid_menu` lists the file's
  ids). The knobs then show the file's values. `reload` reads the file again.
- **Export.** `export_cc` writes the knobs' CDL, at the current time, to a `.cc`.
- **Errors.** A file that cannot be read is a persistent error, and the input passes through.
- **Retired.** It replaces openfx-io's OCIOCDLTransform, whose ID `Engine/OfxRetired/` retires.
- **GPU.** None yet: the node renders on the CPU.

The tolerance is the `expectRender()` default in `Tests/Native/OcioNodeTestSupport.h`
(`Tests/Native/NativeOCIOFileTransforms_Test.cpp`).
