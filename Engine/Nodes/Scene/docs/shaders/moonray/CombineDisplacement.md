# Combine Displacement

MoonRay displacement `CombineDisplacement`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Displacement |

## Controls

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Bound Padding | `bound_padding` | Float | 0 | knob only | bound padding defines how much to extend the bounding box of the object. Keep this value as low as possible unless the geometry skips tessellation because control cage bounding box is out of camera frustum but the displacement stretch out of the original object bounding box (pre-displacement). Setting the bound padding too large will consume more memory and tessellation time. |
| Operation | `operation` | Int | add | knob only | The method used for combining the displacements Options: add, max magnitude, min magnitude. |
| Input 1 | `input_1` | SceneObject* |  | port only | Displacement object 1 |
| Scale 1 | `scale_1` | Float | 1 | port | Scale of input 1 |
| Input 2 | `input_2` | SceneObject* |  | port only | Displacement object 2 |
| Scale 2 | `scale_2` | Float | 1 | port | Scale of input 2 |
