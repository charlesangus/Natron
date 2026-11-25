# OpMap

MoonRay map `OpMap`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Map |

## Controls

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Operation | `operation` | Int | add | knob only | Operation Options: add, subtract, multiply, divide, maximum, minimum, power, cross, dot, invert op1, normalize op1, op1, op2, overlay, screen, abs, ceil, floor, modulo, fraction, length, sine, cosine, round, acos, less_than, less_than_or_equal, greater_than, greater_than_or_equal, equal, not equal, and, or, not, xor, bit_shift_left, bit_shift_right, bitwise_and, bitwise_or, vector_equal, vector_not_equal. |
| Op 1 | `op1` | Rgb | 1 1 1 | port | First operand |
| Op 2 | `op2` | Rgb | 1 1 1 | port | Second operand |
| Op 1 Factor | `op1_factor` | Float | 1 | port | Scalar multiplier on op1 |
| Op 2 Factor | `op2_factor` | Float | 1 | port | Scalar multiplier on op2 |
| Clamp | `clamp` | Bool | off | knob only | Whether to clamp result to 0 - 1 |
| Tolerance | `tolerance` | Float | 0.001 | knob only | Epsilon. Sets range for almost-equals checking of values |
