# VDBResample

Tolerance: none recorded.

VDBResample moves the VDB primitives on its Source input onto different voxels. Larger voxels make a grid lighter and coarser, smaller voxels make it finer, and matching another grid's voxels makes the two line up voxel for voxel. Other geometry passes through unchanged, and so do VDB primitives the group leaves out. Each grid keeps its name.

A signed distance field marked as a level set is rebuilt at the new size, with the same band width in voxels, so it stays a clean distance field. Other grids are sampled with the chosen interpolation.

## Inputs

- **Source** is the geometry holding the VDB primitives.
- **Reference** is optional geometry whose first VDB primitive gives the voxels in Reference mode.

## Parameters

- **Group** chooses the VDB primitives to resample by name. It takes space-separated patterns, with `^` in front of one to leave out what it matches. `*` resamples every one.
- **Mode** chooses where the new voxels come from:
  - **Voxel Size** uses voxels of **Voxel Size** in world units. Each grid keeps its orientation and origin.
  - **Reference** uses the voxels of the first VDB primitive on the Reference input. Without one, the node shows an error.
  - **Voxel Scale** scales each grid's own voxels by **Voxel Scale**. At 2 the voxels are twice as large and the grid has about an eighth as many.
- **Interpolation** chooses how grids that are not level sets are sampled:
  - **Point** takes the closest voxel's value. It is the fastest and looks blocky.
  - **Linear** blends the eight closest voxels.
  - **Quadratic** blends the 27 closest voxels. It is smoother and slower.

Float, double and vector grids can be resampled. Other grids are left as they are, and the node's info names them.
