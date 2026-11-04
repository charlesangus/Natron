# GodRays

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| GodRays (GodRays) | resampling | `kGodRaysTolerance` = `ParityTolerance::make(0., 1e-4)` | GPU against CPU, every filter, with and without Clamp and Black outside: the GPU runs DirBlur's kernel on the CPU's matrices, factors and summation order |

Light rays: copies of the image moved, rotated, scaled or skewed by even steps from none of the
transform to all of it, tinted from From Color to To Color along the way, and averaged (or, with
Max, the brightest copy kept). Scaling up about Center streaks the image away from Center. It
replaces the OFX GodRays (`net.sf.openfx.GodRays`). For blurs along a direction, around a centre or
towards a centre, use DirBlur.

## Model

The output is built from Samples copies of the Source. Copy `i` of `n` sits at `a = i / (n - 1)`,
so the first copy (`a = 0`) is the untouched Source and the last (`a = 1`) takes the whole transform.
With one sample, the only copy is the untouched Source.

- **Transform.** Translate, Rotate, Scale and Skew X/Y act about Center, as the Transform node's
  controls do. The copy at `a` takes `a` of the transform: Translate, Rotate and the skews scale
  linearly, Scale is interpolated geometrically from 1. Each copy is the Source resampled through
  its matrix, as the Transform node resamples it.
- **Tint.** Each copy is tinted per channel from From Color (at `a = 0`) to To Color (at `a = 1`):
  `from + (to - from) * a^(1 / Gamma)`. Gamma 1 is a linear ramp. Above 1 the tint reaches To
  Color sooner, so the rays fade faster; below 1 it stays near From Color for longer.
- **Average.** The output is the mean of the tinted copies. With white tints, a flat image stays
  flat.
- **Max.** Each channel is instead the maximum, starting from 0, of the tinted copies.

No transform, white tints and Max off is the identity.

## Resampling

Filter, Clamp and Black outside are the Transform node's knobs, with its filter set and semantics.
The CPU resamples each copy with `Resampler::resampleRow`, supersampling minified copies as
Transform does. Draft renders use Impulse.

- **Filter** defaults to Cubic.
- **Clamp** works only for the filters that honour it (Keys, Simon, Rifman, Mitchell) and is
  disabled otherwise.
- **Black outside** (default on): a copy reads black beyond the Source's image, and the region of
  definition grows by one pixel. Off, a copy reads the Source's edge pixels, extended outwards.

## Region

The copies are DirBlur's samples (`DirBlurSample`), and the regions are DirBlur's. The region of
definition is the union of the Source's region of definition carried by every copy's forward
matrix, grown by a pixel with Black outside, plus the Source's own region under a mask or a mix
below 1. The Source is read over the window's preimage under every copy, grown by the filter's
footprint (`DirBlur::sourceRegion`). The CPU region of interest and the GPU input region are the
same region. Each copy is resampled on its own and summed in sample order, so a pixel does not
depend on the tile it is rendered in.

## GPU

The GPU path reuses DirBlur's kernel (`dirblur.slang`): GodRays fills its parameter block with the
copies' matrices and tints. Every filter, Clamp, Black outside, Max, the
mask, mix and channels render on the device.

## Knobs

The transform, then the tint, then Samples and Max, then the Filter row. The standard channel,
mask and mix knobs follow. There is no "(Un)premult by": GodRays is a filter.

| Parameter | Label | Default | Meaning |
|---|---|---|---|
| `translate` | Translate | (0, 0) | Translation of the last copy |
| `rotate` | Rotate | 0 | Rotation of the last copy, in degrees, about Center |
| `scale` | Scale | (1, 1) | Scale of the last copy about Center; above 1 the rays streak outwards |
| `skewX`, `skewY` | Skew X, Skew Y | 0 | Skews of the last copy |
| `center` | Center | middle of the format | Where the rays come from: the centre of Rotate, Scale and Skew |
| `fromColor` | From Color | (1, 1, 1, 1) | Tint of the untouched copy |
| `toColor` | To Color | (1, 1, 1, 1) | Tint of the fully transformed copy |
| `gamma` | Gamma | 1 | Shape of the tint ramp: `a^(1 / Gamma)` |
| `samples` | Samples | 32 | Copies, 1 to 512 |
| `max` | Max | off | Per channel maximum of the tinted copies instead of their mean |
| `filter` | Filter | Cubic | The Transform node's filters |
| `clamp` | Clamp | off | Clamp the filter's overshoot (Keys, Simon, Rifman, Mitchell) |
| `black_outside` | Black outside | on | Read black beyond the Source; off repeats its edge pixels |

`uniform` and `skewOrder` are hidden: only the Viewer's transform handle uses them. The Viewer shows
the Transform node's handle (centre, translate, rotate, scale, skew).

## Decisions

- **One transform, no amount or invert.** The last copy takes the whole transform, so the
  transform's own controls set how far the rays reach. A separate amount or an invert would only
  repeat them.
- **Samples, any count.** The number of copies is any count from 1 to 512, not a power of two.
- **Gamma shapes the tint ramp.** It bends the ramp from From Color to To Color and nothing else.
- **Mask and mix are the standard ones.** The rays go through the host's mask, invert and mix like
  every native node; there is no separate blend of the rays over the source.
- **Endpoints included.** The first copy is the untouched source and the last the full transform,
  so From Color and To Color are the tints actually applied at the two ends.

The CPU properties are in `Tests/Native/NativeGodRays_Test.cpp`: the knobs, the sample positions
and tints, the identity, translated copies against a hand-computed mean and maximum, flat-field
tints with Gamma, the scaled region, tiles against the full frame, legacy against task-graph renders
and golden fixtures. The GPU parity (every filter, each part of the transform, tints, Gamma, Max,
mask, mix, channels and a fused Grade chain) is in `Tests/Native/NativeGodRaysGpu_Test.cpp`.
