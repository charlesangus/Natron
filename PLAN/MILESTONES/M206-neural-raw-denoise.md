# M206 - Neural Raw Denoise

A native `RawNeuralDenoise` node that runs the RawNIND UtNet2 Bayer model (GPL-3 weights, darktable-ai) on the raw sensor mosaic. It is ported from darktable 5.6's raw neural restore path (PR #20854, `src/common/ai/restore*.c`, consumer `src/libs/neural_restore.c`). The model denoises and demosaics in one pass: four packed CFA planes go in, camera RGB comes out. So the node is raw in, image out, and in a Develop RAW chain it sits where RawDemosaic does. In darktable this is a batch utility that writes a sibling DNG. Here it is an ordinary node in the graph, rendered tiled on demand.

Deps: M203 - ONNX Inference Backend, M204 - AI Model Manager. It builds on M80 - Native RAW Support (done): `eDataKindRaw`, `RawMetadata.h` and the `raw/stage` contract. See `DECISIONS/2026-10-25-darktable-ai-and-image-tool-ports.md`.

## Scope

**In:**
- **`RawNeuralDenoise`** in `Engine/Nodes/Raw/`, plain ID, major 1, Tab menu `Raw/`.
  - Input: raw kind, Bayer CFA, at the `raw/stage` darktable's restore path expects.
  - Output: image kind in camera RGB, with `raw/stage` set to the demosaiced stage, so RawInputColor and RawColorCalibration follow it unchanged.
- **Knobs:**
  - `model`, via `ModelKnob`, task `denoise-raw-bayer`
  - `strength`, a mix with a plain demosaic: 0 is a bilinear demosaic of the same input and 1 is the model output, default 1
  - `tileSize`, which defaults to the model config
  - every control darktable's neural restore offers for raw (P1.T1 lists them)
- Tiled inference through M203, with the CFA-aligned packing (2×2 superpixels) and an overlap aligned to the CFA period.
- A doc, an icon, `gpuWeight` Heavy, and tests. A Develop RAW variant option: Preferences "Develop RAW demosaic" gains "Neural (RawNIND)", which inserts this node instead of RawDemosaic when the model is installed.

**Out:**
- X-Trans and other non-Bayer CFAs: the model is Bayer-only, so the node shows a persistent error that points to NeuralDenoise's RawNIND linear model after RawInputColor (M207 - Neural Denoise).
- Writing DNGs: none.
- A GPU-resident path: none. The node ends GPU regions (M203).

**Edge cases:**
- **Wrong `raw/stage` or a non-Bayer CFA:** a persistent error naming what is needed, with no render, matching M80's raw-node contract.
- **CFA origin offsets from crops** (`raw/cfa_origin`): tiles are packed from the true origin, so an odd crop never swaps channels.
- **RoI requests:** the node expands the RoI to whole superpixels plus the tile overlap.
- **Proxy scale:** the model always runs at full resolution, and the output is downscaled. Below 1/2 scale, the node falls back to a bilinear demosaic with a viewer notice.

**Assumptions:**
- RawNIND's `config.json` and darktable's preprocessing determine the input normalisation (black-subtracted, white-scaled, with or without WB). P1.T1 confirms this from the source and fixes the stage contract.

## Phase 206.1: Node

- [ ] M206.P1.T1 — Read darktable's raw restore path and write the node contract
  - files: `Engine/Nodes/Raw/docs/RawNeuralDenoise.md` (new, contract section only)
  - approach:
    - Read darktable `src/common/ai/restore*.c` and `src/libs/neural_restore.c` at the `DARKTABLE_REF` in `tools/ci/local/fetch-assets.sh`. Use master if #20854 is newer than the pin, and record the SHA used.
    - Write down the exact pre- and post-processing:
      - normalisation and WB state
      - packing order relative to the CFA
      - output colour space
      - tile size and overlap
      - every user control
    - Map the contract onto M80's `raw/stage` values and `RawMetadata` keys.
  - verify: the doc's contract section names each step with a darktable file:line, and is reviewed at the gate.
  - size: M

- [ ] M206.P1.T2 — RawNeuralDenoise node (CPU tiled inference)
  - files: new `Engine/Nodes/Raw/RawNeuralDenoise.h`, `Engine/Nodes/Raw/RawNeuralDenoise.cpp`, `Engine/Nodes/Raw/RawNeuralDenoise.reg.inc`
  - approach:
    - Build on M137's framework, following RawDemosaic's raw-in/image-out shape (`RawDemosaic.cpp`) and the README "AI nodes" section from M203.P3.T3.
    - `pack` and `unpack` implement P1.T1's contract. The tiler is `runTiled` with a CFA-aligned overlap.
    - The `strength` mix uses a bilinear demosaic of the same input from `RawKernels.h`.
    - Set `raw/stage` on output, and keep the stage and CFA checks from `RawNodeSupport`.
  - verify: P1.T4's tests pass.
  - size: L

- [ ] M206.P1.T3 — Develop RAW "Neural" option
  - files: `Engine/Settings.cpp`, `Engine/Settings.h`, `Engine/Nodes/Raw/RawDevelop.cpp`
  - approach:
    - Add a `developRawDemosaic` preference with two values, Classic (RawDemosaic) and Neural (RawNIND).
    - `RawDevelop::buildChain` (`RawDevelop.cpp:128`) creates RawNeuralDenoise in RawDemosaic's slot when the preference is Neural, the model is installed and the CFA is Bayer. Otherwise it builds the classic chain and logs why.
  - verify: a gtest drives `buildChain` with each preference value and both CFA types and checks the node types in the backdrop.
  - size: M

- [ ] M206.P1.T4 — Tests, a darktable reference and docs
  - files: new `Tests/Native/NativeRawNeuralDenoise_Test.cpp`, `Tests/fixtures/raw/reference/` (one new crop), `tools/raw/make-reference.py`, `Engine/Nodes/Raw/docs/RawNeuralDenoise.md`, `Gui/Resources/Images/NativeNodes/`
  - approach:
    - Identity checks with `expectNativeIdentity`.
    - Errors for the wrong stage and for X-Trans.
    - An odd CFA origin gives the same result as the uncropped run, cropped.
    - With the model installed, a 512² crop of the Canon fixture must match darktable's neural-restore DNG output for the same crop. Add a job to `make-reference.py` if darktable-cli can drive neural restore. Otherwise, take a reference from darktable's Lua AI API (`src/lua/ai.c`) or record a self-reference, and say which in the doc.
    - The tolerance goes in the doc's `Tolerance:` line.
    - Finish the doc, with every knob, and add an icon.
  - verify: `ctest -R NativeRawNeuralDenoise` passes. Model-gated cases skip with a message when the model is absent.
  - size: M

**Verification gate:**
- The full build and ctest pass, with the model-gated tests run on a host that has RawNIND installed.
- Under Xvfb, a Develop RAW chain on a noisy high-ISO raw fixture is rendered in Classic and in Neural mode, and side-by-side screenshots are shared with the user before sign-off.
- An AppImage built with `package.sh` is handed over for UAT.
