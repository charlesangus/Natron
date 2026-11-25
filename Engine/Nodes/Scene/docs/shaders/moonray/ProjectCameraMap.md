# ProjectCameraMap

MoonRay map `ProjectCameraMap`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Map |

## Controls

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Projector | `projector` | SceneObject* |  | port only |  |
| Project On Back Faces | `project_on_back_faces` | Bool | off | knob only | Toggles whether camera projections appear on back faces. |
| Black Outside Projection | `black_outside_projection` | Bool | off | knob only | Toggles whether projections appear outside the 0-1 uv range of the projector |
| Use Reference Space | `use_reference_space` | Bool | off | knob only | use reference space |
| Use Custom Window Coordinates | `use_custom_window_coordinates` | Bool | off | knob only | used to apply non-uniform scaling to projection |
| Window X Min | `window_x_min` | Float | -1 | knob only | minimum projected x coordinate |
| Window X Max | `window_x_max` | Float | 1 | knob only | maximum projected x coordinate |
| Window Y Min | `window_y_min` | Float | -1 | knob only | minimum projected y coordinate |
| Window Y Max | `window_y_max` | Float | 1 | knob only | maximum projected y coordinate |
