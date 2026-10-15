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

Blocked on: M85's benches (P6.T3 and P6.T4), which show which follow-ups matter on real comps with Transform, Erode and ChromaKeyer kernels. They have landed; see below.

Acceptance sketch:
- On the M82 comps, no region halves its strips after the first frame, and contention halvings drop to zero or are explained.
- A forced CPU fallback of a big Blur runs at the normal thread budget.
- Per-member GPU times in profiles come from timestamps.
- An idle Natron's pinned host memory falls to the configured floor.

## Follow-ups found by M85 - Native GPU Kernels

Ranked by what the benches showed (`DESIGN/2026-10-10-gpu-compute-backend.md` §10, M85's decisions):
- **Exposed transfer in unstripped regions** is the largest cost on the M82 comps: 59–84% of region time, ~0.5 ms/Mpx and ~2 ms/Mpx for Transform. cg_multipass has six lone-Grade regions that each move ~171 MB for a 0.3 ms kernel. Overlap their transfer, strip them even when they fit, or let placement refuse a lone cheap point op whose transfer outweighs its kernel. The last is the likeliest to turn negative on a slower bus.
- **Plain Transform regions spend 55–60% of their time outside member kernels and transfer** ("other" in the profile), at every size. Unexplained.
- **Exposed transfer rises again at 24k** for ChromaKeyer and Erode 40 (0.24–0.40 ms/Mpx, against 0.02–0.07 at 16k).
- **A lone spatial node's region `kernelMs` is 1.7–3× its isolated kernel time.** It is probably the halo pass over cold, just-uploaded input, which is unconfirmed.
- **A region whose member had an empty input slot hung** instead of falling back to the CPU. Not diagnosed, because ptrace isn't allowed in the container.
- **`collectPreRenderFrames` records one edge** when a node feeds two inputs of one consumer, contradicting `FrameGraph::Task`'s comment.
- **Blur's narrow vertical pass** (σ3, r ≤ 32) is still ~10× its horizontal pass.
- **`GpuTaskTraits` has no Erode size or Blur filter detail** beyond `iir` and `motionBlur`, so Erode seeds at size 6 and box/triangle/quadratic use the FIR rows. Motion blur at quality 4 isn't seeded.
- The existing list above still stands. The benches saw no fallbacks or retries, so the strip-height memory and contention items didn't bite on these comps.
