# SplineWarp

Warps the Source with curves. A link pairs a source curve with a destination curve: what lies
along the source curve moves onto the destination curve, and the rest of the image follows
smoothly. Curves are drawn with the viewer's curve tools (Bezier, open Bezier, Ellipse,
Rectangle, and a point tool that makes a curve of one point) and listed in the settings panel.

Each curve has a role, shown in the panel's Role column:

- **Source** and **Destination**: the two curves of a link. The Linked To column names the other
  curve. Overlays draw sources red and destinations blue.
- **Hard Boundary**: a closed curve that limits the warp. Overlays draw it yellow.
- **Soft Boundary**: a curve, open or closed, that holds the image still along it without limiting
  the warp elsewhere. Overlays draw it green.
- No role: the curve is ignored by the warp. Overlays draw it grey.

A curve or layer whose Activated box is off at the current frame is ignored by the warp, whatever
its role.

Links and boundaries are made from a curve's context menu in the panel:

- **Duplicate and Link** copies the one selected curve onto itself, links the original as the
  source and the copy as the destination, and selects the copy. Nothing moves until the copy is
  edited. It is refused on a curve that is already linked or a boundary.
- **Link** links two selected unlinked curves, the first selected as the source. The points of the
  two curves are paired automatically: the destination's starting point and direction are chosen
  to best match the source's shape, so curves drawn separately, from different starting points or
  in opposite directions, can be linked. Refine the pairing with correspondences.
- **Unlink** removes the links of the selected curves. The curves stay, with no role.
- **Swap Source/Destination** swaps the two curves of each selected link, keeping how their
  points pair.
- **Toggle Hard Boundary** and **Toggle Soft Boundary** make every selected curve that kind of
  boundary, or none of them when they all are. Making a curve one kind of boundary clears the
  other. Hard boundaries must be closed and unlinked; soft boundaries only unlinked.

Actions that do not apply to the selection are greyed out. Each action is one undo step.

**Correspondences** pin a point of a source curve to a point of its destination curve. With the
correspondence tool, click a linked curve to add one there; its other end lands on the matching
point of the other curve. Drag either end along its curve; it cannot pass the correspondences on
either side, so the pairing never folds. Delete or Backspace removes the selected one. With
auto-keying on, adding or moving a correspondence sets a key at the current frame.

**Boundaries and feather:** with at least one hard boundary, nothing outside every boundary's feather
moves. Inside a boundary's curve the warp is whole; between the curve and its feather it eases to
nothing along the curve's feather fall-off. A boundary with no feather cuts the warp off at the
curve. A soft boundary only pins the image along its curve: the warp fades toward it but still
moves everything on the far side. Boundary BBox (on by default) adds the Source format's
rectangle as a soft boundary, so the image edges stay put. Boundary Curve Resolution is the
distance, in full-resolution pixels, between samples along hard and soft boundaries.

**Amounts:** each link has its own amount (Python: `SplineWarpCurveLink.setAmount`), and the
Amount knob scales every link on top of that. An amount moves the destination toward the source
before the solve, so at 0 the source curve is held in place and nothing moves. Layer Amount scales
every link whose destination curve is inside the selected layers, nested layers multiplying; it
is enabled when only layers are selected. Link Amount edits the amount of the selected linked
curves' links, enabled when every selected curve is linked. Both follow the selection and key
the layer or link they edit; they are not saved themselves. Invert swaps every
link's source and destination, so the warp goes the other way.

**Solver:** Thin Plate Spline, the smoothest map that moves every curve sample where its link asks.
Smoothness lets the solve stray from the curves, which tames folds where curves pull against each
other. Curve Resolution is the distance, in full-resolution pixels, between samples along the
curves. Field Resolution is the spacing of the grid the warp is evaluated on; pixels in between
are interpolated. These, the filter and the motion blur settings are on the Render page. Filter, Clamp and Black Outside set how the Source is resampled. BBox keeps the
Source's region of definition or grows it by where the warp moves the Source's edges and by the
destination curves. A transform above the Source is folded into the same resample. The host mask
and mix apply as for every staged node.

**Viewing:** the viewer's top bar holds two settings that are never saved. Show picks the source
curves, the destination curves, or both, joined by dashed lines between the paired points; hidden
curves cannot be picked. Boundaries and curves with no role are always shown. Output Warped, when
off, makes the node output its Source unwarped, so you can compare; it is on again when the project
is loaded. While a point is dragged the viewer renders a draft: the field four times as coarse and
the curves and boundaries sampled half as finely. The full-quality frame follows when the drag ends.

Python: `node.getSplineWarp()` returns the links, boundaries and layer amounts of the node's curves.
