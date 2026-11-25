# ImageMap

MoonRay map `ImageMap`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Map |

## Blur

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Mip Bias | `mip_bias` | Float | 0 | port | Amount to offset derivatives which controls mipmap selection.   This can be used to provide a cheap, albiet low quality, blur.  Note: Setting this value to -1.0 effectively disables mip mapping and the highest resolution will be used. |

## Color Correction

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Saturation Enabled | `saturation_enabled` | Bool | off | knob only | Enables 'saturation' parameter |
| Saturation | `saturation` | Rgb | 1 1 1 | knob only | Applies saturation per channel with 0.0 being grayscale and 1.0 being full saturation |
| Contrast Enabled | `contrast_enabled` | Bool | off | knob only | Enables 'contrast' parameter |
| Contrast | `contrast` | Rgb | 1 1 1 | knob only | Applies contrast per channel with the pivot at 0.18 |
| Gamma Enabled | `gamma_enabled` | Bool | off | knob only | Enables 'gamma_adjust' parameter |
| Gamma Adjust | `gamma_adjust` | Rgb | 1 1 1 | knob only | Applies gamma(power fuction) per channel. |
| Gain Offset Enabled | `gain_offset_enabled` | Bool | off | knob only | Enables 'gain' and 'offset_adjust' parameters |
| Gain | `gain` | Rgb | 1 1 1 | knob only | Applies multiplier per channel. |
| Offset Adjust | `offset_adjust` | Rgb | 0 0 0 | knob only | Applies offset per channel. |
| TMI Control Enabled | `TMI_control_enabled` | Bool | off | knob only | Enables 'TMI' (Temperature Magenta Intensity) parameter |
| TMI | `TMI` | Vec3f | 0 0 0 | knob only | T = Temperature where positive values push towards blue and negative towards red.   M = Magenta where positive values push towards magenta and negative towards green.  I = Intensity where negative values remove and positive values add energy  |

## Other

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Wrap Around | `wrap_around` | Bool | on | knob only | If true, the image will repeat itself at it's boundries.   If false, the image will be clamped at it's boundries. |
| Texture | `texture` | String |  | knob only | Filename that points to a texture .exr or .tx file (must be mip-mapped and tiled with maketx). If filename contains '<UDIM>', udim index substitution is performed on filename.  In the UDIM case, 'scale', 'offset', and 'wrap around' are ignored. |
| Use Default Color When Missing | `use_default_color_when_missing` | Bool | off | knob only | Uses the 'default color' for missing udims and does not report error |
| Default Color | `default_color` | Rgb | 0 1 0 | knob only | Default color to be used for missing udims when 'use default color when missing' is enabled |
| Texture Coordinates | `texture_coordinates` | Int | texture | knob only | Selects the source of the coordinates used to map the texture.  For meshes, the 'texture' setting will read a Vec2f attribute named 'surface_st' if it exists, otherwise it will use barycentric coordinates.   For points and curves, the 'texture' setting will read a Vec2f attribute named 'uv' if it exists.   If the attribute doesn't exist, parametric st coordinates are used for curves and a constant value of (1.0, 1.0) is used for points.  The 'input texture coordinates' setting will evaluate a binding on the 'input_texture_coordinates' parameter and use the resulting red and green channels as the texture coordinates.   The 'hair surface' setting will try to read an explicit attribute named 'surface_st'.   The 'hair closest surface' setting will try to read an explicit attribute named 'closest_surface_uv'. Options: texture, hair surface, input texture coordinates, hair closest surface. |
| Alpha Only | `alpha_only` | Bool | off | knob only | If true, the alpha channel of the texture will be placed in the rgb channels.  If the texture has no alpha channel, 1.0 is used, and the resulting texture lookup is then always white. |
| Input Texture Coordinates | `input_texture_coordinates` | Vec3f | 0 0 0 | port | If the 'texture_coordinates' parameter is set to 'input texture coordinates' then this parameter is evaluated and the resulting red and green channels are used for the u and v coordinates respectively. |
| Gamma | `gamma` | Int | auto | knob only | If this is set to 'on' or 'auto' and the 'texture' file is 8-bit, then a power of 2.2 will be applied to the RGB channels of the image. Options: off, on, auto. |
| Offset | `offset` | Vec2f | 0 0 | port | A constant offset applied to the texture coordinates. |
| Scale | `scale` | Vec2f | 1 1 | port | A constant scale applied to the texture coordinates. |
| Rotation Angle | `rotation_angle` | Float | 0 | knob only | A constant rotation in degrees applied to the texture coordinates. |
| Rotation Center | `rotation_center` | Vec2f | 0.5 0.5 | knob only | UV coordinate around which to rotate |
