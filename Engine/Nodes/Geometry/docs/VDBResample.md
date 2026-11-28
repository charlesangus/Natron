# VDBResample

Tolerance: none recorded.

VDBResample moves the VDB primitives on its Source input onto different voxels, or moves their values by a transform. Larger voxels make a grid lighter and coarser, smaller voxels make it finer, and matching another grid's voxels makes the two line up voxel for voxel. Other geometry passes through unchanged, and so do VDB primitives the group leaves out. Each grid keeps its name.

With Rebuild SDF on, a signed distance field marked as a level set is rebuilt from its surface, with the same band width in voxels, so it stays a clean distance field. Other grids are sampled with the chosen interpolation.

## Inputs

- **Source** is the geometry holding the VDB primitives.
- **Reference** is optional geometry holding the VDB primitive that gives the voxels in Reference mode.

## Parameters

- **Group** chooses the VDB primitives to resample by name. It takes space-separated patterns, with `^` in front of one to leave out what it matches. `*` resamples every one.
- **Reference VDB** chooses, in Reference mode, the VDB primitive of the Reference input by name pattern; the first that matches is used. `*` by default.
- **Interpolation** chooses how grids are sampled:
  - **Point** takes the closest voxel's value. It is the fastest and looks blocky.
  - **Linear** blends the eight closest voxels.
  - **Quadratic** blends the 27 closest voxels. It is smoother and slower.
  Integer grids always take the closest voxel's value, so integer attribute grids stay in line with the surface.
- **Mode** chooses where the new voxels come from:
  - **Voxel Size** uses voxels of **Voxel Size** in world units. Each grid keeps its orientation and origin.
  - **Reference** uses the voxels of the Reference VDB. Without one, the node shows an error.
  - **Voxel Scale** scales each grid's own voxels by **Voxel Scale**. At 2 the voxels are twice as large and the grid has about an eighth as many.
  - **Explicit** moves each grid's values by the transform below, in index space. The grids keep their voxels.
- **Force Axis Aligned**, in Voxel Size and Voxel Scale modes, puts the new voxels on a fresh transform aligned with the world axes, at the grid's origin, instead of the grid's own transform scaled, which may be rotated or tapered. Off by default.
- **Transform Order** is the order the scale, rotation and translation apply in, first to last. Scale Rotate Translate by default.
- **Rotate Order** is the order the axis rotations apply in, first to last. XYZ by default.
- **Translate Voxels** is the translation, in voxels.
- **Rotate** is the rotation about each axis, in degrees.
- **Scale** is the scale along each axis. It cannot be 0.
- **Pivot** is the point, in index space, that the rotation and scale are about.
- **Transform Vectors**, in Explicit mode, transforms the values of vector grids too, as their vector type says. Off by default.
- **Rebuild SDF** rebuilds SDFs instead of sampling them. On by default.
- **Prune Tolerance** makes voxels within this of the background, which resampling leaves active, inactive, then collapses regions of values within it of each other into tiles. 0 by default.

Float, double, vector and integer grids can be resampled. Other grids are left as they are, and the node's info names them.
