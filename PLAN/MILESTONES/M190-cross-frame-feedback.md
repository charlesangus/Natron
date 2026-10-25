# M190 - Cross-Frame Feedback

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

A cross-frame mode for the loop block (user, 2026-10-25): frame N's LoopBegin input is frame N−1's loop result, starting from a reset at a start frame. It covers accumulating trails, paint build-up and temporal smoothing as well as stepped 2D simulations (reaction-diffusion, fluid-like effects, several substeps per frame), with both use cases weighting the design equally (user). It needs a persistent per-frame result cache with explicit invalidation when anything upstream changes, and bake controls like Houdini's file cache.

Blocked on: M182 - Loop Feedback, whose within-frame back-reference and RoI propagation this extends across frames.

Acceptance sketch:
- A trail effect built with a loop in cross-frame mode accumulates correctly when played forward from the start frame, and re-renders correctly after an upstream edit.
- A reaction-diffusion graph with 4 substeps per frame plays back from a baked cache without recomputing earlier frames.
- Scrubbing backwards or jumping ahead renders correctly (recomputing from the nearest cached frame), never from a stale result.

## Scope

**Assumptions:**
- Unconfirmed: the cache stores results in RAM with an optional disk bake, invalidated by the upstream node hash.
- Unconfirmed: random access to frame N without a cached N−1 recomputes from the start frame or nearest cached frame, with progress shown.

## Decisions

- 2026-10-25 — **Design for trails and simulations equally (user).**
