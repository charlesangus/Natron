# Normal Map

Cycles utility `cycles_normal_map`, for the cycles target.

Generated from the Cycles shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| normal | normal |

## Settings

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Space | `space` | enum | tangent | knob only | Space Options: tangent, object, world, blender_object, blender_world. |
| Convention | `convention` | enum | opengl | knob only | Convention Options: opengl, directx. |
| Base | `base` | enum | original | knob only | Base Options: original, displaced. |
| Attribute | `attribute` | string |  | knob only | Attribute |

## Inputs

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Strength | `strength` | float | 1 | port | Strength |
| Color | `color` | color | 0.5 0.5 1 | port | Color |
