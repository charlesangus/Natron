# BlendMap

MoonRay map `BlendMap`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Map |

## Controls

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Color A | `color_A` | Rgb | 1 1 1 | port | The color you get if blend amount is 0 |
| Color B | `color_B` | Rgb | 1 1 1 | port | The color you get if blend amount is 1 |
| Blend Type | `blend_type` | Int | linear | knob only | The type of blending algorithm Options: linear, cubic. |
| Blend Amount | `blend_amount` | Float | 0.5 | port | The amount to blend between color A (0) and color B (1) |
| Threshold Min | `threshold_min` | Float | 0 | port | If the blend amount is less than or equal to this amount, it will choose color A (0) |
| Threshold Max | `threshold_max` | Float | 1 | port | If the blend amount is greater than this amount, it will choose color B (1) |
