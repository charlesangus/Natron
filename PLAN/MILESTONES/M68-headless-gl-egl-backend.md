# M68 - Headless GL EGL

Full title: Headless GL — EGL surfaceless/device backend

**Deferred — do not start or elaborate without an explicit user go-ahead.**
Split out of M63 (was M63.P5.T4) by user decision on 2026-10-05. GL contexts
are GLX on X11 or EGL on Wayland only, so a GPU passed into the container is
unused under Xvfb (software GLX) and `NatronRenderer`/ctest get no GL headless.

Since this was written, M20 - 3D Node Vocabulary added an EGL surfaceless context for Hydra
(`Engine/Nodes/Scene/HeadlessGLContext.{h,cpp}`), and M108 - Retire Engine GL Host Path left
GL only to the OFX GL suite and the viewer. Reuse or generalise HeadlessGLContext rather than
writing a second EGL loader; the claiming PM re-checks this brief against both (§5a).

## Phase 68.1: EGL backend

- [ ] M68.P1.T1 — EGL surfaceless/device backend for headless GL
  - files: `Engine/OSGLContext*.{h,cpp}`, `Engine/Nodes/Scene/HeadlessGLContext.*`, `Engine/GPUContextPool.*`, `Tests/OSGLContext_Test.cpp`
  - approach: add an EGL `EGL_EXT_platform_device` / `EGL_MESA_platform_surfaceless` backend (no window; Natron already renders into FBOs), selected when neither `DISPLAY` nor `WAYLAND_DISPLAY` is set, so `NatronRenderer` and ctest get real GL on a `/dev/dri` render node. M63's per-thread private contexts (`GPUContextPool::getOrCreateContextForCurrentThread`) must work unchanged on it. Needs `/dev/dri` on the Docker host (`devshell.sh --recreate` adds it when present).
  - verify: `OSGLContext_Test` passes headless on the render node; a ctest GL scheduler case (two concurrent GL tasks on private contexts, store holds no GL image) fails when GL is not loaded rather than passing vacuously (`NATRON_TEST_REQUIRE_GL=1`).
  - size: L

**Verification gate:** full ctest green headless on a render node with `NATRON_TEST_REQUIRE_GL=1`; Xvfb GUI GL check unchanged.
