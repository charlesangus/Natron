# rgba, rgb, alpha and xy replace the Color layer

2026-09-26. User decision, via /cat-plan. The single user-visible "Color" layer is replaced by four built-in registry layers, each with its own ID: `rgba`, `rgb`, `alpha` and `xy`. They replace Color in every menu, in the channel-set, layer-select and channel-select knobs, in Shuffle, in the viewer, in Python and in project files.

They are views of one stored colour plane and share its channels, so `rgb.R` *is* `rgba.R`. The storage/OFX ID `kFnOfxImagePlaneColour` stays internal. OFX maps by component count: 4 ⇄ rgba, 3 ⇄ rgb, 1 ⇄ alpha, 2 ⇄ xy.

`rgba`, `rgb` and `alpha` are always present. A colour channel the stream lacks reads as zero. `xy` is present only for the XY layout.

The change is a clean break with old projects, with no alias. Colour layer values in old projects reset to `rgba` with a warning, and loading must never crash.

Implemented by M65; M37 stacks on it.
