# NeuralDenoise

Removes noise with a learned model, run through ONNX Runtime on the CPU. The models are darktable
5.6's neural restore denoise models, installed through the AI model manager (Preferences > AI
Models, or the node's Download Model button).

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| NeuralDenoise | AI model | `1e-4` in the test | the identity test model through each Model Colourspace; the real models have no golden |

1. **Into the model's space.** The RGB of the processed layer is converted by OpenColorIO from the
   project's working space into `modelColourspace`, the space the model was trained in.
2. **Model.** The image is cut into tiles of `tileSize` (0: the size the model asks for, else 512)
   that overlap by 64 pixels (a quarter of the tile at most). Each tile is run through the
   model, and the overlaps are blended away. Edges are padded by reflection.
3. **Back.** The model's RGB is converted from `modelColourspace` back to the working space.
4. **Strength.** Each colour channel becomes `in + strength * (denoised - in)`. At 0 the node is
   the identity and runs no model. Alpha and the other processed channels pass through.

Nothing else is done to the values. There is no clamp, no exposure and no automatic range
handling. The models were trained on the range of their space (sRGB encoded values in [0, 1], or
scene-linear values of a camera exposure), and keeping the image in that range, by grading
before the node and undoing it after, is up to you. The default space,
"sRGB Encoded Rec.709 (sRGB)", round-trips exactly and maps 0.18 to about 0.46, as the sRGB models
were trained. Values above 1 come back, but the model sees them through the sRGB curve, which it
never saw beyond 1.

| Model | Task | Space it suits |
|---|---|---|
| RawNIND linear | `denoise-linear` | Linear Rec.2020 |
| NIND UNet | `denoise` | sRGB Encoded Rec.709 (sRGB) |
| NAFNet-SIDD | `denoise` | sRGB Encoded Rec.709 (sRGB) |

Until a space is picked, `modelColourspace` follows the selected model: Linear Rec.2020 for a
`denoise-linear` model, sRGB Encoded Rec.709 for a `denoise` one. A config that names neither
keeps the name, and the render reports that the config does not have it. A picked space stays when
the model changes. A space the config does not have stops the render with an error naming it.

| Parameter | Default | Meaning |
|---|---|---|
| `model` | the registry's default `denoise-linear` model | The model, `denoise-linear` models first |
| `modelColourspace` | follows the model | The OpenColorIO space the model runs in |
| `strength` | 1 | Mix of the denoised colour into the input |
| `tileSize` | 0 | Tile size in pixels; 0 is the model's own size, or 512 |

The host Mix and Mask apply after the strength.

**Per-frame.** Each frame is denoised alone; there is no temporal denoise, so the result can
flicker between frames of a sequence.

**Proxy scale.** The model runs at the render scale. A proxy render is not a downscaled
full-resolution render: the model sees less noise per pixel.

**Tiles and crops.** The tiles are laid out over the requested region grown by the overlap, so a
cropped render tiles differently from a full one. With the overlap, the difference is small for
these models, but it is not bit-exact; only a model that looks at each pixel alone (such as the
identity test model) matches exactly.

**Licences.** Each model's weights and training data carry their own licences, which the model
manager shows with the model's card before download. The node's code is under Natron's licence.

The model selection, the colourspace following, the strength, tiles, crops and the error cases are
in `Tests/Native/NativeNeuralDenoise_Test.cpp`.
