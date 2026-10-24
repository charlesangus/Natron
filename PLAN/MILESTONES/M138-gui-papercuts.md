# M138 - GUI Papercuts

Three small GUI fixes reported on 2026-10-18. Clicking `>` in the file picker crashes Natron. The splash screens and Preferences open on the primary monitor. Ctrl-click colour picking should read the viewed node's displayed (output) image, and read the node's input only while a colour knob's eyedropper is active.

## Scope

**In:**
- Reproduce the file-picker crash, fix it, and add a GUI regression script that clicks both the history `>` and the ▶ preview toggle.
- One pure helper that chooses a screen and centres a window on it, unit-tested with fake screen rectangles.
- That helper is used by `SplashScreen` (`Gui/SplashScreen.cpp:92-94`), `LoadProjectSplashScreen` (`:145-147`) and `PreferencesPanel::showEvent` (`Gui/PreferencesPanel.cpp:1206-1211`).
- Picker defaults:
  - Outside knob picking, Ctrl-click, the Ctrl+Shift rectangle and the hover readout read the output.
  - While an eyedropper is active they read the input.
  - Ctrl+Alt and Ctrl+Shift+Alt always pick the other source.

**Out:**
- Main-window placement and its default size (`Gui/GuiPrivate.cpp:668-680`), which nobody reported. (none)
- The splash's DPI scale, which still comes from the primary screen. (none)
- Native Wayland, where the compositor ignores `move()`. (none)
- How picked values are computed. They stay scene-referred, before any viewer transform, as M69.P4.T1 set. (M69 - Minor Cleanup)

**Edge cases:**
- The splash appears before any window exists, so it uses the screen under the cursor. If `screenAt` returns null (the cursor is between monitors), it falls back to the primary screen.
- Preferences uses the main window's screen. If the main window is hidden or minimised, it uses the screen under the cursor.
- The load-project splash uses the active window's screen if there is one, otherwise the screen under the cursor.
- A window larger than the available area is clamped to the area's top-left. Screens with negative origins (a monitor left of or above the primary) are handled.
- The eyedropper turns on or off mid-drag: the source is fixed at press and kept for the whole drag, and for re-picks at `lastPickerPos`.
- During knob picking on a node with no connected input (a Read, a generator), M69's fallback reads the output and the info bar shows "out".

**Assumptions:**
- Users run X11 or XWayland, where `move()` on a top-level window is honoured.
- Unconfirmed: the crash is on the ▶ preview-toggle path (`Gui/SequenceFileDialog.cpp:436-448`), not the history `>` (`_nextButton`, `:306-311`, whose `nextFolder()` is bounds-checked at `:1302-1310` and greyed at `:1666`). P1.T1 settles this.

## Phase 138.1: File-picker crash

- [ ] M138.P1.T1 — Write the GUI script (all sections) and capture the crash backtrace
  - files: `Tests/gui/m138_papercuts.py` (new), `Gui/SequenceFileDialog.cpp` (object names only)
  - approach:
    - Give `_nextButton`, `_previousButton` and `_togglePreviewButton` object names (`:299`, `:306`, `:439`).
    - Section `file-dialog`: open a Read's file dialog the way `Tests/gui/m75_read.py:452-470` does (a `QTimer` inspects the modal dialog). Then go into a subfolder, Back, `>` ×2 (the second click lands on a greyed-out button); toggle ▶ on with no selection, then with a PNG selected, toggle it off, close the dialog, reopen it and toggle ▶ again.
    - Section `placement` (P2.T2's check) and section `picker-defaults` (P3.T2's checks on this script) are written now, from those tasks' verify text, so one agent owns the file.
    - The PM runs the script against the current build under gdb in natron-dev and records the top frames under Decisions.
    - Suspects to check first: `createViewerPreviewNode` dereferences a null `getNodeGui()` or a null `createNode` result (`:2676-2681`); `viewerUI` left null while `viewerNode` is set, crashing on the next toggle (`:2651-2656`, `:2695-2699`); moving the `ViewerGL` into a new top-level window recreates its GL context and `initializeGL` (`Gui/ViewerGL.cpp:1205`, `Gui/ViewerGLPrivate.cpp:448`) re-runs against stale state.
  - verify: the script reproduces the crash on the current build, and the backtrace is recorded.
  - size: M
- [ ] M138.P1.T2 — Fix the crash at the cause the backtrace shows
  - files: `Gui/SequenceFileDialog.cpp`, and whatever the backtrace names (≤3 files)
  - approach: fix the root cause. Also replace the assert-only guards in `onTogglePreviewButtonClicked` and `createViewerPreviewNode` with real checks: on failure, uncheck the button and leave `_preview` clean so the next click retries. No silent catch-all.
  - verify: the `file-dialog` section passes with no crash, and the ▶ preview shows the PNG.
  - size: M

## Phase 138.2: Open on the user's monitor

- [ ] M138.P2.T1 — Add a screen-placement helper with a unit test
  - files: `Gui/WindowPlacement.h` (new), `Gui/WindowPlacement.cpp` (new), `Tests/WindowPlacement_Test.cpp` (new), `Tests/CMakeLists.txt` (`GuiTests_SOURCES`)
  - approach:
    - Pure functions: `int screenIndexAt(const std::vector<QRect>& geoms, QPoint p, int fallback)`; `QPoint centredTopLeft(const QRect& avail, QSize size)`, clamped to `avail` and keeping its origin.
    - A thin wrapper `placeOnUserScreen(QWidget* w, const QWidget* anchor)`: the anchor window's `screen()` when the anchor is visible and not minimised, else `QGuiApplication::screenAt(QCursor::pos())`, else the primary screen.
  - verify: `GuiTests --gtest_filter=WindowPlacement*` covers two side-by-side screens, a screen at a negative origin, a point in no screen (fallback), and a window larger than the screen (clamped).
  - size: M
- [ ] M138.P2.T2 — Use the helper for both splashes and Preferences
  - files: `Gui/SplashScreen.cpp`, `Gui/PreferencesPanel.cpp`
  - approach: the splash calls `placeOnUserScreen(this, nullptr)` after `resize`/`show` (`:92-94`, `:145-147`); the load-project splash passes `QApplication::activeWindow()` as the anchor. `PreferencesPanel::showEvent` passes `parentWidget()` (the `Gui` window, `Gui/Gui30.cpp:679`).
  - verify: with Xvfb's single screen, `m138_papercuts.py` section `placement` checks that Preferences is centred within the screen's `availableGeometry`. Multi-monitor behaviour is checked at UAT on the AppImage.
  - size: M

## Phase 138.3: Picker defaults

- [ ] M138.P3.T1 — Default the picker source to the output, and to the input only while knob picking
  - files: `Gui/ViewerGL.cpp`, `Gui/ViewerGLPrivate.h`, `Gui/ActionShortcuts.h`, `Gui/GuiApplicationManager10.cpp`, `Gui/InfoViewerWidget.cpp`
  - approach:
    - Add `bool ViewerGL::resolvePickOutput(bool alternate)`: `knob = getGui()->hasPickers(); return alternate ? knob : !knob;`.
    - Use it at the press branches (`:1880-1910`), storing the result in `pickerReadsOutput` for point picks too. Mouse-move re-picks (`:2471-2479`) read the stored value. The hover readout (`:2611`, today a hard-coded `false`) calls `resolvePickOutput(false)`.
    - Rename the shortcuts: `"pickOutput"` → `kShortcutIDMousePickAlternateColor` `"pickAlternate"`, `"rectanglePickOutput"` → `"rectanglePickAlternate"` (`ActionShortcuts.h:334-344`, registrations `:913-916`); `eMouseStatePickingOutputColor` → `eMouseStatePickingAlternateColor`. No saved-shortcut compatibility.
    - Rewrite the in/out tooltip (`InfoViewerWidget.cpp:84-85`).
  - verify: the PM's build compiles cleanly, and P3.T2's script passes.
  - size: M
- [ ] M138.P3.T2 — Update the picker GUI checks to the new defaults
  - files: `Tests/gui/m69_cleanup.py` (section `picker-in-out`, `:1887-1935`); `Tests/gui/m138_papercuts.py`'s `picker-defaults` section is written by P1.T1
  - approach:
    - Without an eyedropper, viewing Constant(0.25)→Grade(×2): hover, Ctrl-click and the rectangle read 0.5 "out"; Ctrl+Alt reads 0.25 "in".
    - With the Grade's colour-knob eyedropper on (click its `ColorPickerLabel`): Ctrl-click reads 0.25 "in" and fills the knob, and Ctrl+Alt reads 0.5 "out".
    - On a Read with the eyedropper on, the pick reads "out".
  - verify: both scripts pass under `Tests/gui/run-gui-test.sh`, with info-bar screenshots of both states.
  - size: M

**Verification gate:**
- One `fast` build after all phases (remote on this host).
- `GuiTests` passes, plus `Tests/gui/run-gui-test.sh` on `m138_papercuts.py` and `m69_cleanup.py`.
- A release AppImage for UAT: the splash and Preferences open on the user's second monitor, and the `>` and ▶ buttons don't crash.

## Decisions

- 2026-10-23 — **Claimed with M119 in new lanes 3 and 4 (user).** Elaborated just-in-time from the stub by a consultant; no user calls were needed.
- 2026-10-23 — **"Current monitor":** the splash uses the screen under the cursor, since no window exists yet. Preferences uses the main window's screen. The load-project splash uses the active window's screen, falling back to the cursor's.
- 2026-10-23 — **Ctrl+Alt now means "the other source"** instead of always the output, so both sources stay reachable in both modes.
- 2026-10-23 — **"Displayed image" means the viewed node's output, still scene-referred.** M69.P4.T1's no-viewer-transform decision stands.
- 2026-10-23 — P3.T2 edits M69's `Tests/gui/m69_cleanup.py`: it asserts the old defaults.
- 2026-10-23 — Build mode on this host is remote: the gate's `fast` build is `tools/ci/remote/build.sh fast --tests`.
- 2026-10-24 — **P1.T1 reproduction (fast build 9cca3b082):** the crash is the ▶ preview toggle, every click, not the history `>`. `onTogglePreviewButtonClicked` (`SequenceFileDialog.cpp:2659`) re-parents the top-level preview viewer (made top-level at `:2718`) into `_centerAreaLayout`; destroying its native window hides the dialog (`QWidgetWindow::~QWidgetWindow` → `QDialogPrivate::setVisible(false)`), ending `exec()` without `finished`. `popOpenFileDialog` (`Gui/Gui20.cpp:1024-1026`) then destroys the dialog with the viewer still inside (`teardownPreview()` runs only from `closeEvent`/`done`, `:2793-2805`). Top frames: `QOpenGLShaderProgram::removeAllShaders` ← `ViewerGL::Implementation::~Implementation` ← `ViewerGL::~ViewerGL` ← … `ViewerTab::~ViewerTab` ← `SequenceFileDialog::~SequenceFileDialog` ← `Gui::popOpenFileDialog` ← `Gui::openImageSequenceDialog` ← `NativeRead::onEffectCreated`.
- 2026-10-24 — Also found and folded into P1.T2 (same file): the "Look in" combo connects the Qt5-only `activated(QString)` (`:289`), so choosing a folder there does nothing; Forward into a folder deleted after Back advances `_currentHistoryLocation` before `setDirectory` fails (`:1306-1312`).
