# M273 - RenderIsolation Exit Hang

The RenderIsolation ctest hit its 300 s timeout on 2026-10-27/28 (M228 - 3D Viewer Navigation's gate; M271 - Persistent Message Test Failures' run 40641290616, taskgraph job 122667869101). The log shows a stall mid-render, not an exit hang: `EditUpstreamIsRefusedDuringWriteRender`'s second render stops after frame ~11 and never prints `[ OK ]`. M298 - RenderIsolation Taskgraph Hang traced that log to the race M321 - Render Thread Count Race fixed (c30dff64c), and added a render deadline and `RefusedEditRenderStress`. This milestone shows the hang is gone under load on current main, and makes any future timeout leave thread stacks; if it reproduces, the stacks drive the fix.

## Scope

**In:**
- Stack capture on timeout in `test.sh repeat`.
- Deadlines on every RenderIsolation render.
- 200 runs per scheduler remotely.
- A fix only if a stall reproduces.

**Out:**
- The exit path: M322 - NVIDIA Exit Deadlock Fix.

**Assumptions:**
- The CI container allows ptrace for `eu-stack`/`gdb -p` (unverified; T1 falls back to launching under gdb).

## Phase 273.1: Capture and reproduce

- [x] M273.P1.T1 — `test.sh repeat` dumps thread stacks before it kills a timed-out iteration
  - files: `tools/ci/local/test.sh` (on top of M379 - GpuContext Test Segfault's `repeat` mode; wait for it to merge or cherry-pick its `test.sh`/`build.sh`/`remote-build.yml` changes)
  - approach: in `run_iteration`, replace `timeout` with a watchdog: start the binary, wait up to `REPEAT_TIMEOUT - 20` s, run `eu-stack -p <pid>` (fall back to `gdb -p <pid> -batch -ex 'thread apply all bt'`) into `iter-N.stacks`, then kill it. If ptrace is refused, fall back to the `--gdb` launch (`timeout -s INT` to gdb, then `thread apply all bt`). Print the stacks path in the summary.
  - verify: `repeat` on a throwaway test that sleeps past a short `REPEAT_TIMEOUT` leaves a stacks file showing the sleeping frame, locally in natron-dev and on one remote run.
  - size: M
- [x] M273.P1.T2 — Every RenderIsolation render has a deadline
  - files: `Tests/RenderIsolation_Test.cpp`
  - approach: the deadline on `renderWithActionAtFrame` (~154-263) defaults to the value `RefusedEditRenderStress` uses, not zero, so the original cases (~323, ~339) get it too. Before `std::abort()`, print which test and frame stalled.
  - verify: syntax-check; the PM's build runs RenderIsolation green on both ctest legs.
  - size: M
- [x] M273.P1.T3 — 200 runs per scheduler on current main (PM run)
  - files: none
  - approach: `build.sh release --repeat-test RenderIsolation:200` with T1 and T2; split the run if it doesn't fit the 90-minute repeat step.
  - verify: both `ctest-<scheduler>-repeat.log` files show 200/200, or a captured `iter-N.stacks`.
  - size: S

## Phase 273.2: Fix (only if T3 reproduces)

- [ ] ~~M273.P2.T1 — Fix the stall the stacks show~~ (dropped: 200/200 on both legs, no stall)
  - files: from the stacks (likely `Engine/OutputSchedulerThread.cpp`, `Engine/RenderScheduler.cpp`), `Tests/RenderIsolation_Test.cpp`
  - approach: read the blocked threads (scheduler, frame threads, GIL holder, the refused-edit thread), fix the lock order or lost wakeup, and add a `RefusedEditRenderStress` case that reproduces the interleaving.
  - verify: T3's command passes 200/200 on both legs.
  - size: M

**Verification gate:** a remote release build with `--repeat-test RenderIsolation:200` passes 200/200 on both ctest legs, and the normal ctest legs stay green.

## Decisions

- 2026-11-10 — Elaborated at claim (consultant): the stub's "exit hang" was a mid-render stall already diagnosed by M298 and fixed by M321, so this milestone confirms the fix and adds stack capture rather than hunting a new race. M322 touches only the exit path and is unrelated. The board name stays as it is; renaming is plan-scoped.
- 2026-11-10 — P1.T3: `RenderIsolation` passed 200 of 200 under load on both legs (release run 42850235106), and 100 of 100 earlier (42832794252). P2 is dropped. The run's legacy ctest leg failed only on `SceneViewportStorm` timing out at 300 s, which is M378 - Scene Render Test Stability's hang; this branch has neither its Cycles locks nor its watchdog.
