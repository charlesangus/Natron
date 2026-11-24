# M496 - MoonRay Test Stability

MoonRay suites fail intermittently under parallel ctest on runners and pass when rerun serially. Seen so far:
- `ProgressiveRenderTest` stalls in MoonRay cases (`AMoonRayEditRestartsInPlace`, `AnEngineChangeCutsTheTextureExportThePassWaitsOn`; watchdog abort) — M336 - Heavy Scene Acceptance's PR CI run 44940733888 and its first re-run, legacy and task-graph legs; once on MoonRay's headlight case.
- `ProgressiveRenderTest` segfault inside `UsdImagingGLEngine` construction (task-graph leg, same run).
- `USDRenderMoonRayLiveTextureTest` hitting the 300 s suite timeout under load (same run), and separately hanging after MoonRay's dispatcher exit once all 6 cases had passed in 8 s (M288 - Utility Renderer Projection Modes' gate, run 44195775643, legacy).
- `USDRenderMoonRayTest` segfault: hdMoonray's arras client thread reads a freed socket buffer while the previous render context tears down asynchronously (traced 2026-11-14, an upstream race; candidate fix: wait for the old context in teardown).

Fix the product races in our code or the MoonRay fork (M325 - MoonRay Fork: charlesangus forks of `hdMoonray`, `arras4_core`, …, pinned by `tools/ci/local/build-moonray.sh`), get stacks for the stalls first, and only then decide whether any suite also needs to run apart from others.

## Scope

**In:**
- The arras teardown race, the exit hang, the stalls and the `UsdImagingGLEngine` construction segfault, each fixed at its cause once stacks show it.
- Watchdog coverage (`Tests/TestWatchdog.h`, from M378 - Scene Render Test Stability) for the MoonRay suites that lack it, so the next stall leaves stacks.
- `RESOURCE_LOCK`/`RUN_SERIAL` in `Tests/SuiteProperties.cmake` only for a suite whose remaining failure the stacks show is CPU starvation, not a bug.

**Out:**
- KeyMix's GPU mismatch from the same run (M495 - KeyMix GPU Edge Read Fix).
- Cycles-delegate flakes (fixed in M378 - Scene Render Test Stability).
- Raising timeouts to hide a hang.

**Edge cases:**
- A stall that doesn't reproduce in 20 repeats: record it, keep the watchdog, drop its fix task (as M378 did).

## Phase 496.1: Evidence

- [ ] M496.P1.T1 — Watchdog and phase reporting for the MoonRay suites
  - files: `Tests/Scene/USDRenderMoonRay_Test.cpp` (`USDRenderMoonRayTest`, `USDRenderMoonRayLiveTextureTest` fixtures), `Tests/TestWatchdog.h` (reuse, no change expected)
  - approach: start the M378 watchdog in each fixture's `SetUp` (150 s, as `ProgressiveRender_Test.cpp` does) and report phases around render, abort and teardown; extend the watchdog to cover fixture `TearDown` and static/process teardown if it doesn't already (arm it again in `TearDown`, and from an `atexit` or test-environment `TearDown` so an exit hang dumps stacks instead of running into ctest's timeout).
  - verify: the suites pass at the phase build; the watchdog death test still passes
  - size: M
- [ ] M496.P1.T2 — Repeat the MoonRay suites under load to capture stacks (PM runs)
  - files: none
  - approach: `tools/ci/remote/build.sh debug --tests --repeat-test <suite>:20` for `ProgressiveRenderTest`, `USDRenderMoonRayTest` and `USDRenderMoonRayLiveTextureTest`, both schedulers; collect watchdog stacks and any core/segfault backtrace from the logs. Record which failures reproduced, with stacks, as a milestone decision.
  - verify: logs collected and summarised in `## Decisions`
  - size: S

## Phase 496.2: Fixes

- [ ] M496.P2.T1 — Wait for the previous MoonRay render context before tearing down its arras client
  - files: MoonRay fork (`hdMoonray` render delegate/render pass teardown, `arras4_core` `Client.cc` as needed), `tools/ci/local/build-moonray.sh` pin
  - approach: consultant-confirmed design first (opus): find where hdMoonray starts the new context while the old one tears down asynchronously, and make teardown join the old context's arras client thread (or keep its socket buffers alive until that thread exits) before freeing them. Commit on the fork's `natron/...` branch, then move the pin and checksum in `build-moonray.sh`.
  - verify: the fork builds in the dev image job; `ctest -R USDRenderMoonRay` passes
  - size: L
- [ ] M496.P2.T2 — Fix the hang after MoonRay's dispatcher exit
  - files: per P1.T2's stacks — likely `Engine/Nodes/Scene/MoonRayRuntime.cpp` or the fork's dispatcher/arras shutdown
  - approach: from the exit-hang stacks, find the thread that never joins (arras client, dispatcher, TBB arena) and make shutdown end it. If P1.T2 never reproduces the exit hang, record that and drop this task.
  - verify: `USDRenderMoonRayLiveTextureTest` exits promptly 20/20 in P3.T1
  - size: M
- [ ] M496.P2.T3 — Fix the ProgressiveRenderTest MoonRay stalls
  - files: per P1.T2's stacks — likely `Engine/Nodes/Scene/HydraRenderImpl.cpp`, `Engine/Nodes/Scene/MoonRayRuntime.cpp`, or the fork's `hdMoonray`
  - approach: from the stall stacks, fix the wait that never returns (a restart or texture-export wait missing a cancel or wake-up). If the stacks show only starvation (all threads progressing, slowly), put the MoonRay suites under one `RESOURCE_LOCK` in `Tests/SuiteProperties.cmake` with a comment saying why. If no stall reproduces, record that and drop this task.
  - verify: `ProgressiveRenderTest` 20/20 in P3.T1
  - size: M
- [ ] M496.P2.T4 — Fix the segfault in UsdImagingGLEngine construction
  - files: per P1.T2's backtrace — the test fixture in `Tests/Scene/ProgressiveRender_Test.cpp` or the engine setup in `Engine/Nodes/Scene/`
  - approach: from the backtrace, find what construction races with (a previous engine's teardown, plugin registry or GL context on another thread) and serialise or order it. If it never reproduces, record that and drop this task.
  - verify: `ProgressiveRenderTest` 20/20 in P3.T1
  - size: M

## Phase 496.3: Gate (PM runs)

- [ ] M496.P3.T1 — Repeat under load
  - files: none
  - approach: as P1.T2, 20 each for `ProgressiveRenderTest`, `USDRenderMoonRayTest`, `USDRenderMoonRayLiveTextureTest`, both schedulers
  - verify: 20/20 per suite per scheduler, no watchdog abort, no segfault
  - size: S

**Verification gate:** P3.T1 green on both schedulers; the dev image builds on the new MoonRay pin; full debug PR CI green.
- 2026-11-24 — Two more ProgressiveRenderTest segfaults: PR #346 (M510) task-graph leg run 45154296436, and m554-gate4 legacy leg; both passed on the in-job rerun.
- 2026-11-24 — USDRenderStormTest failed once more on m559-gate7 (task-graph leg), passing on rerun; m106-gate5 and m559-gate5 saw the same.
