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

Each is a 5-dimension double: a **Centre** and four offsets, **Outer Low**, **Inner Low**,
**Inner High** and **Outer High**. The term rises from 0 at centre - outer low to 1 at
centre - inner low, stays 1 to centre + inner high and falls to 0 at centre + outer high.
Offsets are never below 0, and an outer offset below its inner one counts as equal to it
(`KnobDouble::sanitizeRangeOffsets()`). The bake resolves the offsets into the same four absolute
points the CPU and GPU trapezoids read, so `hue.slang` is unaffected by the knob layout.

- **Hue** wraps around its centre, so the range may cross red; each offset counts up to a whole
  turn. Defaults 0.4; 0.085, 0.035, 0.035, 0.085.
- **Saturation** defaults 0.2; 0.18, 0.14, 0.2, 0.2. **Luma** defaults 0.5; 0.5, 0.45, 0.5, 1.5.
  The defaults suit OkLCh, the default model.

In the Properties panel the `useHue`, `useSaturation` and `useLuma` toggles sit above the ranges.
Each range is a row: a strip with four draggable handles and a centre notch over a gradient of
its component, a **Recentre** button (the centre from the source colour, offsets kept, one undo
step) and a **Reset Zoom** button, with the spinboxes below, each labelled above its field. The
trapezoid's base is left open. Dragging the plateau moves the centre; dragging a handle changes
its offset, an inner handle stopping at the centre; shift-dragging an inner handle moves its
outer partner with it. The mouse wheel zooms around the cursor and a middle or Alt drag pans;
Reset Zoom shows one hue turn centred on the centre, saturation 0 to 1 (0 to 0.4 for OkLCh
chroma) or luma 0 to the larger of 1 and the outer high point. A row greys out while its toggle
is off, and the strips grow with the panel's width.

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
