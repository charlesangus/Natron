# M273 - RenderIsolation Exit Hang

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

The RenderIsolation ctest intermittently hits the 300 s timeout after its cases pass (an exit hang), on unrelated branches: M228 - 3D Viewer Navigation's gate (2026-10-27) and M271 - Persistent Message Test Failures' release run 40641290616 (2026-10-28, ctest #151, 300.26 s). Re-running the job passes.

Acceptance sketch:
- Reproduce (loop the test binary with the RenderIsolation filter under the CI image; capture a stack of the hung process with eu-stack per the profiling notes).
- Find the shutdown race (thread pool, render scheduler, GPU context teardown, or a leaked render keeping AppManager alive) and fix it.
- The test passes 200/200 in a loop on CI-equivalent settings.
