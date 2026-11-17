# M486 - Channel Network Core

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

The engine and container for F32 - Channel Networks (`PLAN/DECISIONS/2026-11-17-channel-networks.md`). A channel set is a new port type: named channels, each a sample array over a frame range at a sample rate, with its own interpolation. `ChannelNetwork` is a `NetworkBuilder` (M351 - Shader Graph Presentation) placed in the comp graph. Its nodes are generic network nodes with a cook entry, `targets: [channel]`. A network cooks its whole range, the project range unless a node overrides it, into a cache keyed by the hash of its inputs and knobs. Readers sample the cache and interpolate sub-frames; render threads never cook. This milestone lands the core plus generators: Constant, Wave, Noise, and Fetch Knob/Curve, which reads any knob's animation as channels. It also adds a channel view in the curve editor, so a node's output can be inspected.

Blocked on: M349 - Shader Node Spike's design note and F2 - Native Shader Networks landing the generic core (M350 - Shader Manifest And Nodes, M351 - Shader Graph Presentation).

Acceptance sketch:
- A Wave → Noise network shows its channels in the curve editor, and editing a knob recooks within one UI frame for 10 channels × 1000 frames.
- Fetch Knob of an animated Transform reproduces its curves exactly at integer frames.
- A saved project restores the network and its cache key; the cache itself is not saved.

## Scope

**In:**
- The channel set type, the `ChannelNetwork` container, the cook scheduler and cache, generators, Fetch Knob/Curve, and the curve-editor channel view.

**Out:**
- Export and `chop()`: M487 - Channel Export And chop().
- Operators and Channel VOP: M488 - Channel Operators And Channel VOP.
- Roto, track, geometry and camera sources: M489 - Channel Sources From Shapes, Tracks And Cameras.
- Image and audio sources: M490 - Channel Sources From Images And Audio.

**Edge cases:**
- A Fetch of a knob that this network itself drives (through M487's export): a dependency cycle, refused with a node error naming the knob.
- A range change: recook, with cache keys including the range.

**Assumptions:**
- Unconfirmed: the default sample rate is the project frame rate, one sample per frame, overridable per node as Houdini's is.
