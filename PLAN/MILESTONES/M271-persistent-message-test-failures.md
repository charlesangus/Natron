# M271 - Persistent Message Test Failures

Two tests fail 5 times out of 5 on main after M178 - Render Context Core merged, in a local debug build with ctest on RADV (`NATRON_DEV_GPU=1`), both on `hasPersistentMessage()` being unexpectedly true: `PersistentMessageTest.RenderFailingAfterItsChannelCheckPassedKeepsTheError` (`Tests/PersistentMessage_Test.cpp:437`) and `ShuffleRenderTest.ExplicitRowVariesPerFrameOnShuffleDisableChain` (`Tests/ShuffleRender_Test.cpp:361`). Reported by the gpu PM (inbox, 2026-10-27); both passed on a branch based just before M178. The unconfirmed suspect is M178's new `contextCountWarningMessage` source in `Engine/Node.cpp`. Neither test uses a node the gpu PM's branches touch, and it is not known whether PR CI on lavapipe fails them too.

## Scope

**In:**
- Reproducing both failures, finding the cause, and fixing it.
- A regression guard in the failing tests or a new one.

**Out:**
- Redesigning persistent messages or the render-context warning.
- GPU placement changes (M239 - Weight-Based GPU Placement).

**Edge cases:**
- The failures may be real on every host and only mask on lavapipe, or only appear on RADV; the diagnosis checks both.
- If `contextCountWarningMessage` is correct and the tests' premise is wrong (a warning legitimately raises a persistent message), the fix is in the tests, with the reason recorded as a decision.

**Assumptions:**
- A local RADV host with `NATRON_DEV_GPU=1` is available to the claimant; if not, bisect with a lavapipe debug build and say so.

## Phase 271.1: Diagnose

- [ ] M271.P1.T1 — Reproduce and find the cause
  - files: `Tests/PersistentMessage_Test.cpp`, `Tests/ShuffleRender_Test.cpp`, `Engine/Node.cpp` (read only), this milestone file
  - approach: Build main in debug and run both tests 5 times under ctest, with and without `NATRON_DEV_GPU=1`; then check out the commit before M178 and compare. Print which message source and text `hasPersistentMessage()` reports at the failing line. Confirm or refute `contextCountWarningMessage`. Record the cause, and whether lavapipe PR CI also fails, under `## Decisions`.
  - verify: the cause names a source line and the evidence (message text, bisect commit) is recorded.
  - size: M

## Phase 271.2: Fix

- [ ] M271.P2.T1 — Fix the cause and guard it
  - files: the file named by P1.T1 (likely `Engine/Node.cpp`), `Tests/PersistentMessage_Test.cpp`, `Tests/ShuffleRender_Test.cpp`
  - approach: Fix at the source: clear or never raise the message where the premise says it must not exist, rather than loosening the assertions. If the tests' premise is wrong, fix the tests and record why. Keep M178's own tests passing.
  - verify: both tests pass 5 of 5 runs in a debug build with `NATRON_DEV_GPU=1` and without it; `ctest -R "PersistentMessage|ShuffleRender|RenderContext"` passes.
  - size: M

**Verification gate:** `tools/ci/remote/build.sh debug --tests` passes (it reproduces PR CI, per the debug-asserts note), and `PersistentMessageTest.RenderFailingAfterItsChannelCheckPassedKeepsTheError` and `ShuffleRenderTest.ExplicitRowVariesPerFrameOnShuffleDisableChain` pass 5 of 5 on a local RADV debug build.

## Decisions

- 2026-10-28: cause (by code reading; RADV confirmation pending): a node delivered by a GPU region never ran renderRoI's clear of a stale missing-channel error (EffectInstanceRenderRoI.cpp:1823), so a CPU render's error stuck after a later GPU render of the same Shuffle. contextCountWarningMessage ruled out (needs fan-outs). Fix: runGpuRegion retires channel-selector messages on success (retireChannelSelectorMessages); makeGpuTaskPlanes refuses nodes failing Node::checkSelectedChannelsPresent so the CPU posts the error. Regression tests in NativeGpuChain and GpuRegionFallback (GPU-forced, skip without a GPU). Why the tests passed just before M178 is not established.
