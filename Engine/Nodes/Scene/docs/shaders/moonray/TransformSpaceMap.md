# TransformSpaceMap

MoonRay map `TransformSpaceMap`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Map |

## Controls

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Input | `input` | Vec3f | 1 1 1 | port | the input value to transform |
| Input Type | `input_type` | Int | vector | knob only | the type of input value provided Options: point, vector, normal. |
| From Space | `from_space` | Int | render | knob only | the space to transform from Options: render, camera, world, screen, object, local tangent, instance object transform, instance level 0, instance level 1, instance level 2, instance level 3, instance level 4. |
| To Space | `to_space` | Int | world | knob only | the space to transform to Options: render, camera, world, screen, object, local reference tangent, instance object transform, instance level 0, instance level 1, instance level 2, instance level 3, instance level 4. |
| Concatenate Instance Level Transforms | `concatenate_instance_level_transforms` | Bool | on | knob only | When true, instance level transforms below the specified one are concatenated otherwise only the selected level's transform is used |
| Camera | `camera` | SceneObject* |  | port only | an alternate camera to use when transforming to/from 'camera' space |
| Object | `object` | SceneObject* |  | port only | an alternate object to use when transforming to/from 'object' space |
| Use Custom Window Coordinates | `use_custom_window_coordinates` | Bool | off | knob only | used to apply non-uniform scaling to projection |
| Window X Min | `window_x_min` | Float | -1 | knob only | minimum projected x coordinate |
| Window X Max | `window_x_max` | Float | 1 | knob only | maximum projected x coordinate |
| Window Y Min | `window_y_min` | Float | -1 | knob only | minimum projected y coordinate |
| Window Y Max | `window_y_max` | Float | 1 | knob only | maximum projected y coordinate |
