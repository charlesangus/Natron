# M598 - Progressive Render GIL Flake

`ProgressiveRenderTest.RegisteringAParticipantDuringAWriteReleasesTheGilWhileWaiting` (`Tests/Scene/ProgressiveRender_Test.cpp:1805`, from M504 - GIL Lock Order Fix, #341) gave up at its 10 s deadline with "the threads deadlocked waiting for the registrant to wait for the writer lock". That was on the legacy leg of an M256 gate (run 45529586619); the task-graph leg of the same commit passed. The `SceneWriteBarrier` test hook spins until `/proc/self/task/<tid>/stat` shows the registrant asleep. On a loaded ctest runner the registrant may sleep for some other reason first, or not be seen asleep before the deadline. If the registrant has already finished, its tid is gone and the loop never sees it sleep. Make the wait robust without weakening what the test proves: a registrant that would block holding the GIL still deadlocks and fails.

## Scope

**In:**
- The test's wait condition, and a test-only hook in `SceneWriteBarrier` if one is needed.

**Out:**
- Changes to the lock-order fix itself, unless the diagnosis shows a real deadlock. In that case, stop and report rather than fix it here.
- Raising the deadline as the fix.

**Edge cases:**
- **Registrant done before the hook runs:** a vanished tid, or a recorded "registrant finished" flag, counts as success for the wait, not as a hang.

## Phase 598.1: Fix the wait

- [ ] M598.P1.T1 — Replace the /proc sleep poll with an explicit "registrant is waiting" signal
  - files: `Tests/Scene/ProgressiveRender_Test.cpp` (:1805–~1900, `threadIsSleeping` and `hold.run`), `Engine/Nodes/Scene/SceneWriteBarrier.{h,cpp}` (`setTestHook` :581, plus any new test hook)
  - approach: have `SceneWriteBarrier` call a test hook from `registerParticipant` right before it blocks on the writer lock, after releasing the GIL. The writer's hook then waits on a condition variable for that call instead of polling `/proc`. If the registrant finishes first, its `finish()` also satisfies the wait. A registrant that blocks while still holding the GIL never fires the new hook, so the writer still can't proceed and the test still fails by deadline. Keep the 10 s deadline. Use `/proc` state only as extra diagnostics in the failure message.
  - verify: `ctest -R ProgressiveRender` green; `build.sh debug --tests --repeat-test RegisteringAParticipantDuringAWriteReleasesTheGilWhileWaiting:200` passes under background load; temporarily reverting M504's GIL release in `registerParticipant` locally makes the test fail (then restore it).
  - size: M

**Verification gate:** full build and ctest green; the 200-repeat run under load passes; the revert check fails as expected, and that is noted in the PR.
- 2026-11-27 (PM) — Revert check done (run 46099786942, branch check/m598-revert, since deleted): with `PythonUnlockedWhileWaiting unlocked;` removed from `registerParticipant`, the test fails with "the GIL and the writer lock deadlocked waiting for the reader, writer and registrant to end (registrant /proc state: S)", as expected.
- 2026-11-28 — Merged to main (PR #410, squash).
