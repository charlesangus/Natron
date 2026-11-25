# UsdPrimvarReader_float3

MoonRay map `UsdPrimvarReader_float3`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Map |

## Controls

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Fallback | `fallback` | Vec3f | 0 0 0 | port | fallback value to be returned if geometry fetch failed. |
| Varname | `varname` | String |  | knob only | Name of the primvar to be read from the mesh |
| Warn When Unavailable | `warn_when_unavailable` | Bool | off | knob only | Whether or not to issue a warning when the requested attribute is unavailable |
