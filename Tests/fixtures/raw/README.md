# Camera raw fixtures

Sample raw files for the native Read's rawspeed decoder and the raw nodes. They come from the
[raw.pixls.us](https://raw.pixls.us/) sample archive, which publishes every sample under the
[CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/) public domain dedication.

The files are large, so Git LFS tracks them (`.gitattributes`). A checkout without LFS gets
pointer files instead, and the tests that read them fail. Run `git lfs pull` to fetch them. CI
checkouts that run ctest or the smoke tests set `lfs: true`.

| File | Camera | Layout | Source | Licence | Size (bytes) | SHA-256 |
| --- | --- | --- | --- | --- | --- | --- |
| `canon-eos-5d-mark-iii.cr2` | Canon EOS 5D Mark III | Bayer CFA, CR2 | [raw.pixls.us/data/Canon/Canon EOS 5D Mark III](https://raw.pixls.us/data/Canon/Canon%20EOS%205D%20Mark%20III/) | CC0 1.0 | 25965009 | `ec069b178b9383d80c72f8cdc1b79bd54ca451e7c20793b109d8ad73bef6bdf6` |
| `fujifilm-x-pro2-14bit-uncompressed.raf` | Fujifilm X-Pro2 | X-Trans CFA, 14-bit uncompressed RAF | [raw.pixls.us/data/Fujifilm/X-Pro2](https://raw.pixls.us/data/Fujifilm/X-Pro2/) | CC0 1.0 | 50622976 | `8cc12566e5c0fc676b713bb1612ce0510d47b1dc691e9a5a0bcf3799ad9b8af1` |
| `canon-eos-5d-mark-iii-linear-rgb.dng` | Canon EOS 5D Mark III, converted by Adobe DNG Converter 9.8 | Linear (demosaiced) RGB DNG, no CFA | [raw.pixls.us/data/Canon/Canon EOS 5D Mark III](https://raw.pixls.us/data/Canon/Canon%20EOS%205D%20Mark%20III/) | CC0 1.0 | 63674890 | `357e664b2a59da2bfa1e6d729be7314e5efdb4ea8610b9c4fa37546437540686` |

The files are renamed from the archive's names. The SHA-256 identifies each one.

## darktable references

`reference/` holds darktable's output on these files and on two synthetic mosaics, which
`Tests/Native/RawReference_Test.cpp` and `Tests/Native/NativeRawHighlights_Test.cpp` compare the
raw nodes with. `tools/raw/make-reference.sh` makes all of them: it builds `darktable-cli` at
`DARKTABLE_REF` (`tools/ci/local/fetch-assets.sh`, darktable 5.6.2, `20891f6c6fa7`) in a
throwaway Docker image (`tools/raw/darktable-ref.Dockerfile`, Debian trixie, `-j4`, generic
`BINARY_PACKAGE_BUILD` code so the result does not depend on the build machine's CPU), then runs
`tools/raw/make-reference.py` in it, which writes a sidecar XMP per render, exports a 32-bit
float TIFF and crops it. Run it again only when `DARKTABLE_REF` moves; it needs the LFS files
checked out.

| File | What it is | Size |
| --- | --- | --- |
| `canon-eos-5d-mark-iii-{ppg,rcd,amaze}.tif` | The CR2 developed with PPG, RCD and AMaZE | 192x192 RGB float, ~400 KB, LFS |
| `fujifilm-x-pro2-markesteijn{,3}.tif` | The RAF developed with Markesteijn 1-pass and 3-pass | 192x192 RGB float, ~390 KB, LFS |
| `highlights-{bayer,xtrans}.dng` | Synthetic 48x42 RGGB and X-Trans mosaics with a clipped disc | ~4.5 KB |
| `highlights-{bayer,xtrans}-{clip,unclipped,opposed,lch}.tif` | Each highlights mode's white-balanced mosaic on them | 48x42 one-channel float, ~1 KB |

### darktable settings

Each render's history sets only what differs from darktable's per-image defaults, so rawprepare
(black and white levels, sensor crop) and temperature (as-shot white balance) come from the file
exactly as on import. The XMP marks the automatic presets as applied, which keeps exposure,
filmic or sigmoid, colour calibration and sharpen out, and `darktable-cli` runs with
`--apply-custom-presets false`, `--hq true`, `--icc-type LIN_REC709`, no OpenCL and
`plugins/darkroom/workflow=none`; with a scene-referred workflow temperature would default to
the D65 multipliers and leave the adaptation to colour calibration.

- flip: off (orientation none).
- highlights: inpaint opposed for the camera files (the crops have no clipped photosite, so the
  mode does not change them: a clip render of the same crops is identical); per mode for the
  synthetic mosaics, at darktable's defaults otherwise (clipping threshold 1).
- demosaic: the method under test; no green equilibration, colour smoothing, dual demosaic or
  capture sharpening.
- colorin: the standard matrix (rawspeed's camera matrix, which RawInputColor's "From File"
  uses; darktable's default "enhanced matrix" would pick its own tuned matrix for some cameras),
  no gamut clipping, no blue mapping, linear Rec. 709 working space.
- colorout: linear Rec. 709, matrix path (`force_lcms2=FALSE`). darktable's matrices are D50
  adapted with Bradford on both sides, so camera white maps to Rec. 709 white as it does in
  RawInputColor's camera to XYZ D65 matrix; the test converts Natron's output from the project's
  working space to linear Rec. 709 through the project's working-to-XYZ matrix.

The camera crops are 192x192 pixels at (3072, 1728) of the 5794x3868 CR2 and (3264, 3072) of the
6032x4032 RAF, counted from the top-left of darktable's developed image: textured areas away
from the borders, well under white. The test checks that RawPrepare keeps the same size before
it compares, and flips the rows, as Natron counts them from the bottom.

What darktable 5.6.2 cannot do, and the workarounds:

- It has no unclipped highlights mode: `highlights-*-unclipped.tif` is the highlights module
  turned off, which leaves the data alone as RawHighlights' Unclipped does.
- It cannot export a mosaic. The highlights renders use the "photosite color (debug)" demosaic,
  which puts each photosite in its own channel and zero in the others, and colorin with a linear
  Rec. 709 input profile, which with a linear Rec. 709 working and output profile is the
  identity (the other channels stay under 6e-7). The script keeps each photosite's own channel.
- colorin cannot be turned off (its enable button is hidden), hence the identity profile above.
- `USE_LIBRAW=OFF` does not build at this revision, so the image builds the bundled LibRaw it
  never uses for these files. Ubuntu's archive is not reachable from the sandboxes this was made
  in, hence Debian.
- No floating-point TIFF predictor without Python's `imagecodecs`, which Debian does not package,
  so the crops are zlib without a predictor and need LFS.

### Tolerances

`RawReference_Test` reports the mean and the largest absolute difference over the three
channels of the crop, both relative to the reference's mean value there, and bounds each method:

| Method | Mean | Max |
| --- | --- | --- |
| PPG, RCD | 0.1 % | 2 % |
| AMaZE, Markesteijn 1-pass and 3-pass | 0.3 % | 5 % |

A faithful port differs from darktable only by floating-point evaluation order and by the colour
matrices' rounding (darktable goes through Lab and its own hard-coded profile matrices, Natron
through the OCIO config), which should stay around 1e-5 relative. The bounds sit well above that and well under
what a different algorithm gives on the same crops, measured between darktable's own methods:

| darktable against darktable | Mean | Max |
| --- | --- | --- |
| PPG against RCD | 1.6 % | 32 % |
| AMaZE against RCD | 1.4 % | 31 % |
| PPG against AMaZE | 1.9 % | 44 % |
| Markesteijn 1-pass against 3-pass | 0.9 % | 55 % |

AMaZE and Markesteijn get the looser bound because their ports carry more float-sensitive steps
(AMaZE's many divisions and clamps near zero, Markesteijn's homogeneity maps, whose ties can pick
a different direction for a pixel and move it by a few per cent), which a 0.1 % mean would turn
into failures that are not bugs.

`NativeRawHighlightsTest.MatchesTheDarktableReference` compares every photosite of the
white-balanced mosaic after RawPrepare, RawTemperature and RawHighlights with darktable's, in
units of the white level: 1e-5 for clip and unclipped, which are a multiply and a minimum; 2e-3
for inpaint opposed, whose chrominance offsets darktable sums in float over the whole image;
1e-3 for LCh. Against the unclipped mosaic, clip changes 97 (Bayer) and 84 (X-Trans) photosites
by up to 1, opposed 104 and 115 by up to 0.74, LCh 173 and 134 by up to 0.54.
