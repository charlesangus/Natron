# M210 - Profiled Denoise

A native `DenoiseProfiled` node ported from darktable's `src/iop/denoiseprofile.c` (about 3.8k lines, with OpenCL), plus its noise-profile database: `data/noiseprofiles.json`, covering 438 camera models with Poissonian–Gaussian parameters per ISO, read by `src/common/noiseprofiles.c`. Both are taken at the `DARKTABLE_REF` pinned in `tools/ci/local/fetch-assets.sh`.

How it works:
- A generalised Anscombe transform stabilises variance using the profile.
- Then non-local means or à-trous wavelets denoise the result.
- The inverse transform has bias correction.

The profile is looked up from `raw/make`, `raw/model` and `raw/iso` (`Engine/Nodes/IO/RawMetadata.h`), falling back to `exif/*`, and interpolated between ISOs as darktable does. On a plate with no matching profile, the generic Poissonian profile and manual a/b parameters apply.

No native denoise exists today: M123 and M130 both leave CImg Denoise on OFX. This node fills the gap.

Every user-facing control and darktable default is ported (`port-means-every-control`).

Deps: M209 - Edge-Aware Filter Library (wavelets). See `DECISIONS/2026-10-25-darktable-ai-and-image-tool-ports.md`.

## Scope

**In:**
- **`DenoiseProfiled`** in `Engine/Nodes/Filter/`, plain ID, major 1. Inputs: Source and an optional Mask (the host MaskMix). It also works on any image-kind plate.
- **Every control** of the module's newest params version and GUI:
  - `profile` (auto from metadata, generic Poissonian, or any DB entry)
  - `mode` (non-local means, non-local means auto, wavelets, wavelets auto)
  - `wbAdaptive` (white-balance-adaptive transform)
  - `colorMode` (RGB, Y0U0V0)
  - `adjustAutoset`
  - `preserveShadows`
  - `biasCorrection`
  - `patchSize`, `searchRadius`, `scattering` and `centralPixelWeight`
  - `strength`
  - the wavelet band curves per channel group, as parametric curve knobs
  - manual a/b when the profile is generic
- The vendored noise-profile DB, a lookup with ISO interpolation, and a read-only "profile used" knob.
- A Develop RAW option, off by default: insert DenoiseProfiled between RawDemosaic and RawInputColor, where darktable places it.
- CPU and GPU paths, darktable reference parity, a params-to-knob audit, a doc, an icon.

**Out:**
- Noise profiling tools (darktable's `-DBUILD_NOISE_TOOLS`): none.
- Neural denoise: M207 - Neural Denoise.
- darktable's astro denoise module (`nlmeans`, Lab) and `rawdenoise`, which M80 ported: none.

**Edge cases:**
- **Make or model not found:** fall back to the generic profile, with a message naming what was looked up.
- **ISO outside the profiled range:** clamp to the nearest profile, as darktable does.
- **Input not in camera space** (no `raw/stage`, or after RawInputColor): the profile still applies, but the node shows a notice that profiles assume camera RGB before input colour.
- **Proxy scale:** the NLM patch and search radii and the wavelet scales scale with render scale, as darktable's `roi_in->scale` handling does.
- **Region-of-interest expansion:** by the search radius plus the patch size, or by the largest wavelet scale's support.

**Assumptions:**
- The pinned rawspeed's canonical make and model strings match the DB's keys, since darktable uses the same rawspeed names. P1.T1 checks this for the three raw fixtures.

## Phase 210.1: Profiles

- [ ] M210.P1.T1 — Vendor the noise-profile DB and write the lookup
  - files: new `Engine/Nodes/Filter/data/noiseprofiles.json` (copied at `DARKTABLE_REF`, with a header note in the doc), new `Engine/Nodes/Filter/NoiseProfiles.{h,cpp}`, `tools/license/components/README.md`, `Engine/CMakeLists.txt` (Qt resource), new `Tests/Native/NoiseProfiles_Test.cpp`
  - approach:
    - Port `noiseprofiles.c`: parse, look up by make, model and ISO, and interpolate between neighbouring ISOs.
    - Add a `fetch-assets.sh`-free refresh note: the doc says how to re-copy the file at a new pin.
    - Record the file's GPL-3 provenance in the licence README.
  - verify: `Tests/Native/NoiseProfiles_Test.cpp` resolves the Canon 5D Mark III fixture (`raw/make`/`raw/model` from the Read) at ISOs inside and between entries, matching hand-computed interpolation, and an unknown camera returns generic.
  - size: M

## Phase 210.2: CPU node

- [ ] M210.P2.T1 — Node skeleton, knobs and variance stabilisation
  - files: new `Engine/Nodes/Filter/DenoiseProfiled.h`, `Engine/Nodes/Filter/DenoiseProfiled.cpp`, `Engine/Nodes/Filter/DenoiseProfiled.reg.inc`
  - approach:
    - Every knob from the Scope, with darktable's defaults and auto-mode logic: `adjustAutoset` maps to the derived patch, search, scattering and strength.
    - The generalised Anscombe forward and inverse use bias correction, and the white-balance-adaptive coefficients come from `raw/wb_coeffs` when present.
    - Profile resolution happens once per frame.
  - verify: with the denoiser stubbed to identity, the forward and inverse transforms round-trip within 1e-5, and P2.T4's knob audit passes.
  - size: L

- [ ] M210.P2.T2 — Non-local means path
  - files: `Engine/Nodes/Filter/DenoiseProfiled.cpp`, new `Engine/Nodes/Filter/DenoiseProfiledNlm.cpp`
  - approach: port `process_nlmeans_cpu` and its helpers: patch distances, scattering and central-pixel weight. Thread over rows.
  - verify: P2.T4's reference jobs for the NLM modes pass.
  - size: L

- [ ] M210.P2.T3 — Wavelet path and Y0U0V0
  - files: `Engine/Nodes/Filter/DenoiseProfiled.cpp`, new `Engine/Nodes/Filter/DenoiseProfiledWavelets.cpp`
  - approach:
    - Port `process_wavelets` on M209's `Wavelets`, with per-band thresholds from the curves.
    - Implement the RGB↔Y0U0V0 transform and its curve-group mapping exactly as darktable does.
  - verify: P2.T4's reference jobs for the wavelet modes pass.
  - size: L

- [ ] M210.P2.T4 — Reference parity and knob audit
  - files: `tools/raw/make-reference.py`, `Tests/fixtures/raw/reference/` (new crops), new `Tests/Native/NativeDenoiseProfiled_Test.cpp`
  - approach:
    - Add darktable-cli jobs on the Canon and Fuji fixtures for each mode and color mode, plus shadows, bias and strength variants. Feed the node the same pre-denoise stage through the existing Develop chain, following `RawReferenceModules_Test.cpp`.
    - The audit compares every `dt_iop_denoiseprofile_params_t` field and enum `$DESCRIPTION` against the node's knobs, in the way M80.P7's audit does.
    - Include identity checks.
  - verify: `ctest -R NativeDenoiseProfiled` passes within the doc's `Tolerance:`.
  - size: L

## Phase 210.3: GPU, Develop option and docs

- [ ] M210.P3.T1 — GPU kernels for NLM and the transforms
  - files: new `Engine/Gpu/Kernels/denoiseprofile_nlm.slang`, `denoiseprofile_nlm.cmake`, `denoiseprofile_vst.slang`, `denoiseprofile_vst.cmake`, `Engine/Nodes/Filter/DenoiseProfiled.cpp`
  - approach: port from darktable `data/kernels/denoiseprofile.cl`. Run the wavelet path on M209's GPU wavelets.
  - verify: P3.T2's parity tests pass.
  - size: L

- [ ] M210.P3.T2 — GPU parity tests
  - files: new `Tests/Native/NativeDenoiseProfiledGpu_Test.cpp`
  - approach: run each mode on GPU against CPU on fixture crops.
  - verify: `ctest -R NativeDenoiseProfiledGpu` passes on lavapipe within the doc's tolerance.
  - size: M

- [ ] M210.P3.T3 — Develop RAW option, doc and icon
  - files: `Engine/Settings.{h,cpp}`, `Engine/Nodes/Raw/RawDevelop.cpp`, `Engine/Nodes/Filter/docs/DenoiseProfiled.md`, `Gui/Resources/Images/NativeNodes/`
  - approach:
    - Add a `developRawDenoise` bool (off) that inserts the node into `buildChain` after the demosaic slot.
    - The doc covers profiles, modes, the camera-space notice and the tolerance.
  - verify: a `buildChain` gtest with the option on finds the node in place, and `ctest -R NativePluginList` passes.
  - size: M

**Verification gate:**
- The full build and ctest pass, including both DenoiseProfiled suites on lavapipe and RADV.
- Under Xvfb, a high-ISO raw is developed with DenoiseProfiled in each mode, with screenshots against darktable's render shared with the user before sign-off.
- An AppImage built with `package.sh` is handed over for UAT.

## Decisions

- 2026-10-27 — Restacked on main after M209 merged; gate green. UAT: build/appimages/Natron-m210-profiled-denoise.AppImage, M210-uat.md, draft PR #140. Reference suite timeout 1800 s (full-frame parity cases ~180 s per site).
- 2026-10-30 — **UAT passed (user).** Profiled Denoise is accepted for merge.
