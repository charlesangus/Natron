# DirBlur

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| DirBlur (DirBlur) | resampling | `kDirBlurTolerance` = `ParityTolerance::make(0., 1e-4)` | GPU against CPU, every filter, with and without Clamp and Black outside: the GPU uses the CPU's matrices, factors and summation order, with two-float sample positions |

Smears the image along a direction, around a centre or towards a centre. It replaces Nuke's DirBlur
and the God Rays node (`net.sf.openfx.GodRays`), whose transform is folded in.

## Model

The output is built from Samples copies of the Source. A copy sits at a position `t` along the blur:
`(i + 0.5) / n` for `i = 0 .. n - 1`, or `-1 + (2i + 1) / n` with Centered, so it runs both ways.
Each copy is the Source resampled through one matrix, as the Transform node resamples it. That
matrix is the mode's motion at `t`, composed with the transform at `|t|`.

- **Directional.** The copy moves `t * Length` pixels along Direction. That is degrees
  counter-clockwise from the x axis, or, with Use Target, the direction from Center to Target.
- **Radial.** The copy turns `t * Length` degrees around Center, counter-clockwise for a positive
  Length.
- **Zoom.** The copy is scaled about Center by `k = 1 + (t * Length + Pixel Offset) / R`, where
  `R` is half the format's diagonal. So the format's corners move by `t * Length + Pixel Offset`
  pixels: inwards for a positive Length, outwards for a negative one. `k` is at least 0.01.
- **Transform.** Translate, Rotate, Scale (with Uniform), Skew X/Y and Skew Order act about Center,
  as the Transform node's controls do. Transform Amount and Invert also follow the Transform node.
  The copy at `t` takes the transform at `t * Transform Amount`. Scale is interpolated
  geometrically. A copy before the start of a centred blur takes the inverse transform at `|t|`.
  The transform works in every mode. With Length 0 and only the transform you get God Rays.

Each copy's weight is `(1 - |t|)^Falloff`, or 1 when Falloff is 0. The copy is tinted per channel
by the Ramp from From Color (at `|t| = 0`) to To Color (at `|t| = 1`):

- **Linear:** `from + (to - from) * |t|`.
- **Exponential:** `from * (to / from)^|t|`, with each colour at least 0.001. This is God Rays'
  exponential decay: From white and To black give crepuscular rays.

The output is the sum of the tinted copies divided by the sum of their weights. With white tints,
a flat image stays flat and the blur keeps the image's energy. With **Max**, each channel is instead
the maximum, starting from 0, of the tinted copies, each weighed by its weight without
normalisation. That is God Rays' "brightest value at each pixel".

Length 0 with no transform and no Pixel Offset is the identity, whatever the tints, Max or holdout.

## Resampling

Filter, Clamp and Black outside are the Transform node's knobs, with its filter set and semantics.
The CPU uses `Resampler::resampleRow` per copy, and minified copies are supersampled as Transform
does.

- **Filter** defaults to Cubic, as in God Rays and Transform. Draft renders use Impulse.
- **Clamp** works only for the filters that honour it (Keys, Simon, Rifman, Mitchell) and is
  disabled otherwise. It keeps the filter's overshoot within the neighbouring pixels' range.
- **Black outside** (default on): a copy reads black beyond the Source's image, and the region of
  definition grows by one pixel. Off, a copy reads the Source's edge pixels, extended outwards, and
  the region of definition is the union of the copies' regions, not grown. To keep a tile equal to
  the full frame, the Source's region of interest is then clamped into the Source's region of
  definition instead of being cut by it. Wherever a read goes past the region of definition, the
  region of interest reaches its edge, so the edge pixels are the same in every tile.

## Holdout

The optional Holdout input, with Holdout Channel (any layer, default `rgba.A`; None turns it off).
The channel is resampled through the same matrix as each copy, with the same filter and Clamp,
and always reads black outside its own image. Each copy then counts `1 - clamp(h, 0, 1)` as much.
Without Max, the held-out copies leave the average and the rest are renormalised to their weights,
so a pixel whose copies are all held out is 0. With Max, a held-out copy is skipped. The holdout
works in every mode, whereas Nuke's DirBlur applies it only in Zoom. The Holdout is read over the
Source's region, unclipped by the Source's region of definition.

## Region

The region of definition is the union of the Source's region of definition carried by every copy's
forward matrix, grown by a pixel with Black outside, plus the Source's own region under a mask or a
mix below 1. Each input is read over the window's preimage under every copy, grown by the widest
filter's footprint and by the supersampling of a minifying copy (`DirBlur::sourceRegion`). The CPU
region of interest and the GPU input region are the same region in canonical coordinates. Each copy
is resampled on its own and summed in sample order, so a pixel does not depend on the tile it is
rendered in.

## Knobs

The page has the blur controls, then the Filter row, then three groups: Tint (open), Transform
(closed by default) and Holdout (open). The standard channel, mask and mix knobs follow.

| Parameter | Label | Default | Meaning |
|---|---|---|---|
| `mode` | Mode | Zoom | Directional, Radial or Zoom |
| `length` | Length | 20 | Pixels (Directional, Zoom) or degrees (Radial); with Centered, each way |
| `samples` | Samples | 32 | Copies averaged, 1 to 512 |
| `centered` | Centered | off | The blur runs both ways from each pixel |
| `falloff` | Falloff | 0 | A copy at `t` weighs `(1 - |t|)^Falloff` |
| `max` | Max | off | Per channel maximum of the weighted, tinted copies instead of their average |
| `direction` | Direction | 45 | Directional's direction, in degrees, without Use Target |
| `useTarget` | Use Target | off | Directional runs from Center towards Target |
| `target` | Target | (0.75, 0.5) of the format | Directional's target point, with a Viewer handle |
| `center` | Center | middle of the format | Centre of Radial, Zoom and the transform; Use Target's start |
| `resetCenter` | Reset Center | | Centre to the middle of the Source's region, keeping the full transform in place |
| `pixelOffset` | Pixel Offset | 0 | Zoom only: moves every copy this many pixels further along the blur at the corners |
| `filter` | Filter | Cubic | The Transform node's filters |
| `clamp` | Clamp | off | Clamp the filter's overshoot (Keys, Simon, Rifman, Mitchell) |
| `black_outside` | Black outside | on | Read black beyond the Source; off repeats its edge pixels |
| `fromColor` | From Color | (1, 1, 1, 1) | Tint at the start of the blur |
| `toColor` | To Color | (1, 1, 1, 1) | Tint at the end of the blur |
| `ramp` | Ramp | Linear | Linear or Exponential tint between the colours |
| `translate` | Translate | (0, 0) | Translation of the copy at the end of the blur |
| `rotate` | Rotate | 0 | Rotation, in degrees, about Center |
| `scale` | Scale | (1, 1) | Scale about Center, geometric along the blur |
| `uniform` | Uniform | off | Use the X scale for both axes |
| `skewX`, `skewY` | Skew X, Skew Y | 0 | Skews of the copy at the end of the blur |
| `skewOrder` | Skew Order | XY | Order the skews apply in |
| `transformAmount` | Transform Amount | 1 | Share of the transform the last copy takes |
| `invert` | Invert | off | Invert the transform |
| `interactive` | Interactive | off | Update the values while dragging the Viewer handles, else on release |
| `holdoutChannel` | Holdout Channel | `rgba.A` | The Holdout input's channel |

The Viewer shows the Transform node's handle (centre, translate, rotate, scale, skew) and a Target
handle.

## Decisions

- **Quality dropped.** Nuke's Quality renders the blur at a reduced resolution and scales it back.
  That makes a pixel depend on the reduced grid, which breaks tile independence and GPU parity.
  Samples and the viewer's proxy cover its use.
- **PixelAspect dropped.** The pixel aspect ratio comes from the input's metadata. Matrices are built
  in canonical coordinates and mapped to pixels with it, so anamorphic footage blurs undistorted
  without a knob.
- **Holdout in every mode.** Nuke applies the holdout only in Zoom. Here the holdout works the same
  way in every mode, and the mode does not change what it means.
- **No "(Un)premult by".** DirBlur is a filter: the source is averaged as it comes.
- **God Rays' Steps** (`2^steps` copies) is Samples, which takes any count.

The CPU properties are in `Tests/Native/NativeDirBlur_Test.cpp`. They cover the knobs and groups,
the sample positions and weights, the ramps, each mode's smear, flat fields, the transform,
target, pixel offset, holdout and Max, edge repeat and clamp, tiles against the full frame (every
mode, filters, Clamp and edge repeat) and legacy against task-graph renders. The GPU parity
(every filter in every mode, with and without Black outside, mask, mix, channels, holdout and a
fused Grade chain) is in `Tests/Native/NativeDirBlurGpu_Test.cpp`.
