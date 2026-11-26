# LOD Map

MoonRay map `LODMap`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Map |

## Controls

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Mode | `mode` | Int | feature width | knob only | Use feature_width for LOD based on average, world-space feature-width visible in a pixel, correctly changing with resolution. Use camera_distance for LOD based on distance from render cam. Options: feature width, camera distance. |
| Start | `start` | Float | 0.01 | knob only | feature_width/camera_distance at which to start blending near_value->far_value |
| Stop | `stop` | Float | 0.1 | knob only | feature_width/camera_distance at which to stop blending near_value->far_value |
| Near Value | `near_value` | Rgb | 0 0 0 | port | value output when feature_width/camera_distance is less than or equal to start |
| Far Value | `far_value` | Rgb | 1 1 1 | port | value output when feature_width/camera_distance is more than or equal to stop |
