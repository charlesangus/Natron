# DirBlur

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| DirBlur (DirBlur) | resampling | `kDirBlurTolerance` = `ParityTolerance::make(0., 1e-4)` | GPU against CPU, every filter, with and without Clamp and Black outside: the GPU uses the CPU's matrices, factors and summation order, with two-float sample positions |

Smears the image along a direction, around a centre or towards a centre. It replaces the OFX
DirBlur (`net.sf.openfx.DirBlur`). Rays from a transformed copy of the image are the GodRays node
(see `GodRays.md`).

## Model

The output is built from Samples copies of the Source. A copy sits at a position `t` along the blur:
`(i + 0.5) / n` for `i = 0 .. n - 1`, or `-1 + (2i + 1) / n` with Centered, so it runs both ways.
Each copy is the Source resampled through one matrix, as the Transform node resamples it: the
mode's motion at `t`.

- **Directional.** The copy moves `t * Length` pixels along Direction. That is degrees
  counter-clockwise from the x axis, or, with Use Target, the direction from Center to Target.
- **Radial.** The copy turns `t * Length` degrees around Center, counter-clockwise for a positive
  Length.
- **Zoom.** The copy is scaled about Center by `k = 1 + (t * Length + Pixel Offset) / R`, where
  `R` is half the format's diagonal. So the format's corners move by `t * Length + Pixel Offset`
  pixels: inwards for a positive Length, outwards for a negative one. `k` is at least 0.01.

Each copy's weight is `(1 - |t|)^Falloff`, or 1 when Falloff is 0. The copy is tinted per channel
by the Ramp from From Color (at `|t| = 0`) to To Color (at `|t| = 1`):

- **Linear:** `from + (to - from) * |t|`.
- **Exponential:** `from * (to / from)^|t|`, with each colour at least 0.001: an exponential
  decay from From Color to To Color.

The output is the sum of the tinted copies divided by the sum of their weights. With white tints,
a flat image stays flat and the blur keeps the image's energy. With **Max**, each channel is instead
the maximum, starting from 0, of the tinted copies, each weighed by its weight without
normalisation: the brightest copy at each pixel.

Length 0 with no Pixel Offset is the identity, whatever the tints or Max.

## Resampling

Filter, Clamp, Black outside, Lobes, Bicubic B and Bicubic C are the Transform node's knobs, with
its filter set and semantics; see [Transform](../../Transform/docs/Transform.md). The CPU uses
`Resampler::resampleRow` per copy, and a minified copy widens the filter's kernel as Transform does.

- **Filter** defaults to Cubic, as in Transform. Draft renders use Impulse.
- **Clamp** is shown only for the filters that can overshoot (Keys, Simon, Rifman, Mitchell, the
  Lanczos filters, Sinc and Bicubic). It keeps the filter's overshoot within the range of the
  pixels the filter reads.
- **Lobes** is shown only for Sinc, **Bicubic B** and **Bicubic C** only for Bicubic.
- **Black outside** (default on): a copy reads black beyond the Source's image, and the region of
  definition grows by one pixel. Off, a copy reads the Source's edge pixels, extended outwards, and
  the region of definition is the union of the copies' regions, not grown. To keep a tile equal to
  the full frame, the Source's region of interest is then clamped into the Source's region of
  definition instead of being cut by it. Wherever a read goes past the region of definition, the
  region of interest reaches its edge, so the edge pixels are the same in every tile.

## Region

The region of definition is the union of the Source's region of definition carried by every copy's
forward matrix, grown by a pixel with Black outside, plus the Source's own region under a mask or a
mix below 1. Each input is read over the window's preimage under every copy, grown by the
filter's radius, widened by a minifying copy's scale (`DirBlur::sourceRegion`). The CPU
region of interest and the GPU input region are the same region in canonical coordinates. Each copy
is resampled on its own and summed in sample order, so a pixel does not depend on the tile it is
rendered in.

## Knobs

The page has the blur controls, then the Filter row, then one group: Tint (open).
The standard channel, mask and mix knobs follow. Only the controls the selected Mode uses are
shown; the others are hidden, and come back with their values when the Mode changes back.

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
| `center` | Center | middle of the format | Centre of Radial and Zoom; Use Target's start, with a Viewer handle |
| `resetCenter` | Reset Center | | Center to the middle of the Source's region of definition |
| `pixelOffset` | Pixel Offset | 0 | Zoom only: moves every copy this many pixels further along the blur at the corners |
| `filter` | Filter | Cubic | The Transform node's filters |
| `clamp` | Clamp | off | Clamp the filter's overshoot |
| `black_outside` | Black outside | on | Read black beyond the Source; off repeats its edge pixels |
| `lobes` | Lobes | 4 | Sinc's lobe count and radius, 2 to 8 |
| `bicubicB` | Bicubic B | 1/3 | Bicubic's B; higher blurs more |
| `bicubicC` | Bicubic C | 1/3 | Bicubic's C; higher sharpens more |
| `fromColor` | From Color | (1, 1, 1, 1) | Tint at the start of the blur |
| `toColor` | To Color | (1, 1, 1, 1) | Tint at the end of the blur |
| `ramp` | Ramp | Linear | Linear or Exponential tint between the colours |

The Viewer shows a Center handle and a Target handle.

### Visibility by mode

| Knob | Directional | Directional with Use Target | Radial | Zoom |
|---|---|---|---|---|
| `direction` | shown | hidden | hidden | hidden |
| `useTarget` | shown | shown | hidden | hidden |
| `target` | hidden | shown | hidden | hidden |
| `center`, `resetCenter` | hidden | shown | shown | shown |
| `pixelOffset` | hidden | hidden | hidden | shown |

Every other knob is shown in every mode. Use Target only matters in Directional, so with it on in
Radial or Zoom the table's Radial and Zoom columns still apply. The visibility follows Mode and Use
Target as they change and when a project loads.

## Decisions

- **No reduced-resolution quality control.** Rendering the blur at a reduced resolution and scaling
  it back would make a pixel depend on the reduced grid, which breaks tile independence and GPU parity.
  Samples and the viewer's proxy cover its use.
- **No pixel aspect knob.** The pixel aspect ratio comes from the input's metadata. Matrices are built
  in canonical coordinates and mapped to pixels with it, so anamorphic footage blurs undistorted
  without a knob.
- **No holdout.** DirBlur has no holdout input; the blur is the whole source.
- **No "(Un)premult by".** DirBlur is a filter: the source is averaged as it comes.
- **No transform.** Transformed copies (rays from a scaled, rotated or moved image) are the GodRays
  node's job, so DirBlur keeps only its three modes. Controls a mode does not use are hidden.

The CPU properties are in `Tests/Native/NativeDirBlur_Test.cpp`. They cover the knobs and groups,
the visibility of the knobs in each mode, the sample positions and weights, the ramps, each mode's
smear, flat fields, target, pixel offset and Max, edge repeat and clamp, tiles against the full frame (every
mode, filters, Clamp and edge repeat) and legacy against task-graph renders. The GPU parity
(every filter in every mode, with and without Black outside, mask, mix, channels and a
fused Grade chain) is in `Tests/Native/NativeDirBlurGpu_Test.cpp`.
