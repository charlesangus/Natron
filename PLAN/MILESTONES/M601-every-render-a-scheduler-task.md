# M601 - Every Render A Scheduler Task

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Follow-up to M199 - Remove Legacy Scheduler (user, 2026-11-25; design 6 of the frame-args comparison). Direct pulls run as scheduler tasks, so the task graph is the only render path and each task carries its own context. That covers knobChanged and analysis `getImage` on the main thread, OFX host-thread pulls, GPU placement's `ContextFrameArgsScope` paths, and the deep and task-graph-ineligible fallbacks that still use the recursive `renderRoI`. Once M199's overlay chain lands, the chain shrinks to "task context base + rare overrides". Relates to M198 - RenderEngine On Node.

Blocked on: M199 - Remove Legacy Scheduler landing its overlay chain, which this builds on.

Acceptance sketch:
- No render path calls the recursive `renderRoI` outside a scheduler task.
- Main-thread synchronous callers (knobChanged, analysis) still return their image, and abort and latency budgets hold.

## Scope

**Assumptions:**
- Unconfirmed: elaboration must settle which pulls can't become tasks (re-entrancy from inside a task, main-thread synchronous callers), and how abort and latency budgets are kept.
