# ProjectCameraNormalMap

MoonRay normal_map `ProjectCameraNormalMap`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | NormalMap |

## Controls

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Projector | `projector` | SceneObject* |  | port only | The camera to project from |
| Project On Back Faces | `project_on_back_faces` | Bool | off | knob only | Toggles whether camera projections appear on back faces |
| Use Reference Space | `use_reference_space` | Bool | off | knob only | Use reference space position (ref_P) and normals (ref_N) |
| Texture | `texture` | String |  | knob only | Filename that points to a texture .exr or .tx file (must be mip-mapped and tiled with maketx). |
| Aspect Ratio Source | `aspect_ratio_source` | Int | from texture | knob only | Whether to use the image and pixel aspect ratio of the texture being projected, or a custom aspect ratio Options: from texture, custom. |
| Custom Aspect Ratio | `custom_aspect_ratio` | Float | 1 | knob only | Custom aspect ratio for the projected texture to use when 'aspect_ratio_source' is set to 'custom' |
| Normal Encoding | `normal_encoding` | Int | [0,1] | knob only | Most normal maps are encoded [0,1].   Only certain rare floating point normal maps are encoded [-1,1] Options: [0,1], [-1,1]. |
