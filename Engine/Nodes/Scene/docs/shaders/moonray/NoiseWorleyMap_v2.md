# NoiseWorleyMap_v2

MoonRay map `NoiseWorleyMap_v2`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Map |

## Output

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Output Mode | `output_mode` | Int | distance | knob only | Method by which the shader outputs a color.  Distance uses F1..F4 interpolated between color A and color B, gradient outputs the gradient of the noise, and cell ID outputs a random color for each cell Options: distance, gradient, cell id, cell edges, points. |
| Distance Method | `distance_method` | Int | linear | knob only | Metric for calculating distance to feature points which controls the shape of the falloff when output mode is distance Options: linear, linear squared, manhattan, chebyshev, quadratic, minkowski. |
| Minkowski Number | `minkowski_number` | Float | 3 | port | Exponent on distances when distance method is set to Minkowski |

## Space

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Space | `space` | Int | world | knob only | The space to calculate the noise in Options: render, camera, world, screen, object, reference, texture, input texture coordinates, hair_surface_uv, hair_closest_surface_uv. |
| Input Texture Coordinates | `input_texture_coordinates` | Vec3f | 0 0 0 | port | User specified UVs |
| Object Space | `object_space` | SceneObject* |  | port only | Directly connect object to use that object's space. |
| Camera | `camera` | SceneObject* |  | port only | Camera used to define camera and screen space |

## Adjustment

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Point Size | `point_size` | Float | 1 | knob only | For points output mode, relative radius of points |
| Remap | `remap` | Vec2f | 0 1 | port | Allows mapping the distances from the specified min/max range into the 0..1 range |
| Use Smoothstep | `use_smoothstep` | Bool | off | knob only | Put the noise value through a smoothstep function defined by min/max |
| Smoothstep | `smoothstep` | Vec2f | 0 1 | can be a port | min/max values between which the smoothstep will interpolate |
| Bias | `bias` | Float | 0.5 | can be a port | Bias of interpolation from color A to color B |
| Gain | `gain` | Float | 0.5 | can be a port | Gain of interpolation from color A to color B |
| Invert | `invert` | Bool | off | knob only | Invert the final pattern |

## Transform

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Translation | `translation` | Vec3f | 0 0 0 | port | Translation of the noise in space |
| Scale | `scale` | Vec3f | 1 1 1 | port | Vector to scale the noise non-proportionally |
| Rotation | `rotation` | Vec3f | 0 0 0 | port | Rotates the noise in degrees based on the specified rotation order |
| Rotation Order | `rotation_order` | Int | xyz | knob only | Order in which to apply the euler rotations Options: xyz, xzy, yxz, yzx, zxy, zyx. |
| Transformation Order | `transformation_order` | Int | tsr | knob only | Order in which to apply the translation, rotation, and frequency Options: srt, str, rst, rts, tsr, trs. |

## Advanced

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| F 1 | `F1` | Float | 1 | port | Influence of F1 (the closest feature point) |
| F 2 | `F2` | Float | 0 | can be a port | Influence of F2 (the second closest feature point) |
| F 3 | `F3` | Float | 0 | can be a port | Influence of F3 (the third closest feature point) |
| F 4 | `F4` | Float | 0 | can be a port | Influence of F4 (the fourth closest feature point) |
| Cell ID | `cell_id` | Int | f1 | knob only | Which of the distances determines the cell id Options: f1, f2, f3, f4. |

## Other

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Seed | `seed` | Int | 0 | knob only | The seed for the random number generator |
| Frequency | `frequency` | Float | 1 | port | Scalar multiplier for the frequency vector |
| Jitter | `jitter` | Float | 1 | port | Controls the distortion of the cells |
| Max Level | `max_level` | Float | 1 | port | Number of octaves of noise to add together for the final result |
| Color A | `color_A` | Rgb | 0 0 0 | port | The interpolated color value at distance equals zero |
| Color B | `color_B` | Rgb | 1 1 1 | port | The interpolated color value at distance equals one |
