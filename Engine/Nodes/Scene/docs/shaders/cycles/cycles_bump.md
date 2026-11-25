# Bump

Cycles utility `cycles_bump`, for the cycles target.

Generated from the Cycles shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| normal | normal |

## Settings

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Invert | `invert` | boolean | off | knob only | Invert |
| Use Object Space | `use_object_space` | boolean | off | knob only | UseObjectSpace |

## Inputs

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Height | `height` | float | 1 | port | Height |
| Sample Center | `sample_center` | float | 0 | port | SampleCenter |
| Sample X | `sample_x` | float | 0 | port | SampleX |
| Sample Y | `sample_y` | float | 0 | port | SampleY |
| Normal | `normal` | normal | 0 0 0 | port | Normal |
| Strength | `strength` | float | 1 | port | Strength |
| Distance | `distance` | float | 0.1 | port | Distance |
| Filter Width | `filter_width` | float | 0.1 | port | Filter Width |
