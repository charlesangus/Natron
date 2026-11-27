# GeoToUSD

Tolerance: none recorded.

GeoToUSD puts the geometry of its Geometry input on the scene, over the optional Scene input. Each primitive's `path` attribute names the prim it becomes; primitives without one go under **Root Path**.

## Choosing what is converted

**Group** restricts the conversion to part of the geometry. It takes the same patterns as the other geometry nodes: element numbers and ranges (`0-10`, `0-20:2`), group names and globs, `*`, attribute comparisons (`@P.x<0`), and the `!` and `^` prefixes. A blank group converts everything.

**Group Type** says whether the pattern selects points or primitives. **Guess from Group** takes the class of the group the pattern names, and primitives for plain numbers. Edge patterns are an error.

- **Primitives**: the selected primitives, and the points they use. Loose points, used by no primitive, are left out.
- **Points**: the primitives whose points are all selected, with their points, and the selected loose points. A primitive with only some of its points selected is left out, and so are those points.

Packed primitives, packed USD primitives and loose points are selected like any other element. Nothing outside the selection is written.

A kept primitive becomes the prim it would without a group, so changing the group does not move prims that stay. Three names follow from what is converted rather than from the primitive itself:

- Primitives of several types sharing one path each become a child prim of that path, `mesh_0`, `curves_0` and so on; when the group keeps only one type, it takes the path itself.
- With **Native Instances**, the instances of a path are numbered `instance_0`, `instance_1` and so on in the order of the kept packed primitives.
- The prototypes of a point instancer are numbered in the order their embedded geometry first appears among the kept packed primitives.

Select one of these by a path pattern over the parent prim when the group may change.

## Settings

- **Root Path**: the prim unpathed primitives go under, and relative paths are resolved against.
- **Packed Primitives**: whether packed geometry becomes a point instancer, native instances or unpacked meshes. Packed USD primitives always override the prim they were read from.
- **Default Point Width**: the width of points with no `pscale` attribute.
- **Copy Into Layer**: off, the geometry is served to the scene as it is; on, it is converted and copied into the node's layer.
- **Author Time Samples**: with Copy Into Layer, authors the geometry at every frame of the project range.
