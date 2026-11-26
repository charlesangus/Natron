# Vector Math

Cycles utility `cycles_vector_math`, for the cycles target.

Generated from the Cycles shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| value | float |
| vector | vector |

## Settings

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Math Type | `math_type` | enum | add | knob only | Type Options: add, subtract, multiply, divide, cross_product, project, reflect, dot_product, distance, length, scale, normalize, snap, floor, ceil, modulo, fraction, absolute, minimum, maximum, wrap, sine, cosine, tangent, refract, faceforward, multiply_add, power, sign, round. |

## Inputs

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Vector 1 | `vector1` | vector | 0 0 0 | port | Vector1 |
| Vector 2 | `vector2` | vector | 0 0 0 | port | Vector2 |
| Vector 3 | `vector3` | vector | 0 0 0 | port | Vector3 |
| Scale | `scale` | float | 1 | port | Scale |
