# RawHighlights

Tolerance: none recorded.

Port of darktable's highlight reconstruction (`src/iop/highlights.c` with
`hlreconstruct/opposed.c`, `lch.c`, `inpaint.c`, `laplacian.c` and the reference average of `segbased.c`, darktable
`release-5.6.2`).

- Raw in, raw out, at the prepared stage.
- **Clipping Threshold** is multiplied by `raw/white_level`, which is 1 after RawPrepare. It
  shows the smaller of 1 and `raw/linear_response_limit` (1 when the key is absent), as
  darktable's `reload_defaults()` sets it, until **Override Clipping Threshold** is on; turning
  the override off shows the file's value again, and changing the input keeps an override.
- A mode the source cannot run falls back to Inpaint Opposed with darktable's message as a node
  warning ("highlights: mode not available for this type of image. falling back to inpaint
  opposed."); the Method knob keeps the choice, so a CFA source brings it back. Each mode
  declares the sensor it needs in `rawHighlightsModeRequirement()`: Reconstruct in LCh and
  Reconstruct Color and Segmentation Based need a CFA, and Guided Laplacians needs a Bayer mosaic,
  so an X-Trans source falls back too.
- **Clip Highlights** clips every photosite to the threshold, whatever its colour, which turns
  clipped areas white.
- **Unclipped** is an identity.
- **Inpaint Opposed** is the default, as in darktable. Its threshold is 0.987 x the clipping
  threshold x the `raw/wb_applied` multiplier of each colour. A clipped photosite becomes the
  larger of itself and the cube of the mean cube roots of the opposed colours in its 3x3
  neighbourhood, plus a chrominance offset. That offset is measured over the whole image on the
  unclipped photosites next to clipped ones. The node asks for the whole source and reads it for
  the offset in place, once per source and settings, so every tile agrees; each render copies
  only its window with a 2-photosite halo. Linear data gets darktable's linear variant.
- **Reconstruct in LCh** works on Bayer and X-Trans mosaics with a 2-photosite halo, as darktable's
  `process_lch_bayer()` and `process_lch_xtrans()` do. On data without a CFA it is Inpaint
  Opposed, as in darktable.
- **Reconstruct Color** (`inpaint.c`, a1ex's colour inpainting) scans every row both ways and then
  every column both ways over the whole frame (`RawKernels::inpaintColor()`). A clipped photosite
  is rebuilt from the next one in the scan: from its value times a colour ratio that decays
  exponentially over the unclipped pairs passed on the way (Bayer), or per colour pair (X-Trans,
  looking diagonally at the start of a 2x2 green block); the result is the mean of the four
  rebuilds. The threshold is the clipping threshold times the `raw/wb_applied` multiplier of each
  colour. The node keeps the result as one float per photosite under `RawNodeSupport::frameKey()`,
  reading the source in place, and each render copies its window. On data without a CFA it falls
  back to Inpaint Opposed. The label and the value are appended to the Method choice after
  Reconstruct in LCh.
- Image borders are those of the whole source, never of a tile. darktable's X-Trans LCh keeps a
  ring buffer of clipped columns, which is evaluated directly here with the same result.
- Clip, LCh and Reconstruct Color set `raw/processed_maximum` the way darktable updates its processed maximum.
- **Segmentation Based** (`segbased.c` and `segmentation.c`, `RawKernels::segmentsHighlights()`)
  starts from Inpaint Opposed and its chroma, then works on three planes at a third of the mosaic
  size: clipped areas are dilated by **Combine Segments** and flood-filled into segments, and
  each segment takes the cube of the most suitable unclipped neighbourhood around it, as
  **Candidating** weighs smoothness against the opposed result. **Rebuild** adds a gradient to
  all-clipped segments, scaled by **Strength**, with Poisson noise at **Noise Level** seeded from
  each segment's position, so renders repeat. Strength and Noise Level show only when Rebuild is
  not off. The whole frame is computed once per `RawNodeSupport::frameKey()` and each render
  copies its window. It has no GPU path and none of darktable's mask views. Where darktable
  races in the candidate pass, segments are applied in darktable's serial order.
- **Guided Laplacians** (`laplacian.c`, Aurélien Pierre's, `RawKernels::laplacianReconstruct()` and
  `laplacianRemosaic()`) builds a bilinear RGB image and its norm from the mosaic, divided by the
  `raw/wb_applied` multipliers, with a mask of the colours that read a photosite above 0.995 x the
  clipping threshold x that multiplier, blurred by a 5x5 box mean. Both are downsampled 4 times;
  there **Iterations** rounds each run guided Laplacians (the clipped colours follow the high
  frequencies of the channel with the most detail, with Poisson noise at **Noise Level** / 4 in the
  last round) then heat diffusion of the colour ratios (**Inpaint a Flat Color** damps it towards a
  flat colour), over `ceil(log2(diameter / 4))` wavelet scales for **Diameter of Reconstruction**.
  The result is upsampled and blended over the mosaic by the blurred mask. Noise Level, Iterations,
  Inpaint a Flat Color and Diameter of Reconstruction show only for this mode; Noise Level is shared
  with Segmentation Based. The downsampled result is computed once per
  `RawNodeSupport::frameKey()` from the whole source; each render rebuilds its window at full
  resolution from it and its own photosites grown by 3, so tiles match the full frame bit for bit
  and the full-resolution buffers darktable allocates are never held. A mosaic narrower or shorter
  than 4 photosites passes through. Differences from darktable:
  - The box mean sums each 5-photosite window directly where darktable keeps a running sum down
    each column. The horizontal means of 0 and 1 are the same; the vertical sums can differ in the
    last bits, and no longer depend on the rows above a tile.
  - The noise is seeded from the downsampled position, as in darktable; since that frame is always
    whole, renders repeat and tiles agree.
  - darktable tiles this mode with an overlap when memory is short, and its result then depends on
    the tiling; here the downsampled frame is always whole.
  - Clipping thresholds use `raw/wb_applied` where darktable uses its processed maximum, as for
    Reconstruct Color; `raw/processed_maximum` is left alone, as darktable does for this mode.
- It renders at full resolution only.

Tests: `Tests/Native/NativeRawHighlightsDefaults_Test.cpp` for the threshold default, its
override and the mode fallback; `Tests/Native/NativeRawHighlightsLaplacian_Test.cpp` for Guided
Laplacians (tiles against the full frame, untouched photosites away from clipping, the X-Trans and
linear fallbacks, knob defaults and visibility); `Tests/Native/NativeRawHighlights_Test.cpp`, on synthetic mosaics. They check Clip against
a hand computation, and check Inpaint Opposed and LCh for invariants and for tiles matching the
full frame bit for bit. The darktable-reference comparison per mode belongs to the reference
harness, which does not exist yet; `DISABLED_MatchesTheDarktableReference` is where it plugs in.

GPU: Clip Highlights (`rawhighlights_clip`, bound `ParityTolerance::exact()`) and Reconstruct in
LCh on a Bayer or X-Trans mosaic (`rawhighlights_lch`, bound `ParityTolerance::transcendental()`
for its square root and divisions), both wrapped kernels in `Engine/Gpu/Kernels/rawhighlights.slang`.
LCh reads the source through the wrap's view, which the node asks to be the window grown by 2 and
clipped to the source (`getOpGpuInputRoI()`). Reconstruct Color is a whole-frame scan and stays on the CPU, as do Segmentation Based and Guided Laplacians. Inpaint Opposed stays on the CPU: its chrominance is
a double-precision sum over the whole source, kept per frame under `RawNodeSupport::frameKey()`,
which neither a region's strips nor float sums on the device reproduce. LCh without a CFA is
Inpaint Opposed and stays on the CPU too. With either, the raw run the node is in stays on the CPU
(see `placeGraph()`). GPU tests: `Tests/Native/NativeRawHighlightsGpu_Test.cpp`, on a synthetic
clipped patch.

## darktable controls

Generated by `python3 -I tools/raw/parity-audit.py --write-docs` from
`tools/raw/darktable-parity.json`: every control of darktable's `highlights` and where it is here.

| darktable item | Natron knob/option | darktable default | Natron default | status |
| --- | --- | --- | --- | --- |
| param mode "method" | mode | DT_IOP_HIGHLIGHTS_OPPOSED | opposed | mapped |
| param blendL | N/A: no widget (unused in darktable) | 1.0 |  | N/A |
| param blendC | N/A: no widget (unused in darktable) | 0.0 |  | N/A |
| param strength "strength" | strength | 0.0 | 0.0 | mapped |
| param clip "clipping threshold" | clip | 1.0; at most the image's linear response limit (highlights.c:1255) | from the image | mapped |
| param noise_level "noise level" | noiseLevel | 0.0 | 0.0 | mapped |
| param iterations "iterations" | iterations | 30 | 30 | mapped |
| param scales "diameter of reconstruction" | scales | WAVELETS_7_SCALE | 128px | mapped |
| param candidating "candidating" | candidating | 0.4 | 0.4 | mapped |
| param combine "combine" | combine | 2.0 | 2.0 | mapped |
| param recovery "rebuild" | recovery | DT_RECOVERY_MODE_OFF | off | mapped |
| param solid_color "inpaint a flat color" | solidColor | 0.0 | 0.0 | mapped |
| enum DT_IOP_HIGHLIGHTS_OPPOSED "inpaint opposed" | mode=opposed | default |  | mapped |
| enum DT_IOP_HIGHLIGHTS_LCH "reconstruct in LCh" | mode=lch |  |  | mapped |
| enum DT_IOP_HIGHLIGHTS_CLIP "clip highlights" | mode=clip |  |  | mapped |
| enum DT_IOP_HIGHLIGHTS_SEGMENTS "segmentation based" | mode=segments |  |  | mapped |
| enum DT_IOP_HIGHLIGHTS_LAPLACIAN "guided laplacians" | mode=laplacian |  |  | mapped |
| enum DT_IOP_HIGHLIGHTS_INPAINT "reconstruct color" | mode=inpaint |  |  | mapped |
| enum WAVELETS_1_SCALE "2 px" | scales=2px |  |  | mapped |
| enum WAVELETS_2_SCALE "4 px" | scales=4px |  |  | mapped |
| enum WAVELETS_3_SCALE "8 px" | scales=8px |  |  | mapped |
| enum WAVELETS_4_SCALE "16 px" | scales=16px |  |  | mapped |
| enum WAVELETS_5_SCALE "32 px" | scales=32px |  |  | mapped |
| enum WAVELETS_6_SCALE "64 px" | scales=64px |  |  | mapped |
| enum WAVELETS_7_SCALE "128 px (slow)" | scales=128px | default |  | mapped |
| enum WAVELETS_8_SCALE "256 px (slow)" | scales=256px |  |  | mapped |
| enum WAVELETS_9_SCALE "512 px (very slow)" | scales=512px |  |  | mapped |
| enum WAVELETS_10_SCALE "1024 px (very slow)" | scales=1024px |  |  | mapped |
| enum WAVELETS_11_SCALE "2048 px (insanely slow)" | scales=2048px |  |  | mapped |
| enum WAVELETS_12_SCALE "4096 px (insanely slow)" | scales=4096px |  |  | mapped |
| enum DT_RECOVERY_MODE_OFF "off" | recovery=off | default |  | mapped |
| enum DT_RECOVERY_MODE_ADAPT "generic" | recovery=generic |  |  | mapped |
| enum DT_RECOVERY_MODE_ADAPTF "flat generic" | recovery=flatGeneric |  |  | mapped |
| enum DT_RECOVERY_MODE_SMALL "small segments" | recovery=small |  |  | mapped |
| enum DT_RECOVERY_MODE_LARGE "large segments" | recovery=large |  |  | mapped |
| enum DT_RECOVERY_MODE_SMALLF "flat small segments" | recovery=flatSmall |  |  | mapped |
| enum DT_RECOVERY_MODE_LARGEF "flat large segments" | recovery=flatLarge |  |  | mapped |
| gui mask view combine | DEFERRED: deferred to a future rich-viewer-overlays milestone |  |  | DEFERRED |
| gui mask view candidating | DEFERRED: deferred to a future rich-viewer-overlays milestone |  |  | DEFERRED |
| gui mask view strength | DEFERRED: deferred to a future rich-viewer-overlays milestone |  |  | DEFERRED |
| gui mask view clipped | DEFERRED: deferred to a future rich-viewer-overlays milestone |  |  | DEFERRED |
| | clipOverride | | | EXTRA: clip shows the file-derived default until turned on; darktable stores the value in its params instead |
| | mode=unclipped | | | EXTRA: an identity; darktable's equivalent is turning the module off |
