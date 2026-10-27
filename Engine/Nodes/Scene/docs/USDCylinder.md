# USDCylinder

Defines a `Mesh` at Prim Path (default `/` and the node's name, such as
`/USDCylinder1`, following renames until the path is edited). A cylinder of Radius (default 1) and Height (default 2) centred
on the origin along the up axis, divided into Rows bands along its height (at
least 1, default 30) and Columns faces around (at least 3, default 30). Top Cap
and Bottom Cap (both on by default) close the ends with triangle-fan discs; with
both off it is an open tube. On the side `st` u runs once around with the seam at
the back and v from 0 at the bottom to 1 at the top; each cap maps the disc into
the [0, 1] square.

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
