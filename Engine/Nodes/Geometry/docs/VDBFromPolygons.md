# VDBFromPolygons

Tolerance: none recorded.

VDBFromPolygons turns the polygons on its Source input into a volume, held in a VDB primitive. The volume is either a signed distance field (SDF), which stores how far each voxel is from the surface, or a fog volume, which stores a density. Point, vertex and primitive attributes can be carried into extra grids alongside it. The output holds only the VDB primitives, one per grid, and none of the source geometry.

Polygons with three or more vertices are used; polylines, packed primitives and VDB primitives are ignored. Polygons with more than three vertices are split into triangles first.

## Inputs

- **Source** is the geometry to convert. With nothing connected, or with no polygons, the output is one empty grid and no error.

## Parameters

- **Name** is the name of the distance or density grid. Downstream nodes such as ConvertVDB select grids by it. Attribute grids are named after their attributes.
- **Output Type** chooses what the volume holds:
  - **SDF** stores the distance to the surface in world units, negative inside and positive outside, in a narrow band around the surface. Away from the band, voxels hold the band's edge value.
  - **Fog** stores a density of 1 inside and 0 outside, ramping between them across the interior band.
- **Use Voxel Count** sizes the voxels from the source instead of from Voxel Size. **Voxel Count** voxels then span the source's bounds along **Voxel Count Axis** (X, Y, Z or the longest of the three). When the source's points have no extent along that axis, nothing is made and the info says so.
- **Voxel Size** is the edge length of a voxel in world units. Smaller voxels give more detail, and memory grows with the cube of the change: halving the voxel size makes about eight times as many voxels. There is no upper limit. The node's info lists every grid with its active voxel count and the memory it uses.
- **Exterior Band** and **Interior Band** set how far outside and inside the surface distances are stored, in voxels. Both are at least 1. A wider band costs memory but gives later operations more room, for example when growing the surface. For a fog volume the interior band is the width of the density ramp.
- **Fill Interior** stores distances through the whole inside of the mesh instead of stopping at the interior band. It needs a closed mesh.
- **Attributes** chooses attributes to carry into grids of their own. It is a list of space-separated name patterns such as `Cd uv*`, where `^` in front of a pattern leaves out what it matches. Each voxel of the band holds the attribute's value at the closest point of the surface.
  - Float attributes with one component make float grids, and those with three components make vector grids.
  - Integer attributes make integer grids, taking the value of the closest corner rather than a blend.
  - Point and vertex values are blended across each triangle. Primitive values are taken as they are.
  - When several classes have an attribute of the same name, a vertex attribute is used before a point attribute, and a point attribute before a primitive one.
  - `P` is never carried. Strings, arrays and other tuple sizes are skipped, and the info names them.

## Open meshes

A signed distance field needs an inside, so the mesh must be closed: every edge shared by two polygons. When the mesh has boundary edges, the SDF output stores unsigned distances instead. These are positive on both sides of the surface, Fill Interior has no effect, and the grid is not marked as a level set. The info explains this and suggests the alternatives:

- Choose Fog output, which guesses an inside so it can still make a density.
- Turn Fill Interior off.
- Close the mesh.
