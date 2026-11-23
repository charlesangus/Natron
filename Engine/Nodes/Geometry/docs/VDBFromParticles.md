# VDBFromParticles

Tolerance: none recorded.

VDBFromParticles turns the points on its Source input into a volume, held in a VDB primitive. Each point becomes a sphere and the spheres are merged into one surface. The volume is either a signed distance field (SDF) or a fog volume. Point attributes can be carried into extra grids alongside it. The output holds only the VDB primitives, one per grid. Every live point is used, whether or not a primitive uses it.

## Inputs

- **Source** is the geometry whose points to convert. With nothing connected, or with no points, the output is one empty grid and no error.

## Parameters

- **Name** is the name of the distance or density grid. Attribute grids are named after their attributes.
- **Output Type** chooses what the volume holds:
  - **SDF** stores the distance to the spheres' surface in world units, negative inside, in a narrow band.
  - **Fog** stores a density of 1 inside and 0 outside.
- **Voxel Size** is the edge length of a voxel in world units. Memory grows with the cube of the change in voxel size, and there is no upper limit. The node's info lists every grid with its active voxel count and the memory it uses.
- **Radius Scale** multiplies each point's `pscale` attribute to give its sphere's radius in world units. A point without `pscale` counts as 1, so the radius is Radius Scale itself.
- **Min Radius** is the smallest sphere radius that is drawn, in voxels. Smaller spheres are left out, because they would fall between the voxels, and the info counts how many were skipped. Lower Min Radius or Voxel Size to keep them.
- **Band Width** sets how far each side of the surface distances are stored, in voxels.
- **Trails** stretches each sphere backwards along the point's velocity `v` with a run of spheres that shrink towards Min Radius. The trail is `v` times **Velocity Scale** long, in world units. **Trail Spacing** is the gap between the trail's spheres, in radii: smaller gaps give a smoother trail and take longer. Points with no velocity get no trail.
- **Attributes** chooses point attributes to carry into grids of their own. It takes space-separated name patterns, with `^` in front of one to leave out what it matches. Each voxel holds the value of the point whose sphere surface is closest. Float attributes with one component make float grids, three components make vector grids, and integer attributes make integer grids. Other attributes are skipped, and the info names them.
