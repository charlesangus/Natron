# SplineWarp

Warps the Source with curves. A link pairs a source curve with a destination curve: what lies
along the source curve moves onto the destination curve, and the rest of the image follows
smoothly. Curves are drawn with the viewer's curve tools (Bezier, open Bezier, Ellipse,
Rectangle, and a point tool that makes a curve of one point) and listed in the settings panel.

The settings panel has three pages:

- **Controls:** `amount`, `invert`, `layerAmount`, `linkAmount`, `cookieCutInput`, `outputMask`,
  `cropToFormat`, `boundaryBBox`, and the mask and mix every staged node has.
- **Transform:** the selected curve's or layer's `translate`, `rotate`, `scale` with `uniform`,
  `skewx`, `skewy`, `skewOrder`, `center` and `extraMatrix`.
- **Render:** `solver`, `smoothness`, `curveResolution`, `boundaryCurveResolution`,
  `fieldResolution`, `previewResolution`, `filter`, `clamp`, `black_outside`, `bbox`, and the
  motion blur settings `motionBlur`, `shutter`, `shutterOffset` and `shutterCustomOffset`.

Each curve has a role, shown in the panel's Role column:

- **Source** and **Destination**: the two curves of a link. The Linked To column names the other
  curve. Overlays draw sources red and destinations blue.
- **Hard Boundary**: a closed curve that limits the warp. Overlays draw it yellow.
- **Soft Boundary**: a curve, open or closed, that holds the image still along it without limiting
  the warp elsewhere. Overlays draw it green.
- No role: the curve is ignored by the warp. Overlays draw it grey.

Apart from its role, a closed curve can be a **cookie cutter**, shown Yes in the Cookie Cutter
column. Opening the curve clears the flag.

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
- **Toggle Cookie Cutter** makes every selected closed curve a cookie cutter, or none of them
  when they all are.
- **Link Transform to First** makes every other selected curve or layer take the first one's
  transform in place of its own; the Transform Link column names it. **Unlink Transform** ends
  the links of the selected items, which keep the values they followed.

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

**Solver:** Thin Plate Spline, the smoothest map that moves every curve sample where its link asks,
or Mesh, a triangle mesh through the curve samples that keeps the warp local to the curves.
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
is loaded.

**Draft:** while a point or the transform handle is dragged, the viewer renders a draft, and the
full-quality frame follows when the drag ends. `previewResolution` (Render page, 1 to 100 %,
default 25) sets its quality: the field step is `fieldResolution` × 100 / `previewResolution`, the
curves and boundaries are sampled the square root of that as coarsely, and the filter is Impulse.
At the default the field is four times as coarse and the curves are sampled half as finely.

**Cookie cutters:** with `cookieCutInput` on (default off), the Source is multiplied by the mask of
the cookie cutter curves, at their source positions, before it is warped, so only what they hold
is warped. `outputMask` (default `SplineWarpMask.A`) is the channel the same mask is written to,
warped as the Source is; it can be None or a channel of a layer other than the colour layer.
Both are disabled while no curve is a cookie cutter. A linked cookie cutter cuts with its source
curve's shape.

**Crop To Format:** `cropToFormat` (on by default) samples the Source only inside its format,
counting what is outside as black, and keeps the output inside the format. Off, the warp can pull
in the Source's pixels outside the format, and the whole region of definition is warped.

**Transform page:** every curve and layer has its own transform, applied before the warp: a
transformed destination curve moves the warp with it. The page shows and edits the selected item's
transform. A layer's transform applies to everything inside it, on top of their own. A transform
link makes an item take another item's transform in place of its own (its layers still apply);
links that would form a cycle, or join an item to a layer it is in, are refused. A new curve's
`center` is the centre of the Source's format. When one curve, or one layer with the curves inside
it, is selected with a select tool, the viewer shows the transform handle; dragging it edits the
item's transform, setting keys when auto-keying is on. Editing the curve's points wins over the
handle where they overlap.

## Differences from Nuke

- `amount`, `layerAmount` and `linkAmount` are Nuke's root warp, layer warp and pair warp.
  `cookieCutInput` is Nuke's premultiply input, renamed so it is not taken for a premultiply knob.
- `curveResolution` and `boundaryCurveResolution` are distances in pixels between samples (default
  4), not Nuke's detail levels.
- `solver` defaults to Thin Plate Spline, close to Nuke's classic warping; Mesh, the triangle mesh
  solver, is closer to Nuke's default local function.
- `previewResolution` defaults to 25 rather than Nuke's 100, so a drag stays interactive.
- The filter menu has no Lanczos6 entry.
- There is no morphing: no second input, no output modes beyond the Source warped or not, no morph
  mix, no A/B curve types and no Duplicate in B.

## Python

`node.getSplineWarp()` returns the links, boundaries, cookie cutters, layer amounts and transform
links of the node's curves. The curves themselves are made and edited through
`node.getRotoContext()`, and each curve's transform through its parameters (`curve.getParam("translate")`).

- `link(source, destination, duplicate=False)`, `unlink(curve)`, `getLink(curve)`, `getLinks()`
- `setBoundary(curve, on)`, `isBoundary(curve)`: hard boundaries, closed curves only.
- `setSoftBoundary(curve, on)`, `isSoftBoundary(curve)`: soft boundaries, open or closed. Making a
  curve one kind of boundary clears the other.
- `setCookieCutter(curve, on)`, `isCookieCutter(curve)`: closed curves only.
- `getLayerAmount(layer, time)`, `setLayerAmount(layer, value, time)`,
  `setLayerAmountAtTime(layer, time, value)`
- `linkTransform(item, target)`: `item`, a curve or layer, takes `target`'s transform in place of
  its own. Raises ValueError for a link that would form a cycle or join an item to a layer it is in.
- `unlinkTransform(item)`: ends the link; the item keeps the values it followed.
- `getTransformLink(item)`: the curve or layer whose transform `item` takes, or None.

A link (`SplineWarpCurveLink`) has `getSource()`, `getDestination()`, `getAmount(time)`,
`setAmount(value, time)`, `setAmountAtTime(time, value)`, `setAlignment(startOffset, reversed)`,
the correspondence calls `addCorrespondence`, `setCorrespondence`, `setCorrespondenceAtTime`,
`removeCorrespondence`, `getCorrespondences(time)` and `getCorrespondenceCount()`, and `remove()`.
