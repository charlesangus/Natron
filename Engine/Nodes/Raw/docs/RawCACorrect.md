# RawCACorrect

Tolerance: none recorded.

Port of darktable's raw chromatic aberrations module (`src/iop/cacorrect.c`, after RawTherapee's
`CA_correct_RT.cc`, darktable `release-5.6.2`), and of the C path of `dt_gaussian_blur()`
(`src/common/gaussian.c`) for the colour shift option. The kernel is `RawKernels::caCorrect()`
in `RawCACorrectKernel.cpp`.

- Raw in, raw out, at the prepared stage. Any other stage is a persistent error.
- Bayer only, as in darktable. X-Trans mosaics and data without a CFA pass through unchanged,
  with a persistent warning.
- **Iterations**: once to five times, twice by default. Each pass measures what the previous one
  left.
- **Avoid Colour Shift**: measures the ratio of red and blue before and after the correction,
  blurs it (sigma 30 on the half-resolution grid) and divides it out.
- Values are divided by the processed maximum (`raw/processed_maximum`) for the measurement and
  multiplied back, as darktable does with its pipe's.
- **Whole frame.** The shifts are measured in 128-photosite blocks over the whole image, then fit
  with a polynomial over the block grid, so every render reads the whole source.
  - The corrected mosaic is computed once per source and settings, and every tile is cut from it.
    It is kept under the node's render hash with the time and view
    (`RawNodeSupport::frameKey()`), the same key RawDemosaic and RawHighlights cache their
    whole-image measurements under.
  - One render computes it while the others wait on the same lock, so a frame is never measured
    twice. A cancelled computation is not kept.
  - The cache holds one frame, a float per photosite. darktable's measurement feeds each
    iteration from the previous one's whole corrected image, so caching only the fitted
    shifts would still mean recomputing every iteration over the whole frame for each tile.
- An image smaller than 32 photosites on a side, or with too few blocks with detail to fit
  (fewer than 10 usable blocks), passes through with a persistent warning. darktable leaves it
  unchanged the same way.
- darktable sums the block statistics per thread in thread order. Here they are summed per block
  in a fixed order, so the result does not depend on the thread count, and may differ from
  darktable's in the last bits. The tiles that share interpolated greens run in four groups that
  never overlap, for the same reason.
- RawTherapee keeps the polynomial order between iterations and, when fewer than 32 blocks are
  usable, solves the first 4x4 entries of the 16x16 system it accumulated. Both are kept as in
  darktable. A real photograph has hundreds of blocks; the test image is sized to have more
  than 32.
- It renders at full resolution only.

Tests: `Tests/Native/NativeRawCACorrect_Test.cpp`. A synthetic RGGB mosaic with red magnified and
blue shrunk by 0.3% about the centre has its red and blue error to the scene at least halved
(about 4x measured), green unchanged, and tiles equal to the full frame. Changing the settings
measures again. X-Trans, linear, small and flat data pass through with a warning.
