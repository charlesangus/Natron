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
| `upscale4-nearest.onnx` | 4x nearest-neighbour `Resize`: output pixel (x, y) is input pixel (x/4, y/4) |
| `slow-loop.onnx` | a `Loop` of 2^50 identity steps over a float vector `input` of any length. Only an abort ends it |
| `seg-segnext-encoder.onnx`, `seg-segnext-decoder.onnx` | a SegNext-shaped click-to-segment pair at a 64-pixel input. The "features" are the channel mean of the `[0, 1]` image, and `mask` is `sigmoid(40 (features - 0.5))`, so a bright object on a dark ground segments as itself |
| `seg-sam-encoder.onnx`, `seg-sam-decoder.onnx` | a SAM 2.1-shaped pair at a 64-pixel input. The encoder returns `high_res_feats_0` before `image_embeddings`, which the decoder takes as `image_embed`, so the routing has to reorder them and match by substring. The three logit `masks` are the inverse of the object, the object and nothing; `iou_predictions` always ranks the object first. `low_res_masks` is a 4x4 max-pool of `masks` |
| `flow-constant.onnx` | a RAFT-shaped optical-flow stand-in: float inputs `image1` and `image2`, `[1, 3, height, width]`, and one output `flow`, `[1, 2, height, width]`, equal to dx 2.5 and dy -1.5 at every pixel whatever the images hold. Only the images' size reaches the output, so `OpticalFlow_Test` checks padding, downscale and rescaling, not quality |
| `flow-order.onnx` | the same inputs and output as `flow-constant.onnx`, with a flow equal at every pixel to (mean of `image2` - mean of `image1`) times 1/255 for dx and 2/255 for dy, so swapping the images flips its sign |
| `flow-position.onnx` | the same inputs and output, with dx the column and dy the row of each pixel of the padded input, so a crop at the wrong offset shows |
| `flow-iterations.onnx` | the same images and output plus a third input, `iterations`, float `[1]`, as the packaged SEA-RAFT model takes; the flow is dx 1 and dy 2 per iteration at every pixel, so `OpticalFlow_Test` sees the iteration count the runner feeds |
| `identity.dtmodel` | `identity.onnx` packed as a darktable-ai package, `test-identity`, by `tools/ai/make-test-dtmodel.py` |
| `rawdenoise.dtmodel` | `test-rawdenoise`, by `tools/ai/make-test-rawdenoise.py`: two Bayer stand-ins with a static `[1, 4, 128, 128]` input that set each output 2x2 block to its block's (R, mean G, B), `model_bayer.onnx` times 1000 under `match_gain` and `model_bayer_absolute.onnx` as is under `absolute` |
| `upscale.dtmodel` | the two nearest-neighbour upscales packed as `test-upscale` (`model_x2.onnx`, `model_x4.onnx`, 32-pixel tiles), by `tools/ai/make-test-dtmodel.py` |
