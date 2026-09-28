# rgba, rgb, alpha and xy replace the Color layer

2026-09-26. User decision, via /cat-plan. The single user-visible "Color" layer is replaced by four built-in registry layers, each with its own ID: `rgba`, `rgb`, `alpha` and `xy`. They replace Color in every menu, in the channel-set, layer-select and channel-select knobs, in Shuffle, in the viewer, in Python and in project files.

They are views of one stored colour plane and share its channels, so `rgb.R` *is* `rgba.R`. The storage/OFX ID `kFnOfxImagePlaneColour` stays internal. OFX maps by component count: 4 ⇄ rgba, 3 ⇄ rgb, 1 ⇄ alpha, 2 ⇄ xy.

`rgba`, `rgb` and `alpha` are always present. A colour channel the stream lacks reads as zero. `xy` is present only for the XY layout.

The change is a clean break with old projects, with no alias. Colour layer values in old projects reset to `rgba` with a warning, and loading must never crash.

## Rulings settled during implementation

- **Widen on write, explicit rows only.** When a node's channel set explicitly names a colour view that selects a channel the stream lacks (for example `rgba` on a Grade over an RGB input), the output colour plane widens to RGBA. The colour inputs widen with it, and the channels they lack are zero-filled. `All` and regex rows never widen, so a default-All Blur leaves an RGB JPEG as RGB. Writers never widen. The step runs at the end of metadata checking, reads the inputs' own layouts rather than the plug-in-clamped one, and fires only when the plug-in supports RGBA. Ordinary plug-in conversion (a JPEG into Merge's A input, say) still fills a missing alpha with 1. The two are kept apart on purpose.
- **Consequences worth knowing.** Merge's default is a full `rgba` row, so Merge of two RGB inputs now outputs RGBA with alpha 0. An identity Grade on `rgba` over RGB passes its input through with alpha 1.
- **Shuffle.** Reading a colour channel the input lacks gives 0 with no error. A missing non-colour layer or channel still fails the render. Implicit sources between colour views wire by colour bit (so `alpha` reads rgba's A). The colour output keeps the main input's layout when that covers what it writes, otherwise it is RGBA. Overlapping output views make Out 2 count as None; disjoint views merge into one plane.
- **Clean break.** On load, any layer value naming the old storage ID becomes `rgba` with its channels kept, and each affected node gets one warning: "Colour layer from an older project was reset to rgba". Natron ≤ 2.2 colour choice options are no longer rewritten. Python layer setters raise `ValueError` on the old ID. Third-party scripts that hard-code `uk.co.thefoundry.OfxImagePlaneColour` must switch to a view name.

Implemented by M65; M37 stacks on it.
