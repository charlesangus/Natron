# USDTransform

Transforms the prims of its scene input that Prim Pattern names. The pattern is a
list of absolute prim paths separated by spaces, whose names may use `*` (any run
of characters) and `?` (any one character): `/World/Geo/*` names every child of
`/World/Geo`. Only active, defined prims that can hold a transform match. The
node does not touch colour.

The transform is translate, rotate in the chosen order (degrees) and scale (scale
x Uniform Scale) about Pivot, authored on every match as one matrix op named
`xformOp:transform:` and the node's name (with a numeric suffix if the prim
already has an attribute of that name). A rename re-authors the op under the new
name.

Mode sets how the op combines with what the input authors:

- Append adds the op after the prim's existing ops, so it acts in the prim's
  local space, under its existing (possibly animated) transform. Pivot is in that
  local space. USD takes a prim's op order from its strongest opinion alone, so
  where the input merges branches that each appended a transform to the prim,
  the composed order holds only the stronger branch's ops; Append goes after
  the ops of every branch instead, keeping them all.
- Replace authors `!resetXformStack!` and the op alone: the prim's own ops and its
  parent's transform are discarded, and the transform places the prim in world
  space.

Centre Pivot sets Pivot to the centre of the matched prims' untransformed bounds
at the current frame, as one undoable edit.

Knobs that are not animated author default values; an animated knob authors a
time sample at every frame of the project range, as in USDCamera.

A pattern that matches nothing passes the input scene through unchanged and sets
a warning on the node. A pattern entry that is not an absolute path sets a node
error and the scene action fails.

## Viewport handles

While the node's settings panel is open, the 3D viewer draws handles on the first
match: an arrow per axis to move, a ring per axis to rotate and a box per axis to
scale (X red, Y green, Z blue; the handle under the pointer is yellow). The
arrows and boxes follow the axes of the space the op applies in; the rings are
about the same axes. Dragging a handle writes Translate, Rotate or Scale, and
each drag is one undoable edit.

Tolerance: none recorded.
