## Pending


### 2026-11-25T04:17:08Z — change-request
- refs: M247
- New milestone after M247 - Project User Knobs And Root (E10, F16 - User Knob Editor or standalone): **Remove The Old User Knob Dialogs** — the former M373.P3.T3. Delete Gui/ManageUserParamsDialog.{h,cpp}, Gui/AddKnobDialog.{h,cpp}, Gui/PickKnobDialog.{h,cpp} and every reference (includes, actions, Gui/CMakeLists.txt, DockablePanel.cpp, ProjectGui::editVariables). Anything still used — validation, the path-variable rename, PickKnobDialog's node listing — must already live in M372's API, the in-panel editor (M373) or M247's project editor; move leftovers rather than keeping a dialog. Verify: build; `git grep -n "ManageUserParamsDialog\|AddKnobDialog\|PickKnobDialog"` finds nothing; all m373_*.py GUI scripts pass. Blocked on M247 because the dialog still edits project path variables until M247 gives the project an in-panel editor.

### 2026-11-25T04:23:46Z — change-request
- refs: none
- New small fix milestone (E10): **Curve Editor Expanded Knob Rows** — Tests/gui/m69_cleanup.py section [expanded-curve] fails on main (checked on a 2026-11-20 main AppImage and on M521): after expanding Blur size and bringing the Curve Editor tab to the front, the size row is found with 8 component rows (expected 2) and the curve widget is not found visible. Suspects since the script last passed (3b71764db, 11-12): 5031f097c (M421 - Stroke Keyframe Indicators, tree blockSignals around context removal in Gui/CurveEditor.cpp) and 125e085eb (M399 - Node Creation Keys And Speed, KnobGuiContainerHelper rewrite). Decide whether the curve editor duplicates component rows (a product bug) or the check reads the tree wrongly, fix it, and add m69_cleanup to a gate.

### 2026-11-25T04:24:00Z — change-request
- refs: M247
- From M373 - In-Panel Knob Editor review (#376, finding 8): when M247 - Project User Knobs And Root enables the in-panel editor on Project Settings, fold RenamePathVariableCommand into the applySpec undo entry (Gui/KnobEditProperties.cpp ~793) so a path-variable rename is one Ctrl+Z, and add a GUI check that renames a path variable through the editor.

### 2026-11-25T13:43:28Z — change-request
- refs: none
- Flaky test from M504 - GIL Lock Order Fix (#341): ProgressiveRenderTest.RegisteringAParticipantDuringAWriteReleasesTheGilWhileWaiting (Tests/Scene/ProgressiveRender_Test.cpp:1836) gave up after its 10 s deadline ("the threads deadlocked waiting for the registrant to wait for the writer lock") on the legacy leg of M256 gate2 (run 45529586619), passing on the task-graph leg of the same commit. It polls /proc until the registrant thread is asleep, which can miss the deadline on a loaded ctest runner, or never see the sleep if the registrant finishes and its tid vanishes. File as an E14 flake fix (or fold into M496 - MoonRay Test Stability's flake list): make the wait condition robust without loosening what it proves.

### 2026-11-25T14:14:54Z — change-request
- refs: none
- New milestone (E14 or the CI epic), user-approved 2026-11-25: **Runner Efficiency** — cut GitHub runner slot use (20-concurrent-job cap is the bottleneck; analysis in the PM session 2026-11-25). Tasks: (1) ci.yml runs only for non-draft PRs (types: opened, synchronize, reopened, ready_for_review; job if: !github.event.pull_request.draft); lane PRs open as drafts and the PM marks them ready after a green gate. (2) Shard remote-build.yml's per-script GUI matrix into at most 4 jobs per gate (gui-matrix emits groups; run-gui-test.sh loops with a per-script timeout), so F15's 21 scripts take 4 slots, not 21. (3) build.sh --ctest-regex <re> (and the workflow input) to run only matching ctest cases on intermediate gates; the final gate runs the full suite. (4) Stop duplicate dev-image builds of identical inputs (concurrency group keyed on the image inputs, or dispatch dev-image.yml once after a Dockerfile change); after a main merge restore main's ccache entry first; find why remote ccache hits are near zero on fast builds (19/1313, 0/23) while debug hits 55/60. (5) With M199 - Remove Legacy Scheduler: drop the legacy ctest leg from remote-build.yml and ci.yml. Verify each with run timings before/after.
