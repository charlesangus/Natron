# Retouch

Wavelet retouching with Roto shapes: clone, heal, blur and fill on the image or on one wavelet
scale of it. It is a port of darktable's retouch module (`src/iop/retouch.c`, params version 3,
darktable 5.6.2 `20891f6c6fa7`), on the edge-aware library (`Engine/Nodes/Filter/EdgeAware/`): its
à trous wavelet decomposition, Poisson heal and blurs.

## Scales

Scales splits the image into that many detail scales and a residual: scale 1 holds the finest
detail, each next scale details twice as coarse, and the residual what is left. A shape works on
one of them: scale 0 is the image before the decomposition, 1 to Scales a detail scale, Scales + 1
the residual. A shape on a higher scale does nothing, as in darktable; the panel's
"Scale / Algorithm" column marks it "(off)". The image's size caps the scales, as in darktable.
Merge From, when above 0, merges the scales from it up into one before the shapes run, so a shape
on any of them works on the merged layer.

The shapes are the node's Roto shapes (Beziers, ellipses, rectangles), drawn in the viewer with
the Roto tools, and brush shapes, drawn with the Brush tool (N). Each carries its own settings.
The node's Shape Settings tab is one set of them: with shapes selected it shows the selected
shapes' settings and edits them, as RotoPaint's controls do for its selected strokes; with none
selected it sets what the next shape gets.

| Knob | Label | Default | Range |
| --- | --- | --- | --- |
| `shapeAlgorithm` | Shape Algorithm | Heal | Clone, Heal, Blur, Fill |
| `shapeScale` | Shape Scale | 0 | 0 .. 16 |
| `sourceOffset` | Source Offset | 0, 0 | |
| `shapeBlurType` | Shape Blur Type | Gaussian | Gaussian, Bilateral |
| `shapeBlurRadius` | Shape Blur Radius | 10 | 0.1 .. 200 |
| `shapeFillMode` | Shape Fill Mode | Erase | Erase, Color |
| `shapeFillColor` | Shape Fill Color | 0, 0, 0 | |
| `shapeFillBrightness` | Shape Fill Brightness | 0 | -1 .. 1 |
| `shapeBrushSize` | Shape Brush Size | 100 | 1 .. 4000 |
| `shapeBrushHardness` | Shape Brush Hardness | 0.66 | 0.0005 .. 1 |
| `shapeBrushDensity` | Shape Brush Density | 1 | 0 .. 1 |
| `shapeBrushPressure` | Shape Brush Pressure | Off | Off, Hardness (Relative), Hardness (Absolute), Opacity (Relative), Opacity (Absolute), Brush Size (Relative) |

The brush knobs only change brush shapes. Shape Scale and Source Offset belong to a shape and are
disabled with none selected: a new shape goes on the Current Scale and takes its source as below.
The shape's Roto opacity is darktable's form opacity and its Roto feather the form's feather
(border). The General tab's Roto Color is hidden: a retouch mask has coverage, no colour.

With the brush tool, Shift+drag in the viewer sizes the brush as in RotoPaint: half the pen's move
right plus up is added to the next shape's brush size, and the footprint stays where the drag
started. A painted brush shape stays editable: a click inside its footprint with a select tool
selects it (Shift adds it to the selection), as does its row in the panel, and the Shape Settings
then edit it. Its points are not editable, as a RotoPaint stroke's are not.

A brush shape is darktable's brush form (`src/develop/masks/brush.c`): a freehand stroke, its mask
the stroke's centre line swept by a disc of the Brush Size (a diameter, in canonical pixels),
Brush Density inside the Brush Hardness fraction of the radius and falling linearly to 0 at the
edge, overlaps taking the larger value. The pen pressure is recorded with every point (1 for a
mouse) and Brush Pressure picks what it changes, with darktable's floors: hardness down to
0.0005, opacity to 0.05, size to 0.005% of the smaller image side. The pressure applies at render
time, so changing the mode changes a drawn brush. darktable's brush smoothing preference is not
ported: the stroke is Roto's Catmull-Rom curve through the pen events. Roto opacity scales the
brush mask; Roto feather does not apply to it.

## Algorithms

- **Clone** copies the source, the shape moved by Source Offset, onto the shape.
- **Heal** copies the source and solves a Poisson equation so that its texture blends into the
  colours around the shape, up to Max Heal Iterations.
- **Blur** blurs the layer under the shape: a Gaussian of the radius, or darktable's bilateral
  grid on the lightness.
- **Fill** fills the shape: Erase with grey at the Fill Brightness (on a detail scale, 0 erases the
  detail), Color with the Fill Color plus the brightness.

Source Offset goes from the shape to its source, in canonical pixels, x right and y up. The viewer
draws each Clone and Heal shape's source as a dashed outline with a line to the shape; dragging
the outline moves the source. New Clone and Heal shapes drawn in the viewer take their source as
RotoPaint's clone brush does: Ctrl+drag with a drawing tool (Bezier, ellipse, rectangle or brush)
leaves the source where it is drawn and moves the pen away from it, and every later shape keeps that
offset from the shape's centre or first point. Reset Clone Offset in the viewer toolbar sets the offset
to 0, 0, which is also where it starts, so a shape drawn without Ctrl+drag has its source on itself. A shape made by script takes 0, 0.
A source outside the image is cropped to it. While a drawing tool is active the viewer draws where
the next Clone or Heal shape's source goes: the brush footprint for the brush, a small circle
otherwise.
The brush footprint and the stroke being painted stay drawn while painting. A brush shape has
RotoPaint's stroke lifetime: it shows on the frame it was painted on, and its Life Time can be All,
Single, From Start, To End or Custom, as for a RotoPaint stroke.
Copy to Scale N and Move to Scale N in a shape's right-click menu put a copy of the selected
shapes, or the shapes themselves, on the Current Scale, as one undoable step.

## Controls

| Knob | Label | Default | Range |
| --- | --- | --- | --- |
| `scales` | Scales | 0 | 0 .. 15 |
| `currentScale` | Current Scale | 0 | 0 .. 16 |
| `mergeFrom` | Merge From | 0 | 0 .. 15 |
| `output` | Output | Result | Result, Current Scale, Residual |
| `previewLevels` | Preview Levels | -3, 0, 3 | -3 .. 3 |
| `maxHealIterations` | Max Heal Iterations | 2000 | 1 .. 100000 |

Output shows the result, or the Current Scale's detail through the Preview Levels (black, grey and
white points, darktable's wavelet preview), or the residual. darktable's "display wavelet scale"
toggle and auto levels are GUI state; here they are the Output choice and the Preview Levels knob.
darktable's unused (NONE) algorithm has no option. The hidden `algorithm`, `blurType`,
`blurRadius`, `fillMode`, `fillColor`, `fillBrightness`, `brushSize`, `brushHardness`,
`brushDensity` and `brushPressure` knobs keep the next shape's settings, with the Shape Settings
defaults, while shapes are selected; scripts set the next shape's settings through them. Max Heal Iterations is darktable's
`max_heal_iter`, which it does not show.

The whole source is processed in every render: the node does not tile, and its region of interest
is the source's region of definition. Radii and offsets follow the render scale, so a proxy
retouches the same features. The masks assume square pixels. Mask, mix and the unprocessed
channels are the host's stages.

The GPU path (`Engine/Gpu/Kernels/retouch.slang`, with the edge-aware library's DWT, Poisson heal
and recursive Gaussian kernels) runs the Result output for Clone, Heal, Gaussian Blur and Fill. The
CPU renders the Current Scale and Residual outputs, Merge From, the bilateral blur, and a heal of
over 500 iterations on a device without push descriptors.

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| Retouch (Retouch) | darktable reference | `kDarktableClass` | the node on make-reference.py's plate against darktable 5.6.2, mean error 0.2 % and largest error 5 % of the reference's mean level, each sample's beyond the half float reference's rounding. Roto's feather falloff is not darktable's quadratic one, and the ellipses are Beziers within 0.25 px of darktable's, so the edges of the shapes carry most of the error. The comparison leaves out each shape's feather band, from its outline out to the feather distance, and for a path the pixels within 2 px of its outline on either side: darktable's path rasteriser (`_path_falloff_roi` in `src/develop/masks/path.c`) draws the feather as integer-snapped lines from each border point, one sample longer than the feather, writing the neighbouring pixels too, which leaves a band about a pixel wide at full strength, biased up and left, with a ring at a third. The test prints how many pixels each job leaves out |
| Retouch (Retouch) | identity | `1e-6` | no shapes, or shapes only on skipped scales; the decomposition and recomposition alone |
| Retouch (Retouch) | GPU against CPU | `kRetouchTolerance`, `kRetouchHealTolerance` | `ParityTolerance::make(0., 1e-4)`, the edge-aware library's bound; 1e-3 for the heal, as `EdgeAwareGpu_Test` |

The knob audit, identity, save and load (brush shapes included), the brush mask and pressure
modes, the brush tool, source placement, the column and the copy and move actions and the darktable
references are in `Tests/Native/NativeRetouch_Test.cpp`; the GPU parity is in
`Tests/Native/NativeRetouchGpu_Test.cpp`, the viewer interaction in `Tests/gui/m211_retouch.py`.
The references are described in `Tests/fixtures/raw/README.md`.
