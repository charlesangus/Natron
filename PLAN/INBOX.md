## Pending

### 2026-10-25T16:00:00Z — change-request
- refs: M70.P5.T2, M70.P5.T3
- From the user, via /cat-discuss, for M70 - RFV Declaration And Storage (see `DECISIONS/2026-10-25-rfv-promote-undo-and-active-copies.md`). (1) **Promote must be undoable.** "Add to project variables" becomes one undo step through M114's undo history: undo removes the project's copy, and redo restores it with the same value and animation. Amend this milestone's 2026-10-25 "Project variables and M114 undo" decision to make promote an exception to it; `app.createRFV*Param` is still not recorded. Add to T2's verify: a gtest runs promote, undo and redo. Add to T3's verify: an Xvfb check that Ctrl+Z after the right-click removes the variable from the project Variables section. (2) **Both copies stay active.** After a promote, the Set node's variable must not be disabled, read-only or greyed out, and both copies stay editable. The node's copy keeps overriding the project's upstream of that Set node. Only the menu item disables, with its tooltip. Add to T3's Xvfb verify a check that the node's knob is still enabled and editable after the promote, and include it in the screenshot. Code to change is already on milestone/m70 (03e610296 and the commits after it).

### 2026-10-25T12:00:00Z — info
- refs: Engine/Knob.cpp setExpressionInternal; found during M92
`setExpressionInternal` rethrows a caught `const std::exception&` with `throw e;`, slicing a `std::runtime_error` to a plain `std::exception` and dropping its type. Use `throw;`.

### 2026-10-25T06:30:00Z — info
- refs: M167
A Switch's per-file key/value metadata (custom header attributes) always comes from its input 0, not the input it routes to at that frame, so a Write over a keyed Switch writes the first source's custom attributes into every file. Found while writing M167 P5.T4 (NativeWriteMetadata EachFileOfASequenceTakesItsOwnFramesFormat checks only size and PAR). Fix belongs in Switch / NativeEffectBase::getOutputMetadata routing; outside M167's files.

### 2026-10-25T07:40 — info
- refs: none
- **Nightly on main has failed since `9ab236449`** (runs 40163566294, 40167954015 and 40170317744). The `release-build-and-test` job fails `GuiThreadGLBinding` (from M161 - Startup Compositor Crash) and `TextRenderer` (from M172 - GL Widget Context Loss) in the release build; PR CI's debug build passes them. Found while the gpu PM dispatched Nightly on M171's branch (run 40170808335, which shows the same two failures). Needs an owner, likely bosley-native-nodes, which shipped both. M171 doesn't touch these tests.

### 2026-10-25T12:00:00Z — change-request
- refs: none
- From M168 - NVIDIA Viewer Hand-off Validation (bosley-faux). New milestone suggested ("Startup Pane Paint On NVIDIA"): on NVIDIA GL under KWin (RTX 3070 Ti, driver 580.178.04, :0), a newly started Natron window shows its panes unpainted (#202020) on screen for ~16–18 s after map; only the menu bar and left toolbar draw. Happens with NATRON_GPU=off too, never under Xvfb. Screenshot `.local/m168/diag/run1-live.png`. User-visible startup paint issue, not the GPU hand-off.

### 2026-10-25T12:00:01Z — change-request
- refs: none
- From M168 - NVIDIA Viewer Hand-off Validation. Small test fixes for M107 - GPU Viewer Hand-off's UAT: (1) "the viewer shows the comp" in `Tests/gui/m107_viewer_handoff.py` checks only non-black, so an unpainted #202020 viewer passes; use `looks_blank`. (2) The UAT docstring and `Tests/gui/run-gui-test.sh`'s header should say host-display runs (GUI_TEST_HOST_DISPLAY) need a CPU reference taken on the same display. (3) Cosmetic: `tools/gpu-spike/desktop-handoff-check.sh` warns "GL renderer is not radeonsi" on NVIDIA.
