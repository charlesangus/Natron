# M109 - GPU Region Engine Hardening

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Fix the region-engine follow-ups M84 left, once real kernels show which of them bite:
- remember a strip height that worked per region shape, since heavy-scratch kernels otherwise walk the evict → serial → halve ladder on their first frame;
- wait briefly for a contending region before halving strips;
- import first-frame download destinations ahead of time on a helper thread, which needs `GpuTransfer::resolveImports` outside `importMutex`;
- give a CPU fallback a real thread budget, not 1;
- add an idle trim or a setting for `HostImageMemory` (up to RAM/4 plus pins);
- measure per-member GPU time with timestamps instead of the cost-model estimate;
- make multi-plane tasks placement candidates, so Shuffle-family nodes can join regions (M110).

Blocked on: M85's benches (P6.T3 and P6.T4), which show which follow-ups matter on real comps with Transform, Erode and ChromaKeyer kernels.

Acceptance sketch:
- On the M82 comps, no region halves its strips after the first frame, and contention halvings drop to zero or are explained.
- A forced CPU fallback of a big Blur runs at the normal thread budget.
- Per-member GPU times in profiles come from timestamps.
- An idle Natron's pinned host memory falls to the configured floor.
