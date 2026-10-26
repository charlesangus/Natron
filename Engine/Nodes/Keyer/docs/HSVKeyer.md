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

## Ranges (`hueRange`, `saturationRange`, `lumaRange`)

Each is a 5-dimension double, in order **Outer Low**, **Inner Low**, **Centre**, **Inner High**
and **Outer High**. The inner offsets are measured from the centre and the outer ones from the
inner edges: the term rises from 0 at centre - inner low - outer low to 1 at centre - inner low,
stays 1 to centre + inner high and falls to 0 at centre + inner high + outer high. An outer offset
of 0 is a hard edge. Offsets below 0 count as 0 (`KnobDouble::sanitizeRangeOffsets()`,
`KnobDouble::rangeOffsetsOf()`). The bake resolves the offsets into the absolute points the CPU
and GPU trapezoids read, so `hue.slang` is unaffected by the knob layout.

- **Hue** wraps around its centre, so the range may cross red; each offset counts up to a whole
  turn, so the trapezoid spans at most four. Defaults 0.05, 0.035, 0.4, 0.035, 0.05.
- **Saturation** defaults 0.04, 0.14, 0.2, 0.2, 0 (a hard edge at 0.4). **Luma** defaults 0.05,
  0.45, 0.5, 0.5, 1 (outer high at 2, for HDR).

## Smooth falloffs (`hueSmooth`, `saturationSmooth`, `lumaSmooth`)

Each range has a bool, off by default, saved and not animated, shown as its row's **S** button
rather than as a checkbox. On, the range's falloffs follow the smoothstep 3t^2 - 2t^3 of the
linear ramp t (`HueCurves::smoothStep()`, `hueSmoothStep()` in `hue.slang`, the same operations
in the same order), so a quarter of the way along a falloff keys 0.15625 instead of 0.25. It is
applied to the trapezoid's value, which is exact because that value is the ramp itself inside a
falloff and 0 or 1 elsewhere. The GPU reads it as flag bits 256, 512 and 1024.

## Properties panel

The `useHue`, `useSaturation` and `useLuma` toggles sit above the ranges. Each range is a row: a
strip with four draggable handles and a centre notch over a gradient of its component, then an
**S** button (smooth, checkable), a **Recentre** button (the centre from the source colour,
offsets kept, one undo step) and a **Reset Zoom** button, with the five fields below, each
labelled above it. The trapezoid's base is left open; with smooth on its falloffs are drawn as
the smoothstep curves. Dragging the plateau moves the centre. Dragging an inner handle changes
its inner offset and carries the outer point along, stopping at the centre; shift-dragging it
leaves the outer point in place, stopping there too. Dragging an outer handle changes its outer
offset, stopping at its inner point. The mouse wheel zooms around the cursor and a middle or Alt
drag pans. The initial view and Reset Zoom show one hue turn centred on the centre, and 0 to 1
for saturation and luma in every model, OkLCh chroma included; a side with a point outside 0 to 1
is widened to it plus 5% of the span, so the luma defaults show about 0 to 2.1. A row, its S
button included, greys out while its toggle is off, and the strips grow with the panel's width.

## Hue fade

The hue term is multiplied by a smoothstep of the chroma, so a grey never matches a hue range and
a near-grey's unstable hue counts for little:

- HSV and HSL: the relative chroma (max - min) / max, from 0 at 0 to 1 at 0.05;
- OkLCh: C, from 0 at 0 to 1 at 0.02.

## Source colour and recentring

`sourceColor` is picked from the viewer with the swatch button or typed. Setting it, changing the
model or pressing **Recentre** sets each range's centre to the source's component, keeping its
offsets, in one undo step at the current time. The hue centre is wrapped into 0 to 1 and stays
where it is for a source with no chroma.

## Combination and output

The key is the product of the terms of the enabled components (`useHue`, `useSaturation`, `useLuma`, all
on by default), 1 when none is, or one minus that with `invert`. Alpha is the key in every
`outputMode`; RGB passes unchanged (**Alpha**), is multiplied by the key (**Premultiplied**) or is
replaced by it (**Matte**). The output is RGBA whatever the source. Mask and mix blend the result
with the source.

## GPU and cost

The kernel (`HSVKeyerOp` in `Engine/Gpu/Kernels/hue.slang`) follows the CPU statement for
statement with `precise` arithmetic (`Tests/Native/NativeHueGpu_Test.cpp`). The cost seed in
`HSVKeyer.costseed.inc` is HueKeyer's measurement, which predates the OkLCh model, the luma term
and the output modes; it is to be re-measured.
