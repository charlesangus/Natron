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
