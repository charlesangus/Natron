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

`reference/` holds darktable's output on these files and on five synthetic mosaics, which
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
| `linear-response-limit-bayer.dng`, `gainmap-bayer.dng`, `hotpixels-bayer.dng` | The Bayer disc mosaic with `LinearResponseLimit` 0.8; a clip-free ramp with an OpcodeList2 GainMap per CFA cell; the ramp with hot photosites | ~4.5 KB |
| everything else below | One job each, named `<file>-<module>-<variant>.tif` | 192x192 RGB float crops, LFS; 48x42 one-channel float mosaics |

Every reference and the tolerance class `RawReference_Test` or `NativeRawHighlights_Test` compares
it at (see Tolerances; a crop class names a row of the first table there, a mosaic class is in
units of the white level). `cr2` is `canon-eos-5d-mark-iii`, `raf` is `fujifilm-x-pro2`, `dng`
is `canon-eos-5d-mark-iii-linear-rgb`. Unless a row says otherwise a CR2 job demosaics with RCD
and a RAF job with Markesteijn 1-pass (darktable's defaults), the DNG skips demosaic, and the
input profile is the standard matrix (the embedded one for the DNG).

| Reference | Settings | Class |
| --- | --- | --- |
| `cr2-{ppg,rcd}.tif` | PPG, RCD | RCD |
| `cr2-amaze.tif` | AMaZE | AMaZE |
| `raf-markesteijn{,3}.tif` | Markesteijn 1-pass, 3-pass | Markesteijn |
| `cr2-vng4.tif` | VNG4 | AMaZE |
| `cr2-lmmse-refine-{0..4}.tif` | LMMSE, each refine level | AMaZE |
| `cr2-{rcd,amaze}-dual-{0.2,0.6}.tif` | RCD and AMaZE dual, dual threshold 0.2 and 0.6 | AMaZE |
| `cr2-ppg-median-0.3.tif` | PPG, edge threshold (median_thrs) 0.3 | RCD |
| `cr2-rcd-greeneq-{local,full,both}.tif` | RCD, match greens local, full, full and local | RCD |
| `cr2-rcd-smoothing-{1,5}.tif` | RCD, colour smoothing once and five times | RCD |
| `cr2-passthrough-{mono,colour}.tif` | Bayer passthrough (monochrome), photosite colour (debug) | RCD |
| `raf-{vng,fdc,markesteijn3-dual}.tif` | X-Trans VNG, frequency domain chroma, Markesteijn 3-pass dual (threshold 0.2) | Markesteijn |
| `raf-passthrough-{mono,colour}.tif` | X-Trans passthrough (monochrome), photosite colour (debug) | RCD |
| `{cr2,raf}-capture-sharpen-r0.7-t0.3.tif` | Capture sharpening, radius 0.7, contrast threshold 0.3, other controls at their defaults | AMaZE |
| `{cr2,raf}-capture-sharpen-auto-t0.3.tif` | The same with radius 0, which darktable-cli computes; the `-d pipe` "bayer/xtrans autoradius" line is in the TIFF description and the log | AMaZE |
| `{cr2,raf}-temperature-as-shot.tif` | White balance as shot (no temperature history entry, so darktable's own as-shot coefficients) | RCD (CR2), Markesteijn (RAF) |
| `{cr2,raf}-temperature-camera-reference.tif` | Camera reference (preset 3) with the probed D65 coefficients | RCD, Markesteijn |
| `{cr2,raf}-temperature-as-shot-to-reference.tif` | As shot to reference (preset 4) with the probed as-shot coefficients, so colorin applies the late D65 correction | RCD, Markesteijn |
| `{cr2,raf}-temperature-user.tif` | User coefficients 2.2, 1, 1.6 | RCD, Markesteijn |
| `{cr2,raf}-hotpixels-{default,permissive,strength-1}.tif` | Hot pixels at its defaults (strength 0.25, threshold 0.05), detect by 3 neighbours, strength 1 | RCD, Markesteijn |
| `cr2-cacorrect-{iterations-1,iterations-2,iterations-5,avoidshift}.tif` | Raw CA correction 1, 2 and 5 iterations; avoid colour shift at 2 | AMaZE |
| `{cr2,raf}-rawdenoise-0.05-{flat,shaped}.tif` | Raw denoise threshold 0.05, flat curves; the all-channels curve 0.9, 0.7, 0.5, 0.3, 0.1 | RCD, Markesteijn |
| `{cr2,raf}-colorin-standard.tif` | Standard matrix | RCD, Markesteijn |
| `cr2-colorin-enhanced.tif` | Enhanced matrix (`colormatrices.c` lists the 5D Mark III among the enhanced matrices only, the X-Pro2 nowhere, so there is no vendor or alternate job) | RCD |
| `cr2-colorin-{srgb,adobergb,lin-rec709,rec709,lin-rec2020,pq-rec2020,hlg-rec2020,pq-p3,hlg-p3,display-p3,lin-prophoto,xyz,lab,infrared,brg}.tif` | Each generic input profile | RCD |
| `cr2-colorin-gamut-{srgb,adobergb,lin-rec709,lin-rec2020}.tif` | Standard matrix with each gamut clipping | RCD |
| `dng-colorin-embedded.tif` | Embedded matrix | RCD |
| `highlights-{bayer,xtrans}-segments-rebuild-{off,generic,small}.tif` | Segmentation based, rebuild off, generic and small segments (strength 0.5 when on) | 2e-3 |
| `highlights-{bayer,xtrans}-laplacian.tif`, `-laplacian-i64-n0.1-s0.5.tif` | Guided laplacians at its defaults; 64 iterations, noise level 0.1, inpaint a flat colour 0.5 | 2e-3 |
| `highlights-{bayer,xtrans}-inpaint.tif` | Reconstruct color | 1e-3 |
| `linear-response-limit-bayer-clip.tif` | Clip highlights at its default threshold, which the 0.8 limit lowers | 1e-5 |
| `gainmap-bayer-flat-field-{on,off}.tif` | rawprepare's flat field correction (embedded GainMap) on and off, highlights off | 1e-5 |
| `hotpixels-bayer-{default,permissive,strength-1}.tif` | Hot pixels as on the camera files, highlights off | 1e-5 |
| `{cr2,raf}-colorcal-adaptation-{bradford,cat16,full-bradford,xyz}.tif` | Color calibration, illuminant D 4500 K, each adaptation | RCD, Markesteijn |
| `{cr2,raf}-colorcal-adaptation-none.tif` | Adaptation none (bypass) with the R/G/B mix (0.9, 0.2, -0.05), (0.1, 0.85, 0.1), (-0.05, 0.15, 1.1) | RCD, Markesteijn |
| `{cr2,raf}-colorcal-illuminant-{pipe,a,e,f2,f11,led-b1,led-rgb1,planckian-3200,custom-xy}.tif` | CAT16 with each illuminant; Planckian at 3200 K, custom at xy 0.36, 0.37 | RCD, Markesteijn |
| `{cr2,raf}-colorcal-camera.tif`, `-camera-preset-4.tif` | CAT16, as shot in camera, white balance as shot and as shot to reference (preset 4) | RCD, Markesteijn |
| `{cr2,raf}-colorcal-gamut-{0,1,4}-clip-{on,off}.tif` | D 4500 K CAT16, gamut compression 0, 1, 4, clip negative RGB on and off | RCD, Markesteijn |
| `{cr2,raf}-colorcal-mix-normalize-{on,off}.tif` | D 4500 K CAT16, the R/G/B mix above, normalize on and off | RCD, Markesteijn |
| `{cr2,raf}-colorcal-colorfulness-{plus,minus}-v{1,2,3}.tif` | D 4500 K CAT16, colorfulness red +0.5 and -0.5, each saturation algorithm | RCD, Markesteijn |
| `{cr2,raf}-colorcal-brightness.tif` | D 4500 K CAT16, brightness 0.4, 0.2, -0.3 | RCD, Markesteijn |
| `{cr2,raf}-colorcal-gray-normalize-{on,off}.tif` | D 4500 K CAT16, gray 0.4, 0.6, 0.3, normalize on and off | RCD, Markesteijn |
| `dng-colorcal-camera.tif` | CAT16, as shot in camera, embedded matrix | RCD |
| `{cr2,raf,dng}-scene-referred-default.tif` | `scene-referred (sigmoid)` workflow with its automatic presets, sigmoid, exposure and sharpen off: darktable's default temperature (as shot to reference) and color calibration | RCD, Markesteijn, RCD |

### darktable settings

Each render's history sets only what differs from darktable's per-image defaults, so rawprepare
(black and white levels, sensor crop) and temperature (as-shot white balance) come from the file
exactly as on import, unless the job sets them (the flat field jobs set rawprepare; the
temperature and color calibration jobs set temperature). The XMP marks the automatic presets as applied, which keeps exposure,
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
- colorin: the standard color matrix (rawspeed's camera matrix), no gamut clipping, no blue
  mapping, linear Rec. 709 working space, unless the job says otherwise. The params struct says
  `$DEFAULT: DT_COLORSPACE_ENHANCED_MATRIX`, but `colorin.c` `reload_defaults` (~1904) overrides it
  per image: the embedded matrix for a file that carries one (a DNG), the standard matrix for
  other raws. RawInputColor defaults the same way.
- temperature, hotpixels, cacorrect, rawdenoise, rawprepare, channelmixerrgb: the job's settings,
  packed at the version each module's `DT_MODULE_INTROSPECTION` gives (temperature 4, hotpixels
  1, cacorrect 2, rawdenoise 2, rawprepare 2, channelmixerrgb 3). The color calibration jobs set
  temperature explicitly, as darktable would otherwise switch its default to as shot to reference
  once color calibration is in the history.
- colorout: linear Rec. 709, matrix path (`force_lcms2=FALSE`). darktable's matrices are D50
  adapted with Bradford on both sides, so camera white maps to Rec. 709 white as it does in
  RawInputColor's camera to XYZ D65 matrix; the test converts Natron's output from the project's
  working space to linear Rec. 709 through the project's working-to-XYZ matrix.

The camera crops are 192x192 pixels at (3072, 1728) of the 5794x3868 CR2 and (3264, 3072) of the
6032x4032 RAF, counted from the top-left of darktable's developed image: textured areas away
from the borders, well under white. The test checks that RawPrepare keeps the same size before
it compares, and flips the rows, as Natron counts them from the bottom.

Coefficients and logged values:

- A job that packs the as-shot or D65 white balance needs darktable's coefficients for the file,
  which only its log gives: `make-reference.py` first exports each camera file at 256 px with
  `-d params` and reads temperature's `[dt_iop_reload_defaults] ... D65 r g b, AS-SHOT r g b`
  line. It prints them to three decimals, so those jobs carry coefficients rounded to 1e-3
  (2.5e-4 relative at worst on these files, under the RCD class's mean bound). Each such TIFF's
  description gives the coefficients it used; a test of those jobs sets RawTemperature to them.
- The automatic capture sharpening jobs run with `-d pipe`, and the scene-referred default jobs
  with `-d params`. The script keeps their "bayer/xtrans autoradius" and "[commit color
  calibration] temp= xy=" lines in the TIFF description and prints them, and
  `make-reference.sh` keeps its output in `build/raw-reference/make-reference.log`. Those lines,
  recorded here once rendered, are what RawDemosaic's automatic radius and RawColorCalibration's
  defaults are checked against.
- The scene-referred default jobs mark the automatic presets as not applied, so darktable puts
  the workflow's presets (exposure, sigmoid, color calibration) first in the history and the
  job's entries, which turn sigmoid, exposure and sharpen off, after them. If the log shows no
  "[commit color calibration]" line for them, darktable-cli did not apply the presets: pack the
  illuminant the as-shot color calibration jobs log explicitly instead.

What darktable 5.6.2 cannot do, and the workarounds:

- darktable-cli cannot take a white balance from a picked spot or from a camera's
  `wb_presets.json` entry (both need the GUI), so there is no job for either.

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
channels of the crop, both relative to the reference's mean value there. It bounds the mean, and
the number of pixels whose largest channel difference is over the max bound:

| Method | Mean | Max | Pixels allowed over max |
| --- | --- | --- | --- |
| PPG, RCD | 0.1 % | 2 % | 0 |
| AMaZE | 0.3 % | 5 % | 0 |
| Markesteijn 1-pass and 3-pass | 0.3 % | 5 % | 36 (0.1 % of the crop) |

The other crops take one of these three classes, named after the row (RCD, AMaZE, Markesteijn),
as the reference table above gives: a module's job takes the class of the demosaic it runs on,
and AMaZE's where the module itself adds float-sensitive steps (capture sharpening's iterated
deconvolution, CA correction's fits, the dual and the VNG and LMMSE demosaics); the X-Trans
demosaics take Markesteijn's.

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

Markesteijn averages the directions whose 5x5 homogeneity count reaches the largest count less
an eighth of it, all integers, and each count is a sum of `drv <= 8 * min(drv)` comparisons. One
comparison flipping by a rounding error moves the threshold or a count by one, which adds or
drops a direction and moves that pixel by up to about 17 % of the crop's mean level. On the RAF
crop, scaling every photosite by a random factor within 1e-5 of 1 moves 8 to 17 pixels of the
3-pass result and 3 to 9 of the 1-pass result over 5 %, where the darktable 1-pass and 3-pass
references differ by over 5 % at 4619 pixels. Natron's develop chain does not reproduce
darktable's mosaic bit for bit, and at `DARKTABLE_REF` it puts one 3-pass pixel, (95, 3) from the
top-left, 16.7 % away. The port run outside Natron on a mosaic rebuilt from the RAF gives
darktable's value there, and the 1e-5 perturbation moves that pixel by 14 %: the direction with
the count 108 drops out when the largest count goes from 123 to 124.

`NativeRawHighlightsTest.MatchesTheDarktableReference` compares every photosite of the
white-balanced mosaic after RawPrepare, RawTemperature and RawHighlights with darktable's, in
units of the white level: 1e-5 for clip and unclipped, which are a multiply and a minimum; 2e-3
for inpaint opposed, whose chrominance offsets darktable sums in float over the whole image;
1e-3 for LCh. The other synthetic mosaics take these classes too: 2e-3 for segmentation and
guided laplacians (float sums and iterated wavelets), 1e-3 for reconstruct color, 1e-5 for the
LinearResponseLimit clip, the GainMap flat field and hot pixels, which multiply or replace
photosites. Against the unclipped mosaic, clip changes 97 (Bayer) and 84 (X-Trans) photosites
by up to 1, opposed 104 and 115 by up to 0.74, LCh 173 and 134 by up to 0.54.
