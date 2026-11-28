# M639 - Ctest Load Flakes

Several tests flake under parallel ctest load and now cost a ~90 min rerun on most PRs. Seen together:
- On M602 - 3D Viewer Toolbars' gate10 (run 46043776540, fast preset): `USDRenderCyclesMaterialTest` (`Tests/Scene/USDRenderCyclesMaterial_Test.cpp`) segfaulted on the first attempt, and `USDRenderMoonRaySubdivisionTest` (`Tests/Scene/USDRenderMoonRay_Test.cpp`) timed out on the rerun.
- On the earlier F36/M609 runs, `ProgressiveRenderTest` (`Tests/Scene/ProgressiveRender_Test.cpp`, fixture :381) segfaulted the same way.
- On PR #404 CI (run 46044117803), in one run: a NatronUtilitySamplingDelegate segfault (`Tests/Scene/NatronUtilitySampling_Test.cpp:751-817`), a ProgressiveRenderTest failure, and a `USDRenderMoonRayLiveTextureTest` timeout. All passed on retry.
- On m199-gate14, `CyclesDelegate.DrawFilterHidesPrimsAndLights` (`Tests/Scene/CyclesDelegate_Test.cpp:140`) once saw `/World/a` still visible after the draw filter hid it.
- `SplitOnGapsTest` (`Tests/FileSystemModel_Test.cpp:349`) timed out at 300 s on f18-gate1 and stalled in m496-rep8's ctest leg, in phase 'start' (`Tests/TestWatchdog.h:101`: no `setPhase` yet). The stall is before the test body.

None of these suites carry a `RESOURCE_LOCK` or `LABELS` (`Tests/CMakeLists.txt:487-492`, `Tests/SuiteProperties.cmake`). Reproduce under parallel ctest, find the shared causes (delegate teardown, GPU or engine reuse across tests, process-start waits, timeouts sized for an idle runner) and make the tests deterministic.

## Scope

**In:**
- The tests listed above, and the shared causes behind them.

**Out:**
- `ProgressiveRenderTest.RegisteringAParticipantDuringAWriteReleasesTheGilWhileWaiting`'s wait, owned by M598 - Progressive Render GIL Flake.
- Raising timeouts as the fix. A timeout may change only when the diagnosis shows the work really takes that long on a loaded runner.

**Edge cases:**
- **A real bug behind a flake (use-after-free in teardown):** fix it in the product, with a test that fails without the fix under load.
- **Cause is the Engine TLS race in `makeInheritedTLSData`** (open question on the board): stop and report. That fix is its own milestone.

## Phase 639.1: Reproduce

- [x] M639.P1.T1 — Reproduce the flakes under parallel load and capture stacks
  - files: read-only: the test files above, `Tests/TestWatchdog.h`, `Tests/SuiteProperties.cmake`; notes and logs to `build/m639/repro.md`
  - approach: run `ctest -j8 --repeat until-fail:20 --schedule-random` over the named suites (fast preset) with background CPU load, and run the debug build with ASan where it builds. Collect core-dump stacks for segfaults and watchdog stacks for timeouts. For SplitOnGapsTest, read rep8's watchdog stacks to find what process start waits on. Group the failures by cause.
  - verify: repro.md gives each test's failure rate, a stack per failure mode, and a named cause per group
  - size: L

## Phase 639.2: Fix

- [ ] M639.P2.T1 — Fix the delegate render tests' shared cause
  - files: per T1: `Tests/Scene/USDRender*_Test.cpp`, `NatronUtilitySampling_Test.cpp`, `ProgressiveRender_Test.cpp`, the shared render-test fixture, and the product teardown code T1 names (`Engine/Nodes/Scene/HydraRenderImpl.cpp` or a delegate)
  - approach: per T1's diagnosis: fix a teardown use-after-free in the product, give each test its own engine instead of a reused one, or add a ctest `RESOURCE_LOCK` for a GPU or engine resource that can't be shared. Prefer a product fix over test isolation when the product is wrong.
  - verify: the T1 repeat command passes 20 of 20 for these suites under load
  - size: L
- [ ] M639.P2.T2 — Fix CyclesDelegate.DrawFilterHidesPrimsAndLights' visibility race
  - files: `Tests/Scene/CyclesDelegate_Test.cpp` (:140), and the draw filter path per T1
  - approach: per T1: if the check reads before the filter's edit reaches the render, wait on the render that follows the edit rather than on time. If the filter edit is lost, fix the product.
  - verify: `ctest -R CyclesDelegate --repeat until-fail:50 -j8` passes under load
  - size: M
- [ ] M639.P2.T3 — Fix the SplitOnGapsTest startup stall
  - files: `Tests/FileSystemModel_Test.cpp` (:349), `Tests/TestWatchdog.h`, and whatever T1 shows process start waiting on
  - approach: per T1's stacks: remove the start-up wait (a shared lock file, a Qt platform plugin, a font cache scan). If several startup-phase stalls share it, fix it once in the shared test main.
  - verify: `ctest -R SplitOnGaps --repeat until-fail:50 -j8` under load passes with no stall
  - size: M

**Verification gate:** full build and ctest green; the T1 load-repeat run over every named suite passes 20 of 20; two consecutive PR CI runs of this branch pass with no retries.
- 2026-11-27 (PM) — P1.T1 diagnosis (repro run 46099492660, 6 runs × 15 suites; report in the lane's build/m639/repro.md):
  - **SplitOnGapsTest (3/6):** product deadlock in `Engine/FileSystemModel.cpp`, inside the test body (phase 'start' only means the suite never calls setPhase). The gatherer thread holds the model's last reference, so `~FileSystemModel` runs on it and `quitGatherer()` waits on itself; the next test's inotify event then blocks the main thread on the stuck gatherer. **P2.T3 rescoped:** no strong model reference in the gatherer, quit/abort safe from the gatherer's own thread, disconnect the watcher first; with a test that drops the last model reference from the gatherer.
  - **USDRenderCyclesMaterialTest (1/6):** product teardown-order null deref at process exit — a scene-worker job (`readGivenSettingsSummaries` → `getHashValue`) runs after its app is destroyed; `Node.cpp:1565` calls `getProject()` on a null app. `~AppManager` quits apps before waiting for scene-worker jobs. P2.T1: drain a project's scene jobs before its app is released, guard the null app, test with a job held at a barrier. May also explain the earlier ProgressiveRender / NatronUtilitySampling segfaults (re-check after the fix).
  - **USDRenderMoonRayProjectionTest (2/6):** hangs in `ClipNearFarPutsTheCardOutside`; the client sits in `recv` while `execComp` is alive and silent; the pre-first-frame stall guard waits 300 s (= the ctest timeout), so it hangs instead of failing. Root cause unknown (lost start message, concurrent MoonRay suites sharing /dev/shm or arras sockets, or an execComp deadlock); stacks need `--cap-add=SYS_PTRACE` on the CI test container. Follow-up repeat run once ptrace is enabled.
  - No stack involves `makeInheritedTLSData`. CyclesDelegate / NatronUtilitySamplingDelegate / ProgressiveRender did not fail in this run (inconclusive; one repeat run each after the teardown fix).
