## Pending

### 2026-10-10T23:40:13-04:00 — change-request
- refs: M94
- Add a new milestone, M94 - Delete Unused Maintainer Skill, with board row
  `| M94 | Delete Unused Maintainer Skill | todo | - | [M94-delete-unused-maintainer-skill.md](PLAN/MILESTONES/M94-delete-unused-maintainer-skill.md) |`.
  User request: delete `skills/` entirely — they noticed it had gone stale
  again and nothing in the repo actually consumes it. Full milestone-file
  content to write to `PLAN/MILESTONES/M94-delete-unused-maintainer-skill.md`:

  ```markdown
  # M94 - Delete Unused Maintainer Skill

  `skills/natron-maintainer/SKILL.md` is the only file in `skills/`. M12.P1.T1
  (Documentation cruft removal) deliberately fixed this file rather than
  deleting it, on the theory that it's "agent-facing" guidance worth keeping
  accurate. Reassessed at the user's request: nothing in this repo wires it up
  for automatic discovery by an agent session — there is no `CLAUDE.md` and no
  `.claude/` directory, and it did not surface in a live session's
  available-skills listing — so its only path to being read is a human
  manually pointing an agent at it. The user has also found at least one place
  where it has gone stale again. Given no automated consumer and a repeat
  drift problem, delete it rather than maintain it.

  ## Phase 94.1: Remove the directory

  - [ ] M94.P1.T1 — Delete the skills/ directory
    - files: `skills/natron-maintainer/SKILL.md` (and the now-empty `skills/`
      directory)
    - approach: `git rm -r skills/`. Before removing, grep the tracked tree
      for any reference to `skills/natron-maintainer` or `skills/` outside
      `.plan/` and `docs/decisions/` (the decision archive is allowed to cite
      deleted paths per the M12.P2.T1 precedent — it's a dated record, not a
      live reference) and confirm there is none. Record this as a
      milestone-scoped decision: it reverses M12.P1.T1's "fix, don't delete"
      call, because the premise behind keeping it — that it's read as live
      agent instruction — turned out not to hold in this repo (no
      `CLAUDE.md`, no `.claude/`, no auto-discovery).
    - verify: `test ! -e skills` (directory is gone); `git status` clean;
      `grep -rn "skills/natron-maintainer" --exclude-dir=.git --exclude-dir=.plan .`
      returns nothing.
    - size: S

  **Verification gate:** `skills/` no longer exists in the tree; no tracked
  file outside `.plan/` and `docs/decisions/` references
  `skills/natron-maintainer`; a clean build is unaffected (no build input
  depended on it).
  ```

### 2026-10-11T00:00:11-04:00 — change-request
- refs: M85, M86, M68
- User request (2026-10-11): fold the **retirement of the engine-side GL render path** into
  M85 - Native GPU Kernels and M86 - OFX 1.5 GPU Suites, so Vulkan compute becomes the only
  engine GPU path and GL survives in the engine solely as a context for OFX OpenGL-suite
  plugins. The viewer (`Gui/ViewerGL*`, overlays) and the 2D GL widgets (curve editor, dope
  sheet, timeline, histogram) are **out of scope** and stay on GL.

  Scouted on `main`-based code: native nodes never render through GL
  (`EffectInstance::supportsOpenGLRender` defaults to None). The engine GL path exists for the
  OFX OpenGL render suite (`OfxHost.cpp:307`) and comprises:
  - GL texture image storage and its host-side GL wrappers: `eStorageModeGLTex` paths in
    `Engine/Image.cpp`, `ImageConvert.cpp`, `ImageCopyChannels.cpp`, `ImageMaskMix.cpp`,
    `ImagePremult.cpp`, with `Engine/GLShader.*` and `Engine/Texture.cpp`.
  - Per-render-thread contexts: `Engine/OSGLContext*` (x11, wayland, xdg), `GPUContextPool.*`,
    and the plumbing in `RenderScheduler.cpp`, `FrameRenderContext.cpp`,
    `ParallelRenderArgs.*`, `EffectInstanceRenderRoI.cpp`, `EffectInstanceRenderDeep.cpp`,
    `OfxClipInstance.cpp`, `CacheEntry.h`.
  - Startup GL probing and settings: `AppManager*.cpp` renderer probe, `Settings.cpp`
    `_enableOpenGL` / `_availableOpenGLRenderers`, `Gui/AboutWindow.cpp`.
  - Tests: `Tests/GLScheduler_Test.cpp`, `Tests/OSGLContext_Test.cpp`.
  (`RotoPaint*`, `TrackerNode*` GL calls are overlay drawing — viewer side, out of scope.)

  **Edit `PLAN/MILESTONES/M85-native-gpu-kernels.md`** (stays a stub; still blocked on
  M84 - GPU Placement And Residency). Append to the intent bullets:

  ```markdown
  - **Retire the engine GL render path for host work** (2026-10-11, user). The host's GL
    wrappers around plugin renders — mask/mix, copy-channels, premult and storage conversion
    on `eStorageModeGLTex` images (`ImageMaskMix.cpp`, `ImageCopyChannels.cpp`,
    `ImagePremult.cpp`, `ImageConvert.cpp`, GL paths in `Image.cpp`, `GLShader.*`) — are
    replaced by the backend-neutral kernels above running on Vulkan. GPU device selection
    and the on/off switch in `Settings.cpp` move from the GL renderer list to the Vulkan
    device list. After M85 nothing native or host-side renders through GL; the remaining GL
    engine code serves only the OFX OpenGL suite and is narrowed by M86.
  ```

  and append to its acceptance sketch:

  ```markdown
  - No host-side image operation renders through GL: the `eStorageModeGLTex` mask/mix,
    copy-channels, premult and convert paths are gone or reduced to the M86 interop boundary.
  - The Preferences GPU setting lists Vulkan devices, not GL renderers.
  ```

  **Edit `PLAN/MILESTONES/M86-ofx-gpu-suites.md`** (stays a stub; still blocked on
  M84 - GPU Placement And Residency). Replace the bullet "Keep the existing OpenGL suite
  working through GL/Vulkan interop for Shadertoy and the OCIO plugins." with:

  ```markdown
  - Keep the existing OpenGL suite working for Shadertoy and the OCIO plugins, as the **only
    remaining engine GL path** (2026-10-11, user). A GL-suite plugin's inputs and outputs
    cross a GL/Vulkan interop boundary at the plugin call (exported Vulkan memory imported
    as GL textures or PBOs, ordered by shared semaphores with the hostsync fallback from
    M83). Shrink `OSGLContext*` and `GPUContextPool` to lazily created contexts used only for
    those plugins — none are created when a project has no GL-suite plugin — and delete the
    GL scheduling plumbing in `RenderScheduler.cpp`, `FrameRenderContext.cpp` and
    `ParallelRenderArgs.*` that no longer has a consumer. Rework or retire
    `Tests/GLScheduler_Test.cpp` and `Tests/OSGLContext_Test.cpp` to match. M68 - Headless
    GL EGL then reduces to giving this narrowed context an EGL surfaceless backend.
  ```

  and append to its acceptance sketch:

  ```markdown
  - With no GL-suite plugin in the project, a render creates no GL context.
  - GL is used in the engine only at the OFX OpenGL-suite boundary; everything else on the
    GPU runs on Vulkan.
  ```

  **Record a project-wide decision** `PLAN/DECISIONS/2026-10-11-retire-engine-gl-render-path.md`
  (+ INDEX line): Vulkan compute is the only engine GPU path; the engine's GL render path is
  retired across M85 - Native GPU Kernels (host wrappers, settings) and M86 - OFX 1.5 GPU
  Suites (contexts narrowed to the OFX OpenGL suite behind interop). The viewer and the 2D GL
  widgets stay on GL: OFX overlay interacts require a GL context, Qt 6 allows one graphics
  API per top-level window so the viewer can't move alone, and the M83 interop hand-off
  already costs 0.79 ms at UHD. A later viewer modernisation should go through `QRhiWidget`
  on its OpenGL backend first; the 2D widgets, if reworked, go to `QPainter`, not Vulkan.

### 2026-10-11T00:04:16-04:00 — change-request
- refs: M69
- User request (2026-10-11): in **M69 - Minor Cleanup** (stays a stub), add a new "Panels:" intent
  group, placed after "Node graph interaction" and before "Roto". Right-clicking the empty space
  beside a panel's tabs must bring up the same pane menu as the panel management icon (the
  top-left corner button of a pane). Append this to the stub body:

  ```markdown
  Panels:
  - **Right-click the empty header space for the pane menu.** Right-clicking the empty space to
    the right of a pane's tabs opens the same menu as the panel management icon at the pane's
    top-left (split vertical/horizontal, float, close, hide toolbar/tabs header, "... here"
    entries, set as anchor). Scouted 2026-10-11: that menu is `TabWidget::createMenu()`
    (`Gui/TabWidget.cpp`), connected to `leftCornerButton`'s `clicked()` and shown with
    `menu.exec(leftCornerButton->mapToGlobal(QPoint(0, 0)))`. The empty space is the stretch in
    the pane's header row, which belongs to `TabWidgetHeader` (`Gui/TabWidget.h`), a bare
    `QWidget` that handles only `leaveEvent`. Approach: give `TabWidgetHeader` a
    `contextMenuEvent` (or a `mousePressEvent` for the right button) that emits a signal
    connected to a `TabWidget` slot; refactor `createMenu()` to take the global position to
    open at (the icon passes its own corner, the header passes the cursor) so the menu
    opens at the right place the first time. Keep the icon's behaviour unchanged, and don't
    swallow right-clicks on the tabs themselves (`TabBar`) or the float/close buttons. Fold in
    the "Tab menu position" fix from the node-graph group if the same positioning bug turns out
    to be shared.
  ```

  and append to its acceptance sketch:

  ```markdown
  - Right-clicking the empty space beside a pane's tabs opens the same menu as the panel
    management icon, at the cursor.
  ```
