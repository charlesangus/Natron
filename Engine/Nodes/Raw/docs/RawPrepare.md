# RawPrepare

Tolerance: none recorded.

Port of darktable's `rawprepare` (`src/iop/rawprepare.c`, darktable `release-5.6.2`), without
its OpenCL path and flat-field gain maps.

- Raw in, raw out. The source can be at the raw or the prepared stage. A prepared source passes
  through unchanged, and the node shows a warning. A source with no `raw/stage` renders nothing,
  with `RawMetadata::requireStage()`'s error.
- Each photosite becomes `(value - black) / (white - black)`. `black` is the level of the
  photosite's cell of the 2x2 block at the CFA origin (`raw/black_levels`), or of its channel for
  data without a CFA. `white` is `raw/white_level`. Both are in the units of the raw data. The
  **Override Black Levels** and **Override White Level** knobs replace them. Turning an override on
  in the interface fills the knob from the file.
- The kept area is `raw/crop` (the sensor's active area), or the source's region less the
  **Crop** margins when **Override Crop** is on. It moves to the origin, and the CFA origin moves
  with it (`RawLayout::croppedTo()`), so every photosite keeps its colour.
- The output's metadata has `raw/stage` `prepared`, zero black levels, a white level of 1,
  `raw/crop` covering the whole output and `raw/processed_maximum` (1, 1, 1).
- It renders at full resolution only (`supportsRenderScale` is no), because resampling a mosaic
  has no meaning.

Tests: `Tests/Native/NativeRawPrepare_Test.cpp`, on synthetic mosaics. No real camera files yet.

GPU: `rawprepare_wrapped` (`Engine/Gpu/Kernels/rawprepare.slang`), a wrapped point kernel. The
source view is moved by the kept area's corner, and the parameter block (`RawPrepareGpuParams`,
`Engine/Nodes/Raw/RawGpuParams.h`) carries the cropped CFA layout, so each output pixel finds the
black level cell of the photosite it shows. The GPU divides within 2.5 ulp: bound
`ParityTolerance::arithmetic()`. A prepared source is an identity and a source without
`raw/stage` stays on the CPU, which fails it. GPU tests: `Tests/Native/NativeRawPrepTempGpu_Test.cpp`,
on the Bayer, X-Trans and linear fixtures, with the file's levels and with overridden levels and
an odd crop.
