# Project Camera Map V 2

MoonRay map `ProjectCameraMap_v2`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Map |

## Controls

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Projector | `projector` | SceneObject* |  | port only | The camera to project from |
| Project On Back Faces | `project_on_back_faces` | Bool | off | knob only | Toggles whether camera projections appear on back faces. |
| Black Outside Projection | `black_outside_projection` | Bool | on | knob only | Toggles whether projections appear outside the 0-1 uv range of the projector |
| Use Reference Space | `use_reference_space` | Bool | off | knob only | Use reference space |
| Texture | `texture` | String |  | knob only | Filename that points to a texture .exr or .tx file (must be mip-mapped and tiled with maketx). |
| Alpha Only | `alpha_only` | Bool | off | knob only | When enabled, the alpha channel is returned instead of RGB |
| Unpremultiply | `unpremultiply` | Bool | off | knob only | When enabled, the rgb channels are divided by the alpha channel (where non-zero) |
| Aspect Ratio Source | `aspect_ratio_source` | Int | from texture | knob only | Whether to use the image and pixel aspect ratio of the texture being projected, or a custom aspect ratio Options: from texture, custom. |
| Custom Aspect Ratio | `custom_aspect_ratio` | Float | 1 | knob only | Custom aspect ratio for the projected texture |
| Gamma | `gamma` | Int | auto | knob only | Controls application of gamma to images (off -0, on - 1, auto - 2).   Auto will apply gamma decoding to 8-bit images Options: off, on, auto. |
