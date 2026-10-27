# OCIOColorSpace

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| OCIOColorSpace | transcendental | `expectRender()`'s default, 1e-5 | against OpenColorIO's own CPU processor |

`OCIOColorSpace` (`OCIOColorSpace.h`, plug-in ID `OCIOColorSpace`) converts RGB from
`in_colorspace` to `out_colorspace` of the project's OpenColorIO config, with the Context
variables. Alpha is left as it is. An empty space is the working space, and two names for the
same space (a role, an alias) are an identity with no processor. The spaces come from
`OcioSpaceKnobs`, so each is a string, an `Index` menu and a `Set` flag, and the node shares the
hidden `ocioConfigFile`/`ocioWorkingSpace` pair the project pushes.

- **Preset.** `preset` sets both spaces to the `compositing_log` and `scene_linear` roles, Log to
  Lin or Lin to Log. It replaces openfx-io's OCIOLogConvert; editing a space afterwards returns
  the preset to Custom.
- **Errors.** A space the config lacks, or a config that failed to load, is a persistent error,
  and the input passes through. The menus and the error follow every config change.
- **Retired.** It replaces openfx-io's OCIOColorSpace and OCIOLogConvert, whose IDs
  `Engine/OfxRetired/` retires.
- **GPU.** None yet: the node renders on the CPU, one `CPUProcessor` call per row.

The tolerance is the `expectRender()` default in `Tests/Native/OcioNodeTestSupport.h`: absolute
below 1, relative above (`Tests/Native/NativeOCIOColorSpace_Test.cpp`).
