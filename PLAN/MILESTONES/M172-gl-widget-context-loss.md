# M172 - GL Widget Context Loss

When a `QOpenGLWidget` moves to another top-level window (a pane floated out or docked back, a properties panel floated), Qt 6 destroys its GL context and calls `initializeGL()` again in a new one. M138 - GUI Papercuts fixed this for `ViewerGL` only: `releaseViewerGLResources` (`Gui/ViewerGLPrivate.cpp:78-123`), hooked to `aboutToBeDestroyed` in `initializeGL` (:484) and unhooked in `~Implementation` (:212-222). Every other GL widget in `Gui/` still keeps context-bound names after its context dies. Two kinds of state are affected. The first is `TextRenderer` atlases (`Gui/Histogram.cpp:201`, `Gui/DopeSheetView.cpp:291`, `Gui/CurveWidgetPrivate.h:158`, `Gui/TimeLineGui.cpp:161`, `Gui/HostOverlay.cpp:1015`). The second is each widget's own GL objects: Histogram's shaders, textures, FBOs, VAO and VBOs (`Gui/Histogram.cpp:214-225`, :428-440) and DopeSheet's keyframe textures (`Gui/DopeSheetView.cpp:355`, :804-805). With stale names, text draws blank or samples a foreign texture (`Gui/TextRenderer.cpp:~323`), glyph uploads write into an unrelated texture (~:196-198), and destructors delete names in whatever context is current (~:118, `Gui/Histogram.cpp:421-440`). `HostOverlay` is a separate problem: it is not a widget. There is one per `NodeGui` (`Gui/NodeGui.cpp:4005`) and it draws into every viewer showing that node, so a single `TextRenderer` is shared across several live contexts even when nothing is re-parented. This milestone adds one shared release mechanism, applies it to every GL widget, and proves the fix with a float-out-and-back GUI script. Filed 2026-10-24 from M138 - GUI Papercuts' inbox report.

## Scope

**In:**
- A reusable context-loss helper, with `ViewerGL` moved onto it so that only one pattern exists.
- A `TextRenderer` that keeps per-context state and releases it when that context dies.
- Every `QOpenGLWidget` subclass in `Gui/` on `origin/main`: `ViewerGL`, `Histogram`, `CurveWidget`, `DopeSheetView`, `TimeLineGui`, `CustomParamInteract`. A grep of `origin/main` for `QOpenGLWidget` in `Gui/*.h` finds no others.
- `HostOverlay`'s text drawn in more than one viewer.
- A GUI script under Xvfb that floats each pane out and back and checks text pixels and survival.

**Out:**
- GL objects created by OFX plugin interacts inside `CustomParamInteract` or viewer overlays. They belong to the plugin, and the host cannot release them.
- Switching to shared contexts (`Qt::AA_ShareOpenGLContexts`). `origin/main` does not set it, and this milestone does not change that.

**Edge cases:**
- A widget destroyed before `initializeGL` ran, which has no context: release must be a no-op.
- A context destroyed while another widget's context is current: the handler must make its own widget current first and restore the previous context afterwards.
- `DopeSheetView` rebuilding textures on a DPR change (`kfTexturesScreenPixelRatio`, :806) as well as in `initializeGL`.
- Floating twice in a row without docking back in between.
- Closing a floating window that holds a GL widget.

**Assumptions:**
- As the M138 comment at `Gui/ViewerGLPrivate.cpp:483` says, `QOpenGLWidget::makeCurrent()` still works on the dying context while `aboutToBeDestroyed` is emitted, as long as the connection is direct.
- `PyTabWidget::floatCurrentTab()`/`floatPane()` (`Gui/PythonPanels.cpp:~678-700`) and `PyGuiApp::moveTab` (`Gui/PyGuiApp.h:136`) are enough to float out and dock back from a script; unconfirmed — if `moveTab` doesn't dock back, closing the `FloatingWidget` may be the way. `DockablePanel::floatPanel()` (`Gui/DockablePanel.cpp:1238`) covers properties panels.
- Unconfirmed: whether the GuiTests offscreen QPA can create a GL context (`run-gl-tests.sh` defaults to the `Tests` binary). If it can't, P1.T2's gtest moves into the `Tests` binary or is checked through the GUI script.

## Phase 172.1: Shared mechanism

- [ ] M172.P1.T1 — Add a GL context-loss helper and move ViewerGL onto it
  - files: `Gui/GLContextLoss.h`, `Gui/GLContextLoss.cpp` (new), `Gui/CMakeLists.txt`, `Gui/ViewerGLPrivate.cpp`
  - approach: `GLContextLoss::watch(QOpenGLWidget*, std::function<void()> release)` is called from `initializeGL`. It replaces any earlier connection on that widget, connects `context()->aboutToBeDestroyed` with `Qt::DirectConnection` and a receiver of the widget, makes the widget current, runs `release`, and then restores the previous current context and surface. `GLContextLoss::releaseNow(QOpenGLWidget*)` is for destructors: it does nothing without a context, and otherwise disconnects, makes the widget current and runs `release`, which replaces the hand-written pattern at :212-222. Re-express ViewerGL's :484 hook and `~Implementation` through the helper and leave `releaseViewerGLResources` unchanged.
  - verify: `tools/ci/remote/build.sh fast --tests` is green. Then `Tests/gui/run-gui-test.sh Tests/gui/m138_papercuts.py <AppImage>` with `M138_SECTIONS=file-dialog`, the re-parenting case M138 fixed, still passes.
  - size: M

- [ ] M172.P1.T2 — Make TextRenderer keep its atlases per context and release them on context loss
  - files: `Gui/TextRenderer.h`, `Gui/TextRenderer.cpp`, `Tests/TextRenderer_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: Key `TextRendererPrivate` state (the used-textures list, bitmap cache, `_xOffset`/`_yOffset`) by `QOpenGLContext::currentContext()` per font. `renderText` looks up or creates the current context's entry. The first time a context is seen, the renderer connects to that context's `aboutToBeDestroyed`; the handler makes the context current through the helper's surface-restoring path, or through a `QOffscreenSurface` when no widget surface is available, then deletes that entry's textures. Add `releaseCurrentContext()`, which deletes the current context's entry with that context current. The destructor deletes only entries whose context is current. For other entries it drops the names without calling `glDelete*`, so a dead context's names never get deleted in a live one. This also makes `HostOverlay`'s single renderer safe across several viewers without changing `HostOverlay`.
  - verify: Add a GuiTests gtest that creates two `QOpenGLContext`s on a `QOffscreenSurface`, renders text in both, destroys one, renders again in the survivor and in a new third context, and checks `glIsTexture` for each entry's names. It must pass under `Tests/gui/run-gl-tests.sh "TextRenderer.*" <GuiTests binary>`, which sets `NATRON_TEST_REQUIRE_GL=1`. Also run `tools/ci/remote/build.sh fast --tests`.
  - size: M

## Phase 172.2: Apply to every GL widget

- [ ] M172.P2.T1 — Release Histogram's GL objects and text on context loss
  - files: `Gui/Histogram.cpp`
  - approach: Move the body of `~Histogram` (:421-440) into a `releaseGLResources()`. It deletes the textures, FBOs, VAO and VBOs, zeroes the names, resets the three `QOpenGLShaderProgramPtr`s (:214-225) and calls `textRenderer.releaseCurrentContext()`. Register it with `GLContextLoss::watch` in `initializeGL` (:677), and make the destructor call `GLContextLoss::releaseNow`. `initializeGL` must rebuild everything from zeroed state.
  - verify: `tools/ci/remote/build.sh fast --tests` is green. P3.T1's `histogram` section passes.
  - size: M

- [ ] M172.P2.T2 — Release DopeSheetView's keyframe textures and text on context loss
  - files: `Gui/DopeSheetView.cpp`
  - approach: Factor the `glDeleteTextures(KF_TEXTURES_COUNT, kfTexturesIDs)` calls (:355, :804) into a release step that also zeroes `kfTexturesIDs` and `kfTexturesScreenPixelRatio` and clears the text renderer for this context. Watch it from `initializeGL` (:3215) and call `releaseNow` from `~DopeSheetView` (:2622) instead of deleting in `~DopeSheetViewPrivate`. The DPR-driven regeneration path must not delete names from a previous context.
  - verify: `tools/ci/remote/build.sh fast --tests` is green. P3.T1's `dopesheet` section passes.
  - size: M

- [ ] M172.P2.T3 — Release CurveWidget, TimeLineGui and CustomParamInteract state on context loss
  - files: `Gui/CurveWidget.cpp`, `Gui/CurveWidgetPrivate.cpp`, `Gui/TimeLineGui.cpp`, `Gui/CustomParamInteract.cpp`
  - approach: These three create no GL objects of their own beyond text. The grep finds no `glGen*`; `savedTexture` is only a saved binding. Each one, in `initializeGL` (`CurveWidget.cpp:179`, `TimeLineGui.cpp:289`, `CustomParamInteract.cpp:139`), watches with a release that calls `textRenderer.releaseCurrentContext()`, and its destructor calls `releaseNow`. `CustomParamInteract` has no `TextRenderer`, but it is watched anyway so its saved bindings are reset. Re-grep `origin/main` before starting for any `glGen*`/`QOpenGLShaderProgram` added since this plan was written.
  - verify: `tools/ci/remote/build.sh fast --tests` is green. P3.T1's `curve`, `timeline` and `param-interact` sections pass.
  - size: M

## Phase 172.3: GUI verification

- [ ] M172.P3.T1 — Add a GUI script that floats every GL pane out and back and checks text
  - files: `Tests/gui/m172_gl_context_loss.py` (new)
  - approach: Follow `Tests/gui/m138_papercuts.py`: `guitest` helpers, a faulthandler watchdog, and sections selected by `M172_SECTIONS`. Build a short graph with Read→Grade→Viewer and animated keys so the Viewer, Histogram, Curve Editor, Dope Sheet and timeline all draw text. Sections: `viewer`, `timeline`, `histogram`, `curve`, `dopesheet`, `param-interact` (a node with a custom OFX param interact) and `host-overlay` (Transform shown in two viewers). Each section grabs `grabFramebuffer()` and counts theme-text pixels in the label strip; floats the pane (`floatCurrentTab()`/`floatPane()`, or `floatPanel()` for properties); grabs and counts again; docks back with `moveTab`, grabs and counts; then floats twice in a row and closes the floating window. Each step checks that the text-pixel count stays within tolerance of the baseline (blank text gives about 0) and that the log shows no GL errors. Save screenshots `m172-<section>-{docked,floated,redocked}.png`.
  - verify: Package the AppImage with `package.sh`. `Tests/gui/run-gui-test.sh Tests/gui/m172_gl_context_loss.py <AppImage>` passes every section. On `origin/main` without Phase 172.2, the same script shows blank text or crashes in at least the histogram and timeline sections, which proves it detects the bug. Screenshots go to the user for sign-off.
  - size: M

**Verification gate:** `tools/ci/remote/build.sh fast --tests` is green, including `TextRenderer.*` under `Tests/gui/run-gl-tests.sh`. A debug build (`tools/ci/remote/build.sh debug --tests`) is also green, because the `glIsTexture` asserts in `TextRenderer.cpp` only fire there. `Tests/gui/run-gui-test.sh Tests/gui/m172_gl_context_loss.py <AppImage>` passes every section on a packaged AppImage, `m138_papercuts.py` still passes, and the user has approved the screenshots. The GUI bench does not apply, because no `Gui/NodeGraph*` or `Gui/NodeGui*` file is touched; if P1.T2 turns out to need a change in `NodeGui.cpp` for `HostOverlay`, it applies again.
