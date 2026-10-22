# RawDemosaic

Tolerance: none recorded.

Port of darktable's demosaic module (`src/iop/demosaic.c` and `src/iop/demosaicing/`, darktable
`release-5.6.2`), C paths only. The kernels are in `RawKernels.h`:
- `RawDemosaicBasics.cpp`: PPG, RCD, green equilibration, colour smoothing.
- `RawDemosaicAmaze.cpp`: AMaZE.
- `RawDemosaicVng.cpp`: VNG.
- `RawDemosaicMarkesteijn.cpp`: Markesteijn.

- Raw in at the prepared stage, image out. The output is RGBA with alpha 1.
- **Method (Bayer)**: RCD (the default), PPG, AMaZE or VNG4. **Method (X-Trans)**: Markesteijn
  1-pass (the default), Markesteijn 3-pass or VNG.
- **Match Greens**: Bayer only, darktable's local, full or both averages. The local threshold
  is 0.0001 x `raw/iso`.
- **Edge Threshold**: PPG's pre-median.
- **Color Smoothing**: 0 to 5 median passes.
- Data without a CFA passes through as an image with alpha 1. One channel fills R, G and B.
- RCD is scaled by the processed maximum and AMaZE clips at the processed minimum. Both come from
  `raw/processed_maximum`, as darktable takes them from its pipe.
- Like darktable, it falls back to VNG when the whole image is under 16 photosites on a side.
  AMaZE also falls back below 33, because it reads 32 rows and columns in, and Markesteijn
  3-pass below 19, because it mirrors 18 photosites in from the edges.
- **Tiling.** A render reads its window grown by the method's reach, aligned to the CFA period
  from the source's top-left photosite, at least 32 photosites per side.
  - The reach is darktable's tiling overlap: PPG 8, RCD 10, VNG 6, Markesteijn 12 and 18. Colour
    smoothing adds one per pass, and local green matching adds 2.
  - AMaZE aligns its buffer to its own 128-photosite tile grid, counted from the image's top-left,
    one block before the window. Its seams carry state in place, so no fixed halo reproduces the
    full frame.
  - A tile is therefore bit for bit the same pixels as the full frame. Full green matching
    measures the whole image once per source and settings.
- Every per-tile scratch buffer starts zeroed. darktable reuses one per thread, which makes some
  of its tile-edge reads depend on the tile the thread ran before.
- The output metadata drops `raw/cfa_pattern`, `raw/cfa_origin`, `raw/stage`, `raw/black_levels`
  and `raw/white_level`. It keeps `raw/cam_to_xyz` and the white balance keys for RawInputColor.
- It renders at full resolution only, and the host downscales the result for a proxy.

Tests: `Tests/Native/NativeRawDemosaic_Test.cpp`, on synthetic Bayer and X-Trans mosaics. Real
X-Trans files and darktable-reference comparisons come later.
