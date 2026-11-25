# CombineNormalMap

MoonRay normal_map `CombineNormalMap`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | NormalMap |

## Normal

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Input 1 | `input_1` | SceneObject* |  | port only | First normal map to combine; usually a base map |
| Normal Map 1 Dial | `normal_map_1_dial` | Float | 1 | port | Amount of normal map 1 to blend in |
| Input 2 | `input_2` | SceneObject* |  | port only | Second normal map to combine; usually a base map |
| Normal Map 2 Dial | `normal_map_2_dial` | Float | 1 | port | Amount of normal map 2 to blend in |
