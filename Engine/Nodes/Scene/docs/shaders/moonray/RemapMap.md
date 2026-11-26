# Remap Map

MoonRay map `RemapMap`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Map |

## Channel

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Input Min RGB | `input_min_RGB` | Rgb | 0 0 0 | port | the input value that will be remapped to the 'output min' value |
| Input Max RGB | `input_max_RGB` | Rgb | 1 1 1 | port | the input value that will be remapped to the 'output max' value |
| Output Min RGB | `output_min_RGB` | Rgb | 0 0 0 | port | the value that 'input min' is remapped to |
| Output Max RGB | `output_max_RGB` | Rgb | 1 1 1 | port | the value that 'input max' is remapped to |
| Midpoint Bias RGB | `midpoint_bias_RGB` | Rgb | 0.5 0.5 0.5 | port | biases the in-between values toward 'output min' or 'output max'. Default = 0.5 |
| Clamp Min RGB | `clamp_min_RGB` | Rgb | 0 0 0 | knob only | the minimum value output by this map when 'clamp' is enabled |
| Clamp Max RGB | `clamp_max_RGB` | Rgb | 1 1 1 | knob only | the maximum value output by this map when 'clamp' is enabled |

## Other

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Remap Method | `remap_method` | Int | uniform | knob only | Choose whether you are remapping using single values (uniform) or with separate RGB channels Options: uniform, RGB. |
| Input | `input` | Rgb | 1 1 1 | port | the input values to be remapped |
| Input Min | `input_min` | Float | 0 | port | the input value that will be remapped to the 'output min' value |
| Input Max | `input_max` | Float | 1 | port | the input value that will be remapped to the 'output max' value |
| Output Min | `output_min` | Float | 0 | port | the value that 'input min' is remapped to |
| Output Max | `output_max` | Float | 1 | port | the value that 'input max' is remapped to |
| Midpoint Bias | `midpoint_bias` | Float | 0.5 | port | biases the in-between values toward 'output min' or 'output max'. Default = 0.5 |
| Clamp | `clamp` | Bool | on | knob only | enables/disables clamping of the output values.  This useful prevent out-of-range values when expanding the input values. |
| Clamp Min | `clamp_min` | Float | 0 | knob only | the minimum value output by this map when 'clamp' is enabled |
| Clamp Max | `clamp_max` | Float | 1 | knob only | the maximum value output by this map when 'clamp' is enabled |
| Clamp RGB | `clamp_RGB` | Bool | on | knob only | enables/disables clamping of the output values.  This useful prevent out-of-range values when expanding the input values. |
