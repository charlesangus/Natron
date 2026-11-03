# M321 - Render Thread Count Race

A Write render can hang forever under the task-graph scheduler: all of its frame threads get told to quit while frames are still queued (PM, 2026-11-03, found while gating the loop stack; reproduced on main-equivalent code at 3/250 under load). Diagnosis: /home/bosley/.claude/jobs/2d4837a1/tmp/gdbrun/hang-b-1.log and renderiso-stacks-*.txt.

Deps: none.

## Scope

**In:**
- `OutputSchedulerThread::adjustNumberOfThreads` reads the thread count under `renderThreadsMutex`, unlocks, and appends later under a fresh lock, so two finishing frame threads can both decide to create one (5 threads against a cap of 4).
- `getNRenderThreads()` counts threads already flagged to quit and quit-but-unswept threads, so every later completion flags another live thread (`stopRenderThreads(1)`) until none is left.
- Since M237 a quitting thread leaves queued frames to the others (`pickFrameToRender`), so the stranded frames are never rendered and the scheduler waits on `bufEmptyCondition` forever.
- Possible second mechanism (unconfirmed): a lost wakeup between `threadLoopOnce`'s `resolveState()` check and its `bufMutex` wait when `onAbortRequested` fires.

**Out:** scheduler redesign.

## Phase 321.1: Fix

- [x] M321.P1.T1 — Count only live render threads and decide under one lock
  - files: Engine/OutputSchedulerThread.cpp
  - approach: add `getNLiveRenderThreads()` (listed threads with `!mustQuit()`, caller holds `renderThreadsMutex`). In `adjustNumberOfThreads`, read `getLoad()` and the pool's active count before locking (no lock nesting inside the RenderScheduler's), then under one `renderThreadsMutex` lock create a thread if live == 0 or (live < cap and load allows), else flag exactly one live thread if live > cap; wake `framesToRenderNotEmptyCond` after unlocking. Apply the same live count to the legacy branch. `stopRenderThreads(n > 0)` never flags the last live thread.
  - verify: T2's test passes; RenderIsolation loops clean under load (the PM reruns the docker loop: 0 hangs in 500 under 8 busy loops, taskgraph).
  - size: M
- [x] M321.P1.T2 — Deterministic test: lowering parallel renders mid-render finishes
  - files: Tests/RenderIsolation_Test.cpp
  - approach: `RenderIsolation.LoweringParallelRendersMidRenderFinishes` — `renderWithActionAtFrame` with an own-thread action that sets the `nParallelRenders` setting to 1 mid-render; expect finish code 0 and all 20 frames written; restore the setting. (Under gdb, forcing the cap to 1 mid-render hung 5/5.)
  - verify: fails (hangs → timeout) without T1, passes with it
  - size: M
- [x] M321.P1.T3 — Check the abort lost-wakeup
  - files: Engine/OutputSchedulerThread.cpp, Engine/GenericSchedulerThread.cpp
  - approach: confirm or rule out the `resolveState()` / `bufMutex` wait race on abort; if real, re-check the state under `bufMutex` before waiting.
  - verify: reasoning in the Decisions, plus a test if the race is real
  - size: M

**Verification gate:** CI green; the RenderIsolation docker loop clean.

## Decisions

- 2026-11-03 — Filed by the PM: a hang that predates and is independent of the loop stack (M180–M190, M259), which it was blocking. Not new feature work.
- 2026-11-03 — Merged as c30dff64c (PR #180). Under load (8 busy loops) RenderIsolation.* passed 500/500 taskgraph and 100/100 legacy; the regression test requires more than one live render thread when the cap is lowered.
