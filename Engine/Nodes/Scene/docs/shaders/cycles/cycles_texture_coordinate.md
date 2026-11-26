# Texture Coordinate

Cycles utility `cycles_texture_coordinate`, for the cycles target.

Generated from the Cycles shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| generated | point |
| normal | normal |
| UV | point |
| object | point |
| camera | point |
| window | point |
| reflection | normal |

## Object

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| From Dupli | `from_dupli` | boolean | off | knob only | From Dupli |
| Use Transform | `use_transform` | boolean | off | knob only | Use Transform |
| Ob Tfm | `ob_tfm` | transform | 1 0 0 0 0 1 0 0 0 0 1 0 | knob only | Object Transform |
