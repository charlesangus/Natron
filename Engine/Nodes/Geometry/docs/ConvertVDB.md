# ConvertVDB

Tolerance: none recorded.

ConvertVDB converts the VDB primitives on its Source input. It can turn them into polygons along a surface, or into other VDBs: between signed distance fields (SDFs) and fog volumes, or to another value type and precision, pruned and flood filled. Other geometry passes through unchanged, and so do VDB primitives the group leaves out.

## Inputs

- **Source** is the geometry holding the VDB primitives.
- **Reference** is optional geometry, usually the polygons the VDB was made from. The polygons can take its attributes and groups and keep its sharp edges.
- **Mask** is optional geometry holding VDB primitives that limit where polygons are made and how far they adapt.

## Parameters

- **Group** chooses the VDB primitives to convert by name. It takes space-separated patterns, with `^` in front of one to leave out what it matches. `*` converts every one. A grid a pattern picks up that the conversion cannot take is left as it is and named in the info; one that Group names exactly gives a warning instead, as does a Mask input VDB that is not found.
- **Convert To** chooses what the selected grids become:
  - **Polygons** replaces each grid, and the point it sits on, with quads and triangles along the surface. The polygons face out of the inside: an SDF is inside where it is below the isovalue, and a fog volume where it is above it. Only SDFs and fog volumes are turned into polygons unless Group names a grid exactly, so attribute grids and unsigned distance fields made beside an SDF are left as they are, and the info names them. The polygons do not take the VDB's name.
  - **VDB** keeps the grids as VDBs, converted as the VDB controls below say. Each grid keeps its name.

### VDB controls

- **VDB Class** chooses a class conversion:
  - **No Change** keeps each grid's class and converts its values with VDB Type and VDB Precision.
  - **Convert Fog To SDF** turns each fog volume into a narrow-band SDF, three voxels each side, whose surface is where the density crosses Fog Isovalue.
  - **Convert SDF To Fog** turns each SDF into a fog volume, 1 inside and 0 outside, ramping across its interior band.
  Grids of another class are left as they are, and the info names them.
- **VDB Type** converts the values to **Float**, **Integer**, **Vector** or **Bool**, keeping the active voxels. **Same** keeps the type. A scalar goes into each component of a vector; a vector converted to a scalar keeps only the active voxels. Only float grids stay SDFs or fog volumes. Used only with No Change.
- **VDB Precision** converts the values to **32-bit** or **64-bit**, or keeps them with **Same**. Used only with No Change.
- **Prune Tolerance** makes voxels within this of the background inactive, then collapses regions of values within it of each other into tiles. 0 by default.
- **Signed Flood Fill** gives the inactive values of an SDF the sign of the side they are on. On by default.
- **Activate Inside Voxels** makes every voxel inside an SDF's surface active. It needs Signed Flood Fill, without which inactive values inside may still read as outside, and does nothing when that is off. Off by default.

### Polygon controls

- **Isovalue** is the level of the surface in grids that are not fog volumes. For an SDF it is a distance in world units: above 0 the surface grows outwards, below 0 it shrinks.
- **Fog Isovalue** is the density at the surface of a fog volume. It is used both for polygons and for the conversion to an SDF. 0.5 by default.
- **Adaptivity** controls how many polygons the surface gets. At 0, every polygon is about a voxel in size. Towards 1, flat regions are merged into fewer, larger polygons, while curved regions keep their detail.
- **Compute Vertex Normals** gives the polygons vertex normals `N`, smoothed between polygons that meet within 60 degrees. With a reference, the polygons on its surface and those inside it do not share normals. Off by default.
- **Internal Adaptivity** is the adaptivity, with a reference, of the regions inside its surface, such as the faces of a fracture. 0.01 by default.
- **Transfer Surface Attributes**, with a reference, gives every point the point attributes of the closest reference point, and the polygons on the reference surface the primitive and vertex attributes of the closest reference triangle, overriding computed normals there. On by default.
- **Reference Attributes** chooses the reference attributes transferred, with the same name patterns as Group. `*` by default.
- **Sharpen Features**, with a reference, moves the points of the surface polygons near its sharp edges and corners onto them: each goes to the point that best fits the planes of the reference triangles crossing its voxel, kept inside that voxel. Edge Tolerance sets which edges count as sharp. On by default.
- **Edge Tolerance**, with a reference, keeps full resolution where the reference surface turns sharper than this, as a dot product of normals, whatever the adaptivity. 0.5 by default.
- **Surface Group**, **Interior Group** and **Seam Line Group** name the primitive groups, with a reference, of the polygons on its surface, inside it and along the seams between the two. **Seam Points** names the point group of the seams' points. An empty name makes no group.
- **Use Surface Mask** makes polygons only inside the **Surface Mask** VDB of the Mask input, chosen by name pattern. Its inside is below **Mask Offset** for an SDF, above it for any other float grid, and its active voxels for other types. **Invert Surface Mask** makes polygons only outside it instead.
- **Use Adaptivity Field** scales the adaptivity by the **Adaptivity Field** VDB of the Mask input, chosen by name pattern.

Only float and double grids can be turned into polygons. Other grids, such as vector grids, are left as they are, and the node's info names them.
