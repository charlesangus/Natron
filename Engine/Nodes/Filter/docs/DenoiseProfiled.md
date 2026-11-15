# DenoiseProfiled

Tolerance:

| Comparison | Class | Test | Notes |
|---|---|---|---|
| darktable, CR2 crops | AMaZE: mean 0.3 %, max 5 %, no pixel over | `NativeDenoiseProfiledReferenceTest` | Relative to the crop's mean, after RawInputColor (`Tests/fixtures/raw/README.md`, Tolerances). The wavelets and the non-local means weights add float-sensitive steps to RCD. |
| darktable, RAF crops | Markesteijn: as AMaZE, 36 pixels over | `NativeDenoiseProfiledReferenceTest` | |
| GPU, non-local means (both colour modes, manual and auto) | mean 1e-5, max 1e-3, no pixel over | `NativeDenoiseProfiledGpuTest` | Relative to the CPU render's mean. Patch distances summed per patch on the device and by running sums on the CPU, and device pow/sqrt/division not correctly rounded; the weights are smooth in the distance. Tiled equals whole bit for bit. |
| GPU, wavelets (both colour modes, manual and auto) | mean 5e-5, max 2e-3, 3 pixels over | `NativeDenoiseProfiledGpuTest` | Thresholds from float tree sums on the device against double sums on the CPU; the edge weights' truncation to an integer steps, and five levels compound it. Compute Variance runs on the CPU only. |

Port of darktable's denoise (profiled) module (`src/iop/denoiseprofile.c`, darktable
`release-5.6.2`, commit `20891f6c6fa7`, params version 12), C path only. It is a Custom op. Mix,
mask and (un)premultiplication come from the stages.

- **Profile**: Auto finds the profile from the raw make, model and ISO, or the exif ones when the
  raw keys are absent. It interpolates between the two profiled ISOs around the plate's ISO, as
  darktable's `dt_noiseprofile_interpolate` does. An ISO outside the profiled range, an unknown
  camera or an unreadable database falls back to the generic Poissonian profile and posts a
  warning. Generic uses **Profile A** and **Profile B**. The other entries are the camera's own
  profiles. **Profile Used** shows what applies.
- The node expects camera RGB, after RawDemosaic and before RawInputColor. It warns on a mosaic,
  on a source that is not from a raw file, and after a chromatic adaptation.
- With the **Develop RAW Denoise** preference (`developRawDenoise`, Project Setup, off by default)
  the Develop RAW chain puts the node there, between RawDemosaic and RawInputColor.
- The defaults of Patch Size, Scattering, Preserve Shadows and Bias Correction follow the
  camera's profile, as darktable's `reload_defaults()` makes them, unless they have been edited.
- Non-local means needs a halo of `searchRadius x (1 + scattering) + patchSize` pixels. Below
  render scale 1 the search radius keeps darktable's canvas minimum of 4. Tiles match the full
  frame.
- Wavelets need the whole frame. The result is computed once per frame, render scale and layer,
  and is cached. The number of scales follows darktable's `max_scale` for the full-size frame,
  so a proxy keeps the same bands.

The noise profile database is `data/noiseprofiles.json`, copied from darktable
(`data/noiseprofiles.json`, same commit) and installed with the other node data. To refresh it,
copy the newer file over it and update the commit in `tools/license/components/README.md`.

## darktable params to knobs

| darktable field | Knob (script name) | Default | Range (soft) | Notes |
|---|---|---|---|---|
| `a[3]`, `b[3]`, `a[0] = -1` | `profile`, `profileA`, `profileB` | auto; generic a, b | a 1e-10–1, b -1–1 | `a[0] = -1` is Auto. The transforms read the green fit only, as darktable does. |
| — | `profileUsed` | — | — | Read-only label: darktable's profile combobox entry. |
| `wb_adaptive_anscombe` | `wbAdaptive` | on | | Reads `raw/wb_applied`. |
| `mode` | `mode` | wavelets | nlm, nlmAuto, wavelets, waveletsAuto, computeVariance | darktable hides "compute variance" behind a preference; here it is always offered. |
| — | `variance` | — | — | Read-only label: darktable's variance readout of the compute variance mode. |
| `radius` | `patchSize` | 1, or from profile | 0–12 (0–8) | |
| `nbhood` | `searchRadius` | 7 | 1–30 | |
| `scattering` | `scattering` | 0, or from profile | 0–20 (0–1) | |
| `central_pixel_weight` | `centralPixelWeight` | 0.1 | 0–10 (0–1) | |
| `wavelet_color_mode` | `colorMode` | Y0U0V0 | rgb, y0u0v0 | |
| `x[6][7]`, `y[6][7]` (all, R, G, B) | `rgbCurves` | 0.5 at 7 bands | 0–1 | Curves are Natron parametric curves through the 7 anchors, sampled and quantised as darktable does. Left is the coarsest band. |
| `x[6][7]`, `y[6][7]` (Y0, U0V0) | `y0u0v0Curves` | 0.5 at 7 bands | 0–1 | As above. |
| `overshooting` | `adjustAutoset` | 1 | 0.001–1000 (0.001–4) | Scales the profile's a before the auto modes derive patch size, scattering, shadows and bias. |
| `strength` | `strength` | 1 | 0.001–1000 (0.001–4) | |
| `shadows` | `preserveShadows` | 1, or from profile | 0–1.8 | |
| `bias` | `biasCorrection` | 0, or from profile | -1000–100 (-10–10) | |
| `fix_anscombe_and_nlmeans_norm` | — | — | — | Always on: only the current behaviour is ported. |
| `use_new_vst` | — | — | — | Always on. |
| `compensate_hilite_pres` | `compensateHighlightPreservation` | on | | Shown only when `raw/highlight_preservation` is a whole EV or more, as darktable shows it; otherwise it changes nothing. |

Visibility follows darktable's `gui_changed()`. Non-local means shows Patch Size, Search Radius,
Scattering and Central Pixel Weight. Its auto mode keeps only Central Pixel Weight. The wavelet
modes show Color Mode and the curves of that colour mode. The auto modes show Adjust Autoset and
hide Preserve Shadows and Bias Correction. Compute Variance shows Variance and hides the
non-local means and wavelet knobs.

- **Compensate Highlight Preservation**: a camera that underexposes to protect highlights
  (Fujifilm DR200/DR400, Nikon HLG tone mode or Active D-Lighting extra high, Pentax dynamic
  range expansion) records it in its makernote. The Read stores darktable's estimate in EV as
  `raw/highlight_preservation`, and with this on, Auto looks the profile up at the ISO shifted
  right by its whole EV, so Profile Used reads, for example, `found ISO 400 (ISO 800 -1 EV)`. The
  makernote tags are darktable's (`src/common/exif.cc`), taken in its order, so a Nikon's
  ColorSpace tag hides its Active D-Lighting. Canon's Auto Lighting Optimizer and Olympus
  gradation are rated under one EV, which changes nothing. The Read does not reach Olympus's
  gradation or a CR3's lighting optimizer, which rawspeed does not parse.
- **Compute Variance** passes the image through and shows, in Variance, the red, green and blue
  variance of the variance-stabilised image over the whole frame at the render scale, followed by
  the frame's size and that scale, for example "over the whole frame (4000×3000 at 100%)": 1
  where the profile fits a perfectly uniform patch. Panning the viewer does not change it; a
  zoomed-out viewer renders at a lower scale and measures a smaller frame, and says so.
  darktable means to measure the same, but its `process_variance()` sums an uninitialised scratch
  buffer instead of the stabilised pixels; the stabilised pixels are measured here. The
  variance runs on the CPU, as darktable's does.

## darktable controls

Generated by `python3 -I tools/raw/parity-audit.py --write-docs` from
`tools/raw/darktable-parity.json`: every control of darktable's `denoiseprofile` and where it is here.

| darktable item | Natron knob/option | darktable default | Natron default | status |
| --- | --- | --- | --- | --- |
| param radius "patch size" | patchSize | 1.0, then inferred from the profile's a (denoiseprofile.c:2706) | from the image, as darktable | mapped |
| param nbhood "search radius" | searchRadius | 7.0 | 7 | mapped |
| param strength | strength | 1.0 | 1.0 | mapped |
| param shadows "preserve shadows" | preserveShadows | 1.0, then inferred from the profile's a (denoiseprofile.c:2706) | from the image, as darktable | mapped |
| param bias "bias correction" | biasCorrection | 0.0, then inferred from the profile's a (denoiseprofile.c:2706) | from the image, as darktable | mapped |
| param scattering "scattering" | scattering | 0.0, then inferred from the profile's a (denoiseprofile.c:2706) | from the image, as darktable | mapped |
| param central_pixel_weight "central pixel weight" | centralPixelWeight | 0.1 | 0.1 | mapped |
| param overshooting "adjust autoset parameters" | adjustAutoset | 1.0 | 1.0 | mapped |
| param a | profile=auto, profileA (a[0] = -1 is profile=auto; otherwise the generic or chosen profile's fit, editable as profileA) | -1 (auto) | auto | mapped |
| param b | profileB | the profile's fit |  | mapped |
| param mode | mode | MODE_WAVELETS | wavelets | mapped |
| param x | N/A: the band positions: rgbCurves and y0u0v0Curves are parametric curves through darktable's evenly spaced anchors, sampled at its band positions |  |  | N/A |
| param y | rgbCurves, y0u0v0Curves (all, R, G, B in rgbCurves; Y0, U0V0 in y0u0v0Curves) | 0.5 |  | mapped |
| param wb_adaptive_anscombe "whitebalance-adaptive transform" | wbAdaptive | TRUE | on | mapped |
| param fix_anscombe_and_nlmeans_norm "fix various bugs in algorithm" | N/A: backward compatibility for old edits; always on, no backward compatibility here | TRUE |  | N/A |
| param use_new_vst "upgrade profiled transform" | N/A: backward compatibility for old edits; always on, no backward compatibility here | TRUE |  | N/A |
| param wavelet_color_mode "color mode" | colorMode | MODE_Y0U0V0 | y0u0v0 | mapped |
| param compensate_hilite_pres "compensate highlight preservation" | compensateHighlightPreservation | TRUE | on | mapped |
| enum MODE_NLMEANS "non-local means" | mode=nlm |  |  | mapped |
| enum MODE_NLMEANS_AUTO "non-local means auto" | mode=nlmAuto |  |  | mapped |
| enum MODE_WAVELETS "wavelets" | mode=wavelets | default |  | mapped |
| enum MODE_WAVELETS_AUTO "wavelets auto" | mode=waveletsAuto |  |  | mapped |
| enum MODE_VARIANCE "compute variance" | mode=computeVariance (always offered; darktable shows it only with a preference set) |  |  | mapped |
| enum MODE_RGB "RGB" | colorMode=rgb |  |  | mapped |
| enum MODE_Y0U0V0 "Y0U0V0" | colorMode=y0u0v0 | default |  | mapped |
| gui profile combobox | profile, profileUsed (profileUsed shows the entry auto resolves to, as darktable's combobox label does) |  |  | mapped |
| gui wavelet channel tabs | N/A: the curve knobs show every channel's curve; the tabs only pick the one darktable's graph edits |  |  | N/A |
| gui wavelet curve graph | rgbCurves, y0u0v0Curves |  |  | mapped |
| gui variance readout | variance |  |  | mapped |
