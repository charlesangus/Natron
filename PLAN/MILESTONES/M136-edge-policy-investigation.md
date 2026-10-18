# M136 - Edge Policy Investigation

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Decide, in one principled place, what an image does outside its bounds: black outside versus stretched edge. Today it is scattershot. M112 - Native Node Behaviour Fixes (`M112.P3.T2`) made Crop read black outside its source, so Crop now behaves as if "black outside" is always on, whatever the incoming image is. A Crop larger than the source should respect the source's black-outside-ness: a stretch-edge image shows stretched edges, a black-outside image shows black. The user suspects Nuke does this with a black-outside bit in the image metadata plus a virtual black pixel around the image, so the last real pixel is always the one that stretches. Investigate that model and how it fits Natron's RoD, caching and GPU paths, then survey every node that has its own black-outside, boundary or edge-extend handling: Crop, Reformat, Transform, the warp nodes, Blur and the other filters.

Blocked on: a grill session with the user to settle the model, and M113 - Native Warp Nodes and M110 - Remaining Native GPU Ports if they land first and fix per-node behaviour that this would then replace.

Acceptance sketch:
- A design note the user approves: where the black-outside state lives, how it propagates, and which nodes read or set it.
- Crop larger than its source gives stretched edges for a stretch-edge input and black for a black-outside input.
- The same rule holds on the CPU and GPU paths, and for every node in the survey.
