## Pending

### 2026-10-25T18:00:00Z — change-request
- refs: M92
- From the user's M92 - Linked Nodes UAT. Three changes, each reversing or refining shipped behaviour (the owning PM should add tasks and record decisions):
  1. **Copy, paste and duplicate make an identical copy, links included.** Reverses M92.P2.T1 and the gate item "Right-click Duplicate and Paste produce independent copies". Keyboard and right-click Duplicate, Copy/Paste and clipboard paste all keep the copy's knob links and Linked Node membership; the copy joins the original's link groups (it is not independent, and the copy is drawn as linked). Supersedes the 2026-10-25 decision that pasted copies link only to each other. Alias (permanent) groups still follow whatever the Group copy already does. Update the gate wording and undo/redo coverage.
  2. **Each link line points to only the nearest other node.** Today every member draws to every other member, making a web. Per link group (and per expression dependency set), draw one line to the single nearest other member node, by node-centre distance. A fast approximation is fine; exactness is not required. Applies to both link and expression lines, and to the dashed Linked Node lines.
  3. **Swap the dash styles.** Whole-node links (Linked Nodes) now draw **solid**; single-knob links (Paste Link, drag-to-link, alias) draw **dashed**. Today it is the reverse. Update the Xvfb screenshot expectations in M92.P2.T4 and the gate.

### 2026-10-25T12:00:00Z — info
- refs: Engine/Knob.cpp setExpressionInternal; found during M92
`setExpressionInternal` rethrows a caught `const std::exception&` with `throw e;`, slicing a `std::runtime_error` to a plain `std::exception` and dropping its type. Use `throw;`.

### 2026-10-25T06:30:00Z — info
- refs: M167
A Switch's per-file key/value metadata (custom header attributes) always comes from its input 0, not the input it routes to at that frame, so a Write over a keyed Switch writes the first source's custom attributes into every file. Found while writing M167 P5.T4 (NativeWriteMetadata EachFileOfASequenceTakesItsOwnFramesFormat checks only size and PAR). Fix belongs in Switch / NativeEffectBase::getOutputMetadata routing; outside M167's files.

### 2026-10-25T12:00:00Z — change-request
- refs: none
- From M168 - NVIDIA Viewer Hand-off Validation (bosley-faux). New milestone suggested ("Startup Pane Paint On NVIDIA"): on NVIDIA GL under KWin (RTX 3070 Ti, driver 580.178.04, :0), a newly started Natron window shows its panes unpainted (#202020) on screen for ~16–18 s after map; only the menu bar and left toolbar draw. Happens with NATRON_GPU=off too, never under Xvfb. Screenshot `.local/m168/diag/run1-live.png`. User-visible startup paint issue, not the GPU hand-off.

### 2026-10-25T12:00:01Z — change-request
- refs: none
- From M168 - NVIDIA Viewer Hand-off Validation. Small test fixes for M107 - GPU Viewer Hand-off's UAT: (1) "the viewer shows the comp" in `Tests/gui/m107_viewer_handoff.py` checks only non-black, so an unpainted #202020 viewer passes; use `looks_blank`. (2) The UAT docstring and `Tests/gui/run-gui-test.sh`'s header should say host-display runs (GUI_TEST_HOST_DISPLAY) need a CPU reference taken on the same display. (3) Cosmetic: `tools/gpu-spike/desktop-handoff-check.sh` warns "GL renderer is not radeonsi" on NVIDIA.

### 2026-10-25T15:00:00Z — change-request
- refs: none
- From M169 - Pipelined Strip Member Timing (bosley-faux). Two follow-ups: (1) Flake: `GpuResidencyTest.RegionMembersAreTimedOnTheDevice` failed `EXPECT_GT(blurNs, gradeNs)` in whole mode once in 10 NVIDIA runs (Grade 0.260 ms vs Blur 0.209 ms, first region of the process, no transfer overlap). (2) Spike-robust GPU calibration: 2–16× slow member intervals with no transfer overlap appear in every mode on a shared desktop GPU (22–50% utilisation from other programs) and swing `GpuCostModel`'s EMA; consider outlier rejection or a robust estimator. Data: `.local/m169/p1/` on bosley-faux.
