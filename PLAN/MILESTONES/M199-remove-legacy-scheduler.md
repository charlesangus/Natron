# M199 - Remove Legacy Scheduler

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Delete the Legacy render scheduler mode (`renderSchedulerMode`, `Engine/Settings.cpp:391`, and `NATRON_RENDER_SCHEDULER=legacy`), leaving the task graph from M63 - Task-Graph Render Scheduler as the only path. Two pieces of M63 scaffolding go with it: per-effect thread-local frame args, which the task graph fills on demand from `FrameRenderContext`, and the per-frame `RenderThreadTask` QThreads that only block on the scheduler in task-graph mode, plus the dead `processFrame` path. Moved from M31 - Architectural Cleanup (cancelled 2026-10-25).

Blocked on: the user deciding the task graph has proven itself enough to drop Legacy.

Acceptance sketch:
- No Legacy scheduler mode, setting or environment value remains.
- Render code reads frame args from the frame context, with no per-effect TLS frame args.
- The scheduler owns frame admission; no per-frame feeder QThreads remain.
- Whole ctest green; bench range renders no slower.

Carried from M31.P3.T1 — Per-render context object replacing per-effect TLS frame args:

  - files: `Engine/TLSHolder*`, `Engine/ParallelRenderArgs.*`, `Engine/FrameRenderContext.*`, `Engine/EffectInstance*`
  - approach: M63 materialises `EffectTLSData` from a per-frame context on demand and inherits it along spawner chains; the end state is tasks reading a `FrameRenderContext` directly (args map, request, abort info) with no per-effect thread-local frame args at all. Also fix `FrameView_compare_less` (`ParallelRenderArgs.h`: returns true for `lhs.view > rhs.view`, not a strict weak ordering; multi-view only).
  - verify: whole ctest both scheduler modes; `GraphScalingTLS` and `FrameRenderContext` suites rewritten to the new model.
  - size: L

Carried from M31.P3.T2 — Collapse `RenderThreadTask` frame feeders into the scheduler and delete the dead FFA `processFrame` image path:

  - files: `Engine/OutputSchedulerThread.*`, `Engine/RenderScheduler.*`
  - approach: writers' frames are fed by `RenderThreadTask` QThreads that only block on a future in task-graph mode; the scheduler could own frame admission itself. `processFrame` (`OutputSchedulerThread.cpp:~2432`) is dead for FFA renders.
  - verify: whole ctest both modes; the writer abort tests; bench range renders no slower.
  - size: L

## Scope

**Out:**
- The trimap, which M201 - Trimap Removal owns once this lands.
- RenderEngine ownership, which M198 - RenderEngine On Node owns.

**Assumptions:**
- Unconfirmed: "proven itself" means no open render-correctness or performance regression attributed to the task graph.

## Decisions

- 2026-10-25 — **Scheduler hand-offs become Legacy removal:** both scheduler modes still ship, and the shim and feeder threads only become removable once Legacy goes (user, at M31 triage).
