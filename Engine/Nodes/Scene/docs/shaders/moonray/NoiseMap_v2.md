# NoiseMap_v2

MoonRay map `NoiseMap_v2`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Map |

## Flow Noise

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Flow Angle | `flow_angle` | Float | 0 | can be a port | Angle of rotation for flow noise |
| Flow Advection Rate | `flow_advection_rate` | Float | 0 | can be a port | Rate of advection for flow noise |

## 4D

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Use 4 D Noise | `use_4D_noise` | Bool | off | knob only | If on, 4-dimensional noise is used instead of 3-dimensional |
| Time | `time` | Float | 0 | port | If use 4D noise is on, this is the value for the 4th dimension |

## Adjustment

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Bias | `bias` | Float | 0.5 | can be a port | Bias the noise towards 0 or 1 |
| Gain | `gain` | Float | 0.5 | can be a port | Apply gain to the noise |
| Invert | `invert` | Bool | off | knob only | Invert the final pattern |
| Use Smoothstep | `use_smoothstep` | Bool | off | knob only | Put the noise value through a smoothstep function defined by min/max |
| Smoothstep | `smoothstep` | Vec2f | 0 1 | can be a port | min/max values between which the smoothstep will interpolate |

## Space

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Space | `space` | Int | object | knob only | The space to calculate the noise in Options: render, camera, world, screen, object, reference, texture, input texture coordinates, hair_surface_uv, hair_closest_surface_uv. |
| Input Texture Coordinates | `input_texture_coordinates` | Vec3f | 0 0 0 | port | User specified UVs |
| Object Space | `object_space` | SceneObject* |  | port only | Directly connect object to use that object's space |
| Camera | `camera` | SceneObject* |  | port only | camera used to define camera and screen space |

## Transform

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Translation | `translation` | Vec3f | 0 0 0 | port | Translation of the noise in space |
| Rotation | `rotation` | Vec3f | 0 0 0 | port | Rotates the noise in space based on the specified rotation order |
| Scale | `scale` | Vec3f | 1 1 1 | port | Vector to scale the noise non-proportionally |
| Rotation Order | `rotation_order` | Int | xyz | knob only | Order in which to apply the euler rotations Options: xyz, xzy, yxz, yzx, zxy, zyx. |
| Transformation Order | `transformation_order` | Int | tsr | knob only | Order in which to apply the translation, rotation, and frequency Options: srt, str, rst, rts, tsr, trs. |

## Other

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Seed | `seed` | Int | 0 | knob only | The seed for the random number generator |
| Frequency Multiplier | `frequency_multiplier` | Float | 1 | port | Scalar multiplier for the frequency vector |
| Amplitude | `amplitude` | Float | 1 | port | Intensity of the noise |
| Max Level | `max_level` | Float | 1 | can be a port | Number of octaves of noise to add together for the final result |
| Lacunarity | `lacunarity` | Float | 2 | port | Multiplier on the noise frequency per level |
| Persistence | `persistence` | Float | 0.5 | port | Multiplier on the noise amplitude per level |
| Distortion | `distortion` | Float | 0 | port | Warp input coordinate space with single noise level before looking up noise |
| Distortion Noise Type | `distortion_noise_type` | Int | perlin classic | knob only | Type of noise to use for distortion. Options: perlin classic, perlin simplex. |
| Color | `color` | Bool | off | knob only | Outputs RGB noise |
| Color A | `color_A` | Rgb | 0 0 0 | port | The color value at 0 noise |
| Color B | `color_B` | Rgb | 1 1 1 | port | The color value at 1 noise |
| Noise Type | `noise_type` | Int | perlin classic | knob only | Type of noise to use. Simplex grid activates Flow Noise Angle and Advection Options: perlin classic, perlin simplex. |
