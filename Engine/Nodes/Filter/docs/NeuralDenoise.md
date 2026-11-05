# NeuralDenoise

Removes noise with a learned model, run through ONNX Runtime on the CPU. The models are darktable
5.6's neural restore denoise models, installed through the AI model manager (Preferences > AI
Models, or the node's Download Model button).

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| NeuralDenoise | AI model | `1e-4` in the test | the identity test model through each Model Colourspace, Shadow Boost and Preserve Wide Gamut; the wavelet and gamut passes are checked on their own; the real models have no golden |

1. **Into the model's space.** The RGB of the processed layer is converted by OpenColorIO from the
   project's working space into `modelColourspace`, the space the model was trained in. With
   Shadow Boost on, the conversion goes through Linear Rec.709 instead, where each value becomes
   its square root (keeping its sign) before it continues into `modelColourspace`.
2. **Model.** The image is cut into tiles of `tileSize` (0: the size the model asks for, else 512)
   that overlap by 64 pixels (a quarter of the tile at most). Each tile is run through the
   model, and the overlaps are blended away. Edges are padded by reflection.
3. **Back.** The model's RGB is converted from `modelColourspace` back to the working space,
   squared again (keeping the sign) in Linear Rec.709 when Shadow Boost is on.
4. **Preserve Wide Gamut** (sRGB models). A pixel whose Source colour, in Linear Rec.709, lies
   outside [-0.01, 1.01] on any channel keeps its Source colour. If any pixel in the 5x5 window
   around it is in gamut, all three channels move by the mean Rec.709 luminance of those denoised
   neighbours less the pixel's own.
5. **Strength.** For the sRGB (`denoise`) models, as darktable's denoise: the luminance residual
   `Y(source) - Y(denoised)` (Rec.709 weights, in the working space) is filtered by darktable's
   wavelet denoise (`dwt_denoise`, five a-trous bands), each band keeping only what exceeds
   `detailBandN * sigma`, sigma being the residual's standard deviation over the frame. Each colour
   channel becomes `denoised + (1 - strength) * filtered`. At 1 nothing is recovered; at 0 all the
   texture above the thresholds comes back, which is not the Source. Below 1 the whole frame is
   denoised once and kept, so changing Strength or rendering another region of the frame reruns
   no model. For RawNIND linear (`denoise-linear`), as darktable's raw denoise: each colour
   channel becomes `in + strength * (denoised - in)`, and at 0 the node is the identity and runs
   no model. Alpha and the other processed channels pass through.

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
| `strength` | 1 | Detail recovery (sRGB models) or mix (linear models), as darktable's |
| `preserveWideGamut` | on | sRGB models: out-of-gamut pixels keep their colour |
| `shadowBoost` | Auto | sRGB models: square-root shadow lift around the model |
| `detailRecovery` group: `detailBand1`..`detailBand5` | 0.25, 0.15, 0.05, 0.02, 0.01 | sRGB models: each wavelet band's threshold, in residual standard deviations, finest first |
| `tileSize` | 0 | Tile size in pixels; 0 is the model's own size, or 512 |

`preserveWideGamut`, `shadowBoost` and the detail bands are disabled for a linear model, as darktable
applies them to its denoise models only. Shadow Boost Auto is darktable's behaviour: on for a model
whose config declares the `shadow_boost` attribute (NAFNet-SIDD) when at least 10% of every 16th
pixel, row and column from the top left of the frame, has a Rec.709 luminance under 0.005. On and
Off force it. Preserve Wide Gamut and Shadow Boost need a Linear Rec.709 space in the config, and
the render reports it when there is none.

The host Mix and Mask apply after the strength.

**Per-frame.** Each frame is denoised alone; there is no temporal denoise, so the result can
flicker between frames of a sequence.

**Proxy scale.** The model runs at the render scale. A proxy render is not a downscaled
full-resolution render: the model sees less noise per pixel.

**Tiles and crops.** The tiles are laid out over the requested region grown by the overlap, so a
cropped render tiles differently from a full one. With the overlap, the difference is small for
these models, but it is not bit-exact; only a model that looks at each pixel alone (such as the
identity test model) matches exactly. An sRGB model below Strength 1 denoises the whole frame,
so its crops match the full frame exactly. Auto Shadow Boost with a model that declares it also
reads the whole Source, to measure the shadows, but denoises only the region.

## darktable controls

Ported from darktable master (`src/libs/neural_restore.c`, `src/common/ai/restore*.c`,
`src/common/dwt.c`). Every control of the neural restore module's denoise and raw denoise tabs:

| darktable control | Default | Natron knob | Notes |
|---|---|---|---|
| model (denoise tab) | darktable-ai's default | `model` | the `denoise` models |
| model (raw denoise tab) | darktable-ai's default | `model` | `denoise-linear` (RawNIND linear) only; the Bayer models are NeuralRawDenoise's |
| strength (denoise) | 100 % | `strength` | 0 to 1; detail recovery as darktable's |
| strength (raw denoise) | 100 % | `strength` | 0 to 1; a linear mix as darktable's |
| preserve wide gamut colors | on | `preserveWideGamut` | darktable's two passes, gamut test in Linear Rec.709 |
| shadow boost (model `shadow_boost` attribute, no GUI control) | per model and image | `shadowBoost` | Auto is darktable's; On and Off are Natron's |
| `plugins/lighttable/neural_restore/detail_recovery_bands` (darktablerc) | 0.25,0.15,0.05,0.02,0.01 | `detailBand1`..`detailBand5` | |
| tile size (from the model's `input_sizes`) | the model's | `tileSize` | 0 takes the model's size; may be set |
| scale (denoise / upscale 2x / 4x) | denoise | none | upscaling is NeuralUpscale's |
| output bit depth, compression, ICC profile, output pattern, add to library | | none | the Write node and OCIO |
| preview, preview size | | none | the Viewer |
| AI provider, device, model folder (preferences) | | Preferences > AI | shared by every AI node |

Where Natron differs, because the darktable behaviour is tied to its export pipeline or to a
decision of this port:

- **Space.** darktable runs everything in its export space, linear Rec.2020, and converts to sRGB
  itself. Here the model space is `modelColourspace`, and the luminance of detail recovery, the
  shadow gate and the gamut neighbourhood are measured in the working space with the same
  Rec.709 weights darktable uses on its working values.
- **No clamp.** darktable clamps the model's input at 1 (and boosted negatives at 0). Nothing is
  clamped here; the range is the user's, and Shadow Boost keeps the sign of negative values.
- **Gamut window.** darktable's 5x5 neighbourhood stops at the edge of the model tile; here it
  stops at the frame's edge only.
- **Noise map.** darktable feeds a constant noise-level map to models with a second input; none
  of the three models has one, and the tiler takes one input.
- **Raw denoise pipeline.** darktable's raw denoise also scales the exposure to a mean of 0.30,
  matches each tile's gain to its input and picks a white-balance mode, all from the model's
  attributes rather than controls. None is ported: exposure and range are the user's.
- **Tiling.** darktable keeps the centre of each tile; here overlapping tiles are blended.

**Licences.** Each model's weights and training data carry their own licences, which the model
manager shows with the model's card before download. The node's code is under Natron's licence.

The model selection, the colourspace following, the darktable controls, tiles, crops and the
error cases are in `Tests/Native/NativeNeuralDenoise_Test.cpp`.
