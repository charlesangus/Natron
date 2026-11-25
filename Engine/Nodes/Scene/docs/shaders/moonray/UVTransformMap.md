# UVTransformMap

MoonRay map `UVTransformMap`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Map |

## Controls

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Space | `space` | Int | texture | knob only | Determines source of coordinates. Default is mesh UVs. Other options procedurally create UVs from the chosen space. Options: render, camera, world, screen, object, reference, texture. |
| Offset | `offset` | Vec2f | 0 0 | port | Shifts UV coordinates |
| Scale | `scale` | Vec2f | 1 1 | port | Scales UV coordinates (after rotation, before offset) |
| Rotation Angle | `rotation_angle` | Float | 0 | knob only | Rotation in degrees |
| Rotation Center | `rotation_center` | Vec2f | 0.5 0.5 | knob only | UV coordinate around which to rotate |
| Rotation Axis | `rotation_axis` | Vec3f | 0 0 1 | knob only | Axis in which to rotate (only for 3D spaces) |
