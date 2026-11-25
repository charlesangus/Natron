# OpenVdbMap_v2

MoonRay map `OpenVdbMap_v2`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Map |

## Advanced

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Show Active Field | `show_active_field` | Bool | off | knob only | When enabled active/inactive field locations will be white/black, respectively |

## Other

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Vdb Source | `vdb_source` | Int | from texture | knob only | Where to look for the vdb filename.  Choose 'from texture' to specify a .vdb filename directly via the 'texture' attribute.  Choose 'from OpenVdbGeometry' to use the .vdb filename and transform from an OpenVdbGeometry object in the scene using the 'openvdb geometry' attribute Options: from texture, from OpenVdbGeometry. |
| Texture | `texture` | String |  | knob only |  |
| Openvdb Geometry | `openvdb_geometry` | SceneObjectVector |  | port only | The OpenVdbGeometry object(s) from which to retrieve the .vdb filename and transform when 'vdb source' is set to 'from OpenVdbGeometry' |
| Grid Name | `grid_name` | String |  | knob only | The name of the grid within the .vdb file from which to sample (hint: use openvdb_print to see contents of .vdb file). If no grid is specified, the first grid found in the .vdb will be used.  In cases where there are multiple grids with the same name, the grid name can be indexed (eg. density[1]) |
| Texture Coordinates | `texture_coordinates` | Int | position | knob only | Which coordinate source to use for the texture lookup Options: position, reference position, input texture coordinates. |
| Input Texture Coordinates | `input_texture_coordinates` | Vec3f | 0 0 0 | port | The world-space coordinate to use when 'texture coordinates' is set to 'input texture coordinates' |
| Interpolation | `interpolation` | Int | quadratic | knob only | The type of interpolation to use when sampling the vdb Options: point, box, quadratic. |
| Default Value | `default_value` | Rgb | 0 0 0 | knob only | A default value to use when A) the .vdb file is not found, B) the requested grid is not found, C) the grid is unspecified, but no grid is found |
| Show Warnings | `show_warnings` | Bool | off | knob only | Enables a warning message when A) the .vdb file is not found, B) the requested grid is not found, C) the grid is unspecified, but no grid is found |
