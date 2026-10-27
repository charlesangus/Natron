# USDCard

Defines a `Mesh` at Prim Path (default `/` and the node's name, such as
`/USDCard1`, following renames until the path is edited). A card one unit high and Aspect wide, centred on the origin and
facing the forward axis (+Z with Y up, -Y with Z up), divided into Rows by
Columns quads. Aspect defaults to the project format's width, times its pixel
aspect, over its height, read when the node is created. `st` runs from (0, 0) at
the bottom left to (1, 1) at the top right as seen from the front, so an image
textured on it shows the right way round.

Rows and Columns are at least 1 (default 8).

Every face winds counter-clockwise seen from outside. Normals are authored
faceVarying, so edges stay hard where the shape has them, and texture coordinates
as an indexed faceVarying `st` primvar. The subdivision scheme is `none`. Double
Sided sets the mesh's `doubleSided`, on by default so the back is visible.

Rows, Columns and the other segment counts are not animatable, so the topology is
the same on every frame. A size knob that is not animated authors default
`points` and `extent`; an animated one authors a time sample of each at every
frame of the project range, as in USDCamera.

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
