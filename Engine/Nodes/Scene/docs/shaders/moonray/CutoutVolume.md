# Cutout Volume

MoonRay volume `CutoutVolume`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Volume |

## Volume Baking

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Bake Resolution Mode | `bake_resolution_mode` | Int | default | knob only | Method to specify grid resolution of baked density grid.  Choices are: 		"default": For shaders that are bound to vdb volumes, use vdb resolution. 						For shaders that are bounds to mesh geometries use 100 divisions 		"divisions": Specify number of divisions. 		"voxel size": Specify voxel size. Options: default, divisions, voxel size. |
| Bake Divisions | `bake_divisions` | Int | 100 | knob only | Divide widest axis by this many divisions |
| Bake Voxel Size | `bake_voxel_size` | Float | 10 | knob only | Size of voxel in world space |

## Volume

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Surface Opacity Threshold | `surface_opacity_threshold` | Float | 0.5 | knob only | Accumulated opacity that's considered the 'surface' for computing surface position and Z |

## Other

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Label | `label` | String |  | knob only | label used in light aovs |
| Indirect Volume | `indirect_volume` | SceneObject* |  | port only | The volume to cutout / use for indirect illumination and occlusion.  Cutout behavior is invoked for primary rays but secondary/indirect rays are processed normally. |
