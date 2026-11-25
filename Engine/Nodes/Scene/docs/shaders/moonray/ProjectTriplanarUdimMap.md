# ProjectTriplanarUdimMap

MoonRay map `ProjectTriplanarUdimMap`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Map |

## Controls

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Use Correct UV | `use_correct_uv` | Bool | off | knob only | use correct uv orientation |
| Use Reference Space | `use_reference_space` | Bool | off | knob only | use reference space |
| X Offset | `x_offset` | Vec2f | 0 0 | knob only | 2D offset for x projected map |
| X Scale | `x_scale` | Vec2f | 1 1 | knob only | 2D scale for x projected map |
| X Rotation | `x_rotation` | Float | 0 | knob only | 2D rotation for x projected map |
| X Rotation Center | `x_rotation_center` | Vec2f | 0.5 0.5 | knob only | 2D rotation center for x projected map |
| Y Offset | `y_offset` | Vec2f | 0 0 | knob only | 2D offset for y projected map |
| Y Scale | `y_scale` | Vec2f | 1 1 | knob only | 2D scale for y projected map |
| Y Rotation | `y_rotation` | Float | 0 | knob only | 2D rotation for y projected map |
| Y Rotation Center | `y_rotation_center` | Vec2f | 0.5 0.5 | knob only | 2D rotation center for y projected map |
| Z Offset | `z_offset` | Vec2f | 0 0 | knob only | 2D offset for z projected map |
| Z Scale | `z_scale` | Vec2f | 1 1 | knob only | 2D scale for z projected map |
| Z Rotation | `z_rotation` | Float | 0 | knob only | 2D rotation for z projected map |
| Z Rotation Center | `z_rotation_center` | Vec2f | 0.5 0.5 | knob only | 2D rotation center for z projected map |
| Projection Mode | `projection_mode` | Int | projector | knob only | Source parameters to use for projection transform Options: projector, projection_matrix, TRS. |
| Projector | `projector` | SceneObject* |  | port only | The object whose transform to use for projection when 'projection_mode' is set to 'projector' |
| Projection Matrix | `projection_matrix` | Mat4d | 1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1 | knob only | The transform to use for projection when 'projection_mode' is set to 'projection_matrix' |
| TRS Order | `TRS_order` | Int | Scale Rot Trans | knob only | Order in which to apply transformations when 'projection_mode' is set to 'TRS' Options: Scale Rot Trans, Scale Trans Rot, Rot Scale Trans, Rot Trans Scale, Trans Scale Rot, Trans Rot Scale. |
| Rotation Order | `rotation_order` | Int | xyz | knob only | Order in which to apply rotation transformations when 'projection_mode' is set to 'TRS' Options: xyz, xzy, yxz, yzx, zxy, zyx. |
| Translate | `translate` | Vec3d | 0 0 0 | knob only | Translation of the projection transform when 'projection_mode' is set to 'TRS' |
| Rotate | `rotate` | Vec3d | 0 0 0 | knob only | Rotation of the projection transform when 'projection_mode' is set to 'TRS' |
| Scale | `scale` | Vec3d | 1 1 1 | knob only | Scale of the projection transform when 'projection_mode' is set to 'TRS' |
