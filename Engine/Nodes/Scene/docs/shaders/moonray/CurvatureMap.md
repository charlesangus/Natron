# Curvature Map

MoonRay map `CurvatureMap`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Map |

## Controls

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Invert | `invert` | Bool | off | knob only | Flips the value of the curvature. |
| Power | `power` | Float | 0.5 | knob only | Raises the value of the curvature to the given power. |
| Scale | `scale` | Float | 1 | knob only | Multiplies the value of the curvature. Results are still clamped between 0 and 1. |
| Mode | `mode` | Int | all | knob only | The composite mode outputs the composite of convex curvature and concave curvature as grayscale ((concave - convex) * 0.5) + 0.5. The all mode outputs the convex curvature in the red channel, concave curvature in the green channel, and composite of both curvatures in the blue channel. Options: convex, concave, composite, all. |
