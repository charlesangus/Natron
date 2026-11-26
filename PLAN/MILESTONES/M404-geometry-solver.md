# M404 - Geometry Solver

A Houdini Solver SOP for native geometry: a `GeoSolverBegin`/`GeoSolverEnd` block whose body sees the previous frame's result, so any chain of `Geo*` nodes becomes a simulation. It builds on M402 - Geometry Loops' render contexts on the geometry path, and caches simulated frames in RAM with spill to disk in M403 - Geometry File Cache's format. The image side's equivalent is M190 - Cross-Frame Feedback; this is its geometry counterpart, kept as a dedicated block (user, 2026-11-12). M405 - Particle Solver Core builds particles on it.

Member of F18 - Geometry Loops And Solver: branches off and merges into `feature/f18-geometry-loops-and-solver` (PLAN-FORMAT §9b); the Feature's UAT covers it. Epic: E22 - Simulation And Solvers.

## Scope

**In:**
- `GeoSolverBegin` with outputs Prev Frame (the last solved result, or the Input at the start frame), Input (the current-frame upstream), and up to three extra live inputs passed through at the current frame.
- `GeoSolverEnd` with Start Frame, Substeps, Reset Simulation button, Cache Simulation toggle, and a RAM cache budget.
- Substeps: the body runs N times per frame; `loop.substep`, `loop.dt`, `@TimeInc`-style values reach knobs and wrangles (the kernel language's `@TimeInc`).
- Sim cache: per-frame details in a RAM store with its own budget, spilling to disk in the M403 format under the project cache directory. Any edit to an input or the body clears every frame after the start frame. Jumping ahead sims forward from the last cached frame, cancellably.
- Frames ordered: the block marks itself as needing frames in order, so `GeoFileCache` saves serially through it.

**Out:**
- Particle-specific integration and POP nodes (M405 - Particle Solver Core).
- Keeping frames before an animated edit (user chose reset on edit).
- A DOP-style multi-object network (none).

**Edge cases:**
- Viewing a frame before the start frame: outputs Input unchanged.
- Scrubbing backward: cached frames play back; uncached ones sim forward from the nearest earlier cached frame.
- A sim frame cancelled half-way: not stored; the next request resumes from the last whole frame.
- RAM budget exceeded: oldest frames spill to disk; with disk spill off, they drop and re-sim on demand.
- Upstream time-invariant input: Prev Frame still changes per frame, so solver output is always time-varying.
- Nested solver inside a For-Each, or For-Each inside a solver: contexts nest; the solver's frame store keys on the outer context too.
- Two viewers asking for different frames at once: one sim walk per solver, the second waits on the first's frames.

**Assumptions:**
- Unconfirmed: the RAM store follows `LoopFrameCache`'s keying (loop hash, context key, frame) with geometry entries, rather than a new cache class.
- Unconfirmed: the default RAM budget is a share of the geometry cache budget, set in Preferences.

## Phase 404.1: Solver block

- [ ] M404.P1.T1 — `GeoSolverBegin`/`GeoSolverEnd` with previous-frame feedback
  - files: `Engine/Nodes/Loop/GeoSolverBegin.{h,cpp,reg.inc}`, `Engine/Nodes/Loop/GeoSolverEnd.{h,cpp,reg.inc}`, `Engine/CMakeLists.txt`, `Tests/Native/GeoSolver_Test.cpp`
  - approach: Pair the nodes like `GeoForEachBegin`/`End` (M402 - Geometry Loops). At frame f > start, Begin's Prev Frame is End's result at f−1, fetched from the frame store or by solving f−1 first (iteratively, never recursively). Substeps run the body N times through render contexts carrying `loop.substep`/`loop.dt`. Mark the block as frame-ordered so writers serialise. No caching beyond an in-memory last-frame slot in this task.
  - verify: `GeoSolver_Test`: a body that moves points by `+1` in y per frame gives y = f − start at frame f; with 4 substeps and a body moving by `dt`, the same; frames before start equal the input; results identical at 1 and 8 threads.
  - size: L
- [ ] M404.P1.T2 — Sim frame store with RAM budget, disk spill and reset
  - files: `Engine/Nodes/Loop/GeoSolverEnd.{h,cpp}`, `Engine/Nodes/Loop/GeoSimCache.{h,cpp}`, `Tests/Native/GeoSolver_Test.cpp`
  - approach: Store solved frames keyed by (solver hash over inputs and body, outer context key, view, frame). Budget-limited RAM with LRU spill to the project cache directory through `GeoFileFormat` (M403 - Geometry File Cache). A hash change (any upstream or body edit) or Reset clears frames after start. Cancelling a frame stores nothing.
  - verify: `GeoSolver_Test`: solving to frame 50 then requesting 20 does no new body cooks; an edit then request 20 re-sims from start; a tiny RAM budget spills and reloads identical hashes; a cancelled frame resumes cleanly.
  - size: L

## Phase 404.2: Proof

- [ ] M404.P2.T1 — GUI check: playback, scrub and edit
  - files: `Tests/gui/m404_geo_solver.py`, `Engine/Nodes/Loop/docs/GeoSolver.md`
  - approach: Build `GeoPrimitive → GeoSolverBegin → GeoWrangle (@P += @v * @TimeInc) → GeoSolverEnd → GeoToUSD`, play 1–48, scrub back to 10, edit the wrangle, and check the sim resets and re-plays; cancel a long jump to frame 500 and check the UI stays responsive. Write the node docs fresh.
  - verify: the script passes under Xvfb; screenshots of frames 1, 24 and 48 shared with the user via SendUserFile for the Feature's UAT.
  - size: M

**Verification gate:** full ctest green including `GeoSolver_Test`; `Tests/gui/m404_geo_solver.py` passes under Xvfb; a headless `GeoFileCache` save of 100 solver frames succeeds and matches the interactive result hash for hash.

## Decisions

- 2026-11-12 — **Dedicated `GeoSolverBegin`/`GeoSolverEnd` block (user, /cat-plan grill),** not a cross-frame mode on GeoForEach: Solver SOP naming and controls, and clear nesting with For-Each.
- 2026-11-12 — **RAM store with disk spill, reset on any edit (user):** as Houdini; jumping ahead sims forward from the last cached frame and can be cancelled.

Shared design: `PLAN/DECISIONS/2026-11-12-geometry-simulation-expansion.md`.
- 2026-11-23 — Implemented (gate queued). PM defaults, to confirm at F18 UAT: one output per Begin with a Method knob (Prev Frame or Input), per M402; a new GeoSimCache store; a per-node Cache Memory knob (2048 MB) rather than a Preferences share; frames spill under the disk cache location (no project cache directory exists).
- 2026-11-25 (PM) — m404-gate3 failed two GUI timing checks (seek and cancel). Cause: GeoSolverEnd never polled `abortIfSuperseded()`, so a superseded solve kept the single task-graph backup thread and the kept-frame request queued behind it. Fixed in 1b948b0417 (abort at each substep and in the walk-lock wait; the cancel section jumps before raising substeps). m404-gate4 green; PR #385 merging into F18 once its checks pass.
- 2026-11-26 (PM) — PR #385 debug CI: GeoForEach/GeoRender aborted on the setParallelRenderArgsTLS abort-info assert (M402's, hidden because #365 merged with that CI red) — fixed in 9f3717ec98/69fdfa97fa (fallback args adopt the inherited abort, or own one). Then `AbortingTheMaximumIterationCountReturnsPromptly` returned 2.0-3.3 s after abort: the merge variant built all 100,000 iteration contexts serially before cooking any, unchecked (M402, 1ebc404a5d). Fixed in the next commit: each context is built as its iteration starts, and `GeoMerge::merge` gained a cancellable overload. PR CI rerunning.
- 2026-11-26 (PM) — PR #385 merged into F18 (a4ebefea13 green on debug PR CI). M404 done; F18 claimed for shipping.
