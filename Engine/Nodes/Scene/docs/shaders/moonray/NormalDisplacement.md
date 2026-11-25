# NormalDisplacement

MoonRay displacement `NormalDisplacement`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Displacement |

## Controls

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Bound Padding | `bound_padding` | Float | 0 | knob only | bound padding defines how much to extend the bounding box of the object. Keep this value as low as possible unless the geometry skips tessellation because control cage bounding box is out of camera frustum but the displacement stretch out of the original object bounding box (pre-displacement). Setting the bound padding too large will consume more memory and tessellation time. |
| Height | `height` | Float | 1 | port | This parameter is used as the initial displacement before the zero_value and height_multiplier are applied. |
| Zero Value | `zero_value` | Float | 0 | knob only | This value is added to the displacement before applying the height_multiplier. |
| Height Multiplier | `height_multiplier` | Float | 1 | port | Multiply the computed (post zero-value) height with this factor. |
