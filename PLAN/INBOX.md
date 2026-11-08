## Pending

### 2026-11-08T21:00:00Z — change-request
- refs: M304
- New milestone (user, 2026-11-08): "Roto Point List Render Isolation". M304 - Roto Points Snapping To Origin keeps a GUI side for point positions, keys, interpolation and track links while a render holds a Bezier's internal curves, but the point list itself is still shared: deleting points (or otherwise changing which points are in the list, around `Engine/Bezier.cpp` ~1745 point removal) during a render changes the list the render iterates. Also still shared: each point's `broken` flag, and `Bezier::clone` picks its side without the render-start hold because it can run off the main thread. Design a GUI-side point list (or copy-on-write list snapshot) that renders never see change mid-render, written back with the other GUI edits; keep render starts responsive (no main-thread waits on workers). Tests: a concurrent reader test like M304's `RenderThreadReadsNoEditMadeDuringItsRender`, extended to deletions and structural changes; the m224/m261/m304 GUI scripts.

### 2026-11-08T21:30:00Z — change-request
- refs: Tests/Scene/USDRenderCycles_Test.cpp:571
USDRenderCyclesTest.AbortStopsTheRenderPromptly is flaky: its thread-count check (second run ≤ first run) failed 19 vs 18 on M360's release gate (run 42513356809), on a branch that doesn't touch USD or Cycles. Process thread count picks up unrelated pool threads; the check should count Cycles/Hydra-owned threads, or allow a settle/retry, rather than all process threads.
