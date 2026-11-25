## Pending


### 2026-11-25T04:24:00Z — change-request
- refs: M247
- From M373 - In-Panel Knob Editor review (#376, finding 8): when M247 - Project User Knobs And Root enables the in-panel editor on Project Settings, fold RenamePathVariableCommand into the applySpec undo entry (Gui/KnobEditProperties.cpp ~793) so a path-variable rename is one Ctrl+Z, and add a GUI check that renames a path variable through the editor.

### 2026-11-25T13:43:28Z — change-request
- refs: none
- Flaky test from M504 - GIL Lock Order Fix (#341): ProgressiveRenderTest.RegisteringAParticipantDuringAWriteReleasesTheGilWhileWaiting (Tests/Scene/ProgressiveRender_Test.cpp:1836) gave up after its 10 s deadline ("the threads deadlocked waiting for the registrant to wait for the writer lock") on the legacy leg of M256 gate2 (run 45529586619), passing on the task-graph leg of the same commit. It polls /proc until the registrant thread is asleep, which can miss the deadline on a loaded ctest runner, or never see the sleep if the registrant finishes and its tid vanishes. File as an E14 flake fix (or fold into M496 - MoonRay Test Stability's flake list): make the wait condition robust without loosening what it proves.

### 2026-11-25T14:14:54Z — change-request
- refs: none
- New milestone (E14 or the CI epic), user-approved 2026-11-25: **Runner Efficiency** — cut GitHub runner slot use (20-concurrent-job cap is the bottleneck; analysis in the PM session 2026-11-25). Tasks: (1) ci.yml runs only for non-draft PRs (types: opened, synchronize, reopened, ready_for_review; job if: !github.event.pull_request.draft); lane PRs open as drafts and the PM marks them ready after a green gate. (2) Shard remote-build.yml's per-script GUI matrix into at most 4 jobs per gate (gui-matrix emits groups; run-gui-test.sh loops with a per-script timeout), so F15's 21 scripts take 4 slots, not 21. (3) build.sh --ctest-regex <re> (and the workflow input) to run only matching ctest cases on intermediate gates; the final gate runs the full suite. (4) Stop duplicate dev-image builds of identical inputs (concurrency group keyed on the image inputs, or dispatch dev-image.yml once after a Dockerfile change); after a main merge restore main's ccache entry first; find why remote ccache hits are near zero on fast builds (19/1313, 0/23) while debug hits 55/60. (5) With M199 - Remove Legacy Scheduler: drop the legacy ctest leg from remote-build.yml and ci.yml. Verify each with run timings before/after.
