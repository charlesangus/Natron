# M243 - Startup Pane Paint On NVIDIA

On NVIDIA GL under KWin, a newly started Natron window shows its panes unpainted (the #202020 window colour) for about 16-18 s after the window maps. Only the menu bar and the left toolbar draw. M168 - NVIDIA Viewer Hand-off Validation found it on bosley-faux (RTX 3070 Ti, driver 580.178.04, display :0); the screenshot is `.local/m168/diag/run1-live.png` on that host. It also happens with `NATRON_GPU=off`, so it is not the GPU hand-off, and it never happens under Xvfb with Mesa, so no CI job sees it. It is a user-visible startup defect on a common desktop configuration.

Nothing in the repository says what blocks the first paint, so this milestone diagnoses before it fixes. P1 adds a timestamped startup trace and a host-display probe that samples the screen while Natron starts, then uses them on bosley-faux to name the blocking stage (GL context or QOpenGLWidget creation, shader compile, plugin scan, font and icon loading, or the splash and event pumping). P2 fixes that stage and adds a guard test. P3 checks the fix on the real display.

Related startup work: M158 - Node Toolbar As Menu (`cc6ad6c63`) removed an event pump from `runInitGuiScripts` because the splash pumped events with the main window half-built, which crashed Qt's GL compositor on AMD and NVIDIA. Any fix here must keep the main window from being shown or pumped half-built.

## Scope

**In:**
- A startup trace (`NATRON_STARTUP_TRACE=1`) with millisecond timestamps for the stages from process start to every pane's first paint.
- A probe that launches Natron on an existing display and records when the panes stop being #202020.
- A diagnosis on bosley-faux, recorded in `## Decisions`, that names the blocking stage.
- A fix to that stage, and a GUI test that fails when a pane's first paint comes late.
- A host-display check of the fix.

**Out:**
- The GPU viewer hand-off and its tests (M107 - GPU Viewer Hand-off, M168 - NVIDIA Viewer Hand-off Validation).
- NVIDIA test failures (M244 - NVIDIA Test Fixes).
- Startup time as such (plug-in scan speed, first-frame latency). Only the unpainted window is in scope, and a stage is only fixed if it blocks the first paint.
- Platforms other than Linux x86-64.
- Reproducing on Xvfb or CI. It cannot be, so the guard test checks the trace and does not claim to reproduce.

**Edge cases:**
- The probe samples the root window, so a covering window or another monitor corrupts it. It pins the window as `Tests/gui/guitest.py` `pin_host_window` does, and measures the pane rectangles only.
- A pane that is legitimately #202020 (an empty viewer background, say) must not count as unpainted: the probe decides from the whole window's share of #202020 pixels in pane areas, and the trace decides from paint events.
- First launch with a fresh HOME blocks on the update dialog. The probe pre-seeds `checkForUpdates=false`, as `Tests/gui/run-gui-test.sh` does.
- A warm vs. cold NVIDIA shader cache (`~/.nv/GLCache`) may change the delay. The diagnosis records both.

**Assumptions:**
- Unverified: the 16-18 s is a single blocking stage and not several smaller ones. If P1.T3 finds several, P2 gets one task per stage.
- Unverified: PySide6 is importable on bosley-faux outside the dev container. The probe falls back to `import` (ImageMagick) or `xwd` for the screen grab if it is not.
- The M168 delay is reproducible on every start on bosley-faux, not only the first.

## Phase 243.1: Diagnose

- [ ] M243.P1.T1 — Add a startup trace
  - files: `Global/StartupTrace.h` (new, header-only), `App/NatronApp_main.cpp`, `Engine/AppManager.cpp`, `Gui/GuiApplicationManager.cpp`, `Gui/Gui.cpp`
  - approach: `startupTrace(const char* stage)` writes `startup-trace: <ms since process start> <stage>` to stderr when `NATRON_STARTUP_TRACE` is set, and is a cheap no-op otherwise (read the env var once into a static). Call it at process entry, after QApplication construction, splash shown and hidden, plug-in scan begin and end, project and settings load, main window constructed, `Gui::show` called, the first `QPaintEvent` of each pane (install a one-shot event filter on the pane widgets the layout creates: viewer, node graph, properties, curve editor, dope sheet, script editor), and viewer GL context creation (`initializeGL`) and its first `paintGL`. Do not pump events or change behaviour.
  - verify: a release build started with `NATRON_STARTUP_TRACE=1` under Xvfb prints the stages in order with non-decreasing timestamps, and prints nothing without the variable. `ctest -R "Startup|Gui"` still passes.
  - size: M

- [ ] M243.P1.T2 — Write the host-display startup paint probe
  - files: `Tests/gui/startup-paint-probe.sh` (new), `Tests/gui/startup_paint_sampler.py` (new)
  - approach: the shell script takes `<natron-binary>` and `GUI_TEST_HOST_DISPLAY` (required), creates a throwaway HOME seeded with `checkForUpdates=false` as `run-gui-test.sh` does, starts Natron on that display with `NATRON_STARTUP_TRACE=1` and the caller's environment (so `QT_LOGGING_RULES`, `__GL_SHADER_DISK_CACHE`, `NATRON_GPU` pass through), and runs the sampler beside it. The sampler grabs the root window every 250 ms for 60 s (QScreen.grabWindow(0), falling back to `import -window root`), finds the Natron window, and records the time and the share of pixels equal to #202020 in the area below the menu bar and right of the toolbar. It writes `startup-paint.csv` and prints the first time the share drops under 5 % and stays under, next to the map time, and copies Natron's stderr to `natron.log` in `NATRON_GUI_TEST_OUT`. It refuses a software GL renderer unless `GUI_TEST_ALLOW_SOFTWARE_GL=1`, with the same check as `run-gui-test.sh`. It kills Natron at the end.
  - verify: under Xvfb with Mesa (`GUI_TEST_ALLOW_SOFTWARE_GL=1`) the probe runs to the end, writes the CSV, and reports panes painted within a few seconds of map.
  - size: M

- [ ] M243.P1.T3 — Run the probe on bosley-faux and name the blocking stage
  - files: this milestone file (`## Decisions`), data under `.local/m243/` (not committed)
  - approach: build `fast` with P1.T1 and run the probe on display :0 against it. Compare the trace timestamps with the CSV, and run these variants, keeping the CSV and trace for each: `NATRON_GPU=off`; `QT_LOGGING_RULES="qt.qpa.*=true;qt.rhi.*=true;qt.opengl=true"`; `__GL_SHADER_DISK_CACHE=0` and a cleared `~/.nv/GLCache`; a second start with warm Natron caches (plug-in scan cached); `QT_XCB_GL_INTEGRATION=none` or `QT_OPENGL=software` to test whether the viewer's GL widget is the cause; a start with the viewer pane removed from the layout; a start with the splash disabled; KWin compositing suspended (Alt+Shift+F12) for one run. Add a `perf`-free sample of the main thread during the stall (`eu-stack -p` a few times, or `gdb -batch -ex bt` if the host has it; the container does not apply here). Record in `## Decisions` the stage whose end coincides with the first paint, the variant that moved it, and the evidence. If it is the compositor or driver and not Natron code, say so and name the Natron-side mitigation that works.
  - verify: the Decisions entry names one stage (or several, each with its time share) with the trace lines and CSV excerpts that show it, and the run data is in `.local/m243/`.
  - size: L

## Phase 243.2: Fix and guard

- [ ] M243.P2.T1 — Fix the blocking stage
  - files: set by the P1.T3 finding. The candidates, in the order the trace points to them: `Gui/Gui.cpp`, `Gui/GuiApplicationManager.cpp`, `Gui/ViewerGL.cpp`, `Engine/AppManager.cpp`, `Gui/SplashScreen.cpp`. The brief is rewritten with the real list before this task runs (PLAN-FORMAT section 5a).
  - approach: change only the blocking stage so the panes paint before it, or without waiting for it. Examples by finding: a viewer GL context created lazily or after the first show instead of in the window constructor; a shader compile moved after the first paint or cached; plug-in scan or font and icon loading moved off the GUI thread or deferred past the first paint; the splash hidden, or the window shown, at a point where the window is complete (never pump events with the window half-built, see M158); a repaint requested explicitly after a stage that leaves the compositor with a stale surface. Keep the trace calls.
  - verify: on bosley-faux the probe reports the panes painted within 3 s of map on three consecutive starts, with and without `NATRON_GPU=off`. The existing GUI tests still pass under Xvfb: `Tests/gui/run-gui-test.sh Tests/gui/m137_panels.py` and `Tests/gui/m158_nodes_menu.py`.
  - size: L

- [ ] M243.P2.T2 — GUI test: every pane paints soon after the window maps
  - files: `Tests/gui/m243_startup_paint.py` (new), `Tests/gui/guitest.py` (only if a helper is needed)
  - approach: the test runs inside the started Natron, so it cannot see the trace's start-up lines through stderr. P1.T1 also keeps the stage timestamps in a process-global list the test reads through a hidden debug binding or, if that is too invasive, from `natron.log` in `NATRON_GUI_TEST_OUT` (parse the `startup-trace:` lines; the test sets `NATRON_STARTUP_TRACE=1` through its `.natronpath`-style launcher environment). It fails when a pane's first paint is later than 5 s after `Gui::show`, or never happens, for each pane the default layout has. It also checks that a grab of the viewer pane is not all #202020 once the first paint has been reported.
  - verify: passes under Xvfb with `Tests/gui/run-gui-test.sh Tests/gui/m243_startup_paint.py`; reverting the P2.T1 fix on bosley-faux with `GUI_TEST_HOST_DISPLAY=:0` makes it fail (record the failing run in Decisions).
  - size: M

## Phase 243.3: Host-display check

- [ ] M243.P3.T1 — Check the fix on the NVIDIA display and share screenshots
  - files: none in code. Output under `.local/m243/`, results in this milestone file.
  - approach: on bosley-faux, with the release AppImage from `tools/ci/local/package.sh` (not the raw binary), run the probe on :0 five times (cold and warm shader cache, `NATRON_GPU=off` and default), and `GUI_TEST_HOST_DISPLAY=:0 Tests/gui/run-gui-test.sh Tests/gui/m243_startup_paint.py <AppImage>`. Take a screenshot of the window 1 s, 3 s and 20 s after map and show them to the user for sign-off before the task is checked.
  - verify: all five probe runs report painted panes within 3 s of map, the test passes, and the user has seen the screenshots.
  - size: M

**Verification gate:** remote `fast --tests` CI green (the trace is a no-op off). On bosley-faux (RTX 3070 Ti, KWin, display :0), with the packaged release AppImage: `Tests/gui/startup-paint-probe.sh` reports every pane painted within 3 s of map on five starts, with and without `NATRON_GPU=off`, `GUI_TEST_HOST_DISPLAY=:0 Tests/gui/run-gui-test.sh Tests/gui/m243_startup_paint.py` passes, and the user has approved the screenshots. The release gate (`ctest` in the dev container, both schedulers) passes.
