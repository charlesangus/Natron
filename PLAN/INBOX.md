## Pending

### 2026-10-26T19:30:00Z — info
- refs: M19, M20
- From the user (2026-10-26): merge M19 - USD/Hydra Foundation and M20 - 3D Node Vocabulary as they are, plus the two UAT fixes in the 2026-10-26T19:00:00Z change-request for M20 - 3D Node Vocabulary. None of the follow-up MVP work goes in: that is M226–M235, now on the board. Remaining gate items: tick M19.P3.T6 (its numbers are already recorded in M19's file); rebuild the AppImage with the two fixes for the user's re-check; and run the AMD/NVIDIA hardware checks with screenshots, unless the user confirms their UAT covered them. Then merge M19 first, without `--delete-branch` (that would close M20's stacked PR). Retarget M20's PR to main via REST and merge it.

### 2026-10-26T19:00:00Z — change-request
- refs: M20
- From the M19/M20 UAT grill (user chose to fix these before M20 - 3D Node Vocabulary's gate). Add two fix tasks to M20 - 3D Node Vocabulary. (1) **File pickers show no files with the `*` filter.** This is an M16 regression that hits every file knob not marked as an input image: USDRead's File, USDMaterial's texture knobs, USDWrite's save dialog. `Gui/KnobGuiFile.cpp:157-161` passes `{"*"}`. `SequenceFileDialog::setDefaultFilter` (`Gui/SequenceFileDialog.cpp:1942-1949`) turns that into a bare `*`. `FileSystemModel::setRegexpFilters` (`Engine/FileSystemModel.cpp:1043`) skips the `*` token, so `isAcceptedByRegexps` (`:1079-1092`) rejects every file. Fix: an empty regexp list with `*` means accept all. Also give USDRead's knob the USD/Alembic extensions and the texture knobs image extensions. Verify with a gtest on FileSystemModel and an Xvfb check that the USDRead dialog lists `.usda` files. (2) **Right-click leaves a stuck dolly.** `SceneViewport::mousePressEvent` (`Gui/SceneViewport.cpp:1517-1533`) stores `_imp->button` for every press. The context menu then grabs the release, so `mouseReleaseEvent` never resets it, and later hover moves dolly (`:1564-1565`). Fix: set the button only on accepted presses, clear it in `contextMenuEvent`, and check `e->buttons()` in move. Verify with an Xvfb script: right-click, Esc, then mouse moves leave the camera unchanged. (Paths from the m20 worktree.)

### 2026-10-26T17:00:00Z — change-request
- refs: none
- From bosley-native-nodes (PR CI). `Comps/CacheEquivalenceBenchCompTest` runs 372–434 s inside the debug ctest legs against a 450 s timeout (M177's PR CI: 386 s and 434 s; M92's legacy leg timed out at 450 s, then passed alone in 190 s). It flakes under load on every branch. Shrink the comp or its frame count so it runs well under the timeout, keeping what it proves.

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
