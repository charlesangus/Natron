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

- [x] M431.P1.T1 — Time panel builds per knob type and per stage
  - files: `Engine/NodeCreationStageLog.h`/`.cpp` (reuse; enabled by `NATRON_NODE_CREATION_STAGE_LOG`), `Gui/KnobGuiContainerHelper.cpp` (`initializeKnobVectorItem` and the row-creation path), `Gui/KnobGui10.cpp` (tooltip), `Gui/NodeGui.cpp` (`createPanel`, docking), `tools/bench/gui/gui_bench.py` (new op)
  - approach: Add stage scopes around one knob row's creation keyed by knob type (`panelKnob:<typeName>`), and sub-stages for tooltip build, widget creation, layout insertion and show/dock. Add a `gui_bench.py` op `open_panel` that, for a list of plugin IDs (default: Blur, Merge, Grade, Roto, Transform, a USD/3D node, the Cycles render node), opens and closes the node's panel N times on an existing node and reports wall time per panel plus the per-stage log. Logging off must cost nothing measurable (the existing `enabled()` check).
  - verify: build and ctest pass; `gui_bench.py --op open_panel` under Xvfb prints per-panel totals and per-knob-type stage times for every listed node.
  - size: M

- [x] M431.P1.T2 — Profile and rank the panel build cost
  - files: no product code; results into this file's `## Decisions`; samples under `build/m431-prof/` in the worktree
  - approach: Run T1's op under Xvfb (relative) and sample a long loop of panel opens with `eu-stack` (`-u root --privileged`, per the profiling recipe in `tools/bench/gui/profile_ops.sh`). Rank where the time goes: show/restyle, QSS polish, tooltips, label/font metrics, signal connections, layouts, per-type widget cost (colour, choice, file, curve knobs). For each cause worth ≥5% of a typical panel, record a fix proposal; the PM files each as a trailing-numbered P2 task before P2 starts. Also ship a release AppImage with a steps `.md` asking the user to run the `open_panel` op on their real display and send back the output, so the targets are judged on it.
  - verify: a ranked breakdown with per-cause ms for at least Blur, Merge, Roto and the Cycles render node is recorded; the fix tasks are filed.
  - size: M

## Phase 431.2: Make it fast

- [x] M431.P2.T1 — Build every panel off-screen and show it once
  - files: `Gui/NodeGui.cpp` (`createPanel`, `startPanelBuild`), `Gui/DockablePanel.cpp`, `Gui/KnobGuiContainerHelper.cpp`, `Gui/Gui*.cpp` (`addVisibleDockablePanel`)
  - approach: Create the panel widget hidden and unparented from the shown properties bin, build every page's rows into it, then dock it and show it in one step, so no `setVisible` on a row ever runs against a visible hierarchy. Keep the current tab, scroll position and page order the synchronous build gives today. Applies to the incremental path while it still exists and to the synchronous path.
  - verify: build and ctest pass; T1's op shows the show/restyle stage gone from per-row cost; Xvfb screenshots of a fresh Merge and Roto panel match the current layout.
  - size: L

- [x] M431.P2.T2 (dropped, see Decisions) — Build knob tooltips on first hover
  - files: `Gui/KnobGui10.cpp` (tooltip builder at :703-771), `Gui/KnobGui.h`, the widgets that call it (label, spin boxes, buttons; find callers of the tooltip builder)
  - approach: Stop calling `setToolTip` at row creation; answer `QEvent::ToolTip` on the row's widgets (an event filter installed per row, or on the label and value widgets) by building the tooltip then, cached until the knob's hint, expression, link or animation state changes. Tooltips must read exactly as today, including the Markdown-converted hints and the link line.
  - verify: build and ctest pass; a GUI check hovers three knobs (a plain hint, a Markdown hint, a linked knob) and compares the tooltip text to today's; T1's op shows the tooltip stage gone from creation.
  - size: M

- [x] M431.P2.T3 — Create secret rows' widgets on first reveal
  - files: `Gui/KnobGuiContainerHelper.cpp`, `Gui/KnobGui*.cpp`
  - approach: A secret or hidden knob's row is not built until it is first shown; no per-child show()/hide() for rows the parent's show covers, so no extra setVisible polish. 15–21% of build in P1.T2.
  - verify: `open_panel` build drops by at least that share on Blur and Merge; GUI scripts that reveal secret knobs still pass.
  - size: M

- [x] M431.P2.T4 — Lighter colour rows
  - files: `Gui/KnobGuiColor.cpp`, `Gui/KnobGuiValue.cpp`
  - approach: Build the colour row's eyedropper, dialog button and per-dimension spin boxes on demand (first expand/hover/click); the collapsed row is one widget. 12.6 ms per row, 48% of Grade's build in P1.T2.
  - verify: Grade's build drops by at least a third; colour knob GUI scripts pass.
  - size: M

- [x] M431.P2.T5 — Sub-stage onKnobsInitialized and fix its top cause
  - files: `Gui/NodeSettingsPanel.cpp`, `Gui/KnobGuiContainerHelper.cpp`, `tools/bench/gui/gui_bench.py`
  - approach: Add stage scopes inside onKnobsInitialized (tabs, pages, group building) and fix whichever dominates; install `open_panel`'s event filter after the open so it doesn't inflate the build. 23–33% of build in P1.T2.
  - verify: the stage log names the cost; the fix cuts it measurably.
  - size: M

- [x] M431.P2.T6 — Collapsed groups and other pages built lazily
  - files: `Gui/KnobGuiContainerHelper.cpp`, `Gui/KnobGuiGroup.cpp`
  - approach: Rows in a collapsed group or a non-current page are built when first expanded or shown. Dock/show is 21% of USDRenderSettings (29 groups).
  - verify: USDRenderSettings' build drops by at least that share; m431_panel_appears_whole passes.
  - size: M

## Phase 431.3: Remove the incremental build

- [x] M431.P3.T1 — Delete the deferred panel build
  - files: `Gui/KnobGuiContainerHelper.cpp`/`.h` (`PendingKnobBuild`, `PendingKnobPage`, `PendingKnobFrame`, `initializeKnobsIncrementally`, `createPendingPages`, `buildNextPendingKnob`, `buildPendingPage`, `runKnobBuildStep`, `ensureKnobGuisCreated`, `finishKnobBuild`, `dropPendingKnob`, `isKnobBuildPending`, `cancelKnobBuild`, `deferChildrenOf`), `Gui/DockablePanel.cpp`/`.h` (`onKnobBuildStarted`), `Gui/NodeGui.cpp`/`.h` (`startPanelBuild(incremental)`, `dockPendingPanel`, `dropUnbuiltPanel`, `_panelDockPending`, `_panelTail*`), callers on the creation and undo paths
  - approach: Only once P2's tasks meet the target (P1.T2's real-display numbers, a typical panel under 16 ms): interactive creation builds the panel synchronously after the node's first paint, in the same deferred slot it uses now for docking, so the node still paints first. Remove every incremental-only branch and member; undo and close destroy a complete panel. Keep the panel-tail work (Roto/Tracker panels, `onKnobsInitialized`) inside the one build so it is shown with the rest.
  - verify: build and full ctest pass; `git grep` finds none of the removed names; M399 - Node Creation Keys And Speed's GUI scripts and benches still pass, with `create_by_shortcut` node-paint latency not worse.
  - size: L

- [x] M431.P3.T2 — GUI test: a panel is never shown partly built
  - files: `Tests/gui/m431_panel_appears_whole.py` (new, run with `Tests/gui/run-gui-test.sh`, modelled on `Tests/gui/m101_merge_panel.py` and `Tests/gui/guitest.py`)
  - approach: Create Blur, Merge and Roto by the create-node shortcut with real key events, and open the panel of an existing Merge by double-click. Poll every event-loop pass with a `QTimer.singleShot(0)` chain; on each pass where the panel `isVisible()`, assert the current page's visible knob-row count equals its final count. Create then undo straight away and assert no crash and no panel left in the properties bin. Keep PySide owner wrappers alive per the repo's GUI-script rules.
  - verify: the script passes against P3.T1's build under Xvfb, and fails against `main` (run once there to confirm it catches the popping).
  - size: M

**Verification gate:** build and full ctest green; `Tests/gui/m431_panel_appears_whole.py` and M399 - Node Creation Keys And Speed's GUI scripts pass remotely via `build.sh --from-run <id> --gui "<scripts>"`; T1's `open_panel` op before/after table (Xvfb, relative) recorded here; on the user's real display a typical panel (Blur, Merge, Grade, Transform) builds in under 16 ms and heavy panels in a few frames; Xvfb screenshots of fresh Merge and Roto panels shared; a release AppImage with its steps `.md` for UAT: create several nodes by shortcut and Tab menu, and open existing nodes' panels — each panel appears at once, complete.

## Decisions

- 2026-11-26 (user): node and panel may appear in the same frame; the panel is built in the creation pass and the node no longer has to paint first (supersedes P3.T1's "the node still paints first").
- 2026-11-26 (user): the app stylesheet may be replaced on knob rows by something faster (a QStyle/QProxyStyle or palette) that keeps the look.
- 2026-11-26 (user): no vsync or swap-interval changes; the user's display settings are not ours to control.

- 2026-11-14 — Fix the build cost, don't hide it (user): building the deferred panel hidden and revealing it once was rejected as papering over a build that is far slower than a few dozen widget rows should be.
- 2026-11-14 — Target (user): a typical panel builds within one frame (under 16 ms) on the user's real display; a few frames is acceptable for very heavy nodes.
- 2026-11-14 — Remove the incremental build (user): once the target is met the deferred, time-sliced build is deleted outright, not kept as a fallback for big panels.
- 2026-11-17 — P1.T1, P2.T1 and P3.T2 code in 644013adf; gate running. open_panel benches a fresh node's panel each rep, since a reopened panel isn't rebuilt. P2.T2 (lazy tooltips) waits for P1.T2's numbers (PM call; ~45 GUI scripts read widget tooltips, so the option depends on whether tooltips cost ≥5%). P1.T2 runs open_panel on the gate AppImage and on main's under Xvfb.
- 2026-11-17 — Gate 44095362805 green (m431_panel_appears_whole 43/43 after the wrapper-lifetime fix; m399 and m101 regressions pass). Next: P1.T2 runs open_panel on this gate's AppImage against main's under Xvfb, then P2.T2 is decided from the numbers.
- 2026-11-17 — **P1.T2 results** (Xvfb, gate 334109146 vs main beta192, 20 opens, paint / build median ms): Blur 110/56 vs 155/60; Merge 157/70 vs 187/74; Grade 293/183 vs 330/190; Roto 434/255 vs 480/252; Transform 197/111 vs 258/117; USD Camera 171/107 vs 220/112; USDRenderSettings 474/360 vs 620/297 (the gate's build includes the dock). `open_panel`'s app-wide Python event filter doubled every figure; the numbers use a copy that installs it after the open, and the in-tree op needs the same fix. Build breakdown: row widgets 24–29%, hiding secret rows 15–21%, onKnobsInitialized 23–33%, dock/show 6–21%, labels 3–5%, KnobGui creation 4%, tooltip text 0.8–1.2%. QSS polish via setVisible is 37–63% inclusive. Colour rows cost 12.6 ms each (48% of Grade). Raw data under the worktree's build/m431-prof/.
- 2026-11-17 — **P2.T2 dropped (PM call):** tooltips are about 1% of a panel, under the 5% bar.
- 2026-11-18 — Gate green on 801f8c5ba (build 44319132335; all ten GUI scripts pass, m138_papercuts 85/85 in 44343610758). The m138 fixes were script-only: fixtures written once per run (a Read refuses a file rewritten after its first read), plus the file dialog's dead preview reader (cd50f72e4, pre-existing on main). P3.T1 waits for the real-display timing UAT against the target.
- 2026-11-18 — Review round 1 on #315 fixed in 2643bdf8e (+ the hide-unmodified test fix), gate 44348488426 green with the panel script rerun 44368411415. UAT AppImage rebuilt from 2643bdf8e; P3.T1 waits for the real-display timings.
- 2026-11-21 — **UAT round 1 (user):** "too slow and seems to be knob-number or node panel complexity-based time to open the panel; i don't really understand why drawing a node panel should take any appreciable amount of time? something is wrong if it's taking a noticeable amount of time, keep investigating." Investigation dispatched: per-step profiling of panel open on a real display.
- 2026-11-21 — UAT investigation fixes in 9297013fb: rows built in their final parent (reparenting re-styled every widget), no redundant reparent on dock, a lazy warning icon and grid spacer, curve-editor visibility looking up one node. New dock, layout and first-paint stage timers. Measured before (N100, Xvfb, paint/build ms): Blur 114/59, Grade 297/187, Roto 445/261, USDRenderSettings 490/377. Gate running; then `open_panel --stages` on the same nodes.
- 2026-11-21 — Bench after 9297013fb (N100, Xvfb, `open_panel --stages`, 10 opens; paint/build median ms, before → after):
  - Blur 114/59 → 69/30
  - Merge 164/73 → 113/43
  - Grade 297/187 → 163/71
  - Roto 445/261 → 174/83
  - Transform 202/116 → 151/71
  - USDRenderSettings 490/377 → 112/76

  That's 1.5–4x faster but still far over the 16 ms budget. Top remaining stages: panelBuildKnobs 15–45, panelDock 13–32 (polish 4–23), panelFirstPaint 12–17, and Color rows (Grade) 24. Round 3 dispatched: build only the shown page's rows and the rest on first show, per-widget style-sheet cost, and the open-to-paint gap.

- 2026-11-26 — **UAT round 2 (user):** "still a noticeable lag on making a node before the properties panel pops up ... a node properties panel is fairly simple, qt should have no problem creating and painting it in no more than a few frames". PM: every measurement so far timed the panel build under Xvfb; the lag may lie elsewhere between the choice and the first painted panel (creation, Python callbacks, renders triggered on the main thread, graph/dope-sheet updates). Adding an opt-in end-to-end trace (`NATRON_TRACE_NODE_CREATION=1`) with main-thread gap attribution, plus a plain-Qt panel floor probe; the user runs it on their real display.
- 2026-11-26 (PM) — Trace in 937d3a6a48 (`NATRON_TRACE_NODE_CREATION=1`: stages from the Tab/menu event to the panel's first shown frame, main-thread gaps over 4 ms attributed to Qt events; `tools/bench/gui/panel_floor.py` for a plain-Qt floor). Main merged (94a1d4259e; two conflicts, both kept). **User:** run a cloud GPU profile if needed. Harness in `build/vast-stage-m431/` + `build/vast-logs/m431-trace.sh`: drives Blur/Grade/Merge/Transform/Roto/USDCamera through the real Tab menu ×3 on a real NVIDIA Xorg, records the trace, perf (dwarf call graphs) with the debug-symbol tarball, and the panel floor. m431-gate7 (release --tests --debug-symbols + GUI) queued for its AppImage.
- 2026-11-26 — UAT round 2 lag trace (cloud GPU run cloud-gpu-20261127T002316Z, real NVIDIA Xorg): Tab menu to first shown panel frame is 53 ms (Blur) to 125 ms (Roto) median, 8–32% faster than before (Roto 190→130 ms). The commits 26ff2c3754..d4d3499c92 (one-pass build, viewer toolbar built later, layout before first frame, Tab popup closed before creation, build straight after createNode) respond to it. Remaining main-thread gaps over 4 ms are mostly UpdateRequest on the Gui (5–16 ms each). Main merged in a7fe0e6f5a; the m431_panel_appears_whole failure in gate12 was the script's wrapper lifetime, fixed in a3c9bb6df3.
- 2026-11-27 (PM) — Xvfb bench, one sitting on the N100, mixed topology, `--preset create --ops create_by_shortcut,open_panel --sizes 0,300`; before = main (M599's 60af66f86f AppImage, CI-only changes over main), after = M431 a7fe0e6f5a+e30562976f (gate4). compare.py: 0 regressions.

  | Op | Nodes | Before | After | Change |
  |---|---|---|---|---|
  | create_by_shortcut | 0 | 152.7 ms | 58.6 ms | −61.7% |
  | create_by_shortcut | 300 | 147.4 ms | 72.2 ms | −51.0% |
  | open_panel | 0 | 532.9 ms (p95 1067) | 334.2 ms (p95 415) | −37.3% |
  | open_panel | 300 | 546.3 ms (p95 1091) | 332.1 ms (p95 434) | −39.2% |
- 2026-11-28 — **UAT approved (user).** Main merged (4c6534dba1; conflicts in GuiApplicationManager.cpp — kept main's makeNodesMenuCreateNodeArgs plus the immediate panel build — and TimeLineGui.cpp includes). PR #315 ready; merge on green CI.
