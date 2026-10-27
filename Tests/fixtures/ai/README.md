# AI test models

Tiny ONNX models that `Tests/Ai` runs against the real ONNX Runtime. `tools/ai/make-test-models.py`
writes them. They are committed so that building and testing never need the `onnx` Python package.
Only regenerating them does (`pip install onnx`). The script's output is byte-identical from run to
run, and `make-test-models.py --check` reports any committed model that has drifted from it.

Every model uses opset 17 and IR version 8. The image models take one NCHW float input, `image`,
shaped `[1, 3, height, width]`, and return one output, `output`.

| File | What it does |
|---|---|
| `identity.onnx` | float32 identity, any height and width |
| `identity-fp16.onnx` | float16 identity, any height and width |
| `identity-64.onnx` | float32 identity with a fixed `[1, 3, 64, 64]` input |
| `scale2.onnx` | float32 multiply by 2 |
| `scale2-fp16.onnx` | float16 multiply by 2 |
| `upscale2-nearest.onnx` | 2x nearest-neighbour `Resize`: output pixel (x, y) is input pixel (x/2, y/2) |
| `slow-loop.onnx` | a `Loop` of 2^50 identity steps over a float vector `input` of any length. Only an abort ends it |
| `seg-segnext-encoder.onnx`, `seg-segnext-decoder.onnx` | a SegNext-shaped click-to-segment pair at a 64-pixel input. The "features" are the channel mean of the `[0, 1]` image, and `mask` is `sigmoid(40 (features - 0.5))`, so a bright object on a dark ground segments as itself |
| `seg-sam-encoder.onnx`, `seg-sam-decoder.onnx` | a SAM 2.1-shaped pair at a 64-pixel input. The encoder returns `high_res_feats_0` before `image_embeddings`, which the decoder takes as `image_embed`, so the routing has to reorder them and match by substring. The three logit `masks` are the inverse of the object, the object and nothing; `iou_predictions` always ranks the object first. `low_res_masks` is a 4x4 max-pool of `masks` |
| `identity.dtmodel` | `identity.onnx` packed as a darktable-ai package, `test-identity`, by `tools/ai/make-test-dtmodel.py` |
