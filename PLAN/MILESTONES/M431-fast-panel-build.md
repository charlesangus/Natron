# M431 - Fast Panel Build

Since M399 - Node Creation Keys And Speed, a node created interactively docks its properties
panel first and then fills it over 8 ms passes (`KnobGuiContainerHelper::runKnobBuildStep`),
showing each knob row as it is built. On a real display the knobs pop in one at a time
(user, 2026-11-14, found while testing F9 - Delegate Projection's UAT). The cause is a panel
build that costs far too much: M399 measured 117-195 ms under Xvfb (P4.T3) and deferred it
rather than fixing it. This milestone makes the build itself fast, then removes the deferred
build so every panel appears complete, in one frame, built synchronously.

Known suspects, unmeasured as to their split (M399's samples are gone):
- Rows are shown on an already-visible panel: M399's round-4 profile put most of each
  pass's cost in Qt's per-widget `setVisible` (restyle, ancestor relayout, X round trip).
- Every knob's tooltip is built at creation, including a Markdown → HTML conversion
  (`Gui/KnobGui10.cpp:703-771`), though it is only needed on hover.
- QSS stylesheet polish per widget.

## Scope

**In:**
- Measuring where panel build time goes, per knob type and per stage, for representative
  panels.
- Fixing the hot spots: build the panel off-screen and show it once, lazy tooltips, and the
  further fixes the profile ranks (inserted as new P2 tasks by the PM, trailing-numbered).
- Removing the incremental build entirely once the targets are met: the pass scheduler,
  pending pages, deferred children, `initializeKnobsIncrementally`, `isKnobBuildPending`,
  `cancelKnobBuild`, `dropUnbuiltPanel`, `dockPendingPanel`, `onKnobBuildStarted`.
- Both build paths: interactive creation and opening the panel of an existing node.
**Out:**
- Node-graph creation latency other than the panel (M399 - Node Creation Keys And Speed's
  graph work stays as is).
- F16 - User Knob Editor's in-panel editing (M372 - User Knob Engine Edits, M373 - In-Panel
  Knob Editor).
**Edge cases:**
- Heavy panels (the Cycles/MoonRay render settings, Roto with its shape table, Tracker,
  USD/3D nodes with many pages): a few frames is acceptable (user, 2026-11-14), still built
  synchronously and shown once — never filled in on screen.
- Multi-instance (Tracker) and main-instance hidden panels: they already build
  synchronously; they must keep working when the incremental path is gone.
- Undo right after creation: with no pending build, undo destroys a complete panel; it must
  stay cheap (no extra rebuild).
**Assumptions:**
- Real-display timings differ from Xvfb (llvmpipe compositing, M399 decision 2026-11-12);
  Xvfb numbers are relative, the targets are judged on the user's real display.

## Phase 431.1: Measure

- [ ] M431.P1.T1 — Time panel builds per knob type and per stage
  - files: `Engine/NodeCreationStageLog.h`/`.cpp` (reuse; enabled by `NATRON_NODE_CREATION_STAGE_LOG`), `Gui/KnobGuiContainerHelper.cpp` (`initializeKnobVectorItem` and the row-creation path), `Gui/KnobGui10.cpp` (tooltip), `Gui/NodeGui.cpp` (`createPanel`, docking), `tools/bench/gui/gui_bench.py` (new op)
  - approach: Add stage scopes around one knob row's creation keyed by knob type (`panelKnob:<typeName>`), and sub-stages for tooltip build, widget creation, layout insertion and show/dock. Add a `gui_bench.py` op `open_panel` that, for a list of plugin IDs (default: Blur, Merge, Grade, Roto, Transform, a USD/3D node, the Cycles render node), opens and closes the node's panel N times on an existing node and reports wall time per panel plus the per-stage log. Logging off must cost nothing measurable (the existing `enabled()` check).
  - verify: build and ctest pass; `gui_bench.py --op open_panel` under Xvfb prints per-panel totals and per-knob-type stage times for every listed node.
  - size: M

- [ ] M431.P1.T2 — Profile and rank the panel build cost
  - files: no product code; results into this file's `## Decisions`; samples under `build/m431-prof/` in the worktree
  - approach: Run T1's op under Xvfb (relative) and sample a long loop of panel opens with `eu-stack` (`-u root --privileged`, per the profiling recipe in `tools/bench/gui/profile_ops.sh`). Rank where the time goes: show/restyle, QSS polish, tooltips, label/font metrics, signal connections, layouts, per-type widget cost (colour, choice, file, curve knobs). For each cause worth ≥5% of a typical panel, record a fix proposal; the PM files each as a trailing-numbered P2 task before P2 starts. Also ship a release AppImage with a steps `.md` asking the user to run the `open_panel` op on their real display and send back the output, so the targets are judged on it.
  - verify: a ranked breakdown with per-cause ms for at least Blur, Merge, Roto and the Cycles render node is recorded; the fix tasks are filed.
  - size: M

## Phase 431.2: Make it fast

- [ ] M431.P2.T1 — Build every panel off-screen and show it once
  - files: `Gui/NodeGui.cpp` (`createPanel`, `startPanelBuild`), `Gui/DockablePanel.cpp`, `Gui/KnobGuiContainerHelper.cpp`, `Gui/Gui*.cpp` (`addVisibleDockablePanel`)
  - approach: Create the panel widget hidden and unparented from the shown properties bin, build every page's rows into it, then dock it and show it in one step, so no `setVisible` on a row ever runs against a visible hierarchy. Keep the current tab, scroll position and page order the synchronous build gives today. Applies to the incremental path while it still exists and to the synchronous path.
  - verify: build and ctest pass; T1's op shows the show/restyle stage gone from per-row cost; Xvfb screenshots of a fresh Merge and Roto panel match the current layout.
  - size: L

- [ ] M431.P2.T2 — Build knob tooltips on first hover
  - files: `Gui/KnobGui10.cpp` (tooltip builder at :703-771), `Gui/KnobGui.h`, the widgets that call it (label, spin boxes, buttons; find callers of the tooltip builder)
  - approach: Stop calling `setToolTip` at row creation; answer `QEvent::ToolTip` on the row's widgets (an event filter installed per row, or on the label and value widgets) by building the tooltip then, cached until the knob's hint, expression, link or animation state changes. Tooltips must read exactly as today, including the Markdown-converted hints and the link line.
  - verify: build and ctest pass; a GUI check hovers three knobs (a plain hint, a Markdown hint, a linked knob) and compares the tooltip text to today's; T1's op shows the tooltip stage gone from creation.
  - size: M

## Phase 431.3: Remove the incremental build

- [ ] M431.P3.T1 — Delete the deferred panel build
  - files: `Gui/KnobGuiContainerHelper.cpp`/`.h` (`PendingKnobBuild`, `PendingKnobPage`, `PendingKnobFrame`, `initializeKnobsIncrementally`, `createPendingPages`, `buildNextPendingKnob`, `buildPendingPage`, `runKnobBuildStep`, `ensureKnobGuisCreated`, `finishKnobBuild`, `dropPendingKnob`, `isKnobBuildPending`, `cancelKnobBuild`, `deferChildrenOf`), `Gui/DockablePanel.cpp`/`.h` (`onKnobBuildStarted`), `Gui/NodeGui.cpp`/`.h` (`startPanelBuild(incremental)`, `dockPendingPanel`, `dropUnbuiltPanel`, `_panelDockPending`, `_panelTail*`), callers on the creation and undo paths
  - approach: Only once P2's tasks meet the target (P1.T2's real-display numbers, a typical panel under 16 ms): interactive creation builds the panel synchronously after the node's first paint, in the same deferred slot it uses now for docking, so the node still paints first. Remove every incremental-only branch and member; undo and close destroy a complete panel. Keep the panel-tail work (Roto/Tracker panels, `onKnobsInitialized`) inside the one build so it is shown with the rest.
  - verify: build and full ctest pass; `git grep` finds none of the removed names; M399 - Node Creation Keys And Speed's GUI scripts and benches still pass, with `create_by_shortcut` node-paint latency not worse.
  - size: L

- [ ] M431.P3.T2 — GUI test: a panel is never shown partly built
  - files: `Tests/gui/m431_panel_appears_whole.py` (new, run with `Tests/gui/run-gui-test.sh`, modelled on `Tests/gui/m101_merge_panel.py` and `Tests/gui/guitest.py`)
  - approach: Create Blur, Merge and Roto by the create-node shortcut with real key events, and open the panel of an existing Merge by double-click. Poll every event-loop pass with a `QTimer.singleShot(0)` chain; on each pass where the panel `isVisible()`, assert the current page's visible knob-row count equals its final count. Create then undo straight away and assert no crash and no panel left in the properties bin. Keep PySide owner wrappers alive per the repo's GUI-script rules.
  - verify: the script passes against P3.T1's build under Xvfb, and fails against `main` (run once there to confirm it catches the popping).
  - size: M

**Verification gate:** build and full ctest green; `Tests/gui/m431_panel_appears_whole.py` and M399 - Node Creation Keys And Speed's GUI scripts pass remotely via `build.sh --from-run <id> --gui "<scripts>"`; T1's `open_panel` op before/after table (Xvfb, relative) recorded here; on the user's real display a typical panel (Blur, Merge, Grade, Transform) builds in under 16 ms and heavy panels in a few frames; Xvfb screenshots of fresh Merge and Roto panels shared; a release AppImage with its steps `.md` for UAT: create several nodes by shortcut and Tab menu, and open existing nodes' panels — each panel appears at once, complete.

## Decisions

- 2026-11-14 — Fix the build cost, don't hide it (user): building the deferred panel hidden and revealing it once was rejected as papering over a build that is far slower than a few dozen widget rows should be.
- 2026-11-14 — Target (user): a typical panel builds within one frame (under 16 ms) on the user's real display; a few frames is acceptable for very heavy nodes.
- 2026-11-14 — Remove the incremental build (user): once the target is met the deferred, time-sliced build is deleted outright, not kept as a fallback for big panels.
- 2026-11-17 — P1.T1, P2.T1 and P3.T2 code in 644013adf; gate running. open_panel benches a fresh node's panel each rep, since a reopened panel isn't rebuilt. P2.T2 (lazy tooltips) waits for P1.T2's numbers (PM call; ~45 GUI scripts read widget tooltips, so the option depends on whether tooltips cost ≥5%). P1.T2 runs open_panel on the gate AppImage and on main's under Xvfb.
- 2026-11-17 — Gate 44095362805 green (m431_panel_appears_whole 43/43 after the wrapper-lifetime fix; m399 and m101 regressions pass). Next: P1.T2 runs open_panel on this gate's AppImage against main's under Xvfb, then P2.T2 is decided from the numbers.
