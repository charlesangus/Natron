# ProjectPlanarNormalMap

MoonRay normal_map `ProjectPlanarNormalMap`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | NormalMap |

## Controls

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Use Reference Space | `use_reference_space` | Bool | off | knob only | Use reference space position (ref_P) and normals (ref_N) |
| Texture | `texture` | String |  | knob only | Filename that points to a texture .exr or .tx file (must be mip-mapped and tiled with maketx). |
| Wrap Around | `wrap_around` | Bool | on | knob only | Controls whether to repeat (true) or clamp (false) the texture |
| Normal Encoding | `normal_encoding` | Int | [0,1] | knob only | Most normal maps are encoded [0,1].   Only certain rare floating point normal maps are encoded [-1,1] Options: [0,1], [-1,1]. |
| Projection Mode | `projection_mode` | Int | projector | knob only | Source parameters to use for projection transform Options: projector, projection_matrix, TRS. |
| Projector | `projector` | SceneObject* |  | port only | The object whose transform to use for projection when 'projection_mode' is set to 'projector' |
| Projection Matrix | `projection_matrix` | Mat4d | 1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1 | knob only | The transform to use for projection when 'projection_mode' is set to 'projection_matrix' |
| TRS Order | `TRS_order` | Int | Scale Rot Trans | knob only | Order in which to apply transformations when 'projection_mode' is set to 'TRS' Options: Scale Rot Trans, Scale Trans Rot, Rot Scale Trans, Rot Trans Scale, Trans Scale Rot, Trans Rot Scale. |
| Rotation Order | `rotation_order` | Int | xyz | knob only | Order in which to apply rotation transformations when 'projection_mode' is set to 'TRS' Options: xyz, xzy, yxz, yzx, zxy, zyx. |
| Translate | `translate` | Vec3d | 0 0 0 | knob only | Translation of the projection transform when 'projection_mode' is set to 'TRS' |
| Rotate | `rotate` | Vec3d | 0 0 0 | knob only | Rotation of the projection transform when 'projection_mode' is set to 'TRS' |
| Scale | `scale` | Vec3d | 1 1 1 | knob only | Scale of the projection transform when 'projection_mode' is set to 'TRS' |
