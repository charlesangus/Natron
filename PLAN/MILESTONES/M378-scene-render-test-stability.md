# M378 - Scene Render Test Stability

Three Cycles progressive-refinement tests fail under `ctest -j`, plus `USDRenderCyclesTest.EmissionMatchesReferenceRender` aborted with heap corruption in run 42757353797. Two causes are product races in hdCycles (the charlesangus/cycles fork, pinned by `NATRON_CYCLES_COMMIT`/`SHA256` in `tools/ci/local/Dockerfile`, pin 3dbd87c10; local clone `build/cycles-fork`): lock-free AOV-binding and render-buffer access, and a likely deadlock between Cycles' scene lock and TBB task stealing. The third, the samples 1-vs-12 wall-time wait, is already fixed on main by #243 (09d6d7e37). Fix the fork where reading shows a race, and make a stalled viewport render fail fast with stacks.

Evidence (consultant, code at the fork pin):
- **Heap corruption:** `SyncAovBindings` copy-assigns `_aovBindings` (hydra/session.cpp:149) while the session thread's `HdCyclesOutputDriver` walks it by reference (output_driver.cpp, session.h:53); the "Unknown pass elementId" warning (:154) precedes both recorded aborts. `Map()` resizes `_data` lazily (render_buffer.cpp:77-79) from our reader (`HydraRenderImpl.cpp:976`) and the session thread at once; `Allocate` changes sizes after a racy `IsMapped`; `Finalize` deallocates while the session thread may hold the pointer.
- **Hang (`SceneViewportStorm.ARefinementFailureClearsAtTheNextSceneEdit`):** one call blocks while holding `stageMutex`. H1 (likely): the session thread holds `scene->mutex` through the scene update, waits on TBB tasks, steals a Hydra rprim Sync task that takes `SceneLock` on the same mutex, and never finishes. H2: slow, not deadlocked. H3: a Cycles cancel that never returns in `dropEngine`.

## Scope

**In:**
- Locks in the fork for `_aovBindings` and Map/Allocate/WritePixels/Finalize, plus the Dockerfile pin and image.
- A per-test watchdog that dumps stacks.
- A fix for the hang.
- Re-verifying the cap test, and 20× repeat gates.

**Out:**
- M273 - RenderIsolation Exit Hang's RenderIsolation stall.
- Sanitizer builds.

**Assumptions:**
- CI allows ptrace for `eu-stack` (unverified; fall back to gdb launch as in M273.P1.T1).
- M379 - GpuContext Test Segfault's `--repeat-test` tooling is merged first.

## Phase 378.1: hdCycles thread safety (fork)

- [x] M378.P1.T1 — Guard the AOV bindings so they cannot change while a tile is being written
  - files: fork `src/hydra/session.{h,cpp}`, `src/hydra/output_driver.cpp`, `src/hydra/render_pass.cpp`
  - approach: add an `_aovMutex`. `SyncAovBindings`, `RemoveAovBinding`, `Set/GetDisplayAovBinding` take it. `update_render_tile`/`write_render_tile` hold it for the whole loop, so `Finalize` waits out a write in progress. `GetAovBindings` returns a copy, used by render_pass.cpp's `IsConverged`/`ResetConverged`.
  - verify: every `_aovBindings` access is under the lock (review); the fork builds in P1.T3's image build
  - size: M
- [x] M378.P1.T2 — Make the render buffer's storage and size thread-safe
  - files: fork `src/hydra/render_buffer.{h,cpp}`
  - approach: add a buffer mutex. `Allocate` resizes `_data` immediately under the lock; `Map` takes the lock and holds it until `Unmap`; `WritePixels` only while mapped; `_Deallocate` and the size/format getters take the lock; `Map` never resizes.
  - verify: review; a Map/Allocate/Map sequence leaves `_data.size()==_dataSize`
  - size: M
- [x] M378.P1.T3 — Move the image's Cycles pin to the locked fork commit
  - files: `tools/ci/local/Dockerfile` (`NATRON_CYCLES_COMMIT`/`SHA256`)
  - approach: push T1 and T2 to `natron/deep-5.2` (rebase on 4727b1c51, already ahead of the pin), compute the archive sha256, update the ARGs.
  - verify: the PR's dev image job builds and its `build-cycles.sh` checks pass
  - size: M

## Phase 378.2: Stall watchdog and hang fix

- [ ] M378.P2.T1 — A per-test watchdog that dumps thread stacks and aborts
  - files: new `Tests/TestWatchdog.h`, `Tests/SceneViewport_Test.cpp`, `Tests/Scene/ProgressiveRender_Test.cpp`
  - approach: started in fixture `SetUp` with a 150 s limit; callers report the current phase (`render`, `syncScene`, `refineTo`). On expiry: `prctl(PR_SET_PTRACER_ANY)`, `eu-stack -p getpid()` (fall back to `gdb -p -batch`), print test name and phase, `std::abort()`.
  - verify: a test-only case sleeping past a 2 s limit aborts and prints its own stack (PM build)
  - size: M
- [ ] M378.P2.T2 — Stop task stealing while Cycles' scene lock is held (H1)
  - files: `Engine/Nodes/Scene/HydraRenderImpl.cpp` (renderGuarded, Cycles makeEngine/dropEngine); fork `src/session/session.cpp`
  - approach: in the fork, run the locked part of `run_update_for_next_iteration` through `isolate_task`; on our side, run Cycles engine calls (Render, make, drop) in a dedicated `tbb::task_arena` owned by the worker and by RenderThread::cpu. If P3.T2's stacks show H2 or H3 instead, replace this task with that fix.
  - verify: review; P3.T2's repeat shows no stall
  - size: M

## Phase 378.3: Gates (PM runs)

- [ ] M378.P3.T1 — Re-verify `ThePrimsSettingsAreTheEnginesAndTheCapAppliesOverThem` with #243
  - files: none
  - approach: covered by P3.T2's `ProgressiveRenderTest` repeat; no code change expected
  - verify: 20/20 per scheduler
  - size: S
- [ ] M378.P3.T2 — Repeat under load on remote debug builds
  - files: none
  - approach: `build.sh debug --tests --repeat-test` for `SceneViewportStorm`, `ProgressiveRenderTest` and `USDRenderCyclesTest`, 20 each (one run per suite if `--repeat-test` takes one), both schedulers
  - verify: 20/20 per suite per scheduler; any stall leaves P2.T1's stacks
  - size: S

**Verification gate:** P3.T2 green on both schedulers with no aborts; full debug PR CI green; the image runs on the new Cycles pin.

## Decisions

- 2026-11-10 — Elaborated at claim (consultant): the heap corruption is fixed from code reading in the fork (no sanitizer build; Cycles, USD and TBB aren't instrumented), the cap test is already fixed by #243, and the hang gets a stack-dumping watchdog plus the ranked H1 fix. M371's run showed the same corruption in `EmissionMatchesReferenceRender`, so the cause is hdCycles, not progressive coalescing.
