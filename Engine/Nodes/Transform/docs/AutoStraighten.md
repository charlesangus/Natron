# AutoStraighten

A port of darktable's rotate and perspective module (ashift). It corrects converging lines and a
tilted horizon by rotating, shifting the lens and shearing the image, then crops away the empty
corners. Everything it does is driven by four knobs: `rotation` in degrees (darktable's slider
value), `lensShiftV` for converging verticals, `lensShiftH` for converging horizontals and `shear`.
They animate like any other knob.

## Fits

`fitVertical`, `fitHorizontal` and `fitBoth` fit the knobs to the guide lines on the current
frame and set them in one undo step. If a knob is animated, the fit sets a key at the current
frame. `fitParameters` picks the knobs the fit may move: all of them, rotation only, lens shift
only, or rotation and lens shift. Shear is fitted only by Fit Both with All. Fit Vertical makes the
vertical lines vertical and Fit Horizontal does the same for the horizontal lines. Fit Both does
both.

`structure` says where the lines come from:

- **Drawn Lines** uses guide lines you draw in the viewer. Pick the Draw Line tool in the viewer
  toolbar (shortcut V, in the Guide Lines group) and drag along an edge that should be vertical or
  horizontal, such as a wall, a door frame or a roof line. Each line is tagged by its direction when
  you release: mostly up and down is Vertical, mostly sideways is Horizontal. Change the tag with
  the `lineOrientation` knob while the line is selected, or with Set Vertical / Set Horizontal in the
  curve panel's right-click menu, where the Orientation column shows each line's tag. Lines are the
  node's curves: select them, move their end points, add points, delete them, lock or deactivate
  them, and animate their points like Roto shapes, all undoable. An open curve with more than two
  points gives one guide segment per pair of neighbouring points. Closed shapes are ignored.
  Fit Vertical needs at least two vertical lines, Fit Horizontal two horizontal ones and Fit Both
  two of each. Draw them far apart and as long as you can: the fit is only as good as the lines.
  darktable detects lines automatically with LSD. LSD is under the AGPL, so this node does not
  detect lines; you draw them, as in darktable's drawn-lines mode.
- **Rectangle** uses the four corner knobs `topLeft`, `topRight`, `bottomLeft` and `bottomRight`.
  They are drawn as handles in the viewer over the source image. Place them on something that
  should be a rectangle, such as a window or a facade. They default to 20% and 80% of the format,
  as in darktable, and can be animated.

A fit that cannot work says why: there are too few guide lines in a fitted direction, or the fit
did not converge. The knobs are left as they were.

## Lens model

The `lensModel` Generic is a 28 mm lens on a full-frame sensor. Specific shows `focalLength`,
`cropFactor`, `lensDependence` (darktable's orthocorr, in percent) and `aspectAdjust`. The lens
model changes how strongly a lens shift bends the image. For the same lines, a longer lens needs a
larger shift.

## Crop

`autoCrop` Off keeps the whole corrected image, with black corners. Largest Area crops to the
largest rectangle inside the corrected image. Original Format crops to the largest rectangle of the
source's aspect ratio. The output format is the cropped size, at the source's pixel aspect ratio,
with its bottom-left corner at the source format's. darktable lets you drag the crop rectangle
around in the image. That interaction belongs to the viewer work of M165. Here the crop is always centred where darktable's automatic crop puts it.

When the knobs leave the image unchanged, or when the result would be less than 4 pixels across,
the node passes its input through untouched, as darktable does.

The image is resampled by the shared resampler: `filter`, `clamp` and `black_outside`.
`black_outside` defaults to on, matching darktable's black corners. The GPU path uses the same
matrix. Fitting is an action and always runs on the CPU.

## Parity

`tools/raw/make-reference.py` renders darktable-cli's ashift on a synthetic 160x120 source
(`ashift-*.tif` in `Tests/fixtures/raw/reference/`). It covers rotation, vertical keystone, every
parameter together, and the specific lens model. darktable-cli never runs the automatic crop, so
each job states its crop box. The test passes the same box to the node through `setCropOverride`.
The crop computation is tested on its own: its corners must map back inside the source, and
Original Format must keep the aspect ratio.

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| AutoStraighten (AutoStraighten) | custom | `kReferenceMaxError` 2e-3, `kReferenceMeanError` 5e-4 | Bilinear against half-float darktable references. Output pixels that sample within 2 pixels of the source edge are skipped |
| AutoStraighten (AutoStraighten) | custom | `ParityTolerance::make(1e-5, 1e-4)` | GPU against CPU, every filter |
| AutoStraighten (AutoStraighten) | custom | 0.05 degrees, 0.01 lens shift | The Rectangle and Drawn Lines fits recover a known model |
