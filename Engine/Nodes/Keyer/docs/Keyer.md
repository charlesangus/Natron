# Keyer

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| Keyer | transcendental | `keyerTolerance()` | |

- **Redesign, no migration.** The key source is Luminance (by default the working space's luma weights), Red, Green or Blue; a channel source keys on that channel alone. The alpha is the trapezoid A <= B <= C <= D over the key value, RGB passes through unpremultiplied, and the host mask and mix replace the InM, OutM and Bg inputs. Despill, the key colour and its Colour key source, the output and source-alpha modes and the Screen and None key modes are gone; projects saved with an earlier Keyer do not load its settings.
- **Range widget.** The range is the trapezoid strip HSVKeyer's ranges use (`KnobDouble::setRangeWidget()`, `Gui/KnobGuiTrapezoid`), with the luma role and no source colour, model, enable or smooth knob, so the strip has no source tick and no Recentre or Smooth button. Its five values are a centre and four offsets (outer low, inner low, centre, inner high, outer high); `keyerRangePoints()` in `KeyerRange.h`, shared with HSVKeyer, turns them into A, B, C and D. Offsets below 0 count as 0, so the points are always in order. The default, 0.1, 0.4, 0.5, 0.4 and 0.1, is the trapezoid 0, 0.1, 0.9, 1.

Keyer holds its CPU class, transcendental, for every key source (Luminance, Red, Green, Blue) and luminance formula, hard and soft edges, the host mask, inverted mask and mix, and the alpha channel alone; the CPU and the device compute the key value and the ramps in float with the same roundings, and only the device's division differs (`Tests/Native/NativeKeyerGpu_Test.cpp`).
