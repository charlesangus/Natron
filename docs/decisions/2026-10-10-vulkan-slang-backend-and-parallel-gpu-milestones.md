# Vulkan compute + Slang confirmed; run the GPU milestones in parallel where possible

2026-10-10, user. The engine-wide GPU backend direction for M83 - GPU Compute Backend Spike is
confirmed as **Vulkan compute with kernels in Slang** — this is the "user confirming the backend
direction" half of M83's blocker. The spike still measures GL 4.3 compute and OpenCL 3.0 on the
same kernels as reference points, but it builds toward Vulkan + Slang rather than choosing between
them.

The user also asked that the GPU milestones (M81, M83–M87) run **in parallel as much as possible**.
So M83 starts now in its own worktree, overlapping the comp-profiling end of M82 - Render Cost
Profiling instead of waiting for its go/no-go (accepted risk: if profiling says GPU isn't worth it,
the spike is sunk cost). Dependencies as currently understood: M84 needs M83; M85 and M86 each need
M84 and can run side by side; M87 needs M85's profiles (or M82 evidence); M81 needs M80 (other
machine) plus M83, ideally M84. Within each milestone, independent tasks are dispatched
concurrently; builds still go one at a time through the natron-dev container.

Later the same day the OpenCL reference was dropped (no runtime on the host or in the image), so only GL 4.3 compute was measured. The outcome is recorded in `2026-10-10-gpu-compute-backend-chosen.md`.
