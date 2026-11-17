# Channel networks drive animation (CHOP model)

Animation and expression driving follows Houdini's CHOP model (user, 2026-11-17).

- **Networks.** A channel network is a `NetworkBuilder` container, placed in the comp graph like any node. Its nodes pass channel sets: named sample arrays over a frame range.
- **Cooking.** A network cooks its whole frame range, the project range unless a node overrides it, into a cache keyed by input hash. Knobs read cached samples and interpolate sub-frames. History filters such as Lag and Spring are exact, and render threads never cook.
- **Export.** An export overrides a target knob's value, keeping its keys and expression but making them inactive. Driven knobs show a distinct colour and a jump-to-network action.
- **Expressions.** Expressions get a `chop("<network>/<node>/<channel>")` read.
- **Sources.** Generators, knobs and curves, roto and shape curves, tracks, geometry, cameras (point projection), image sampling and audio files are all in scope.
- **Animation layers.** M453 - Follow Animation Layer builds its layers on channel networks, as Houdini's animation layers are built on CHOPs.
- **Shipping.** Everything ships as one Feature, F32 - Channel Networks.

A per-knob visual expression graph and baking to keys were rejected as the primary model.
