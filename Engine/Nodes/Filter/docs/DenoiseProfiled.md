# DenoiseProfiled

Tolerance: none recorded.

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
| `mode` | `mode` | wavelets | nlm, nlmAuto, wavelets, waveletsAuto | darktable's "compute variance" mode is a darkroom readout and is not ported. |
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
| `compensate_hilite_pres` | — | — | — | Not ported: it reads darktable's raw highlight `iso_shift`, which Natron has no equivalent of. |

Visibility follows darktable's `gui_changed()`. Non-local means shows Patch Size, Search Radius,
Scattering and Central Pixel Weight. Its auto mode keeps only Central Pixel Weight. The wavelet
modes show Color Mode and the curves of that colour mode. The auto modes show Adjust Autoset and
hide Preserve Shadows and Bias Correction.
