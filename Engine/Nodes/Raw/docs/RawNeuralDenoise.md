# RawNeuralDenoise

Tolerance: against darktable's neural restore of a 512 x 512 crop of the Canon fixture, the
mean absolute difference of normalised photosites is at most 2e-3 and the largest at most 3e-2.
These bounds are provisional until the reference is recorded (see Reference). Against the
fixture models, 1e-5 for flat fields and 1e-6 between a window and the full frame.

Port of the Bayer path of darktable's neural restore (`src/common/ai/restore_raw_bayer.c`,
`restore.c`, `restore_common.h`, consumer `src/libs/neural_restore.c`). The source is darktable
`release-5.6.2`, commit `20891f6c6fa7be995e5bf9dff9d1ee5062051773`, the `DARKTABLE_REF` of
`tools/ci/local/fetch-assets.sh`. PR #20854 is in that release, so master was not needed. The
model is RawNIND UtNet2 Bayer (`model_bayer.onnx`) from the `rawdenoise-nind` package of
darktable-ai `release-5.6.0` (commit `6bcd41c6`, `models/rawdenoise-nind/model.yaml`).

## Contract

Line numbers are in darktable at the commit above unless a file says otherwise.

### Model and policy

- The package's `config.json` gives the policy under `attributes.model_bayer`, with
  `attributes.input_sizes` for the whole package. The keys are parsed in `restore.c:111-155`, and
  their defaults are set in `restore.c:353-403`. RawNIND ships:
  - `input_kind: bayer_v1`
  - `bayer_orientation: force_rggb`
  - `edge_pad: mirror_cropped`
  - `wb_norm: none`
  - `output_scale: match_gain`
  - `input_sizes: [512]`
- The tile size is the first entry of `<stem>.input_sizes`, else of `input_sizes`
  (`restore.c:187-206`). The ONNX file is exported static (`model.yaml`, `convert:` with
  `static: true`, 512 x 512).
- One inference takes `[1, 4, T, T]` and returns `[1, 3, 2T, 2T]`
  (`dt_restore_run_patch_bayer()`, `restore.c:554-575`). The model denoises and demosaics in one
  pass.

### Input

- **Stage.** darktable feeds the raw mosaic straight from the full mipmap, black not subtracted
  and not white balanced (`neural_restore.c:1210-1240`).
- **Normalisation.** Each photosite becomes `(value - black[site]) / (white - black[site])`. The
  per-site black is from the 2x2 block index `((row & 1) << 1) | (col & 1)` (`_pack_bayer_tile()`,
  `restore_raw_bayer.c:316-319`; black and range from `_compute_cfa_black_range()`,
  `restore_common.h:173-196`).
- **White balance.** The normalised value is multiplied by `wb_norm` for its colour
  (`restore_raw_bayer.c:320-322`). `wb_norm` is chosen in `_compute_bayer_prep()`
  (`restore_raw_bayer.c:116-160`):
  - `none` is 1, 1, 1. This is what RawNIND uses.
  - `daylight` is D65 from the Adobe matrix (`_bayer_wb_daylight()`, `:81-100`), falling back to
    as-shot.
  - `as_shot` is the as-shot multipliers over green (`_bayer_wb_as_shot()`, `:103-114`), falling
    back to daylight.
- **Packing.** The CFA becomes four planes at half resolution, one per site of each 2x2 block.
  Plane `k` takes the site `(dr, dc) = (k >> 1, k & 1)`, read from the top-left of the block down
  (`_pack_bayer_tile()`, `restore_raw_bayer.c:291-327`).
- **Orientation.** With `force_rggb`, every block starts on the red photosite. The origin `(y0,
  x0)` is the first red site in the top-left 2x2 of the image (`_bayer_origin()`, `:45-57`), so
  the planes are always R, G (right of R), G (below R), B (`_bayer_tile_geometry()`,
  `:231-280`). Non-Bayer layouts (`filters` 0 or 9, X-Trans) are refused (`:46`, `:122-129`).
- **Edges.** Reads past the image are mirrored without repeating the edge photosite, and
  periodically for long reaches (`_mirror()` and `_mirror_in_range()`,
  `restore_common.h:201-221`). With `mirror_cropped`, the mirror range starts at `(y0, x0)` and
  its length is cut to an even number, so a reflected block is still RGGB
  (`restore_raw_bayer.c:259-270`). The range is the visible crop, never the optical black
  (`:252-257`).

### Tiling

- Tiles are laid in packed (half-resolution) pixels over the working region. The working region is
  `[y0, y0 + 2Hh) x [x0, x0 + 2Wh)`, with `Hh = (visibleHeight - y0) / 2` and `Wh` likewise
  (`restore_raw_bayer.c:367-375`).
- The overlap is `O = 32` packed pixels (`OVERLAP_PACKED`, `:40`). Tiles step by `T - 2O`, and a
  tile size below `4O` is refused (`:378-383`). The grid has `ceil(Wh / step)` columns and
  `ceil(Hh / step)` rows (`:388-389`).
- Tile `(tx, ty)` reads packed rows from `ty * step - O` for `T` rows, and columns likewise
  (`:471-485`). Its core is `[ty * step, min((ty + 1) * step, Hh))`.
- Neighbouring tiles are blended over the `2O` packed pixels centred on the edge between their
  cores, which is 128 sensor pixels. The weights are linear ramps that sum to one
  (`_seam_ramp()`, `_seam_ax()`, `_seam_ay()`, `restore_common.h:228-255`, used at
  `restore_raw_bayer.c:437` and `:616-700`). A tile with no neighbour on a side stops at its core
  there.

### Output

- **Gain.** With `match_gain`, each tile's three output planes are scaled so that their mean equals
  the mean of its four input planes. The mean is taken over the whole tile, mirrored padding
  included. The gain may be negative. If the output mean is within `1e-6 * |input mean|` of zero,
  the gain is 1 (`_bayer_gain_match()`, `restore_raw_bayer.c:176-213`, applied at `:547-549`).
  With `absolute` the output is left alone.
- **Colour space.** Camera RGB at full resolution, in the input's normalised, `wb_norm` frame.
  The camera matrix is not applied (darktable-ai `models/rawdenoise-nind/README.md`).
- **Re-mosaic.** darktable keeps only each photosite's own colour. It undoes the white balance,
  normalisation and black, `model / wb_norm[colour] * range + black`
  (`_bayer_remosaic_raw()`, `restore_raw_bayer.c:163-170`, used at `:646-649`).
- **Strength.** The result is blended with the source mosaic in raw ADC units,
  `strength * model + (1 - strength) * source` (`:342-344`, `:653`). It is clipped to
  `[0, white]` and written as a 16-bit Bayer DNG (`:684-745`, `neural_restore.c:1258-1295`).
- **Margins.** The 0 to 2 rows and columns outside the working region keep the source photosites
  (`restore_raw_bayer.c:356-364`).

### User controls

For raw data, darktable's neural restore has a single control, **strength**: 0 to 100 %, default
100, "blend between the source CFA (0%) and the denoised output (100%)"
(`neural_restore.c:4382-4396`). The Bayer or linear model is chosen by the active `rawdenoise`
model, not by a control (`:4374-4376`). The output folder, file name and bit depth are batch
export settings and do not apply to a node.

### Mapping onto Natron's raw contract

`RawMetadata.h` defines the raw stages, and there are only two: `raw` and `prepared`. Natron has
no demosaiced stage. The node therefore takes RawDemosaic's place and contract:

- **Input stage: prepared.** RawPrepare outputs `(value - black) / (white - black)` over the
  visible crop, which is darktable's normalised value. Feeding the prepared stage loses nothing,
  and it lets the node sit after RawTemperature and RawHighlights, as RawDemosaic does. Any other
  stage is a persistent error. Two things differ from darktable's input:
  - RawPrepare applies the DNG flat-field gain map.
  - In the Develop RAW chain, RawHighlights has already reconstructed clipped photosites.
- **White balance.** RawTemperature has multiplied each colour by `raw/wb_applied`. Packing
  divides that back out and then multiplies by `wb_norm`, so the model sees darktable's input.
  The output is multiplied by `wb_applied / wb_norm`, so it is in the same white-balanced camera
  RGB frame RawDemosaic outputs, which RawInputColor expects.
- **Layout.** The CFA comes from `raw/cfa_pattern` and `raw/cfa_origin` (`RawLayout`). The red
  origin `(y0, x0)` is found from the top-left photosite of the source's region of definition,
  which is RawPrepare's crop. An X-Trans or CFA-less source is a persistent error that points to
  NeuralDenoise's RawNIND linear model after RawInputColor.
- **Output metadata.** The output is an image, so, as in RawDemosaic, `raw/cfa_pattern`,
  `raw/cfa_origin`, `raw/stage`, `raw/black_levels` and `raw/white_level` are removed. The camera
  matrix and the white-balance keys stay for RawInputColor and RawColorCalibration.
- **Output values.** Camera RGB, not a re-mosaic: the node replaces the demosaic, so the model's
  own interpolation is kept. Strength therefore blends with a bilinear demosaic of the same input
  (`RawKernels::vng()` with only its linear interpolation), not with the mosaic.

## Knobs

- **Model** (`model`), with **Download Model** (`downloadModel`): the `denoise-raw-bayer` model,
  from Preferences > AI Models. RawNIND UtNet2 Bayer is the only one listed. A model that is not
  installed, or AI switched off, is a persistent error.
- **Strength** (`strength`): 0 to 1, default 1. Mixes the model's output with a bilinear demosaic
  of the same input, `strength * model + (1 - strength) * bilinear`. At 0 the model does not run.
  darktable's 0 to 100 % slider mixes with the source mosaic instead (see Deviations).
- **Tile Size** (`tileSize`): 0 to 4096, default 0, not animated. The edge of a tile in 2x2
  photosite blocks. 0 takes the model's `input_sizes`, else 512. Below 128 (`4 * O`) is an error,
  as in darktable. RawNIND's ONNX file is exported at a fixed 512, so any other size is a
  persistent error that names the size the model takes. darktable has no such control: it always
  uses the model's size.

## Deviations

- **Input stage and output values.** Prepared input, white balance divided out and multiplied
  back, camera RGB out instead of a re-mosaic. See the mapping above.
- **Strength** mixes with a bilinear demosaic, not with the source mosaic, because the output is
  an image.
- **Clipping.** Only negatives are clipped. darktable clips to `[0, white]` on writing its 16-bit
  DNG; values above white are kept here for RawHighlights' reconstruction and scene-referred
  grading.
- **Daylight white balance** (models with `wb_norm: daylight`) is computed from `raw/cam_to_xyz`,
  inverted to camera from XYZ, instead of from the Adobe matrix darktable reads; they can differ
  slightly. RawNIND uses `wb_norm: none`, so this applies only to other models.
- **Tiling** goes through the shared tiler (`runTiled()` with `linearBlend` and a per-tile
  `finish` hook for the gain match). A render reads the darktable tiles that reach its window, so
  windows match the full frame.
- **Margins.** The 0 to 2 rows and columns outside the 2x2 blocks take the bilinear demosaic, not
  the source photosite.
- **Proxy scale.** The node renders at full resolution only. The planned bilinear fallback below
  half scale with a viewer notice is not implemented.
- **GPU.** No GPU path, and no `gpuWeight`: the render cost seeds are measured, not declared.

## Reference

darktable-cli cannot run neural restore, which exists only as the lighttable utility in
`src/libs/neural_restore.c`. darktable's Lua `ai_lib` (`src/lua/ai.c`) exposes tensors and model
runs, not the restore pipeline, so it would test a re-implementation. The reference is therefore
darktable's own output, made by hand:

1. In darktable release-5.6.2 with RawNIND installed, run neural restore, raw denoise, strength
   100 %, on `Tests/fixtures/raw/canon-eos-5d-mark-iii.cr2`. It writes
   `canon-eos-5d-mark-iii_raw-denoise.dng`.
2. Run `tools/raw/make-neural-restore-reference.py <that DNG>
   Tests/fixtures/raw/reference/canon-eos-5d-mark-iii-neural-restore.tif`. It keeps the 512 x 512
   photosites at (3072, 1728) from the top-left of the visible image, normalised.

`NativeRawNeuralDenoise.MatchesDarktableNeuralRestore` renders Read, RawPrepare and this node over
the same crop and compares each photosite's own colour, both clipped to `[0, 1]`. It runs when
RawNIND is installed in the folder `NATRON_TEST_AI_MODELS_DIR` names, else in the default models
folder, and when the reference exists. Otherwise it prints why and passes.

The other tests run offline with `Tests/fixtures/ai/rawdenoise.dtmodel`
(`tools/ai/make-test-rawdenoise.py`).
