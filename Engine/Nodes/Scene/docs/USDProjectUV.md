# USDProjectUV

Writes texture coordinates on the meshes of its Scene input that Prim Pattern
names, by projecting their points through a camera, as Nuke's UVProject does.
The pattern is a list of absolute prim paths separated by spaces, whose names may
use `*` and `?`, as in USDEditTransform. The result is a `faceVarying` primvar
named by Primvar Name (`st` by default, which `UsdPreviewSurface` textures read),
indexed per face vertex, authored as an over on each mesh. Texture it with a
USDMaterial. The UVs are per vertex, so a projection onto coarse geometry warps
between vertices; subdivide the mesh where that shows.

The camera comes from the Camera input, read where it is wired, or from the Scene
input when the Camera input is not connected. Camera lists that scene's cameras;
until one is chosen the first is used. A chosen camera the scene does not have is
a node error naming its path.

Projection:

- Perspective: a point's UV is where it lands in the camera's aperture,
  conformed to Format by Fit Mode (as in USDRender): (0, 0) at the bottom left of
  the frame and (1, 1) at its top right. A point behind the camera gets the UV of
  the frame edge it is beyond, and a node warning gives the number of such
  points. Frustum Culling leaves faces wholly outside the camera's view, near and
  far clipping planes included, with the UVs the mesh already has.
- Planar: the point's position on Plane of the camera's space, U Scale and V
  Scale scene units across, centred on the camera.
- Spherical: the point's longitude about the camera's Y axis (0.5 straight
  ahead) and its latitude, bottom to top.
- Cylindrical: longitude as Spherical; V is the height along the camera's Y axis,
  0 to 1 over two scene units (scaled by V Scale).
- Off: the scene passes through unchanged.

With no camera in the scene at all, Planar, Spherical and Cylindrical work in
world space; Perspective is an error.

Project On limits the projection to faces whose normals face the camera (Front)
or face away from it (Back). Normals are the mesh's authored ones
(`primvars:normals`, else `normals`), else each face's geometric normal. Face
vertices the projection does not fall on keep the value the primvar already has,
or (0, 0) if the mesh has none. Invert U and Invert V flip the result (1 - U,
1 - V).

Use Frame Hold projects through the camera as it is at Frame Hold on every frame;
the meshes still move. When a mesh or the (unheld) camera moves, the UVs are a
time sample at every frame of the project range; otherwise a default value.

A pattern that matches nothing passes the scene through with a warning. Matched
prims that are not geometry (xforms, scopes) are skipped; matched geometry that
is not a mesh, and meshes with invalid topology, are named in a warning. The
node's knobs are not animatable.

Tolerance: none recorded.
