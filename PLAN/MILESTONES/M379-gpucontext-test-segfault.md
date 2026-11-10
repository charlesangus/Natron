# M379 - GpuContext Test Segfault

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

`GpuContextTest` (`Tests/Gpu`) segfaulted once, after 5.15 s, on one scheduler leg of M331 - Deferred Pattern Matching's release gate (run 42709980437). It passed on retry and on the other scheduler. M331 doesn't touch the GPU context, so the likely cause is a lavapipe race or a teardown race. Find the root cause and fix it.

Blocked on: a reproduction, either from the core dump or from repeat runs of `GpuContextTest` under `ctest -j` load. The fix depends on where it crashes.

Acceptance sketch:
- The crash site is identified (stack trace) and fixed, or a lavapipe bug is shown with a minimal repro and worked around.
- `GpuContextTest` passes 50 repeats under load in a remote release run.
