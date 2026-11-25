# RampMap

MoonRay map `RampMap`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Map |

## Ramp Knot

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Positions | `positions` | FloatVector | 4 rows | knob only | Color ramp |
| Colors | `colors` | RgbVector | 4 rows | knob only | List of colors on the ramp |
| Interpolations | `interpolations` | IntVector | 4 rows | knob only | None: 0 \| Linear: 1 \| Exponential Up: 2 \| Exponential Down: 3 \| 			Smooth: 4 \| Catmull Rom: 5 \| Monotone Cubic: 6 |

## Ramp properties

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Ramp Type | `ramp_type` | Int | v | knob only | Determines input signal / shape of the ramp Options: v, u, diagonal, radial, circular, box, uxv, four corner, input. |
| Input | `input` | Float | 1 | port | Input signal for ramp, used when ramp type is set to input |
| Space | `space` | Int | render | knob only | Only applies when 'texture coordinates' is set to 'default state coordinates' Options: render, camera, world, screen, object, reference, texture. |
| Object | `object` | SceneObject* |  | port only |  |
| Camera | `camera` | SceneObject* |  | port only | Camera used to define camera and screen space |
| Color Space | `color_space` | Int | rgb | knob only | Color space to perform interpolation in Options: rgb, hsv, hsl. |

## Additional properties

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Input Texture Coordinates | `input_texture_coordinates` | Vec3f | 0 0 0 | port | Bind custom UV coordinates |
| Wrap Type | `wrap_type` | Int | wrap | knob only | Whether to repeat the ramp or maintain the color at each edge Options: wrap, clamp. |
| UV Wave | `uv_wave` | Vec2f | 0 0 | knob only | Creates waves which perturb the ramp pattern |
| UV Repeat | `uv_repeat` | Vec2f | 1 1 | knob only | Number of times to repeat the ramp pattern |

## Other

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Texture Coordinates | `texture_coordinates` | Int | default state coordinates | knob only | Whether to read existing coordinates or bind custom ones Options: default state coordinates, input texture coordinates. |
