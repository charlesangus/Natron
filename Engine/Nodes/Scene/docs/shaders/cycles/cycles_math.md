# Math

Cycles utility `cycles_math`, for the cycles target.

Generated from the Cycles shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| value | float |

## Settings

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Math Type | `math_type` | enum | add | knob only | Type Options: add, subtract, multiply, divide, sine, cosine, tangent, arcsine, arccosine, arctangent, power, logarithm, minimum, maximum, round, less_than, greater_than, modulo, absolute, arctan2, floor, ceil, fraction, sqrt, inversesqrt, sign, exponent, radians, degrees, sinh, cosh, tanh, trunc, snap, wrap, compare, multiply_add, pingpong, smoothmin, smoothmax, floored_modulo. |
| Use Clamp | `use_clamp` | boolean | off | knob only | Use Clamp |

## Inputs

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Value 1 | `value1` | float | 0.5 | port | Value1 |
| Value 2 | `value2` | float | 0.5 | port | Value2 |
| Value 3 | `value3` | float | 0 | port | Value3 |
