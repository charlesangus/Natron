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
| `{cr2,raf}-denoiseprofile-{wavelets-y0u0v0,wavelets-rgb,wavelets-auto,nlm,nlm-auto}.tif` | Denoise (profiled) after the demosaic, auto profile, highlight preservation compensated, each mode; wavelets in Y0U0V0 and RGB; other controls at their defaults, curves flat at 0.5 | AMaZE, Markesteijn |
| `{cr2,raf}-denoiseprofile-nlm-p2-k4-s0.5.tif` | Non-local means, patch size 2, search radius 4, scattering 0.5, central pixel weight 0.3 | AMaZE, Markesteijn |
| `{cr2,raf}-denoiseprofile-wavelets-{shadows-0.5,bias-minus-2,strength-2}.tif` | Wavelets Y0U0V0 with preserve shadows 0.5, bias correction -2, strength 2 | AMaZE, Markesteijn |
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
- temperature, hotpixels, cacorrect, rawdenoise, denoiseprofile, rawprepare, channelmixerrgb: the
  job's settings, packed at the version each module's `DT_MODULE_INTROSPECTION` gives (temperature
  4, hotpixels 1, cacorrect 2, rawdenoise 2, denoiseprofile 12, rawprepare 2, channelmixerrgb 3).
  denoiseprofile packs `a[0] = -1`, so darktable looks up the profile for the file as the node's
  auto profile does; in its non-auto modes darktable uses the packed patch size, scattering,
  shadows and bias rather than the values it infers from the profile, so the test sets them. The color calibration jobs set
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

### Recorded values

Rendered once with the image at `DARKTABLE_REF` (darktable 5.6.2, `20891f6c6fa7`); a second render
of `canon-eos-5d-mark-iii-colorin-xyz.tif` and `highlights-bayer-lch.tif` reproduced them bit for bit.

Probed white balance coefficients (`-d params`, rounded to 1e-3 by darktable's log):

| File | D65 | As shot |
| --- | --- | --- |
| `cr2` | 2.251 1.000 1.418 | 1.649 1.000 2.165 |
| `raf` | 2.293 1.000 1.612 | 1.791 1.000 2.202 |
| `dng` | 1.605 1.000 0.526 | 1.165 1.000 0.821 |

Capture sharpening autoradius (`-d pipe`, radius 0, contrast threshold 0.3):

- `cr2`: `bayer autoradius ... sc=1.000; new radius=0.61 is reliable at (1158/773) 3477x2321`
- `raf`: `xtrans autoradius ... sc=1.000; new radius=0.86 is reliable at (1206/806) 3619x2419`

Color calibration defaults of the scene-referred default jobs (`-d params`). darktable-cli applied
the workflow presets, so no explicit illuminant was packed:

| File | `[commit color calibration]` |
| --- | --- |
| `cr2` | `temp=3222  xy=0.4251 0.4102 - XYZ=1.0364 1.0000 0.4014 - LMS=1.0454 0.9635 0.4294  DT_ILLUMINANT_CUSTOM` |
| `raf` | `temp=3678  xy=0.3989 0.3969 - XYZ=1.0049 1.0000 0.5144 - LMS=1.0270 0.9765 0.5372  DT_ILLUMINANT_CUSTOM` |
| `dng` | `temp=3182  xy=0.4280 0.4123 - XYZ=1.0381 1.0000 0.3872 - LMS=1.0468 0.9624 0.4159  DT_ILLUMINANT_CUSTOM` |

The linear DNG's crop at (2100, 1180) of its 3960x2640 image is textured (green channel 5th, 50th
and 95th percentiles 0.003, 0.012, 0.085; standard deviation 0.05 against a mean of 0.03) and well
under white (largest value 0.43 after `colorin-embedded`), so it stays.

SHA-256 of every file in `reference/`:

| File | Size (bytes) | SHA-256 |
| --- | --- | --- |
| `canon-eos-5d-mark-iii-amaze-dual-0.2.tif` | 401358 | `fefe3e16e96b48a7ecad407cf5f7134b3856f801867d43b5ef7cbb126347617f` |
| `canon-eos-5d-mark-iii-amaze-dual-0.6.tif` | 401340 | `343f51aebda7c2cd57f34ed19f6267e12d09ed02464760586763daf6108d6e1b` |
| `canon-eos-5d-mark-iii-amaze.tif` | 401718 | `0c9458b3f0c50147f6ece096300071bf1a6ec093745b1a7b2f4ad31f2195aa1a` |
| `canon-eos-5d-mark-iii-cacorrect-avoidshift.tif` | 401507 | `60841b5a3f43bfcfe5f42b97ef1ac140651784b62e302ab49b21dd6338820c7e` |
| `canon-eos-5d-mark-iii-cacorrect-iterations-1.tif` | 401725 | `e978e3ae39b14c224017edbaef7f4dbe5fa3e2dfa57a06b50eb1dbc4d892aa50` |
| `canon-eos-5d-mark-iii-cacorrect-iterations-2.tif` | 401495 | `0b725aed946a8f4d48244ad1a62c0e24f63bcd6d0deb0f629c489785fc8b05fa` |
| `canon-eos-5d-mark-iii-cacorrect-iterations-5.tif` | 401489 | `6346eeb471b53268a35815fbb15dfc77038c5dbbd0821b3218d88b0ddafb3dc4` |
| `canon-eos-5d-mark-iii-capture-sharpen-auto-t0.3.tif` | 402162 | `86506865f2173c2eb0c528e80489f965a56bcb1953ed327ae21a608befc996bb` |
| `canon-eos-5d-mark-iii-capture-sharpen-r0.7-t0.3.tif` | 402048 | `15c94162f0769ef4cbb5a64aa6de7f887ed9b8672ae185784758f76e895d2c9b` |
| `canon-eos-5d-mark-iii-colorcal-adaptation-bradford.tif` | 400981 | `990169d410329c3f2788c38b601f6bc23a691016b0897c06f37e45d479f1d9ca` |
| `canon-eos-5d-mark-iii-colorcal-adaptation-cat16.tif` | 400939 | `4fb14fedb6802e184442675e744ceb6a8b6f61da74c242520b286d6d35a61982` |
| `canon-eos-5d-mark-iii-colorcal-adaptation-full-bradford.tif` | 401000 | `6543987f50d77f76e14d6287bf60085c6d19194a172715c6a4ee9b3dc437a151` |
| `canon-eos-5d-mark-iii-colorcal-adaptation-none.tif` | 401805 | `0df9abf549d37327f61d9e1dab3bdbc6ebf79ab16021eb49fe7d6b600fea661c` |
| `canon-eos-5d-mark-iii-colorcal-adaptation-xyz.tif` | 401247 | `1f0c6b568213b9f257f8d69db595d2451d81ad91a9c74242bff4dff281ac8db3` |
| `canon-eos-5d-mark-iii-colorcal-brightness.tif` | 400604 | `aebe1f8f277c7f974847fe79f65bb8949a3124b333fa59e9723fd3c388ce348a` |
| `canon-eos-5d-mark-iii-colorcal-camera-preset-4.tif` | 405966 | `fcbdd7ecfa5346ec5cf448b80dc179a757e9556e78e8380c7cb2823dbd256fa4` |
| `canon-eos-5d-mark-iii-colorcal-camera.tif` | 401788 | `2edab6f63e1b4ad64bb38216f4edc6d864649918c965d90955953d14f1eff548` |
| `canon-eos-5d-mark-iii-colorcal-colorfulness-minus-v1.tif` | 399532 | `b165042cc561de8e57f827134e394af77d307d5195306dbc21755b9adb5df1d9` |
| `canon-eos-5d-mark-iii-colorcal-colorfulness-minus-v2.tif` | 400320 | `06fd4589390a6cd7a66efa81e752496ff2585acd8d32d553b8c8b13c1b8e5d40` |
| `canon-eos-5d-mark-iii-colorcal-colorfulness-minus-v3.tif` | 398291 | `947669416a85a0612cd5d5f06081c8a0b1ce71a2778fdf27cfb51360782ef05e` |
| `canon-eos-5d-mark-iii-colorcal-colorfulness-plus-v1.tif` | 392809 | `ba67d9aa2185941004c7cd52ea99f7cf7e4eb0e79d23d9816b5b563db7fb2892` |
| `canon-eos-5d-mark-iii-colorcal-colorfulness-plus-v2.tif` | 399745 | `6c00e7ac173813480044d53784b60295b2b67dccaa1ec97fcb874fb712701f21` |
| `canon-eos-5d-mark-iii-colorcal-colorfulness-plus-v3.tif` | 401635 | `2b5c2f7e97bf85f3982add57e6cec4863de59b990728d8eb8b663a872e6baee6` |
| `canon-eos-5d-mark-iii-colorcal-gamut-0-clip-off.tif` | 400942 | `34da59cd99b0ea8a4a762dc0e0ad75f52f96bf82c20c31d807c18fd1a868a7eb` |
| `canon-eos-5d-mark-iii-colorcal-gamut-0-clip-on.tif` | 400939 | `328fd5fb8622510ba6f0020dfac4ec28e9283ef41768de4034d28c6e44d97e1d` |
| `canon-eos-5d-mark-iii-colorcal-gamut-1-clip-off.tif` | 400972 | `965c7cd93a177ebeb1780ce88815d7c60f794a4ebdbc9ac91be569dd7f6fe6e7` |
| `canon-eos-5d-mark-iii-colorcal-gamut-1-clip-on.tif` | 400971 | `657fd05b14d298bd27002cf2303bb02ad7d929fe34f3c1c829dae4fb320b24bd` |
| `canon-eos-5d-mark-iii-colorcal-gamut-4-clip-off.tif` | 399898 | `083217a0ca49d63ffb6693378e925f5c6c27077efe1cfd20897aac52543fde35` |
| `canon-eos-5d-mark-iii-colorcal-gamut-4-clip-on.tif` | 399898 | `06982cb1b33cd64528f9c44f046b0f4cae970a2a55c3e4fc17c926337f3ca730` |
| `canon-eos-5d-mark-iii-colorcal-gray-normalize-off.tif` | 239829 | `7fed7bdbadf953035d145bf2b53b9c39951e4974b1591afbf8f52cbe299d816d` |
| `canon-eos-5d-mark-iii-colorcal-gray-normalize-on.tif` | 238919 | `33de8b81b93997315f5fd7fe4074b22e090ff0689635d7f7e083ba3912d596fa` |
| `canon-eos-5d-mark-iii-colorcal-illuminant-a.tif` | 400044 | `02f4a12033c5cf8a17d8133fc110d0dd2f5f77d75e3fb763ea18fb8cc1c60f0f` |
| `canon-eos-5d-mark-iii-colorcal-illuminant-custom-xy.tif` | 401403 | `ac7ce8d8c913c275b11df8660ce884755c969dafd8afa3289c5e5bc15b8ac286` |
| `canon-eos-5d-mark-iii-colorcal-illuminant-e.tif` | 402217 | `d64416c923e72ae0d587654dc33ac4cefb7fdb7a0887353481e9f10e66213375` |
| `canon-eos-5d-mark-iii-colorcal-illuminant-f11.tif` | 398733 | `c8009e31f4a91bb84e833ebaa74583fd696b40074b222933f467826e887a7b65` |
| `canon-eos-5d-mark-iii-colorcal-illuminant-f2.tif` | 399182 | `b7854103a01fbdc5e69d88f74b3c2c8ee07a6c242e329053b24ac8f5cc85fef8` |
| `canon-eos-5d-mark-iii-colorcal-illuminant-led-b1.tif` | 400511 | `5aa580b901456cc84e5ceb7264064cb582a431eef3c921d348173bef07154ff5` |
| `canon-eos-5d-mark-iii-colorcal-illuminant-led-rgb1.tif` | 400416 | `9b612a3cc855ca0a69e72a5b7a156affe8c76b5555436a867d1ef9e981bb4b2f` |
| `canon-eos-5d-mark-iii-colorcal-illuminant-pipe.tif` | 401666 | `6537223bc273553a68ba0e66337daf230c389aade87d343768b7e0518f3334c7` |
| `canon-eos-5d-mark-iii-colorcal-illuminant-planckian-3200.tif` | 397562 | `7898fbeeacf50c2b80830a5f4a51d0e1b97507834f24cb8f4433b48a99c877f3` |
| `canon-eos-5d-mark-iii-colorcal-mix-normalize-off.tif` | 400779 | `bb39c26bcf91fa420abaf4270578f054d024f600bdae859e0fa3b81d6550c8c6` |
| `canon-eos-5d-mark-iii-colorcal-mix-normalize-on.tif` | 401548 | `3e89ea07fd2c5af53847e8601bba8c14823438246d5f1c894639bff8ac9cc7d9` |
| `canon-eos-5d-mark-iii-colorin-adobergb.tif` | 410044 | `0e8e5bb7e4ba662d41cd5920a1b9f005d3b4a052d558d5444fe8616729dde394` |
| `canon-eos-5d-mark-iii-colorin-brg.tif` | 397073 | `e6f471320a49ce6668d1de3ff8050dfa79e549a650c1dee9ad704db089938b35` |
| `canon-eos-5d-mark-iii-colorin-display-p3.tif` | 402774 | `8aaeb44d8c2956b538208e8ce1f8534ae0cf9860762fae33c90fadd8f57a67ea` |
| `canon-eos-5d-mark-iii-colorin-enhanced.tif` | 401797 | `5e528d62ba28fec7dd946ddef2cde7a58c29e22288e3c54e06c12e9a0914d9e7` |
| `canon-eos-5d-mark-iii-colorin-gamut-adobergb.tif` | 401830 | `961d9fc5398d9e235e11f5267b36aa6e917e9ac7cb049da8cbce52ce078a5b30` |
| `canon-eos-5d-mark-iii-colorin-gamut-lin-rec2020.tif` | 401898 | `855b7d66cb34dd1d955652bbbf7fec18e33453bb127e6b9c066b10c183f0ebc3` |
| `canon-eos-5d-mark-iii-colorin-gamut-lin-rec709.tif` | 401746 | `8adb970b6d752976adb32b890a89baad709c40608bc69eedc51f09716f86db82` |
| `canon-eos-5d-mark-iii-colorin-gamut-srgb.tif` | 401746 | `e5e7e041b85816bc7d71636c31e8dfcaa7d0e5513448d7a0b8f0090c2d1494f2` |
| `canon-eos-5d-mark-iii-colorin-hlg-p3.tif` | 393592 | `277abf11fd4d1ff32f544d97db729ef9e566a025179338f420298a10cc4bc795` |
| `canon-eos-5d-mark-iii-colorin-hlg-rec2020.tif` | 394929 | `358986be5ead228a72fc4c2abaaa8922220d1e7c5cf7433715f726b27cc8fb80` |
| `canon-eos-5d-mark-iii-colorin-infrared.tif` | 388876 | `469f7c699bff817383b284f11aa690167e3e5f70ef462e3e9647ddd13da7d680` |
| `canon-eos-5d-mark-iii-colorin-lab.tif` | 401550 | `065bf881b8542de0dacef39c54a7d70f6bd60b73b7db0a0efe1a32b729c11ec3` |
| `canon-eos-5d-mark-iii-colorin-lin-prophoto.tif` | 401871 | `86befd570aab3bd95dc01f87b6fed3ee4e0ef80153567bd23bdea6d059764c66` |
| `canon-eos-5d-mark-iii-colorin-lin-rec2020.tif` | 401550 | `63e7765dce9e9c8cc332a739aebe5ae14088ca187539b43a802cce2102381993` |
| `canon-eos-5d-mark-iii-colorin-lin-rec709.tif` | 388169 | `e37352eac5327fb7aede5aac874597bd4d9a9de4a26d1027dd08972b94e1c85a` |
| `canon-eos-5d-mark-iii-colorin-pq-p3.tif` | 409271 | `44cd13441245c3001427f95c93a1d19cb1f3fed8adfdbb1e8623292f6d0949a0` |
| `canon-eos-5d-mark-iii-colorin-pq-rec2020.tif` | 410369 | `a8723094b892eab1177e53d9766b3fbdc4ce6e4bd6139cfe3b9cd4c7f27319c4` |
| `canon-eos-5d-mark-iii-colorin-rec709.tif` | 391935 | `25442750e3d47cab229838512611ff589f9b84e586bfc2b9779e155c037ce16c` |
| `canon-eos-5d-mark-iii-colorin-srgb.tif` | 396334 | `93e0d0a6dc9856f3e6a55f4811902f5ac813d3dec43c268b5d1aa01cc8f9d2e1` |
| `canon-eos-5d-mark-iii-colorin-standard.tif` | 401834 | `6ec3a1963a98bbea183d7ccc86732170eaed919cc11c3f526920faafc7e1cd73` |
| `canon-eos-5d-mark-iii-colorin-xyz.tif` | 401814 | `048df0a7fecb716182ed113997ee20c20d8cb1cbf58aa2a386d547cafacc3d53` |
| `canon-eos-5d-mark-iii-hotpixels-default.tif` | 401834 | `857037aa1d47ea00d471268419579c3f58fa8b868e179a78836df31efafb7051` |
| `canon-eos-5d-mark-iii-hotpixels-permissive.tif` | 401834 | `e84e3c9813e243519317b74f9693b652d5cdb912c7117fb229668346b717289a` |
| `canon-eos-5d-mark-iii-hotpixels-strength-1.tif` | 401834 | `a0c84f53c28b326e9b8e1821ba0e09870668d2700475ced64df0c83e8d717a5e` |
| `canon-eos-5d-mark-iii-linear-rgb-colorcal-camera.tif` | 370914 | `a78eef571a868e0c221b365ae20a5045a012de1365b8b72909d8659da3cde809` |
| `canon-eos-5d-mark-iii-linear-rgb-colorin-embedded.tif` | 371282 | `5c3a9a034d42b9f6e769920242aa847a5539f54a07376a03c8ac538ac8b33c1f` |
| `canon-eos-5d-mark-iii-linear-rgb-scene-referred-default.tif` | 371519 | `d0fb64c10e001279e23f43907d5b1c0654f930ad4b7392bda079d43b43d33239` |
| `canon-eos-5d-mark-iii-lmmse-refine-0.tif` | 401687 | `fc593945ab50decc6cf1ff248fc7ea3e2beeed6e36fc0be3699d20b9ce691eeb` |
| `canon-eos-5d-mark-iii-lmmse-refine-1.tif` | 401648 | `9ea4e867b8e2d3287f11c452c29ffee46066901a3007ca06f99e750527bfd746` |
| `canon-eos-5d-mark-iii-lmmse-refine-2.tif` | 401623 | `d7a02ea8dcefc3e7a4ab9b65f52274466d361c653f4d96ad504a829a96a5f9b7` |
| `canon-eos-5d-mark-iii-lmmse-refine-3.tif` | 401648 | `928bf2024fd913fd8f9b592b430cc6dd8efe574cbac6dee75956ed37e21552fe` |
| `canon-eos-5d-mark-iii-lmmse-refine-4.tif` | 401775 | `8b692f993925fcbd49a7ccf07127759da886bc3d882e5ca70cefacee921430b1` |
| `canon-eos-5d-mark-iii-passthrough-colour.tif` | 181083 | `65d874bf6ec828c45410d765b4d5ee60435da05ea97121fde9d79e01bdb37403` |
| `canon-eos-5d-mark-iii-passthrough-mono.tif` | 133994 | `60c628eed72b7398221fa38433490e5de93510d6825aa152e3c29f5bb2a40350` |
| `canon-eos-5d-mark-iii-ppg-median-0.3.tif` | 401919 | `6cfae34c9a2c8115b396e5f1acc6d00ec5201930c935c19b79a358c349e888b7` |
| `canon-eos-5d-mark-iii-ppg.tif` | 402057 | `69221c45edf79e886c9c7dfa90bbe6bfc0e89c1e53d2b224008f15d31d7286c3` |
| `canon-eos-5d-mark-iii-rawdenoise-0.05-flat.tif` | 398899 | `2de9dbf01af0cfe9d9fb9ba4bdd1c6d95f41b583bb67de3662b645aee57d1fd2` |
| `canon-eos-5d-mark-iii-rawdenoise-0.05-shaped.tif` | 398562 | `b50c1193c6f6276e87757af0b1ffaa195edd4e02a01f8cc3097bc0dce8ca7ae8` |
| `canon-eos-5d-mark-iii-rcd-dual-0.2.tif` | 401501 | `1d98ccd5fa5cb5f95bc1e96e9dbcdb87275a2c700d9d1c16a9008f17e31ace9e` |
| `canon-eos-5d-mark-iii-rcd-dual-0.6.tif` | 401323 | `27e1e5493cb63e8df9a7c2d464fce07907cdc41a481341c88692ca29993efbad` |
| `canon-eos-5d-mark-iii-rcd-greeneq-both.tif` | 401953 | `b5f13a820cc72a9eda4c154f4e59f0fa7f2ed01f2082520515300b72e20031c8` |
| `canon-eos-5d-mark-iii-rcd-greeneq-full.tif` | 401810 | `2648d462134beff8b7ea5bda71ede8ef335ce4a98d0017c07b3150cc34158a2e` |
| `canon-eos-5d-mark-iii-rcd-greeneq-local.tif` | 401848 | `e96a44a4dd049239f04f678ad2f42ccd0bc6c13affeb3865ec72de7e01de4467` |
| `canon-eos-5d-mark-iii-rcd-smoothing-1.tif` | 401562 | `51420fcde577c45cd1fd9221115949450b59247dfe8bdb0a308f8405c0c87f47` |
| `canon-eos-5d-mark-iii-rcd-smoothing-5.tif` | 401324 | `df43380433c51c271e11da264a93046814d04be7b504eb3f10dc6d7d4102f64f` |
| `canon-eos-5d-mark-iii-rcd.tif` | 401802 | `befc1d87b11d59bd034739cf77ee9d41557e4ef1425d2a61a9f1a851391ab37f` |
| `canon-eos-5d-mark-iii-scene-referred-default.tif` | 402927 | `645ed17a10b03cc152707b6bf2219e81c04660327ab6ce10e3cffa522f755193` |
| `canon-eos-5d-mark-iii-temperature-as-shot-to-reference.tif` | 406941 | `283a90c18d17ca11d84f1f9c7e6f6809c7a3a0494df06a5f057f4c64fc3fe691` |
| `canon-eos-5d-mark-iii-temperature-as-shot.tif` | 401850 | `9570a3cf692848fb8ef04b14291fac989cc15deb3c29ddcbf0381ce3b544a81b` |
| `canon-eos-5d-mark-iii-temperature-camera-reference.tif` | 407250 | `0894ec8397e692b84caa6803e91af62a97d54c539b6932aff531d7e5802fcece` |
| `canon-eos-5d-mark-iii-temperature-user.tif` | 406253 | `2ed9a12f8cb7e4cba2d9196b8f541e6d7445babae68b02e759f2c6b56ff4f730` |
| `canon-eos-5d-mark-iii-vng4.tif` | 401640 | `5c127ca2bb6542cde2d06e89d6229ed1c07f4dfcee4fa694175ea8688813ab2f` |
| `fujifilm-x-pro2-capture-sharpen-auto-t0.3.tif` | 392199 | `2ae370e21b535954e560350abeb14350beb7350d020c46224679b05d39b58e1e` |
| `fujifilm-x-pro2-capture-sharpen-r0.7-t0.3.tif` | 391500 | `7f4bab88e9633c2848fc8d5222b58d5d529bb1a6ac6506d824ec73af6c4f72a6` |
| `fujifilm-x-pro2-colorcal-adaptation-bradford.tif` | 391086 | `7f17981bdcea8ec6718b3ddb25c37dfa176d54ad5ff445641800bee2e9edc82a` |
| `fujifilm-x-pro2-colorcal-adaptation-cat16.tif` | 391221 | `0de7ffc9cc3b79f88d2c14561ae366de173c786f275727b8014ab7e2943f5ce7` |
| `fujifilm-x-pro2-colorcal-adaptation-full-bradford.tif` | 391191 | `13aa653efc3b1fa9f0c37f1e03678087eb42d2a0e7903c3e756e0834c62875da` |
| `fujifilm-x-pro2-colorcal-adaptation-none.tif` | 389478 | `5f619c98db95dd4fe389e3c28bc6997a7c33abff340f7efaa5c357bf1e0ad45a` |
| `fujifilm-x-pro2-colorcal-adaptation-xyz.tif` | 391428 | `2543f67f377f5ccc69a94854805a97c70da80cfca96fc5e7fdb103f404e03da1` |
| `fujifilm-x-pro2-colorcal-brightness.tif` | 391440 | `ded988188f274a4523594bea5756f670549be22148a37b26ae67a7ff07801c47` |
| `fujifilm-x-pro2-colorcal-camera-preset-4.tif` | 391721 | `c2e10457b95313163f133a69e625bbd4b61c6fd1f2e2d617840c89248eb57772` |
| `fujifilm-x-pro2-colorcal-camera.tif` | 390190 | `b08ec9478aa6333a2bbaf6c88024aa9607ce1892dfd2238bee08eba350681b5c` |
| `fujifilm-x-pro2-colorcal-colorfulness-minus-v1.tif` | 390022 | `c7fc125d1bf19deee78df99051164da31c2520dd676a1a6469501ae3b32ceb66` |
| `fujifilm-x-pro2-colorcal-colorfulness-minus-v2.tif` | 389633 | `cf5d3fc605786db6638f68b0a47fbcf20d46447ad23004ab5f9a36a350e6aa8a` |
| `fujifilm-x-pro2-colorcal-colorfulness-minus-v3.tif` | 389622 | `8d3e3d71b188fda7bc0af163bc6e366320cba05003cd3aa683a3cc679eef5686` |
| `fujifilm-x-pro2-colorcal-colorfulness-plus-v1.tif` | 393295 | `53d97dbe7c0e70a5a0724bfcd4ec2f627abb1aa1d015427c620bd428d0376847` |
| `fujifilm-x-pro2-colorcal-colorfulness-plus-v2.tif` | 393140 | `7ec9e36c73028455e94bec990b18163025124cd5984f20eed3cec11f1bd48d7d` |
| `fujifilm-x-pro2-colorcal-colorfulness-plus-v3.tif` | 392897 | `f733f577cf4f088b60aba469629fe6a9528e4ad833b7ff967777acf48de9a5d4` |
| `fujifilm-x-pro2-colorcal-gamut-0-clip-off.tif` | 391216 | `a836fa086cfe199f65c398d7aeb9cd42cd6811e2fddeb93e44592ae4d8653189` |
| `fujifilm-x-pro2-colorcal-gamut-0-clip-on.tif` | 391216 | `312584d7b0e1e76d45621f62b9841c9c4c0e630cacc1a03d3b4834bd64055254` |
| `fujifilm-x-pro2-colorcal-gamut-1-clip-off.tif` | 391253 | `1d52e8e2b10835e1c23e7c6c6ca120c9468e6de79ce67a87f349fd4d98bf069b` |
| `fujifilm-x-pro2-colorcal-gamut-1-clip-on.tif` | 391253 | `57a4e4ea4ac013046050399c154de577bca046f1a53a6e7fdafe515b5e6dc8b8` |
| `fujifilm-x-pro2-colorcal-gamut-4-clip-off.tif` | 390086 | `36ecdc97333bc0d1c62d23bd87a09ad06815b361b5daacf2beb9ab85d0f0e7c7` |
| `fujifilm-x-pro2-colorcal-gamut-4-clip-on.tif` | 390086 | `bf7e947f1a2fadf6f1bff669568c05f52d06dd238a7503e2260f2c98f660df7c` |
| `fujifilm-x-pro2-colorcal-gray-normalize-off.tif` | 236952 | `8eaf948d801202f4d263bcc5d7dc9f892628240b0e0f7ecba25742fe6f0ce714` |
| `fujifilm-x-pro2-colorcal-gray-normalize-on.tif` | 237296 | `440bb20be7568af6a55fd80d608357a403737f9428e81bfdba52fecd6d6bc9c2` |
| `fujifilm-x-pro2-colorcal-illuminant-a.tif` | 384979 | `82d0b408bee9f6fd4d555b83b5ce1f792f601302b80dc46a6fad243c867b4204` |
| `fujifilm-x-pro2-colorcal-illuminant-custom-xy.tif` | 391020 | `3ff2a50a989d230bd8a0b3000791b9267b1f03f3348beacee698190767992a5c` |
| `fujifilm-x-pro2-colorcal-illuminant-e.tif` | 390841 | `62d6a17bfe4ffc98e33bacb4517ca91294262dd45df020a63b4ad79fe3625908` |
| `fujifilm-x-pro2-colorcal-illuminant-f11.tif` | 392985 | `7cced23022e1ad321a0d45201196d8b2b946de5d3634d254f31b1aa3d62e5fc7` |
| `fujifilm-x-pro2-colorcal-illuminant-f2.tif` | 392267 | `f1ff8b8e4c9d847b7140c66df1e81e2edf017e041863a4a2bb1bb19f187b54f6` |
| `fujifilm-x-pro2-colorcal-illuminant-led-b1.tif` | 379625 | `a0de5bfc7511d12fe4a76d0b878b3937917611235343383009d2cee2ad45180a` |
| `fujifilm-x-pro2-colorcal-illuminant-led-rgb1.tif` | 386399 | `81218c062b148b46fe413c0e35b8925a3eca5bcd46f2d48d9c982d16715055fe` |
| `fujifilm-x-pro2-colorcal-illuminant-pipe.tif` | 390146 | `afc22e772404a5ca4937beade7c2a36b0a4957eef57c54417a3b862531133f12` |
| `fujifilm-x-pro2-colorcal-illuminant-planckian-3200.tif` | 393397 | `ba260adefb1c30e7e89d8da58b3173f5455a7436a08a9796b8e035f897548393` |
| `fujifilm-x-pro2-colorcal-mix-normalize-off.tif` | 392219 | `6ec4eb5f9620aa711659d4ce776925a27b377bb7e7d11a752ba797c145aa313b` |
| `fujifilm-x-pro2-colorcal-mix-normalize-on.tif` | 391260 | `ff72a148f14dd8250f6f73b62a745ab6d5a277566bcba51c359c70626afd049f` |
| `fujifilm-x-pro2-colorin-standard.tif` | 390159 | `e3eaeda397576cd2a61acaa5ca4850a241b5a32dcfb01eb69cfb113717114a7f` |
| `fujifilm-x-pro2-fdc.tif` | 389451 | `adf0380647f80f91cbad7b4f09b2e2bf2ce685ce661b5ecc025a65eeb1c11e7b` |
| `fujifilm-x-pro2-hotpixels-default.tif` | 390143 | `27d63e5f257c6a676faff565220b31628861497e8d1db2a0ec82cdbb7c7d2b6f` |
| `fujifilm-x-pro2-hotpixels-permissive.tif` | 390143 | `0f332a7312886b1526d8c3f731dbde69e0573ec4871e14b7bef531e10e746e39` |
| `fujifilm-x-pro2-hotpixels-strength-1.tif` | 390143 | `b79d0f7258849a976a761460c46ae29a69ad98dba0131a56a563591d99454bb8` |
| `fujifilm-x-pro2-markesteijn.tif` | 390127 | `c678bfb45a707652f1625fddd1dd4b4f5db9364e4da9df62da214f93764b8646` |
| `fujifilm-x-pro2-markesteijn3-dual.tif` | 389873 | `e96f1425826ec4581085ed5639d1383226809cbf5ef53a3a86710fdd2fda431c` |
| `fujifilm-x-pro2-markesteijn3.tif` | 390102 | `947b0eadd553c0ee81a2d67ce7e7b576b1557ba7f191858d00a88918ccd5fcb6` |
| `fujifilm-x-pro2-passthrough-colour.tif` | 215654 | `a30b620bba613d3784b58ea36c3af62cb08b4e60862fe9b137b0ed76860fb59d` |
| `fujifilm-x-pro2-passthrough-mono.tif` | 152176 | `0261f2d60e24d30686f465f3171e134bc767f349c9362acad330e7a7a6fb6f80` |
| `fujifilm-x-pro2-rawdenoise-0.05-flat.tif` | 383317 | `033b14d0ecd0f1a741d01a39cc336a5fe20535b639eea69b18fee3e6abfca22c` |
| `fujifilm-x-pro2-rawdenoise-0.05-shaped.tif` | 383819 | `4555eab271cd670fdc27388753cfe5bc0c478c33af022d847ad68f08c34f3430` |
| `fujifilm-x-pro2-scene-referred-default.tif` | 390034 | `ced63fb1b59cbe47672524de631e7d43ed023a0a6c14dfd25d500a33a82f7d89` |
| `fujifilm-x-pro2-temperature-as-shot-to-reference.tif` | 391716 | `c397e8f5a04a348786f972c097696da9c602bbe2d1f54caa597a41d3122a46c5` |
| `fujifilm-x-pro2-temperature-as-shot.tif` | 390159 | `129455ce3c7b150faac161a669a5ce38707fecfb810cd2546ab5285af0b494d5` |
| `fujifilm-x-pro2-temperature-camera-reference.tif` | 391678 | `3b6d18d9e0b8974664df2218dd419b32f5436fc4011df168336fa9a3ec982eea` |
| `fujifilm-x-pro2-temperature-user.tif` | 391560 | `588e3afe5749313d16fbab3bef9315d88ebfb9fe470fe728686256e3cff397b9` |
| `fujifilm-x-pro2-vng.tif` | 389996 | `25c63fbc201877fd82eb6c026546ccaf48227c987bcf21de742d78c74d5f5398` |
| `gainmap-bayer-flat-field-off.tif` | 6968 | `29ab6f6995be94d93455bd6094539006cbcd7cd7b12d3590e2bb9118e971dd0d` |
| `gainmap-bayer-flat-field-on.tif` | 7479 | `0af5b9f3944f88c68e579a970a5aee3d79dbe5010e8f88430a059638a6864a9f` |
| `gainmap-bayer.dng` | 5104 | `9953caf85a9f2a127782704a0603249353b65285c9d99efdf81cc47ba74d50ef` |
| `highlights-bayer-clip.tif` | 910 | `b50cd0789a707dd6719b3b136e958ab1ac5b0b68692da4bffa5808342c8bcf79` |
| `highlights-bayer-inpaint.tif` | 1052 | `f93a0ec8f8765871a61baeed84d06004b4f65ec2057d47d43220322f2fa0b56a` |
| `highlights-bayer-laplacian-i64-n0.1-s0.5.tif` | 3068 | `23d611755bd2f6a5217d5b0c3061c8e1c27404cc1d3a8e83a0162668ca6f18ed` |
| `highlights-bayer-laplacian.tif` | 3011 | `22d5ca2a57e9298488d3b0d9c3c45a58c78752673e8d481931bf1bd332267c6d` |
| `highlights-bayer-lch.tif` | 1057 | `074c25664cde1b98f5d0f07bad64dd12d2c1bc914baf0ece23ed59983063d86a` |
| `highlights-bayer-opposed.tif` | 966 | `3e93b61747458b46ded6658130d9baac0d390806127c5d52dba288cc189b2159` |
| `highlights-bayer-segments-rebuild-generic.tif` | 998 | `35b6bebe417f46039f064c166f7b289bb4e4d7d250dce399b86760224033c1ce` |
| `highlights-bayer-segments-rebuild-off.tif` | 982 | `4d3ca917a38b5c044d86647fe757bca9e6110c4554f5c75cf2434c8a057f1241` |
| `highlights-bayer-segments-rebuild-small.tif` | 998 | `0cc0a1d82c6a137b33e725e09dea0489002eae2d5e123a8de5ad9ce99e541950` |
| `highlights-bayer-unclipped.tif` | 912 | `7bd9d56ff28795bf2cd7aeb5b35674fa2c5fe96a792b82fcdcb590f81b6f775c` |
| `highlights-bayer.dng` | 4576 | `488259d646d498ef9b41b646bd5a017c936ef2e4412a56941751d530ca924aad` |
| `highlights-xtrans-clip.tif` | 1152 | `1f3d69c127e168def15fc0c67f56509030143577e84c71c08f9990cbddf3ba47` |
| `highlights-xtrans-inpaint.tif` | 1338 | `efc4d14935b7672c6cd93c63ed44bd3205b357fafba5ab95fea69c32c78d87f3` |
| `highlights-xtrans-laplacian-i64-n0.1-s0.5.tif` | 3259 | `5dab773a9ab9343e9e8cf7ac9eabbdc878db50ccae3ec69b11dcdb1f8b9e015f` |
| `highlights-xtrans-laplacian.tif` | 3192 | `d312eba3d0be2ee3f55d9ded700e48b06b82b8846e5a228e66d72d0fc447177c` |
| `highlights-xtrans-lch.tif` | 1174 | `884b430a15b46ce3c7bbb051f33f9310f3d97c326f2edb37b307ede0d4ead693` |
| `highlights-xtrans-opposed.tif` | 1223 | `a52027e25bc6c798f1293853934c623df7963479a8eebc233fe15e0b7a694083` |
| `highlights-xtrans-segments-rebuild-generic.tif` | 1255 | `8086723dbf31ff205339abee4bb687254e50925c3fd37c1df497c8e2a2f03ef6` |
| `highlights-xtrans-segments-rebuild-off.tif` | 1239 | `0dd2c903bf67f5d30f20078f7b9a99cf06f2f6d6b26ac22243b0a75c3f7dd0bf` |
| `highlights-xtrans-segments-rebuild-small.tif` | 1255 | `6bbd4f8672a959b52a117259041972fa94c40bc1ab8a90a4dff438afd8011f02` |
| `highlights-xtrans-unclipped.tif` | 1156 | `241162b796604ba4466a8bcccc2a47a9f1fa7bc6d048fa4801aa23791a1c64b9` |
| `highlights-xtrans.dng` | 4608 | `24fa99c7328ebdcf588bf2fc03899a89c7a4ff3f6ff32b3fa7ae77e6d3f8dad1` |
| `hotpixels-bayer-default.tif` | 6973 | `10c90abd8116e410fd13643291f4e423926d501ea203822dee4df5f78dc1c745` |
| `hotpixels-bayer-permissive.tif` | 6973 | `dd59c3627a9be70815b4eb4653a93ce7a01a12af5d91b338acce22fa8de34631` |
| `hotpixels-bayer-strength-1.tif` | 6968 | `849d303ec7d0c450c33b7b0329a0fd242949aaa07593bfc9ac8230e176309313` |
| `hotpixels-bayer.dng` | 4576 | `a27b9cdea99db628f3f2f371db18d404685d7fbfd8113c9ee6d0b0dcccf0ff66` |
| `linear-response-limit-bayer-clip.tif` | 942 | `d7edbf16d584c342e0d9d311857f7b247d6f98ea0981ff09d13804e8b0cc1ee0` |
| `linear-response-limit-bayer.dng` | 4592 | `2e05131eebdfb1450c65036ab5004e603054f8eb1d587095d1f492e7d01f0b45` |

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
deconvolution, CA correction's fits, the dual and the VNG and LMMSE demosaics, denoise (profiled)'s
wavelets and non-local means weights); the X-Trans
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

## Memory bounds on the 100 MP mosaic

`Tests/Native/RawLarge_Test.cpp` develops a synthetic 12288 x 8192 mosaic through the whole chain,
one mode at a time. Each mode renders a 224-row band at the bottom, a 160-row band at the top and a
64 x 64 window at each right-hand corner. Every render starts from an empty node cache, because
a cached image that is asked for a second region grows to the bounding box of both. The test
resets the peak resident set (`/proc/self/clear_refs`) before each mode and bounds its growth. It
also checks that each corner rendered on its own matches the band holding it, which is what the
tiled and whole-frame kernels promise.

F below is one float per photosite, 384 MiB. The band allowance, 2 F (half the frame in RGBA
floats), is the bound that Clip already meets.

| Mode | Bound | Why |
|---|---|---|
| Clip, Flat Field (GainMap) | 2 F | Window plus a halo; the gain maps are a few kilobytes |
| LMMSE, RCD dual, AMaZE dual, Markesteijn 3-pass dual, FDC | 2 F | Tile grids or halos of at most a few hundred photosites around the window |
| Capture sharpening, set radius | 2 F | Halo of 8 x iterations + 10 photosites |
| Inpaint opposed (default) | 3 F + 2 F | Whole source held by the source, RawPrepare and RawTemperature |
| Reconstruct color | 4 F + 2 F | As opposed, plus the rebuilt frame kept per frame key |
| Segmentation based | 4 F + 14 F / 9 + 2 F | As reconstruct color, plus 13 planes at 3 x 3 decimation and a flood-fill stack of F / 144 per plane |
| Guided laplacians | 3 F + 6 F / 4 + 2 F | Kept RGBA image at quarter size (F / 4) and five scratch images of that size |
| Capture sharpening, automatic radius | 118 F / 25 + 2 F | See below |

Capture sharpening with the automatic radius doesn't meet a band bound. RawDemosaic merges the
centre 60 % x 60 % of the frame, where it measures the radius, into the region it asks of its
source. Every band and tile therefore pulls the bounding box of itself and the centre, about 80 %
of the frame, through RawPrepare, RawTemperature and RawHighlights. That costs four stages of
4 F / 5, Clip's top-down copy of 4 F / 5 and two copies of the 9 F / 25 centre, on every render
and not only when the radius is measured. It would meet the band bound if the centre were fetched
in its own request, made only when the radius for the frame key is not yet known.

The whole-frame results (the opposed chrominance; the reconstruct color, segmentation and laplacian
frames; the automatic radius; the full green equilibration ratio; the CA correction) are node
members kept per `frameKey()`. Clear Caches does not release them, so up to F stays resident per
RawHighlights node after a whole-frame mode has run.
