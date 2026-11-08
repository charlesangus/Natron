## Pending

### 2026-11-08T21:30:00Z — change-request
- refs: Tests/Scene/USDRenderCycles_Test.cpp:571
USDRenderCyclesTest.AbortStopsTheRenderPromptly is flaky: its thread-count check (second run ≤ first run) failed 19 vs 18 on M360's release gate (run 42513356809), on a branch that doesn't touch USD or Cycles. Process thread count picks up unrelated pool threads; the check should count Cycles/Hydra-owned threads, or allow a settle/retry, rather than all process threads.
