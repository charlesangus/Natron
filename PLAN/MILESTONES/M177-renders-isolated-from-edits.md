# M177 - Renders Isolated From Edits

No render but the viewer can see a knob edit made while it runs. Foreground Write renders and tracking block the UI behind an application-modal progress dialog with Cancel. Background renders always run in a separate `NatronRenderer` process from the project saved at submission. `abortAnyEvaluation` aborts only viewer renders on an edit; deactivation and deletion still abort everything.

Today (M167's branch) an edit aborts every downstream output engine (`Engine/EffectInstance.cpp:6050-6093`). The Render dialog's "background" option renders in-process when the "Render in a separate process" preference (`Engine/Settings.cpp:410-414`) is off. Tracking runs with the UI live (`Engine/TrackerContext.cpp:1491-1502`, progress panel only). Renders read knob values live (`Knob<T>::getValue` under `_valueMutex`, `Engine/KnobImpl.h:703-739`); only hashes and metadata are snapshotted, in `ParallelRenderArgsSetter::buildArgsMap` (`Engine/ParallelRenderArgs.cpp:1284`). So isolation means no write can reach a running non-viewer render; snapshotting knob values is not the approach.

Reused foundations: M55's application-modal foreground progress dialog (`Gui/RenderDialog.cpp:614-733`); separate-process renders already save the project at submission, one file per item (`Engine/AppInstance.cpp:1983-1993`); tracking is already cancellable through its progress task (`Engine/TrackerContext.cpp:1493,1507`).

Filed from M167 - Per-Frame Node Metadata, Phase 3, which inherited M144's "edits during non-abortable renders" acceptance; the user ruled such edits must be impossible rather than tolerated. Deps: M167 - Per-Frame Node Metadata.

## Scope

**In:**
- An edit (keepOldestRender path: `Knob.cpp:6853,7015`, `KnobImpl.h:1146,1562`, `NodeInputs.cpp:822,968,1016,1116,1245`) aborts and restarts only `ViewerInstance` engines. Deactivation (`Node.cpp:4212`) and deletion (`Node.cpp:2800-2808`) abort everything, as now.
- `AppInstance::startWritersRendering` takes an explicit mode (Foreground / Background / Blocking). In a GUI session Background always means a separate process. The "Render in a separate process" preference and `rendersInSeparateProcess()` (`Gui/RenderDialog.cpp:166`) are deleted.
- Foreground in-process renders always show the modal: the Render dialog, the Write Render knob triggered from Python, `app.render` from the GUI, and DiskCache pre-render.
- A restarted (paused) background task re-renders its submission file, not a fresh save.
- Tracking (track forward/backward/previous/next/range, TrackerPM, `PyTracker` from the GUI) shows the modal; Cancel aborts tracking.
- Script writes during a foreground render or track are refused with an error (Phase 177.5).
- Tests: gtests for the abort rule and the render modes; Xvfb tests for the modal, tracking and the frozen background copy.

**Out:**
- Per-frame metadata snapshots (M167). Moving the `RenderEngine` to `Node` (M198). Clean-hash gating of the identity, actions and image caches (M202).
- Command-line and `NatronRenderer` renders: no UI; they keep Blocking mode.
- In-process graph cloning for background renders: the separate process is the frozen copy.

**Edge cases:**
- Colour sampler: it already runs synchronously on the main thread (`Gui/ViewerGL.cpp:3006,4105,5079`; `Engine/NodeColorSampler.cpp:219`), so nothing in the GUI can edit during it. No modal unless the user asks for one.
- `GuiApp.renderBlocking` (`Gui/PyGuiApp.cpp:599-615`) blocks the main thread in `QtConcurrent::blockingMap` (`AppInstance.cpp:2014-2023`), with no repaint and no Cancel; it already isolates.
- A modal started from inside another modal (Preferences, a Python panel): the dialog is parented to `Gui` with `Qt::ApplicationModal` (`RenderDialog.cpp:730-731`), so it stacks over the open one.
- Cancel mid-frame: frames in flight finish or are dropped, as `RenderSchedulerAbort_Test.cpp:587-660` already pins for the engine.
- Tracker keyboard shortcuts and viewer buttons: the application-modal dialog takes all input; the tests check this.
- Viewer previews and thumbnails are not output engines; deactivation still aborts them (`Node.cpp:4213`).

**Assumptions:**
- The separate-process path freezes the project at submission: `saveForRenderProcess` runs per item inside `startWritersRendering` (`AppInstance.cpp:1986`), and the save ignores user input while it waits (`Project.cpp:796`). Confirmed in code.
- Unconfirmed: pausing a separate-process task kills the child, and the child deletes its save file once loaded (`AppInstance.cpp:689`). Either would make restart re-save the live project.
- Unconfirmed: a before-frame callback (`OutputSchedulerThread.cpp:2331-2368`) that writes an upstream knob aborts its own Write render today.
- Unconfirmed: writers leave no partial file on cancel.

## Phase 177.1: The edit rule and render modes

- [x] M177.P1.T1 — Edits abort only viewer renders
  - files: `Engine/EffectInstance.cpp` (`abortAnyEvaluation` :6050-6093), `Tests/RenderIsolation_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: In the loop over `outputNodes`, when `keepOldestRender` is true (the edit path), call `abortRenderingAutoRestart()` only on engines whose effect is a `ViewerInstance`. When it is false (deactivation), keep aborting every engine. Leave `hasOutputNodesConnected` (`Node.cpp:4110`) alone; M198 re-keys it.
  - verify: three gtests. (1) A blocking 20-frame Write render, built like `RenderSchedulerAbort_Test.cpp:587-660`, with a `frameRendered` DirectConnection that edits an upstream knob at frame 3 from a second thread: `renderFinished` gives 0 and all 20 files exist. (2) Deactivating the upstream node at frame 3 gives 1. (3) In the `ViewerScheduler` fixture (`Tests/ViewerScheduler_Test.cpp`), an edit during a viewer render aborts it.
  - size: M

- [x] M177.P1.T2 — Name the two abort intents
  - files: `Engine/EffectInstance.h`, `Engine/EffectInstance.cpp`, `Engine/Knob.h` (:2801-2807), `Engine/Knob.cpp`, `Engine/KnobImpl.h`, `Engine/NodeInputs.cpp`, `Engine/Node.cpp`
  - approach: Split `abortAnyEvaluation(bool)` into `abortViewerRendersForEdit()` and `abortAllRendersReadingThis()`, and rename `abortAnyEvaluationForChange` to match. A mechanical rename the compiler drives, with no behaviour change.
  - verify: phase build; `git grep -n abortAnyEvaluation Engine Gui` is empty; P1.T1's tests pass.
  - size: M

- [x] M177.P1.T3 — Explicit render mode; background means separate process
  - files: `Engine/AppInstance.h`, `Engine/AppInstance.cpp` (:1951-2051), `Engine/Settings.h`, `Engine/Settings.cpp` (:410-414, :2859), `Gui/RenderDialog.cpp`, plus one-line caller edits
  - approach:
    - Add `enum RenderModeEnum {eRenderModeForeground, eRenderModeBackground, eRenderModeBlocking}` and pass it to `startWritersRendering`.
    - Background in a GUI app → `saveForRenderProcess` + `ProcessHandler`. Foreground → in-process, non-blocking. Blocking, or any mode when `appPTR->isBackground()` → today's `blockingMap` path.
    - Delete the preference and `rendersInSeparateProcess()`; `resolveRenderRange`'s `separateProcess` becomes the dialog's background checkbox.
    - Map callers to their current intent: `true` → Blocking; the dialog's background branch (`RenderDialog.cpp:755`) → Background; `runForegroundRender` (:632) → Foreground; `ProgressTaskInfo.cpp:339`, `Node.cpp:6709`, `DiskCacheNode.cpp:170`, `PyAppInstance.cpp:359,406` → Foreground for now (Phase 177.2 routes them through the modal).
  - verify: phase build; `SchedulerWriters`, `BlockingRenderIdle`, `RenderSchedulerAbort` and `RenderDialogRange` pass; `git grep -n isRenderInSeparatedProcessEnabled` is empty.
  - size: M

**Verification gate:** one `tools/ci/remote/build.sh fast --tests` passes in both scheduler legs, including `RenderIsolation`.

## Phase 177.2: Foreground renders behind the modal

- [x] M177.P2.T1 — Extract the foreground modal
  - files: `Gui/ForegroundTaskDialog.h`, `Gui/ForegroundTaskDialog.cpp` (new), `Gui/RenderDialog.cpp` (:493-733), `Gui/CMakeLists.txt`
  - approach: Move `ForegroundRenderState`, `refreshForegroundRender`, `cancelForegroundRender` and the dialog setup into a class bound to a list of `ProgressTaskInfoPtr` and labels. Keep `Qt::ApplicationModal` with `show()`, not `open()`; keep bar updates off `setValue`; keep deferred error dialogs. `runForegroundRender` uses it.
  - verify: phase build; `Tests/gui/m76_write.py` and `m55_node_graph.py` pass under Xvfb.
  - size: M

- [x] M177.P2.T2 — One foreground entry point on the app
  - files: `Engine/AppInstance.h`, `Engine/AppInstance.cpp`, `Gui/GuiAppInstance.h`, `Gui/GuiAppInstance.cpp`, `Gui/RenderDialog.cpp`
  - approach: Add virtual `AppInstance::renderForeground(works)`. The base renders Blocking; `GuiAppInstance` runs Foreground with a `ForegroundTaskDialog`. `runForegroundRender` becomes that override's body.
  - verify: a gtest calls `renderForeground` on a background app and gets every frame written before it returns.
  - size: M

- [x] M177.P2.T3 — Route the remaining in-process renders through it
  - files: `Engine/Node.cpp` (:6698-6710), `Engine/DiskCacheNode.cpp` (:160-171), `Engine/PyAppInstance.cpp` (:306-320), `Gui/ProgressTaskInfo.cpp` (:296-340, in-process restarts only)
  - approach: Replace each `startWritersRendering(false, …)` with `getApp()->renderForeground(works)`. DiskCache pre-render fills this process's cache, so it renders foreground behind the modal.
  - verify: `Tests/gui/m177_foreground_modal.py` (new, Xvfb). For the Render dialog, `knob.trigger()` on a Write's Render button, and `app.render`: `QApplication.activeModalWidget()` is the dialog while the render runs; a QTest key edit to an open Grade spinbox and a node-graph drag leave the knob and position unchanged; Cancel ends the task as Canceled. An edit while the viewer plays restarts the viewer render. Screenshots shared.
  - size: M

**Verification gate:** fast build passes; `m177_foreground_modal.py` passes on the phase AppImage, screenshots approved by the user.

## Phase 177.3: Background renders from the submission copy

- [x] M177.P3.T1 — Restart re-renders the submission file
  - files: `Engine/AppInstance.h`, `Engine/AppInstance.cpp` (`RenderQueueItem::savePath` :155, removal :2163-2177, child load :689), `Engine/ProcessHandler.h`, `Engine/ProcessHandler.cpp`, `Gui/ProgressTaskInfo.cpp`
  - approach: The parent owns the `RENDER_SAVE` file until the task leaves the panel. The child no longer deletes it. `restartTask` starts a new `ProcessHandler` on the same file from the last rendered frame instead of calling `startWritersRendering`.
  - verify: a gtest on `AppInstancePrivate` queue bookkeeping: a restarted item carries the original `savePath`, and that file exists until the task is removed.
  - size: M

- [x] M177.P3.T2 — The frozen-copy test
  - files: `Tests/gui/m177_background_frozen.py` (new)
  - approach: Constant → Write, 10 frames, background render. Change the Constant's colour right after submission. Pause after 3 frames, edit again, restart.
  - verify: every written frame has the colour at submission.
  - size: M

**Verification gate:** fast build passes; `m177_background_frozen.py` passes under Xvfb on the phase AppImage.

## Phase 177.4: Tracking behind the modal

- [x] M177.P4.T1 — Show the modal for every tracking run
  - files: `Gui/GuiAppInstance.cpp` or `Gui/TrackerPanel.cpp`, `Gui/MultiInstancePanel.cpp` (:1766-1767), `Gui/ForegroundTaskDialog.h`, `Gui/ForegroundTaskDialog.cpp`
  - approach: On `TrackerContext::trackingStarted` (`TrackerContext.h:415`) and the TrackerPM scheduler's signal, bind a `ForegroundTaskDialog` to the tracker node's progress task (`progressStart` at `TrackerContext.cpp:1493`) and close it on `trackingFinished`. Cancel reaches the scheduler through `progressUpdate` returning false (:1507). This covers `TrackerNodeInteract.cpp:167-275`, `TrackerNode.cpp:596` and `PyTracker.cpp:153`.
  - verify: `Tests/gui/m177_tracking_modal.py` (new). Track forward over 30 frames: the modal is active; the viewer's track and stop shortcuts sent with QTest do nothing; Cancel stops tracking, and no marker keys exist past the stop frame.
  - size: M

- [x] M177.P4.T2 — Keep the modal up through the tracker's solve
  - files: `Engine/TrackerContext.cpp`, `Engine/TrackerContextPrivate.cpp`, `Gui/GuiAppInstance.cpp`, `Tests/TrackerLayer_Test.cpp`, `Tests/gui/m177_tracking_modal.py`
  - approach: after `trackingFinished` the scheduler thread still re-slaves the markers' enabled knobs and runs `solveTransformParams()`, which writes the tracker's transform/CornerPin knobs under its own progress task. Hold the tracking modal until that solve has finished (one dialog spanning tracking and solve, without the solver's `progressStart` replacing the tracking task or its `progressEnd` closing the solver's), so no write lands after the dialog closes.
  - verify: the gtest records the transform knobs at the dialog's release and they don't change afterwards; the GUI script's "no key after stop" check also covers the transform knobs.
  - size: M

**Verification gate:** fast build passes; `m177_tracking_modal.py` passes under Xvfb, screenshots approved by the user.

## Phase 177.5: Writes during a foreground job

A script's write to a node a running foreground job reads is refused with an error. Before-frame callbacks get no frame-at-a-time ordering.

- [x] M177.P5.T1 — Refuse writes to nodes a foreground job reads
  - files: `Engine/IsolatedJob.h`, `Engine/IsolatedJob.cpp` (new), `Engine/KnobImpl.h` (entry of `setValue` :1141, `setValueAtTime` :1557), `Engine/Knob.cpp` (expression and slave entry points carrying `KnobContentWrite`), `Engine/OutputSchedulerThread.cpp`, `Engine/TrackerContext.cpp`
  - approach: An RAII `IsolatedJob` registered on the app for a Foreground render or a track holds the root's upstream closure. The job's own threads are marked thread-local: the frame runnable at `OutputSchedulerThread.cpp:2312` and the track functor. A content write to a holder in an active job's closure from an unmarked thread throws `std::runtime_error` before any state changes, which Python surfaces. `KnobContentWrite` carries no holder today, so the check needs the holder passed in.
  - verify: a gtest edits an upstream knob from a second thread during a Foreground render on a background app and catches the error, with every frame identical to a reference render; a write from the before-frame callback succeeds.
  - size: L

**Verification gate:** all phase builds and a final `tools/ci/remote/build.sh debug --tests` pass in both legs; the three m177 Xvfb tests pass on the release AppImage; `git grep -n "abortAnyEvaluation\|isRenderInSeparatedProcessEnabled" Engine Gui` is empty.

## Decisions

- 2026-10-25 — **Script writes refused; no frame-at-a-time callbacks (user):** a script's knob write to a node a running foreground render or track reads fails with an error. A Write with a before-frame callback renders like any other: a render can't rely on frames being rendered in sequence (a farm doesn't), so that ordering is not supported.
- 2026-10-25 — **Cancel keeps the dialog up; the solve is covered next (bosley-native-nodes):** Qt 6 made the old member-pointer disconnect a no-op, so Cancel hid the dialog at once; it now stays ("Stopping...") until the job stops, and Cancel aborts tracking directly. The tracker's post-run transform solve still writes after `trackingFinished`; P4.T2 extends the modal over it. A background render's project save now creates the autosave folder itself, which fixes background renders in sessions without a GUI.
- 2026-10-25 — **Foreground Cancel ends as Canceled; tracking modal needs no panel (bosley-native-nodes):** a cancelled foreground render is Canceled, not Paused, because restarting it in-process would read the live, edited project; pause and restart stay for background (separate-process) renders, which restart from their submission file. Tracking shows the modal in any GUI session, including tracks started from Python with no panel open. Pausing a background task asks the child to abort over IPC (it exits itself); the child no longer deletes its save file, the parent does when the task leaves the Progress panel.
- 2026-10-25 — **Viewer restart covered under Xvfb (bosley-native-nodes):** no gtest fixture can drive viewer playback without a UI context, so P1.T1's third case (an edit restarts a viewer render) moves to `m177_foreground_modal.py` in Phase 177.2. PrecompNode's render maps to Foreground.
- 2026-10-25 — **Elaborated (bosley-native-nodes):** five phases built on the existing foreground modal, the separate-process save at submission and the tracker's cancellable progress task. Isolation means no write reaches a running non-viewer render; knob values are not snapshotted. The colour sampler already runs synchronously on the main thread, so it gets no modal; DiskCache pre-render runs foreground behind the modal. Phase 177.5 waits on the user's answers about script writes and before-frame callbacks.
- 2026-10-24 — **Created at the user's request (user):** foreground renders block the UI; background renders work from a frozen copy captured at submission; tracker and colour sampler block the UI like a foreground render; only the viewer restarts on a knob change. Kept out of M167 at the user's request.
