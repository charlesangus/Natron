## Pending

### 2026-11-25T15:48:34Z — change-request
- refs: M199
- New milestone (E14), user-requested 2026-11-25 follow-up to M199 - Remove Legacy Scheduler: **Every Render A Scheduler Task** (design 6 of the frame-args comparison). Make direct pulls (knobChanged/analysis getImage on the main thread, OFX host-thread pulls, GPU placement's ContextFrameArgsScope paths, the deep and task-graph-ineligible fallbacks that still use the recursive renderRoI) run as scheduler tasks, so the task graph is the only render path and each task carries its own context. After M199's overlay chain lands, the chain then shrinks to "task context base + rare overrides". Relates to M198 - RenderEngine On Node. Needs elaboration: which pulls can't become tasks (re-entrancy from inside a task, main-thread synchronous callers), and how to keep abort and latency budgets.

- F7 - Natron Utility Core UAT approved (user 2026-11-25): "approved for now, we can revisit the performance on very large scenes later if it comes up in practice".
