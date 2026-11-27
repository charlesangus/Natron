# VDBFromPolygons

Tolerance: none recorded.

VDBFromPolygons turns the polygons on its Source input into a volume, held in a VDB primitive. The volume is a signed distance field (SDF), which stores how far each voxel is from the surface, a fog volume, which stores a density, or both. Point, vertex and primitive attributes can be carried into extra grids alongside it. The output holds only the VDB primitives, one per grid, and none of the source geometry.

Polygons with three or more vertices are used; polylines, packed primitives and VDB primitives are ignored. Polygons with more than three vertices are split into triangles first.

## Inputs

- **Source** is the geometry to convert. With nothing connected, or with no polygons, the output is an empty grid for each volume asked for, and no error.
- **Reference** is optional geometry holding a VDB primitive whose voxels the volumes take: their size, orientation and origin. With it, Voxel Size and Use Voxel Count are ignored.

## Parameters

- **Reference VDB** chooses the VDB primitive of the Reference input, by name pattern; the first that matches is used. When none matches, Voxel Size is used and the info says so.
- **Distance VDB** makes the distance grid, named **Distance Name** (`surface` by default). It stores the distance to the surface in world units, negative inside and positive outside, in a narrow band around the surface. Away from the band, voxels hold the band's edge value. On by default.
- **Fog VDB** makes the fog grid, named **Fog Name** (`density` by default). It stores a density of 1 inside and 0 outside, ramping between them across the interior band. Off by default; both grids can be made at once. Downstream nodes such as ConvertVDB select grids by name, and attribute grids are named after their attributes.
- **Use Voxel Count** sizes the voxels from the source instead of from Voxel Size. **Voxel Count** voxels then span the source's bounds along **Voxel Count Axis** (X, Y, Z or the longest of the three). When the source's points have no extent along that axis, nothing is made and the info says so.
- **Voxel Size** is the edge length of a voxel in world units. Smaller voxels give more detail, and memory grows with the cube of the change: halving the voxel size makes about eight times as many voxels. There is no upper limit. Before building, the node's info shows an estimate of the active voxels and memory per grid, so a very small voxel size can be caught early; afterwards it lists every grid with its active voxel count and the memory it uses.
- **Use World Space For Band** gives the bands in world units, with **Exterior Band World** and **Interior Band World** (0.3 each), instead of in voxels with **Exterior Band** and **Interior Band** (3 each). The bands are at least one voxel.
- **Exterior Band** and **Interior Band** set how far outside and inside the surface distances are stored, in voxels. A wider band costs memory but gives later operations more room, for example when growing the surface. For a fog volume the interior band is the width of the density ramp, with or without Fill Interior.
- **Fill Interior** stores distances through the whole inside of the mesh instead of stopping at the interior band. It needs a closed mesh.
- **Unsigned Distance Field** makes the distance grid store unsigned distances: positive on both sides of the surface, with no inside, so open meshes and single sheets work. The grid is not marked as a level set, and Fill Interior has no effect on it. The fog grid still needs an inside.
- **Attributes** chooses attributes to carry into grids of their own. It is a list of space-separated name patterns such as `Cd uv*`, where `^` in front of a pattern leaves out what it matches. Each voxel of the band holds the attribute's value at the closest point of the surface.
  - Float attributes with one component make float grids, and those with three components make vector grids.
  - Integer attributes make integer grids, taking the value of the closest corner rather than a blend.
  - Point and vertex values are blended across each triangle. Primitive values are taken as they are.
  - When several classes have an attribute of the same name, a vertex attribute is used before a point attribute, and a point attribute before a primitive one.
  - `P` is never carried. Strings, arrays and other tuple sizes are skipped, and the info names them.

## Open meshes

A signed distance field needs an inside, so the mesh should be closed: every edge shared by two polygons. When the mesh has boundary edges, the inside is guessed, so the signs of the distances and the fog may leak through the holes. The info says so and suggests the alternatives:

- Turn Unsigned Distance Field on, for distances that need no inside.
- Close the mesh.
