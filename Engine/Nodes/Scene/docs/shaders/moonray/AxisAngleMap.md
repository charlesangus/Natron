# AxisAngleMap

MoonRay map `AxisAngleMap`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Map |

## Controls

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Input Vector | `input_vector` | Vec3f | 0 0 1 | port | input vector to be rotated |
| Input Space | `input_space` | Int | render | knob only | the space to transform from Options: render, camera, world, screen, object. |
| Rotation Axis | `rotation_axis` | Vec3f | 0 1 0 | port | axis to be rotated around |
| Axis Space | `axis_space` | Int | world | knob only | the space of the axis to rotate about Options: world, object. |
| Angle | `angle` | Float | 0 | port | the angle of rotation in degrees |
| Output Space | `output_space` | Int | render | knob only | the space to transform the resulting vector to Options: render, camera, world, screen, object. |
