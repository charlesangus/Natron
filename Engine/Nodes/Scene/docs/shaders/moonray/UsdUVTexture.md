# UsdUVTexture

MoonRay map `UsdUVTexture`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Map |

## Controls

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| File | `file` | String |  | knob only | Filename of an EXR or TX format texture (must be mip-mapped and tiled with maketx). If filename contains '<UDIM>', udim index substitution is performed on filename. |
| Output Mode | `output_mode` | Int | rgb | knob only | Controls which channel(s) to output Options: rgb, r, g, b, a. |
| St | `st` | Vec2f | 1 1 | port | Texture coordinate to use to fetch this texture. |
| Wrap S | `wrapS` | Int | useMetadata | knob only | Wrap mode when reading this texture. Options: black, clamp, repeat, mirror, useMetadata. |
| Wrap T | `wrapT` | Int | useMetadata | knob only | Wrap mode when reading this texture. Options: black, clamp, repeat, mirror, useMetadata. |
| Fallback | `fallback` | Rgb | 0 0 0 | knob only | Fallback value used when texture can not be read. |
| Scale | `scale` | Rgb | 1 1 1 | knob only | Scale to be applied to all components of the texture.  output = textureValue * scale + bias |
| Bias | `bias` | Rgb | 0 0 0 | knob only | Bias to be applied to all components of the texture.  output = textureValue * scale + bias |
| Source Color Space | `sourceColorSpace` | Int | auto | knob only | Flag indicating the color space in which the source texture is encoded.  If set to auto, gamma correction will be applied if the images is not single channel. Options: raw, sRGB, auto. |
