# HSVKeyer

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| HSVKeyer | transcendental | `ParityTolerance::transcendental()` | the model, the hue fade and the three trapezoids run statement for statement on both sides; OkLCh adds a cube root, an arctangent polynomial and a square root, and Vulkan's `sqrt` and division are not correctly rounded |

A clean-sheet keyer (`Tests/Native/NativeHSVKeyer_Test.cpp`). It replaces OpenFX's curve-based
HueKeyer (`net.sf.openfx.HueKeyer`) with three trapezoids over the components of a colour model.

## Colour models (`colorModel`)

Hue is in turns from red at 0, and 0 for a colour with no chroma.

- **HSV**: hexagonal hue, saturation (max - min) / max (0 when max is not positive), value max.
- **HSL**: hexagonal hue, saturation (max - min) / (L <= 0.5 ? max + min : 2 - max - min),
  lightness L = (max + min) / 2. Neither is clamped, so HDR and negative values give values
  outside 0 to 1.
- **OkLCh**: the working RGB goes to XYZ, is adapted to D65 by Bradford and goes to OkLab's LMS
  (M1), all fused into one 3x3 matrix at bake; then a signed cube root and M2 give L, a and b.
  The components are h = atan2(b, a) in turns, C = sqrt(a^2 + b^2) (about 0 to 0.4) and L.

## Ranges (`hueRange`, `satRange`, `lumaRange`)

Each is a 4-dimension double (`outerLow`, `innerLow`, `innerHigh`, `outerHigh`): the term rises
from 0 at the outer low point to 1 at the inner low point, stays 1 to the inner high point and
falls to 0 at the outer high point.

- **Hue** points are measured going up the hue circle from `outerLow`, so the range may cross red.
  Defaults 0.25, 0.30, 0.37, 0.42 (greens).
- **Saturation** and **luma** points out of order are pushed up to the one before. Saturation
  defaults 0.1, 0.3, 1, 1; luma defaults 0, 0.05, 1, 2.

In the Properties panel each range is drawn as a strip with four draggable handles over a
gradient of its component (the hue strip centred on the plateau), with the spinboxes below.
Dragging the plateau moves all four points; shift-dragging an inner handle moves its outer
partner with it.

## Hue fade

The hue term is multiplied by a smoothstep of the chroma, so a grey never matches a hue range and
a near-grey's unstable hue counts for little:

- HSV and HSL: the relative chroma (max - min) / max, from 0 at 0 to 1 at 0.05;
- OkLCh: C, from 0 at 0 to 1 at 0.02.

## Source colour and recentring

`sourceColor` is picked from the viewer with the swatch button or typed. Setting it, changing the
model or pressing **Recentre** moves each range by the distance that centres its plateau on the
source's component, keeping its widths, in one undo step at the current time. The hue range
moves the short way round and its points are wrapped into 0 to 1; it stays where it is for a
source with no chroma.

## Combination and output

The key is the product of the terms of the enabled components (`useHue`, `useSat`, `useLuma`, all
on by default), 1 when none is, or one minus that with `invert`. Alpha is the key in every
`outputMode`; RGB passes unchanged (**Alpha**), is multiplied by the key (**Premultiplied**) or is
replaced by it (**Matte**). The output is RGBA whatever the source. Mask and mix blend the result
with the source.

## GPU and cost

The kernel (`HSVKeyerOp` in `Engine/Gpu/Kernels/hue.slang`) follows the CPU statement for
statement with `precise` arithmetic (`Tests/Native/NativeHueGpu_Test.cpp`). The cost seed in
`HSVKeyer.costseed.inc` is HueKeyer's measurement, which predates the OkLCh model, the luma term
and the output modes; it is to be re-measured.
