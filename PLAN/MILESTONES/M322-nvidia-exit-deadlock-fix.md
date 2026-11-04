# M322 - NVIDIA Exit Deadlock Fix

Natron.bin sometimes never exits after its main window closes. It has no window, but it keeps its RAM and VRAM and ignores SIGTERM. On the user's NVIDIA host (KDE Plasma X11, driver 580.178.04, 8 GB) about 20 of these piled up over 1.5 days. One held 4.2 GB of VRAM, and once the GPU was full plasmashell segfaulted in libnvidia-glcore on every start. The user investigated on 2026-11-03; notes and two gdb dumps are in `.local/nvidia-exit-deadlock/` on bosley-faux (README.md, hang-379326.txt, hang-421817.txt).

Diagnosis: `main()` has already returned (Qt's `QThreadStorage: entry N destroyed before end of thread` is logged). The main thread is in `_dl_fini`, running libGLX_nvidia's library destructor, which takes a glcore mutex and then joins the driver's own `CPMMListener` thread. That thread is blocked on the same mutex, whose owner word is the main thread's TID in both dumps. So this is a self-deadlock inside the driver, and it happens only when CPMMListener is active at the moment of exit. Natron's threads are idle and hold no locks. The SIGTERM handler (`Engine/AppManager.cpp` `handleShutDownSignal`) calls `appPTR->quitApplication()` from signal context: by then the process is past `main`, so it prints "Caught termination signal, exiting!" and nothing else happens.

The fix: once Natron's own teardown has finished, leave with `_exit` so library destructors never run. Make the termination signals async-signal-safe. Add a watchdog so a hang during teardown can't leak a process.

Deps: none. The user exempted it from the "start nothing new" rule (2026-11-03).

## Scope

**In:**
- Both entry points, `App/NatronApp_main.cpp` (GUI and background branches) and `Renderer/NatronRenderer_main.cpp`: run the manager in an inner scope, do the explicit flushes, then `_exit(rc)`.
- Running explicitly, before `_exit`, every needed piece of work that currently runs from `atexit` or a static destructor. Known ones: `profileSink()`'s atexit flush (`Engine/RenderScheduler.cpp`) and `Singleton<T>::ScheduleForDestruction` for `Log`, which closes the log file.
- SIGINT/SIGTERM handling that is async-signal-safe and works during teardown and after `main` returns.
- A shutdown watchdog.

**Out:**
- Explicitly destroying GL contexts and widgets before teardown. The user declined it (2026-11-03) because it doesn't remove the race.
- Drafting the NVIDIA driver bug report. The user declined it.
- `Tests/wmain.cpp` (gtest). It runs on llvmpipe and has never hung.
- The SIGSEGV/SIGFPE backtrace handler, which already avoids `exit()`.

**Edge cases:**
- `std::exit` called before the manager is destroyed (the three `std::exit(1)` calls in `setShutDownSignal`/`setSigSegvSignal`, and any `QCoreApplication::exit` path) still reaches `_dl_fini`. The audit lists them, and they only need fixing if they can run once GL is loaded.
- A second SIGTERM/SIGINT during a slow teardown leaves immediately with `_exit(128 + sig)`.
- A signal before the event loop runs, or in background mode with no event loop, must still end the process.
- Exit codes are unchanged. A failed background render still returns non-zero.

**Assumptions:**
- `_dl_fini` (registered by ld.so before `main`) runs after every `atexit`/`__cxa_atexit` handler, so `_exit` right after teardown skips only work that P1.T1 accounts for.
- Natron flushes and closes everything it writes (project autosave, QSettings, caches) during manager destruction, not afterwards. P1.T1 confirms this.

## Phase 322.1: Leave Without Library Destructors

- [x] M322.P1.T1 — Audit what runs after the manager is destroyed (128e805f2; fixes 0afb29e86, be0cb8ce8; gate green)
  - files: App/NatronApp_main.cpp, Renderer/NatronRenderer_main.cpp, Engine/AppManager.cpp, Engine/Singleton.h, Engine/RenderScheduler.cpp
  - approach: Read only; write the findings into this milestone's `## Decisions`. List every `atexit`/`std::atexit` registration, `Singleton<T>` user, function-local or global static whose destructor writes a file or flushes data (`static QSettings`, ofstream/QFile logs, the profile sink, caches), where Python finalizes relative to the manager's destruction (`tearDownPython` in `~AppManager`), and every `std::exit`/`exit`/`QCoreApplication::exit` reachable after GL is loaded. For each, record whether it has to run before `_exit` and how T2 will run it explicitly. Search Engine/, Gui/, App/ and Renderer/, and also libs/ code built into Natron.bin.
  - verify: the Decisions entry lists each item with a file:line and a verdict, and nothing is left marked "unknown".
  - size: M
- [x] M322.P1.T2 — `_exit` after teardown in both entry points (128e805f2; fixes 0afb29e86, be0cb8ce8; gate green)
  - files: App/NatronApp_main.cpp, Renderer/NatronRenderer_main.cpp, Engine/AppManager.h, Engine/AppManager.cpp
  - approach: Put each manager (`GuiApplicationManager`, the background `AppManager`, NatronRenderer's `AppManager`) in an inner scope that yields `rc`. After that scope, call one shared helper, e.g. `AppManager::exitWithoutLibraryDestructors(int rc)`, declared `[[noreturn]]`. It runs the work T1 marked as needed (at least `profileSink().flush()` and `Log` destruction), then `std::fflush(nullptr)`, flushes `std::cout`/`std::cerr`, and calls `_exit(rc)`. Keep the early `return error.value()` paths as they are, because nothing GL is loaded at that point. Add a one-line comment naming the libGLX_nvidia `_dl_fini` deadlock. Return codes must not change.
  - verify: NatronRenderer renders a small project and returns 0, and a failing render still returns non-zero. With `NATRON_PROFILE` (or whatever the sink's env var is), the profile file is complete. The GUI under Xvfb quits via File > Quit with exit code 0 and a complete log file. `ltrace`/gdb, or a breakpoint on `_dl_fini`, shows it is never reached.
  - size: M

## Phase 322.2: Signals And Watchdog

- [x] M322.P2.T1 — Async-signal-safe SIGINT/SIGTERM (128e805f2; fixes 0afb29e86, be0cb8ce8; gate green)
  - files: Engine/AppManager.cpp, Engine/AppManagerPrivate.h
  - approach: The handler only does async-signal-safe work. On the first signal it writes the signal number to a self-pipe (created in `setShutDownSignal`, `O_NONBLOCK|O_CLOEXEC`), or calls `_exit(128 + sig)` straight away if a "past teardown" atomic flag is set. On a second signal it calls `_exit(128 + sig)`. A small reader, either a `QSocketNotifier` on the pipe's read end owned by the manager or a dedicated thread for when there is no event loop (background mode), prints the "Caught termination signal" line and requests the quit on the main thread (`QMetaObject::invokeMethod(..., Qt::QueuedConnection)` → `quitApplication()`). Set the flag at the start of manager destruction and arm the M322.P2.T2 watchdog there, so a signal during teardown or after `main` ends the process. `appPTR` is never touched from signal context.
  - verify: Under Xvfb, launch the GUI with a project open, send SIGTERM, and it exits in under 10 s. Send SIGTERM to a background NatronRenderer render and it exits non-zero. Send SIGTERM twice in quick succession and it exits immediately.
  - size: M
- [x] M322.P2.T2 — Shutdown watchdog (128e805f2; fixes 0afb29e86, be0cb8ce8; gate green)
  - files: Engine/AppManager.cpp, Engine/AppManager.h, Engine/AppManagerPrivate.h
  - approach: When teardown starts (the quit request is accepted, or manager destruction begins, whichever comes first), start a detached `std::thread` that waits on a condition variable with a timeout, 30 s by default, overridable by an env var for tests. If the timeout expires, it writes one line to stderr naming the stall and calls `_exit(124)`. `exitWithoutLibraryDestructors` never needs to disarm it, because `_exit` ends the thread. If the user cancels the quit (a save prompt answered Cancel), disarm it.
  - verify: with the env var set to 2 s and an artificial teardown stall (a debug-only env hook that sleeps in `~AppManager`), the process exits with 124 after about 2 s. With no stall, a normal quit exits 0 and never prints the watchdog line. Cancelling a quit at the save prompt doesn't kill the app 30 s later.
  - size: M

## Phase 322.3: Prove It

- [x] M322.P3.T1 — Xvfb exit regression script (128e805f2; fixes 0afb29e86, be0cb8ce8; gate green)
  - files: Tests/gui/ (a new exit-behaviour script beside the existing GUI checks), the GUI-check runner list that the remote `--gui` mode reads
  - approach: A GUI script that launches Natron under Xvfb with a viewer showing a rendered image, quits via the menu, and asserts exit code 0 within 15 s. A second case sends SIGTERM and asserts exit within 15 s with the code P2.T1 defines. Follow the existing GUI-check conventions, including pre-seeding checkForUpdates=false.
  - verify: passes locally under Xvfb and through `tools/ci/remote/build.sh --gui`, and fails against a build without P2.T1, because there SIGTERM never ends the process after `main` returns.
  - size: M

**Verification gate:** CI green, and the P3.T1 exit script passes remotely. A release UAT AppImage plus steps .md goes to the user. On their NVIDIA host they open and close it at least 30 times, some with a viewer actively rendering, and also SIGTERM a few instances; afterwards `pgrep -fa '[N]atron.bin'` and `nvidia-smi` show no leftover Natron processes. The user approves the UAT.
- 2026-11-03 — P1.T1 audit (c30dff64c lines): Singleton Log atexit (Singleton.h:110, Log.cpp) inert unless NATRON_LOG, helper destroys it under that define; profile sink atexit (RenderScheduler.cpp:269-281) would lose data → helper calls RenderScheduler::flushProfile(); AbortWatchdog (AbortWatchdog.cpp:84-95) stopped in ~AppManager; FileStampRegistry.cpp:162, GpuTransferModel.cpp:53 memory only; GpuPipelineCache.cpp:424 and OFX cache (OfxHost.cpp:960) write at creation/load, nothing at exit; ~OfxHost/clearPluginCache (OfxHost.cpp:188-196) inside manager destruction; StartupGLDiag.cpp:194-205,500 flushes per line; QSettings all locals; Python finalises in ~AppManager (tearDownPython, AppManager.cpp:3394); stdio flushed by the helper. exit() reachable with GL loaded: Python sys.exit via PyErr_Print/PyRun_SimpleString (AppManager.cpp:1804,1822,3911,3996,4028,4048), Qt xcb on lost X connection, glog/gflags fatal paths → covered by a glibc on_exit guard active while a manager exists; std::exit at AppManager.cpp:189,290,294 replaced by the helper; CLArgs early returns are before GL.
- 2026-11-03 — PM: a signal-initiated quit exits 128+sig (143 for SIGTERM), normal quits keep their codes. Env vars: NATRON_SHUTDOWN_WATCHDOG_SECONDS (default 30, 0 off, exit 124), NATRON_DEBUG_SHUTDOWN_STALL_SECONDS (debug builds). One detached signal-reader thread serves GUI and background modes.
- 2026-11-04 — PR #187. Gate fixes: SIGTERM quits like File > Quit (stale TimeLineGui viewer pointer and a playback-scheduler deadlock on the old path). Codex review fixes (be0cb8ce8): teardown check before touching the manager, idempotent requests that wait out modal dialogs and main-thread saves, dismissed save question answers Escape, tighter double-SIGTERM check and a save-question case. Release gate and exit check green (run 41791167886). UAT AppImage: build/appimages/Natron-m322-nvidia-exit-deadlock-fix.AppImage.
