# M222 - User Control Of GPU Placement

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Investigate giving the user control over where nodes execute and where GPU residency starts and ends, on top of the automatic placement. Candidates the user named: a per-node execution knob with CPU / Auto / GPU options (Auto being today's placement), and/or a node that forcibly begins or ends a GPU region. The investigation compares these (and combinations) against the placement layer, the GPU weights and the viewer hand-off, then the chosen design is planned and built.

Blocked on: M239 - Weight-Based GPU Placement landing its placement rule (a region goes to the GPU when a member is Heavy or its summed weight reaches a threshold, above a pixel floor), which any manual control has to compose with; then the user's choice of control model from the investigation's options.

Acceptance sketch:
- A written comparison of the control models (per-node knob, region begin/end nodes, both), covering how each composes with automatic placement, GPU weights, unsupported formats or ops, and the viewer hand-off, with a recommendation the user signs off.
- The chosen control is implemented: a user can force a node onto the CPU or the GPU, or force a region boundary, and the placement honours it, falling back to the CPU with a visible reason when the GPU can't run it.
- The node graph shows where each node executes and where GPU regions begin and end.
- Placement tests cover forced CPU, forced GPU, forced boundaries, and the fallback.
