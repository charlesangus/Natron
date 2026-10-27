# M207 - Neural Denoise

A native `NeuralDenoise` node, image in and image out, built from darktable 5.6's neural restore denoise (PR #20523, `src/common/ai/restore*.c`). darktable runs it as a batch utility that writes a TIFF; here it is a graph node, rendered tiled on demand.

There are three models. NIND UNet and NAFNet-SIDD are the two that darktable feeds **sRGB [0,1]**, which would clip scene-linear values above 1. This milestone adds a shared **scene-linear range wrapper** that makes those two safe, and M208 - Neural Upscale reuses it:
- **Compress:** after an exposure normalisation, OCIO (M50 - Project OCIO Support) converts the working space to an sRGB display-referred space whose transform maps scene-linear onto [0,1] and is invertible. One transform does both the colour conversion and the range compression, for most working spaces. Negative residue is held aside.
- **Restore:** after the model, the same OCIO transform runs inverted, the exposure is undone and the residue is added back.

The third model, RawNIND's linear Rec.2020 variant, is natively scene-linear. It is the default and bypasses the compression. It also covers X-Trans and other non-Bayer raws after RawInputColor, which M206 - Neural Raw Denoise can't.

Deps: M203 - ONNX Inference Backend, M204 - AI Model Manager. See `DECISIONS/2026-10-25-darktable-ai-and-image-tool-ports.md`.

## Scope

**In:**
- **`NeuralDenoise`** in `Engine/Nodes/Filter/`, plain ID, major 1. Inputs: Source and an optional Mask (the host MaskMix).
- **Knobs:**
  - `model`, via `ModelKnob`, tasks `denoise-linear` and `denoise`; the default is RawNIND linear
  - `strength`, a linear mix of input and result, default 1
  - `layer` and channels, RGB of any layer, with alpha passing through
  - range-wrapper controls (sRGB models only):
    - `exposureMode`: Auto from the frame's 99.5th-percentile luminance, or Manual
    - `exposure`, in stops
    - `compression`, the OCIO sRGB display-referred space used to compress, limited to spaces whose transform is invertible; the default is the config's sRGB display view
  - `tileSize`
  - every control darktable's neural restore offers for denoise
- **`Engine/Ai/SceneLinearWrapper.{h,cpp}`**: the compress and restore stages above, plus a per-frame exposure statistic computed on a downsampled frame, so every tile of a frame gets the same exposure.
- Tests, a doc, an icon and `gpuWeight` Heavy.

**Out:**
- Temporal (multi-frame) denoise: no milestone owns it. Per-frame results may flicker, and the doc says so.
- Profiled classical denoise: M210 - Profiled Denoise.
- Upscaling: M208 - Neural Upscale.

**Edge cases:**
- **Auto exposure on a black frame:** the statistic floors at a tiny value, so a black frame stays black rather than turning into noise.
- **Bright values:** preserved after the round trip within the OCIO inverse's precision. The model's own changes there are compressed by the display transform's shoulder. The doc explains the trade-off.
- **A config with no invertible sRGB display-referred space:** the sRGB models report a persistent error naming the config. RawNIND linear still works.
- **Negative and non-finite values:** negatives skip the model through the residue path. Non-finite values pass through untouched.
- **Region-of-interest requests:** expanded by the tile overlap. The exposure statistic is per frame, not per RoI, so a crop renders identically.
- **Proxy scale:** the model runs at the render scale. The doc notes that the result differs from the full-resolution result.

**Assumptions:**
- Each model's `config.json` states its colour space, `srgb` or `linear-rec2020` (M204.P1.T2's `ModelConfig`).
- The project's OCIO configs (M50) carry an sRGB display-referred space or view whose forward and inverse round-trip HDR values closely enough. A LUT-based view may not reach 1e-5, and its precision bounds P1.T1's tolerance.

## Phase 207.1: Wrapper and node

- [ ] M207.P1.T1 — Scene-linear range wrapper
  - files: new `Engine/Ai/SceneLinearWrapper.h`, `Engine/Ai/SceneLinearWrapper.cpp`, new `Tests/Ai/SceneLinearWrapper_Test.cpp`
  - approach:
    - `prepare(frameStats, cfg)` returns the wrapper parameters: the exposure gain and the OCIO forward and inverse `CPUProcessor`s from working space to the chosen sRGB display-referred space (M50's project config). It rejects a space whose transform has no inverse.
    - `forward(rgb, n) -> modelRgb, residue` applies the gain, splits off the residue `min(x,0)`, then runs the forward processor.
    - `inverse` runs the inverse processor, undoes the gain and adds the residue back.
    - `frameStats(image)` reads the 99.5th-percentile luminance from a 256-px-wide downsample.
  - verify: the gtest round-trips forward then inverse without the model on HDR data (values up to 1e4 and negatives) through the default config's sRGB space, within a tolerance measured once and recorded in the test. The compressed output lies in [0,1]. A non-invertible space is rejected by `prepare`.
  - size: M

- [ ] M207.P1.T2 — NeuralDenoise node
  - files: new `Engine/Nodes/Filter/NeuralDenoise.h`, `Engine/Nodes/Filter/NeuralDenoise.cpp`, `Engine/Nodes/Filter/NeuralDenoise.reg.inc`
  - approach:
    - The node is CPU-only per the README "AI nodes" section.
    - Its `pack` and `unpack` callbacks run the wrapper when the model's colour space is `srgb`. When it is `linear-rec2020`, they only convert working space to Rec.2020 and back.
    - The frame statistic is computed once per frame and time-cached through the node's frame-varying cache key.
    - `strength` mixes in linear space.
    - Read darktable's restore denoise controls at `DARKTABLE_REF` and expose every one.
  - verify: P1.T3's tests pass.
  - size: L

- [ ] M207.P1.T3 — Tests, docs and identity
  - files: new `Tests/Native/NativeNeuralDenoise_Test.cpp`, `Engine/Nodes/Filter/docs/NeuralDenoise.md`, `Gui/Resources/Images/NativeNodes/`
  - approach:
    - Identity checks with `expectNativeIdentity`.
    - `strength 0` gives the input exactly.
    - Model-gated cases on a synthetic noisy gradient with highlights up to 50:
      - Each model reduces the noise standard deviation by at least 2× in flat patches.
      - The highlights survive with a mean error under 2% for the sRGB models via the wrapper.
      - Without the wrapper (a test hook), they clip. This proves the wrapper matters.
    - A cropped RoI matches the full frame, cropped.
    - The doc carries a `Tolerance:` line, the flicker note and the licence notes.
  - verify: `ctest -R NativeNeuralDenoise` passes. Model-gated cases skip with a message when the model is absent.
  - size: M

**Verification gate:**
- The full build and ctest pass, with the model-gated tests run where the models are installed.
- Under Xvfb, NeuralDenoise renders on a high-ISO EXR plate with bright highlights for each model. Screenshots, compared against the input, are shared with the user before sign-off.
- An AppImage built with `package.sh` is handed over for UAT.

## Decisions

- 2026-10-27 — **Colour space is the user's (user, supersedes the OCIO range-wrapper decision):** the node has an OCIO colourspace knob. It converts from the working space into that space, runs the model, and converts back. No automatic range wrapper, clamp, residue or exposure: choosing a space that suits the model and managing range/variance is the user's job. Default: "sRGB Encoded Rec.709 (sRGB)" (measured: exact round trip, 0.18 → 0.46 as the models were trained). Measurements behind it are in the 2026-10-27 consultant report (ACES 2.0 SDR is not invertible on saturated colours).
- 2026-10-27 — **Range (user):** 1e4 is unrealistic; camera values rarely exceed 50–70. Measure invertibility of the config's ACES/sRGB-ish views and log spaces over 0–50 and pick from that. **ModelKnob may take several tasks (user)** — extend M204's ModelKnob.
- 2026-10-27 — The range wrapper compresses through OCIO, not a custom soft-knee: an invertible sRGB display-referred OCIO space takes most working spaces to sRGB [0,1] in one transform (user, inbox 2026-10-27).
