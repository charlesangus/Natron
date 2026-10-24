# RawHighlights

Tolerance: none recorded.

Port of darktable's highlight reconstruction (`src/iop/highlights.c` with
`hlreconstruct/opposed.c`, `lch.c`, `inpaint.c` and the reference average of `segbased.c`, darktable
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
  Reconstruct Color and Segmentation Based need a CFA, and Guided Laplacians, not yet ported, needs a
  Bayer mosaic.
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
- The guided Laplacian mode is not ported.
- It renders at full resolution only.

Tests: `Tests/Native/NativeRawHighlightsDefaults_Test.cpp` for the threshold default, its
override and the mode fallback; `Tests/Native/NativeRawHighlights_Test.cpp`, on synthetic mosaics. They check Clip against
a hand computation, and check Inpaint Opposed and LCh for invariants and for tiles matching the
full frame bit for bit. The darktable-reference comparison per mode belongs to the reference
harness, which does not exist yet; `DISABLED_MatchesTheDarktableReference` is where it plugs in.

GPU: Clip Highlights (`rawhighlights_clip`, bound `ParityTolerance::exact()`) and Reconstruct in
LCh on a Bayer or X-Trans mosaic (`rawhighlights_lch`, bound `ParityTolerance::transcendental()`
for its square root and divisions), both wrapped kernels in `Engine/Gpu/Kernels/rawhighlights.slang`.
LCh reads the source through the wrap's view, which the node asks to be the window grown by 2 and
clipped to the source (`getOpGpuInputRoI()`). Reconstruct Color is a whole-frame scan and stays on the CPU. Inpaint Opposed stays on the CPU: its chrominance is
a double-precision sum over the whole source, kept per frame under `RawNodeSupport::frameKey()`,
which neither a region's strips nor float sums on the device reproduce. LCh without a CFA is
Inpaint Opposed and stays on the CPU too. With either, the raw run the node is in stays on the CPU
(see `placeGraph()`). GPU tests: `Tests/Native/NativeRawHighlightsGpu_Test.cpp`, on a synthetic
clipped patch.
