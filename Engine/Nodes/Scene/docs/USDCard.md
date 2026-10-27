# USDCard

Defines a `Mesh` at Prim Path (default `/` and the node's name, such as
`/USDCard1`, following renames until the path is edited). A unit card (1 by 1), centred on the origin and
facing the forward axis (+Z with Y up, -Y with Z up), divided into Rows by
Columns quads. The mesh is always the unit card; the width comes from the
transform: a new card's Scale X is the project format's width, times its pixel
aspect, over its height, read when the node is created, so it starts at the
format's proportions. `st` runs from (0, 0) at
the bottom left to (1, 1) at the top right as seen from the front, so an image
textured on it shows the right way round.

Rows and Columns are at least 1 (default 8).

Every face winds counter-clockwise seen from outside. Normals are authored
faceVarying, so edges stay hard where the shape has them, and texture coordinates
as an indexed faceVarying `st` primvar. The subdivision scheme is `none`. Double
Sided sets the mesh's `doubleSided`, on by default so the back is visible.

Rows and Columns are not animatable, so the topology is the same on every frame,
and the points and extent are authored as defaults.

The transform is translate, rotate in the chosen order (degrees) and scale (scale
x Uniform Scale) about Pivot, authored as one `xformOp:transform` matrix op on the
mesh, as in USDXformPrim.

The optional Material input takes a scene holding a material, such as USDMaterial's.
Its root prims are grafted under the mesh and the first material among them is
bound to it; links leaving the grafted roots are dropped and named in a node
warning. Unplugged, the mesh is unbound.

The shape is built Y-up and turned to the project's Up Axis: with Z up, Y-up
(x, y, z) becomes (x, -z, y), so the forward axis is -Y.

A Prim Path that is not an absolute prim path sets a node error and the scene
action fails.

Tolerance: none recorded.
