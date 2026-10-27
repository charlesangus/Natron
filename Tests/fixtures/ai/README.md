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
