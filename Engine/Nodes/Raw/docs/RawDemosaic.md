# RawDemosaic

Tolerance: none recorded.

Port of darktable's demosaic module (`src/iop/demosaic.c` and `src/iop/demosaicing/`, darktable
`release-5.6.2`), C paths only. The kernels are in `RawKernels.h`:
- `RawDemosaicBasics.cpp`: PPG, RCD, the photosite modes, green equilibration, colour smoothing.
- `RawDemosaicAmaze.cpp`: AMaZE.
- `RawDemosaicLmmse.cpp`: LMMSE (`lmmse.c`).
- `RawDemosaicVng.cpp`: VNG.
- `RawDemosaicMarkesteijn.cpp`: Markesteijn.
- `RawDemosaicDual.cpp`: dual demosaicing (`dual.c`) with the detail mask it builds
  (`dt_masks_calc_scharr_mask()` and `dt_masks_calc_detail_blend()` from `develop/masks/detail.c`,
  `dt_gaussian_fast_blur()` from `common/gaussian.c`).

- Raw in at the prepared stage, image out. The output is RGBA with alpha 1.
- **Method (Bayer)**: RCD (the default), PPG, AMaZE or VNG4, then the photosite modes, then LMMSE,
  then RCD (dual) and AMaZE (dual).
  **Method (X-Trans)**: Markesteijn 1-pass (the default), Markesteijn 3-pass or VNG, then the
  photosite modes, then Markesteijn 3-pass (dual).
- **Dual methods** (appended, so saved values keep their meaning): darktable's `dual_demosaic()`
  after the method, on the same green-matched mosaic. A blend mask is built from the method's
  output: the white balance is undone (1 / `raw/wb_applied`, none when absent, as darktable uses
  its pipe's temperature coefficients), the Scharr gradient of the square root of the mean of R, G
  and B is divided by 16 and clipped, passed through darktable's sigmoid at
  `0.005 * threshold^1.1` (`_slider2contrast()`) and blurred by darktable's 9x9 Gaussian of sigma 2.
  The output is mask x method + (1 - mask) x VNG, VNG being the linear interpolation only followed
  by 2 colour smoothing passes, as darktable runs it. A threshold of 0 skips the blend entirely, so
  the output is the method's own; the mask is never exactly 0 elsewhere (its sigmoid floor is about
  1e-7). Below 16 photosites a side the tiny-image VNG fallback applies and there is no blend.
  `dt_fast_hypotf()` is taken as the plain square root of the sum of squares of darktable's
  `-ffast-math` Release builds. darktable's "show dual mask" view is not ported. No GPU path.
- **Dual Threshold**: 0 to 1, default 0.2 (darktable's `dual_thrs`). Shown only when either
  method list holds a dual method, since the node does not know the sensor when knobs change.
- **LMMSE Refine**: darktable's five levels: basic, median (the default), 3x median, refine &
  medians and 2x refine + medians. It is shown only when the method is LMMSE, as in darktable's
  `gui_changed()`. LMMSE works in darktable's gamma space, scaled by the processed maximum, and PPG
  fills the outer 4 photosites.
- **Photosite modes**, appended to both method lists so saved values keep their meaning:
  "passthrough (monochrome)" puts each photosite's value in R, G and B; "photosite color (debug)"
  puts it in its own CFA colour's channel and 0 in the others (darktable's `passthrough.c`). They
  are Point ops with no halo, and darktable's tiny-image fallback to VNG does not apply to them.
  Match Greens and Color Smoothing still apply, as in darktable. No GPU path: they keep the node
  on the CPU. darktable's "Monochrome" (a true mono sensor) is the no-CFA pass-through below.
- **Match Greens**: Bayer only, darktable's local, full or both averages. The local threshold
  is 0.0001 x `raw/iso`.
- **Edge Threshold**: PPG's pre-median.
- **Color Smoothing**: 0 to 5 median passes. Forced off for a dual method, as darktable's
  `commit_params()` does (darktable also hides it then; here it stays visible).
- Data without a CFA passes through as an image with alpha 1. One channel fills R, G and B.
- RCD and LMMSE are scaled by the processed maximum and AMaZE clips at the processed minimum. Both come from
  `raw/processed_maximum`, as darktable takes them from its pipe.
- Like darktable, it falls back to VNG when the whole image is under 16 photosites on a side.
  AMaZE also falls back below 33, because it reads 32 rows and columns in, and Markesteijn
  3-pass below 19, because it mirrors 18 photosites in from the edges.
- **Tiling.** A render reads its window grown by the method's reach, aligned to the CFA period
  from the source's top-left photosite, at least 32 photosites per side.
  - The reach is darktable's tiling overlap: PPG 8, RCD 10, VNG 6, Markesteijn 12 and 18. Colour
    smoothing adds one per pass, and local green matching adds 2. A dual method adds 5, the
    mask's Scharr gradient and 9x9 blur, to the method's reach, and at least 8 (VNG and its 2
    smoothing passes); with AMaZE the window is grown by 5 before its tile-grid span is taken.
  - AMaZE aligns its buffer to its own 128-photosite tile grid, counted from the image's top-left,
    one block before the window. Its seams carry state in place, so no fixed halo reproduces the
    full frame.
  - LMMSE does the same with darktable's 136-photosite tiles, which load 128 rows and columns,
    advance by 112 and write the middle 112. The first and last tiles of the image write further
    out and median and refine differently. So the buffer starts one tile before the window's first
    tile and ends 4 photosites past the last tile's reads, or at the image edge.
  - A tile is therefore bit for bit the same pixels as the full frame. Full green matching
    measures the whole image once per source and settings.
- Every per-tile scratch buffer starts zeroed. darktable reuses one per thread, which makes some
  of its tile-edge reads depend on the tile the thread ran before. For LMMSE this keeps darktable's
  zeros past the image edge, so the photosites just inside the PPG frame are not exactly neutral
  on a flat field.
- The output metadata drops `raw/cfa_pattern`, `raw/cfa_origin`, `raw/stage`, `raw/black_levels`
  and `raw/white_level`. It keeps `raw/cam_to_xyz` and the white balance keys for RawInputColor.
- It renders at full resolution only, and the host downscales the result for a proxy.

Tests: `Tests/Native/NativeRawDemosaic_Test.cpp`, on synthetic Bayer and X-Trans mosaics. Real
X-Trans files and darktable-reference comparisons come later. `NativeRawDemosaicPassthrough_Test.cpp` covers the photosite modes on hand-checked
Bayer and X-Trans patterns, and their tiles against the full frame. `NativeRawDemosaicLmmse_Test.cpp`
covers LMMSE tiles against the full frame at every refine level and two CFA phases, a flat field, and
the LMMSE Refine knob's visibility. `NativeRawDemosaicDual_Test.cpp` covers the three dual methods:
tiles against the full frame at thresholds 0.2 and 0.6 (and with local green matching), threshold 0
equal to the plain method, busy areas keeping the method and flat ones not, colour smoothing being
off, and the Dual Threshold knob's default, range and visibility.

GPU: PPG and RCD without green equilibration or colour smoothing, and the pass-through of data
without a CFA, in `Engine/Gpu/Kernels/demosaic_bayer.slang`, following the passes of darktable's
`demosaic_ppg.cl` and `demosaic_rcd.cl` with the CPU port's arithmetic: the median, PPG's green
and red-blue passes, RCD's population, its steps 1 to 4.3, and the output. Each pass covers the
whole buffer the CPU would run over for the window (`sourceRegionFor()`), with the CPU's row and
column ranges and its half-width plane indexing, so it gives the values the CPU keeps from its
112-photosite tiles. The window is processed in bands sharing one work buffer of at most 128 MB;
a band's result does not depend on the band, as a tile's does not. The passes read global memory;
none uses workgroup-shared tiles. The device divides within 2.5 ulp: bound
`ParityTolerance::transcendental()` for PPG and RCD, `ParityTolerance::exact()` for the
pass-through. AMaZE, VNG, Markesteijn and the dual methods have no GPU path, nor LMMSE (darktable has none either), nor does local equilibration or colour
smoothing; full equilibration measures the whole source per frame, as Inpaint Opposed does. Any of
them keeps the node, and its raw run, on the CPU. GPU tests:
`Tests/Native/NativeRawDemosaicGpu_Test.cpp`, PPG and RCD on the Bayer fixture inside the image and
at its corners, a tiled GPU render against the full-frame one, and the linear pass-through.
