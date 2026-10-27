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
| `identity.dtmodel` | `identity.onnx` packed as a darktable-ai package, `test-identity`, by `tools/ai/make-test-dtmodel.py` |
| `rawdenoise.dtmodel` | `test-rawdenoise`, by `tools/ai/make-test-rawdenoise.py`: two Bayer stand-ins with a static `[1, 4, 128, 128]` input that set each output 2x2 block to its block's (R, mean G, B), `model_bayer.onnx` times 1000 under `match_gain` and `model_bayer_absolute.onnx` as is under `absolute` |
