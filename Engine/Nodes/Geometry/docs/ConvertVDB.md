# ConvertVDB

Tolerance: none recorded.

ConvertVDB converts the VDB primitives on its Source input. It can turn them into polygons along a surface, or switch them between signed distance fields (SDFs) and fog volumes. Other geometry passes through unchanged, and so do VDB primitives the group leaves out.

## Inputs

- **Source** is the geometry holding the VDB primitives.
- **Reference** is optional geometry the polygons can take point attributes from.

## Parameters

- **Group** chooses the VDB primitives to convert by name. It takes space-separated patterns, with `^` in front of one to leave out what it matches. `*` converts every one.
- **Convert To** chooses what the selected grids become. A grid already of the chosen kind is left as it is.
  - **Polygons** replaces each grid, and the point it sits on, with quads and triangles along the surface. The polygons face out of the inside: an SDF is inside where it is below the isovalue, and a fog volume where it is above it.
  - **Fog** turns an SDF into a fog volume, 1 inside and 0 outside, ramping across the interior band. The grid keeps its name.
  - **SDF** turns a fog volume into a narrow-band SDF whose surface is where the density crosses Fog Isovalue. The grid keeps its name.
- **Isovalue** is the level of the surface in grids that are not fog volumes. For an SDF it is a distance in world units: above 0 the surface grows outwards, below 0 it shrinks.
- **Fog Isovalue** is the density at the surface of a fog volume. It is used both for polygons and for the conversion to an SDF.
- **Adaptivity** controls how many polygons the surface gets. At 0, every polygon is about a voxel in size. Towards 1, flat regions are merged into fewer, larger polygons, while curved regions keep their detail.
- **Reference Attributes** chooses the point attributes copied from the Reference input. Each point of the polygons takes the values of the closest reference point. The parameter takes the same name patterns as Group. Without a Reference input it does nothing.

Only float and double grids can be converted. Other grids, such as vector grids, are left as they are, and the node's info names them.
