# GradientMap

MoonRay map `GradientMap`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Map |

## Gradient properties

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Color A | `color_A` | Rgb | 0 0 0 | port | Start color |
| Color B | `color_B` | Rgb | 1 1 1 | port | End color |
| Start | `start` | Vec3f | 0 0 0 | knob only | Start position in the chosen space |
| End | `end` | Vec3f | 0 1 0 | knob only | End position in the chosen space |
| Space | `space` | Int | render | knob only | The transformation space in which to perform the blending Options: render, camera, world, screen, object, reference, texture. |
| Object | `object` | SceneObject* |  | port only | Use the provided object's transformation space (only used if object space is also specified) |

## Falloff properties

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Falloff Type | `falloff_type` | Int | natural | knob only | Falloff blend mode Options: none, natural, linear, squared, gaussian, ease out. |
| Falloff Exponent | `falloff_exponent` | Float | 1 | knob only | Adjusts rate of blending |
| Falloff Start | `falloff_start` | Float | 0 | knob only | Shifts where the falloff starts |
| Falloff End | `falloff_end` | Float | 1 | knob only | Shifts where the falloff ends |
| Falloff End Intensity | `falloff_end_intensity` | Float | 1 | knob only | Adjust the intensity of the end color |
| Falloff Bias | `falloff_bias` | Float | 0.5 | knob only | Compresses the blending towards the start or end color |

## Additional properties

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Symmetric | `symmetric` | Bool | off | knob only | Color A blends into Color B and then back into Color A from the start to the end point |
| Symmetric Center | `symmetric_center` | Float | 0.5 | knob only | Shifts the center of the symmetric falloff |
