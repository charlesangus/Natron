# M136 - Edge Policy Investigation

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Decide, in one principled place, what an image does outside its bounds: black outside versus stretched edge. Today it is scattershot. M112 - Native Node Behaviour Fixes (`M112.P3.T2`) made Crop read black outside its source, so Crop now behaves as if "black outside" is always on, whatever the incoming image is. A Crop larger than the source should respect the source's black-outside-ness: a stretch-edge image shows stretched edges, a black-outside image shows black. The user suspects Nuke does this with a black-outside bit in the image metadata plus a virtual black pixel around the image, so the last real pixel is always the one that stretches. Investigate that model and how it fits Natron's RoD, caching and GPU paths, then survey every node that has its own black-outside, boundary or edge-extend handling: Crop, Reformat, Transform, the warp nodes, Blur and the other filters.

Blocked on: a grill session with the user to settle the model, and M113 - Native Warp Nodes and M110 - Remaining Native GPU Ports if they land first and fix per-node behaviour that this would then replace.

Acceptance sketch:
- A design note the user approves: where the black-outside state lives, how it propagates, and which nodes read or set it.
- Crop larger than its source gives stretched edges for a stretch-edge input and black for a black-outside input.
- The same rule holds on the CPU and GPU paths, and for every node in the survey.

## Scope

**In:**
- A design note the user approves, then the implementation across the surveyed native nodes (Crop, Reformat, Transform, the warp nodes, Blur and the other filters), CPU and GPU.
- A per-image black-outside bit, stored as a `NodeMetadata` field (rides M167 - Per-Frame Node Metadata's per-(time, view) propagation and hashing). Values: black outside or stretch edge.
- Sources (Read, generators) default to stretch edge. Nodes that do not touch the bit forward their primary input's bit.
- Crop, Reformat and Transform replace their boolean Black Outside knob with a tri-state: Input (default), Black, Stretch. The output bit follows the choice. Blur and the other filters get the same per-filter knob.
- Consumers read outside an input's RoD by bit-driven sampling: clamp to the nearest pixel for stretch, zero for black. One shared CPU helper and one shared GPU sampler. No materialised 1-pixel black border in the RoD.
- The resolved edge state shown in the node's metadata/info readout.
- Crop larger than its source respects the source's bit.

**Out:**
- Mirror, repeat and other edge modes: none (the bit is binary).
- Roto, shape and tracker black-outside code (`Engine/Roto*`, `Bezier.cpp`, `TrackerContext*`): none.
- OpenFX plugin changes or an OFX extension: none.
- Deep images and deep nodes: M21 - Deep Tier-2 And Bridges.
- A node-graph badge or any other new graph UI.

**Edge cases:**
- Multi-input nodes (Merge, Keymix, Copy): the output bit is the primary input's bit (B, or the first input).
- Switch, Dissolve and other identity nodes: follow the picked input's bit, per M167's identity rule.
- OpenFX nodes are opaque: they ignore the input bit and their output is the source default (stretch).
- Crop smaller than its source: the Crop's own tri-state governs what lies outside the crop rectangle.

**Assumptions:**
- Unconfirmed: the bit is hashed as part of `NodeMetadata`, so it invalidates caches without separate handling.
- Unconfirmed: the Nuke-style behaviour the user describes is a metadata bit plus a virtual black pixel; the design note confirms it only as far as needed, since sampling is bit-driven here.
- Unconfirmed: the filter list for the per-filter knob is every native node that samples outside its source RoD, found by the survey.

## Decisions

- 2026-10-25 — Deliverable is a design note followed by implementation: the acceptance sketch already requires working CPU and GPU behaviour.
- 2026-10-25 — State is a per-image bit, Nuke-style: one principled place that propagates through the graph, and the only way Crop can respect its input.
- 2026-10-25 — Sources default to stretch edge and pass-through nodes forward the input's bit: the user's choice, so a plain Read stretches when transformed.
- 2026-10-25 — Crop, Reformat and Transform get a tri-state Input / Black / Stretch knob: Input follows the incoming bit and the user can force either; the fork has no backward compatibility, so the knob type changes freely.
- 2026-10-25 — Blur and the other filters get a per-filter knob like Transform, rather than a fixed rule: the user wants control per filter.
- 2026-10-25 — The bit is a `NodeMetadata` field and M136 depends on M167 - Per-Frame Node Metadata: this reuses its propagation and hashing and avoids two metadata mechanisms.
- 2026-10-25 — Consumers sample by bit with no materialised border: keeps the RoD the real pixel extent and avoids the RoI and cache interactions of a 1-pixel RoD growth.
- 2026-10-25 — OpenFX nodes are opaque and reset the chain to the default: OFX clip preferences have no slot for the bit.
- 2026-10-25 — Multi-input nodes take the primary input's bit: matches how PAR and format already resolve.
- 2026-10-25 — Verification gate is a per-node CPU/GPU parity matrix over both bit values, plus a Crop-larger-than-source case and an Xvfb GUI check of the tri-state knobs with screenshots.
- 2026-10-25 — Edge state is visible only in the metadata/info readout: no new graph decoration.
