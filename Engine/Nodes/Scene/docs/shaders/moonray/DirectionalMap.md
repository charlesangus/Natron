# Directional Map

MoonRay map `DirectionalMap`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Map |

## Normal

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Input Normal | `input_normal` | SceneObject* |  | port only | specifies an alternate shading normal when bound. The binding multiplier is ignored |
| Input Normal Dial | `input_normal_dial` | Float | 1 | port | controls the amount of influence of the alternate normal |

## Adjustment

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Clamping Behavior | `clamping_behavior` | Int | clamp | knob only | determines how surfaces facing opposite the prime direction are handled Options: clamp, absolute. |
| Bias | `bias` | Float | 0.5 | port | controls the rate at which the effect increases as the shading normal approaches the prime direction |
| Use Smoothstep | `use_smoothstep` | Bool | off | knob only | apply smoothstep function to result |
| Smoothstep Start | `smoothstep_start` | Float | 0.45 | port | the value at which the effect is considered 100% off |
| Smoothstep End | `smoothstep_end` | Float | 0.55 | port | the value at which the effect is considered 100% on |
| Falloff Type | `falloff_type` | Int | cosine | knob only | determines how the effect falls off as the difference angle increases Options: cosine, linear. |

## Other

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Prime Direction | `prime_direction` | Int | observer direction | knob only | which source is used for the prime direction Options: observer direction, custom direction, axis of object, look-at object. |
| Polarity | `polarity` | Int | perpendicular | knob only | determines which directions are given color A and which are given color B. Switching this effectively swaps the colors Options: perpendicular, parallel. |
| Color A | `color_a` | Rgb | 0 0 0 | port | the color of the effect when the difference angle is greatest |
| Color B | `color_b` | Rgb | 1 1 1 | port | the color of the effect when the difference angle is smallest |
| Object | `object` | SceneObject* |  | port only | the object to use when 'prime direction' is set to 'axis of object' or 'look-at object' |
| Object Axis | `object_axis` | Int | -X axis | knob only | which axis to use when 'prime direction' is set to 'axis of object' Options: +X axis, -X axis, +Y axis, -Y axis, +Z axis, -Z axis. |
| Custom Direction | `custom_direction` | Vec3f | 0 1 0 | port | specifies a custom direction in world space as the prime direction |
| Use Reference Space | `use_reference_space` | Bool | off | knob only | use reference space position and normals |
