# Ambient Occlusion

Cycles utility `cycles_ambient_occlusion`, for the cycles target.

Generated from the Cycles shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| color | color |
| ao | float |

## Settings

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Samples | `samples` | int | 16 | knob only | Samples |
| Inside | `inside` | boolean | off | knob only | Inside |
| Only Local | `only_local` | boolean | off | knob only | Only Local |

## Inputs

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Color | `color` | color | 1 1 1 | port | Color |
| Distance | `distance` | float | 1 | port | Distance |
| Normal | `normal` | normal | 0 0 0 | port | Normal |
