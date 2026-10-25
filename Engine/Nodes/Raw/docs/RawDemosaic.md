# RawDemosaic

Tolerance: none recorded.

Port of darktable's demosaic module (`src/iop/demosaic.c` and `src/iop/demosaicing/`, darktable
`release-5.6.2`), C paths only. The kernels are in `RawKernels.h`:
- `RawDemosaicBasics.cpp`: PPG, RCD, the photosite modes, green equilibration, colour smoothing.
- `RawDemosaicAmaze.cpp`: AMaZE.
- `RawDemosaicLmmse.cpp`: LMMSE (`lmmse.c`).
- `RawDemosaicVng.cpp`: VNG.
- `RawDemosaicMarkesteijn.cpp`: Markesteijn.
- `RawDemosaicFdc.cpp`: Frequency Domain Chroma (`xtrans_fdc_interpolate()` in `xtrans.c`).
- `RawDemosaicDual.cpp`: dual demosaicing (`dual.c`) with the detail mask it builds
  (`dt_masks_calc_scharr_mask()` and `dt_masks_calc_detail_blend()` from `develop/masks/detail.c`,
  `dt_gaussian_fast_blur()` from `common/gaussian.c`).

- Raw in at the prepared stage, image out. The output is RGBA with alpha 1.
- **Method (Bayer)**: RCD (the default), PPG, AMaZE or VNG4, then the photosite modes, then LMMSE,
  then RCD (dual) and AMaZE (dual).
  **Method (X-Trans)**: Markesteijn 1-pass (the default), Markesteijn 3-pass or VNG, then the
  photosite modes, then Markesteijn 3-pass (dual), then Frequency Domain Chroma.
- **Knob visibility** follows darktable's `gui_changed()` for the source's sensor, read from its
  CFA when the input changes or its metadata is refreshed. A Bayer source shows only the Bayer
  list, an X-Trans one only the X-Trans list. Edge Threshold shows for PPG only, Match Greens for
  Bayer only, Color Smoothing unless the method is a photosite mode or dual, Dual Threshold for a
  dual method only and LMMSE Refine for LMMSE only. With no source, or one without a CFA, both
  lists, Edge Threshold, Match Greens and Color Smoothing show, LMMSE Refine shows for the Bayer
  LMMSE and Dual Threshold when either list holds a dual method.
- **Frequency Domain Chroma** (appended): darktable's `xtrans_fdc_interpolate()`. Markesteijn
  1-pass gives the luma; the chroma is demodulated from the mosaic by four complex 13x13 filters
  and median-filtered. Up to ISO 1600 (`raw/iso`, truncated to an integer as darktable does) each
  chroma channel takes FDC's value only where both of FDC's are about as small as Markesteijn's, above it FDC's alone. 1600
  is the default of darktable's `plugins/darkroom/demosaic/fdc_xover_iso` preference, which is not
  exposed. The filter tables are copied verbatim. The hybrid's test for V compares FDC's `|u|`
  with `1.02 |v|` of Markesteijn, where `1.02 |u|` looks meant, as darktable writes it. Pure FDC is not exact
  on a flat field: its tabulated filters leave the chroma about 5e-4 off a 0.35 grey. Unlike
  Markesteijn it ends without VNG's linear interpolation of the border. No GPU path.
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
- **Dual Threshold**: 0 to 1, default 0.2 (darktable's `dual_thrs`). Shown only for a dual method.
- **Capture Sharpen** (default off): darktable's capture sharpening (`capture.c`), a luminance
  Richardson-Lucy deconvolution of the demosaiced output, run where darktable's `process()` runs
  it: after the method, on the green-matched mosaic, before the dual blend and colour smoothing.
  It applies to every method except the photosite modes, and to data without a CFA only when it
  has one channel (darktable's true monochrome); colour data without a CFA passes through
  untouched. Controls, shown as darktable's `gui_changed()` shows them (the switch where capture
  is supported, the rest while it is on, Sharp Center while Corner Boost is not 0):
  **Iterations** 1 to 25, default 8, run as `n + (int)(0.000065 n^4)` (50 at 25);
  **Radius** 0 to 1.5, 0 measuring it per frame by `_calc_auto_radius()` on the raw centre (the
  middle 60% of each side) before green matching, white balance undone by `1 / max(1, raw/wb_applied)`,
  0.2 added for X-Trans, 0.5 when the frame is too small; the measured value is shown in the
  Radius tooltip and never written to the knob; **Contrast Sensitivity** 0 to 1, whose default is
  darktable's `_get_variance_threshold()` (0.4, minus 0.07 when `raw/sensor_white_level` is over
  4096, moved by 0.012 per 100 ISO from 600, ISO clamped to 100..1000), shown in the knob until
  **Override Contrast Sensitivity** is on (RawHighlights' clipping-threshold pattern; an
  overriding 0 takes the default cut to a hundredth, as `commit_params()` does);
  **Corner Boost** 0 to 1.5, default 0; **Sharp Center** 0 to 1, default 0 (a percentage in
  darktable). The clip mask uses 0.9 x `raw/wb_applied` per colour. Every border condition
  (clip-mask frame, variance window clamp, Gaussian edges, the per-pixel sigma's fade and its
  distance from the centre) is taken from the frame rather than the buffer, and the halo is
  `8 n + 10` for n iterations, so tiles equal the full frame bit for bit, where darktable's
  18-photosite tile overlap is approximate. The clip mask is gathered rather than scattered and the
  automatic radius's maximum is reduced per row, so threaded runs equal serial ones. Frames under 5
  photosites a side are left alone (darktable reads out of bounds there). darktable's "show
  sharpened areas" and "show radius" views are not ported. No GPU path: capture keeps the node on
  the CPU. `raw/sensor_white_level` is set by RawPrepare from the file's white level before it
  normalises `raw/white_level` to 1.
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
  While "passthrough (monochrome)" is chosen the output carries `raw/monochrome`, as darktable
  flags the image `DT_IMAGE_MONOCHROME_BAYER`, so RawColorCalibration defaults to the plain
  channel mixer. The key the Read sets for a monochrome sensor passes through in every mode.
- **Match Greens**: Bayer only, darktable's local, full or both averages. The local threshold
  is 0.0001 x `raw/iso`.
- **Edge Threshold**: PPG's pre-median.
- **Color Smoothing**: 0 to 5 median passes. Forced off for a dual method, as darktable's
  `commit_params()` does, and hidden then.
- Data without a CFA passes through as an image with alpha 1. One channel fills R, G and B.
- RCD and LMMSE are scaled by the processed maximum and AMaZE clips at the processed minimum. Both come from
  `raw/processed_maximum`, as darktable takes them from its pipe.
- Like darktable, it falls back to VNG when the whole image is under 16 photosites on a side.
  AMaZE also falls back below 33, because it reads 32 rows and columns in, and Markesteijn
  3-pass below 19, because it mirrors 18 photosites in from the edges. Frequency Domain Chroma
  mirrors 14 in, which the 16 photosites cover.
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
  - Frequency Domain Chroma lays darktable's 122-photosite tiles every 96 from 13 before the
    buffer's top-left, and a tile's result depends on where it sits. Its buffer starts on the
    image's grid one tile before the window's first and ends where the window's last tile stops
    reading (13 past its output), or at the image edge. The window is first grown by the colour
    smoothing passes.
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
off, and the Dual Threshold knob's default, range and visibility. `NativeRawDemosaicFdc_Test.cpp`
covers Frequency Domain Chroma tiles against the full frame on its tile seams at ISO 400 and 3200, a
flat field at all 36 X-Trans phases in both modes, and the knobs' visibility with a Bayer source,
an X-Trans source and none. `NativeRawDemosaicCapture_Test.cpp` covers capture sharpening: tiles
against the full frame with a fixed and the automatic radius, the automatic radius being repeatable
on a synthetic blurred mosaic, off equal to bypass bit for bit, the knobs' defaults and visibility,
and the automatic radius of the CR2 and RAF fixtures.

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
pass-through.

VNG, VNG4 on Bayer data and VNG on X-Trans data, also runs on the GPU, in
`Engine/Gpu/Kernels/demosaic_vng.slang`, as three dispatches over the same buffer the CPU runs
over (`sourceRegionFor()`, a halo of 6): the linear interpolation, the gradient pass, and the
output, which mixes VNG4's two greens. The work buffer holds the two RGBA planes, 8 floats a
photosite, in bands of at most 128 MB. This covers the images below 33 photosites, and 19 for
Markesteijn 3-pass, where AMaZE and Markesteijn fall back on VNG. The gradient pass compares each
gradient with a threshold over values the linear pass divided, and the device divides within 2.5
ulp, so a pixel whose gradient sits on the threshold can take another set of directions. Bound,
relative to the CPU render's mean colour level: mean error at most 1e-4, and at most one pixel a
thousand (rounded up) with a colour channel off by more than 1e-4; alpha exact. A tiled GPU
render equals the full-frame one bit for bit.

Markesteijn 1- and 3-pass also run on the GPU, in `Engine/Gpu/Kernels/demosaic_markesteijn.slang`,
over the same buffer as the CPU (a halo of 12 or 18) and its 122-photosite tiles, numbered as the
CPU numbers them. VNG's linear pass first interpolates the whole buffer, which keeps the border the
CPU leaves to it; the tiles then go through one dispatch a stage, a tile per workgroup layer: the
mirrored fill, the green minima and maxima (one thread a tile row, each walking the rows the CPU's
serial scan walks from that row, since which photosite of a pair reads the hexagon depends on the
walk; the scan starts each row it enters from the left afresh, and a walk from a row of vertical
pairs ends on the row below, so the walks write disjoint photosites), the four green
directions, and for each pass the green recalculation, the solitary greens' red and blue, red for
blue and blue for red, and the 2x2 greens; then the YPbPr derivatives, the homogeneity maps, and
the 5x5 sums with the average of the most homogeneous directions, written inside the border. No
stage reads what the same stage writes, so a photosite per thread gives the CPU's values. A tile
takes 122 x 122 x (5 x directions + 2) floats: the RGB directions, the green minima and maxima,
the derivatives and the homogeneity maps, separate where the CPU overlaps them. The RGBA plane
comes in bands of at most 64 MB and the tiles in batches of at most 256 MB. Below 14 photosites
(19 for 3 passes) a buffer falls back, as on the CPU, on VNG's linear interpolation. The
recalculated green, the 2x2 greens and the fill's means divide by 3 or a count, and the device
divides within 2.5 ulp, so a pixel whose smoothness comparison or homogeneity count sits on the
threshold can take other directions. Bound, as VNG's, relative to the CPU render's mean colour
level: mean error at most 1e-4, and at most one pixel a thousand (rounded up) with a colour channel
off by more than 1e-4; alpha exact. A tiled GPU render equals the full-frame one bit for bit, and
a flat grey mosaic gives a neutral grey at all 36 X-Trans phases.

AMaZE also runs on the GPU, on Bayer data, in `Engine/Gpu/Kernels/demosaic_amaze.slang` and
`Engine/Nodes/Raw/RawAmazeGpu.h`, over the CPU's 160-photosite tiles stepping by 128, numbered as the
CPU numbers them, one dispatch a stage with a tile per workgroup layer. The buffer is the CPU's
(`sourceRegionFor()`, whose start is on the 128-photosite grid counted from the image's top-left),
so its tiles are the CPU's. Bands of rows split the render when the RGBA plane (4 floats a
photosite) would pass 128 MB: a band ends on that grid, so its buffer again starts on it, and
neighbouring bands share their halo blocks but never move a tile. The tiles go in batches of at most
64 MB, 15 planes of 160 x 160 floats each. Below 33 photosites it falls back on VNG, as above. AMaZE
branches on many comparisons of quotients (a colour ratio against arthresh, two variances, the
Nyquist test, and whether a neighbourhood's hvwt or pmwt is further from 0.5), and the device
divides within 2.5 ulp, so a photosite on a threshold can take the other branch, which the in-place
hvwt and pmwt refinements carry to the photosites after it. Bound, relative to the CPU render's mean
colour level: mean error at most 1e-4, and at most one pixel a thousand (rounded up) with a colour
channel off by more than 1e-4; alpha exact. A tiled GPU render equals the full-frame one bit for
bit.

Frequency Domain Chroma and the dual methods have no GPU path, nor LMMSE (darktable has none either), nor does local equilibration or colour
smoothing; full equilibration measures the whole source per frame, as Inpaint Opposed does. Any of
them keeps the node, and its raw run, on the CPU. GPU tests:
`Tests/Native/NativeRawDemosaicGpu_Test.cpp`, PPG and RCD on the Bayer fixture inside the image and
at its corners, a tiled GPU render against the full-frame one, and the linear pass-through;
`Tests/Native/NativeRawDemosaicAmazeGpu_Test.cpp`, AMaZE on the CR2 fixture inside the image,
across the tile grid and at its corners, and tiled against full frame;
`Tests/Native/NativeRawDemosaicXTransGpu_Test.cpp`, VNG on the RAF and VNG4 on the CR2 fixture
inside the image and at its corners, tiled against full frame, and AMaZE and Markesteijn 3-pass
falling back on VNG on small synthetic mosaics, and Markesteijn 1- and 3-pass on the RAF fixture
inside the image and at its corners, tiled against full frame, and on a flat grey mosaic at every
X-Trans phase.

## darktable controls

Generated by `python3 -I tools/raw/parity-audit.py --write-docs` from
`tools/raw/darktable-parity.json`: every control of darktable's `demosaic` and where it is here.

| darktable item | Natron knob/option | darktable default | Natron default | status |
| --- | --- | --- | --- | --- |
| param green_eq "match greens" | greenEquilibration | DT_IOP_GREEN_EQ_NO | none | mapped |
| param median_thrs "edge threshold" | edgeThreshold | 0.0 | 0.0 | mapped |
| param color_smoothing "color smoothing" | colorSmoothing | DT_DEMOSAIC_SMOOTH_OFF | 0 | mapped |
| param demosaicing_method "method" | method, xtransMethod | DT_IOP_DEMOSAIC_RCD; DT_IOP_DEMOSAIC_MARKESTEIJN for X-Trans (demosaic.c:1478) | rcd, markesteijn1 | mapped |
| param lmmse_refine "LMMSE refine" | lmmseRefine | DT_LMMSE_REFINE_1 | median | mapped |
| param dual_thrs "dual threshold" | dualThreshold | 0.2 | 0.2 | mapped |
| param cs_radius "radius" | captureRadius (0 measures the radius) | 0.0 | 0.0 | mapped |
| param cs_thrs "contrast sensitivity" | captureContrastSensitivity (shown until captureContrastSensitivityOverride is on) | from the image (_get_variance_threshold, capture.c:49) | from the image | mapped |
| param cs_boost "corner boost" | captureCornerBoost | 0.0 | 0.0 | mapped |
| param cs_iter "iterations" | captureIterations | 8 | 8 | mapped |
| param cs_center "sharp center" | captureSharpCenter | 0.0 | 0.0 | mapped |
| param cs_enabled "capture sharpen" | captureSharpen | FALSE | off | mapped |
| enum DT_IOP_DEMOSAIC_PPG "PPG" | method=ppg |  |  | mapped |
| enum DT_IOP_DEMOSAIC_AMAZE "AMaZE" | method=amaze |  |  | mapped |
| enum DT_IOP_DEMOSAIC_VNG4 "VNG4" | method=vng4 |  |  | mapped |
| enum DT_IOP_DEMOSAIC_RCD "RCD" | method=rcd | default |  | mapped |
| enum DT_IOP_DEMOSAIC_LMMSE "LMMSE" | method=lmmse |  |  | mapped |
| enum DT_IOP_DEMOSAIC_RCD_DUAL "RCD (dual)" | method=rcdDual |  |  | mapped |
| enum DT_IOP_DEMOSAIC_AMAZE_DUAL "AMaZE (dual)" | method=amazeDual |  |  | mapped |
| enum DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME "passthrough (monochrome)" | method=passthroughMonochrome |  |  | mapped |
| enum DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR "photosite color (debug)" | method=photositeColor |  |  | mapped |
| enum DT_IOP_DEMOSAIC_VNG "VNG" | xtransMethod=vng |  |  | mapped |
| enum DT_IOP_DEMOSAIC_MARKESTEIJN "Markesteijn 1-pass" | xtransMethod=markesteijn1 |  |  | mapped |
| enum DT_IOP_DEMOSAIC_MARKESTEIJN_3 "Markesteijn 3-pass" | xtransMethod=markesteijn3 |  |  | mapped |
| enum DT_IOP_DEMOSAIC_FDC "frequency domain chroma" | xtransMethod=fdc |  |  | mapped |
| enum DT_IOP_DEMOSAIC_MARKEST3_DUAL "Markesteijn 3-pass (dual)" | xtransMethod=markesteijn3Dual |  |  | mapped |
| enum DT_IOP_DEMOSAIC_PASSTHR_MONOX "passthrough (monochrome)" | xtransMethod=passthroughMonochrome |  |  | mapped |
| enum DT_IOP_DEMOSAIC_PASSTHR_COLORX "photosite color (debug)" | xtransMethod=photositeColor |  |  | mapped |
| enum DT_IOP_DEMOSAIC_MONO "Monochrome" | N/A: no widget: picked by reload_defaults for monochrome sraw files only (demosaic.c:1478); data without a CFA passes through RawDemosaic unchanged |  |  | N/A |
| enum DT_IOP_GREEN_EQ_NO "disabled" | greenEquilibration=none | default |  | mapped |
| enum DT_IOP_GREEN_EQ_LOCAL "local average" | greenEquilibration=local |  |  | mapped |
| enum DT_IOP_GREEN_EQ_FULL "full average" | greenEquilibration=full |  |  | mapped |
| enum DT_IOP_GREEN_EQ_BOTH "full and local average" | greenEquilibration=both |  |  | mapped |
| enum DT_DEMOSAIC_SMOOTH_OFF "disabled" | colorSmoothing (value 0) | default |  | mapped |
| enum DT_DEMOSAIC_SMOOTH_1 "once" | colorSmoothing (value 1) |  |  | mapped |
| enum DT_DEMOSAIC_SMOOTH_2 "twice" | colorSmoothing (value 2) |  |  | mapped |
| enum DT_DEMOSAIC_SMOOTH_3 "three times" | colorSmoothing (value 3) |  |  | mapped |
| enum DT_DEMOSAIC_SMOOTH_4 "four times" | colorSmoothing (value 4) |  |  | mapped |
| enum DT_DEMOSAIC_SMOOTH_5 "five times" | colorSmoothing (value 5) |  |  | mapped |
| enum DT_LMMSE_REFINE_0 "basic" | lmmseRefine=basic |  |  | mapped |
| enum DT_LMMSE_REFINE_1 "median" | lmmseRefine=median | default |  | mapped |
| enum DT_LMMSE_REFINE_2 "3x median" | lmmseRefine=3xMedian |  |  | mapped |
| enum DT_LMMSE_REFINE_3 "refine & medians" | lmmseRefine=refineMedians |  |  | mapped |
| enum DT_LMMSE_REFINE_4 "2x refine + medians" | lmmseRefine=2xRefineMedians |  |  | mapped |
| gui dual mask view | DEFERRED: deferred to a future rich-viewer-overlays milestone |  |  | DEFERRED |
| gui capture sharpen mask view | DEFERRED: deferred to a future rich-viewer-overlays milestone |  |  | DEFERRED |
| gui capture sharpen corner boost mask view | DEFERRED: deferred to a future rich-viewer-overlays milestone |  |  | DEFERRED |
| | captureContrastSensitivityOverride | | | EXTRA: captureContrastSensitivity shows the file-derived default until turned on; darktable stores the value in its params instead |
