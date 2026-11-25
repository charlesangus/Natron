# VectorDisplacement

MoonRay displacement `VectorDisplacement`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Displacement |

## Controls

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Bound Padding | `bound_padding` | Float | 0 | knob only | bound padding defines how much to extend the bounding box of the object. Keep this value as low as possible unless the geometry skips tessellation because control cage bounding box is out of camera frustum but the displacement stretch out of the original object bounding box (pre-displacement). Setting the bound padding too large will consume more memory and tessellation time. |
| Vector | `vector` | Vec3f | 0 0 0 | port | The direction to apply the displacement.  Bind a map with tangent or object space vector values here. |
| Factor | `factor` | Float | 1 | knob only | A multiplier applied to the input vector |
| Source Space | `source_space` | Int | tangent | knob only | The space that the map bound to the vector parameter is in Options: tangent, object. |
| Tangent Space Style | `tangent_space_style` | Int | tnb | knob only | Controls how RGB maps to Tangent, Normal, and Bi-Normal Options: tnb, tbn. |
