# M174 - Wayland Support

Stub. Make Natron run natively on Wayland on Linux. Today the fork targets X11: Qt runs on the xcb platform, the engine's own GL contexts use GLX (`Engine/OSGLContext_x11.cpp`), and the EGL/Wayland path (`Engine/OSGLContext_wayland.cpp`) compiles only under `__NATRON_WAYLAND__`, which nothing in the build defines; the natron-dev image's EGL headers are also too old to build it. M161 - Startup Compositor Crash kept its GL binding save/restore symmetric in the EGL file, but that code has never been compiled.

Blocked on the user's approval to start (user, 2026-10-24). Elaborate into phases when approved.

## Scope

**In (to confirm at elaboration):**
- A build option that compiles the EGL/Wayland GL context path, with headers from the natron-dev image, and CI that builds it.
- Runtime selection between GLX (xcb) and EGL (wayland) to match Qt's platform plugin, including the startup GPU probe and the GL binding save/restore.
- Running the GUI on `QT_QPA_PLATFORM=wayland`: viewer GL widgets, the compositor, screen and DPI handling, cursor warping (used by the trackball and slider drags, which Wayland restricts), window placement (splash, Preferences, floating panes), and clipboard/drag-and-drop.
- Packaging: the AppImage ships Qt's Wayland platform plugin and its dependencies.
- An Xvfb-equivalent headless Wayland test path (e.g. a headless compositor in natron-dev) for the GUI test scripts.

**Out:**
- Other platforms (the fork is Linux x86-64 only).

**Edge cases:**
- XWayland: decide whether X11 under XWayland stays the default and Wayland is opt-in.
- NVIDIA proprietary EGL vs Mesa.

**Assumptions:**
- Unconfirmed: Vulkan compute is unaffected by the windowing system.

## Decisions

- 2026-10-24 — **Created at the user's request (user):** a milestone for Wayland support, blocked until the user approves starting it. Filed from M161's review, which found the EGL/Wayland GL path is dead code (never compiled).
