## Pending

### 2026-10-26T16:00:00Z — change-request
- refs: none
- From M185 - Variable Warps UAT (bosley-native-nodes, user). New milestone suggested ("Cache Invalidation Audit"): the user saw STMap keep rendering as if its old Map were still connected after the Map input was disconnected; not reproducible later. Thoroughly audit cache invalidation for holes: input connect/disconnect (including optional inputs such as Map/Mask), node hash and render-hash composition, the ActionsCache, the image and viewer texture caches, GPU residency and region caches, the timeline cached-frames line, expression and link invalidation, and per-frame metadata retention. Related: M178 - Render Context Core's UAT found the timeline line ignoring invalidation and OFX binary stamps changing per AppImage mount. Deliver a written map of every cache and what invalidates it, stress tests (random edit/connect/disconnect sequences compared against a cache-off render), and fixes.

### 2026-10-25T12:00:00Z — change-request
- refs: none
- From M168 - NVIDIA Viewer Hand-off Validation (bosley-faux). New milestone suggested ("Startup Pane Paint On NVIDIA"): on NVIDIA GL under KWin (RTX 3070 Ti, driver 580.178.04, :0), a newly started Natron window shows its panes unpainted (#202020) on screen for ~16–18 s after map; only the menu bar and left toolbar draw. Happens with NATRON_GPU=off too, never under Xvfb. Screenshot `.local/m168/diag/run1-live.png`. User-visible startup paint issue, not the GPU hand-off.

### 2026-10-25T12:00:01Z — change-request
- refs: none
- From M168 - NVIDIA Viewer Hand-off Validation. Small test fixes for M107 - GPU Viewer Hand-off's UAT: (1) "the viewer shows the comp" in `Tests/gui/m107_viewer_handoff.py` checks only non-black, so an unpainted #202020 viewer passes; use `looks_blank`. (2) The UAT docstring and `Tests/gui/run-gui-test.sh`'s header should say host-display runs (GUI_TEST_HOST_DISPLAY) need a CPU reference taken on the same display. (3) Cosmetic: `tools/gpu-spike/desktop-handoff-check.sh` warns "GL renderer is not radeonsi" on NVIDIA.

### 2026-10-25T15:00:00Z — change-request
- refs: none
- From M169 - Pipelined Strip Member Timing (bosley-faux). Two follow-ups: (1) Flake: `GpuResidencyTest.RegionMembersAreTimedOnTheDevice` failed `EXPECT_GT(blurNs, gradeNs)` in whole mode once in 10 NVIDIA runs (Grade 0.260 ms vs Blur 0.209 ms, first region of the process, no transfer overlap). (2) Spike-robust GPU calibration: 2–16× slow member intervals with no transfer overlap appear in every mode on a shared desktop GPU (22–50% utilisation from other programs) and swing `GpuCostModel`'s EMA; consider outlier rejection or a robust estimator. Data: `.local/m169/p1/` on bosley-faux.

### 2026-10-25T17:00:00Z — change-request
- refs: none
- From M169 - Pipelined Strip Member Timing (bosley-faux), NVIDIA RTX 3070 Ti gate: (1) `CurveSnapshotPrecisionTest.CurveMulKeepsTheProductError` (M171 - GPU Correctness Fixes' test) fails on NVIDIA in both schedulers and both push-descriptor variants: 25 BelowTiny pairs (`8e-31 × [0.5,1]`), e.g. `|hi+lo-exact| 5.06e-39 > 3.07e-44`. Residuals sit just above FLT_MIN but the Dekker partial products are subnormal and appear flushed to zero; the test's `representable` guard checks only the final residual. Passes on lavapipe. (2) `RenderSchedulerTest.CachedOutputsOfTwoFramesAtOnce` fails consistently on bosley-faux (leaf nodes render twice, with or without GPU) but passes on CI; undiagnosed. (3) The bosley-faux host env lacks git-lfs (raw fixtures are pointers) and has stale `build/assets/Plugins` (fetch-assets --check-pins fails); a host-setup note, not code. Logs: `.local/m169/p3/out/`.

### 2026-10-26T23:00:00Z — change-request
- refs: M101
- From gpu (user decision, 2026-10-26): cost seeds are void (`PLAN/DECISIONS/2026-10-26-weight-based-gpu-placement.md`). Strike any unfinished M101 - Clean-Sheet Merge Node task or clause that measures or writes a `*.costseed.inc`, and don't add the README cost-seed checklist step. M239 - Weight-Based GPU Placement deletes the seed machinery; if M101 lands first, its already-committed seed file is removed in M239's rebase.

### 2026-10-27T05:00:00Z — change-request
- refs: M122, M123, M129, M185
- From gpu (user decision, 2026-10-26): cost seeds are abolished (`PLAN/DECISIONS/2026-10-26-weight-based-gpu-placement.md`). M239 - Weight-Based GPU Placement deletes `*.costseed.inc`, `Engine/GpuCostSeeds/`, `render-profile-report.py --emit-cost-table` and the measured cost model. Nodes declare `gpuWeight()` instead (Engine/GpuWeight.h). Override it only when cost grows with a knob (`gpuSizeScaledWeight`) or the work always wants the GPU (`GpuWeight::heavy()`). In M122 - Native Colour Utilities, M123 - Native Sharpen And Glow, M129 - Clean-Sheet Noise Node and M185 - Variable Warps, strike any unfinished task or clause that measures or writes cost seeds, and don't add new `*.costseed.inc` files. Seed files already on a branch are deleted when that branch rebases after M239 merges, or by M239's rebase if your milestone merges first.
