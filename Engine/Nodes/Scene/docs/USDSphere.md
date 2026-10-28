# USDSphere

Defines a `Mesh` at Prim Path (default `/` and the node's name, such as
`/USDSphere1`, following renames until the path is edited). A UV sphere of Radius (default 1) centred on the origin, its poles
on the up axis, divided into Rows bands from pole to pole (at least 2, default
30) and Columns faces around (at least 3, default 30). The bands next to the
poles are triangle fans. `st` u runs once around the sphere with the seam at the
back, the seam's points carrying u = 0 and u = 1; v runs from 0 at the bottom pole
to 1 at the top.

Every face winds counter-clockwise seen from outside. Normals are authored
faceVarying, so edges stay hard where the shape has them, and texture coordinates
as an indexed faceVarying `st` primvar. The subdivision scheme is `none`. Double
Sided sets the mesh's `doubleSided`, off by default.

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
