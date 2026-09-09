# Milestone 25: Guard the GL init path against the debug FP traps

`App/NatronApp_main.cpp:66` arms `FE_DIVBYZERO|FE_INVALID|FE_OVERFLOW` for the
whole process as the first statement of `main()` under `-DDEBUG`, which
`CMakeLists.txt:41` adds for `CMAKE_BUILD_TYPE=Debug`. Everywhere Natron enters
third-party code it disarms them for the duration with a
`boost_adaptbx::floating_point::exception_trapping trap(0)` —
`AppManager::exec()` (`Engine/AppManager.cpp:2591`), `Node.cpp:3595`,
`Project.cpp:521`, `OfxImageEffectInstance.cpp:154`.

`AppManager::initializeOpenGLFunctionsOnce()` (`Engine/AppManager.cpp:818`) does
not, and it calls into the GL driver via `OSGLContext_x11Private::createContextGLX()`
(`Engine/OSGLContext_x11.cpp:769`). A software rasteriser raises ordinary FP edge
cases while compiling its own shaders, harmless with traps masked, fatal with them
armed. So a debug build dies of SIGFPE inside `libgallium` on any host without
hardware GL — every CI runner, and the Xvfb the release gate runs under.

Established during M23 by swapping binaries between a passing and a failing bundle
in both directions, and confirmed with an `LD_PRELOAD` shim stubbing
`feenableexcept` to a no-op, which makes the same bundle pass. Pre-existing since
upstream `300ddcbd0` (2018-04-04); not a regression. See
`DECISIONS/2026-09-09-guard-gl-init-against-fp-traps.md` and M23's `## Decisions`.

## Phase 25.1: Guard the call into the driver

- [ ] M25.P1.T1 — Wrap the GL context-creation path in the existing trap guard
  - files: `Engine/AppManager.cpp`, `Engine/OSGLContext_x11.cpp` if the right scope proves to be there
  - approach: give the GL initialisation the same `#ifdef DEBUG` /
    `boost_adaptbx::floating_point::exception_trapping trap(0)` treatment the four
    existing call sites use — copy their idiom exactly rather than inventing a
    variant. Scope it to the call into the driver, not to a whole function that also
    runs Natron's own arithmetic: the traps exist to catch Natron's bugs and must
    stay armed for Natron's own code. Determine whether the right seam is
    `initializeOpenGLFunctionsOnce()` or further down in `createContextGLX()`, and
    check whether the GL entry points are called from anywhere else that needs the
    same guard (`OSGLContext*`, the viewer's render path) rather than fixing only
    the one site that was observed to crash.
  - verify: a `Debug` build starts under Xvfb on a software-GL host and reaches its
    main window — the exact thing that fails today. Confirm the traps are still
    armed afterwards, so the guard is scoped and not a global disarm: a deliberate
    division by zero in Natron's own code must still trap.
  - size: M

- [ ] M25.P1.T2 — Prove it with a headless debug-build run
  - files: `tools/ci/local/README.md`, `tools/ci/local/test.sh` or the release check scripts as appropriate
  - approach: the reason this bug survived eight years is that nothing ever started
    a debug build headlessly. Make that a step someone runs. The Xvfb GUI probe in
    `tools/release/check-startup.sh` already does exactly this against a staged
    bundle; the cheapest honest answer is likely to point the same probe, or a
    trimmed version of it, at a `build/debug` tree. Do not weaken
    `check-startup.sh`'s existing assertions to make a build tree fit it.
  - verify: the step starts a debug build headlessly and fails if M25.P1.T1 is
    reverted.
  - size: M

**Verification gate:** `format`, `lint-ci` and `build-and-test` green; a `Debug`
build starts under Xvfb and maps its main window on a software-GL host; Natron's
own FP traps still fire; and `tools/release/check-startup.sh`'s SIGFPE diagnostic,
added in M23, is either still accurate or updated to match what is now true.
