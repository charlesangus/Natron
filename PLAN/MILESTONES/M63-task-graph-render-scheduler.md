# Milestone 63: Task-graph render scheduler

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

The 2026-09-25 bench found HD renders using 1.0–1.7 of 4 cores. At any moment one node renders, and whatever parallelism exists comes from that plugin's own multithread-suite call (it gets 3 workers). Independent branches of the graph never render concurrently, and per-node serial work (the host's setup and copies, a plugin's single-threaded sections) leaves cores idle. This milestone replaces the recursive, pull-per-node `renderRoI` with a scheduler: build the frame's dependency graph (nodes × RoIs) up front, then execute ready nodes on a shared thread pool, so independent branches and consecutive frames overlap and the multithread suite shares the same pool instead of oversubscribing. It keeps the pull model's RoI and caching semantics, and must interoperate with the deep/typed-edge nodes from M17/M18. Render ownership (M31's `RenderEngine`-on-`Node` refactor) may need to come first or together; decide that at elaboration.

Blocked on: M62's re-benchmark (M62.P6.T1). Its `## Decisions` must show how much HD wall time is spent under-occupied once the algorithmic hotspots are gone, and whether the remaining cost is per-node host overhead or plugin pixel work. If occupancy is already good, this milestone shrinks or is cancelled. Elaboration also needs a design pass (opus consultant) over `EffectInstanceRenderRoI.cpp`, `ParallelRenderArgs`, `AppTLS` and the OFX multithread suite, recorded as a project-wide decision.

Acceptance sketch:
- HD comp 100/300 and wide 100/300 renders keep all 4 cores busy (`sample_states.sh` mean ≥3.2 running threads) and render at least 1.8x faster than after M62.
- Chains get no slower. Pixels match the old renderer bit for bit across the ctest render suites and the OFX integration tests.
- Aborting a render (viewer scrub) still cancels promptly.

## Decisions
- 2026-10-04 — **First elaboration rejected by the user** (plan commit `364ae032c` holds it for reference; its decision draft is removed). It proposed making the existing pull concurrent instead of a task graph and re-based the targets to this 4-core host's ceiling. The user's direction: re-run the design consultant clean on the `fable` model without exposure to that run; the design must scale to workstations with dozens of threads and hundreds of GB of RAM as well as modest machines, and must handle graphs with both large depth and large breadth; keep the task-graph architecture unless a clearly superior architecture emerges that meets those requirements.
