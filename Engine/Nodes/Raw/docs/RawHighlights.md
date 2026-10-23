# RawHighlights

Tolerance: none recorded.

Port of darktable's highlight reconstruction (`src/iop/highlights.c` with
`hlreconstruct/opposed.c`, `lch.c` and the reference average of `segbased.c`, darktable
`release-5.6.2`).

- Raw in, raw out, at the prepared stage.
- **Clipping Threshold** is multiplied by `raw/white_level`, which is 1 after RawPrepare.
- **Clip Highlights** clips every photosite to the threshold, whatever its colour, which turns
  clipped areas white.
- **Unclipped** is an identity.
- **Inpaint Opposed** is the default, as in darktable. Its threshold is 0.987 x the clipping
  threshold x the `raw/wb_applied` multiplier of each colour. A clipped photosite becomes the
  larger of itself and the cube of the mean cube roots of the opposed colours in its 3x3
  neighbourhood, plus a chrominance offset. That offset is measured over the whole image on the
  unclipped photosites next to clipped ones. The node reads the whole source for it, once per
  source and settings, so every tile agrees. Linear data gets darktable's linear variant.
- **Reconstruct in LCh** works on Bayer and X-Trans mosaics with a 2-photosite halo, as darktable's
  `process_lch_bayer()` and `process_lch_xtrans()` do. On data without a CFA it is Inpaint
  Opposed, as in darktable.
- Image borders are those of the whole source, never of a tile. darktable's X-Trans LCh keeps a
  ring buffer of clipped columns, which is evaluated directly here with the same result.
- Clip and LCh set `raw/processed_maximum` the way darktable updates its processed maximum.
- The segmentation, guided Laplacian and inpaint-colour modes are not ported.
- It renders at full resolution only.

Tests: `Tests/Native/NativeRawHighlights_Test.cpp`, on synthetic mosaics. They check Clip against
a hand computation, and check Inpaint Opposed and LCh for invariants and for tiles matching the
full frame bit for bit. The darktable-reference comparison per mode belongs to the reference
harness, which does not exist yet; `DISABLED_MatchesTheDarktableReference` is where it plugs in.

GPU: Clip Highlights (`rawhighlights_clip`, bound `ParityTolerance::exact()`) and Reconstruct in
LCh on a Bayer or X-Trans mosaic (`rawhighlights_lch`, bound `ParityTolerance::transcendental()`
for its square root and divisions), both wrapped kernels in `Engine/Gpu/Kernels/rawhighlights.slang`.
LCh reads the source through the wrap's view, which the node asks to be the window grown by 2 and
clipped to the source (`getOpGpuInputRoI()`). Inpaint Opposed stays on the CPU: its chrominance is
a double-precision sum over the whole source, kept per frame under `RawNodeSupport::frameKey()`,
which neither a region's strips nor float sums on the device reproduce. LCh without a CFA is
Inpaint Opposed and stays on the CPU too. With either, the raw run the node is in stays on the CPU
(see `placeGraph()`). GPU tests: `Tests/Native/NativeRawHighlightsGpu_Test.cpp`, on a synthetic
clipped patch.
