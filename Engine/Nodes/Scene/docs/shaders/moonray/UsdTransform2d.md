# UsdTransform2d

MoonRay map `UsdTransform2d`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Map |

## Controls

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| In | `in` | Rgb | 1 1 1 | port | This input provides the data. It is usually connected to a UsdPrimvarReader_float2 that will provide the data. |
| Rotation | `rotation` | Float | 0 | knob only | Counter-clockwise rotation in degrees around the origin to be applied to all components of the data. |
| Scale | `scale` | Vec2f | 1 1 | knob only | Scale around the origin to be applied to all components of the data. |
| Translation | `translation` | Vec2f | 0 0 | knob only | Translation to be applied to all components of the data. |
