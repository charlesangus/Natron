# M221 - Neural Optical Flow

Add a `Neural` method to M217 - VectorGenerator Node. It runs a RAFT-class flow model (SEA-RAFT or RAFT, both BSD-3) through M203 - ONNX Inference Backend, with the model fetched on demand by M204 - AI Model Manager. It writes the same Forward, Backward and `vectorConfidence` layers as the classical methods, with confidence from the same round-trip check. It is CPU/ORT-side like every E16 - AI Tools node, so it ends a GPU region.

Deps:
- M217 - VectorGenerator Node: the node, the layers, the confidence code and the fixtures.
- M203 - ONNX Inference Backend: sessions, EP selection, float16, abort, and the per-device run cap.
- M204 - AI Model Manager: the manifest, download, sha256, the licence display, and the `task` registry.

Scouted facts (2026-10-25, from the plan; neither dependency is built yet):
- M204's manifest is `Engine/Ai/data/ai_models.json`, taken from darktable-ai. Its `task` values are `mask-object`, `denoise`, `denoise-raw-bayer`, `denoise-linear`, `upscale-2x` and `upscale-4x`, and there's no flow task. M204's Out list excludes user-supplied models outside the manifest. A `file://` entry serves tests only.
- darktable-ai ships no optical-flow model, so this milestone adds a Natron-hosted manifest entry.
- The E16 rule applies: weights must carry a GPL-3-compatible licence, and both the weights licence and the training-data licence are shown before download (`DECISIONS/2026-10-25-darktable-ai-and-image-tool-ports.md`).

## Scope

**In:**
- **Model choice and export:**
  - Pick SEA-RAFT or RAFT, whichever exports to ONNX with dynamic sizes and runs acceptably on CPU ORT.
  - `tools/ai/export-flow-model.py` exports it to ONNX (opset pinned) and packages it as a `.dtmodel`-style archive.
  - A new manifest `task` `optical-flow`, plus one entry, with the training-data licence recorded (Sintel, KITTI and FlyingThings licences vary).
- **Runner:** `Engine/Ai/OpticalFlow.{h,cpp}`.
  - Inputs are padded to the model's multiple of 8.
  - Frames above `maxInferenceSize` (default 1024 on the long edge) are downscaled, with the vectors upscaled and rescaled.
  - Forward and backward are two runs.
  - Pixel values go through the E16 invertible range wrapper.
- **Node:** `method` gains Neural. It is disabled, with a node message, while AI is disabled in Preferences or the model isn't installed, and the message links to the model manager. `quality` maps to the model's refinement iterations.
- Tests with a tiny generated fixture model.

**Out:**
- GPU-resident (Vulkan) inference → none. E16 keeps tensors in host memory.
- Training or fine-tuning → none.
- Video-propagation models → none.

**Edge cases:**
- **Model missing, or AI disabled, on a saved project using Neural:** the node keeps `method` and errors with the reason. It never silently switches to DIS.
- **ORT abort:** the render aborts through `RunOptionsSetTerminate`.
- **VRAM or run-cap contention:** queued behind M203's per-device cap.
- **Frames smaller than the model's minimum size:** padded.

**Assumptions:**
- Unconfirmed: the model archive is hosted as an asset on the fork's GitHub releases, and the manifest entry points there.
- Unconfirmed: SEA-RAFT's weights are BSD-3 and GPL-3-compatible, and its training data's licences permit the E16 display rule. Verify this in T1 before anything else. If it fails, fall back to RAFT, and record the reason as a decision.

## Phase 221.1: Model and runner

- [ ] M221.P1.T1 — Choose, export and register the flow model
  - files: `tools/ai/export-flow-model.py` (new), `Engine/Ai/data/ai_models.json`, `Engine/Ai/ModelRegistry.cpp`, `docs/ai-models.md` (or M204's model doc, whichever exists)
  - approach:
    - Verify the SEA-RAFT and RAFT weight and training-data licences, and record the choice.
    - Export with dynamic H×W, check ORT CPU inference on a 1024×576 pair, and package it.
    - Add the `optical-flow` task and the entry, with its sha256, size and licences.
  - verify: the export script reproduces the archive's sha256. `ctest -R ModelRegistry` lists the entry under `optical-flow`. The licence check is recorded in `## Decisions`.
  - size: M

- [ ] M221.P1.T2 — Optical-flow runner
  - files: `Engine/Ai/OpticalFlow.{h,cpp}` (new), `Engine/CMakeLists.txt`, `Tests/Ai/OpticalFlow_Test.cpp` (new), `Tests/fixtures/ai/` (tiny generated flow fixture model and its README)
  - approach:
    - Wrap an M203 session: pad, downscale and range-wrap the input; run forward and backward; upscale and rescale the vectors; crop.
    - The fixture model is a generated ONNX graph that outputs a constant flow, so the tests check the plumbing, not the quality.
  - verify: `ctest -R OpticalFlow` passes:
    - the constant-flow fixture comes back exactly at full size;
    - after downscale, the vectors are rescaled to full-scale pixels;
    - padding is cropped;
    - abort terminates the run.
  - size: M

## Phase 221.2: Node integration

- [ ] M221.P2.T1 — Neural method in VectorGenerator
  - files: `Engine/Nodes/Time/VectorGenerator.{h,cpp}`, `Engine/Flow/FlowEstimate.{h,cpp}`, `Engine/Nodes/Time/docs/VectorGenerator.md`, `Tests/Native/NativeVectorGeneratorNeural_Test.cpp` (new)
  - approach:
    - Add `Neural` to `method`, and route `FlowEstimate` to `OpticalFlow`.
    - Confidence and the matte composite reuse M217's code.
    - Declare the node CPU-only when the method is Neural.
    - Report a missing model or disabled AI as a node error, per `## Scope`.
    - Update the doc.
  - verify: `ctest -R NativeVectorGeneratorNeural` passes with the fixture model: Forward and Backward come from the model, confidence is computed, a missing model errors, and the GPU region ends.
  - size: M

**Verification gate:**
- The build and the full ctest pass, including `OpticalFlow`, `ModelRegistry` and `NativeVectorGeneratorNeural`.
- On the release AppImage on the target host, with the real model installed through the model manager:
  - the Neural EPE on M217's synthetic cases beats DIS Medium;
  - the 2K CPU and GPU-EP timings are recorded in `## Decisions`.
- An Xvfb screenshot of Neural Forward vectors next to DIS on the same plate, shared before sign-off.
- An AppImage built with `package.sh` for UAT.

## Decisions

- 2026-10-25 — **Neural flow gets its own milestone under E16 - AI Tools (user),** elaborated now and run after M203 and M204.
