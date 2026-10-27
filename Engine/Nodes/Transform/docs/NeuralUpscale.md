# NeuralUpscale

Enlarges the image 2 or 4 times with a learned model, run through ONNX Runtime on the CPU. The
models are darktable 5.6's neural restore upscale models (RealPLKSR, BSRGAN), installed through
the AI model manager (Preferences > AI Models, or the node's Download Model button).

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| NeuralUpscale | AI model | `1e-4` in the test | the nearest-neighbour test models through the sRGB Model Colourspace, at full and proxy scale; the alpha filter within `1e-5` of `Resampler` cubic; the real models are checked only where installed (PSNR above cubic, highlights within 2%) |

1. **Into the model's space.** The RGB of the processed layer is converted by OpenColorIO from the
   project's working space into `modelColourspace`, the space the model was trained in.
2. **Model.** The Source is cut into tiles of `tileSize` (0: the size the model asks for, else
   512) that overlap by 16 Source pixels (a quarter of the tile at most), darktable's upscale
   overlap. Each tile is run through the model, which returns it `scale` times larger, and the
   overlaps are blended away. Edges are padded by reflection. A large window runs in bands of
   rows, so at most about 256 MB of model output is held at once.
3. **Back.** The model's RGB is converted from `modelColourspace` back to the working space.
4. **Alpha filter.** Alpha, an unchecked R, G or B, and every channel of a one- or two-channel
   layer, are enlarged separably with `alphaFilter`: Natron's cubic filter (the one Transform's
   Cubic uses, which never overshoots) or Lanczos-3 (sharper, rings a little past 0 and 1). Edges
   repeat the Source's edge pixel.

Nothing else is done to the values: no clamp and no automatic range handling. The models were
trained on sRGB encoded values in [0, 1]. The default space, "sRGB Encoded Rec.709 (sRGB)",
round-trips exactly, but values above 1 reach the model through the sRGB curve, beyond anything it
saw. To keep highlights, pick a log space such as ACEScct, which holds scene-linear values up to
several stops over 1 within the model's range.

| Parameter | Default | Meaning |
|---|---|---|
| `scale` | 2x | The factor, 2x or 4x |
| `model` | the registry's default `upscale-2x` model | The model; `upscale-2x` models first, then `upscale-4x` |
| `modelColourspace` | follows the model | The OpenColorIO space the model runs in |
| `alphaFilter` | Cubic | How alpha and unchecked channels are enlarged: Cubic or Lanczos |
| `tileSize` | 0 | Tile size in Source pixels; 0 is the model's own size, or 512 |

`scale` and `model` follow each other: picking a model sets `scale` to its factor, and changing
`scale` selects the same package's model for the new factor (RealPLKSR 2x becomes RealPLKSR 4x),
or the default model for that factor. A model whose factor is not `scale` stops the render with an
error.

**Format and geometry.** The output format is the Source's format times the factor, about the
origin, with the Source's pixel aspect ratio; the region of definition is the Source's times the
factor about the origin. A request for an output region reads the Source over that region divided
by the factor, grown by the 16-pixel overlap at the render scale. The output metadata carries
`upscale/factor` (2 or 4). Put a Reformat after the node for any other size.

**Layers.** Only the processed layer is output. The Source's other layers keep the Source's size,
so they are not passed through.

**Proxy scale.** The model runs on the Source at the render scale and outputs the render scale
times the factor, so a proxy render shows the enlarged image at the right size; the canonical
format does not change. It is not a downscaled full-resolution render: the model sees less detail.

**Transforms.** The node is a hard concatenation break: Transforms before and after it never
merge across it.

**Large outputs.** The node logs a warning once per output size when the output reaches 60
megapixels (darktable's threshold), or when one image of it is larger than the RAM cache
(Preferences > Caching), in which case it is rendered again whenever it is needed.

**Per-frame.** Each frame is upscaled alone; the result can flicker between frames.

**Tiles and crops.** The tiles are laid out over the requested region grown by the overlap, so a
cropped render tiles differently from a full one. With the overlap, the difference is small for
these models, but it is not bit-exact; only a model that looks at each pixel alone (such as the
nearest-neighbour test models) matches exactly.

## darktable controls

Ported from darktable master (`src/libs/neural_restore.c`, `src/common/ai/restore*.c`). Every
control of the neural restore module's upscale tab:

| darktable control | Default | Natron knob | Notes |
|---|---|---|---|
| scale (upscale 2x / 4x) | 2x | `scale` | denoise is NeuralDenoise's |
| model (upscale) | darktable-ai's default | `model` | the `upscale-2x` and `upscale-4x` models |
| tile size (from the model's `input_sizes`) | the model's | `tileSize` | 0 takes the model's size; may be set |
| large-output warning (`LARGE_OUTPUT_MP`) | 60 MP | none | always on: a log warning |
| preserve wide gamut colors | | none | darktable applies it at scale 1 only (denoise) |
| strength | | none | darktable's denoise only |
| output bit depth, compression, ICC profile, output pattern, add to library | | none | the Write node and OCIO |
| preview, preview size | | none | the Viewer |
| AI provider, device, model folder (preferences) | | Preferences > AI | shared by every AI node |

Where Natron differs, because the darktable behaviour is tied to its export pipeline or to a
decision of this port:

- **Space.** darktable converts its linear Rec.2020 export to sRGB itself. Here the model space is
  `modelColourspace`.
- **No clamp.** darktable clamps the model's input at 1 before the sRGB curve. Nothing is clamped
  here; the range is the user's.
- **Alpha.** darktable writes no alpha. Here alpha is enlarged with `alphaFilter`.
- **Output.** darktable writes a TIFF; here the result is a node output, rendered tiled.
