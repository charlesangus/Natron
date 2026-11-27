# VDBFromParticles

Tolerance: none recorded.

VDBFromParticles turns the points on its Source input into a volume, held in a VDB primitive. Each point becomes a sphere and the spheres are merged into one surface. The output holds a signed distance field (SDF), a fog volume and a mask, each when asked for. Point attributes can be carried into extra grids alongside it. The output holds only the VDB primitives, one per grid. Every live point is used, whether or not a primitive uses it.

## Inputs

- **Source** is the geometry whose points to convert. With nothing connected, or with no points, the output is an empty grid for each volume asked for, and no error.
- **Reference** is optional geometry holding a VDB primitive whose voxels the volumes take. With it, Voxel Size is ignored.

## Parameters

- **Reference VDB** chooses the VDB primitive of the Reference input, by name pattern; the first that matches is used. When none matches, Voxel Size is used and the info says so.
- **Merge With Reference VDB** adds the spheres to the reference VDB's own surface instead of starting empty. It needs a float SDF; for other grids the info says the merge was skipped.
- **Distance VDB** makes the distance grid, named **Distance Name** (`surface` by default): the distance to the spheres' surface in world units, negative inside, in a narrow band. On by default.
- **Fog VDB** makes a fog grid, named **Fog Name** (`density` by default): 1 inside and 0 outside. Off by default.
- **Mask VDB** makes a mask grid, named **Mask Name** (`boundingvolume` by default): a fog volume of 1 in a shell around each sphere's surface, **Mask Width Scale** radii wide (1 by default). It limits later smoothing of the surface to where the points are. Off by default.
- **Voxel Size** is the edge length of a voxel in world units. Memory grows with the cube of the change in voxel size, and there is no upper limit. Before building, the node's info shows an estimate of the active voxels and memory, and afterwards it lists every grid with its active voxel count and the memory it uses.
- **Radius Scale** multiplies each point's `pscale` attribute to give its sphere's radius in world units. A point without `pscale` counts as 1, so the radius is Radius Scale itself.
- **Min Radius** is the smallest sphere radius that is drawn, in voxels. Smaller spheres are left out, because they would fall between the voxels, and the info counts how many were skipped. Lower Min Radius or Voxel Size to keep them.
- **Prune** collapses regions of equal values into tiles when the spheres are drawn, which saves memory.
- **Use World Space For Band** gives the band in world units with **Band Width World** (0.3) instead of in voxels with **Band Width** (3). Either sets how far each side of the surface distances are stored.
- **Particle Footprint** chooses the shape each point leaves:
  - **Spherical** draws one sphere per point.
  - **Velocity Trail** also stretches each sphere backwards along the point's velocity `v` with a run of spheres that shrink towards Min Radius. The trail is `v` times **Velocity Scale** long, in world units. **Trail Spacing** is the gap between the trail's spheres, in half-radii: smaller gaps give a smoother trail and take longer. Points with no velocity get no trail.
- **Attributes** chooses point attributes to carry into grids of their own. It takes space-separated name patterns, with `^` in front of one to leave out what it matches. Each voxel holds the value of the point whose sphere surface is closest. Float attributes with one component make float grids, three components make vector grids, and integer attributes make integer grids. Other attributes are skipped, and the info names them.
