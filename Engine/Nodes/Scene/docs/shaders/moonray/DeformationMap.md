# Deformation Map

MoonRay map `DeformationMap`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Map |

## Controls

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Output Mode | `output_mode` | Int | deformation_avg | knob only | Controls output:  		    RGB - R = deformation along S, G = deformation along T, B = average deformation from ref space  		    deformation_S - deformation along S  		    deformation_T - deformation along T  		    deformation_avg - average deformation from ref space Options: RGB, deformation_S, deformation_T, deformation_avg. |
| Use Warning Color | `use_warning_color` | Bool | off | knob only | If derivatives are missing or zero output the warning color erroring out |
| Warning Color | `warning_color` | Rgb | 1 1 1 | port | Warning color to output when derivatives are missing or zero |
