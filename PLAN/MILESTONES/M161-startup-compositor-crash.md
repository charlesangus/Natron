# M161 - Startup Compositor Crash

Natron sometimes crashes at startup on NVIDIA (proprietary driver) and AMD hosts. Qt's widget compositor fails to compile its own `#version 150` vertex shader with an empty info log, all three pipeline builds fail, and Qt then segfaults in `QPlatformBackingStore::rhiFlush`. It happens with or without `~/.Natron/initGui.py`, so M158 - Node Toolbar As Menu's early return (`cc6ad6c63`) only moved the race. It stops with `QT_XCB_GL_INTEGRATION=none` (but no window appears), and after one such launch, normal launches work again for a while. Leading hypotheses:
- **H1, stale GL binding:** Natron calls raw `glXMakeCurrent` on the GUI thread behind Qt's back, so Qt's RHI compiles with no context or a dead one. On NVIDIA (no `GLX_MESA_query_renderer`) `OSGLContext_x11::getGPUInfos` leaves its probe context current and destroys it while current (`Engine/OSGLContext_x11.cpp:912-957`); it runs on the GUI thread from the `AboutWindow` built in `Gui::setupUi` (`Gui/Gui05.cpp:145`, `Gui/AboutWindow.cpp:443,578-580`). Other raw switches: `unsetCurrentContextNoRender` (`Engine/OSGLContext.cpp:390-400`), `~OSGLContext` (`:298-308`), `EffectInstance::dettachAllOpenGLContexts`.
- **H2, persisted cache:** fits "one no-GL launch repairs it". Qt's program-binary cache lives under `~/.cache/INRIA/Natron`, shared by every fork build and stock Natron (`Engine/AppManager.cpp:2626-2638`, `Engine/StandardPaths.cpp:323`); the driver caches (`~/.cache/nvidia/GLCache`) are shared by GL and Natron's Vulkan compute, which keeps its own pipeline cache (`Engine/Gpu/GpuPipelineCache.cpp:65`).
- **H3, startup event pumps:** the splash still pumps events after `show()` (`Gui/GuiAppInstance.cpp:291,358,368`).
- **H4, driver init race:** Vulkan compute initialises on its own thread (`Engine/GpuContext.cpp:131`) while Qt creates its GLX context.

`NATRON_DISABLE_GPU` does not exist in the code; `NATRON_GPU=off` stops Vulkan compute and GL-suite rendering but not the GLX probe or the main-thread Vulkan enumeration (`Engine/AppManager.cpp:350,747,754,763`).

## Scope

**In:**
- GL-context and cache diagnostics that run from an AppImage on the user's GPU host, a root-cause fix, an Xvfb regression test, and the user's confirmation on NVIDIA.

**Out:**
- Wayland/EGL beyond keeping changes symmetric; other platforms (the fork is Linux x86-64 only); redesigning GPU compute. (none)

**Edge cases:**
- NVIDIA without `GLX_MESA_query_renderer`; several renderers under Mesa; probe early returns (`checkOpenGLVersion` throws); a cache written by another Qt build or driver; launches with and without an initGui.py.

**Assumptions:**
- CI has only llvmpipe, so the regression test checks the context invariant, not the NVIDIA symptom.
- One user run of P1.T2's script ranks H1–H4.
- Unconfirmed: Qt 6.8's RHI skips `makeCurrent` when its stored context matches (`ensureContext`); P1.T1's MISMATCH check settles it.

## Phase 161.1: Isolate

- [ ] M161.P1.T1 — Env-gated GL startup diagnostics
  - files: `Engine/StartupGLDiag.h`, `Engine/StartupGLDiag.cpp` (new), `Engine/OSGLContext_x11.cpp`, `Engine/OSGLContext.cpp`, `Gui/GuiApplicationManager.cpp`
  - approach: only when `NATRON_DIAG_GL=1`: log every `makeContextCurrent` with thread, caller tag and the before/after `glXGetCurrentContext`/drawable; `checkpoint(name)` logs Qt's `QOpenGLContext::currentContext()` native GLX handle against `glXGetCurrentContext()`, flagging MISMATCH (Gui registers the hook so Engine needs no QtGui); a `qInstallMessageHandler` that, on "Failed to compile shader" or "Failed to build graphics pipeline", dumps the contexts, `glGetError`, `GL_RENDERER` and listings with mtimes of `~/.cache/INRIA/Natron/qtshadercache-*`, `~/.cache/nvidia/GLCache`, `~/.cache/mesa_shader_cache` and Natron's GPU pipeline cache. First read Qt 6.8's `QRhiGles2::ensureContext` and record whether it trusts the cached context.
  - verify: syntax-check; one Xvfb launch prints the checkpoints with no MISMATCH; with the variable unset the output is unchanged.
  - size: M

- [ ] M161.P1.T2 — Startup checkpoints, isolation switches and the host script
  - files: `Gui/GuiAppInstance.cpp`, `Engine/AppManager.cpp`, `Gui/AboutWindow.cpp`, `Engine/GpuContext.cpp`, `tools/diag/startup-gl-matrix.sh` (new)
  - approach: checkpoints around `createGui`, `runInitGuiScripts`, `show`, each `setLoadingStatus` after show, and the first window expose. Switches: `NATRON_DIAG_SKIP_GL_PROBE` (the `:747` probe and the About-window probe), `NATRON_DIAG_SKIP_VK_ENUM` (`:754`), `NATRON_DIAG_GPU_INIT_SYNC` (join the Vulkan init before the GUI is created), `NATRON_DIAG_NO_SPLASH_PUMP`, `NATRON_DIAG_RESTORE_GL` (save/restore the GLX binding around every Natron switch, the H1 candidate fix). The script snapshots the caches and QSettings, then runs about 5 launches per config — baseline, each switch, `NATRON_GPU=off`, `QT_DISABLE_SHADER_DISK_CACHE=1`, `__GL_SHADER_DISK_CACHE=0`, a fresh `XDG_CACHE_HOME` — with `QT_LOGGING_RULES="qt.rhi.*=true;qt.opengl.diskcache=true"`, prints a crash/clean table, diffs the snapshots and writes one tarball.
  - verify: the script runs end to end under Xvfb against an AppImage.
  - size: M

- [ ] M161.P1.T3 — Diagnostic AppImage; the user runs the matrix once on the NVIDIA host
  - files: none (`tools/ci/local/package.sh` or the remote AppImage)
  - approach: ranking — a MISMATCH at the failure, or `RESTORE_GL`/`SKIP_GL_PROBE` curing it → H1; only the cache configs curing it, or cache changes in the diff → H2; `GPU_INIT_SYNC` or `NATRON_GPU=off` curing it → H4; `NO_SPLASH_PUMP` curing it → H3.
  - verify: the result and log excerpts are recorded in ## Decisions; the user agrees with the diagnosis.
  - size: S

**Phase gate:** the user's tarball points to one cause before Phase 161.2 starts; re-plan Phase 161.2 if it is not H1 or H2.

## Phase 161.2: Fix and guard

- [ ] M161.P2.T1 — Root-cause fix
  - files: `Engine/OSGLContext_x11.cpp`, `Engine/OSGLContext_wayland.cpp`, `Engine/OSGLContext.cpp`, `Gui/AboutWindow.cpp`, `Engine/AppManagerPrivate.cpp`
  - approach: for H1 — a scoped save/restore of the current GLX/EGL context and drawable around every probe and every no-render make-current, early returns included; unset the probe context before destroying it; the About window and the requirements text read AppManager's cached renderer list instead of probing again. For H2 — give Qt's shader cache its own versioned directory per build, or set `Qt::AA_DisableShaderDiskCache` before the application object exists. Remove the diagnostic switches the fix makes redundant; keep the `NATRON_DIAG_GL` logging.
  - verify: syntax-check; the matrix with the fix is clean under Xvfb.
  - size: M

- [ ] M161.P2.T2 — Xvfb regression test
  - files: `Tests/gui/m161_startup_compositor.py` (new), a fixture initGui.py beside it modelled on the user's (`build/m161/user-initGui.py`: adds a QAction under the Merge submenu of `natron.nodesMenu()`), `Tests/gui/run-gui-test.sh` if it needs a repeat option
  - approach: launch with `NATRON_DIAG_GL=1` 10 times with the initGui.py and 10 times without; assert no compositor or shader failure line, no MISMATCH at any checkpoint, and startup completes (window shown, then a clean quit). Pre-seed `checkForUpdates=false`.
  - verify: 20/20 under Xvfb; with the fix reverted it reports MISMATCH (H1) or the cache check fails (H2), proving the test catches the bug on llvmpipe.
  - size: M

## Phase 161.3: Confirm

- [ ] M161.P3.T1 — Release AppImage for the user on NVIDIA
  - files: none
  - approach: package the release build; the user launches it 10 times with their initGui.py and 10 without, against their existing caches with no cleanup, and sends `NATRON_DIAG_GL=1` logs from any failure.
  - verify: the user reports 20/20 clean launches.
  - size: S

**Verification gate:** fast build and full ctest green; the M161 GUI test passes 20/20; the M158 GUI scripts (`m158_init_gui`, `m158_nodes_menu`) still pass; PR CI green; the user signs off on the NVIDIA host.

## Decisions

- 2026-10-24 — **Created by bosley-native-nodes at the user's request** after the M16 - Project Format Redesign UAT AppImage crashed at startup on the user's NVIDIA host with M158's fix in place. Fix the root cause, not another startup early return. User data: removing initGui.py does not help; `QT_XCB_GL_INTEGRATION=none` stops the crash (no window) and one such launch makes later normal launches work; `NATRON_DISABLE_GPU` had no effect (it does not exist).
