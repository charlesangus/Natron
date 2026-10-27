# M208 - Neural Upscale

A native `NeuralUpscale` node, 2× or 4×, from darktable 5.6's neural restore upscale (PR #20523). It runs RealPLKSR (about 7M parameters, MIT, the default) or BSRGAN (Apache-2.0) from darktable-ai. Both models expect sRGB, so the node runs them through M207 - Neural Denoise's scene-linear range wrapper. The output format and region of definition scale by the factor, pixel aspect is kept, and alpha is upscaled with the native cubic resampler. In darktable this is a batch utility that writes a TIFF. Here it is a node, rendered tiled.

Deps: M203 - ONNX Inference Backend, M204 - AI Model Manager, M207 - Neural Denoise (`SceneLinearWrapper`). See `DECISIONS/2026-10-25-darktable-ai-and-image-tool-ports.md`.

## Scope

**In:**
- **`NeuralUpscale`** in `Engine/Nodes/Transform/`, plain ID, major 1. Input: Source.
- **Knobs:**
  - `scale` (2×, 4×), which picks the task `upscale-2x` or `upscale-4x`
  - `model`, via `ModelKnob` on that task
  - `layer` and channels, RGB of any layer
  - `alphaFilter` (Cubic, Lanczos)
  - the range-wrapper controls from M207
  - `tileSize`
  - every darktable upscale control
- **Format and geometry:** the output format is the input format × scale, with pixel aspect unchanged. The region of definition scales about the origin. A region-of-interest request maps back through ÷scale plus the tile overlap.
- Metadata: the output carries an `upscale/factor` metadata key.
- Tests, a doc, an icon and `gpuWeight` Heavy.

**Out:**
- Non-integer factors, and an output-format choice: put a Reformat after the node.
- Temporal consistency: no milestone owns it.
- Training-data licence gaps: Flickr2K, WED and OST have no explicit licence. M204's licence screen shows this, and nothing more is done here.

**Edge cases:**
- **Proxy or render scale below 1:** the node renders at render scale × factor without changing the canonical output format, so the viewer shows the right size.
- **A very large output** (a 4K plate at 4× is 16K): tiles stream into the output image. The node warns once if the output would exceed the image cache's single-image limit.
- **Transform concatenation:** the node is a hard break. It never concatenates with Transform.

## Phase 208.1: Node

- [ ] M208.P1.T1 — NeuralUpscale node
  - files: new `Engine/Nodes/Transform/NeuralUpscale.h`, `Engine/Nodes/Transform/NeuralUpscale.cpp`, `Engine/Nodes/Transform/NeuralUpscale.reg.inc`
  - approach:
    - Override the format, region-of-definition and region-of-interest methods to scale by the factor. Follow `Reformat.cpp` for how a native node changes its output format.
    - Run inference through `runTiled` with `TileSpec.scale = factor`, wrapped in `SceneLinearWrapper` (P1 of M207).
    - Upscale alpha separately with `Resampler` (`Engine/Nodes/Image/Resampler.h`).
    - Read darktable's upscale controls at `DARKTABLE_REF` and expose every one.
  - verify: P1.T2's tests pass.
  - size: L

- [ ] M208.P1.T2 — Tests, docs and identity
  - files: new `Tests/Native/NativeNeuralUpscale_Test.cpp`, `Engine/Nodes/Transform/docs/NeuralUpscale.md`, `Gui/Resources/Images/NativeNodes/`
  - approach:
    - Identity checks with `expectNativeIdentity`.
    - Format, RoD and RoI arithmetic are tested with the fixture `upscale2-nearest.onnx` installed through a `file://` manifest. That model is deterministic, so these checks need no real model.
    - Alpha follows the chosen filter.
    - Model-gated cases:
      - A 2× pass of a downsampled fixture beats a cubic upscale in PSNR against the original.
      - Highlights above 1 survive within 2%.
    - Write the doc and add an icon.
  - verify: `ctest -R NativeNeuralUpscale` passes, with the model-gated cases skipping with a message when no model is installed.
  - size: M

**Verification gate:**
- The full build and ctest pass, with the model-gated tests run where the models are installed.
- Under Xvfb, NeuralUpscale renders 2× and 4× on a plate crop. Screenshots comparing it with Reformat cubic are shared with the user before sign-off.
- An AppImage built with `package.sh` is handed over for UAT.
